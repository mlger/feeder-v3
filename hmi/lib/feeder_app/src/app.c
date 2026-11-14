#include "app.h"

#include <stdio.h>
#include <string.h>

#include "plat.h"

#define STORE_MAGIC 0x41494F31u /* "AIO1" */

#define PUSH_PERIOD_MS   3000u /* min gap between repeated pushes to a node */
#define FEED_RETRY_MS    400u
#define FEED_TRIES       5
/* A feeder in pairing mode visits our channel once every ~5 s (13 channels
 * x 400 ms), so keep it listed for more than two rounds. */
#define FOUND_TTL_MS     12000u
#define TIME_TOLERANCE_S 5u

typedef struct {
    uint32_t       magic;
    app_node_cfg_t nodes[APP_MAX_NODES];
} store_nodes_t;

typedef struct {
    uint32_t        magic;
    uint16_t        head;  /* next write position */
    uint16_t        count;
    app_log_entry_t e[APP_LOG_SIZE];
} store_log_t;

typedef struct {
    uint32_t       magic;
    app_settings_t s;
} store_settings_t;

static store_nodes_t    nodes;
static store_log_t      logbuf;
static store_settings_t settings;
static app_node_rt_t    rt[APP_MAX_NODES];
static app_found_t      found[APP_MAX_FOUND];
static uint32_t         changes;
static uint8_t          tx_seq;

static struct {
    bool     active;
    int      node;
    uint8_t  seq;
    uint8_t  tries;
    uint32_t last_ms;
    uint8_t  on_ds, pulses;
    uint8_t  result;
    int      last_node;
} feed = { .last_node = -1 };

static struct {
    bool               active;
    int                node, last_node;
    uint8_t            tries, result;
    uint32_t           last_ms;
    fd_msg_scale_set_t msg;
} scale = { .last_node = -1 };

#define OTA_TIMEOUT_MS  150000u
#define OTA_RETRY_MS    1000u
#define OTA_START_TRIES 8

static struct {
    app_ota_state_t    state;
    int                node;
    uint32_t           start_ms, last_ms;
    uint8_t            tries;
    bool               accepted; /* the feeder acknowledged the request */
    fd_msg_ota_start_t msg;
    char               text[64];
} ota = { .node = -1 };

/* Grams asked for per feeding time. Kept apart from `nodes` so that the stored
 * feeders, schedules and history survive this being added. */
typedef struct {
    uint32_t magic;
    uint16_t t[APP_MAX_NODES][FD_MAX_ENTRIES];
} store_targets_t;
static store_targets_t targets;

/* Per feeder: plan amounts by flow rate (g/s) instead of whole pulses. Its own
 * blob, so the stored feeders and targets survive this being added. */
typedef struct {
    uint32_t magic;
    uint8_t  by_rate[APP_MAX_NODES];
} store_modes_t;
static store_modes_t modes;
static void save_modes(void) { plat_save("modes", &modes, sizeof(modes)); }

static void save_targets(void) { plat_save("targets", &targets, sizeof(targets)); }
static void save_nodes(void)
{
    plat_save("nodes", &nodes, sizeof(nodes));
    save_targets();
}
static void save_log(void) { plat_save("log", &logbuf, sizeof(logbuf)); }

static void ota_finish(app_ota_state_t state, const char *text);
static void resync_targets(int idx);
static void learn_from_weighing(int idx, const fd_msg_feed_event_t *ev);
static void ota_on_status(int idx, const fd_msg_status_t *st);

/* ---- init ---- */

void app_init(void)
{
    if (!plat_load("nodes", &nodes, sizeof(nodes)) || nodes.magic != STORE_MAGIC) {
        memset(&nodes, 0, sizeof(nodes));
        nodes.magic = STORE_MAGIC;
    }
    if (!plat_load("targets", &targets, sizeof(targets)) || targets.magic != STORE_MAGIC) {
        memset(&targets, 0, sizeof(targets));
        targets.magic = STORE_MAGIC;
    }
    if (!plat_load("modes", &modes, sizeof(modes)) || modes.magic != STORE_MAGIC) {
        memset(&modes, 0, sizeof(modes));
        modes.magic = STORE_MAGIC;
    }
    if (!plat_load("log", &logbuf, sizeof(logbuf)) || logbuf.magic != STORE_MAGIC ||
        logbuf.head >= APP_LOG_SIZE || logbuf.count > APP_LOG_SIZE) {
        memset(&logbuf, 0, sizeof(logbuf));
        logbuf.magic = STORE_MAGIC;
    }
    if (!plat_load("settings", &settings, sizeof(settings)) || settings.magic != STORE_MAGIC) {
        memset(&settings, 0, sizeof(settings));
        settings.magic = STORE_MAGIC;
        settings.s.dark = 1;
        settings.s.sleep_min = 5;
        settings.s.feed_on_ds = 10;
        settings.s.feed_pulses = 1;
        settings.s.ntp = 0;
        settings.s.utc_offset_min = 0;
    }
    plat_set_utc_offset(settings.s.utc_offset_min);
    plat_ntp_enable(settings.s.ntp);
    memset(rt, 0, sizeof(rt));
    memset(found, 0, sizeof(found));
    for (int i = 0; i < APP_MAX_NODES; i++) {
        rt[i].adopted = nodes.nodes[i].used; /* stored nodes were adopted before */
    }
    changes = 0xFFFFFFFFu;
}

