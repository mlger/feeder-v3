/*
 * The simulated world behind the HMI application: a virtual clock, storage
 * that forgets, a fake Wi-Fi router and NTP server, and two feeder nodes
 * that speak the real protocol and use the same safety logic as the node
 * firmware. Shared by the scripted test (sim_main.c) and the browser demo
 * (web_main.c).
 */
#include "sim_world.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "plat.h"

/* ------------------------------------------------------- virtual time */

uint32_t now_ms = 1000;
static uint32_t utc_base;   /* UTC epoch at now_ms == 0; 0 = clock not set */
static int32_t  utc_offset; /* seconds */

uint32_t plat_millis(void) { return now_ms; }
bool plat_time_valid(void) { return utc_base != 0; }
uint32_t plat_local_epoch(void)
{
    return utc_base ? (uint32_t)((int32_t)(utc_base + now_ms / 1000) + utc_offset) : now_ms / 1000;
}
void plat_set_local_epoch(uint32_t e) { utc_base = (uint32_t)((int32_t)e - utc_offset) - now_ms / 1000; }
void plat_set_utc_offset(int16_t minutes) { utc_offset = minutes * 60; }

/* ------------------------------------------------- fake Wi-Fi and NTP */

/* "Real" UTC, as an NTP server would report it: Mon 2026-11-09 00:10:00
 * at the moment the simulation starts. */

static plat_wifi_state_t wifi_state = PLAT_WIFI_IDLE;
static char wifi_ssid[33];
static uint32_t wifi_since_ms, scan_since_ms;
bool ntp_on, ntp_synced, updates_allowed;
char last_status[800];

plat_wifi_state_t plat_wifi_state(void) { return wifi_state; }
void plat_wifi_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    snprintf(ssid, ssid_len, "%s", wifi_ssid);
    snprintf(ip, ip_len, "%s", wifi_state == PLAT_WIFI_CONNECTED ? "192.168.1.50" : "");
}
void plat_wifi_connect(const char *ssid, const char *pass)
{
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
    /* The fake router only knows one password. */
    wifi_state = strcmp(pass, SIM_WIFI_PASSWORD) == 0 ? PLAT_WIFI_CONNECTING : PLAT_WIFI_FAILED;
    wifi_since_ms = now_ms;
}
void plat_wifi_forget(void) { wifi_ssid[0] = '\0'; wifi_state = PLAT_WIFI_IDLE; }
void plat_wifi_scan(void) { scan_since_ms = now_ms; }
int plat_wifi_scan_get(plat_ap_t *out, int max)
{
    static const plat_ap_t found_aps[] = {
        { "HomeNetwork", -48, true }, { "Neighbour-5G", -71, true }, { "CoffeeShop Guest", -80, false },
    };
    if (now_ms - scan_since_ms < 1500) {
        return -1;
    }
    int n = max < 3 ? max : 3;
    memcpy(out, found_aps, sizeof(plat_ap_t) * (size_t)n);
    return n;
}
void plat_ntp_enable(bool on) { ntp_on = on; if (!on) ntp_synced = false; }
bool plat_ntp_synced(void) { return ntp_synced; }
void plat_updates_allow(bool on) { updates_allowed = on; }
void plat_publish_status(const char *json) { snprintf(last_status, sizeof(last_status), "%s", json); }

static void net_tick(void)
{
    if (wifi_state == PLAT_WIFI_CONNECTING && now_ms - wifi_since_ms > 2000) {
        wifi_state = PLAT_WIFI_CONNECTED;
        wifi_since_ms = now_ms;
    }
    if (ntp_on && !ntp_synced && wifi_state == PLAT_WIFI_CONNECTED && now_ms - wifi_since_ms > 1000) {
        utc_base = TRUE_UTC_AT_START - 1;
        ntp_synced = true;
    }
}

/* Feeder firmware "uploaded on the web page" and the update access point. */
uint32_t node_fw_size;
int node_ota_request = -1;
bool ota_ap_open, ota_served;
uint32_t plat_node_fw_size(void) { return node_fw_size; }
int plat_take_node_ota_request(void) { int r = node_ota_request; node_ota_request = -1; return r; }
bool plat_node_ota_begin(char *ssid, char *pass, uint8_t ip[4], uint16_t *port)
{
    strcpy(ssid, "AIO-Feeder-OTA");
    strcpy(pass, "sim-password");
    ip[0] = 192; ip[1] = 168; ip[2] = 4; ip[3] = 1;
    *port = 80;
    ota_ap_open = true;
    ota_served = false;
    return true;
}
void plat_node_ota_end(void) { ota_ap_open = false; }
bool plat_node_ota_served(void) { return ota_served; }

/* ------------------------------------------------------------ storage */

bool plat_load(const char *key, void *buf, size_t len)
{
    (void)key; (void)buf; (void)len;
    return false; /* always start empty */
}

int saves;
void plat_save(const char *key, const void *buf, size_t len)
{
    (void)key; (void)buf; (void)len;
    saves++;
}

