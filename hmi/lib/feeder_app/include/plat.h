/*
 * Platform services the portable HMI application needs. Implemented once for
 * the ESP32-S3 board (src/plat_esp.c) and once for the host simulator
 * (sim/plat_sim.c).
 */
#ifndef FEEDER_PLAT_H
#define FEEDER_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t plat_millis(void);

/*
 * Local wall-clock time as seconds since 1970 (see feeder/proto.h).
 * The platform clock runs in UTC; local time is UTC plus the offset below.
 */
bool     plat_time_valid(void);
uint32_t plat_local_epoch(void);
void     plat_set_local_epoch(uint32_t local_epoch);

/* Must be called once at start-up (it loads the clock), then on changes. */
void plat_set_utc_offset(int16_t minutes);

/* Internet time. While enabled and Wi-Fi is up, the clock follows NTP. */
void plat_ntp_enable(bool on);
bool plat_ntp_synced(void); /* NTP has set the clock since start-up */

/* ---- Wi-Fi ---- */

typedef enum {
    PLAT_WIFI_IDLE,       /* no network configured */
    PLAT_WIFI_CONNECTING,
    PLAT_WIFI_CONNECTED,
    PLAT_WIFI_FAILED      /* configured but not reachable; retried by itself */
} plat_wifi_state_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
} plat_ap_t;

plat_wifi_state_t plat_wifi_state(void);
/* Configured network name and current IP address; empty strings if none. */
void plat_wifi_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len);
void plat_wifi_connect(const char *ssid, const char *pass); /* remembered */
void plat_wifi_forget(void);
void plat_wifi_scan(void);
/* -1 while a scan is running, otherwise the number of networks copied. */
int  plat_wifi_scan_get(plat_ap_t *out, int max);

/*
 * Wi-Fi set-up from a phone: open a temporary access point whose web page
 * asks for the network name and password. ssid and pass must each hold 24
 * bytes. plat_wifi_setup_done() turns true once the page submitted them.
 */
bool plat_wifi_setup_begin(char *ssid, char *pass);
void plat_wifi_setup_end(void);
bool plat_wifi_setup_done(void);

/* ---- SD card ---- */

bool plat_sd_ready(void); /* a card is inserted and writable */
/* Append one line to a file on the card; `header` starts a new file.
 * Returns at once; the line is dropped if there is no card. */
void plat_sd_append(const char *file, const char *header, const char *line);

/* ---- firmware updates ---- */

/* Size of the feeder firmware uploaded through the web page, 0 if none. */
uint32_t plat_node_fw_size(void);

/* A feeder slot whose update was requested on the web page, or -1. */
int plat_take_node_ota_request(void);

/* Open / close the temporary access point a feeder downloads from.
 * ssid and pass must each hold 24 bytes. */
bool plat_node_ota_begin(char *ssid, char *pass, uint8_t ip[4], uint16_t *port);
void plat_node_ota_end(void);
/* True once the whole firmware has been downloaded since the last begin. */
bool plat_node_ota_served(void);

/* One-line state for the web page, written by the UI thread. */
void plat_publish_status(const char *json);

/* Send one ESP-NOW frame. Delivery is best effort. */
void plat_send(const uint8_t mac[6], const void *msg, uint8_t len);

/* Small named blobs that survive power loss. */
bool plat_load(const char *key, void *buf, size_t len);
void plat_save(const char *key, const void *buf, size_t len);

void plat_backlight(bool on);

#ifdef __cplusplus
}
#endif

#endif
