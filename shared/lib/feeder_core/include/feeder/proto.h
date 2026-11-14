/*
 * AIO-Feeder ESP-NOW wire protocol, shared by the HMI (ESP32-S3) and the
 * feeder nodes (ESP8266). Every message starts with fd_hdr_t. All fields are
 * little-endian and the structs are packed, so they can be sent as-is.
 *
 * Times are "local epoch": seconds since 1970-01-01 00:00 in the user's local
 * wall-clock time. The HMI owns the time zone; nodes never need one.
 */
#ifndef FEEDER_PROTO_H
#define FEEDER_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FD_PROTO_MAGIC   0xFD
#define FD_PROTO_VERSION 2 /* 2: scale readings in STATUS and FEED_EVENT, SCALE_SET */

#define FD_MAX_ENTRIES 16 /* schedule entries per node */
#define FD_NAME_LEN    16 /* node name, NUL padded */

#define FD_PACKED __attribute__((packed))

typedef enum {
    FD_MSG_HELLO      = 1,  /* node -> broadcast: "I am unpaired / in pairing mode" */
    FD_MSG_PAIR       = 2,  /* hmi  -> node: adopt this node */
    FD_MSG_UNPAIR     = 3,  /* hmi  -> node: forget the HMI */
    FD_MSG_TIME_SYNC  = 4,  /* hmi  -> node: set the DS3231 */
    FD_MSG_SCHED_SET  = 5,  /* hmi  -> node: replace schedule + limits */
    FD_MSG_FEED_NOW   = 6,  /* hmi  -> node: manual feed */
    FD_MSG_STATUS_REQ = 7,  /* hmi  -> node: send a STATUS now */
    FD_MSG_STATUS     = 8,  /* node -> hmi: periodic heartbeat */
    FD_MSG_FEED_EVENT = 9,  /* node -> hmi: a feed ran or was refused */
    FD_MSG_ACK        = 10, /* both ways */
    FD_MSG_OTA_START  = 11, /* hmi  -> node: fetch new firmware over Wi-Fi */
    FD_MSG_SCALE_SET  = 12  /* hmi  -> node: zero the scale or set its span */
} fd_msg_type_t;

/* Outcome of a feed request. Also used as the ACK code. */
typedef enum {
    FD_RES_OK           = 0,
    FD_RES_COOLDOWN     = 1, /* too soon after the previous feed */
    FD_RES_DAILY_CAP    = 2, /* would exceed the daily on-time limit */
    FD_RES_BUSY         = 3, /* a feed is already running */
    FD_RES_BAD_REQUEST  = 4, /* malformed message or zero-length feed */
    FD_RES_ABORTED      = 5, /* failsafe cut the relay */
    FD_RES_TIME_INVALID = 6, /* clock not set; scheduled feed not run */
    FD_RES_STORAGE      = 7  /* the feed could not be recorded to flash, so it was not run */
} fd_result_t;

/* STATUS flags */
#define FD_FLAG_TIME_VALID  0x01 /* DS3231 present and has been set */
#define FD_FLAG_RTC_MISSING 0x02 /* DS3231 not answering on I2C */
#define FD_FLAG_FEEDING     0x04 /* relay sequence in progress */
#define FD_FLAG_PAIRING     0x08 /* node is in pairing mode */
#define FD_FLAG_SCALE_FOUND 0x10 /* a load-cell amplifier is answering */
#define FD_FLAG_SCALE       0x20 /* the scale is set up: weights are valid */

/* measured_mg in FEED_EVENT when the feed was not weighed. */
#define FD_NOT_MEASURED 0xFFFFFFFFu

/* fd_msg_scale_set_t.op */
#define FD_SCALE_ZERO 1 /* the hopper is empty now */
#define FD_SCALE_SPAN 2 /* the hopper now holds known_mg */

/* Feed source in FEED_EVENT: schedule entry index, or this for manual. */
#define FD_SOURCE_MANUAL 0xFF

/* dow_mask: bit0 = Sunday ... bit6 = Saturday, bit7 = entry enabled */
#define FD_DOW_ALL     0x7F
#define FD_ENTRY_ENABLED 0x80

typedef struct FD_PACKED {
    uint8_t magic;   /* FD_PROTO_MAGIC */
    uint8_t version; /* FD_PROTO_VERSION */
    uint8_t type;    /* fd_msg_type_t */
    uint8_t seq;     /* sender's counter; an ACK echoes it */
} fd_hdr_t;