bool backlight = true;
void plat_backlight(bool on) { backlight = on; }

/* -------------------------------------------------------- fake feeders */



fake_node_t fnodes[2] = {
    { .mac = { 0x5C, 0xCF, 0x7F, 0x10, 0x20, 0x01 }, .powered = true, .fw_minor = 1 },
    { .mac = { 0x5C, 0xCF, 0x7F, 0x10, 0x20, 0x02 }, .powered = true, .fw_minor = 1 },
};

/* Frames travelling node -> HMI, delivered from the main loop. */
typedef struct { uint8_t mac[6]; uint8_t len; uint8_t data[250]; } frame_t;
static frame_t inbox[32];
static int inbox_n;

static void to_hmi(const fake_node_t *n, const void *msg, uint8_t len)
{
    if (inbox_n < 32) {
        memcpy(inbox[inbox_n].mac, n->mac, 6);
        memcpy(inbox[inbox_n].data, msg, len);
        inbox[inbox_n].len = len;
        inbox_n++;
    }
}

static uint32_t fnode_epoch(const fake_node_t *n)
{
    return (uint32_t)((int32_t)plat_local_epoch() + n->clock_offset);
}

static void fnode_ack(fake_node_t *n, const fd_hdr_t *req, uint8_t code)
{
    fd_msg_ack_t a;
    fd_hdr_init(&a.hdr, FD_MSG_ACK, req->seq);
    a.acked_type = req->type;
    a.code = code;
    a.event_id = 0;
    to_hmi(n, &a, sizeof(a));
}

static void fnode_event(fake_node_t *n, uint8_t source, uint8_t result, uint8_t pulses, uint16_t on_ms)
{
    fd_hdr_init(&n->ev.hdr, FD_MSG_FEED_EVENT, ++n->seq);
    n->ev.node_id = n->node_id;
    n->ev.event_id = ++n->next_event;
    n->ev.local_epoch = fnode_epoch(n);
    n->ev.source = source;
    n->ev.result = result;
    n->ev.pulses_done = pulses;
    n->ev.on_ms_total = on_ms;
    n->ev_pending = true;
    n->ev_sent_ms = 0;
}

static uint8_t fnode_feed(fake_node_t *n, uint8_t source, uint8_t on_ds, uint8_t pulses)
{
    fd_result_t r = fd_guard_check(&n->guard, &n->limits, fnode_epoch(n), &on_ds, &pulses);
    if (r == FD_RES_OK) {
        uint32_t on_ms = (uint32_t)on_ds * 100u * pulses;
        fd_guard_commit(&n->guard, fnode_epoch(n), on_ms);
        n->feeds_run++;
        fnode_event(n, source, FD_RES_OK, pulses, (uint16_t)on_ms);
    } else if (source != FD_SOURCE_MANUAL) {
        fnode_event(n, source, (uint8_t)r, 0, 0);
    }
    return (uint8_t)r;
}

static void fnode_rx(fake_node_t *n, const uint8_t *data, uint8_t len)
{
    uint8_t type = fd_proto_validate(data, len);
    const fd_hdr_t *hdr = (const fd_hdr_t *)data;
    if (!n->powered || type == 0) {
        return;
    }
    switch (type) {
    case FD_MSG_PAIR:
        if (!n->paired) {
            n->paired = true;
            n->node_id = ((const fd_msg_pair_t *)data)->node_id;
            n->limits = (fd_limits_t){ 5, 600, 30 };
            fnode_ack(n, hdr, FD_RES_OK);
            n->last_status_ms = 0;
        }
        break;
    case FD_MSG_UNPAIR:
        n->paired = false;
        break;
    case FD_MSG_TIME_SYNC:
        n->clock_offset = (int32_t)((const fd_msg_time_sync_t *)data)->local_epoch -
                          (int32_t)plat_local_epoch();
        n->time_valid = true;
        fnode_ack(n, hdr, FD_RES_OK);
        break;
    case FD_MSG_SCHED_SET: {
        const fd_msg_sched_set_t *m = (const fd_msg_sched_set_t *)data;
        n->rev = m->rev;
        n->limits = m->limits;
        n->count = m->count;
        memcpy(n->entries, m->entries, m->count * sizeof(fd_sched_entry_t));
        fnode_ack(n, hdr, FD_RES_OK);
        break;
    }
    case FD_MSG_FEED_NOW: {
        const fd_msg_feed_now_t *m = (const fd_msg_feed_now_t *)data;
        if (!(n->have_feed_seq && n->last_feed_seq == hdr->seq)) {
            n->have_feed_seq = true;
            n->last_feed_seq = hdr->seq;
            n->last_feed_code = fnode_feed(n, FD_SOURCE_MANUAL, m->on_ds, m->pulses);
        }
        fnode_ack(n, hdr, n->last_feed_code);
        break;
    }
    case FD_MSG_OTA_START:
        fnode_ack(n, hdr, FD_RES_OK);
        if (!n->ota_until_ms) {
            n->ota_until_ms = now_ms + 8000; /* join, download, flash */
        }
        break;
    case FD_MSG_ACK: {
        const fd_msg_ack_t *a = (const fd_msg_ack_t *)data;
        if (a->acked_type == FD_MSG_FEED_EVENT && n->ev_pending && a->event_id == n->ev.event_id) {
            n->ev_pending = false;
        }
        break;
    }
    default:
        break;
    }
}

