/*
 * HMI user interface. Layout lives in the XML files under hmi/ui/ and is
 * loaded at run time. The UI code registers what the XML refers to (fonts,
 * subjects, event callbacks), fills the dynamic lists and turns user actions
 * into calls on the application model (app.h). See ui_internal.h for how it
 * is divided between files.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_xml.h"

#define UI_TICK_MS 250

enum { PAGE_HOME, PAGE_SCHEDULE, PAGE_NODES, PAGE_HISTORY, PAGE_SETTINGS, PAGE_NETWORK };

/* ------------------------------------------------------------ subjects */

static lv_subject_t s_page, s_node_count, s_dark;
static lv_subject_t s_title, s_clock, s_date, s_alert, s_log_summary, s_about;
static lv_subject_t s_set_year, s_set_month, s_set_day, s_set_hour, s_set_min;
static lv_subject_t s_feed_on, s_feed_pulses, s_feed_on_txt, s_sleep_min, s_sleep_txt;

/* --------------------------------------------------------------- state */

static lv_obj_t *root;
static lv_obj_t *home_list, *node_list, *found_list, *hist_list;

bool ui_loading;
lv_obj_t *ui_modal;

static int  rename_node = -1;
static void (*text_done_cb)(const char *text);
static uint32_t last_minute;
static bool pending_home, pending_nodes; /* lists to refresh once no finger is down */
static bool backlight_on = true;

static dlg_action_t dlg_action;
static int dlg_arg;

/* ------------------------------------------------------------- helpers */

/* Slot index stored on each list row. +1 so that 0 means "not a row". */
void ui_row_set_index(lv_obj_t *row, int idx) { lv_obj_set_user_data(row, (void *)(intptr_t)(idx + 1)); }

/* Walk up from the widget that fired an event to the row that holds it. */
int ui_row_index_of(lv_event_t *e)
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

/* -------------------------------------------------------------- modals */

void ui_modal_close(void)
{
    if (ui_modal) {
        lv_obj_delete_async(ui_modal);
        ui_modal = NULL;
    }
}

void ui_dialog_open(dlg_action_t action, int arg, const char *title, const char *text,
                        const char *ok_text)
{
    ui_modal_close();
    dlg_action = action;
    dlg_arg = arg;
    const char *attrs[] = {
        "title", title,
        "text", text,
        "ok_text", ok_text ? ok_text : "OK",
        "no_cancel", action == DLG_NONE ? "true" : "false",
        NULL, NULL,
    };
    ui_modal = lv_xml_create(lv_layer_top(), "dialog", attrs);
}

/* ------------------------------------------------------------- refresh */

