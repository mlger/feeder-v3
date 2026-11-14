#include "feeder/node.h"

#include <string.h>

static void roll_fed_day(fd_node_t *n, uint32_t now_epoch);

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void fd_node_default_config(fd_node_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->channel = 1;
    cfg->limits.gap_ds = 5;
    cfg->limits.cooldown_s = 600;
    cfg->limits.daily_max_on_s = 30;
}

/*
 * Scheduled feeds are skipped for any minute at or before the last one that
 * fed, which stops a repeat after a reboot or a small backwards correction
 * (for example the end of daylight saving). If the clock is now far behind
 * that mark, the mark came from a wrong clock; drop it so feeding resumes.
 */
static void reconcile_schedule_mark(fd_node_t *n, uint32_t epoch)
{
    uint32_t minute = epoch / 60u;
    if (minute + 120u < n->st.last_sched_minute) {
        n->st.last_sched_minute = minute;
        n->io.save_state(n->io.ctx, &n->st);
    }
}

static void set_channel(fd_node_t *n, uint8_t ch)
{
    n->channel = ch;
    n->io.set_channel(n->io.ctx, ch);
}

void fd_node_init(fd_node_t *n, const fd_node_io_t *io, const fd_node_cfg_t *cfg,
                  const fd_node_state_t *st, uint8_t fw_major, uint8_t fw_minor,
                  bool rtc_present, bool time_valid, uint32_t now_ms, uint32_t now_epoch)
{
    memset(n, 0, sizeof(*n));
    n->io = *io;
    n->cfg = *cfg;
    n->st = *st;
    n->fw_major = fw_major;
    n->fw_minor = fw_minor;
    n->rtc_present = rtc_present;
    n->time_valid = rtc_present && time_valid;
    n->next_event_id = 1;
    n->feed_source = FD_SOURCE_MANUAL;
    n->boot_ms = now_ms;
    if (n->cfg.count > FD_MAX_ENTRIES || n->cfg.channel < 1 || n->cfg.channel > FD_NODE_CHANNEL_MAX) {
        fd_node_default_config(&n->cfg);
    }

    n->io.relay(n->io.ctx, false);
    if (n->time_valid) {
        reconcile_schedule_mark(n, now_epoch);
    }
    set_channel(n, n->cfg.channel);
    if (!n->cfg.paired) {
        fd_node_enter_pairing(n, now_ms);
    }
}

void fd_node_enter_pairing(fd_node_t *n, uint32_t now_ms)
{
    n->pairing = true;
    n->pairing_since_ms = now_ms;
}

void fd_node_failsafe_fired(fd_node_t *n) { n->failsafe_fired = true; }

fd_node_led_t fd_node_led(const fd_node_t *n)
{
    if (n->pulse.active) {
        return FD_LED_FEEDING;
    }
    if (n->pairing || !n->cfg.paired) {
        return FD_LED_PAIRING;
    }
    if (!n->linked) {
        return FD_LED_SEARCHING;
    }
    return n->time_valid ? FD_LED_OK : FD_LED_NO_TIME;
}

/* ---- sending ---- */

static void send_ack(fd_node_t *n, const uint8_t mac[6], const fd_hdr_t *req, uint8_t code)
{
    fd_msg_ack_t a;
    fd_hdr_init(&a.hdr, FD_MSG_ACK, req->seq);
    a.acked_type = req->type;
    a.code = code;
    a.event_id = 0;
    n->io.send(n->io.ctx, mac, &a, sizeof(a));
}

static uint8_t status_flags(const fd_node_t *n)
{
    uint8_t f = 0;
    if (n->time_valid) f |= FD_FLAG_TIME_VALID;
    if (!n->rtc_present) f |= FD_FLAG_RTC_MISSING;
    if (n->pulse.active) f |= FD_FLAG_FEEDING;
    if (n->pairing) f |= FD_FLAG_PAIRING;
    if (n->io.scale_found && n->io.scale_found(n->io.ctx)) f |= FD_FLAG_SCALE_FOUND;
    return f;
}

