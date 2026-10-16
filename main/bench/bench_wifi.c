/* Wi-Fi benchmark: radio/link info, channel scan, gateway ping (latency and
 * loss) and an optional TCP upload throughput test against a host you run
 * (e.g. `iperf -s` or `nc -lk 5001 > /dev/null`). */
#include "bench.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_WIFI_SUPPORTED && CONFIG_WEBDBG_ENABLE_WIFI

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"

#include "wifi_mgr.h"

#define SCAN_MAX_RECORDS 24

static void link_info(bench_ctx_t *ctx)
{
    char ip[16], gw[16];
    wifi_ap_record_t rec;
    if (wifi_mgr_sta_connected() && esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
        bench_text(ctx, "Station link", "%s, ch %d, %d dBm", (const char *)rec.ssid, rec.primary, rec.rssi);
        char proto[32] = "";
        if (rec.phy_11b) strlcat(proto, "b ", sizeof(proto));
        if (rec.phy_11g) strlcat(proto, "g ", sizeof(proto));
        if (rec.phy_11n) strlcat(proto, "n ", sizeof(proto));
#if SOC_WIFI_HE_SUPPORT
        if (rec.phy_11ax) strlcat(proto, "ax ", sizeof(proto));
#endif
        bench_text(ctx, "Negotiated protocols", "802.11 %s", proto);
        if (wifi_mgr_sta_ip(ip, sizeof(ip))) bench_text(ctx, "Station IP", "%s", ip);
        if (wifi_mgr_sta_gateway(gw, sizeof(gw))) bench_text(ctx, "Gateway", "%s", gw);
    } else {
        bench_text(ctx, "Station link", "not connected%s",
                   sizeof(CONFIG_ESP_WIFI_SSID) > 1 ? " (check credentials)" : " (no SSID configured)");
    }
    int8_t power = 0;
    if (esp_wifi_get_max_tx_power(&power) == ESP_OK) {
        bench_metric(ctx, "Max TX power", power * 0.25, "dBm");
    }
    wifi_sta_list_t list = { 0 };
    esp_wifi_ap_get_sta_list(&list);
    bench_metric(ctx, "Stations on our access point", list.num, "");
}

static void scan(bench_ctx_t *ctx)
{
    wifi_scan_config_t cfg = { .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    int64_t t0 = bench_now_us();
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    int64_t dt = bench_now_us() - t0;
    if (err != ESP_OK) {
        bench_note(ctx, "Scan failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    uint16_t n = SCAN_MAX_RECORDS;
    wifi_ap_record_t *recs = calloc(SCAN_MAX_RECORDS, sizeof(*recs));
    if (!recs) return;
    esp_wifi_scan_get_ap_records(&n, recs);

    bench_metric(ctx, "Scan time (all channels)", dt / 1000.0, "ms");
    bench_metric(ctx, "Networks found", total, "");
    if (n > 0) {
        int best = 0;
        int per_channel[15] = { 0 };
        for (int i = 0; i < n; i++) {
            if (recs[i].rssi > recs[best].rssi) best = i;
            if (recs[i].primary >= 1 && recs[i].primary <= 14) per_channel[recs[i].primary]++;
        }
        bench_text(ctx, "Strongest network", "%s (%d dBm, ch %d)", recs[best].ssid[0] ? (const char *)recs[best].ssid : "<hidden>",
                   recs[best].rssi, recs[best].primary);
        int busiest = 1;
        for (int c = 2; c <= 14; c++) if (per_channel[c] > per_channel[busiest]) busiest = c;
        bench_text(ctx, "Busiest channel", "ch %d (%d networks)", busiest, per_channel[busiest]);
    }
    free(recs);
}

/* ---- ping ---- */

typedef struct {
    SemaphoreHandle_t done;
    uint32_t sent, received;
    uint32_t min_ms, max_ms, sum_ms;
} ping_stats_t;

static void on_ping_ok(esp_ping_handle_t h, void *arg)
{
    ping_stats_t *s = arg;
    uint32_t ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    if (s->received == 0 || ms < s->min_ms) s->min_ms = ms;
    if (ms > s->max_ms) s->max_ms = ms;
    s->sum_ms += ms;
    s->received++;
}

static void on_ping_end(esp_ping_handle_t h, void *arg)
{
    ping_stats_t *s = arg;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &s->sent, sizeof(s->sent));
    xSemaphoreGive(s->done);
}

static void ping_gateway(bench_ctx_t *ctx)
{
    char gw[16];
    if (!wifi_mgr_sta_gateway(gw, sizeof(gw))) {
        bench_note(ctx, "Ping skipped: station is not connected.");
        return;
    }
    ping_stats_t st = { .done = xSemaphoreCreateBinary() };
    if (!st.done) return;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 20;
    cfg.interval_ms = 100;
    cfg.timeout_ms = 1000;
    ip_addr_t target;
    ipaddr_aton(gw, &target);
    cfg.target_addr = target;

    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_ok, .on_ping_end = on_ping_end, .cb_args = &st };
    esp_ping_handle_t h;
    if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) {
        esp_ping_start(h);
        if (xSemaphoreTake(st.done, pdMS_TO_TICKS(10000)) == pdTRUE) {
            bench_metric(ctx, "Ping gateway: replies", st.received, "of 20");
            if (st.received) {
                bench_metric(ctx, "Ping latency (min)", st.min_ms, "ms");
                bench_metric(ctx, "Ping latency (avg)", (double)st.sum_ms / st.received, "ms");
                bench_metric(ctx, "Ping latency (max)", st.max_ms, "ms");
            }
            bench_metric(ctx, "Packet loss", (1.0 - st.received / 20.0) * 100.0, "%");
        } else {
            bench_note(ctx, "Ping did not finish in time.");
        }
        esp_ping_stop(h);
        esp_ping_delete_session(h);
    }
    vSemaphoreDelete(st.done);
}

