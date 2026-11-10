/*
 * Host simulator for the HMI application.
 *
 * Runs the real app + UI code on a headless 800x480 LVGL display, with two
 * simulated feeder nodes that speak the real protocol and use the same
 * safety logic as the node firmware. It walks through a scripted session,
 * checks the outcome and writes a screenshot (PPM) of every screen.
 *
 *   feeder_sim <output dir>
 *
 * Exit code is non-zero if a check fails or LVGL logged a warning or error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "lvgl.h"
#include "plat.h"
#include "ui.h"

#define W 800
#define H 480

/* ------------------------------------------------------- virtual time */

static uint32_t now_ms = 1000;
static uint32_t epoch_base; /* local epoch at now_ms == 0; 0 = clock not set */

uint32_t plat_millis(void) { return now_ms; }
bool plat_time_valid(void) { return epoch_base != 0; }
uint32_t plat_local_epoch(void) { return epoch_base ? epoch_base + now_ms / 1000 : now_ms / 1000; }
void plat_set_local_epoch(uint32_t e) { epoch_base = e - now_ms / 1000; }

/* ------------------------------------------------------------ storage */

bool plat_load(const char *key, void *buf, size_t len)
{
    (void)key; (void)buf; (void)len;
    return false; /* always start empty */
}

static int saves;
void plat_save(const char *key, const void *buf, size_t len)
{
    (void)key; (void)buf; (void)len;
    saves++;
}

static bool backlight = true;
void plat_backlight(bool on) { backlight = on; }

/* -------------------------------------------------------- fake feeders */

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
} fake_node_t;

static fake_node_t fnodes[2] = {
    { .mac = { 0x5C, 0xCF, 0x7F, 0x10, 0x20, 0x01 }, .powered = true },
    { .mac = { 0x5C, 0xCF, 0x7F, 0x10, 0x20, 0x02 }, .powered = true },
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
    if (!n->paired) {
        if (now_ms - n->last_hello_ms >= 400) {
            n->last_hello_ms = now_ms;
            fd_msg_hello_t h;
            fd_hdr_init(&h.hdr, FD_MSG_HELLO, ++n->seq);
            h.fw_major = 0; h.fw_minor = 1; h.flags = FD_FLAG_PAIRING;
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
        s.fw_major = 0; s.fw_minor = 1;
        s.sched_rev = n->rev;
        s.local_epoch = fnode_epoch(n);
        s.last_feed_epoch = n->guard.last_feed_epoch;
        s.on_ms_today = n->guard.on_ms_today;
        s.feeds_today = n->guard.feeds_today;
        s.uptime_s = now_ms / 1000;
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

/* ------------------------------------------------------------ display */

static uint16_t fb[W * H];
static const char *out_dir = ".";
static int problems;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static void log_cb(lv_log_level_t level, const char *buf)
{
    fputs(buf, stderr);
    if (level >= LV_LOG_LEVEL_WARN) {
        problems++;
    }
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) {
        now_ms += 5;
        lv_tick_inc(5);
        for (size_t i = 0; i < 2; i++) {
            fnode_tick(&fnodes[i]);
        }
        int n = inbox_n;
        static frame_t batch[32];
        memcpy(batch, inbox, sizeof(frame_t) * (size_t)n);
        inbox_n = 0;
        for (int i = 0; i < n; i++) {
            app_on_packet(batch[i].mac, batch[i].data, batch[i].len);
        }
        lv_timer_handler();
    }
}

static void shot(const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = fb[i];
        uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 0x1F) * 255 / 31), (uint8_t)(((p >> 5) & 0x3F) * 255 / 63),
                           (uint8_t)((p & 0x1F) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  screenshot %s\n", path);
}

/* ------------------------------------------------------- UI automation */

#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            printf("  ok    %s\n", what);                                       \
        } else {                                                                \
            printf("  FAIL  %s\n", what);                                       \
            problems++;                                                         \
        }                                                                       \
    } while (0)

/* Depth-first: the n-th (0-based) visible button under `root` whose label
 * child has exactly this text. */
static lv_obj_t *find_button(lv_obj_t *root, const char *text, int *n)
{
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_button_class)) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
            lv_obj_t *c = lv_obj_get_child(root, (int32_t)i);
            if (lv_obj_check_type(c, &lv_label_class) && strcmp(lv_label_get_text(c), text) == 0) {
                if ((*n)-- == 0) {
                    return root;
                }
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *r = find_button(lv_obj_get_child(root, (int32_t)i), text, n);
        if (r) {
            return r;
        }
    }
    return NULL;
}