static void send_status(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch)
{
    fd_msg_status_t s;
    fd_guard_roll_day(&n->st.guard, now_epoch);
    fd_hdr_init(&s.hdr, FD_MSG_STATUS, ++n->tx_seq);
    s.node_id = n->cfg.node_id;
    s.flags = status_flags(n);
    s.fw_major = n->fw_major;
    s.fw_minor = n->fw_minor;
    s.sched_rev = n->cfg.sched_rev;
    s.local_epoch = now_epoch;
    s.last_feed_epoch = n->st.guard.last_feed_epoch;
    s.on_ms_today = n->st.guard.on_ms_today;
    s.feeds_today = n->st.guard.feeds_today;
    s.uptime_s = (now_ms - n->boot_ms) / 1000u;
    s.hopper_mg = 0;
    roll_fed_day(n, now_epoch);
    s.fed_mg_today = n->st.fed_mg_today;
    int32_t mg;
    if (n->io.scale_read && n->io.scale_read(n->io.ctx, &mg)) {
        s.flags |= FD_FLAG_SCALE;
        s.hopper_mg = mg;
    } else if (n->scale_ok) {
        /* Not steady at this instant (for example mid-feed): report the
         * last good reading instead of dropping the scale from view. */
        s.flags |= FD_FLAG_SCALE;
        s.hopper_mg = n->last_hopper_mg;
    }
    n->io.send(n->io.ctx, n->cfg.hmi_mac, &s, sizeof(s));
}

static void roll_fed_day(fd_node_t *n, uint32_t now_epoch)
{
    uint32_t day = now_epoch / 86400u;
    if (day != n->st.fed_mg_day) {
        n->st.fed_mg_day = day;
        n->st.fed_mg_today = 0;
    }
}

static void send_hello(fd_node_t *n)
{
    fd_msg_hello_t h;
    fd_hdr_init(&h.hdr, FD_MSG_HELLO, ++n->tx_seq);
    h.fw_major = n->fw_major;
    h.fw_minor = n->fw_minor;
    h.flags = status_flags(n);
    n->io.send(n->io.ctx, BROADCAST, &h, sizeof(h));
}

/* ---- events ---- */

static void push_event(fd_node_t *n, uint32_t now_epoch, uint8_t source, fd_result_t result,
                       uint8_t pulses_done, uint32_t on_ms, uint32_t measured_mg)
{
    if (n->ev_count == FD_NODE_EVENT_QUEUE) { /* full: drop the oldest */
        n->ev_head = (uint8_t)((n->ev_head + 1) % FD_NODE_EVENT_QUEUE);
        n->ev_count--;
    }
    fd_msg_feed_event_t *e = &n->events[(n->ev_head + n->ev_count) % FD_NODE_EVENT_QUEUE];
    fd_hdr_init(&e->hdr, FD_MSG_FEED_EVENT, 0);
    e->node_id = n->cfg.node_id;
    e->event_id = n->next_event_id++;
    e->local_epoch = now_epoch;
    e->source = source;
    e->result = (uint8_t)result;
    e->pulses_done = pulses_done;
    e->on_ms_total = on_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)on_ms;
    e->measured_mg = measured_mg;
    n->ev_count++;
    n->last_event_send_ms = 0; /* send on the next tick */
}

/* ---- feeding ---- */

/* A steady scale reading, remembered for status reports. */
static bool scale_now(fd_node_t *n, int32_t *mg)
{
    if (!n->io.scale_read || !n->io.scale_read(n->io.ctx, mg)) {
        return false;
    }
    n->scale_ok = true;
    n->last_hopper_mg = *mg;
    return true;
}

/* The relay sequence is over: report it, after weighing if there is a scale. */
static void feed_finished(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch, fd_result_t result,
                          uint8_t pulses, uint32_t on_ms)
{
    if (!n->have_before) {
        push_event(n, now_epoch, n->feed_source, result, pulses, on_ms, FD_NOT_MEASURED);
        return;
    }
    n->weighing = true;
    n->weigh_since_ms = now_ms;
    n->weigh_result = result;
    n->weigh_pulses = pulses;
    n->weigh_on_ms = on_ms;
}

