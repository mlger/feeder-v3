/* The Schedule page: feeding times per feeder, the editor and the limits. */
#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#define LIMITS_SETTLE_MS 1200

static lv_subject_t s_sched_node;
static lv_subject_t s_lim_cool, s_lim_daily, s_lim_gap;
static lv_subject_t s_lim_cool_txt, s_lim_daily_txt, s_lim_gap_txt;
static lv_subject_t s_ed_hour, s_ed_min, s_ed_on, s_ed_pulses, s_ed_on_txt, s_ed_title;
static lv_subject_t s_ed_day[7];

static lv_obj_t *sched_list, *sched_dd;
static int  dd_map[APP_MAX_NODES]; /* dropdown position -> node slot */
static int  dd_count;
static uint32_t limits_dirty_ms;   /* 0 = clean */
static int  limits_dirty_node = -1;
static int  ed_entry = -1;         /* entry being edited, -1 = new */

int ui_sched_node_idx(void)
{
    int pos = lv_subject_get_int(&s_sched_node);
    return pos >= 0 && pos < dd_count ? dd_map[pos] : -1;
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
    ui_fmt_ds(buf, sizeof(buf), lv_subject_get_int(&s_lim_gap));
    lv_subject_copy_string(&s_lim_gap_txt, buf);
}

void ui_schedule_refresh(void)
{
    /* Rebuild the feeder dropdown, keeping the same feeder selected. */
    int prev = ui_sched_node_idx();
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
    ui_loading = true;
    lv_dropdown_set_options(sched_dd, opts);
    lv_subject_set_int(&s_sched_node, sel);
    lv_dropdown_set_selected(sched_dd, (uint32_t)sel);

    lv_obj_clean(sched_list);
    int idx = ui_sched_node_idx();
    const app_node_cfg_t *n = app_node(idx);
    if (n) {
        for (uint8_t i = 0; i < n->count; i++) {
            const fd_sched_entry_t *e = &n->entries[i];
            char time[8], days[40], portion[32];
            snprintf(time, sizeof(time), "%02d:%02d", e->hour, e->minute);
            ui_fmt_days(days, sizeof(days), e->dow_mask);
            ui_fmt_portion(portion, sizeof(portion), e->on_ds, e->pulses);
            const char *attrs[] = {
                "time", time, "days", days, "portion", portion,
                "enabled", (e->dow_mask & FD_ENTRY_ENABLED) ? "true" : "false", NULL, NULL,
            };
            lv_obj_t *row = lv_xml_create(sched_list, "sched_row", attrs);
            ui_row_set_index(row, i);
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
    ui_loading = false;
}

static void on_sched_node_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    if (!ui_loading) {
        limits_dirty_ms = 0;
        ui_schedule_refresh();
    }
}

static void on_limit_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    limits_to_text();
    if (ui_loading) {
        return;
    }
    limits_dirty_node = ui_sched_node_idx();
    limits_dirty_ms = plat_millis() ? plat_millis() : 1;
}

static void on_ed_on_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    char buf[16];
    ui_fmt_ds(buf, sizeof(buf), lv_subject_get_int(s));
    lv_subject_copy_string(&s_ed_on_txt, buf);
}

static void editor_open(int entry)
{
    int idx = ui_sched_node_idx();
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

    ui_modal_close();
    ui_modal = lv_xml_create(lv_layer_top(), "sched_editor", NULL);

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
    lv_obj_t *rh = lv_obj_find_by_name(ui_modal, "ed_hour");
    lv_obj_t *rm = lv_obj_find_by_name(ui_modal, "ed_min");
    lv_roller_set_options(rh, hours, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_options(rm, mins, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(rh, e.hour, LV_ANIM_OFF);
    lv_roller_set_selected(rm, e.minute, LV_ANIM_OFF);
}

static void on_sched_add(lv_event_t *e)
{
    (void)e;
    const app_node_cfg_t *n = app_node(ui_sched_node_idx());
    if (!n) {
        return;
    }
    if (n->count >= FD_MAX_ENTRIES) {
        ui_dialog_open(DLG_NONE, 0, "Schedule full", "This feeder already has the maximum number of feeding times.",
                    NULL);
        return;
    }
    editor_open(-1);
}

static void on_sched_edit(lv_event_t *e) { editor_open(ui_row_index_of(e)); }

static void on_sched_delete(lv_event_t *e)
{
    int entry = ui_row_index_of(e);
    const app_node_cfg_t *n = app_node(ui_sched_node_idx());
    if (!n || entry < 0 || entry >= n->count) {
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Remove the %02d:%02d feeding time?", n->entries[entry].hour,
             n->entries[entry].minute);
    ui_dialog_open(DLG_DELETE_ENTRY, entry, "Delete feeding time", text, "Delete");
}

static void on_sched_toggle(lv_event_t *e)
{
    int entry = ui_row_index_of(e);
    int idx = ui_sched_node_idx();
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
    ui_modal_close();
}

static void on_ed_save(lv_event_t *e)
{
    (void)e;
    int idx = ui_sched_node_idx();
    app_node_cfg_t *n = app_node(idx);
    if (!n) {
        ui_modal_close();
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
    ui_modal_close();
    app_schedule_changed(idx);
}

/* Push edited limits once the user has stopped tapping. */
void ui_schedule_tick(void)
{
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
}

void ui_schedule_register(void)
{
    INT_SUBJECT(s_sched_node, "sched_node", 0);
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

    lv_xml_register_event_cb(NULL, "on_sched_add", on_sched_add);
    lv_xml_register_event_cb(NULL, "on_sched_edit", on_sched_edit);
    lv_xml_register_event_cb(NULL, "on_sched_delete", on_sched_delete);
    lv_xml_register_event_cb(NULL, "on_sched_toggle", on_sched_toggle);
    lv_xml_register_event_cb(NULL, "on_ed_cancel", on_ed_cancel);
    lv_xml_register_event_cb(NULL, "on_ed_save", on_ed_save);
}

void ui_schedule_attach(lv_obj_t *root)
{
    sched_list = lv_obj_find_by_name(root, "sched_list");
    sched_dd = lv_obj_find_by_name(root, "sched_dd");
    lv_subject_add_observer(&s_sched_node, on_sched_node_changed, NULL);
    lv_subject_add_observer(&s_lim_cool, on_limit_changed, NULL);
    lv_subject_add_observer(&s_lim_daily, on_limit_changed, NULL);
    lv_subject_add_observer(&s_lim_gap, on_limit_changed, NULL);
    lv_subject_add_observer(&s_ed_on, on_ed_on_changed, NULL);
}
