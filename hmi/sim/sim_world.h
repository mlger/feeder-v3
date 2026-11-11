#ifndef SIM_WORLD_H
#define SIM_WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "feeder/node.h"

/* "Real" UTC, as the fake NTP server reports it: Mon 2026-11-09 00:10:00
 * at the moment the simulation starts. */
#define TRUE_UTC_AT_START (1794182400u + 600u)

/* The fake router accepts this password for any network. */
#define SIM_WIFI_PASSWORD "fishfood"

typedef struct {
    uint8_t  mac[6];
    bool     powered;       /* tests switch this to cut or restore power */
    bool     running;
    uint8_t  fw_minor;
    uint8_t  channel;
    bool     relay;
    int      relay_pulses;  /* times the relay switched on */
    uint32_t clock_base;    /* the feeder's clock chip: epoch at now_ms == 0 */
    bool     clock_set;
    bool     flash_valid;   /* flash contents, kept across reboots */
    fd_node_cfg_t   flash_cfg;
    fd_node_state_t flash_state;
    /* Optional scale under the hopper. */
    bool     scale_fitted;  /* a load cell is connected */
    bool     scale_zeroed, scale_ready;
    int32_t  hopper_mg;     /* what is really in the hopper */
    int32_t  true_mg_per_pulse;
    bool     jam;           /* the mechanism moves but no food falls */
    uint32_t ota_until_ms;  /* downloading firmware until then, 0 = not */
    bool     ota_broken;    /* this feeder fails to download */
    fd_node_t n;            /* the real node state machine */
} sim_node_t;

extern sim_node_t fnodes[2];
extern uint8_t sim_hmi_channel; /* the channel the HMI's radio is on */

/* Set a feeder's clock chip, as if it had been synchronised earlier. */
void sim_node_set_clock(int i, uint32_t local_epoch);
/* Make a feeder start already paired with the HMI, as after an earlier set-up. */
void sim_node_prepair(int i, uint8_t node_id);
/* Feeder clock minus HMI clock, in seconds. */
int32_t sim_node_clock_error(int i);

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
