/*
 * The Feed now dialog. On a calibrated feeder the amount is chosen in grams
 * (and turned into the nearest whole number of pulses, shown underneath);
 * otherwise in pulses of the length set in Settings.
 */
#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

static lv_subject_t s_cal, s_tenths, s_tenths_txt, s_pulses, s_title, s_note;
static int feed_node = -1;
static lv_obj_t *feed_modal;
/* The amount last used, so the dialog opens where the user left it. */
static int last_tenths[APP_MAX_NODES];

static void show_note(void)
{
    char buf[80] = "";
    int idx = feed_node;
    if (app_calibrated(idx)) {
        uint32_t asked = (uint32_t)lv_subject_get_int(&s_tenths) * 100u;
        ui_fmt_plan_note(buf, sizeof(buf), idx, asked, "about");
    } else {
        uint8_t on = app_settings()->feed_on_ds;
        ui_fmt_portion(buf, sizeof(buf), on, (uint8_t)lv_subject_get_int(&s_pulses));
    }
    lv_subject_copy_string(&s_note, buf);
}

static void on_tenths_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    char buf[16];
    ui_fmt_grams(buf, sizeof(buf), (uint32_t)lv_subject_get_int(s) * 100u);
    lv_subject_copy_string(&s_tenths_txt, buf);
    if (feed_node >= 0) {
        show_note();
    }
}

static void on_pulses_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    if (feed_node >= 0) {
        show_note();
    }
}

void ui_feed_open(int idx)
{
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    char title[48];
    snprintf(title, sizeof(title), "Feed %s now", n->name);
    lv_subject_copy_string(&s_title, title);
    feed_node = idx;
    bool cal = app_calibrated(idx);
    lv_subject_set_int(&s_cal, cal ? 1 : 0);
    lv_subject_set_int(&s_pulses, app_settings()->feed_pulses);
    int tenths = last_tenths[idx];
    if (tenths <= 0) {
        tenths = cal ? (int)(((uint32_t)n->mg_per_pulse * app_settings()->feed_pulses + 50u) / 100u) : 10;
    }
    lv_subject_set_int(&s_tenths, tenths < 1 ? 1 : tenths > 600 ? 600 : tenths);
    lv_subject_notify(&s_tenths);
    show_note();

    ui_modal_close();
    ui_modal = feed_modal = lv_xml_create(lv_layer_top(), "feed_dialog", NULL);
}

static void close_dialog(void)
{
    if (feed_modal && ui_modal == feed_modal) {
        ui_modal_close();
    }
    feed_modal = NULL;
    feed_node = -1;
}

static void on_fd_cancel(lv_event_t *e)
{
    (void)e;
    close_dialog();
}

static void on_fd_feed(lv_event_t *e)
{
    (void)e;
    int idx = feed_node;
    bool ok;
    if (app_calibrated(idx)) {
        last_tenths[idx] = lv_subject_get_int(&s_tenths);
        ok = app_feed_grams(idx, (uint32_t)last_tenths[idx] * 100u);
    } else {
        ok = app_feed_custom(idx, app_settings()->feed_on_ds, (uint8_t)lv_subject_get_int(&s_pulses));
    }
    close_dialog();
    if (!ok) {
        ui_dialog_open(DLG_NONE, 0, "Busy", "Another feed request is still in progress.", NULL);
    }
}

void ui_feed_register(void)
{
    INT_SUBJECT(s_cal, "fd_cal", 0);
    INT_SUBJECT(s_tenths, "fd_tenths", 10);
    STR_SUBJECT(s_tenths_txt, "fd_tenths_txt", 16, "");
    INT_SUBJECT(s_pulses, "fd_pulses", 1);
    STR_SUBJECT(s_title, "fd_title", 48, "");
    STR_SUBJECT(s_note, "fd_note", 80, "");
    lv_subject_add_observer(&s_tenths, on_tenths_changed, NULL);
    lv_subject_add_observer(&s_pulses, on_pulses_changed, NULL);
    lv_xml_register_event_cb(NULL, "on_fd_cancel", on_fd_cancel);
    lv_xml_register_event_cb(NULL, "on_fd_feed", on_fd_feed);
}