/* ---- TCP upload throughput ---- */

static void tcp_upload(bench_ctx_t *ctx)
{
    const char *host = bench_param_str(ctx, "host", NULL);
    if (!host) {
        bench_note(ctx, "TCP throughput skipped: enter a host running `iperf -s` or `nc -lk 5001 >/dev/null`.");
        return;
    }
    int port = bench_param_int(ctx, "port", 5001);
    int kb = bench_param_int(ctx, "kb", 2048);
    if (kb < 64) kb = 64;
    if (kb > 16384) kb = 16384;

    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) {
        bench_fail(ctx, "Cannot resolve %s", host);
        return;
    }
    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        freeaddrinfo(res);
        bench_fail(ctx, "socket() failed");
        return;
    }

    /* connect with a 4 s timeout */
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        struct timeval tv = { .tv_sec = 4 };
        int err = 0;
        socklen_t len = sizeof(err);
        if (select(s + 1, NULL, &wset, NULL, &tv) > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
            rc = 0;
        }
    }
    if (rc != 0) {
        close(s);
        bench_fail(ctx, "Could not connect to %s:%d", host, port);
        return;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    struct timeval snd = { .tv_sec = 5 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

    static uint8_t block[1460];
    memset(block, 0x5A, sizeof(block));
    size_t total = (size_t)kb * 1024, sent = 0;
    int64_t t0 = bench_now_us();
    while (sent < total) {
        int n = send(s, block, sizeof(block), 0);
        if (n <= 0) break;
        sent += n;
    }
    int64_t dt = bench_now_us() - t0;
    close(s);

    if (sent < total) {
        bench_fail(ctx, "Upload aborted after %u KB", (unsigned)(sent / 1024));
    } else {
        bench_metric(ctx, "TCP upload throughput", sent * 8.0 / (double)dt, "Mbit/s");
        bench_text(ctx, "TCP upload", "%u KB to %s:%d in %.0f ms", (unsigned)(sent / 1024), host, port, dt / 1000.0);
    }
}

static void run(bench_ctx_t *ctx)
{
    link_info(ctx);
    ping_gateway(ctx);
    tcp_upload(ctx);
    if (strcmp(bench_param_str(ctx, "scan", "1"), "0") != 0) {
        scan(ctx); /* last: scanning hops channels and briefly stalls the web UI */
    }
    bench_note(ctx, "Scanning leaves the channel for a moment; the UI WebSocket may pause and reconnect.");
}

static bool available(const char **reason)
{
    (void)reason;
    return true;
}

#else

static void run(bench_ctx_t *ctx) { (void)ctx; }

static bool available(const char **reason)
{
    *reason = "Wi-Fi is not available on this chip / not enabled in this build.";
    return false;
}

#endif

const bench_def_t bench_def_wifi = {
    .id = "wifi",
    .name = "Wi-Fi",
    .group = "Connectivity",
    .desc = "Link info, gateway ping (latency/loss), optional TCP upload throughput and a channel scan.",
    .available = available,
    .run = run,
};
