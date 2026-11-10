/*
 * HMI user interface. Layout lives in the XML files under hmi/ui/ and is
 * loaded at run time; this file registers what the XML refers to (fonts,
 * subjects, event callbacks), fills the dynamic lists and turns user actions
 * into calls on the application model (app.h).
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "lvgl.h"
#include "plat.h"
#include "ui_xml.h"

#define UI_TICK_MS       250
#define LIMITS_SETTLE_MS 1200

enum { PAGE_HOME, PAGE_SCHEDULE, PAGE_NODES, PAGE_HISTORY, PAGE_SETTINGS, PAGE_NETWORK };

#define UPDATES_WINDOW_MS (10u * 60u * 1000u)
#define MAX_APS 8

/* ------------------------------------------------------------ subjects */

static lv_subject_t s_page, s_node_count, s_sched_node, s_dark;
static lv_subject_t s_title, s_clock, s_date, s_alert, s_log_summary, s_about;
static lv_subject_t s_lim_cool, s_lim_daily, s_lim_gap;
static lv_subject_t s_lim_cool_txt, s_lim_daily_txt, s_lim_gap_txt;
static lv_subject_t s_ed_hour, s_ed_min, s_ed_on, s_ed_pulses, s_ed_on_txt, s_ed_title;
static lv_subject_t s_ed_day[7];
static lv_subject_t s_set_year, s_set_month, s_set_day, s_set_hour, s_set_min;
static lv_subject_t s_feed_on, s_feed_pulses, s_feed_on_txt, s_sleep_min, s_sleep_txt;
static lv_subject_t s_wifi_status, s_ntp_on, s_ntp_status, s_utc_off, s_utc_txt, s_ota_allow, s_ota_status;

#define STR_SUBJECT(subj, name, len, init)                                      \
    do {                                                                        \
        static char buf_[len], prev_[len];                                      \
        lv_subject_init_string(&(subj), buf_, prev_, len, init);                \
        lv_xml_register_subject(NULL, name, &(subj));                           \
    } while (0)

#define INT_SUBJECT(subj, name, init)                                           \
    do {                                                                        \
        lv_subject_init_int(&(subj), init);                                     \
        lv_xml_register_subject(NULL, name, &(subj));                           \
    } while (0)

/* --------------------------------------------------------------- state */

static lv_obj_t *root;
static lv_obj_t *home_list, *sched_list, *sched_dd, *node_list, *found_list, *hist_list, *wifi_list;
static lv_obj_t *modal; /* the one open dialog/editor, or NULL */

static int  dd_map[APP_MAX_NODES]; /* dropdown position -> node slot */
static int  dd_count;
static bool loading;               /* true while code (not the user) sets subjects */
static uint32_t limits_dirty_ms;   /* 0 = clean */
static int  limits_dirty_node = -1;
static int  ed_entry = -1;         /* entry being edited, -1 = new */
static int  rename_node = -1;
typedef enum { TEXT_RENAME, TEXT_WIFI_PASS } text_purpose_t;
static text_purpose_t text_purpose;
static plat_ap_t aps[MAX_APS];
static int  ap_count;
static bool scanning;
static char wifi_pick[33];
static uint32_t updates_since_ms; /* when uploads were allowed; 0 = not allowed */
static uint32_t last_minute;
static bool backlight_on = true;

typedef enum { DLG_NONE, DLG_FEED, DLG_REMOVE, DLG_DELETE_ENTRY, DLG_CLEAR_LOG } dlg_action_t;
static dlg_action_t dlg_action;
static int dlg_arg;

/* ------------------------------------------------------------- helpers */

static const char *const DOW3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MON3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* "Mon 09 Nov 18:00", or "today 18:00" style when `relative` and close. */
static void fmt_when(char *buf, size_t n, uint32_t epoch, bool relative)
{
    if (epoch == 0) {
        snprintf(buf, n, "never");
        return;
    }
    fd_civil_t c;
    fd_civil_from_epoch(epoch, &c);
    if (relative && plat_time_valid()) {
        int32_t dd = (int32_t)(epoch / 86400u) - (int32_t)(plat_local_epoch() / 86400u);
        if (dd >= -1 && dd <= 1) {
            snprintf(buf, n, "%s %02d:%02d", dd == 0 ? "today" : dd < 0 ? "yesterday" : "tomorrow",
                     c.hour, c.minute);
            return;
        }
    }
    snprintf(buf, n, "%s %02d %s %02d:%02d", DOW3[c.dow], c.day, MON3[c.month - 1], c.hour, c.minute);
}

static void fmt_ds(char *buf, size_t n, int ds) { snprintf(buf, n, "%d.%d s", ds / 10, ds % 10); }

static void fmt_days(char *buf, size_t n, uint8_t mask)
{
    mask &= FD_DOW_ALL;
    if (mask == FD_DOW_ALL) {
        snprintf(buf, n, "Every day");
    } else if (mask == 0x3E) {
        snprintf(buf, n, "Weekdays");
    } else if (mask == 0x41) {
        snprintf(buf, n, "Weekends");
    } else if (mask == 0) {
        snprintf(buf, n, "No days");
    } else {
        size_t pos = 0;
        buf[0] = '\0';
        for (int d = 0; d < 7 && pos + 5 < n; d++) {
            if (mask & (1u << d)) {
                pos += (size_t)snprintf(buf + pos, n - pos, "%s%s", pos ? " " : "", DOW3[d]);
            }
        }
    }
}

static void fmt_portion(char *buf, size_t n, uint8_t on_ds, uint8_t pulses)
{
    if (pulses == 1) {
        snprintf(buf, n, "1 pulse of %d.%d s", on_ds / 10, on_ds % 10);
    } else {
        snprintf(buf, n, "%d pulses of %d.%d s", pulses, on_ds / 10, on_ds % 10);
    }
}

/* Slot index stored on each list row. +1 so that 0 means "not a row". */
static void row_set_index(lv_obj_t *row, int idx) { lv_obj_set_user_data(row, (void *)(intptr_t)(idx + 1)); }