uint32_t app_take_changes(void)
{
    uint32_t c = changes;
    changes = 0;
    return c;
}

/* ---- accessors ---- */

app_node_cfg_t *app_node(int idx)
{
    if (idx < 0 || idx >= APP_MAX_NODES || !nodes.nodes[idx].used) {
        return NULL;
    }
    return &nodes.nodes[idx];
}

const app_node_rt_t *app_node_rt(int idx)
{
    return app_node(idx) ? &rt[idx] : NULL;
}

bool app_node_online(int idx)
{
    return app_node(idx) && rt[idx].have_status &&
           plat_millis() - rt[idx].last_seen_ms < APP_ONLINE_TIMEOUT_MS;
}

int app_node_count(void)
{
    int n = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        n += nodes.nodes[i].used ? 1 : 0;
    }
    return n;
}

const app_found_t *app_found(int i)
{
    if (i < 0 || i >= APP_MAX_FOUND || !found[i].used) {
        return NULL;
    }
    return &found[i];
}

app_settings_t *app_settings(void) { return &settings.s; }
void app_settings_save(void) { plat_save("settings", &settings, sizeof(settings)); }

static int find_node(const uint8_t mac[6])
{
    for (int i = 0; i < APP_MAX_NODES; i++) {
        if (nodes.nodes[i].used && memcmp(nodes.nodes[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- sending ---- */

static void send_pair(int idx)
{
    fd_msg_pair_t m;
    fd_hdr_init(&m.hdr, FD_MSG_PAIR, ++tx_seq);
    m.node_id = (uint8_t)(idx + 1);
    memcpy(m.name, nodes.nodes[idx].name, FD_NAME_LEN);
    plat_send(nodes.nodes[idx].mac, &m, sizeof(m));
}

static void send_schedule(int idx)
{
    const app_node_cfg_t *n = &nodes.nodes[idx];
    fd_msg_sched_set_t m;
    memset(&m, 0, sizeof(m));
    fd_hdr_init(&m.hdr, FD_MSG_SCHED_SET, ++tx_seq);
    m.rev = n->rev;
    m.limits = n->limits;
    m.count = n->count;
    memcpy(m.entries, n->entries, n->count * sizeof(fd_sched_entry_t));
    plat_send(n->mac, &m, (uint8_t)FD_SCHED_SET_LEN(n->count));
}

static void send_time(int idx)
{
    fd_msg_time_sync_t m;
    fd_hdr_init(&m.hdr, FD_MSG_TIME_SYNC, ++tx_seq);
    m.local_epoch = plat_local_epoch();
    plat_send(nodes.nodes[idx].mac, &m, sizeof(m));
}

static void send_ack(const uint8_t mac[6], const fd_hdr_t *req, uint16_t event_id)
{
    fd_msg_ack_t a;
    fd_hdr_init(&a.hdr, FD_MSG_ACK, req->seq);
    a.acked_type = req->type;
    a.code = FD_RES_OK;
    a.event_id = event_id;
    plat_send(mac, &a, sizeof(a));
}

static void send_feed(void)
{
    fd_msg_feed_now_t m;
    fd_hdr_init(&m.hdr, FD_MSG_FEED_NOW, feed.seq);
    m.on_ds = feed.on_ds;
    m.pulses = feed.pulses;
    plat_send(nodes.nodes[feed.node].mac, &m, sizeof(m));
    feed.last_ms = plat_millis();
    feed.tries++;
}

/*
 * Bring a node in line with what the HMI holds: schedule first, then clock.
 * Called on every STATUS, so a lost message is repaired by the next one.
 */
static void reconcile(int idx)
{
    app_node_rt_t *r = &rt[idx];
    uint32_t ms = plat_millis();
    if (r->last_push_ms != 0 && ms - r->last_push_ms < PUSH_PERIOD_MS) {
        return;
    }
    if (r->status.sched_rev != nodes.nodes[idx].rev) {
        send_schedule(idx);
        r->last_push_ms = ms ? ms : 1;
        return;
    }
    if (plat_time_valid()) {
        uint32_t ours = plat_local_epoch();
        uint32_t theirs = r->status.local_epoch;
        uint32_t diff = ours > theirs ? ours - theirs : theirs - ours;
        if (!(r->status.flags & FD_FLAG_TIME_VALID) || diff > TIME_TOLERANCE_S) {
            if (!(r->status.flags & FD_FLAG_RTC_MISSING)) {
                send_time(idx);
                r->last_push_ms = ms ? ms : 1;
            }
        }
    }
}

/* ---- log ---- */

/*
 * A weighed feed tells us what a pulse really drops. Use it to keep the
 * grams-per-pulse estimate honest (it is what portions are planned with),
 * and to notice a feed that gave next to nothing: a jam or an empty hopper.
 */
static void learn_from_weighing(int idx, const fd_msg_feed_event_t *ev)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n || ev->result != FD_RES_OK || ev->measured_mg == FD_NOT_MEASURED || ev->pulses_done == 0) {
        return;
    }
    uint8_t on_ds = (uint8_t)(ev->on_ms_total / ev->pulses_done / 100u);
    uint32_t per = ev->measured_mg / ev->pulses_done;
    uint32_t expected = app_mg(idx, on_ds, ev->pulses_done);

    bool low = expected >= 300u && ev->measured_mg * 4u < expected;
    if (low != rt[idx].low_output) {
        rt[idx].low_output = low;
        changes |= APP_CH_NODES;
    }
    if (low || per == 0 || per > 0xFFFFu) {
        return; /* do not learn from a jam */
    }
    if (!app_calibrated(idx)) {
        n->mg_per_pulse = (uint16_t)per; /* the scale calibrates the feeder by itself */
        n->cal_on_ds = on_ds;
    } else if (on_ds == n->cal_on_ds || (app_plan_by_rate(idx) && on_ds > 0)) {
        /* by rate, a run of any length says what a calibrated-length pulse drops */
        uint32_t at_cal = on_ds == n->cal_on_ds ? per : per * n->cal_on_ds / on_ds;
        if (at_cal == 0 || at_cal > 0xFFFFu) {
            return;
        }
        n->mg_per_pulse = (uint16_t)((3u * n->mg_per_pulse + at_cal + 2u) / 4u); /* move a quarter of the way */
    } else {
        return;
    }
    save_nodes();
    resync_targets(idx);
    changes |= APP_CH_NODES | APP_CH_SCHEDULE;
}

/* One CSV line per event on the SD card, which keeps the full history. */
static void sd_log(const app_log_entry_t *e)
{
    char name[2 * FD_NAME_LEN + 3], line[176];
    const app_node_cfg_t *n = app_node(e->node);
    /* CSV quoting: wrap in quotes and double any quote inside. */
    size_t k = 0;
    name[k++] = '"';
    for (const char *p = n ? n->name : "?"; *p && k < sizeof(name) - 3; p++) {
        if (*p == '"') {
            name[k++] = '"';
        }
        name[k++] = *p;
    }
    name[k++] = '"';
    name[k] = '\0';

    fd_civil_t c;
    fd_civil_from_epoch(e->epoch, &c);
    char source[16];
    if (e->source == FD_SOURCE_MANUAL) {
        snprintf(source, sizeof(source), "manual");
    } else {
        snprintf(source, sizeof(source), "schedule %u", e->source + 1);
    }
    /* Weighed grams when the feeder has a scale; otherwise the estimate,
     * which exists only for the calibrated pulse length. */
    char grams[12] = "";
    const char *kind = "";
    uint32_t mg = 0;
    if (e->measured_mg != FD_NOT_MEASURED) {
        mg = e->measured_mg;
        kind = "weighed";
    } else if (e->pulses) {
        mg = app_mg(e->node, (uint8_t)(e->on_ms / e->pulses / 100u), e->pulses);
        kind = mg ? "estimated" : "";
    }
    if (kind[0]) {
        uint32_t tenths = (mg + 50u) / 100u; /* rounded, as on the screen */
        snprintf(grams, sizeof(grams), "%u.%u", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    }
    /* What was asked for, if the request was in grams. */
    uint32_t asked = 0;
    if (e->source == FD_SOURCE_MANUAL) {
        asked = rt[e->node].manual_asked_mg;
    } else if (n && e->source < n->count) {
        asked = targets.t[e->node][e->source];
    }
    char asked_txt[12] = "";
    if (asked) {
        uint32_t tenths = (asked + 50u) / 100u;
        snprintf(asked_txt, sizeof(asked_txt), "%u.%u", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    }
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u,%s,%s,%s,%u.%u,%u,%s,%s,%s", c.year, c.month, c.day,
             c.hour, c.minute, c.second, name, source, e->result == FD_RES_OK ? "fed" : app_result_text(e->result),
             e->on_ms / 1000, e->on_ms % 1000 / 100, e->pulses, grams, kind, asked_txt);
    plat_sd_append(APP_SD_LOG_FILE, APP_SD_LOG_HEADER, line);
}

static void log_add(int node, const fd_msg_feed_event_t *ev)
{
    app_log_entry_t *e = &logbuf.e[logbuf.head];
    e->epoch = ev->local_epoch;
    e->node = (uint8_t)node;
    e->source = ev->source;
    e->result = ev->result;
    e->pulses = ev->pulses_done;
    e->on_ms = ev->on_ms_total;
    e->measured_mg = ev->measured_mg;
    learn_from_weighing(node, ev);
    logbuf.head = (uint16_t)((logbuf.head + 1) % APP_LOG_SIZE);
    if (logbuf.count < APP_LOG_SIZE) {
        logbuf.count++;
    }
    save_log();
    sd_log(e);
    changes |= APP_CH_LOG;
}

int app_log_count(void) { return logbuf.count; }

const app_log_entry_t *app_log_get(int i)
{
    if (i < 0 || i >= logbuf.count) {
        return NULL;
    }
    int pos = ((int)logbuf.head - 1 - i + 2 * APP_LOG_SIZE) % APP_LOG_SIZE;
    return &logbuf.e[pos];
}

void app_log_clear(void)
{
    logbuf.head = 0;
    logbuf.count = 0;
    save_log();
    changes |= APP_CH_LOG;
}

/* ---- feeder firmware update ---- */

static void ota_finish(app_ota_state_t state, const char *text)
{
    plat_node_ota_end();
    ota.state = state;
    snprintf(ota.text, sizeof(ota.text), "%s", text);
    changes |= APP_CH_OTA | APP_CH_NODES;
}

const char *app_node_ota_start(int idx)
{
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return "No such feeder";
    }
    if (ota.state == APP_OTA_RUNNING) {
        return "Another update is running";
    }
    if (plat_node_fw_size() == 0) {
        return "Upload the feeder firmware first";
    }
    if (!app_node_online(idx)) {
        return "The feeder is offline";
    }
    memset(&ota.msg, 0, sizeof(ota.msg));
    uint16_t port = 0;
    if (!plat_node_ota_begin(ota.msg.ssid, ota.msg.pass, ota.msg.ip, &port)) {
        return "Could not open the update access point";
    }
    ota.msg.port = port;
    fd_hdr_init(&ota.msg.hdr, FD_MSG_OTA_START, ++tx_seq);
    ota.msg.size = plat_node_fw_size();
    ota.state = APP_OTA_RUNNING;
    ota.node = idx;
    ota.accepted = false;
    ota.tries = 1;
    ota.start_ms = ota.last_ms = plat_millis();
    ota.text[0] = '\0';
    plat_send(n->mac, &ota.msg, sizeof(ota.msg));
    changes |= APP_CH_OTA;
    return NULL;
}

/* The feeder reboots into the new firmware and reports in again. */
static void ota_on_status(int idx, const fd_msg_status_t *st)
{
    if (ota.state != APP_OTA_RUNNING || ota.node != idx || !ota.accepted) {
        return;
    }
    if (st->uptime_s * 1000u < plat_millis() - ota.start_ms) {
        if (plat_node_ota_served()) {
            char text[64];
            snprintf(text, sizeof(text), "Now running firmware %u.%u", st->fw_major, st->fw_minor);
            ota_finish(APP_OTA_DONE, text);
        } else {
            /* It restarted without taking the whole file. */
            ota_finish(APP_OTA_FAILED, "The feeder could not download the firmware");
        }
    }
}

static void ota_tick(uint32_t ms)
{
    if (ota.state != APP_OTA_RUNNING) {
        return;
    }
    const app_node_cfg_t *n = app_node(ota.node);
    if (!n) {
        ota_finish(APP_OTA_FAILED, "The feeder was removed");
    } else if (ms - ota.start_ms > OTA_TIMEOUT_MS) {
        ota_finish(APP_OTA_FAILED, ota.accepted ? "The feeder did not come back"
                                                : "The feeder did not answer");
    } else if (!ota.accepted && ms - ota.last_ms >= OTA_RETRY_MS && ota.tries < OTA_START_TRIES) {
        ota.last_ms = ms;
        ota.tries++;
        plat_send(n->mac, &ota.msg, sizeof(ota.msg)); /* same seq */
    }
}

app_ota_state_t app_ota_state(void) { return ota.state; }
int app_ota_node(void) { return ota.node; }
const char *app_ota_message(void) { return ota.text; }

/* ---- receive ---- */

static void on_hello(const uint8_t mac[6])
{
    int idx = find_node(mac);
    if (idx >= 0) {
        /* One of ours is asking to be adopted (new, or it was reset). */
        send_pair(idx);
        return;
    }
    int slot = -1;
    for (int i = 0; i < APP_MAX_FOUND; i++) {
        if (found[i].used && memcmp(found[i].mac, mac, 6) == 0) {
            found[i].seen_ms = plat_millis();
            return;
        }
        if (!found[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        found[slot].used = true;
        memcpy(found[slot].mac, mac, 6);
        found[slot].seen_ms = plat_millis();
        changes |= APP_CH_FOUND;
    }
}

void app_on_packet(const uint8_t mac[6], const uint8_t *data, uint8_t len)
{
    uint8_t type = fd_proto_validate(data, len);
    if (type == 0) {
        return;
    }
    const fd_hdr_t *hdr = (const fd_hdr_t *)data;

    if (type == FD_MSG_HELLO) {
        on_hello(mac);
        return;
    }

    int idx = find_node(mac);
    if (idx < 0) {
        return; /* not one of our nodes */
    }
    app_node_rt_t *r = &rt[idx];

    switch (type) {
    case FD_MSG_STATUS: {
        memcpy(&r->status, data, sizeof(r->status));
        ota_on_status(idx, &r->status);
        r->have_status = true;
        r->adopted = true;
        r->last_seen_ms = plat_millis();
        send_ack(mac, hdr, 0);
        reconcile(idx);
        changes |= APP_CH_NODES;
        break;
    }
    case FD_MSG_FEED_EVENT: {
        const fd_msg_feed_event_t *ev = (const fd_msg_feed_event_t *)data;
        send_ack(mac, hdr, ev->event_id);
        /* The node resends until it sees our ACK; log each event once. A
         * feeder numbers its events from 1 again after every restart, so
         * the number alone does not identify a repeat: the time must match
         * as well. */
        if (!(r->have_event && r->last_event_id == ev->event_id && r->last_event_epoch == ev->local_epoch)) {
            r->have_event = true;
            r->last_event_id = ev->event_id;
            r->last_event_epoch = ev->local_epoch;
            log_add(idx, ev);
        }
        break;
    }
    case FD_MSG_ACK: {
        const fd_msg_ack_t *a = (const fd_msg_ack_t *)data;
        if (a->acked_type == FD_MSG_PAIR) {
            r->adopted = true;
            changes |= APP_CH_NODES;
        } else if (a->acked_type == FD_MSG_OTA_START && ota.state == APP_OTA_RUNNING &&
                   ota.node == idx && hdr->seq == ota.msg.hdr.seq) {
            if (a->code == FD_RES_OK) {
                ota.accepted = true;
            } else {
                ota_finish(APP_OTA_FAILED, a->code == FD_RES_BUSY ? "The feeder is busy feeding"
                                                                   : "The feeder refused the update");
            }
        } else if (a->acked_type == FD_MSG_SCALE_SET && scale.active && scale.node == idx &&
                   hdr->seq == scale.msg.hdr.seq) {
            scale.active = false;
            scale.result = a->code;
            changes |= APP_CH_SCALE;
            /* Ask for a fresh reading so the screen shows the effect. */
            fd_msg_simple_t q;
            fd_hdr_init(&q.hdr, FD_MSG_STATUS_REQ, ++tx_seq);
            plat_send(nodes.nodes[idx].mac, &q, sizeof(q));
        } else if (a->acked_type == FD_MSG_FEED_NOW && feed.active && feed.node == idx &&
                   hdr->seq == feed.seq) {
            feed.active = false;
            feed.result = a->code;
            changes |= APP_CH_FEED;
        }
        break;
    }
    default:
        break;
    }
}

/* ---- periodic ---- */

void app_tick(void)
{
    uint32_t ms = plat_millis();

    for (int i = 0; i < APP_MAX_FOUND; i++) {
        if (found[i].used && ms - found[i].seen_ms > FOUND_TTL_MS) {
            found[i].used = false;
            changes |= APP_CH_FOUND;
        }
    }

    if (feed.active && ms - feed.last_ms >= FEED_RETRY_MS) {
        if (feed.tries >= FEED_TRIES) {
            feed.active = false;
            feed.result = APP_FEED_NO_REPLY;
            changes |= APP_CH_FEED;
        } else {
            send_feed(); /* same seq: the node will not feed twice */
        }
    }

    if (scale.active && ms - scale.last_ms >= FEED_RETRY_MS) {
        if (scale.tries >= FEED_TRIES || !app_node(scale.node)) {
            scale.active = false;
            scale.result = APP_FEED_NO_REPLY;
            changes |= APP_CH_SCALE;
        } else {
            /* Same seq: zeroing or setting the span twice is harmless. */
            plat_send(nodes.nodes[scale.node].mac, &scale.msg, sizeof(scale.msg));
            scale.last_ms = ms;
            scale.tries++;
        }
    }

    ota_tick(ms);

    /* Online/offline transitions need a redraw even with no traffic. */
    static uint8_t online_mask;
    uint8_t mask = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        if (app_node_online(i)) {
            mask |= (uint8_t)(1u << i);
        }
    }
    if (mask != online_mask) {
        online_mask = mask;
        changes |= APP_CH_NODES;
    }
}

/* ---- node management ---- */

int app_adopt(const uint8_t mac[6])
{
    int idx = find_node(mac);
    if (idx < 0) {
        for (int i = 0; i < APP_MAX_NODES && idx < 0; i++) {
            if (!nodes.nodes[i].used) {
                idx = i;
            }
        }
        if (idx < 0) {
            return -1;
        }
        app_node_cfg_t *n = &nodes.nodes[idx];
        memset(n, 0, sizeof(*n));
        n->used = 1;
        memcpy(n->mac, mac, 6);
        snprintf(n->name, sizeof(n->name), "Tank %d", idx + 1);
        n->rev = 1;
        n->limits.gap_ds = 5;
        n->limits.cooldown_s = 600;
        n->limits.daily_max_on_s = 30;
        memset(&rt[idx], 0, sizeof(rt[idx]));
        memset(targets.t[idx], 0, sizeof(targets.t[idx]));
        modes.by_rate[idx] = 0;
        save_modes();
        save_nodes();
    }
    for (int i = 0; i < APP_MAX_FOUND; i++) {
        if (found[i].used && memcmp(found[i].mac, mac, 6) == 0) {
            found[i].used = false;
        }
    }
    /* The node hops channels; its next HELLO triggers another PAIR. */
    send_pair(idx);
    changes |= APP_CH_NODES | APP_CH_FOUND | APP_CH_SCHEDULE;
    return idx;
}

void app_remove_node(int idx)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    fd_msg_simple_t m;
    fd_hdr_init(&m.hdr, FD_MSG_UNPAIR, ++tx_seq);
    plat_send(n->mac, &m, sizeof(m));
    memset(n, 0, sizeof(*n));
    memset(&rt[idx], 0, sizeof(rt[idx]));
    memset(targets.t[idx], 0, sizeof(targets.t[idx]));
    modes.by_rate[idx] = 0;
    save_modes();
    if (feed.active && feed.node == idx) {
        feed.active = false;
    }
    save_nodes();
    changes |= APP_CH_NODES | APP_CH_SCHEDULE;
}

void app_rename_node(int idx, const char *name)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n || !name || !name[0]) {
        return;
    }
    memset(n->name, 0, sizeof(n->name));
    strncpy(n->name, name, sizeof(n->name) - 1);
    save_nodes();
    send_pair(idx); /* carries the new name; harmless if the node ignores it */
    changes |= APP_CH_NODES | APP_CH_SCHEDULE;
}

void app_schedule_changed(int idx)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    n->rev++;
    if (n->rev == 0) {
        n->rev = 1;
    }
    save_nodes();
    send_schedule(idx);
    rt[idx].last_push_ms = plat_millis() ? plat_millis() : 1;
    changes |= APP_CH_SCHEDULE | APP_CH_NODES;
}