static void refresh_alert(void)
{
    char buf[64] = "";
    int offline = 0, noclock = 0, jam = -1, low = -1;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_rt_t *r = app_node_rt(i);
        if (!r) {
            continue;
        }
        if (r->low_output && jam < 0) {
            jam = i;
        }
        if (app_has_scale(i) && app_hopper_mg(i) < APP_LOW_FOOD_MG && low < 0) {
            low = i;
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
    } else if (jam >= 0) {
        /* The scale saw far less leave the hopper than the feed should give. */
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s: little food came out", app_node(jam)->name);
    } else if (low >= 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s: food is low", app_node(low)->name);
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

/*
 * The Home and Feeders lists are rebuilt only when the text they show has
 * changed, and never while a finger is down. Deleting a button between
 * press and release loses the tap, and every status message from every
 * feeder used to rebuild both lists.
 */
static uint32_t fnv(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

static bool pointer_down(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) {
            return true;
        }
    }
    return false;
}

typedef struct {
    int         idx;
    char        name[FD_NAME_LEN], state[24], last[48], next[48], today[48], food[40];
    const char *color; /* points at a string constant */
} home_row_t;

static home_row_t home_rows[APP_MAX_NODES];
static int        home_count;
static uint32_t   home_sig;
static bool       home_built;

static void home_collect(void)
{
    uint32_t now = plat_local_epoch();
    memset(home_rows, 0, sizeof(home_rows));
    home_count = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_cfg_t *n = app_node(i);
        if (!n) {
            continue;
        }
        home_row_t *h = &home_rows[home_count++];
        const app_node_rt_t *r = app_node_rt(i);
        const char *state, *color;
        node_state(i, &state, &color);
        h->idx = i;
        h->color = color;
        snprintf(h->name, sizeof(h->name), "%s", n->name);
        snprintf(h->state, sizeof(h->state), "%s", state);

        char when[32];
        if (r->have_status) {
            ui_fmt_when(when, sizeof(when), r->status.last_feed_epoch, true);
            snprintf(h->last, sizeof(h->last), "Last fed: %s", when);
            /* In grams when calibrated; this assumes today's feeds used the
             * calibrated pulse length, hence "about". */
            char amount[24];
            if (app_has_scale(i)) {
                char g[16];
                ui_fmt_grams(g, sizeof(g), r->status.fed_mg_today);
                snprintf(amount, sizeof(amount), "%s weighed", g);
            } else if (app_calibrated(i)) {
                char g[16];
                ui_fmt_grams(g, sizeof(g),
                             app_plan_by_rate(i) ? r->status.on_ms_today * app_rate_mg_per_s(i) / 1000u
                                                 : r->status.on_ms_today / (n->cal_on_ds * 100u) * n->mg_per_pulse);
                snprintf(amount, sizeof(amount), "about %s", g);
            } else {
                snprintf(amount, sizeof(amount), "%u.%u s", (unsigned)(r->status.on_ms_today / 1000),
                         (unsigned)(r->status.on_ms_today % 1000 / 100));
            }
            snprintf(h->today, sizeof(h->today), "Today: %u feed%s, %s", r->status.feeds_today,
                     r->status.feeds_today == 1 ? "" : "s", amount);
        } else {
            snprintf(h->last, sizeof(h->last), "Last fed: unknown");
            snprintf(h->today, sizeof(h->today), "Today: unknown");
        }
        uint32_t nx = plat_time_valid() ? app_next_feed(i, now) : 0;
        if (nx) {
            ui_fmt_when(when, sizeof(when), nx, true);
            snprintf(h->next, sizeof(h->next), "Next: %s", when);
        } else {
            snprintf(h->next, sizeof(h->next), n->count ? "Next: nothing scheduled" : "Next: no schedule");
        }

        /* Only a feeder with a scale knows how much food is left. */
        if (app_has_scale(i)) {
            char g[16];
            int32_t left = app_hopper_mg(i);
            ui_fmt_grams(g, sizeof(g), left > 0 ? (uint32_t)left : 0);
            snprintf(h->food, sizeof(h->food), "Food left: %s", g);
        }
    }
}

/* Returns true if the list was rebuilt. */
static bool refresh_home(void)
{
    home_collect();
    uint32_t sig = 2166136261u;
    for (int i = 0; i < home_count; i++) {
        const home_row_t *h = &home_rows[i];
        sig = fnv(sig, h->name, sizeof(h->name) + sizeof(h->state) + sizeof(h->last) + sizeof(h->next) +
                                    sizeof(h->today) + sizeof(h->food));
        sig = fnv(sig, h->color, strlen(h->color));
        sig = fnv(sig, &h->idx, sizeof(h->idx));
    }
    sig = fnv(sig, &home_count, sizeof(home_count));
    if (home_built && sig == home_sig) {
        return false;
    }
    home_built = true;
    home_sig = sig;

    lv_obj_clean(home_list);
    for (int i = 0; i < home_count; i++) {
        const home_row_t *h = &home_rows[i];
        const char *attrs[] = {
            "name", h->name, "state", h->state, "state_color", h->color,
            "last", h->last, "next", h->next, "today", h->today, "food", h->food, NULL, NULL,
        };
        lv_obj_t *card = lv_xml_create(home_list, "tank_card", attrs);
        if (!h->food[0]) {
            lv_obj_add_flag(lv_obj_find_by_name(card, "food"), LV_OBJ_FLAG_HIDDEN);
        }
        ui_row_set_index(card, h->idx);
    }
    return true;
}