/* Walk up from the widget that fired an event to the row that holds it. */
static int row_index_of(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target_obj(e);
    while (o) {
        intptr_t v = (intptr_t)lv_obj_get_user_data(o);
        if (v > 0) {
            return (int)v - 1;
        }
        o = lv_obj_get_parent(o);
    }
    return -1;
}

static int sched_node_idx(void)
{
    int pos = lv_subject_get_int(&s_sched_node);
    return pos >= 0 && pos < dd_count ? dd_map[pos] : -1;
}

/* -------------------------------------------------------------- modals */

static void modal_close(void)
{
    if (modal) {
        lv_obj_delete_async(modal);
        modal = NULL;
    }
}

static void dialog_open(dlg_action_t action, int arg, const char *title, const char *text,
                        const char *ok_text)
{
    modal_close();
    dlg_action = action;
    dlg_arg = arg;
    const char *attrs[] = {
        "title", title,
        "text", text,
        "ok_text", ok_text ? ok_text : "OK",
        "no_cancel", action == DLG_NONE ? "true" : "false",
        NULL, NULL,
    };
    modal = lv_xml_create(lv_layer_top(), "dialog", attrs);
}

/* ------------------------------------------------------------- refresh */

static void refresh_alert(void)
{
    char buf[64] = "";
    int offline = 0, noclock = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_rt_t *r = app_node_rt(i);
        if (!r) {
            continue;
        }
        if (!app_node_online(i)) {
            offline++;
        } else if (r->status.flags & FD_FLAG_RTC_MISSING) {
            noclock++;
        }
    }
    if (!plat_time_valid()) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " Clock not set");
    } else if (offline) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %d feeder%s offline", offline, offline > 1 ? "s" : "");
    } else if (noclock) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %d feeder%s without a clock", noclock,
                 noclock > 1 ? "s have" : " has");
    }
    lv_subject_copy_string(&s_alert, buf);
}

static void node_state(int idx, const char **text, const char **color)
{
    const app_node_rt_t *r = app_node_rt(idx);
    if (!app_node_online(idx)) {
        *text = r && r->adopted ? "Offline" : "Pairing...";
        *color = r && r->adopted ? "0xd64545" : "0xe09a1a";
    } else if (r->status.flags & FD_FLAG_FEEDING) {
        *text = "Feeding";
        *color = "0x2196f3";
    } else if (r->status.flags & FD_FLAG_RTC_MISSING) {
        *text = "No clock";
        *color = "0xd64545";
    } else if (!(r->status.flags & FD_FLAG_TIME_VALID)) {
        *text = "Clock not set";
        *color = "0xe09a1a";
    } else if (r->status.sched_rev != app_node(idx)->rev) {
        *text = "Syncing";
        *color = "0xe09a1a";
    } else {
        *text = "Online";
        *color = "0x2e9e5b";
    }
}

static void refresh_home(void)
{
    lv_obj_clean(home_list);
    uint32_t now = plat_local_epoch();
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_cfg_t *n = app_node(i);
        if (!n) {
            continue;
        }
        const app_node_rt_t *r = app_node_rt(i);
        const char *state, *color;
        node_state(i, &state, &color);

        char when[32], last[48], next[48], today[48];
        if (r->have_status) {
            fmt_when(when, sizeof(when), r->status.last_feed_epoch, true);
            snprintf(last, sizeof(last), "Last fed: %s", when);
            snprintf(today, sizeof(today), "Today: %u feed%s, %u.%u s", r->status.feeds_today,
                     r->status.feeds_today == 1 ? "" : "s", (unsigned)(r->status.on_ms_today / 1000),
                     (unsigned)(r->status.on_ms_today % 1000 / 100));
        } else {
            snprintf(last, sizeof(last), "Last fed: unknown");
            snprintf(today, sizeof(today), "Today: unknown");
        }
        uint32_t nx = plat_time_valid() ? app_next_feed(i, now) : 0;
        if (nx) {
            fmt_when(when, sizeof(when), nx, true);
            snprintf(next, sizeof(next), "Next: %s", when);
        } else {
            snprintf(next, sizeof(next), n->count ? "Next: nothing scheduled" : "Next: no schedule");
        }

        const char *attrs[] = {
            "name", n->name, "state", state, "state_color", color,
            "last", last, "next", next, "today", today, NULL, NULL,
        };
        lv_obj_t *card = lv_xml_create(home_list, "tank_card", attrs);
        row_set_index(card, i);
    }
}

static void limits_to_text(void)
{
    char buf[24];
    int v = lv_subject_get_int(&s_lim_cool);
    snprintf(buf, sizeof(buf), v ? "%d min" : "Off", v);
    lv_subject_copy_string(&s_lim_cool_txt, buf);
    v = lv_subject_get_int(&s_lim_daily);
    snprintf(buf, sizeof(buf), v ? "%d s" : "Off", v);
    lv_subject_copy_string(&s_lim_daily_txt, buf);
    fmt_ds(buf, sizeof(buf), lv_subject_get_int(&s_lim_gap));
    lv_subject_copy_string(&s_lim_gap_txt, buf);
}

