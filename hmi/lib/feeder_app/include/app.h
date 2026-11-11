/*
 * HMI application model: the paired nodes, their schedules, the feed log and
 * the HMI side of the ESP-NOW protocol. No LVGL and no hardware access here.
 *
 * Everything in this file must be called from one thread (the LVGL thread).
 */
#ifndef FEEDER_APP_H
#define FEEDER_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "feeder/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VERSION    "0.3.0"
#define APP_MAX_NODES  8
#define APP_MAX_FOUND  4
#define APP_LOG_SIZE   64

#define APP_ONLINE_TIMEOUT_MS 35000u
#define APP_FEED_NO_REPLY     0xFF /* feed result: node did not answer */

/* Stored per node. */
typedef struct {
    uint8_t          used;
    uint8_t          mac[6];
    char             name[FD_NAME_LEN];
    uint16_t         rev;      /* schedule revision the node should have */
    fd_limits_t      limits;
    uint8_t          count;
    fd_sched_entry_t entries[FD_MAX_ENTRIES];
} app_node_cfg_t;

/* Runtime state per node. */
typedef struct {
    bool            adopted;      /* node has acknowledged PAIR at least once */
    bool            have_status;
    uint32_t        last_seen_ms;
    fd_msg_status_t status;
    bool            have_event;
    uint16_t        last_event_id;
    uint32_t        last_push_ms; /* rate limit for SCHED_SET / TIME_SYNC */
} app_node_rt_t;

typedef struct {
    uint8_t  mac[6];
    uint32_t seen_ms;
    bool     used;
} app_found_t;

typedef struct {
    uint32_t epoch;
    uint8_t  node;    /* node slot index */
    uint8_t  source;  /* schedule entry index or FD_SOURCE_MANUAL */
    uint8_t  result;  /* fd_result_t */
    uint8_t  pulses;
    uint16_t on_ms;
} app_log_entry_t;

typedef struct {
    uint8_t dark;          /* 1 = dark theme */
    uint8_t sleep_min;     /* screen off after N idle minutes, 0 = never */
    uint8_t feed_on_ds;    /* manual feed portion */
    uint8_t feed_pulses;
    uint8_t ntp;           /* 1 = follow internet time */
    int16_t utc_offset_min;
} app_settings_t;

/* What changed; the UI redraws the matching part. */
typedef enum {
    APP_CH_NODES    = 1 << 0, /* node list or a node's status */
    APP_CH_FOUND    = 1 << 1, /* discovered (unpaired) nodes */
    APP_CH_LOG      = 1 << 2,
    APP_CH_SCHEDULE = 1 << 3,
    APP_CH_FEED     = 1 << 4, /* a manual feed got its answer */
    APP_CH_OTA      = 1 << 5  /* a feeder update changed state */
} app_change_t;

void app_init(void);

/* Call about four times a second. */
void app_tick(void);

/* Feed a received ESP-NOW frame in. */
void app_on_packet(const uint8_t mac[6], const uint8_t *data, uint8_t len);

/* Returns and clears the pending change flags. */
uint32_t app_take_changes(void);

/* ---- nodes ---- */
app_node_cfg_t      *app_node(int idx);       /* NULL if the slot is free */
const app_node_rt_t *app_node_rt(int idx);
bool                 app_node_online(int idx);
int                  app_node_count(void);
const app_found_t   *app_found(int i);        /* NULL if unused or stale */

/* Adopt a discovered node. Returns the new slot or -1 if full. */
int  app_adopt(const uint8_t mac[6]);
void app_remove_node(int idx);
void app_rename_node(int idx, const char *name);

/* Call after editing app_node(idx)->entries/limits: saves and pushes. */
void app_schedule_changed(int idx);

/* Local epoch of the node's next scheduled feed, 0 if none. */
uint32_t app_next_feed(int idx, uint32_t now_epoch);

/* ---- manual feed ---- */
bool    app_feed_now(int idx); /* false if another request is in flight */
bool    app_feed_busy(void);
int     app_feed_last_node(void);
uint8_t app_feed_last_result(void); /* fd_result_t or APP_FEED_NO_REPLY */

/* ---- feeder firmware update ---- */
typedef enum {
    APP_OTA_IDLE,
    APP_OTA_RUNNING,
    APP_OTA_DONE,
    APP_OTA_FAILED
} app_ota_state_t;

/* Tell a feeder to fetch the firmware uploaded on the web page.
 * Returns NULL on success, or why it could not start. */
const char     *app_node_ota_start(int idx);
app_ota_state_t app_ota_state(void);
int             app_ota_node(void);
const char     *app_ota_message(void); /* detail for DONE / FAILED */

/* ---- log ---- */
int                    app_log_count(void);
const app_log_entry_t *app_log_get(int i); /* 0 = newest */
void                   app_log_clear(void);

/* ---- settings ---- */
app_settings_t *app_settings(void);
void            app_settings_save(void);

/* Every feed event is also appended to this file on the SD card. */
#define APP_SD_LOG_FILE   "feedlog.csv"
#define APP_SD_LOG_HEADER "time,feeder,source,result,seconds,pulses"

/* Text for a feed result code. */
const char *app_result_text(uint8_t result);

#ifdef __cplusplus
}
#endif

#endif