uint32_t app_next_feed(int idx, uint32_t now_epoch)
{
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return 0;
    }
    uint32_t day0 = now_epoch - now_epoch % 86400u;
    uint32_t best = 0;
    for (uint8_t i = 0; i < n->count; i++) {
        const fd_sched_entry_t *e = &n->entries[i];
        if (!(e->dow_mask & FD_ENTRY_ENABLED) || !fd_sched_entry_valid(e)) {
            continue;
        }
        for (uint32_t d = 0; d < 8; d++) {
            uint32_t t = day0 + d * 86400u + e->hour * 3600u + e->minute * 60u;
            fd_tod_t tod;
            fd_time_split(t, &tod);
            /* Strictly after the current minute. */
            if (t / 60u > now_epoch / 60u && (e->dow_mask & (1u << tod.dow))) {
                if (best == 0 || t < best) {
                    best = t;
                }
                break;
            }
        }
    }
    return best;
}

/* ---- manual feed ---- */

/* ---- exact grams ---- */

bool app_plan_by_rate(int idx)
{
    return idx >= 0 && idx < APP_MAX_NODES && modes.by_rate[idx] && app_calibrated(idx);
}

void app_set_plan_by_rate(int idx, bool on)
{
    if (!app_node(idx)) {
        return;
    }
    modes.by_rate[idx] = on ? 1 : 0;
    save_modes();
    resync_targets(idx);
    changes |= APP_CH_NODES | APP_CH_SCHEDULE;
}

