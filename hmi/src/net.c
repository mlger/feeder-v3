/*
 * Network side of the HMI: Wi-Fi station, NTP, and firmware updates.
 *
 * Updates are uploaded from a browser on the local network. The page is
 * always readable, but uploads are accepted only while the user has switched
 * "Firmware updates" on at the touchscreen, so changing the firmware needs
 * someone standing at the controller.
 *
 * Feeders have no network connection of their own. To update one, the HMI
 * opens a short-lived access point with a random password, tells the feeder
 * over ESP-NOW, and serves the uploaded file to it.
 */
#include "net.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "plat.h"
#include "plat_esp.h"
#include "sdlog.h"
#include "app.h"
#include "web_page.h"

static const char *TAG = "net";

#define RETRY_PERIOD_US   (30 * 1000 * 1000)
#define MAX_SCAN          16
#define NODE_FW_MAX       (1024 * 1024)
#define NODE_FW_MIN       (64 * 1024)
#define ESP_IMAGE_MAGIC   0xE9
#define OTA_AP_SSID       "AIO-Feeder-OTA"
#define SETUP_AP_SSID     "AIO-Feeder-Setup"
#define HTTP_PORT         80

static SemaphoreHandle_t lock; /* guards the strings and buffers below */

/* ---------------------------------------------------------------- Wi-Fi */

static atomic_int wifi_state = PLAT_WIFI_IDLE;
static char sta_ssid[33], sta_ip[16];
static bool sta_configured;
static esp_timer_handle_t retry_timer;
static esp_netif_t *ap_netif;

static atomic_bool scanning;
static plat_ap_t scan_aps[MAX_SCAN];
static int scan_count;

static atomic_bool ntp_wanted, ntp_running, ntp_synced;

static void ntp_apply(void);

static void retry_cb(void *arg)
{
    (void)arg;
    if (sta_configured && atomic_load(&wifi_state) != PLAT_WIFI_CONNECTED && !atomic_load(&scanning)) {
        atomic_store(&wifi_state, PLAT_WIFI_CONNECTING);
        esp_wifi_connect();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (sta_configured) {
            atomic_store(&wifi_state, PLAT_WIFI_CONNECTING);
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(lock, portMAX_DELAY);
        sta_ip[0] = '\0';
        xSemaphoreGive(lock);
        atomic_store(&wifi_state, sta_configured ? PLAT_WIFI_FAILED : PLAT_WIFI_IDLE);
        /* Retry rarely: every attempt takes the radio off the ESP-NOW channel. */
        esp_timer_stop(retry_timer);
        if (sta_configured) {
            esp_timer_start_once(retry_timer, RETRY_PERIOD_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        xSemaphoreTake(lock, portMAX_DELAY);
        snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        xSemaphoreGive(lock);
        atomic_store(&wifi_state, PLAT_WIFI_CONNECTED);
        ESP_LOGI(TAG, "connected, address %s", sta_ip);
        ntp_apply();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        static wifi_ap_record_t recs[MAX_SCAN];
        uint16_t n = MAX_SCAN;
        if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
            n = 0;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        scan_count = 0;
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            bool dup = ssid[0] == '\0'; /* hidden networks cannot be picked */
            for (int k = 0; k < scan_count && !dup; k++) {
                dup = strcmp(scan_aps[k].ssid, ssid) == 0;
            }
            if (!dup) {
                snprintf(scan_aps[scan_count].ssid, sizeof(scan_aps[0].ssid), "%s", ssid);
                scan_aps[scan_count].rssi = recs[i].rssi;
                scan_aps[scan_count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                scan_count++;
            }
        }
        xSemaphoreGive(lock);
        atomic_store(&scanning, false);
        if (sta_configured && atomic_load(&wifi_state) != PLAT_WIFI_CONNECTED) {
            esp_timer_stop(retry_timer);
            esp_timer_start_once(retry_timer, 2 * 1000 * 1000);
        }
    }
}

plat_wifi_state_t plat_wifi_state(void) { return (plat_wifi_state_t)atomic_load(&wifi_state); }

void plat_wifi_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(ssid, ssid_len, "%s", sta_ssid);
    snprintf(ip, ip_len, "%s", sta_ip);
    xSemaphoreGive(lock);
}

void plat_wifi_connect(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);
    esp_timer_stop(retry_timer);
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(sta_ssid, sizeof(sta_ssid), "%s", ssid);
    xSemaphoreGive(lock);
    sta_configured = true;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg); /* stored in flash by the driver */
    atomic_store(&wifi_state, PLAT_WIFI_CONNECTING);
    esp_wifi_connect();
}