static void refresh_schedule(void)
{
    /* Rebuild the feeder dropdown, keeping the same feeder selected. */
    int prev = sched_node_idx();
    char opts[APP_MAX_NODES * (FD_NAME_LEN + 1)] = "";
    int sel = 0;
    dd_count = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_cfg_t *n = app_node(i);
        if (!n) {
            continue;
        }
        if (dd_count) {
            strcat(opts, "\n");
        }
        strcat(opts, n->name);
        if (i == prev) {
            sel = dd_count;
        }
        dd_map[dd_count++] = i;
    }
    loading = true;
    lv_dropdown_set_options(sched_dd, opts);
    lv_subject_set_int(&s_sched_node, sel);
    lv_dropdown_set_selected(sched_dd, (uint32_t)sel);

    lv_obj_clean(sched_list);
    int idx = sched_node_idx();
    const app_node_cfg_t *n = app_node(idx);
    if (n) {
        for (uint8_t i = 0; i < n->count; i++) {
            const fd_sched_entry_t *e = &n->entries[i];
            char time[8], days[40], portion[32];
            snprintf(time, sizeof(time), "%02d:%02d", e->hour, e->minute);
            fmt_days(days, sizeof(days), e->dow_mask);
            fmt_portion(portion, sizeof(portion), e->on_ds, e->pulses);
            const char *attrs[] = {
                "time", time, "days", days, "portion", portion,
                "enabled", (e->dow_mask & FD_ENTRY_ENABLED) ? "true" : "false", NULL, NULL,
            };
            lv_obj_t *row = lv_xml_create(sched_list, "sched_row", attrs);
            row_set_index(row, i);
        }
        if (n->count == 0) {
            lv_obj_t *l = lv_label_create(sched_list);
            lv_label_set_text(l, "No feeding times yet.");
        }
        if (limits_dirty_ms == 0) {
            lv_subject_set_int(&s_lim_cool, n->limits.cooldown_s / 60);
            lv_subject_set_int(&s_lim_daily, n->limits.daily_max_on_s);
            lv_subject_set_int(&s_lim_gap, n->limits.gap_ds);
        }
    }
    limits_to_text();
    loading = false;
}

static void refresh_nodes(void)
{
    lv_obj_clean(node_list);
    int count = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_cfg_t *n = app_node(i);
        if (!n) {
            continue;
        }
        count++;
        const app_node_rt_t *r = app_node_rt(i);
        const char *state, *color;
        node_state(i, &state, &color);
        char detail[64];
        int len = snprintf(detail, sizeof(detail), "%02X:%02X:%02X:%02X:%02X:%02X", n->mac[0], n->mac[1],
                           n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
        if (r->have_status) {
            snprintf(detail + len, sizeof(detail) - (size_t)len, "   firmware %u.%u", r->status.fw_major,
                     r->status.fw_minor);
        }
        const char *attrs[] = {
            "name", n->name, "detail", detail, "state", state, "state_color", color, NULL, NULL,
        };
        lv_obj_t *row = lv_xml_create(node_list, "node_row", attrs);
        row_set_index(row, i);
    }
    if (count == 0) {
        lv_obj_t *l = lv_label_create(node_list);
        lv_label_set_text(l, "None yet.");
    }
    lv_subject_set_int(&s_node_count, count);
}

static void refresh_found(void)
{
    lv_obj_clean(found_list);
    int count = 0;
    for (int i = 0; i < APP_MAX_FOUND; i++) {
        const app_found_t *f = app_found(i);
        if (!f) {
            continue;
        }
        count++;
        char mac[24];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", f->mac[0], f->mac[1], f->mac[2],
                 f->mac[3], f->mac[4], f->mac[5]);
        const char *attrs[] = { "mac", mac, NULL, NULL };
        lv_obj_t *row = lv_xml_create(found_list, "found_row", attrs);
        row_set_index(row, i);
    }
    if (count == 0) {
        lv_obj_t *l = lv_label_create(found_list);
        lv_label_set_text(l, "Searching...");
    }
}

static void refresh_history(void)
{
    lv_obj_clean(hist_list);
    int count = app_log_count();
    char sum[40];
    snprintf(sum, sizeof(sum), count ? "Last %d events, newest first" : "No feeds recorded yet", count);
    lv_subject_copy_string(&s_log_summary, sum);

    for (int i = 0; i < count; i++) {
        const app_log_entry_t *e = app_log_get(i);
        const app_node_cfg_t *n = app_node(e->node);
        char when[32], what[32], tank[FD_NAME_LEN + 4];
        fmt_when(when, sizeof(when), e->epoch, false);
        if (n) {
            snprintf(tank, sizeof(tank), "%s", n->name);
        } else {
            snprintf(tank, sizeof(tank), "(removed)");
        }
        if (e->source == FD_SOURCE_MANUAL) {
            snprintf(what, sizeof(what), "Manual");
        } else {
            snprintf(what, sizeof(what), "Schedule #%d", e->source + 1);
        }
        char result[48];
        if (e->result == FD_RES_OK) {
            snprintf(result, sizeof(result), "Fed %u.%u s", e->on_ms / 1000, e->on_ms % 1000 / 100);
        } else {
            snprintf(result, sizeof(result), "%s", app_result_text(e->result));
        }
        const char *color = e->result == FD_RES_OK ? "0x2e9e5b"
                            : e->result == FD_RES_ABORTED ? "0xd64545" : "0xe09a1a";
        const char *attrs[] = {
            "when", when, "tank", tank, "what", what, "result", result, "result_color", color,
            NULL, NULL,
        };
        lv_xml_create(hist_list, "hist_row", attrs);
    }
}

static void refresh_clock(bool force)
{
    uint32_t now = plat_local_epoch();
    if (!force && now / 60 == last_minute) {
        return;
    }
    last_minute = now / 60;
    if (!plat_time_valid()) {
        lv_subject_copy_string(&s_clock, "--:--");
        lv_subject_copy_string(&s_date, "");
    } else {
        fd_civil_t c;
        char buf[24];
        fd_civil_from_epoch(now, &c);
        snprintf(buf, sizeof(buf), "%02d:%02d", c.hour, c.minute);
        lv_subject_copy_string(&s_clock, buf);
        snprintf(buf, sizeof(buf), "%s %d %s", DOW3[c.dow], c.day, MON3[c.month - 1]);
        lv_subject_copy_string(&s_date, buf);
    }
    refresh_alert();
    refresh_home(); /* "today", "tomorrow" and the next feed depend on the time */
}

/* Put the current time (or a sane default) into the Settings steppers. */
static void load_time_steppers(void)
{
    fd_civil_t c = { 2026, 1, 1, 12, 0, 0, 0 };
    if (plat_time_valid()) {
        fd_civil_from_epoch(plat_local_epoch(), &c);
    }
    lv_subject_set_int(&s_set_year, c.year);
    lv_subject_set_int(&s_set_month, c.month);
    lv_subject_set_int(&s_set_day, c.day);
    lv_subject_set_int(&s_set_hour, c.hour);
    lv_subject_set_int(&s_set_min, c.minute);
}