uint32_t app_rate_mg_per_s(int idx)
{
    const app_node_cfg_t *n = app_node(idx);
    return app_calibrated(idx) ? (uint32_t)n->mg_per_pulse * 10u / n->cal_on_ds : 0;
}

/* By pulses: the calibrated length, the nearest whole number of pulses.
 * By rate: the relay time that drops the amount (0.1 s steps), split into
 * equal runs of at most the hard limit when it is longer than that. */
void app_plan(int idx, uint32_t target_mg, uint8_t *on_ds, uint8_t *pulses)
{
    const app_node_cfg_t *n = app_node(idx);
    *on_ds = app_feed_on_ds(idx);
    *pulses = 1;
    if (!app_calibrated(idx)) {
        return;
    }
    uint32_t mg = n->mg_per_pulse;
    if (!app_plan_by_rate(idx)) {
        uint32_t p = (target_mg + mg / 2u) / mg;
        *on_ds = n->cal_on_ds;
        *pulses = (uint8_t)(p < 1 ? 1 : p > FD_HARD_MAX_PULSES ? FD_HARD_MAX_PULSES : p);
        return;
    }
    /* mg per 0.1 s = mg_per_pulse / cal_on_ds */
    uint32_t total = (target_mg * n->cal_on_ds + mg / 2u) / mg;
    if (total < 1) {
        total = 1;
    }
    if (total > (uint32_t)FD_HARD_MAX_ON_DS * FD_HARD_MAX_PULSES) {
        total = (uint32_t)FD_HARD_MAX_ON_DS * FD_HARD_MAX_PULSES;
    }
    uint32_t p = (total + FD_HARD_MAX_ON_DS - 1u) / FD_HARD_MAX_ON_DS;
    *pulses = (uint8_t)p;
    *on_ds = (uint8_t)((total + p / 2u) / p);
}

