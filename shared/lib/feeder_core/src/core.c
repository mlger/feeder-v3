#include "feeder/core.h"

#include <stddef.h>

/* ---- protocol ---- */

uint8_t fd_proto_validate(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len < sizeof(fd_hdr_t)) {
        return 0;
    }
    const fd_hdr_t *h = (const fd_hdr_t *)buf;
    if (h->magic != FD_PROTO_MAGIC || h->version != FD_PROTO_VERSION) {
        return 0;
    }

    uint16_t want;
    switch (h->type) {
    case FD_MSG_HELLO:      want = sizeof(fd_msg_hello_t); break;
    case FD_MSG_PAIR:       want = sizeof(fd_msg_pair_t); break;
    case FD_MSG_UNPAIR:
    case FD_MSG_STATUS_REQ: want = sizeof(fd_msg_simple_t); break;
    case FD_MSG_TIME_SYNC:  want = sizeof(fd_msg_time_sync_t); break;
    case FD_MSG_FEED_NOW:   want = sizeof(fd_msg_feed_now_t); break;
    case FD_MSG_STATUS:     want = sizeof(fd_msg_status_t); break;
    case FD_MSG_FEED_EVENT: want = sizeof(fd_msg_feed_event_t); break;
    case FD_MSG_ACK:        want = sizeof(fd_msg_ack_t); break;
    case FD_MSG_SCHED_SET: {
        /* Variable length: the count field decides. */
        if (len < FD_SCHED_SET_LEN(0)) {
            return 0;
        }
        uint8_t count = buf[offsetof(fd_msg_sched_set_t, count)];
        if (count > FD_MAX_ENTRIES) {
            return 0;
        }
        want = (uint16_t)FD_SCHED_SET_LEN(count);
        break;
    }
    default:
        return 0;
    }
    return len == want ? h->type : 0;
}

/* ---- time ---- */

void fd_time_split(uint32_t local_epoch, fd_tod_t *out)
{
    uint32_t days = local_epoch / 86400u;
    uint32_t secs = local_epoch % 86400u;
    out->hour = (uint8_t)(secs / 3600u);
    out->minute = (uint8_t)((secs % 3600u) / 60u);
    out->second = (uint8_t)(secs % 60u);
    out->dow = (uint8_t)((days + 4u) % 7u); /* 1970-01-01 was a Thursday */
}

/* ---- schedule ---- */

bool fd_sched_entry_valid(const fd_sched_entry_t *e)
{
    return e->hour < 24 && e->minute < 60 && e->on_ds >= 1 && e->pulses >= 1;
}

uint16_t fd_sched_due(const fd_sched_entry_t *entries, uint8_t count, uint32_t local_epoch)
{
    fd_tod_t t;
    fd_time_split(local_epoch, &t);

    uint16_t mask = 0;
    if (count > FD_MAX_ENTRIES) {
        count = FD_MAX_ENTRIES;
    }
    for (uint8_t i = 0; i < count; i++) {
        const fd_sched_entry_t *e = &entries[i];
        if (!(e->dow_mask & FD_ENTRY_ENABLED)) {
            continue;
        }
        if (!(e->dow_mask & (1u << t.dow))) {
            continue;
        }
        if (e->hour == t.hour && e->minute == t.minute && fd_sched_entry_valid(e)) {
            mask |= (uint16_t)(1u << i);
        }
    }
    return mask;
}

/* ---- safety guard ---- */

void fd_guard_roll_day(fd_guard_t *g, uint32_t now_epoch)
{
    uint32_t day = now_epoch / 86400u;
    if (day != g->day) {
        g->day = day;
        g->on_ms_today = 0;
        g->feeds_today = 0;
    }
}

