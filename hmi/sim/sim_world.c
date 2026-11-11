/*
 * The simulated world behind the HMI application: a virtual clock, storage
 * that forgets, a fake Wi-Fi router and NTP server, and two feeders running
 * the real node state machine on fake hardware. Shared by the scripted test (sim_main.c) and the browser demo
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
void plat_wifi_forget(void) { wifi_ssid[0] = '\0'; wifi_state = PLAT_WIFI_IDLE; sim_hmi_channel = 1; }
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
        sim_hmi_channel = 6; /* the router's channel; ESP-NOW follows it */
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

static const uint8_t mac_hmi[6] = { 0x24, 0x0A, 0xC4, 0x00, 0x00, 0x01 };

/* ---------------------------------------------------- simulated feeders */

/*
 * Each feeder runs the real node state machine (feeder/node.h) on fake
 * hardware: a radio that only hears the HMI when both are on the same
 * channel, a clock, a relay, and flash that survives a power cut.
 */
sim_node_t fnodes[2] = {
    { .mac = { 0x5C, 0xCF, 0x7F, 0x10, 0x20, 0x01 }, .powered = true, .fw_minor = 1 },
    { .mac = { 0x5C, 0xCF, 0x7F, 0x10, 0x20, 0x02 }, .powered = true, .fw_minor = 1 },
};
uint8_t sim_hmi_channel = 1;

/* Frames travelling feeder -> HMI, delivered from sim_world_step(). */
typedef struct { uint8_t mac[6]; uint8_t len; uint8_t data[250]; } frame_t;
static frame_t inbox[64];
static int inbox_n;

static uint32_t node_epoch(const sim_node_t *s) { return s->clock_base + now_ms / 1000u; }

void sim_node_set_clock(int i, uint32_t local_epoch)
{
    fnodes[i].clock_base = local_epoch - now_ms / 1000u;
    fnodes[i].clock_set = true;
}

void sim_node_prepair(int i, uint8_t node_id)
{
    sim_node_t *s = &fnodes[i];
    fd_node_default_config(&s->flash_cfg);
    memset(&s->flash_state, 0, sizeof(s->flash_state));
    s->flash_cfg.paired = 1;
    memcpy(s->flash_cfg.hmi_mac, mac_hmi, 6);
    s->flash_cfg.node_id = node_id;
    s->flash_cfg.channel = sim_hmi_channel;
    s->flash_valid = true;
    s->running = false; /* boot from this flash on the next step */
}

int32_t sim_node_clock_error(int i)
{
    return (int32_t)node_epoch(&fnodes[i]) - (int32_t)plat_local_epoch();
}

static void io_send(void *ctx, const uint8_t mac[6], const void *msg, uint8_t len)
{
    sim_node_t *s = ctx;
    (void)mac; /* the HMI is the only other station */
    if (s->channel == sim_hmi_channel && inbox_n < 64) {
        memcpy(inbox[inbox_n].mac, s->mac, 6);
        memcpy(inbox[inbox_n].data, msg, len);
        inbox[inbox_n].len = len;
        inbox_n++;
    }
}
static void io_channel(void *ctx, uint8_t ch) { ((sim_node_t *)ctx)->channel = ch; }
static void io_relay(void *ctx, bool on)
{
    sim_node_t *s = ctx;
    if (on && !s->relay) {
        s->relay_pulses++;
    }
    s->relay = on;
}
static void io_save_cfg(void *ctx, const fd_node_cfg_t *cfg) { ((sim_node_t *)ctx)->flash_cfg = *cfg; }
static void io_save_state(void *ctx, const fd_node_state_t *st) { ((sim_node_t *)ctx)->flash_state = *st; }
static bool io_set_clock(void *ctx, uint32_t e)
{
    sim_node_t *s = ctx;
    s->clock_base = e - now_ms / 1000u;
    s->clock_set = true;
    return true;
}
static void io_failsafe(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }
static void io_ota(void *ctx, const fd_msg_ota_start_t *m)
{
    (void)m;
    ((sim_node_t *)ctx)->ota_until_ms = now_ms + 8000; /* join, download, flash */
}

static void node_boot(sim_node_t *s)
{
    const fd_node_io_t io = { s, io_send, io_channel, io_relay, io_save_cfg, io_save_state,
                              io_set_clock, io_failsafe, io_ota };
    if (!s->flash_valid) {
        fd_node_default_config(&s->flash_cfg);
        memset(&s->flash_state, 0, sizeof(s->flash_state));
        s->flash_valid = true;
    }
    s->relay = false;
    fd_node_init(&s->n, &io, &s->flash_cfg, &s->flash_state, 0, s->fw_minor, true, s->clock_set, now_ms,
                 node_epoch(s));
    s->running = true;
}

static void node_tick(sim_node_t *s)
{
    if (!s->powered) {
        s->running = false;
        s->relay = false;
        s->ota_until_ms = 0;
        return;
    }
    if (s->ota_until_ms) {
        if (now_ms < s->ota_until_ms) {
            return; /* off the air while it downloads */
        }
        /* Reboot: into the new firmware if the download worked. */
        s->ota_until_ms = 0;
        if (ota_ap_open && !s->ota_broken) {
            ota_served = true;
            s->fw_minor++;
        }
        node_boot(s);
    }
    if (!s->running) {
        node_boot(s);
    }
    fd_node_tick(&s->n, now_ms, node_epoch(s));
}

void plat_send(const uint8_t mac[6], const void *msg, uint8_t len)
{
    for (size_t i = 0; i < 2; i++) {
        sim_node_t *s = &fnodes[i];
        if (memcmp(s->mac, mac, 6) == 0 && s->running && !s->ota_until_ms && s->channel == sim_hmi_channel) {
            fd_node_rx(&s->n, now_ms, node_epoch(s), mac_hmi, msg, len);
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
    static frame_t batch[64];

    now_ms = ms;
    net_tick();
    for (size_t i = 0; i < 2; i++) {
        node_tick(&fnodes[i]);
    }
    int n = inbox_n;
    memcpy(batch, inbox, sizeof(frame_t) * (size_t)n);
    inbox_n = 0;
    for (int i = 0; i < n; i++) {
        app_on_packet(batch[i].mac, batch[i].data, batch[i].len);
    }
}