uint8_t app_pulses_for(int idx, uint32_t target_mg)
{
    uint8_t on, p;
    if (!app_calibrated(idx)) {
        return 0;
    }
    app_plan(idx, target_mg, &on, &p);
    return p;
}

uint32_t app_max_target_mg(int idx)
{
    const app_node_cfg_t *n = app_node(idx);
    if (!app_calibrated(idx)) {
        return 0;
    }
    return app_plan_by_rate(idx) ? (uint32_t)n->mg_per_pulse * FD_HARD_MAX_ON_DS * FD_HARD_MAX_PULSES / n->cal_on_ds
                                 : (uint32_t)n->mg_per_pulse * FD_HARD_MAX_PULSES;
}

uint16_t app_entry_target(int idx, int entry)
{
    const app_node_cfg_t *n = app_node(idx);
    return n && entry >= 0 && entry < n->count ? targets.t[idx][entry] : 0;
}

/* After the grams-per-pulse changed, re-plan every feeding time that was
 * set in grams, and send the schedule if any pulse count moved. */
static void resync_targets(int idx)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n || !app_calibrated(idx)) {
        return;
    }
    bool moved = false;
    for (uint8_t i = 0; i < n->count; i++) {
        if (targets.t[idx][i] == 0) {
            continue;
        }
        uint8_t on, p;
        app_plan(idx, targets.t[idx][i], &on, &p);
        if (n->entries[i].pulses != p || n->entries[i].on_ds != on) {
            n->entries[i].pulses = p;
            n->entries[i].on_ds = on;
            moved = true;
        }
    }
    if (moved) {
        app_schedule_changed(idx);
    }
}