static void weigh_tick(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch)
{
    if (!n->weighing || now_ms - n->weigh_since_ms < FD_NODE_SETTLE_MS) {
        return;
    }
    int32_t after;
    uint32_t measured = FD_NOT_MEASURED;
    if (scale_now(n, &after)) {
        measured = n->before_mg > after ? (uint32_t)(n->before_mg - after) : 0;
        roll_fed_day(n, now_epoch);
        n->st.fed_mg_today += measured;
        n->io.save_state(n->io.ctx, &n->st);
    } else if (now_ms - n->weigh_since_ms < FD_NODE_WEIGH_TIMEOUT_MS) {
        return; /* still shaking: try again */
    }
    n->weighing = false;
    push_event(n, now_epoch, n->feed_source, n->weigh_result, n->weigh_pulses, n->weigh_on_ms, measured);
}

static fd_result_t try_feed(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch, uint8_t source,
                            uint8_t on_ds, uint8_t pulses)
{
    if (n->pulse.active || n->weighing) {
        return FD_RES_BUSY;
    }
    fd_result_t res = fd_guard_check(&n->st.guard, &n->cfg.limits, now_epoch, &on_ds, &pulses);
    if (res != FD_RES_OK) {
        return res;
    }

    /* Record the feed before energising the relay. If power fails mid-feed
     * the node must not repeat it on the next boot. */
    fd_guard_t before = n->st.guard;
    fd_guard_commit(&n->st.guard, now_epoch, (uint32_t)on_ds * 100u * pulses);
    if (!n->io.save_state(n->io.ctx, &n->st)) {
        n->st.guard = before; /* not recorded, so the feed must not happen */
        return FD_RES_STORAGE;
    }

    n->feed_source = source;
    n->failsafe_fired = false;
    /* With a scale, the feed is measured as the weight the hopper loses. */
    n->have_before = scale_now(n, &n->before_mg);
    n->io.arm_failsafe(n->io.ctx, fd_pulse_duration_ms(on_ds, pulses, n->cfg.limits.gap_ds) +
                                      FD_NODE_FAILSAFE_MARGIN_MS);
    fd_pulse_start(&n->pulse, now_ms, on_ds, pulses, n->cfg.limits.gap_ds);
    n->io.relay(n->io.ctx, n->pulse.relay_on);
    return FD_RES_OK;
}

static void feed_tick(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch)
{
    if (n->failsafe_fired) {
        n->failsafe_fired = false;
        if (n->pulse.active) {
            uint8_t done = n->pulse.pulses_done;
            uint32_t on = n->pulse.on_ms_total;
            fd_pulse_abort(&n->pulse);
            n->io.relay(n->io.ctx, false);
            feed_finished(n, now_ms, now_epoch, FD_RES_ABORTED, done, on);
        }
        return;
    }
    if (!n->pulse.active) {
        return;
    }
    n->io.relay(n->io.ctx, fd_pulse_tick(&n->pulse, now_ms));
    if (!n->pulse.active) {
        n->io.arm_failsafe(n->io.ctx, 0);
        n->io.relay(n->io.ctx, false);
        feed_finished(n, now_ms, now_epoch, FD_RES_OK, n->pulse.pulses_done, n->pulse.on_ms_total);
    }
}

static void schedule_tick(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch)
{
    if (!n->time_valid) {
        return;
    }
    uint32_t minute = now_epoch / 60u;
    /* Each minute is handled once, and never a minute at or before one that
     * has already fed. This survives reboots and the clock being set back. */
    if (minute <= n->st.last_sched_minute) {
        return;
    }
    uint16_t due = fd_sched_due(n->cfg.entries, n->cfg.count, now_epoch);
    if (due == 0) {
        return;
    }
    n->st.last_sched_minute = minute;
    bool started = false;
    for (uint8_t i = 0; i < n->cfg.count; i++) {
        if (!(due & (1u << i))) {
            continue;
        }
        fd_result_t res = started ? FD_RES_BUSY
                                  : try_feed(n, now_ms, now_epoch, i, n->cfg.entries[i].on_ds,
                                             n->cfg.entries[i].pulses);
        if (res == FD_RES_OK) {
            started = true; /* try_feed saved the state */
        } else {
            push_event(n, now_epoch, i, res, 0, 0, FD_NOT_MEASURED);
        }
    }
    if (!started) {
        n->io.save_state(n->io.ctx, &n->st);
    }
}

/* ---- receive ---- */