/* Click the n-th visible button with this label, on the top layer first. */
static bool click(const char *text, int n)
{
    int k = n;
    lv_obj_t *b = find_button(lv_layer_top(), text, &k);
    if (!b) {
        k = n;
        b = find_button(lv_screen_active(), text, &k);
    }
    if (!b) {
        printf("  FAIL  button \"%s\" #%d not found\n", text, n);
        problems++;
        return false;
    }
    lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
    run(400); /* longer than one UI tick, so lists are rebuilt */
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        out_dir = argv[1];
    }

    lv_init();
    lv_log_register_print_cb(log_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    app_init();
    ui_init();

    printf("First boot, no clock, no feeders\n");
    fnodes[0].powered = fnodes[1].powered = false;
    run(1000);
    CHECK(app_node_count() == 0, "starts with no feeders");
    shot("01_home_empty");

    printf("Set the clock from Settings\n");
    ui_show_page(4);
    run(300);
    shot("02_settings");
    click("Set clock", 0);
    CHECK(plat_time_valid(), "clock is valid after Set clock");
    click("OK", 0);
    /* Jump to a fixed moment so the screenshots are reproducible:
     * Monday 2026-11-09 07:58:30. */
    plat_set_local_epoch(1794182400u + 7 * 3600 + 58 * 60 + 30);

    printf("Two feeders are put in pairing mode\n");
    fnodes[0].powered = fnodes[1].powered = true;
    ui_show_page(2);
    run(1500);
    CHECK(app_found(0) && app_found(1), "both feeders are discovered");
    shot("03_feeders_discovered");
    click("Add", 0);
    click("Add", 0);
    run(12000);
    CHECK(app_node_count() == 2, "both feeders adopted");
    CHECK(app_node_online(0) && app_node_online(1), "both feeders online");
    CHECK(fnodes[0].time_valid && fnodes[1].time_valid, "feeders received the time");
    shot("04_feeders_paired");

    printf("Rename the first feeder\n");
    click("Rename", 0);
    shot("05_rename");
    app_rename_node(0, "Goldfish");
    lv_obj_send_event(lv_obj_find_by_name(lv_layer_top(), "rename_kb"), LV_EVENT_CANCEL, NULL);
    run(300);

    printf("Build a schedule\n");
    ui_show_page(1);
    run(300);
    click(LV_SYMBOL_PLUS "  Add feeding time", 0);
    shot("06_schedule_editor");
    click("Save", 0); /* default 08:00 every day */
    click(LV_SYMBOL_PLUS "  Add feeding time", 0);
    lv_subject_set_int(lv_xml_get_subject(NULL, "ed_hour"), 18);
    lv_subject_set_int(lv_xml_get_subject(NULL, "ed_min"), 30);
    lv_subject_set_int(lv_xml_get_subject(NULL, "ed_pulses"), 3);
    lv_subject_set_int(lv_xml_get_subject(NULL, "ed_d0"), 0);
    lv_subject_set_int(lv_xml_get_subject(NULL, "ed_d6"), 0);
    click("Save", 0);
    run(12000);
    CHECK(app_node(0)->count == 2, "two feeding times stored");
    CHECK(fnodes[0].count == 2 && fnodes[0].rev == app_node(0)->rev, "feeder has the new schedule");
    CHECK(fnodes[0].entries[1].hour == 18 && fnodes[0].entries[1].dow_mask == (FD_ENTRY_ENABLED | 0x3E),
          "weekday 18:30 entry arrived intact");
    shot("07_schedule");

    printf("Wait for the 08:00 scheduled feed\n");
    ui_show_page(0);
    run(90000);
    CHECK(fnodes[0].feeds_run == 1, "scheduled feed ran once");
    CHECK(app_log_count() == 1 && app_log_get(0)->result == FD_RES_OK && app_log_get(0)->source == 0,
          "scheduled feed is in the history");
    shot("08_home");

    printf("Manual feed on the second feeder\n");
    click("Feed now", 1);
    shot("09_feed_confirm");
    click("Feed", 0);
    run(1000);
    CHECK(fnodes[1].feeds_run == 1, "manual feed ran");
    shot("10_feed_result");
    click("OK", 0);

    printf("Second manual feed is refused by the cooldown\n");
    click("Feed now", 1);
    click("Feed", 0);
    run(1000);
    CHECK(fnodes[1].feeds_run == 1, "feeder did not feed again");
    CHECK(app_feed_last_result() == FD_RES_COOLDOWN, "HMI reports the cooldown");
    shot("11_feed_refused");
    click("OK", 0);

    printf("A feeder loses power\n");
    fnodes[1].powered = false;
    run(40000);
    CHECK(!app_node_online(1) && app_node_online(0), "it is shown offline");
    click("Feed now", 1);
    click("Feed", 0);
    run(3000);
    CHECK(app_feed_last_result() == APP_FEED_NO_REPLY, "feed to an offline feeder reports no reply");
    click("OK", 0);
    shot("12_home_offline");

    ui_show_page(3);
    run(12000);
    CHECK(app_log_count() == 2, "history has both feeds");
    shot("13_history");

    printf("Light theme\n");
    lv_subject_set_int(lv_xml_get_subject(NULL, "dark"), 0);
    ui_show_page(0);
    run(300);
    shot("14_home_light");
    ui_show_page(1);
    run(300);
    shot("15_schedule_light");

    printf("Screen sleep\n");
    CHECK(backlight, "backlight on while active");
    run(6 * 60000);
    CHECK(!backlight, "backlight off after the idle timeout");

    CHECK(saves > 0, "state was saved");
    printf("%s (%d problem%s)\n", problems ? "FAILED" : "PASSED", problems, problems == 1 ? "" : "s");
    return problems ? 1 : 0;
}
