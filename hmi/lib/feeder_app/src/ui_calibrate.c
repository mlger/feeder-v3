/*
 * Grams calibration. The feeder has no scale, so the user measures once:
 * the feeder runs a fixed number of pulses into a cup, the user weighs the
 * food and enters it. From then on portions on that feeder are shown and
 * chosen in grams (see app_mg()).
 *
 * The run is an ordinary manual feed, so the feeder's cooldown and daily
 * limit apply to it and it counts towards today's total.
 */
#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

enum { STEP_SETUP, STEP_WAIT, STEP_WEIGH };

static lv_subject_t s_step, s_on, s_on_txt, s_grams, s_grams_txt, s_title, s_text;
static int cal_node = -1;
static lv_obj_t *cal_modal;

static bool active(void) { return cal_modal != NULL && ui_modal == cal_modal; }

static void close_dialog(void)
{
    if (active()) {
        ui_modal_close();
    }
    cal_modal = NULL;
    cal_node = -1;
}

static void show_setup(void)
{
    const app_node_cfg_t *n = app_node(cal_node);
    char text[220];
    snprintf(text, sizeof(text),
             "Hold a cup under the outlet. The feeder runs %d pulses, then you weigh the food.\n"
             "Shorter pulses give finer portions, as long as each one still drops food.",
             APP_CAL_PULSES);
    if (app_calibrated(cal_node)) {
        size_t len = strlen(text);
        snprintf(text + len, sizeof(text) - len, "\nNow: %u.%02u g per pulse.", n->mg_per_pulse / 1000,
                 n->mg_per_pulse % 1000 / 10);
    }
    lv_subject_copy_string(&s_text, text);
    lv_subject_set_int(&s_step, STEP_SETUP);
}

void ui_calibrate_open(int idx)
{
    const app_node_cfg_t *n = app_node(idx);
    if (!n) {
        return;
    }
    char title[48];
    snprintf(title, sizeof(title), "Calibrate %s", n->name);
    lv_subject_copy_string(&s_title, title);
    cal_node = idx;
    lv_subject_set_int(&s_on, app_feed_on_ds(idx));
    lv_subject_set_int(&s_grams, 0);
    show_setup();

    ui_modal_close();
    ui_modal = cal_modal = lv_xml_create(lv_layer_top(), "cal_dialog", NULL);
}

static void on_cal_cancel(lv_event_t *e)
{
    (void)e;
    close_dialog();
}

static void on_cal_run(lv_event_t *e)
{
    (void)e;
    if (!app_feed_custom(cal_node, (uint8_t)lv_subject_get_int(&s_on), APP_CAL_PULSES)) {
        lv_subject_copy_string(&s_text, "Another feed request is still in progress. Try again in a moment.");
        return;
    }
    lv_subject_copy_string(&s_text, "Asking the feeder to run...");
    lv_subject_set_int(&s_step, STEP_WAIT);
}

bool ui_calibrate_feed_result(int idx, uint8_t result)
{
    if (!active() || idx != cal_node || lv_subject_get_int(&s_step) != STEP_WAIT) {
        return false;
    }
    char text[200];
    if (result == FD_RES_OK) {
        snprintf(text, sizeof(text),
                 "The feeder is running %d pulses. When it stops, weigh the food and enter the weight.\n"
                 "Enter 0 to remove the calibration.",
                 APP_CAL_PULSES);
        lv_subject_copy_string(&s_text, text);
        lv_subject_set_int(&s_step, STEP_WEIGH);
    } else {
        /* Typically the cooldown or the daily limit; say which and let the
         * user try again later. */
        snprintf(text, sizeof(text), "The feeder did not run: %s.\nNothing was changed.", app_result_text(result));
        lv_subject_copy_string(&s_text, text);
        lv_subject_set_int(&s_step, STEP_SETUP);
    }
    return true;
}

static void on_cal_save(lv_event_t *e)
{
    (void)e;
    /* The stepper counts tenths of a gram. */
    app_set_calibration(cal_node, (uint8_t)lv_subject_get_int(&s_on), (uint32_t)lv_subject_get_int(&s_grams) * 100u);
    close_dialog();
}

static void on_on_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    char buf[16];
    ui_fmt_ds(buf, sizeof(buf), lv_subject_get_int(s));
    lv_subject_copy_string(&s_on_txt, buf);
}

static void on_grams_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    char buf[16];
    ui_fmt_grams(buf, sizeof(buf), (uint32_t)lv_subject_get_int(s) * 100u);
    lv_subject_copy_string(&s_grams_txt, buf);
}

void ui_calibrate_register(void)
{
    INT_SUBJECT(s_step, "cal_step", STEP_SETUP);
    INT_SUBJECT(s_on, "cal_on", 10);
    STR_SUBJECT(s_on_txt, "cal_on_txt", 16, "");
    INT_SUBJECT(s_grams, "cal_grams", 0);
    STR_SUBJECT(s_grams_txt, "cal_grams_txt", 16, "");
    STR_SUBJECT(s_title, "cal_title", 48, "");
    STR_SUBJECT(s_text, "cal_text", 220, "");
    lv_subject_add_observer(&s_on, on_on_changed, NULL);
    lv_subject_add_observer(&s_grams, on_grams_changed, NULL);

    lv_xml_register_event_cb(NULL, "on_cal_cancel", on_cal_cancel);
    lv_xml_register_event_cb(NULL, "on_cal_run", on_cal_run);
    lv_xml_register_event_cb(NULL, "on_cal_save", on_cal_save);
}