typedef struct {
    int         idx;
    char        name[FD_NAME_LEN], detail[96], state[24];
    const char *color;
} node_row_t;

static node_row_t node_rows[APP_MAX_NODES];
static uint32_t   nodes_sig;
static bool       nodes_built;

static bool refresh_nodes(void)
{
    memset(node_rows, 0, sizeof(node_rows));
    int count = 0;
    for (int i = 0; i < APP_MAX_NODES; i++) {
        const app_node_cfg_t *n = app_node(i);
        if (!n) {
            continue;
        }
        node_row_t *nr = &node_rows[count++];
        const app_node_rt_t *r = app_node_rt(i);
        const char *state, *color;
        node_state(i, &state, &color);
        nr->idx = i;
        nr->color = color;
        snprintf(nr->name, sizeof(nr->name), "%s", n->name);
        snprintf(nr->state, sizeof(nr->state), "%s", state);
        int len = snprintf(nr->detail, sizeof(nr->detail), "%02X:%02X:%02X:%02X:%02X:%02X", n->mac[0], n->mac[1],
                           n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
        if (r->have_status) {
            len += snprintf(nr->detail + len, sizeof(nr->detail) - (size_t)len, "   firmware %u.%u",
                            r->status.fw_major, r->status.fw_minor);
        }
        if (app_calibrated(i) && len < (int)sizeof(nr->detail)) {
            if (app_plan_by_rate(i)) {
                unsigned r = app_rate_mg_per_s(i);
                snprintf(nr->detail + len, sizeof(nr->detail) - (size_t)len, "   %u.%02u g/s", r / 1000, r % 1000 / 10);
            } else {
                snprintf(nr->detail + len, sizeof(nr->detail) - (size_t)len, "   %u.%02u g per pulse",
                         n->mg_per_pulse / 1000, n->mg_per_pulse % 1000 / 10);
            }
        }
    }
    lv_subject_set_int(&s_node_count, count);

    uint32_t sig = fnv(2166136261u, &count, sizeof(count));
    for (int i = 0; i < count; i++) {
        const node_row_t *nr = &node_rows[i];
        sig = fnv(sig, nr->name, sizeof(nr->name) + sizeof(nr->detail) + sizeof(nr->state));
        sig = fnv(sig, nr->color, strlen(nr->color));
        sig = fnv(sig, &nr->idx, sizeof(nr->idx));
    }
    if (nodes_built && sig == nodes_sig) {
        return false;
    }
    nodes_built = true;
    nodes_sig = sig;

    lv_obj_clean(node_list);
    for (int i = 0; i < count; i++) {
        const node_row_t *nr = &node_rows[i];
        const char *attrs[] = {
            "name", nr->name, "detail", nr->detail, "state", nr->state, "state_color", nr->color, NULL, NULL,
        };
        lv_obj_t *row = lv_xml_create(node_list, "node_row", attrs);
        ui_row_set_index(row, nr->idx);
    }
    if (count == 0) {
        lv_obj_t *l = lv_label_create(node_list);
        lv_label_set_text(l, "None yet.");
    }
    return true;
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
        ui_row_set_index(row, i);
    }
    if (count == 0) {
        lv_obj_t *l = lv_label_create(found_list);
        lv_label_set_text(l, "Searching...");
    }
}