/* ----------------------------------------------------------- observers */

static void apply_theme(void)
{
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE),
                                           lv_palette_main(LV_PALETTE_TEAL),
                                           app_settings()->dark, LV_FONT_DEFAULT);
    lv_display_set_theme(d, th);
    lv_obj_report_style_change(NULL);
}

static void on_page_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    static const char *const titles[] = { "Home", "Schedule", "Feeders", "History", "Settings", "Network" };
    int p = lv_subject_get_int(s);
    lv_subject_copy_string(&s_title, titles[p >= 0 && p < 6 ? p : 0]);
    if (p == PAGE_SETTINGS && !loading) {
        load_time_steppers();
    }
}

static void on_sched_node_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    if (!loading) {
        limits_dirty_ms = 0;
        refresh_schedule();
    }
}

static void on_limit_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    limits_to_text();
    if (loading) {
        return;
    }
    limits_dirty_node = sched_node_idx();
    limits_dirty_ms = plat_millis() ? plat_millis() : 1;
}

static void on_ed_on_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    char buf[16];
    fmt_ds(buf, sizeof(buf), lv_subject_get_int(s));
    lv_subject_copy_string(&s_ed_on_txt, buf);
}

static void on_settings_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    char buf[16];
    fmt_ds(buf, sizeof(buf), lv_subject_get_int(&s_feed_on));
    lv_subject_copy_string(&s_feed_on_txt, buf);
    int sl = lv_subject_get_int(&s_sleep_min);
    snprintf(buf, sizeof(buf), sl ? "%d min" : "Never", sl);
    lv_subject_copy_string(&s_sleep_txt, buf);
    if (loading) {
        return;
    }
    app_settings_t *st = app_settings();
    bool theme = st->dark != (uint8_t)lv_subject_get_int(&s_dark);
    st->dark = (uint8_t)lv_subject_get_int(&s_dark);
    st->sleep_min = (uint8_t)sl;
    st->feed_on_ds = (uint8_t)lv_subject_get_int(&s_feed_on);
    st->feed_pulses = (uint8_t)lv_subject_get_int(&s_feed_pulses);
    app_settings_save();
    if (theme) {
        apply_theme();
    }
}

/* Keep the day stepper inside the chosen month. */
static void on_set_date_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    int dim = fd_days_in_month((uint16_t)lv_subject_get_int(&s_set_year),
                               (uint8_t)lv_subject_get_int(&s_set_month));
    if (lv_subject_get_int(&s_set_day) > dim) {
        lv_subject_set_int(&s_set_day, dim);
    }
}

/* -------------------------------------------------------- event callbacks */

static void on_feed_click(lv_event_t *e)
{
    int idx = row_index_of(e);
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    char title[40], text[96], portion[32];
    const app_settings_t *st = app_settings();
    fmt_portion(portion, sizeof(portion), st->feed_on_ds, st->feed_pulses);
    snprintf(title, sizeof(title), "Feed %s now?", n->name);
    snprintf(text, sizeof(text), "The feeder will run %s.", portion);
    dialog_open(DLG_FEED, idx, title, text, "Feed");
}