void plat_wifi_forget(void)
{
    wifi_config_t cfg = { 0 };
    esp_timer_stop(retry_timer);
    sta_configured = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    sta_ssid[0] = '\0';
    xSemaphoreGive(lock);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    atomic_store(&wifi_state, PLAT_WIFI_IDLE);
}

void plat_wifi_scan(void)
{
    if (atomic_load(&scanning)) {
        return;
    }
    /* A scan cannot run during a connection attempt. */
    if (atomic_load(&wifi_state) != PLAT_WIFI_CONNECTED) {
        esp_timer_stop(retry_timer);
        esp_wifi_disconnect();
    }
    atomic_store(&scanning, true);
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        xSemaphoreTake(lock, portMAX_DELAY);
        scan_count = 0;
        xSemaphoreGive(lock);
        atomic_store(&scanning, false);
    }
}

int plat_wifi_scan_get(plat_ap_t *out, int max)
{
    if (atomic_load(&scanning)) {
        return -1;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = scan_count < max ? scan_count : max;
    memcpy(out, scan_aps, sizeof(plat_ap_t) * (size_t)n);
    xSemaphoreGive(lock);
    return n;
}

/* ------------------------------------------------------------------ NTP */

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    atomic_store(&ntp_synced, true);
    plat_esp_time_synced();
}

/* Start or stop SNTP so that it runs exactly when wanted and connected. */
static void ntp_apply(void)
{
    bool want = atomic_load(&ntp_wanted) && atomic_load(&wifi_state) == PLAT_WIFI_CONNECTED;
    if (want && !atomic_load(&ntp_running)) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.sync_cb = on_time_sync;
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            atomic_store(&ntp_running, true);
        }
    } else if (!atomic_load(&ntp_wanted) && atomic_load(&ntp_running)) {
        esp_netif_sntp_deinit();
        atomic_store(&ntp_running, false);
        atomic_store(&ntp_synced, false);
    }
}

void plat_ntp_enable(bool on)
{
    atomic_store(&ntp_wanted, on);
    ntp_apply();
}

bool plat_ntp_synced(void) { return atomic_load(&ntp_synced); }

/* -------------------------------------------------------------- updates */

static atomic_bool updates_allowed;
static atomic_int node_ota_request = -1;
static char status_json[800] = "{}";

static uint8_t *node_fw;       /* PSRAM */
static uint32_t node_fw_len;
static char node_fw_md5[33];
static atomic_bool ota_ap_open, ota_served;

void plat_updates_allow(bool on) { atomic_store(&updates_allowed, on); }

uint32_t plat_node_fw_size(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t n = node_fw_len;
    xSemaphoreGive(lock);
    return n;
}

int plat_take_node_ota_request(void) { return atomic_exchange(&node_ota_request, -1); }

void plat_publish_status(const char *json)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(status_json, sizeof(status_json), "%s", json);
    xSemaphoreGive(lock);
}

static atomic_bool setup_ap_open, setup_done;

/* Bring up the access point next to the station. The two temporary uses
 * (feeder update, phone set-up) never run at the same time. */