void ui_history_refresh(void)
{
    lv_obj_clean(hist_list);
    int count = app_log_count();
    char sum[96];
    int len = snprintf(sum, sizeof(sum), count ? "Last %d events, newest first" : "No feeds recorded yet", count);
    snprintf(sum + len, sizeof(sum) - (size_t)len, "%s",
             plat_sd_ready() ? ".  SD card: logging to " APP_SD_LOG_FILE : ".  No SD card");
    lv_subject_copy_string(&s_log_summary, sum);

    for (int i = 0; i < count; i++) {
        const app_log_entry_t *e = app_log_get(i);
        const app_node_cfg_t *n = app_node(e->node);
        char when[32], what[32], tank[FD_NAME_LEN + 4];
        ui_fmt_when(when, sizeof(when), e->epoch, false);
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
            char amount[32];
            uint32_t mg = e->pulses ? app_mg(e->node, (uint8_t)(e->on_ms / e->pulses / 100u), e->pulses) : 0;
            if (e->measured_mg != FD_NOT_MEASURED) {
                ui_fmt_grams(amount, sizeof(amount), e->measured_mg);
                snprintf(result, sizeof(result), "Fed %s, weighed", amount);
            } else if (mg) {
                ui_fmt_grams(amount, sizeof(amount), mg);
                snprintf(result, sizeof(result), "Fed about %s", amount);
            } else {
                snprintf(result, sizeof(result), "Fed %u.%u s", e->on_ms / 1000, e->on_ms % 1000 / 100);
            }
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
        snprintf(buf, sizeof(buf), "%s %d %s", ui_dow3[c.dow], c.day, ui_mon3[c.month - 1]);
        lv_subject_copy_string(&s_date, buf);
    }
    refresh_alert();
    pending_home = true; /* "today", "tomorrow" and the next feed depend on the time */
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
    if (p == PAGE_SETTINGS && !ui_loading) {
        load_time_steppers();
    }
}

static void on_settings_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    char buf[16];
    ui_fmt_ds(buf, sizeof(buf), lv_subject_get_int(&s_feed_on));
    lv_subject_copy_string(&s_feed_on_txt, buf);
    int sl = lv_subject_get_int(&s_sleep_min);
    snprintf(buf, sizeof(buf), sl ? "%d min" : "Never", sl);
    lv_subject_copy_string(&s_sleep_txt, buf);
    if (ui_loading) {
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

static void on_feed_click(lv_event_t *e) { ui_feed_open(ui_row_index_of(e)); }

static void on_node_calibrate(lv_event_t *e) { ui_calibrate_open(ui_row_index_of(e)); }

static void on_node_remove(lv_event_t *e)
{
    int idx = ui_row_index_of(e);
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    char title[40];
    snprintf(title, sizeof(title), "Remove %s?", n->name);
    ui_dialog_open(DLG_REMOVE, idx, title,
                "Its schedule is deleted from this controller and the feeder stops feeding on its own "
                "once it receives the command.",
                "Remove");
}

static void text_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && ui_modal && text_done_cb) {
        text_done_cb(lv_textarea_get_text(lv_obj_find_by_name(ui_modal, "text_ta")));
    }
    ui_modal_close();
}

void ui_text_open(const char *label, const char *initial, uint32_t max_len, bool password,
                  void (*done)(const char *text))
{
    text_done_cb = done;
    ui_modal_close();
    const char *attrs[] = { "label", label, NULL, NULL };
    ui_modal = lv_xml_create(lv_layer_top(), "text_dialog", attrs);
    lv_obj_t *ta = lv_obj_find_by_name(ui_modal, "text_ta");
    lv_obj_t *kb = lv_obj_find_by_name(ui_modal, "text_kb");
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, initial);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(kb, text_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, text_done, LV_EVENT_CANCEL, NULL);
}

static void rename_entered(const char *text) { app_rename_node(rename_node, text); }

static void on_node_rename(lv_event_t *e)
{
    int idx = ui_row_index_of(e);
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    rename_node = idx;
    ui_text_open("Name", n->name, FD_NAME_LEN - 1, false, rename_entered);
}

static void on_found_add(lv_event_t *e)
{
    const app_found_t *f = app_found(ui_row_index_of(e));
    if (!f) {
        return;
    }
    uint8_t mac[6];
    memcpy(mac, f->mac, 6);
    if (app_adopt(mac) < 0) {
        ui_dialog_open(DLG_NONE, 0, "No room", "This controller already manages the maximum number of feeders.",
                    NULL);
    }
}

