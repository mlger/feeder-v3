/*
 * Platform services for the HMI application on the ESP32-S3 board:
 * time (PCF85063-backed), NVS storage and the ESP-NOW link.
 */
#include <string.h>
#include <sys/time.h>

#include "board.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "net.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "plat.h"
#include "plat_esp.h"

static const char *TAG = "plat";
#define NVS_NS "feeder"

/* ----------------------------------------------------------------- time */

/* The system clock runs in UTC. Local time is UTC plus a fixed offset chosen
 * by the user; the RTC chip holds local time. */
static bool time_valid;
static bool offset_loaded;
static int32_t utc_offset_s;

uint32_t plat_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool plat_time_valid(void) { return time_valid; }

static uint32_t utc_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

uint32_t plat_local_epoch(void) { return (uint32_t)((int32_t)utc_now() + utc_offset_s); }

static void set_utc(uint32_t utc)
{
    const struct timeval tv = { .tv_sec = (time_t)utc };
    settimeofday(&tv, NULL);
    time_valid = true;
}

void plat_set_local_epoch(uint32_t local_epoch)
{
    set_utc((uint32_t)((int32_t)local_epoch - utc_offset_s));
    board_rtc_set(local_epoch);
}

void plat_set_utc_offset(int16_t minutes)
{
    utc_offset_s = (int32_t)minutes * 60;
    if (!offset_loaded) {
        /* First call, at start-up: load the clock from the RTC. */
        offset_loaded = true;
        uint32_t local;
        if (board_rtc_get(&local)) {
            set_utc((uint32_t)((int32_t)local - utc_offset_s));
        } else {
            ESP_LOGW(TAG, "RTC has no valid time; set the clock in Settings");
        }
    } else if (time_valid) {
        board_rtc_set(plat_local_epoch()); /* local time moved with the offset */
    }
}

/* NTP has just set the system clock. */
void plat_esp_time_synced(void)
{
    time_valid = true;
    board_rtc_set(plat_local_epoch());
}

/* -------------------------------------------------------------- storage */

bool plat_load(const char *key, void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t got = len;
    esp_err_t err = nvs_get_blob(h, key, buf, &got);
    nvs_close(h);
    return err == ESP_OK && got == len;
}

void plat_save(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed, '%s' not saved", key);
        return;
    }
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving '%s' failed: %s", key, esp_err_to_name(err));
    }
    nvs_close(h);
}

void plat_backlight(bool on) { board_backlight(on); }

/* -------------------------------------------------------------- ESP-NOW */

typedef struct {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[250];
} rx_frame_t;

static QueueHandle_t rx_queue;

/* Runs in the Wi-Fi task: hand the frame over to the UI task. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > (int)sizeof(((rx_frame_t *)0)->data)) {
        return;
    }
    rx_frame_t f;
    memcpy(f.mac, info->src_addr, 6);
    memcpy(f.data, data, (size_t)len);
    f.len = (uint8_t)len;
    xQueueSend(rx_queue, &f, 0);
}

bool plat_esp_receive(uint8_t mac[6], uint8_t *data, uint8_t *len)
{
    rx_frame_t f;
    if (xQueueReceive(rx_queue, &f, 0) != pdTRUE) {
        return false;
    }
    memcpy(mac, f.mac, 6);
    memcpy(data, f.data, f.len);
    *len = f.len;
    return true;
}

void plat_send(const uint8_t mac[6], const void *msg, uint8_t len)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
        memcpy(peer.peer_addr, mac, 6);
        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGW(TAG, "peer table full");
            return;
        }
    }
    esp_now_send(mac, msg, len);
}

esp_err_t plat_esp_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    rx_queue = xQueueCreate(16, sizeof(rx_frame_t));

    /* ESP-NOW shares the radio with the Wi-Fi station and follows its
     * channel; feeders search the channels until they find the HMI. */
    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    return ESP_OK;
}