static int entry_minutes(const fd_sched_entry_t *e) { return e->hour * 60 + e->minute; }

bool app_entry_save(int idx, int entry, const fd_sched_entry_t *e, uint16_t target_mg)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return false;
    }
    int at = entry;
    if (entry < 0 || entry >= n->count) {
        if (n->count >= FD_MAX_ENTRIES) {
            return false;
        }
        at = n->count++;
    }
    n->entries[at] = *e;
    targets.t[idx][at] = 0;
    if (target_mg && app_calibrated(idx)) {
        targets.t[idx][at] = target_mg;
        app_plan(idx, target_mg, &n->entries[at].on_ds, &n->entries[at].pulses);
    }
    /* Keep the list in time order (stable insertion sort), targets moving along. */
    for (int i = 1; i < n->count; i++) {
        fd_sched_entry_t key = n->entries[i];
        uint16_t key_t = targets.t[idx][i];
        int j = i - 1;
        while (j >= 0 && entry_minutes(&n->entries[j]) > entry_minutes(&key)) {
            n->entries[j + 1] = n->entries[j];
            targets.t[idx][j + 1] = targets.t[idx][j];
            j--;
        }
        n->entries[j + 1] = key;
        targets.t[idx][j + 1] = key_t;
    }
    app_schedule_changed(idx);
    return true;
}

