#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "adc_scope.h"
#include "bench.h"
#include "buses.h"
#include "caps.h"
#include "gpio_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

#define MAX_WS_CLIENTS   8
#define MAX_WS_MSG_LEN   512
#define FILE_CHUNK_BYTES 1024

static httpd_handle_t    s_server;
static int               s_clients[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_clients_lock;
static SemaphoreHandle_t s_send_lock; /* one writer at a time so frames never interleave */

/* -------------------------------------------------------------- WS clients */

static void clients_add(int fd)
{
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    bool known = false;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_clients[i] == fd) known = true;
    for (int i = 0; i < MAX_WS_CLIENTS && !known; i++) {
        if (s_clients[i] < 0) {
            s_clients[i] = fd;
            known = true;
        }
    }
    xSemaphoreGive(s_clients_lock);
    if (!known) ESP_LOGW(TAG, "too many WebSocket clients, fd %d not tracked", fd);
}

static void clients_remove(int fd)
{
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_clients[i] == fd) s_clients[i] = -1;
    xSemaphoreGive(s_clients_lock);
}

static int clients_snapshot(int *out)
{
    int n = 0;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_clients[i] >= 0) out[n++] = s_clients[i];
    xSemaphoreGive(s_clients_lock);
    return n;
}

bool web_server_has_clients(void)
{
    if (!s_server) return false;
    int fds[MAX_WS_CLIENTS];
    return clients_snapshot(fds) > 0;
}

static void send_to(int fd, const char *msg)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = strlen(msg),
        .final = true,
    };
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
    xSemaphoreGive(s_send_lock);
    if (err != ESP_OK) {
        /* Slow or vanished client: forget it and close its socket so one stuck
         * browser tab cannot keep blocking telemetry for everybody else. */
        clients_remove(fd);
        httpd_sess_trigger_close(s_server, fd);
    }
}

void web_server_broadcast(const char *msg)
{
    if (!s_server || !msg) return;
    int fds[MAX_WS_CLIENTS];
    int n = clients_snapshot(fds);
    for (int i = 0; i < n; i++) send_to(fds[i], msg);
}

static void broadcast_owned(char *msg)
{
    if (!msg) return;
    web_server_broadcast(msg);
    cJSON_free(msg);
}

static void broadcast_pins(void) { broadcast_owned(gpio_mgr_snapshot_json()); }

static void send_error(int fd, const char *what)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", what);
    send_to(fd, buf);
}

/* ---------------------------------------------------------- command parsing */

static void handle_json_command(int fd, const char *msg)
{
    cJSON *root = cJSON_Parse(msg);
    const cJSON *cmd = root ? cJSON_GetObjectItemCaseSensitive(root, "cmd") : NULL;
    if (!cJSON_IsString(cmd)) {
        send_error(fd, "bad JSON command");
        cJSON_Delete(root);
        return;
    }
    if (strcmp(cmd->valuestring, "bench") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *params = cJSON_DetachItemFromObjectCaseSensitive(root, "params");
        if (!cJSON_IsString(id)) {
            cJSON_Delete(params);
            send_error(fd, "bench id missing");
        } else {
            esp_err_t err = bench_request(id->valuestring, params);
            if (err == ESP_ERR_INVALID_STATE) send_error(fd, "a benchmark is already running");
            else if (err != ESP_OK) send_error(fd, "unknown benchmark");
        }
    } else {
        send_error(fd, "unknown command");
    }
    cJSON_Delete(root);
}

static void handle_text_command(int fd, const char *msg)
{
    int pin = -1, val = 0, duty = 50;
    char word[16];

    if (sscanf(msg, "MODE:%d,%15s", &pin, word) == 2) {
        esp_err_t err;
        if (strcmp(word, "PWM") == 0) err = gpio_mgr_set_pwm(pin, 1000, duty);
        else if (strcmp(word, "OUTPUT") == 0) err = gpio_mgr_set_mode(pin, PIN_MODE_OUTPUT);
        else err = gpio_mgr_set_mode(pin, PIN_MODE_INPUT);
        if (err != ESP_OK) send_error(fd, "pin mode not supported on this pin");
        broadcast_pins();
    } else if (sscanf(msg, "WRITE:%d,%d", &pin, &val) == 2) {
        if (gpio_mgr_write(pin, val != 0) != ESP_OK) send_error(fd, "pin is not an output");
        broadcast_pins();
    } else if (sscanf(msg, "PWM:%d,%d,%d", &pin, &val, &duty) >= 2) {
        if (gpio_mgr_set_pwm(pin, (uint32_t)val, (uint32_t)duty) != ESP_OK) send_error(fd, "PWM setup failed (frequency/duty/channel)");
        broadcast_pins();
    } else if (sscanf(msg, "OSCILLO:%d", &pin) == 1) {
        if (adc_scope_select(pin) != ESP_OK) send_error(fd, "pin has no ADC1 channel");
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"scope_sel\":%d}", adc_scope_selected());
        web_server_broadcast(buf);
    } else if (strncmp(msg, "UART_SEND:", 10) == 0) {
        buses_uart_write(msg + 10, strlen(msg + 10));
    } else if (sscanf(msg, "UART_BAUD:%d", &val) == 1) {
        if (buses_uart_set_baud((uint32_t)val) != ESP_OK) send_error(fd, "invalid baud rate");
    } else if (strcmp(msg, "UART_READ") == 0) {
        char in[128];
        int n = buses_uart_read(in, sizeof(in));
        cJSON *o = cJSON_CreateObject();
        for (int i = 0; i < n; i++) if ((unsigned char)in[i] >= 0x80) in[i] = '.';
        char *text = strndup(in, n);
        cJSON_AddStringToObject(o, "uart", text ? text : "");
        free(text);
        broadcast_owned(cJSON_PrintUnformatted(o));
        cJSON_Delete(o);
    } else if (strcmp(msg, "I2C_SCAN") == 0) {
        uint8_t addrs[126];
        int n = buses_i2c_scan(addrs, sizeof(addrs));
        if (n < 0) {
            send_error(fd, "I2C bus not available");
        } else {
            cJSON *o = cJSON_CreateObject();
            cJSON *arr = cJSON_AddArrayToObject(o, "i2c");
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(addrs[i]));
            broadcast_owned(cJSON_PrintUnformatted(o));
            cJSON_Delete(o);
        }
    } else {
        send_error(fd, "unknown command");
    }
}