static void editor_open(int entry)
{
    int idx = sched_node_idx();
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    fd_sched_entry_t e = { 8, 0, FD_ENTRY_ENABLED | FD_DOW_ALL, app_settings()->feed_on_ds,
                           app_settings()->feed_pulses };
    if (entry >= 0 && entry < n->count) {
        e = n->entries[entry];
    }
    ed_entry = entry;
    lv_subject_copy_string(&s_ed_title, entry < 0 ? "New feeding time" : "Edit feeding time");
    lv_subject_set_int(&s_ed_hour, e.hour);
    lv_subject_set_int(&s_ed_min, e.minute);
    lv_subject_set_int(&s_ed_on, e.on_ds);
    lv_subject_set_int(&s_ed_pulses, e.pulses);
    for (int d = 0; d < 7; d++) {
        lv_subject_set_int(&s_ed_day[d], (e.dow_mask >> d) & 1);
    }

    modal_close();
    modal = lv_xml_create(lv_layer_top(), "sched_editor", NULL);

    /* Roller option lists are long, so they are generated here. */
    static char hours[24 * 3], mins[60 * 3];
    if (!hours[0]) {
        char *p = hours;
        for (int i = 0; i < 24; i++) {
            p += sprintf(p, i ? "\n%02d" : "%02d", i);
        }
        p = mins;
        for (int i = 0; i < 60; i++) {
            p += sprintf(p, i ? "\n%02d" : "%02d", i);
        }
    }
    lv_obj_t *rh = lv_obj_find_by_name(modal, "ed_hour");
    lv_obj_t *rm = lv_obj_find_by_name(modal, "ed_min");
    lv_roller_set_options(rh, hours, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_options(rm, mins, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(rh, e.hour, LV_ANIM_OFF);
    lv_roller_set_selected(rm, e.minute, LV_ANIM_OFF);
}

static void on_sched_add(lv_event_t *e)
{
    (void)e;
    const app_node_cfg_t *n = app_node(sched_node_idx());
    if (!n) {
        return;
    }
    if (n->count >= FD_MAX_ENTRIES) {
        dialog_open(DLG_NONE, 0, "Schedule full", "This feeder already has the maximum number of feeding times.",
                    NULL);
        return;
    }
    editor_open(-1);
}

static void on_sched_edit(lv_event_t *e) { editor_open(row_index_of(e)); }

static void on_sched_delete(lv_event_t *e)
{
    int entry = row_index_of(e);
    const app_node_cfg_t *n = app_node(sched_node_idx());
    if (!n || entry < 0 || entry >= n->count) {
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Remove the %02d:%02d feeding time?", n->entries[entry].hour,
             n->entries[entry].minute);
    dialog_open(DLG_DELETE_ENTRY, entry, "Delete feeding time", text, "Delete");
}

static void on_sched_toggle(lv_event_t *e)
{
    int entry = row_index_of(e);
    int idx = sched_node_idx();
    app_node_cfg_t *n = app_node(idx);
    if (!n || entry < 0 || entry >= n->count) {
        return;
    }
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    if (on) {
        n->entries[entry].dow_mask |= FD_ENTRY_ENABLED;
    } else {
        n->entries[entry].dow_mask &= (uint8_t)~FD_ENTRY_ENABLED;
    }
    app_schedule_changed(idx);
}

static void on_ed_cancel(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void on_ed_save(lv_event_t *e)
{
    (void)e;
    int idx = sched_node_idx();
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        modal_close();
        return;
    }
    fd_sched_entry_t ne;
    ne.hour = (uint8_t)lv_subject_get_int(&s_ed_hour);
    ne.minute = (uint8_t)lv_subject_get_int(&s_ed_min);
    ne.on_ds = (uint8_t)lv_subject_get_int(&s_ed_on);
    ne.pulses = (uint8_t)lv_subject_get_int(&s_ed_pulses);
    ne.dow_mask = 0;
    for (int d = 0; d < 7; d++) {
        if (lv_subject_get_int(&s_ed_day[d])) {
            ne.dow_mask |= (uint8_t)(1u << d);
        }
    }
    if (ed_entry >= 0 && ed_entry < n->count) {
        ne.dow_mask |= n->entries[ed_entry].dow_mask & FD_ENTRY_ENABLED;
        n->entries[ed_entry] = ne;
    } else if (n->count < FD_MAX_ENTRIES) {
        ne.dow_mask |= FD_ENTRY_ENABLED;
        n->entries[n->count++] = ne;
    }
    /* Keep the list in time order. */
    for (int i = 1; i < n->count; i++) {
        fd_sched_entry_t key = n->entries[i];
        int j = i - 1;
        while (j >= 0 && (n->entries[j].hour * 60 + n->entries[j].minute) > (key.hour * 60 + key.minute)) {
            n->entries[j + 1] = n->entries[j];
            j--;
        }
        n->entries[j + 1] = key;
    }
    modal_close();
    app_schedule_changed(idx);
}

static void on_node_remove(lv_event_t *e)
{
    int idx = row_index_of(e);
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    char title[40];
    snprintf(title, sizeof(title), "Remove %s?", n->name);
    dialog_open(DLG_REMOVE, idx, title,
                "Its schedule is deleted from this controller and the feeder stops feeding on its own "
                "once it receives the command.",
                "Remove");
}

static void text_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && modal) {
        const char *text = lv_textarea_get_text(lv_obj_find_by_name(modal, "text_ta"));
        if (text_purpose == TEXT_RENAME) {
            app_rename_node(rename_node, text);
        } else {
            plat_wifi_connect(wifi_pick, text);
        }
    }
    modal_close();
}

static void text_open(text_purpose_t purpose, const char *label, const char *initial, uint32_t max_len,
                      bool password)
{
    text_purpose = purpose;
    modal_close();
    const char *attrs[] = { "label", label, NULL, NULL };
    modal = lv_xml_create(lv_layer_top(), "text_dialog", attrs);
    lv_obj_t *ta = lv_obj_find_by_name(modal, "text_ta");
    lv_obj_t *kb = lv_obj_find_by_name(modal, "text_kb");
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, initial);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(kb, text_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, text_done, LV_EVENT_CANCEL, NULL);
}

static void on_node_rename(lv_event_t *e)
{
    int idx = row_index_of(e);
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    rename_node = idx;
    text_open(TEXT_RENAME, "Name", n->name, FD_NAME_LEN - 1, false);
}

/* ------------------------------------------------------------- network */

static void set_text(lv_subject_t *subj, const char *text)
{
    if (strcmp(lv_subject_get_string(subj), text) != 0) {
        lv_subject_copy_string(subj, text);
    }
}

static void refresh_wifi_list(void)
{
    lv_obj_clean(wifi_list);
    if (scanning) {
        lv_obj_t *l = lv_label_create(wifi_list);
        lv_label_set_text(l, "Scanning...");
        return;
    }
    for (int i = 0; i < ap_count; i++) {
        char detail[32];
        snprintf(detail, sizeof(detail), "%s%d dBm", aps[i].secure ? LV_SYMBOL_EYE_CLOSE "  " : "",
                 aps[i].rssi);
        const char *attrs[] = { "ssid", aps[i].ssid, "detail", detail, NULL, NULL };
        lv_obj_t *row = lv_xml_create(wifi_list, "wifi_row", attrs);
        row_set_index(row, i);
    }
}