fd_result_t fd_guard_check(fd_guard_t *g, const fd_limits_t *lim, uint32_t now_epoch,
                           uint8_t *on_ds, uint8_t *pulses)
{
    if (*on_ds == 0 || *pulses == 0) {
        return FD_RES_BAD_REQUEST;
    }
    if (*on_ds > FD_HARD_MAX_ON_DS) {
        *on_ds = FD_HARD_MAX_ON_DS;
    }
    if (*pulses > FD_HARD_MAX_PULSES) {
        *pulses = FD_HARD_MAX_PULSES;
    }

    fd_guard_roll_day(g, now_epoch);

    /*
     * If the clock was moved back behind the last feed we cannot know how
     * long ago it really was, so start a fresh cooldown from now.
     */
    if (g->last_feed_epoch > now_epoch) {
        g->last_feed_epoch = now_epoch;
    }
    if (g->last_feed_epoch != 0 && lim->cooldown_s != 0 &&
        now_epoch - g->last_feed_epoch < lim->cooldown_s) {
        return FD_RES_COOLDOWN;
    }

    if (lim->daily_max_on_s != 0) {
        uint32_t want_ms = (uint32_t)(*on_ds) * 100u * (uint32_t)(*pulses);
        uint32_t cap_ms = (uint32_t)lim->daily_max_on_s * 1000u;
        if (g->on_ms_today + want_ms > cap_ms) {
            return FD_RES_DAILY_CAP;
        }
    }
    return FD_RES_OK;
}

void fd_guard_commit(fd_guard_t *g, uint32_t now_epoch, uint32_t on_ms)
{
    fd_guard_roll_day(g, now_epoch);
    g->on_ms_today += on_ms;
    g->feeds_today++;
    g->last_feed_epoch = now_epoch;
}

/* ---- relay pulse sequencer ---- */

static uint8_t clamp_gap(uint8_t gap_ds)
{
    return gap_ds < FD_HARD_MIN_GAP_DS ? FD_HARD_MIN_GAP_DS : gap_ds;
}

uint32_t fd_pulse_duration_ms(uint8_t on_ds, uint8_t pulses, uint8_t gap_ds)
{
    if (on_ds > FD_HARD_MAX_ON_DS) {
        on_ds = FD_HARD_MAX_ON_DS;
    }
    if (pulses > FD_HARD_MAX_PULSES) {
        pulses = FD_HARD_MAX_PULSES;
    }
    if (on_ds == 0 || pulses == 0) {
        return 0;
    }
    return (uint32_t)pulses * on_ds * 100u + (uint32_t)(pulses - 1) * clamp_gap(gap_ds) * 100u;
}

void fd_pulse_start(fd_pulse_t *p, uint32_t now_ms, uint8_t on_ds, uint8_t pulses, uint8_t gap_ds)
{
    if (on_ds > FD_HARD_MAX_ON_DS) {
        on_ds = FD_HARD_MAX_ON_DS;
    }
    if (pulses > FD_HARD_MAX_PULSES) {
        pulses = FD_HARD_MAX_PULSES;
    }
    p->on_ms = (uint16_t)(on_ds * 100u);
    p->gap_ms = (uint16_t)(clamp_gap(gap_ds) * 100u);
    p->pulses_left = pulses;
    p->pulses_done = 0;
    p->on_ms_total = 0;
    p->phase_start_ms = now_ms;
    p->active = (on_ds != 0 && pulses != 0);
    p->relay_on = p->active;
}

bool fd_pulse_tick(fd_pulse_t *p, uint32_t now_ms)
{
    if (!p->active) {
        return false;
    }
    uint32_t elapsed = now_ms - p->phase_start_ms; /* wrap-safe */

    if (p->relay_on) {
        if (elapsed >= p->on_ms) {
            p->relay_on = false;
            p->on_ms_total += p->on_ms;
            p->pulses_done++;
            p->pulses_left--;
            p->phase_start_ms = now_ms;
            if (p->pulses_left == 0) {
                p->active = false;
            }
        }
    } else if (elapsed >= p->gap_ms) {
        p->relay_on = true;
        p->phase_start_ms = now_ms;
    }
    return p->relay_on;
}

void fd_pulse_abort(fd_pulse_t *p)
{
    p->active = false;
    p->relay_on = false;
    p->pulses_left = 0;
}