static void on_log_clear(lv_event_t *e)
{
    (void)e;
    if (app_log_count()) {
        ui_dialog_open(DLG_CLEAR_LOG, 0, "Clear history", "Delete all recorded feed events?", "Clear");
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
    refresh_home();
    ui_dialog_open(DLG_NONE, 0, "Clock set", "The feeders pick up the new time within a few seconds.", NULL);
}

static void on_dialog_cancel(lv_event_t *e)
{
    (void)e;
    ui_modal_close();
}

static void on_dialog_ok(lv_event_t *e)
{
    (void)e;
    dlg_action_t action = dlg_action;
    int arg = dlg_arg;
    ui_modal_close();
    switch (action) {
    case DLG_REMOVE:
        app_remove_node(arg);
        break;
    case DLG_DELETE_ENTRY:
        app_entry_delete(ui_sched_node_idx(), arg);
        break;
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

    ui_schedule_tick();

    uint32_t ch = app_take_changes();
    if (ch & APP_CH_NODES) {
        pending_nodes = pending_home = true;
        refresh_alert();
    }
    if (ch & APP_CH_FOUND) {
        refresh_found();
    }
    if (ch & APP_CH_SCHEDULE) {
        ui_schedule_refresh();
        pending_home = true;
    }
    if (!pointer_down()) {
        if (pending_nodes) {
            pending_nodes = false;
            refresh_nodes();
        }
        if (pending_home) {
            pending_home = false;
            refresh_home();
        }
    }
    if (ch & APP_CH_LOG) {
        ui_history_refresh();
    }
    if ((ch & APP_CH_FEED) && !ui_calibrate_feed_result(app_feed_last_node(), app_feed_last_result())) {
        const app_node_cfg_t *n = app_node(app_feed_last_node());
        uint8_t res = app_feed_last_result();
        char text[80];
        snprintf(text, sizeof(text), "%s: %s", n ? n->name : "Feeder",
                 res == FD_RES_OK ? "feeding started" : app_result_text(res));
        ui_dialog_open(DLG_NONE, 0, res == FD_RES_OK ? "Feeding" : "Not fed", text, NULL);
    }
    if (ch & APP_CH_SCALE) {
        ui_scale_result(app_scale_last_node(), app_scale_last_result());
    }
    ui_calibrate_tick();
    if (ch & APP_CH_OTA) {
        const app_node_cfg_t *n = app_node(app_ota_node());
        char text[96];
        if (app_ota_state() == APP_OTA_DONE || app_ota_state() == APP_OTA_FAILED) {
            snprintf(text, sizeof(text), "%s: %s", n ? n->name : "Feeder", app_ota_message());
            ui_dialog_open(DLG_NONE, 0, app_ota_state() == APP_OTA_DONE ? "Feeder updated" : "Update failed",
                        text, NULL);
        }
    }
    ui_network_refresh();
    ui_network_publish_status();
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

/*
 * LVGL 9.4.0 bug: lv_xml_register_component_from_data("globals") copies the
 * scope into its parser state but never initialises the rest of that state
 * (every other component goes through lv_xml_parser_state_init). It then
 * reads whatever the stack held. On a PC that is usually harmless; on the
 * ESP32-S3 it made the parser think it was inside an <api> section and
 * crash on a NULL name. Zeroing the stack just below this function's frame
 * first gives it the all-zero state it expects.
 */
static void __attribute__((noinline)) scrub_stack(void)
{
    volatile uint8_t pad[2048];
    for (size_t i = 0; i < sizeof(pad); i++) {
        pad[i] = 0;
    }
}

static void register_xml(void)
{
    static const char *const order[] = {
        "globals", "row", "col", "nav_button", "stepper", "tank_card", "sched_row", "node_row",
        "found_row", "hist_row", "page_home", "page_schedule", "page_nodes", "page_history",
        "page_settings", "wifi_row", "page_network", "sched_editor", "dialog", "text_dialog", "cal_dialog",
        "scale_dialog", "feed_dialog",
        "setup_dialog", "main",
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const char *xml = ui_xml_get(order[i]);
        if (i == 0) {
            scrub_stack(); /* "globals" is first: see scrub_stack() */
        }
        if (!xml || lv_xml_register_component_from_data(order[i], xml) != LV_RESULT_OK) {
            LV_LOG_ERROR("UI component '%s' failed to load", order[i]);
        }
    }
}

void ui_init(void)
{
    const app_settings_t *st = app_settings();
    ui_loading = true;

    lv_xml_register_font(NULL, "font_s", &lv_font_montserrat_14);
    lv_xml_register_font(NULL, "font_m", &lv_font_montserrat_16);
    lv_xml_register_font(NULL, "font_l", &lv_font_montserrat_20);
    lv_xml_register_font(NULL, "font_xl", &lv_font_montserrat_28);

    INT_SUBJECT(s_page, "page", PAGE_HOME);
    INT_SUBJECT(s_node_count, "node_count", 0);
    INT_SUBJECT(s_dark, "dark", st->dark);
    STR_SUBJECT(s_title, "title", 16, "Home");
    STR_SUBJECT(s_clock, "clock", 8, "--:--");
    STR_SUBJECT(s_date, "date", 16, "");
    STR_SUBJECT(s_alert, "alert", 64, "");
    STR_SUBJECT(s_log_summary, "log_summary", 96, "");
    STR_SUBJECT(s_about, "about", 64, "AIO-Feeder controller " APP_VERSION);

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

    lv_xml_register_event_cb(NULL, "on_feed_click", on_feed_click);
    lv_xml_register_event_cb(NULL, "on_node_remove", on_node_remove);
    lv_xml_register_event_cb(NULL, "on_node_calibrate", on_node_calibrate);
    lv_xml_register_event_cb(NULL, "on_node_rename", on_node_rename);
    lv_xml_register_event_cb(NULL, "on_found_add", on_found_add);
    lv_xml_register_event_cb(NULL, "on_log_clear", on_log_clear);
    lv_xml_register_event_cb(NULL, "on_time_set", on_time_set);
    lv_xml_register_event_cb(NULL, "on_dialog_ok", on_dialog_ok);
    lv_xml_register_event_cb(NULL, "on_dialog_cancel", on_dialog_cancel);

    ui_schedule_register();
    ui_calibrate_register();
    ui_feed_register();
    ui_network_register();

    apply_theme();
    register_xml();

    root = lv_xml_create(lv_screen_active(), "main", NULL);
    home_list = lv_obj_find_by_name(root, "home_list");
    node_list = lv_obj_find_by_name(root, "node_list");
    found_list = lv_obj_find_by_name(root, "found_list");
    hist_list = lv_obj_find_by_name(root, "hist_list");
    ui_schedule_attach(root);
    ui_network_attach(root);

    lv_subject_add_observer(&s_page, on_page_changed, NULL);
    lv_subject_add_observer(&s_dark, on_settings_changed, NULL);
    lv_subject_add_observer(&s_sleep_min, on_settings_changed, NULL);
    lv_subject_add_observer(&s_feed_on, on_settings_changed, NULL);
    lv_subject_add_observer(&s_feed_pulses, on_settings_changed, NULL);
    lv_subject_add_observer(&s_set_year, on_set_date_changed, NULL);
    lv_subject_add_observer(&s_set_month, on_set_date_changed, NULL);
    lv_subject_add_observer(&s_set_day, on_set_date_changed, NULL);

    load_time_steppers();
    ui_loading = false;

    (void)app_take_changes();
    refresh_nodes();
    refresh_found();
    ui_schedule_refresh();
    ui_history_refresh();
    refresh_clock(true);
    refresh_home();

    lv_timer_create(ui_tick, UI_TICK_MS, NULL);
}

void ui_clock_redraw(void) { last_minute = 0; }

void ui_show_page(int page) { lv_subject_set_int(&s_page, page); }