static void refresh_network(void)
{
    char ssid[33], ip[20], buf[128];
    plat_wifi_info(ssid, sizeof(ssid), ip, sizeof(ip));
    plat_wifi_state_t ws = plat_wifi_state();
    switch (ws) {
    case PLAT_WIFI_CONNECTED:  snprintf(buf, sizeof(buf), "Connected to %s, address %s", ssid, ip); break;
    case PLAT_WIFI_CONNECTING: snprintf(buf, sizeof(buf), "Connecting to %s...", ssid); break;
    case PLAT_WIFI_FAILED:     snprintf(buf, sizeof(buf), "Cannot reach %s, retrying", ssid); break;
    default:                   snprintf(buf, sizeof(buf), "Not set up. Tap Scan to choose a network."); break;
    }
    set_text(&s_wifi_status, buf);

    if (!app_settings()->ntp) {
        snprintf(buf, sizeof(buf), "Off. Set your UTC offset, then switch on.");
    } else if (plat_ntp_synced()) {
        snprintf(buf, sizeof(buf), "Clock is set from the internet.");
    } else if (ws == PLAT_WIFI_CONNECTED) {
        snprintf(buf, sizeof(buf), "Waiting for the time server...");
    } else {
        snprintf(buf, sizeof(buf), "Needs Wi-Fi. Using the built-in clock.");
    }
    set_text(&s_ntp_status, buf);

    if (updates_since_ms && plat_millis() - updates_since_ms > UPDATES_WINDOW_MS) {
        lv_subject_set_int(&s_ota_allow, 0); /* the observer closes the window */
    }
    const app_node_cfg_t *on = app_node(app_ota_node());
    if (app_ota_state() == APP_OTA_RUNNING) {
        snprintf(buf, sizeof(buf), "Updating %s. Leave it powered.", on ? on->name : "feeder");
    } else if (ws != PLAT_WIFI_CONNECTED) {
        snprintf(buf, sizeof(buf), "Needs Wi-Fi.");
    } else if (!updates_since_ms) {
        snprintf(buf, sizeof(buf), "Switch on, then open http://%s in a browser on the same network.", ip);
    } else {
        snprintf(buf, sizeof(buf), "Uploads allowed for 10 minutes at http://%s", ip);
    }
    set_text(&s_ota_status, buf);

    if (scanning) {
        int n = plat_wifi_scan_get(aps, MAX_APS);
        if (n >= 0) {
            scanning = false;
            ap_count = n;
            refresh_wifi_list();
        }
    }

    /* A feeder update asked for on the web page. */
    int req = plat_take_node_ota_request();
    if (req >= 0) {
        const char *err = app_node_ota_start(req);
        if (err) {
            dialog_open(DLG_NONE, 0, "Update not started", err, NULL);
        }
    }
}

/* State for the web page: uploads, feeders and the running update. */
static void publish_status(void)
{
    static const char *const ota_names[] = { "idle", "running", "done", "failed" };
    char json[700];
    int n = snprintf(json, sizeof(json),
                     "{\"version\":\"" APP_VERSION "\",\"allow\":%s,\"node_fw\":%u,"
                     "\"ota\":\"%s\",\"ota_node\":%d,\"ota_msg\":\"%s\",\"nodes\":[",
                     updates_since_ms ? "true" : "false", (unsigned)plat_node_fw_size(),
                     ota_names[app_ota_state()], app_ota_node(), app_ota_message());
    bool first = true;
    for (int i = 0; i < APP_MAX_NODES && n < (int)sizeof(json) - 80; i++) {
        const app_node_cfg_t *nd = app_node(i);
        if (!nd) {
            continue;
        }
        const app_node_rt_t *r = app_node_rt(i);
        /* Names come from the on-screen keyboard; drop what JSON cannot carry. */
        char name[FD_NAME_LEN];
        size_t k = 0;
        for (const char *p = nd->name; *p && k < sizeof(name) - 1; p++) {
            if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) {
                name[k++] = *p;
            }
        }
        name[k] = '\0';
        n += snprintf(json + n, sizeof(json) - (size_t)n,
                      "%s{\"i\":%d,\"name\":\"%s\",\"online\":%s,\"fw\":\"%u.%u\"}", first ? "" : ",", i,
                      name, app_node_online(i) ? "true" : "false", r->have_status ? r->status.fw_major : 0,
                      r->have_status ? r->status.fw_minor : 0);
        first = false;
    }
    snprintf(json + n, sizeof(json) - (size_t)n, "]}");
    plat_publish_status(json);
}

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    plat_wifi_scan();
    scanning = true;
    refresh_wifi_list();
}

static void on_wifi_forget(lv_event_t *e)
{
    (void)e;
    plat_wifi_forget();
}

static void on_wifi_pick(lv_event_t *e)
{
    int i = row_index_of(e);
    if (i < 0 || i >= ap_count) {
        return;
    }
    snprintf(wifi_pick, sizeof(wifi_pick), "%s", aps[i].ssid);
    if (aps[i].secure) {
        text_open(TEXT_WIFI_PASS, "Password", "", 63, true);
    } else {
        plat_wifi_connect(wifi_pick, "");
    }
    ap_count = 0;
    refresh_wifi_list();
}

static void on_network_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    int off = lv_subject_get_int(&s_utc_off);
    char buf[16];
    snprintf(buf, sizeof(buf), "%c%02d:%02d", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60,
             (off < 0 ? -off : off) % 60);
    lv_subject_copy_string(&s_utc_txt, buf);
    if (loading) {
        return;
    }
    app_settings_t *st = app_settings();
    if (st->utc_offset_min != off) {
        st->utc_offset_min = (int16_t)off;
        plat_set_utc_offset(st->utc_offset_min);
        last_minute = 0; /* redraw the clock now */
    }
    if (st->ntp != (uint8_t)lv_subject_get_int(&s_ntp_on)) {
        st->ntp = (uint8_t)lv_subject_get_int(&s_ntp_on);
        plat_ntp_enable(st->ntp);
    }
    app_settings_save();
}

static void on_ota_allow_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    bool on = lv_subject_get_int(s) != 0;
    updates_since_ms = on ? (plat_millis() ? plat_millis() : 1) : 0;
    plat_updates_allow(on);
}

static void on_found_add(lv_event_t *e)
{
    const app_found_t *f = app_found(row_index_of(e));
    if (!f) {
        return;
    }
    uint8_t mac[6];
    memcpy(mac, f->mac, 6);
    if (app_adopt(mac) < 0) {
        dialog_open(DLG_NONE, 0, "No room", "This controller already manages the maximum number of feeders.",
                    NULL);
    }
}

static void on_log_clear(lv_event_t *e)
{
    (void)e;
    if (app_log_count()) {
        dialog_open(DLG_CLEAR_LOG, 0, "Clear history", "Delete all recorded feed events?", "Clear");
    }
}

static void on_time_set(lv_event_t *e)
{
    (void)e;
    fd_civil_t c = {
        .year = (uint16_t)lv_subject_get_int(&s_set_year),
        .month = (uint8_t)lv_subject_get_int(&s_set_month),
        .day = (uint8_t)lv_subject_get_int(&s_set_day),
        .hour = (uint8_t)lv_subject_get_int(&s_set_hour),
        .minute = (uint8_t)lv_subject_get_int(&s_set_min),
    };
    plat_set_local_epoch(fd_civil_to_epoch(&c));
    refresh_clock(true);
    dialog_open(DLG_NONE, 0, "Clock set", "The feeders pick up the new time within a few seconds.", NULL);
}