static void send_welcome(int fd)
{
    char *pins = gpio_mgr_snapshot_json();
    if (pins) {
        send_to(fd, pins);
        cJSON_free(pins);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"scope_sel\":%d}", adc_scope_selected());
    send_to(fd, buf);
    snprintf(buf, sizeof(buf), "{\"bench\":{\"event\":\"state\",\"running\":%s}}", bench_is_running() ? "true" : "false");
    send_to(fd, buf);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) { /* handshake complete */
        clients_add(fd);
        ESP_LOGI(TAG, "WebSocket client connected (fd %d)", fd);
        send_welcome(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        clients_remove(fd);
        return ESP_OK;
    }
    if (frame.len == 0 || frame.len > MAX_WS_MSG_LEN) {
        if (frame.len > MAX_WS_MSG_LEN) {
            send_error(fd, "message too long");
            return ESP_ERR_INVALID_SIZE; /* closes the connection */
        }
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        if (buf[0] == '{') handle_json_command(fd, (const char *)buf);
        else handle_text_command(fd, (const char *)buf);
    }
    free(buf);
    return ret;
}

/* ------------------------------------------------------------ HTTP handlers */

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html";
    if (!strcmp(dot, ".css")) return "text/css";
    if (!strcmp(dot, ".js")) return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    if (!strcmp(dot, ".txt")) return "text/plain";
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");

    char *chunk = malloc(FILE_CHUNK_BYTES);
    if (!chunk) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, mime_for(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(chunk, 1, FILE_CHUNK_BYTES, f)) > 0) {
        err = httpd_resp_send_chunk(req, chunk, n);
    }
    fclose(f);
    free(chunk);
    if (err != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_file(req, "/spiffs/index.html");
}

static esp_err_t static_handler(httpd_req_t *req)
{
    char path[64] = "/spiffs";
    const char *uri = req->uri;
    size_t len = strcspn(uri, "?#");
    if (len == 0 || len > 40 || strstr(uri, "..") != NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    }
    strncat(path, uri, len);
    return send_file(req, path);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return err;
}

static esp_err_t info_handler(httpd_req_t *req)
{
    cJSON *root = caps_info_json();
    cJSON_AddItemToObject(root, "benchmarks", bench_list_json());
    cJSON *scope = cJSON_AddObjectToObject(root, "scope");
    cJSON_AddItemToObject(scope, "pins", adc_scope_pins_json());
    cJSON_AddNumberToObject(scope, "selected", adc_scope_selected());
    cJSON_AddItemToObject(root, "wifi", wifi_mgr_status_json());
    return send_json(req, root);
}

static esp_err_t bench_handler(httpd_req_t *req)
{
    return send_json(req, bench_results_json());
}

static void on_session_close(httpd_handle_t hd, int fd)
{
    clients_remove(fd);
    close(fd);
}

esp_err_t web_server_start(void)
{
    s_clients_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    memset(s_clients, -1, sizeof(s_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard; /* needed by the catch-all static handler */
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.lru_purge_enable = true; /* long-lived WebSockets must not starve new connections */
    config.send_wait_timeout = 1;   /* seconds a send may block on a stalled client (default 5) */
    config.close_fn = on_session_close;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",          .method = HTTP_GET, .handler = index_handler },
        { .uri = "/api/info",  .method = HTTP_GET, .handler = info_handler },
        { .uri = "/api/bench", .method = HTTP_GET, .handler = bench_handler },
        { .uri = "/ws",        .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
        { .uri = "/*",         .method = HTTP_GET, .handler = static_handler }, /* must stay last */
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