void fd_node_rx(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch, const uint8_t mac[6],
                const uint8_t *data, uint8_t len)
{
    uint8_t type = fd_proto_validate(data, len);
    if (type == 0) {
        return;
    }
    const fd_hdr_t *hdr = (const fd_hdr_t *)data;

    /* PAIR is the only message accepted from an unknown sender, and only
     * while the user has put the node in pairing mode. */
    if (type == FD_MSG_PAIR) {
        if (!n->pairing) {
            return;
        }
        const fd_msg_pair_t *m = (const fd_msg_pair_t *)data;
        memcpy(n->cfg.hmi_mac, mac, 6);
        n->cfg.paired = 1;
        n->cfg.node_id = m->node_id;
        memcpy(n->cfg.name, m->name, FD_NAME_LEN);
        n->cfg.name[FD_NAME_LEN - 1] = '\0';
        n->cfg.channel = n->channel;
        n->io.save_config(n->io.ctx, &n->cfg);
        n->pairing = false;
        n->linked = true;
        n->last_heard_ms = now_ms;
        n->last_status_ms = now_ms - FD_NODE_STATUS_PERIOD_MS; /* report in at once */
        send_ack(n, mac, hdr, FD_RES_OK);
        return;
    }

    if (!n->cfg.paired || memcmp(mac, n->cfg.hmi_mac, 6) != 0) {
        return;
    }
    /* Anything valid from our HMI proves the channel is right. */
    n->last_heard_ms = now_ms;
    if (!n->linked) {
        n->linked = true;
        if (n->cfg.channel != n->channel) {
            n->cfg.channel = n->channel;
            n->io.save_config(n->io.ctx, &n->cfg);
        }
    }

    switch (type) {
    case FD_MSG_ACK: {
        const fd_msg_ack_t *a = (const fd_msg_ack_t *)data;
        if (a->acked_type == FD_MSG_FEED_EVENT && n->ev_count > 0 &&
            a->event_id == n->events[n->ev_head].event_id) {
            n->ev_head = (uint8_t)((n->ev_head + 1) % FD_NODE_EVENT_QUEUE);
            n->ev_count--;
            n->last_event_send_ms = 0;
        }
        break;
    }
    case FD_MSG_TIME_SYNC: {
        const fd_msg_time_sync_t *m = (const fd_msg_time_sync_t *)data;
        if (n->rtc_present && n->io.set_clock(n->io.ctx, m->local_epoch)) {
            n->time_valid = true;
            reconcile_schedule_mark(n, m->local_epoch);
            send_ack(n, mac, hdr, FD_RES_OK);
        } else {
            send_ack(n, mac, hdr, FD_RES_TIME_INVALID);
        }
        break;
    }
    case FD_MSG_SCHED_SET: {
        const fd_msg_sched_set_t *m = (const fd_msg_sched_set_t *)data;
        bool ok = true;
        for (uint8_t i = 0; i < m->count; i++) {
            ok = ok && fd_sched_entry_valid(&m->entries[i]);
        }
        if (ok) {
            n->cfg.sched_rev = m->rev;
            n->cfg.limits = m->limits;
            n->cfg.count = m->count;
            memset(n->cfg.entries, 0, sizeof(n->cfg.entries));
            memcpy(n->cfg.entries, m->entries, m->count * sizeof(fd_sched_entry_t));
            n->io.save_config(n->io.ctx, &n->cfg);
        }
        send_ack(n, mac, hdr, ok ? FD_RES_OK : FD_RES_BAD_REQUEST);
        break;
    }
    case FD_MSG_FEED_NOW: {
        const fd_msg_feed_now_t *m = (const fd_msg_feed_now_t *)data;
        /* A retransmission of the request we just handled gets the same
         * answer and must not feed twice. Only a recent one counts: the
         * sequence number is 8 bits and wraps, or restarts when the HMI
         * reboots, so an old match is a new request. */
        bool repeat = n->have_feed_seq && hdr->seq == n->last_feed_seq &&
                      now_ms - n->last_feed_rx_ms < FD_NODE_FEED_DEDUPE_MS;
        if (!repeat) {
            n->last_feed_seq = hdr->seq;
            n->last_feed_rx_ms = now_ms;
            n->have_feed_seq = true;
            n->last_feed_code = (uint8_t)try_feed(n, now_ms, now_epoch, FD_SOURCE_MANUAL, m->on_ds, m->pulses);
        }
        send_ack(n, mac, hdr, n->last_feed_code);
        break;
    }
    case FD_MSG_OTA_START:
        if (n->pulse.active) {
            send_ack(n, mac, hdr, FD_RES_BUSY);
        } else {
            memcpy(&n->ota_msg, data, sizeof(n->ota_msg));
            n->ota_pending = true;
            send_ack(n, mac, hdr, FD_RES_OK);
        }
        break;
    case FD_MSG_SCALE_SET: {
        const fd_msg_scale_set_t *m = (const fd_msg_scale_set_t *)data;
        bool ok = !n->pulse.active && !n->weighing && n->io.scale_set &&
                  (m->op == FD_SCALE_ZERO || m->op == FD_SCALE_SPAN) &&
                  n->io.scale_set(n->io.ctx, m->op, m->known_mg);
        if (ok) {
            n->scale_ok = false; /* old readings no longer mean anything */
        }
        send_ack(n, mac, hdr, ok ? FD_RES_OK : FD_RES_BAD_REQUEST);
        break;
    }
    case FD_MSG_STATUS_REQ:
        send_status(n, now_ms, now_epoch);
        n->last_status_ms = now_ms;
        break;
    case FD_MSG_UNPAIR:
        send_ack(n, mac, hdr, FD_RES_OK);
        n->cfg.paired = 0;
        memset(n->cfg.hmi_mac, 0, 6);
        n->io.save_config(n->io.ctx, &n->cfg);
        n->linked = false;
        fd_node_enter_pairing(n, now_ms);
        break;
    default:
        break;
    }
}