static void on_dialog_cancel(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void on_dialog_ok(lv_event_t *e)
{
    (void)e;
    dlg_action_t action = dlg_action;
    int arg = dlg_arg;
    modal_close();
    switch (action) {
    case DLG_FEED:
        if (!app_feed_now(arg)) {
            dialog_open(DLG_NONE, 0, "Busy", "Another feed request is still in progress.", NULL);
        }
        break;
    case DLG_REMOVE:
        app_remove_node(arg);
        break;
    case DLG_DELETE_ENTRY: {
        int idx = sched_node_idx();
        app_node_cfg_t *n = app_node(idx);
        if (n && arg >= 0 && arg < n->count) {
            for (int i = arg; i + 1 < n->count; i++) {
                n->entries[i] = n->entries[i + 1];
            }
            n->count--;
            app_schedule_changed(idx);
        }
        break;
    }
    case DLG_CLEAR_LOG:
        app_log_clear();
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- tick */

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    app_tick();

    /* Push edited limits once the user has stopped tapping. */
    if (limits_dirty_ms && plat_millis() - limits_dirty_ms >= LIMITS_SETTLE_MS) {
        app_node_cfg_t *n = app_node(limits_dirty_node);
        limits_dirty_ms = 0;
        if (n) {
            n->limits.cooldown_s = (uint16_t)(lv_subject_get_int(&s_lim_cool) * 60);
            n->limits.daily_max_on_s = (uint16_t)lv_subject_get_int(&s_lim_daily);
            n->limits.gap_ds = (uint8_t)lv_subject_get_int(&s_lim_gap);
            app_schedule_changed(limits_dirty_node);
        }
    }

    uint32_t ch = app_take_changes();
    if (ch & APP_CH_NODES) {
        refresh_nodes();
        refresh_home();
        refresh_alert();
    }
    if (ch & APP_CH_FOUND) {
        refresh_found();
    }
    if (ch & APP_CH_SCHEDULE) {
        refresh_schedule();
        refresh_home();
    }
    if (ch & APP_CH_LOG) {
        refresh_history();
    }
    if (ch & APP_CH_FEED) {
        const app_node_cfg_t *n = app_node(app_feed_last_node());
        uint8_t res = app_feed_last_result();
        char text[80];
        snprintf(text, sizeof(text), "%s: %s", n ? n->name : "Feeder",
                 res == FD_RES_OK ? "feeding started" : app_result_text(res));
        dialog_open(DLG_NONE, 0, res == FD_RES_OK ? "Feeding" : "Not fed", text, NULL);
    }
    if (ch & APP_CH_OTA) {
        const app_node_cfg_t *n = app_node(app_ota_node());
        char text[96];
        if (app_ota_state() == APP_OTA_DONE || app_ota_state() == APP_OTA_FAILED) {
            snprintf(text, sizeof(text), "%s: %s", n ? n->name : "Feeder", app_ota_message());
            dialog_open(DLG_NONE, 0, app_ota_state() == APP_OTA_DONE ? "Feeder updated" : "Update failed",
                        text, NULL);
        }
    }
    refresh_network();
    publish_status();
    refresh_clock(false);

    /* Screen sleep. Any touch resets LVGL's inactivity timer and wakes it. */
    uint8_t sleep_min = app_settings()->sleep_min;
    bool want_on = sleep_min == 0 || lv_display_get_inactive_time(NULL) < (uint32_t)sleep_min * 60000u;
    if (want_on != backlight_on) {
        backlight_on = want_on;
        plat_backlight(want_on);
    }
}

/* ---------------------------------------------------------------- init */

static void register_xml(void)
{
    static const char *const order[] = {
        "globals", "row", "col", "nav_button", "stepper", "tank_card", "sched_row", "node_row",
        "found_row", "hist_row", "page_home", "page_schedule", "page_nodes", "page_history",
        "page_settings", "wifi_row", "page_network", "sched_editor", "dialog", "text_dialog", "main",
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const char *xml = ui_xml_get(order[i]);
        if (!xml || lv_xml_register_component_from_data(order[i], xml) != LV_RESULT_OK) {
            LV_LOG_ERROR("UI component '%s' failed to load", order[i]);
        }
    }
}

void ui_init(void)
{
    const app_settings_t *st = app_settings();
    loading = true;

    lv_xml_register_font(NULL, "font_s", &lv_font_montserrat_14);
    lv_xml_register_font(NULL, "font_m", &lv_font_montserrat_16);
    lv_xml_register_font(NULL, "font_l", &lv_font_montserrat_20);
    lv_xml_register_font(NULL, "font_xl", &lv_font_montserrat_28);

    INT_SUBJECT(s_page, "page", PAGE_HOME);
    INT_SUBJECT(s_node_count, "node_count", 0);
    INT_SUBJECT(s_sched_node, "sched_node", 0);
    INT_SUBJECT(s_dark, "dark", st->dark);
    STR_SUBJECT(s_title, "title", 16, "Home");
    STR_SUBJECT(s_clock, "clock", 8, "--:--");
    STR_SUBJECT(s_date, "date", 16, "");
    STR_SUBJECT(s_alert, "alert", 64, "");
    STR_SUBJECT(s_log_summary, "log_summary", 40, "");
    STR_SUBJECT(s_about, "about", 64, "AIO-Feeder controller " APP_VERSION);

    INT_SUBJECT(s_lim_cool, "lim_cool", 10);
    INT_SUBJECT(s_lim_daily, "lim_daily", 30);
    INT_SUBJECT(s_lim_gap, "lim_gap", 5);
    STR_SUBJECT(s_lim_cool_txt, "lim_cool_txt", 16, "");
    STR_SUBJECT(s_lim_daily_txt, "lim_daily_txt", 16, "");
    STR_SUBJECT(s_lim_gap_txt, "lim_gap_txt", 16, "");

    INT_SUBJECT(s_ed_hour, "ed_hour", 8);
    INT_SUBJECT(s_ed_min, "ed_min", 0);
    INT_SUBJECT(s_ed_on, "ed_on", 10);
    INT_SUBJECT(s_ed_pulses, "ed_pulses", 1);
    STR_SUBJECT(s_ed_on_txt, "ed_on_txt", 16, "");
    STR_SUBJECT(s_ed_title, "ed_title", 24, "");
    static const char *const day_names[7] = { "ed_d0", "ed_d1", "ed_d2", "ed_d3", "ed_d4", "ed_d5", "ed_d6" };
    for (int d = 0; d < 7; d++) {
        lv_subject_init_int(&s_ed_day[d], 1);
        lv_xml_register_subject(NULL, day_names[d], &s_ed_day[d]);
    }

    INT_SUBJECT(s_set_year, "set_year", 2026);
    INT_SUBJECT(s_set_month, "set_month", 1);
    INT_SUBJECT(s_set_day, "set_day", 1);
    INT_SUBJECT(s_set_hour, "set_hour", 12);
    INT_SUBJECT(s_set_min, "set_min", 0);
    INT_SUBJECT(s_feed_on, "feed_on", st->feed_on_ds);
    INT_SUBJECT(s_feed_pulses, "feed_pulses", st->feed_pulses);
    STR_SUBJECT(s_feed_on_txt, "feed_on_txt", 16, "");
    INT_SUBJECT(s_sleep_min, "sleep_min", st->sleep_min);
    STR_SUBJECT(s_sleep_txt, "sleep_txt", 16, "");

    STR_SUBJECT(s_wifi_status, "wifi_status", 128, "");
    INT_SUBJECT(s_ntp_on, "ntp_on", st->ntp);
    STR_SUBJECT(s_ntp_status, "ntp_status", 128, "");
    INT_SUBJECT(s_utc_off, "utc_off", st->utc_offset_min);
    STR_SUBJECT(s_utc_txt, "utc_txt", 16, "");
    INT_SUBJECT(s_ota_allow, "ota_allow", 0);
    STR_SUBJECT(s_ota_status, "ota_status", 128, "");

    lv_xml_register_event_cb(NULL, "on_wifi_scan", on_wifi_scan);
    lv_xml_register_event_cb(NULL, "on_wifi_forget", on_wifi_forget);
    lv_xml_register_event_cb(NULL, "on_wifi_pick", on_wifi_pick);
    lv_xml_register_event_cb(NULL, "on_feed_click", on_feed_click);
    lv_xml_register_event_cb(NULL, "on_sched_add", on_sched_add);
    lv_xml_register_event_cb(NULL, "on_sched_edit", on_sched_edit);
    lv_xml_register_event_cb(NULL, "on_sched_delete", on_sched_delete);
    lv_xml_register_event_cb(NULL, "on_sched_toggle", on_sched_toggle);
    lv_xml_register_event_cb(NULL, "on_ed_cancel", on_ed_cancel);
    lv_xml_register_event_cb(NULL, "on_ed_save", on_ed_save);
    lv_xml_register_event_cb(NULL, "on_node_remove", on_node_remove);
    lv_xml_register_event_cb(NULL, "on_node_rename", on_node_rename);
    lv_xml_register_event_cb(NULL, "on_found_add", on_found_add);
    lv_xml_register_event_cb(NULL, "on_log_clear", on_log_clear);
    lv_xml_register_event_cb(NULL, "on_time_set", on_time_set);
    lv_xml_register_event_cb(NULL, "on_dialog_ok", on_dialog_ok);
    lv_xml_register_event_cb(NULL, "on_dialog_cancel", on_dialog_cancel);

    apply_theme();
    register_xml();

    root = lv_xml_create(lv_screen_active(), "main", NULL);
    home_list = lv_obj_find_by_name(root, "home_list");
    sched_list = lv_obj_find_by_name(root, "sched_list");
    sched_dd = lv_obj_find_by_name(root, "sched_dd");
    node_list = lv_obj_find_by_name(root, "node_list");
    found_list = lv_obj_find_by_name(root, "found_list");
    hist_list = lv_obj_find_by_name(root, "hist_list");
    wifi_list = lv_obj_find_by_name(root, "wifi_list");

    lv_subject_add_observer(&s_page, on_page_changed, NULL);
    lv_subject_add_observer(&s_sched_node, on_sched_node_changed, NULL);
    lv_subject_add_observer(&s_lim_cool, on_limit_changed, NULL);
    lv_subject_add_observer(&s_lim_daily, on_limit_changed, NULL);
    lv_subject_add_observer(&s_lim_gap, on_limit_changed, NULL);
    lv_subject_add_observer(&s_ed_on, on_ed_on_changed, NULL);
    lv_subject_add_observer(&s_dark, on_settings_changed, NULL);
    lv_subject_add_observer(&s_sleep_min, on_settings_changed, NULL);
    lv_subject_add_observer(&s_feed_on, on_settings_changed, NULL);
    lv_subject_add_observer(&s_feed_pulses, on_settings_changed, NULL);
    lv_subject_add_observer(&s_utc_off, on_network_changed, NULL);
    lv_subject_add_observer(&s_ntp_on, on_network_changed, NULL);
    lv_subject_add_observer(&s_ota_allow, on_ota_allow_changed, NULL);
    lv_subject_add_observer(&s_set_year, on_set_date_changed, NULL);
    lv_subject_add_observer(&s_set_month, on_set_date_changed, NULL);
    lv_subject_add_observer(&s_set_day, on_set_date_changed, NULL);

    load_time_steppers();
    loading = false;

    (void)app_take_changes();
    refresh_nodes();
    refresh_found();
    refresh_schedule();
    refresh_history();
    refresh_clock(true);

    lv_timer_create(ui_tick, UI_TICK_MS, NULL);
}

void ui_show_page(int page) { lv_subject_set_int(&s_page, page); }
