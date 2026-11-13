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
static lv_subject_t s_rate_on, s_rate_vis;
static lv_subject_t s_has_scale, s_scale_text, s_scale_reading, s_scale_known, s_scale_known_txt;
static bool scale_open; /* the open dialog is the scale one */
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
    scale_open = false;
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
        unsigned r = app_rate_mg_per_s(cal_node);
        snprintf(text + len, sizeof(text) - len, "\nNow: %u.%02u g per pulse, %u.%02u g per second.",
                 n->mg_per_pulse / 1000, n->mg_per_pulse % 1000 / 10, r / 1000, r % 1000 / 10);
    }
    lv_subject_set_int(&s_rate_vis, app_calibrated(cal_node) ? 1 : 0);
    lv_subject_set_int(&s_rate_on, app_plan_by_rate(cal_node) ? 1 : 0);
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
    lv_subject_set_int(&s_has_scale, app_scale_found(idx) ? 1 : 0);
    scale_open = false;
    show_setup();

    ui_modal_close();
    ui_modal = cal_modal = lv_xml_create(lv_layer_top(), "cal_dialog", NULL);
}

/* The plan-by-rate switch takes effect at once. */
static void on_rate_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    if (cal_node >= 0 && app_calibrated(cal_node) && (lv_subject_get_int(s) != 0) != app_plan_by_rate(cal_node)) {
        app_set_plan_by_rate(cal_node, lv_subject_get_int(s) != 0);
    }
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

/* ---- scale set-up ---- */

static const char SCALE_HELP[] =
    "This feeder has a scale under its hopper. Set it up once:\n"
    "1. Empty the hopper and press Zero.\n"
    "2. Put a known weight in the hopper, choose it below and press Set weight.";

static void show_reading(void)
{
    char buf[48];
    if (app_has_scale(cal_node)) {
        char g[16];
        int32_t mg = app_hopper_mg(cal_node);
        ui_fmt_grams(g, sizeof(g), mg > 0 ? (uint32_t)mg : 0);
        snprintf(buf, sizeof(buf), "Reading: %s%s", mg < 0 ? "-" : "", g);
    } else {
        snprintf(buf, sizeof(buf), "Not set up yet");
    }
    if (strcmp(lv_subject_get_string(&s_scale_reading), buf) != 0) {
        lv_subject_copy_string(&s_scale_reading, buf);
    }
}

void ui_calibrate_tick(void)
{
    if (active() && scale_open) {
        show_reading();
    }
}

static void on_cal_scale(lv_event_t *e)
{
    (void)e;
    int idx = cal_node;
    lv_subject_copy_string(&s_scale_text, SCALE_HELP);
    ui_modal_close();
    ui_modal = cal_modal = lv_xml_create(lv_layer_top(), "scale_dialog", NULL);
    cal_node = idx;
    scale_open = true;
    show_reading();
}

static void scale_send(uint8_t op, uint32_t known_mg)
{
    if (!app_scale_set(cal_node, op, known_mg)) {
        lv_subject_copy_string(&s_scale_text, "Another request is still in progress. Try again in a moment.");
    } else {
        lv_subject_copy_string(&s_scale_text, "Asking the feeder...");
    }
}

static void on_scale_zero(lv_event_t *e)
{
    (void)e;
    scale_send(FD_SCALE_ZERO, 0);
}

static void on_scale_span(lv_event_t *e)
{
    (void)e;
    scale_send(FD_SCALE_SPAN, (uint32_t)lv_subject_get_int(&s_scale_known) * 1000u);
}

void ui_scale_result(int idx, uint8_t result)
{
    if (!active() || !scale_open || idx != cal_node) {
        return;
    }
    if (result == FD_RES_OK) {
        lv_subject_copy_string(&s_scale_text, "Done. The reading below updates in a few seconds.\n"
                                              "After Zero, put the known weight in and press Set weight.");
    } else if (result == APP_FEED_NO_REPLY) {
        lv_subject_copy_string(&s_scale_text, "No reply from the feeder. Nothing was changed.");
    } else {
        lv_subject_copy_string(&s_scale_text,
                               "The feeder could not do that. Keep the hopper still, press Zero before "
                               "Set weight, and use a weight of at least 5 g.");
    }
}

static void on_scale_known_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d g", (int)lv_subject_get_int(s));
    lv_subject_copy_string(&s_scale_known_txt, buf);
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
    INT_SUBJECT(s_rate_on, "cal_rate", 0);
    INT_SUBJECT(s_rate_vis, "cal_rate_vis", 0);
    lv_subject_add_observer(&s_rate_on, on_rate_changed, NULL);
    INT_SUBJECT(s_on, "cal_on", 10);
    STR_SUBJECT(s_on_txt, "cal_on_txt", 16, "");
    INT_SUBJECT(s_grams, "cal_grams", 0);
    STR_SUBJECT(s_grams_txt, "cal_grams_txt", 16, "");
    STR_SUBJECT(s_title, "cal_title", 48, "");
    STR_SUBJECT(s_text, "cal_text", 220, "");
    lv_subject_add_observer(&s_on, on_on_changed, NULL);
    lv_subject_add_observer(&s_grams, on_grams_changed, NULL);

    INT_SUBJECT(s_has_scale, "cal_scale", 0);
    STR_SUBJECT(s_scale_text, "scale_text", 240, "");
    STR_SUBJECT(s_scale_reading, "scale_reading", 48, "");
    INT_SUBJECT(s_scale_known, "scale_known", 100);
    STR_SUBJECT(s_scale_known_txt, "scale_known_txt", 16, "");
    lv_subject_add_observer(&s_scale_known, on_scale_known_changed, NULL);
    lv_xml_register_event_cb(NULL, "on_cal_scale", on_cal_scale);
    lv_xml_register_event_cb(NULL, "on_scale_zero", on_scale_zero);
    lv_xml_register_event_cb(NULL, "on_scale_span", on_scale_span);

    lv_xml_register_event_cb(NULL, "on_cal_cancel", on_cal_cancel);
    lv_xml_register_event_cb(NULL, "on_cal_run", on_cal_run);
    lv_xml_register_event_cb(NULL, "on_cal_save", on_cal_save);
}
