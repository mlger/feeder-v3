/*
 * AIO-Feeder core logic: schedule matching, feed safety limits and the relay
 * pulse sequencer. Pure C with no hardware access, so it is unit-tested on
 * the host and runs unchanged on the node.
 */
#ifndef FEEDER_CORE_H
#define FEEDER_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "feeder/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hard limits compiled into the node. Nothing received over the air can
 * exceed them. Override with -D at build time to suit the mechanism.
 */
#ifndef FD_HARD_MAX_ON_DS
#define FD_HARD_MAX_ON_DS 50 /* 5.0 s continuous relay on-time */
#endif
#ifndef FD_HARD_MAX_PULSES
#define FD_HARD_MAX_PULSES 20
#endif
#ifndef FD_HARD_MIN_GAP_DS
#define FD_HARD_MIN_GAP_DS 2 /* 0.2 s rest between pulses */
#endif

/* ---- time ---- */

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t dow; /* 0 = Sunday */
} fd_tod_t;

void fd_time_split(uint32_t local_epoch, fd_tod_t *out);

typedef struct {
    uint16_t year;
    uint8_t  month;  /* 1-12 */
    uint8_t  day;    /* 1-31 */
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  dow;    /* 0 = Sunday */
} fd_civil_t;

/* Calendar date <-> local epoch. `dow` is ignored by fd_civil_to_epoch. */
void     fd_civil_from_epoch(uint32_t local_epoch, fd_civil_t *out);
uint32_t fd_civil_to_epoch(const fd_civil_t *c);
uint8_t  fd_days_in_month(uint16_t year, uint8_t month);

/* ---- schedule ---- */

/* True if every field of the entry is in range. */
bool fd_sched_entry_valid(const fd_sched_entry_t *e);

/*
 * Bitmask of entries (bit i = entries[i]) that are enabled and due in the
 * minute containing `local_epoch`.
 */
uint16_t fd_sched_due(const fd_sched_entry_t *entries, uint8_t count, uint32_t local_epoch);

/* ---- safety guard ---- */

typedef struct {
    uint32_t day;             /* local_epoch / 86400 of the counters below */
    uint32_t on_ms_today;
    uint16_t feeds_today;
    uint32_t last_feed_epoch; /* 0 = never */
} fd_guard_t;

/*
 * Decide whether a feed may run now. Clamps *on_ds and *pulses to the hard
 * limits first, then applies cooldown and daily cap. Does not change the
 * counters; call fd_guard_commit() when the feed actually starts.
 */
fd_result_t fd_guard_check(fd_guard_t *g, const fd_limits_t *lim, uint32_t now_epoch,
                           uint8_t *on_ds, uint8_t *pulses);

void fd_guard_commit(fd_guard_t *g, uint32_t now_epoch, uint32_t on_ms);

/* Reset the daily counters if `now_epoch` is on a different day. */
void fd_guard_roll_day(fd_guard_t *g, uint32_t now_epoch);

/* ---- relay pulse sequencer ---- */

typedef struct {
    bool     active;
    bool     relay_on;
    uint8_t  pulses_left;  /* including the one in progress */
    uint8_t  pulses_done;
    uint16_t on_ms;
    uint16_t gap_ms;
    uint32_t phase_start_ms;
    uint32_t on_ms_total;  /* completed on-time */
} fd_pulse_t;

/* Start a sequence. on_ds / pulses / gap_ds are clamped to the hard limits. */
void fd_pulse_start(fd_pulse_t *p, uint32_t now_ms, uint8_t on_ds, uint8_t pulses, uint8_t gap_ds);

/* Advance the sequence. Returns the level the relay must have right now. */
bool fd_pulse_tick(fd_pulse_t *p, uint32_t now_ms);

/* Stop immediately; the relay must be switched off by the caller. */
void fd_pulse_abort(fd_pulse_t *p);

/* Worst-case duration of a sequence, for arming a failsafe timer. */
uint32_t fd_pulse_duration_ms(uint8_t on_ds, uint8_t pulses, uint8_t gap_ds);

#ifdef __cplusplus
}
#endif

#endif /* FEEDER_CORE_H */