/* ---- link ---- */

static void hop(fd_node_t *n)
{
    set_channel(n, n->channel >= FD_NODE_CHANNEL_MAX ? 1 : (uint8_t)(n->channel + 1));
}

static void link_tick(fd_node_t *n, uint32_t ms, uint32_t now_epoch)
{
    if (n->pairing && n->cfg.paired && ms - n->pairing_since_ms > FD_NODE_PAIR_WINDOW_MS) {
        n->pairing = false;
        set_channel(n, n->cfg.channel);
    }

    if (n->pairing || !n->cfg.paired) {
        /* Walk the channels announcing ourselves until an HMI adopts us. */
        if (ms - n->last_hop_ms >= FD_NODE_HOP_PERIOD_MS) {
            n->last_hop_ms = ms;
            hop(n);
            send_hello(n);
        }
        return;
    }

    if (n->linked && ms - n->last_heard_ms > FD_NODE_LINK_TIMEOUT_MS) {
        n->linked = false;
    }

    if (!n->linked) {
        /* The HMI follows its Wi-Fi router's channel, which can change. */
        if (ms - n->last_hop_ms >= FD_NODE_HOP_PERIOD_MS) {
            n->last_hop_ms = ms;
            hop(n);
            send_status(n, ms, now_epoch);
        }
        return;
    }

    if (ms - n->last_status_ms >= FD_NODE_STATUS_PERIOD_MS) {
        n->last_status_ms = ms;
        send_status(n, ms, now_epoch);
    }
    if (n->ev_count > 0 &&
        (n->last_event_send_ms == 0 || ms - n->last_event_send_ms >= FD_NODE_EVENT_RETRY_MS)) {
        n->last_event_send_ms = ms ? ms : 1;
        fd_msg_feed_event_t *e = &n->events[n->ev_head];
        e->hdr.seq = ++n->tx_seq;
        e->node_id = n->cfg.node_id;
        n->io.send(n->io.ctx, n->cfg.hmi_mac, e, sizeof(*e));
    }
}

void fd_node_tick(fd_node_t *n, uint32_t now_ms, uint32_t now_epoch)
{
    feed_tick(n, now_ms, now_epoch);
    weigh_tick(n, now_ms, now_epoch);
    schedule_tick(n, now_ms, now_epoch);
    link_tick(n, now_ms, now_epoch);

    if (n->ota_pending && !n->pulse.active && !n->weighing) {
        n->ota_pending = false;
        n->io.arm_failsafe(n->io.ctx, 0);
        n->io.relay(n->io.ctx, false);
        n->io.ota(n->io.ctx, &n->ota_msg);
    }
}
