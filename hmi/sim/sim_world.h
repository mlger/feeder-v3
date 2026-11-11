#ifndef SIM_WORLD_H
#define SIM_WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "feeder/core.h"

/* "Real" UTC, as the fake NTP server reports it: Mon 2026-11-09 00:10:00
 * at the moment the simulation starts. */
#define TRUE_UTC_AT_START (1794182400u + 600u)

/* The fake router accepts this password for any network. */
#define SIM_WIFI_PASSWORD "fishfood"

typedef struct {
    uint8_t          mac[6];
    bool             paired;
    bool             powered;
    bool             time_valid;
    int32_t          clock_offset; /* node clock minus sim clock, seconds */
    uint8_t          node_id;
    uint16_t         rev;
    fd_limits_t      limits;
    uint8_t          count;
    fd_sched_entry_t entries[FD_MAX_ENTRIES];
    fd_guard_t       guard;
    uint32_t         last_sched_minute;
    uint32_t         last_status_ms, last_hello_ms;
    uint16_t         next_event;
    bool             ev_pending;
    fd_msg_feed_event_t ev;
    uint32_t         ev_sent_ms;
    bool             have_feed_seq;
    uint8_t          last_feed_seq, last_feed_code;
    uint8_t          seq;
    int              feeds_run;
    uint8_t          fw_minor;
    uint32_t         boot_ms;      /* sim time of the last (re)boot */
    uint32_t         ota_until_ms; /* downloading until then, 0 = not */
    bool             ota_broken;   /* this feeder fails to download */
} fake_node_t;

extern fake_node_t fnodes[2];
extern uint32_t now_ms;
extern bool ntp_on, ntp_synced, updates_allowed;
extern char last_status[800];
extern int saves;
extern bool backlight;
extern uint32_t node_fw_size;
extern int node_ota_request;
extern bool ota_ap_open, ota_served;
extern bool setup_ap_open, setup_done;
extern bool sd_inserted;
extern int sd_lines;
extern char sd_last_line[200], sd_last_header[80];

/* Advance the world to `ms` (virtual milliseconds): run the fake feeders and
 * network and hand any frames they sent to the application. */
void sim_world_step(uint32_t ms);

#endif
