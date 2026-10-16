#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_log.h"

#if SOC_WIFI_SUPPORTED && CONFIG_WEBDBG_ENABLE_WIFI

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi_mgr";

#define HAS_STA_CONFIG (sizeof(CONFIG_ESP_WIFI_SSID) > 1)
#define RECONNECT_MAX_DELAY_MS 30000

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static volatile bool s_sta_connected;
static int s_retry;

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = { .callback = reconnect_timer_cb, .name = "wifi_retry" };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            esp_wifi_connect();
            return;
        }
    }
    int delay_ms = 500 << (s_retry < 6 ? s_retry : 6); /* 0.5 s .. 30 s */
    if (delay_ms > RECONNECT_MAX_DELAY_MS) delay_ms = RECONNECT_MAX_DELAY_MS;
    s_retry++;
    esp_timer_stop(timer);
    esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (HAS_STA_CONFIG) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_connected = false;
        ESP_LOGW(TAG, "STA disconnected (reason %d), retrying", ((wifi_event_sta_disconnected_t *)data)->reason);
        schedule_reconnect();
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station " MACSTR " joined the AP", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station " MACSTR " left the AP", MAC2STR(e->mac));
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_sta_connected = true;
        s_retry = 0;
        ESP_LOGI(TAG, "web UI: http://" IPSTR "/  (station)", IP2STR(&e->ip_info.ip));
    }
}

esp_err_t wifi_mgr_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    /* The STA interface always exists (the Wi-Fi benchmark needs it to scan),
     * it only connects when a station SSID is configured. */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, "esp32-debugger");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, CONFIG_WEBDBG_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(CONFIG_WEBDBG_AP_SSID);
    ap.ap.channel = CONFIG_WEBDBG_AP_CHANNEL;
    ap.ap.max_connection = CONFIG_WEBDBG_AP_MAX_CONN;
    if (strlen(CONFIG_WEBDBG_AP_PASSWORD) >= 8) {
        strlcpy((char *)ap.ap.password, CONFIG_WEBDBG_AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    if (HAS_STA_CONFIG) {
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, CONFIG_ESP_WIFI_SSID, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* A debugging tool wants low latency, not modem sleep. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "web UI: http://192.168.4.1/  (access point \"%s\")", CONFIG_WEBDBG_AP_SSID);
    return ESP_OK;
}

bool wifi_mgr_sta_connected(void) { return s_sta_connected; }

static bool netif_ip(esp_netif_t *netif, bool gateway, char *out, size_t len)
{
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    esp_ip4_addr_t a = gateway ? info.gw : info.ip;
    if (a.addr == 0) return false;
    snprintf(out, len, IPSTR, IP2STR(&a));
    return true;
}

bool wifi_mgr_sta_ip(char *out, size_t len) { return s_sta_connected && netif_ip(s_sta_netif, false, out, len); }
bool wifi_mgr_sta_gateway(char *out, size_t len) { return s_sta_connected && netif_ip(s_sta_netif, true, out, len); }
bool wifi_mgr_ap_ip(char *out, size_t len) { return netif_ip(s_ap_netif, false, out, len); }

cJSON *wifi_mgr_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    cJSON_AddStringToObject(root, "mode", mode == WIFI_MODE_APSTA ? "APSTA" : mode == WIFI_MODE_AP ? "AP" : "STA");

    char ip[16];
    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap, "ssid", CONFIG_WEBDBG_AP_SSID);
    wifi_sta_list_t list = { 0 };
    esp_wifi_ap_get_sta_list(&list);
    cJSON_AddNumberToObject(ap, "stations", list.num);
    if (wifi_mgr_ap_ip(ip, sizeof(ip))) cJSON_AddStringToObject(ap, "ip", ip);

    cJSON *sta = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddBoolToObject(sta, "configured", HAS_STA_CONFIG);
    wifi_ap_record_t rec;
    bool linked = HAS_STA_CONFIG && esp_wifi_sta_get_ap_info(&rec) == ESP_OK;
    cJSON_AddBoolToObject(sta, "connected", linked && s_sta_connected);
    if (linked) {
        cJSON_AddStringToObject(sta, "ssid", (const char *)rec.ssid);
        cJSON_AddNumberToObject(sta, "rssi", rec.rssi);
        cJSON_AddNumberToObject(sta, "channel", rec.primary);
        if (wifi_mgr_sta_ip(ip, sizeof(ip))) cJSON_AddStringToObject(sta, "ip", ip);
    }
    return root;
}

#else /* no Wi-Fi on this chip / disabled in menuconfig */

esp_err_t wifi_mgr_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool wifi_mgr_sta_connected(void) { return false; }
bool wifi_mgr_sta_ip(char *out, size_t len) { (void)out; (void)len; return false; }
bool wifi_mgr_sta_gateway(char *out, size_t len) { (void)out; (void)len; return false; }
bool wifi_mgr_ap_ip(char *out, size_t len) { (void)out; (void)len; return false; }
cJSON *wifi_mgr_status_json(void) { return cJSON_CreateObject(); }

#endif