void app_entry_delete(int idx, int entry)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n || entry < 0 || entry >= n->count) {
        return;
    }
    for (int i = entry; i + 1 < n->count; i++) {
        n->entries[i] = n->entries[i + 1];
        targets.t[idx][i] = targets.t[idx][i + 1];
    }
    n->count--;
    targets.t[idx][n->count] = 0;
    memset(&n->entries[n->count], 0, sizeof(n->entries[0]));
    app_schedule_changed(idx);
}

bool app_feed_grams(int idx, uint32_t target_mg)
{
    uint8_t on_ds, pulses;
    if (!app_calibrated(idx)) {
        return false;
    }
    app_plan(idx, target_mg, &on_ds, &pulses);
    if (!app_feed_custom(idx, on_ds, pulses)) {
        return false;
    }
    rt[idx].manual_asked_mg = target_mg > 0xFFFFu ? 0xFFFFu : (uint16_t)target_mg;
    return true;
}

bool app_feed_custom(int idx, uint8_t on_ds, uint8_t pulses)
{
    if (feed.active || !app_node(idx)) {
        return false;
    }
    feed.active = true;
    feed.node = idx;
    feed.last_node = idx;
    feed.seq = ++tx_seq;
    feed.tries = 0;
    feed.on_ds = on_ds;
    feed.pulses = pulses;
    rt[idx].manual_asked_mg = 0;
    send_feed();
    return true;
}