static bool ap_open(const char *ssid, const char *pass, int max_clients)
{
    wifi_config_t ap = { 0 };
    uint8_t primary = 1;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);

    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", pass);
    ap.ap.channel = primary; /* must share the channel with the station */
    ap.ap.max_connection = (uint8_t)max_clients;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }
    ESP_LOGI(TAG, "access point %s open on channel %u", ssid, primary);
    return true;
}

bool plat_wifi_setup_begin(char *ssid, char *pass)
{
    if (atomic_load(&ota_ap_open)) {
        return false;
    }
    /* Eight digits: easy to type on a phone, and the minimum WPA2 allows. */
    snprintf(pass, 24, "%08lu", (unsigned long)(esp_random() % 100000000u));
    snprintf(ssid, 24, "%s", SETUP_AP_SSID);
    if (!ap_open(ssid, pass, 2)) {
        return false;
    }
    atomic_store(&setup_done, false);
    atomic_store(&setup_ap_open, true);
    return true;
}

void plat_wifi_setup_end(void)
{
    if (atomic_exchange(&setup_ap_open, false)) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "set-up access point closed");
    }
}

bool plat_wifi_setup_done(void) { return atomic_load(&setup_done); }

bool plat_node_ota_begin(char *ssid, char *pass, uint8_t ip[4], uint16_t *port)
{
    if (atomic_load(&setup_ap_open)) {
        return false;
    }
    snprintf(pass, 24, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(ssid, 24, "%s", OTA_AP_SSID);
    if (!ap_open(ssid, pass, 1)) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap_netif, &info) != ESP_OK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }
    ip[0] = esp_ip4_addr1(&info.ip);
    ip[1] = esp_ip4_addr2(&info.ip);
    ip[2] = esp_ip4_addr3(&info.ip);
    ip[3] = esp_ip4_addr4(&info.ip);
    *port = HTTP_PORT;
    atomic_store(&ota_served, false);
    atomic_store(&ota_ap_open, true);
    return true;
}

void plat_node_ota_end(void)
{
    if (atomic_exchange(&ota_ap_open, false)) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "update access point closed");
    }
}

bool plat_node_ota_served(void) { return atomic_load(&ota_served); }

/* ----------------------------------------------------------- web server */

static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

static bool uploads_open(httpd_req_t *req)
{
    if (atomic_load(&updates_allowed)) {
        return true;
    }
    reply(req, "403 Forbidden", "Uploads are locked. Switch on Firmware updates on the controller.");
    return false;
}

/* Receive the request body in pieces. Returns bytes read, or <= 0 on error. */
static int recv_some(httpd_req_t *req, char *buf, size_t want)
{
    for (int tries = 0; tries < 5; tries++) {
        int n = httpd_req_recv(req, buf, want);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return n;
        }
    }
    return -1;
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* A phone on the set-up access point gets the Wi-Fi form. */
    return httpd_resp_send(req, atomic_load(&setup_ap_open) ? WIFI_PAGE : WEB_PAGE, HTTPD_RESP_USE_STRLEN);
}

/* ---- Wi-Fi set-up form (only while the set-up access point is open) ---- */