static void fnode_tick(fake_node_t *n)
{
    if (!n->powered) {
        return;
    }
    if (n->ota_until_ms) {
        if (now_ms < n->ota_until_ms) {
            return; /* off the air while it downloads */
        }
        /* Reboot: into the new firmware if the download worked. */
        n->ota_until_ms = 0;
        n->boot_ms = now_ms;
        n->last_status_ms = 0;
        if (ota_ap_open && !n->ota_broken) {
            ota_served = true;
            n->fw_minor++;
        }
    }
    if (!n->paired) {
        if (now_ms - n->last_hello_ms >= 400) {
            n->last_hello_ms = now_ms;
            fd_msg_hello_t h;
            fd_hdr_init(&h.hdr, FD_MSG_HELLO, ++n->seq);
            h.fw_major = 0; h.fw_minor = n->fw_minor; h.flags = FD_FLAG_PAIRING;
            to_hmi(n, &h, sizeof(h));
        }
        return;
    }
    if (n->time_valid) {
        uint32_t e = fnode_epoch(n), minute = e / 60;
        if (minute > n->last_sched_minute) {
            uint16_t due = fd_sched_due(n->entries, n->count, e);
            if (due) {
                n->last_sched_minute = minute;
                for (uint8_t i = 0; i < n->count; i++) {
                    if (due & (1u << i)) {
                        fnode_feed(n, i, n->entries[i].on_ds, n->entries[i].pulses);
                        break;
                    }
                }
            }
        }
    }
    if (n->last_status_ms == 0 || now_ms - n->last_status_ms >= 10000) {
        n->last_status_ms = now_ms;
        fd_guard_roll_day(&n->guard, fnode_epoch(n));
        fd_msg_status_t s;
        fd_hdr_init(&s.hdr, FD_MSG_STATUS, ++n->seq);
        s.node_id = n->node_id;
        s.flags = n->time_valid ? FD_FLAG_TIME_VALID : 0;
        s.fw_major = 0; s.fw_minor = n->fw_minor;
        s.sched_rev = n->rev;
        s.local_epoch = fnode_epoch(n);
        s.last_feed_epoch = n->guard.last_feed_epoch;
        s.on_ms_today = n->guard.on_ms_today;
        s.feeds_today = n->guard.feeds_today;
        s.uptime_s = (now_ms - n->boot_ms) / 1000;
        to_hmi(n, &s, sizeof(s));
    }
    if (n->ev_pending && (n->ev_sent_ms == 0 || now_ms - n->ev_sent_ms >= 2000)) {
        n->ev_sent_ms = now_ms;
        to_hmi(n, &n->ev, sizeof(n->ev));
    }
}

void plat_send(const uint8_t mac[6], const void *msg, uint8_t len)
{
    for (size_t i = 0; i < 2; i++) {
        if (memcmp(fnodes[i].mac, mac, 6) == 0) {
            fnode_rx(&fnodes[i], msg, len);
        }
    }
}


/* Phone set-up access point and SD card. */
bool setup_ap_open, setup_done;
bool sd_inserted = true;
int sd_lines;
char sd_last_line[200], sd_last_header[80];

bool plat_wifi_setup_begin(char *ssid, char *pass)
{
    if (ota_ap_open) {
        return false;
    }
    strcpy(ssid, "AIO-Feeder-Setup");
    strcpy(pass, "48151623");
    setup_ap_open = true;
    setup_done = false;
    return true;
}
void plat_wifi_setup_end(void) { setup_ap_open = false; }
bool plat_wifi_setup_done(void) { return setup_done; }

bool plat_sd_ready(void) { return sd_inserted; }
void plat_sd_append(const char *file, const char *header, const char *line)
{
    (void)file;
    if (!sd_inserted) {
        return;
    }
    snprintf(sd_last_header, sizeof(sd_last_header), "%s", header);
    snprintf(sd_last_line, sizeof(sd_last_line), "%s", line);
    sd_lines++;
}

void sim_world_step(uint32_t ms)
{
    static frame_t batch[32];

    now_ms = ms;
    net_tick();
    for (size_t i = 0; i < 2; i++) {
        fnode_tick(&fnodes[i]);
    }
    int n = inbox_n;
    memcpy(batch, inbox, sizeof(frame_t) * (size_t)n);
    inbox_n = 0;
    for (int i = 0; i < n; i++) {
        app_on_packet(batch[i].mac, batch[i].data, batch[i].len);
    }
}