bool app_feed_now(int idx)
{
    return app_feed_custom(idx, app_feed_on_ds(idx), settings.s.feed_pulses);
}

/* ---- scale ---- */

bool app_scale_found(int idx)
{
    return app_node_online(idx) && (rt[idx].status.flags & (FD_FLAG_SCALE_FOUND | FD_FLAG_SCALE));
}

bool app_has_scale(int idx)
{
    return app_node(idx) && rt[idx].have_status && (rt[idx].status.flags & FD_FLAG_SCALE);
}

int32_t app_hopper_mg(int idx) { return app_has_scale(idx) ? rt[idx].status.hopper_mg : 0; }

bool app_scale_set(int idx, uint8_t op, uint32_t known_mg)
{
    if (scale.active || !app_node(idx)) {
        return false;
    }
    fd_hdr_init(&scale.msg.hdr, FD_MSG_SCALE_SET, ++tx_seq);
    scale.msg.op = op;
    scale.msg.known_mg = known_mg;
    scale.active = true;
    scale.node = scale.last_node = idx;
    scale.tries = 1;
    scale.last_ms = plat_millis();
    plat_send(nodes.nodes[idx].mac, &scale.msg, sizeof(scale.msg));
    return true;
}

int app_scale_last_node(void) { return scale.last_node; }
uint8_t app_scale_last_result(void) { return scale.result; }

/* ---- calibration ---- */

void app_set_calibration(int idx, uint8_t on_ds, uint32_t total_mg)
{
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    uint32_t per = (total_mg + APP_CAL_PULSES / 2) / APP_CAL_PULSES;
    n->mg_per_pulse = per > 0xFFFFu ? 0xFFFFu : (uint16_t)per;
    n->cal_on_ds = n->mg_per_pulse ? on_ds : 0;
    save_nodes();
    resync_targets(idx);
    changes |= APP_CH_NODES | APP_CH_SCHEDULE | APP_CH_LOG;
}

bool app_calibrated(int idx)
{
    const app_node_cfg_t *n = app_node(idx);
    return n && n->mg_per_pulse != 0 && n->cal_on_ds != 0;
}

uint8_t app_feed_on_ds(int idx)
{
    return app_calibrated(idx) ? app_node(idx)->cal_on_ds : settings.s.feed_on_ds;
}

/* By pulses the weight is only known for the calibrated length; by rate it is
 * known for any length (flow rate times relay time). */
uint32_t app_mg(int idx, uint8_t on_ds, uint32_t pulses)
{
    if (!app_calibrated(idx)) {
        return 0;
    }
    const app_node_cfg_t *n = app_node(idx);
    if (app_plan_by_rate(idx)) {
        return ((uint32_t)n->mg_per_pulse * on_ds * pulses + n->cal_on_ds / 2u) / n->cal_on_ds;
    }
    if (on_ds != n->cal_on_ds) {
        return 0;
    }
    return (uint32_t)n->mg_per_pulse * pulses;
}

bool app_feed_busy(void) { return feed.active; }
int app_feed_last_node(void) { return feed.last_node; }
uint8_t app_feed_last_result(void) { return feed.result; }

const char *app_result_text(uint8_t result)
{
    switch (result) {
    case FD_RES_OK:           return "Fed";
    case FD_RES_COOLDOWN:     return "Skipped: cooldown";
    case FD_RES_DAILY_CAP:    return "Skipped: daily limit";
    case FD_RES_BUSY:         return "Skipped: busy";
    case FD_RES_BAD_REQUEST:  return "Rejected";
    case FD_RES_ABORTED:      return "Aborted by failsafe";
    case FD_RES_TIME_INVALID: return "Skipped: clock not set";
    case FD_RES_STORAGE:      return "Not fed: feeder storage error";
    case APP_FEED_NO_REPLY:   return "No reply from feeder";
    default:                  return "Unknown";
    }
}