static bool setup_open(httpd_req_t *req)
{
    if (atomic_load(&setup_ap_open)) {
        return true;
    }
    reply(req, "403 Forbidden", "Wi-Fi set-up is closed. On the controller open Network and press Use phone.");
    return false;
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    if (!setup_open(req)) {
        return ESP_OK;
    }
    static plat_ap_t aps[MAX_SCAN];
    static char out[MAX_SCAN * 90 + 64];
    static bool asked;
    int n = plat_wifi_scan_get(aps, MAX_SCAN);
    if (n == 0 && !asked) { /* nothing yet: start one scan */
        asked = true;
        plat_wifi_scan();
        n = -1;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n < 0) {
        return httpd_resp_sendstr(req, "{\"scanning\":true,\"aps\":[]}");
    }
    asked = false;
    size_t k = (size_t)snprintf(out, sizeof(out), "{\"scanning\":false,\"aps\":[");
    for (int i = 0; i < n && k < sizeof(out) - 100; i++) {
        k += (size_t)snprintf(out + k, sizeof(out) - k, "%s{\"ssid\":\"", i ? "," : "");
        for (const char *p = aps[i].ssid; *p && k < sizeof(out) - 40; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                out[k++] = '\\';
                out[k++] = (char)c;
            } else if (c < 0x20) {
                k += (size_t)snprintf(out + k, sizeof(out) - k, "\\u%04x", c);
            } else {
                out[k++] = (char)c;
            }
        }
        k += (size_t)snprintf(out + k, sizeof(out) - k, "\",\"rssi\":%d}", aps[i].rssi);
    }
    snprintf(out + k, sizeof(out) - k, "]}");
    return httpd_resp_sendstr(req, out);
}

/* Decode application/x-www-form-urlencoded text in place. */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static esp_err_t h_wifi_connect(httpd_req_t *req)
{
    if (!setup_open(req)) {
        return ESP_OK;
    }
    char body[400], ssid[100], pass[200];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return reply(req, "400 Bad Request", "Request too large.");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = recv_some(req, body + got, req->content_len - got);
        if (n <= 0) {
            return reply(req, "400 Bad Request", "Request interrupted.");
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    pass[0] = '\0';
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return reply(req, "400 Bad Request", "Enter the network name.");
    }
    httpd_query_key_value(body, "pass", pass, sizeof(pass)); /* may be absent for open networks */
    url_decode(ssid);
    url_decode(pass);
    if (ssid[0] == '\0' || strlen(ssid) > 32 || strlen(pass) > 63) {
        return reply(req, "400 Bad Request", "The network name or password is too long.");
    }
    reply(req, "200 OK", "Details received. Look at the controller's screen to see whether it connected.");
    plat_wifi_connect(ssid, pass);
    atomic_store(&setup_done, true);
    return ESP_OK;
}

/* ---- feed history from the SD card ---- */

static esp_err_t h_log(httpd_req_t *req)
{
    FILE *f = plat_sd_ready() ? fopen(SDLOG_MOUNT "/" APP_SD_LOG_FILE, "r") : NULL;
    if (!f) {
        return reply(req, "404 Not Found", "No history on the SD card (no card, or nothing logged yet).");
    }
    static char buf[2048];
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"" APP_SD_LOG_FILE "\"");
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    fclose(f);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t h_status(httpd_req_t *req)
{
    static char copy[sizeof(status_json)];
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(copy, status_json, sizeof(copy));
    xSemaphoreGive(lock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, copy);
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t h_update_hmi(httpd_req_t *req)
{
    if (!uploads_open(req)) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return reply(req, "400 Bad Request", "The file is empty or too large for the controller.");
    }
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return reply(req, "500 Internal Server Error", "Could not start the update.");
    }
    static char buf[4096];
    size_t left = req->content_len;
    bool ok = true;
    while (left > 0 && ok) {
        int n = recv_some(req, buf, left < sizeof(buf) ? left : sizeof(buf));
        ok = n > 0 && esp_ota_write(ota, buf, (size_t)n) == ESP_OK;
        left -= ok ? (size_t)n : 0;
    }
    if (!ok) {
        esp_ota_abort(ota);
        return reply(req, "500 Internal Server Error", "Upload interrupted. The controller is unchanged.");
    }
    /* esp_ota_end() checks that this is a valid image for this chip. */
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return reply(req, "400 Bad Request", "This is not a valid controller firmware. Nothing was changed.");
    }
    reply(req, "200 OK", "Controller updated. It restarts now.");
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1500 * 1000);
    }
    return ESP_OK;
}