/* One feed = `pulses` relay activations of `on_ds` x 0.1 s each. */
typedef struct FD_PACKED {
    uint8_t hour;     /* 0-23 */
    uint8_t minute;   /* 0-59 */
    uint8_t dow_mask;
    uint8_t on_ds;    /* relay on-time per pulse, 0.1 s units */
    uint8_t pulses;   /* 1..FD_HARD_MAX_PULSES */
} fd_sched_entry_t;

/* Limits enforced by the node on top of its compile-time hard limits. */
typedef struct FD_PACKED {
    uint8_t  gap_ds;          /* relay off-time between pulses, 0.1 s units */
    uint16_t cooldown_s;      /* minimum seconds between two feeds */
    uint16_t daily_max_on_s;  /* max total relay on-time per day; 0 = no cap */
} fd_limits_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  flags;
} fd_msg_hello_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint8_t  node_id;
    char     name[FD_NAME_LEN];
} fd_msg_pair_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;
} fd_msg_simple_t; /* UNPAIR, STATUS_REQ */

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint32_t local_epoch;
} fd_msg_time_sync_t;

typedef struct FD_PACKED {
    fd_hdr_t         hdr;
    uint16_t         rev;    /* schedule revision, chosen by the HMI */
    fd_limits_t      limits;
    uint8_t          count;  /* entries that follow, 0..FD_MAX_ENTRIES */
    fd_sched_entry_t entries[FD_MAX_ENTRIES];
} fd_msg_sched_set_t;

/* Bytes actually sent for a SCHED_SET carrying `count` entries. */
#define FD_SCHED_SET_LEN(count) \
    (sizeof(fd_msg_sched_set_t) - (FD_MAX_ENTRIES - (count)) * sizeof(fd_sched_entry_t))

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint8_t  on_ds;
    uint8_t  pulses;
} fd_msg_feed_now_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint8_t  node_id;
    uint8_t  flags;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint16_t sched_rev;
    uint32_t local_epoch;      /* node clock right now */
    uint32_t last_feed_epoch;  /* 0 = never */
    uint32_t on_ms_today;
    uint16_t feeds_today;
    uint32_t uptime_s;
    int32_t  hopper_mg;        /* food in the hopper; valid with FD_FLAG_SCALE */
    uint32_t fed_mg_today;     /* weighed food given today; valid with FD_FLAG_SCALE */
} fd_msg_status_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint8_t  node_id;
    uint16_t event_id;   /* node's event counter; ACK echoes it */
    uint32_t local_epoch;
    uint8_t  source;     /* schedule entry index or FD_SOURCE_MANUAL */
    uint8_t  result;     /* fd_result_t */
    uint8_t  pulses_done;
    uint16_t on_ms_total;
    uint32_t measured_mg; /* weight the hopper lost, or FD_NOT_MEASURED */
} fd_msg_feed_event_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;
    uint8_t  op;       /* FD_SCALE_ZERO or FD_SCALE_SPAN */
    uint32_t known_mg; /* for FD_SCALE_SPAN */
} fd_msg_scale_set_t;

/*
 * The HMI opens a temporary access point that serves the node firmware at
 * http://ip:port/node.bin. The credentials are random and only valid for
 * that one update, so sending them unencrypted gives nothing lasting away.
 */
#define FD_OTA_STR_LEN 24
typedef struct FD_PACKED {
    fd_hdr_t hdr;
    char     ssid[FD_OTA_STR_LEN]; /* NUL terminated */
    char     pass[FD_OTA_STR_LEN]; /* NUL terminated */
    uint8_t  ip[4];
    uint16_t port;
    uint32_t size;                 /* firmware size in bytes */
} fd_msg_ota_start_t;

typedef struct FD_PACKED {
    fd_hdr_t hdr;        /* hdr.seq = seq of the message being acknowledged */
    uint8_t  acked_type;
    uint8_t  code;       /* fd_result_t */
    uint16_t event_id;   /* only for FEED_EVENT acks */
} fd_msg_ack_t;

/* Fill a header. */
static inline void fd_hdr_init(fd_hdr_t *h, uint8_t type, uint8_t seq)
{
    h->magic = FD_PROTO_MAGIC;
    h->version = FD_PROTO_VERSION;
    h->type = type;
    h->seq = seq;
}

/*
 * Check a received buffer: magic, version, and that `len` is right for the
 * message type. Returns the type, or 0 if the packet must be dropped.
 */
uint8_t fd_proto_validate(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* FEEDER_PROTO_H */
