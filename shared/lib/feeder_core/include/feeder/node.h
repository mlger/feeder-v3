/*
 * The feeder node as a hardware-free state machine: pairing, the ESP-NOW
 * link, the stored schedule, feeding and the event queue.
 *
 * The node firmware supplies the hardware through fd_node_io_t and calls
 * fd_node_tick() from its main loop. The HMI simulator and the unit tests
 * drive exactly the same code with fake hardware.
 */
#ifndef FEEDER_NODE_H
#define FEEDER_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "feeder/core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FD_NODE_STATUS_PERIOD_MS 10000u  /* heartbeat to the HMI */
#define FD_NODE_LINK_TIMEOUT_MS  35000u  /* nothing heard for this long = link lost */
#define FD_NODE_HOP_PERIOD_MS    400u    /* channel dwell while searching */
#define FD_NODE_EVENT_RETRY_MS   2000u   /* resend un-ACKed feed events */
#define FD_NODE_PAIR_WINDOW_MS   120000u /* pairing mode timeout if already paired */
#define FD_NODE_FAILSAFE_MARGIN_MS 500u
#define FD_NODE_SETTLE_MS         1500u  /* let the hopper stop shaking before weighing */
#define FD_NODE_WEIGH_TIMEOUT_MS  6000u  /* give up weighing if it never settles */
#define FD_NODE_FEED_DEDUPE_MS   10000u  /* a FEED_NOW repeating the last seq within this is a retransmission */
#define FD_NODE_CHANNEL_MAX      13
#define FD_NODE_EVENT_QUEUE      16

/* Stored in flash; survives power loss. */
typedef struct {
    uint8_t          paired;
    uint8_t          hmi_mac[6];
    uint8_t          node_id;
    uint8_t          channel;
    char             name[FD_NAME_LEN];
    uint16_t         sched_rev;
    fd_limits_t      limits;
    uint8_t          count;
    fd_sched_entry_t entries[FD_MAX_ENTRIES];
} fd_node_cfg_t;

/* Also stored; changes with every feed. */
typedef struct {
    fd_guard_t guard;
    uint32_t   last_sched_minute; /* epoch/60 of the last scheduled feed handled */
    uint32_t   fed_mg_day;        /* epoch/86400 that fed_mg_today belongs to */
    uint32_t   fed_mg_today;      /* weighed food given that day */
} fd_node_state_t;

/* The hardware, as seen by the state machine. */
typedef struct {
    void *ctx;
    void (*send)(void *ctx, const uint8_t mac[6], const void *msg, uint8_t len);
    void (*set_channel)(void *ctx, uint8_t channel);
    void (*relay)(void *ctx, bool on);
    void (*save_config)(void *ctx, const fd_node_cfg_t *cfg);
    /* Returns false if the write failed; a feed is then refused. */
    bool (*save_state)(void *ctx, const fd_node_state_t *st);
    /* Set the clock. Returns false if there is no working clock chip. */
    bool (*set_clock)(void *ctx, uint32_t local_epoch);
    /* Arm an independent timer that cuts the relay after `ms`; 0 disarms. */
    void (*arm_failsafe)(void *ctx, uint32_t ms);
    /* Fetch new firmware. On real hardware this does not return. */
    void (*ota)(void *ctx, const fd_msg_ota_start_t *m);

    /* Optional scale under the hopper; all three may be NULL. */
    /* True if a load-cell amplifier is answering at all. */
    bool (*scale_found)(void *ctx);
    /* The weight of food in the hopper. False if the scale is missing, not
     * set up, or the reading is not steady right now. */
    bool (*scale_read)(void *ctx, int32_t *mg);
    /* FD_SCALE_ZERO / FD_SCALE_SPAN. False if it could not be done. */
    bool (*scale_set)(void *ctx, uint8_t op, uint32_t known_mg);
} fd_node_io_t;

typedef enum {
    FD_LED_OK,        /* linked and healthy */
    FD_LED_FEEDING,
    FD_LED_PAIRING,   /* waiting for an HMI */
    FD_LED_SEARCHING, /* paired, HMI not heard */
    FD_LED_NO_TIME    /* linked, clock not set */
} fd_node_led_t;

typedef struct {
    fd_node_cfg_t   cfg;
    fd_node_state_t st;
    fd_node_io_t    io;
    uint8_t         fw_major, fw_minor;
    bool            rtc_present, time_valid;

    /* feeding */
    fd_pulse_t   pulse;
    uint8_t      feed_source;
    volatile bool failsafe_fired;

    /* weighing a finished feed: the event is held back until it is done */
    bool        weighing;
    bool        have_before;
    int32_t     before_mg;
    uint32_t    weigh_since_ms;
    fd_result_t weigh_result;
    uint8_t     weigh_pulses;
    uint32_t    weigh_on_ms;
    bool        scale_ok;       /* a good reading has been seen since the scale was set up */
    int32_t     last_hopper_mg;

    /* events waiting for the HMI's ACK */
    fd_msg_feed_event_t events[FD_NODE_EVENT_QUEUE];
    uint8_t  ev_head, ev_count;
    uint16_t next_event_id;
    uint32_t last_event_send_ms;

    /* link */
    uint8_t  tx_seq;
    bool     pairing, linked;
    uint32_t pairing_since_ms, last_heard_ms, last_status_ms, last_hop_ms;
    uint8_t  channel;
    bool     have_feed_seq;
    uint8_t  last_feed_seq, last_feed_code;
    uint32_t last_feed_rx_ms;

    /* firmware update */
    bool               ota_pending;
    fd_msg_ota_start_t ota_msg;

    uint32_t boot_ms;
} fd_node_t;

/* Fill in defaults for a node that has never been set up. */
void fd_node_default_config(fd_node_cfg_t *cfg);

/*
 * Start the state machine. `cfg` and `st` are what was loaded from flash
 * (use fd_node_default_config() and zeroes for a blank node).
 */
void fd_node_init(fd_node_t *n, const fd_node_io_t *io, const fd_node_cfg_t *cfg,
                  const fd_node_state_t *st, uint8_t fw_major, uint8_t fw_minor,
                  bool rtc_present, bool time_valid, uint32_t now_ms, uint32_t now_epoch);

/* Call often. `now_epoch` is the node's clock (local epoch). */
void fd_node_tick(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch);

/* Hand in one received ESP-NOW frame. */
void fd_node_rx(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch, const uint8_t mac[6],
                const uint8_t *data, uint8_t len);

/* The user asked for pairing mode (button held). */
void fd_node_enter_pairing(fd_node_t *n, uint32_t now_ms);

/* The independent failsafe timer cut the relay. Safe to call from a timer. */
void fd_node_failsafe_fired(fd_node_t *n);

fd_node_led_t fd_node_led(const fd_node_t *n);

#ifdef __cplusplus
}
#endif

#endif /* FEEDER_NODE_H */