static esp_err_t h_update_node(httpd_req_t *req)
{
    if (!uploads_open(req)) {
        return ESP_OK;
    }
    if (atomic_load(&ota_ap_open)) {
        return reply(req, "409 Conflict", "A feeder update is running. Try again when it has finished.");
    }
    /* The controller's own firmware is larger than this, which also stops
     * it being loaded as feeder firmware by mistake. */
    if (req->content_len < NODE_FW_MIN || req->content_len > NODE_FW_MAX) {
        return reply(req, "400 Bad Request", "This does not look like feeder firmware (wrong size).");
    }
    uint8_t *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return reply(req, "500 Internal Server Error", "Not enough memory.");
    }
    size_t got = 0;
    while (got < req->content_len) {
        size_t want = req->content_len - got;
        int n = recv_some(req, (char *)buf + got, want < 4096 ? want : 4096);
        if (n <= 0) {
            heap_caps_free(buf);
            return reply(req, "500 Internal Server Error", "Upload interrupted.");
        }
        got += (size_t)n;
    }
    if (buf[0] != ESP_IMAGE_MAGIC) {
        heap_caps_free(buf);
        return reply(req, "400 Bad Request", "This is not a firmware image.");
    }
    md5_context_t ctx;
    uint8_t digest[16];
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, buf, (uint32_t)got);
    esp_rom_md5_final(digest, &ctx);

    xSemaphoreTake(lock, portMAX_DELAY);
    heap_caps_free(node_fw);
    node_fw = buf;
    node_fw_len = (uint32_t)got;
    for (int i = 0; i < 16; i++) {
        snprintf(node_fw_md5 + 2 * i, 3, "%02x", digest[i]);
    }
    xSemaphoreGive(lock);
    return reply(req, "200 OK", "Feeder firmware loaded. Now update each feeder.");
}

static esp_err_t h_node_push(httpd_req_t *req)
{
    if (!uploads_open(req)) {
        return ESP_OK;
    }
    char query[16], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "i", val, sizeof(val)) != ESP_OK || val[0] < '0' || val[0] > '9') {
        return reply(req, "400 Bad Request", "Missing feeder.");
    }
    atomic_store(&node_ota_request, val[0] - '0');
    return reply(req, "200 OK", "Update requested. Watch the controller's screen.");
}

/* Fetched by the feeder over the temporary access point. */
static esp_err_t h_node_bin(httpd_req_t *req)
{
    if (!atomic_load(&ota_ap_open)) {
        return reply(req, "404 Not Found", "No update in progress.");
    }
    /* The buffer cannot change while the access point is open. */
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "x-MD5", node_fw_md5);
    esp_err_t err = httpd_resp_send(req, (const char *)node_fw, (ssize_t)node_fw_len);
    if (err == ESP_OK) {
        atomic_store(&ota_served, true);
    }
    return err;
}

static void web_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server did not start");
        return;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/update/hmi", .method = HTTP_POST, .handler = h_update_hmi },
        { .uri = "/update/node", .method = HTTP_POST, .handler = h_update_node },
        { .uri = "/node/push", .method = HTTP_POST, .handler = h_node_push },
        { .uri = "/node.bin", .method = HTTP_GET, .handler = h_node_bin },
        { .uri = "/wifi/scan", .method = HTTP_GET, .handler = h_wifi_scan },
        { .uri = "/wifi/connect", .method = HTTP_POST, .handler = h_wifi_connect },
        { .uri = "/" APP_SD_LOG_FILE, .method = HTTP_GET, .handler = h_log },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

/* ----------------------------------------------------------------- init */

esp_err_t net_init(void)
{
    lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* The driver remembers the last network in flash. */
    wifi_config_t saved;
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0] != '\0') {
        snprintf(sta_ssid, sizeof(sta_ssid), "%s", (const char *)saved.sta.ssid);
        sta_configured = true;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!sta_configured) {
        /* No router to follow: stay on a fixed channel for the feeders. */
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    }
    web_start();
    return ESP_OK;
}
