/*
 * Host simulator for the HMI application.
 *
 * Runs the real app + UI code on a headless 800x480 LVGL display against the
 * simulated world in sim_world.c. It walks through a scripted session,
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
#include "sim_world.h"
#include "ui.h"

void ui_fmt_grams(char *buf, size_t n, uint32_t mg); /* from ui_internal.h */

#define W 800
#define SETUP_TIMEOUT_TEST_MS (5 * 60000 + 2000)
#define H 480

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
        lv_tick_inc(5);
        sim_world_step(now_ms + 5);
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
    setvbuf(stdout, NULL, _IOLBF, 0); /* keep the log if a check crashes */

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
    run(7000); /* a feeder visits each channel once every ~5 s */
    CHECK(app_found(0) && app_found(1), "both feeders are discovered");
    shot("03_feeders_discovered");
    click("Add", 0);
    click("Add", 0);
    run(30000);
    CHECK(app_node_count() == 2, "both feeders adopted");
    CHECK(app_node_online(0) && app_node_online(1), "both feeders online");
    CHECK(fnodes[0].n.time_valid && fnodes[1].n.time_valid, "feeders received the time");
    shot("04_feeders_paired");

    printf("Rename the first feeder\n");
    click("Rename", 0);
    shot("05_rename");
    app_rename_node(0, "Goldfish");
    lv_obj_send_event(lv_obj_find_by_name(lv_layer_top(), "text_kb"), LV_EVENT_CANCEL, NULL);
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
    CHECK(fnodes[0].n.cfg.count == 2 && fnodes[0].n.cfg.sched_rev == app_node(0)->rev, "feeder has the new schedule");
    CHECK(fnodes[0].n.cfg.entries[1].hour == 18 && fnodes[0].n.cfg.entries[1].dow_mask == (FD_ENTRY_ENABLED | 0x3E),
          "weekday 18:30 entry arrived intact");
    shot("07_schedule");

    printf("Wait for the 08:00 scheduled feed\n");
    ui_show_page(0);
    run(90000);
    CHECK(fnodes[0].relay_pulses == 1, "scheduled feed ran once");
    CHECK(app_log_count() == 1 && app_log_get(0)->result == FD_RES_OK && app_log_get(0)->source == 0,
          "scheduled feed is in the history");
    shot("08_home");

    printf("Manual feed on the second feeder\n");
    click("Feed now", 1);
    shot("09_feed_confirm");
    click("Feed", 0);
    run(1000);
    CHECK(fnodes[1].relay_pulses == 1, "manual feed ran");
    shot("10_feed_result");
    click("OK", 0);

    printf("Second manual feed is refused by the cooldown\n");
    click("Feed now", 1);
    click("Feed", 0);
    run(1000);
    CHECK(fnodes[1].relay_pulses == 1, "feeder did not feed again");
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

    printf("SD card log\n");
    CHECK(sd_lines == app_log_count(), "every history event was written to the SD card");
    CHECK(strcmp(sd_last_header, "time,feeder,source,result,seconds,pulses,grams,grams_are") == 0, "CSV header is right");
    CHECK(strncmp(sd_last_line, "2026-11-09 08:00:", 17) == 0 &&
          strcmp(sd_last_line + 19, ",\"Tank 2\",manual,fed,1.0,1,,") == 0, "CSV line is as expected (no grams yet)");
    printf("    last line: %s\n", sd_last_line);
    CHECK(strstr(lv_subject_get_string(lv_xml_get_subject(NULL, "log_summary")), "SD card: logging") != NULL,
          "History shows that the SD card is logging");
    sd_inserted = false;
    run(1000);
    CHECK(strstr(lv_subject_get_string(lv_xml_get_subject(NULL, "log_summary")), "No SD card") != NULL,
          "History shows when the card is removed");
    sd_inserted = true;
    run(1000);

    printf("Wi-Fi set-up from a phone\n");
    ui_show_page(5);
    run(500);
    click("Use phone", 0);
    CHECK(setup_ap_open, "set-up access point opens");
    shot("14a_phone_setup");
    click("Cancel", 0);
    CHECK(!setup_ap_open, "Cancel closes the access point");
    click("Use phone", 0);
    run(2000);
    plat_wifi_connect("HomeNetwork", "wrong"); /* the phone submits the form */
    setup_done = true;
    run(1000);
    CHECK(setup_ap_open, "access point stays up briefly so the phone gets its answer");
    run(4000);
    CHECK(!setup_ap_open, "access point closes after the details were submitted");
    CHECK(plat_wifi_state() == PLAT_WIFI_FAILED, "the submitted details were used");
    click("Use phone", 0);
    run(SETUP_TIMEOUT_TEST_MS);
    CHECK(!setup_ap_open, "access point closes by itself after 5 minutes");
    lv_display_trigger_activity(NULL);
    plat_wifi_forget();
    run(500);

    printf("Wi-Fi: wrong password, then the right one\n");
    fnodes[1].powered = true;
    ui_show_page(5);
    run(500);
    shot("14_network_start");
    click("Scan", 0);
    run(2500);
    shot("15_network_scan");
    click("Connect", 0);
    shot("16_wifi_password");
    lv_textarea_set_text(lv_obj_find_by_name(lv_layer_top(), "text_ta"), "wrong");
    lv_obj_send_event(lv_obj_find_by_name(lv_layer_top(), "text_kb"), LV_EVENT_READY, NULL);
    run(4000);
    CHECK(plat_wifi_state() == PLAT_WIFI_FAILED, "wrong password is reported as not connected");
    click("Scan", 0);
    run(2500);
    click("Connect", 0);
    lv_textarea_set_text(lv_obj_find_by_name(lv_layer_top(), "text_ta"), "fishfood");
    lv_obj_send_event(lv_obj_find_by_name(lv_layer_top(), "text_kb"), LV_EVENT_READY, NULL);
    run(4000);
    CHECK(plat_wifi_state() == PLAT_WIFI_CONNECTED, "connects with the right password");

    printf("Internet time with a +08:00 offset\n");
    uint32_t before = plat_local_epoch();
    CHECK(!plat_ntp_synced(), "clock is not touched while internet time is off");
    lv_subject_set_int(lv_xml_get_subject(NULL, "utc_off"), 480);
    lv_subject_set_int(lv_xml_get_subject(NULL, "ntp_on"), 1);
    run(3000);
    CHECK(plat_ntp_synced(), "internet time synchronised");
    {
        fd_civil_t c;
        fd_civil_from_epoch(plat_local_epoch(), &c);
        /* True UTC is 00:10 plus the elapsed run time; local must be 8 h ahead. */
        uint32_t want = TRUE_UTC_AT_START + now_ms / 1000 + 8 * 3600;
        CHECK(plat_local_epoch() - want <= 2 || want - plat_local_epoch() <= 2, "local time is UTC + 8 h");
        CHECK(c.day == 9 && c.hour >= 8 && c.hour <= 9, "the date and hour look right");
        (void)before;
    }
    CHECK(sim_hmi_channel == 6, "joining Wi-Fi moved the controller to the router's channel");
    run(75000); /* feeders notice the silence, search the channels and find it */
    CHECK(app_node_online(0) && app_node_online(1), "feeders found the controller on the new channel");
    CHECK(fnodes[0].channel == 6 && fnodes[0].flash_cfg.channel == 6, "and remembered that channel");
    CHECK(abs(sim_node_clock_error(0)) <= 1 && abs(sim_node_clock_error(1)) <= 1, "feeders followed the new time");
    shot("17_network_connected");

    printf("Feeder firmware update\n");
    CHECK(app_node_ota_start(0) != NULL, "refused while no firmware is uploaded");
    node_fw_size = 312000;
    lv_subject_set_int(lv_xml_get_subject(NULL, "ota_allow"), 1);
    run(500);
    CHECK(updates_allowed, "uploads are allowed after the switch");
    node_ota_request = 0; /* "Update" pressed on the web page */
    run(1000);
    CHECK(app_ota_state() == APP_OTA_RUNNING && ota_ap_open, "update started, access point open");
    shot("18_feeder_updating");
    run(25000);
    CHECK(app_ota_state() == APP_OTA_DONE && !ota_ap_open, "update finished, access point closed");
    CHECK(fnodes[0].fw_minor == 2 && app_node_rt(0)->status.fw_minor == 2, "feeder reports the new version");
    CHECK(fnodes[0].n.cfg.count == 2 && fnodes[0].relay_pulses == 1, "schedule and history kept");
    shot("19_feeder_updated");
    click("OK", 0);

    fnodes[1].ota_broken = true;
    node_ota_request = 1;
    run(30000);
    CHECK(app_ota_state() == APP_OTA_FAILED && !ota_ap_open, "a failed download is reported as failed");
    CHECK(fnodes[1].fw_minor == 1, "the feeder kept its old firmware");
    click("OK", 0);
    CHECK(strstr(last_status, "\"ota\":\"failed\"") && strstr(last_status, "\"name\":\"Goldfish\""),
          "web status carries the feeders and the update state");

    run(10 * 60000);
    CHECK(!updates_allowed, "uploads are refused again after 10 minutes");
    lv_display_trigger_activity(NULL);
    run(500);

    printf("Grams calibration\n");
    lv_subject_t *cal_step = lv_xml_get_subject(NULL, "cal_step");
    int pulses_before = fnodes[0].relay_pulses;
    int log_before = app_log_count();
    ui_show_page(2);
    run(500);
    click("Calibrate", 0);
    shot("19a_calibrate");
    click("Run", 0);
    run(1500);
    CHECK(lv_subject_get_int(cal_step) == 2, "the dialog asks for the weight once the feeder accepted");
    run(20000);
    CHECK(fnodes[0].relay_pulses - pulses_before == APP_CAL_PULSES, "the feeder ran the calibration pulses");
    /* This feeder restarted during the firmware update, so its event
     * numbers began again at 1; the event must still be recorded. */
    CHECK(app_log_count() == log_before + 1 && app_log_get(0)->pulses == APP_CAL_PULSES,
          "the first feed after a feeder restart is recorded in the history");
    lv_subject_set_int(lv_xml_get_subject(NULL, "cal_grams"), 60); /* the cup weighed 6.0 g */
    shot("19b_calibrate_weight");
    click("Save", 0);
    CHECK(app_calibrated(0) && app_node(0)->mg_per_pulse == 600 && app_node(0)->cal_on_ds == 10,
          "10 pulses weighing 6.0 g gives 0.6 g per pulse");
    CHECK(app_mg(0, 10, 3) == 1800 && app_mg(0, 20, 3) == 0, "grams are only claimed for the calibrated pulse length");
    CHECK(!app_calibrated(1), "the other feeder is not affected");
    {
        /* Rounding: 1.96 g is 2.0 g, not 1.0 g; 0.04 g is 0.0 g; 0.95 g is 1.0 g. */
        char a[16], b[16], c[16], d[16];
        ui_fmt_grams(a, sizeof(a), 1960);
        ui_fmt_grams(b, sizeof(b), 40);
        ui_fmt_grams(c, sizeof(c), 950);
        ui_fmt_grams(d, sizeof(d), 12340);
        CHECK(!strcmp(a, "2.0 g") && !strcmp(b, "0.0 g") && !strcmp(c, "1.0 g") && !strcmp(d, "12.3 g"),
              "grams are rounded to the nearest tenth");
    }

    ui_show_page(1);
    run(500);
    click(LV_SYMBOL_EDIT, 1); /* the weekday 18:30 entry: 3 pulses */
    CHECK(lv_subject_get_int(lv_xml_get_subject(NULL, "ed_cal")) == 1 &&
          strcmp(lv_subject_get_string(lv_xml_get_subject(NULL, "ed_grams_txt")), "1.8 g") == 0,
          "the schedule editor shows the portion in grams");
    shot("19c_editor_grams");
    click("Cancel", 0);
    ui_show_page(0);
    run(500);
    shot("19d_home_grams");

    ui_show_page(2);
    run(500);
    click("Calibrate", 0);
    click("Run", 0);
    run(1500);
    CHECK(lv_subject_get_int(cal_step) == 0 &&
          strstr(lv_subject_get_string(lv_xml_get_subject(NULL, "cal_text")), "cooldown") != NULL,
          "a second run straight away is refused by the cooldown, and says so");
    CHECK(app_node(0)->mg_per_pulse == 600, "a refused run leaves the calibration alone");
    click("Cancel", 0);

    run(11 * 60000); /* past the cooldown */
    lv_display_trigger_activity(NULL);
    ui_show_page(0);
    run(500);
    click("Feed now", 0);
    shot("19e_feed_confirm_grams");
    click("Feed", 0);
    run(3000);
    click("OK", 0);
    run(4000);
    CHECK(strstr(sd_last_line, ",\"Goldfish\",manual,fed,1.0,1,0.6,estimated") != NULL,
          "the SD log line carries the estimated grams");
    printf("    last line: %s\n", sd_last_line);

    app_set_calibration(0, 10, 0);
    CHECK(!app_calibrated(0) && app_feed_on_ds(0) == app_settings()->feed_on_ds, "entering 0 g removes the calibration");
    app_set_calibration(0, 10, 6000);

    printf("A scale is fitted under the first feeder's hopper\n");
    lv_subject_t *alert = lv_xml_get_subject(NULL, "alert");
    lv_subject_t *scale_text = lv_xml_get_subject(NULL, "scale_text");
    fnodes[0].scale_fitted = true;
    fnodes[0].hopper_mg = 0; /* empty */
    fnodes[0].true_mg_per_pulse = 500; /* a pulse really drops 0.5 g, not the calibrated 0.6 */
    run(12000);
    CHECK(app_scale_found(0) && !app_has_scale(0), "the controller sees a scale that is not set up yet");
    CHECK(!app_scale_found(1), "the other feeder has none");
    ui_show_page(2);
    run(500);
    click("Calibrate", 0);
    CHECK(lv_subject_get_int(lv_xml_get_subject(NULL, "cal_scale")) == 1, "the Scale button is offered");
    click("Scale", 0);
    click("2. Set weight", 0);
    run(1500);
    CHECK(strstr(lv_subject_get_string(scale_text), "could not") != NULL && !app_has_scale(0),
          "setting the weight before zeroing is refused");
    click("1. Zero", 0);
    run(1500);
    CHECK(strstr(lv_subject_get_string(scale_text), "Done") != NULL, "zeroing is confirmed");
    fnodes[0].hopper_mg = 100000; /* the user puts a 100 g weight in */
    click("2. Set weight", 0);
    run(2500);
    CHECK(app_has_scale(0) && app_hopper_mg(0) == 100000, "after the span is set the reading is 100 g");
    shot("19f_scale_setup");
    click("Close", 0);

    fnodes[0].hopper_mg = 180000; /* weight out, hopper filled */
    run(11 * 60000);
    lv_display_trigger_activity(NULL);
    ui_show_page(0);
    run(500);
    click("Feed now", 0);
    click("Feed", 0);
    run(6000);
    click("OK", 0);
    run(3000);
    CHECK(app_log_get(0)->measured_mg == 500, "the feed was weighed: 0.5 g left the hopper");
    CHECK(strstr(sd_last_line, ",manual,fed,1.0,1,0.5,weighed") != NULL, "the SD log has the weighed grams");
    CHECK(app_node(0)->mg_per_pulse == 575, "the estimate moved a quarter of the way to what was weighed");
    run(12000);
    CHECK(app_hopper_mg(0) == 179500 && app_node_rt(0)->status.fed_mg_today == 500,
          "food left and today's weighed total are reported");
    shot("19g_home_scale");
    ui_show_page(3);
    run(500);
    shot("19h_history_weighed");

    printf("The mechanism jams\n");
    fnodes[0].jam = true;
    run(11 * 60000);
    lv_display_trigger_activity(NULL);
    ui_show_page(0);
    run(500);
    click("Feed now", 0);
    click("Feed", 0);
    run(6000);
    click("OK", 0);
    run(3000);
    CHECK(app_log_get(0)->measured_mg == 0 && app_node_rt(0)->low_output, "a feed that drops nothing is noticed");
    CHECK(strstr(lv_subject_get_string(alert), "little food came out") != NULL, "and shown as an alert");
    CHECK(app_node(0)->mg_per_pulse == 575, "a jam does not spoil the estimate");
    shot("19i_jam_alert");

    printf("The hopper runs low\n");
    fnodes[0].jam = false;
    fnodes[0].hopper_mg = 12000;
    run(11 * 60000);
    lv_display_trigger_activity(NULL);
    click("Feed now", 0);
    click("Feed", 0);
    run(6000);
    click("OK", 0);
    run(14000);
    CHECK(!app_node_rt(0)->low_output, "a good feed clears the jam warning");
    CHECK(strstr(lv_subject_get_string(alert), "food is low") != NULL, "less than 20 g left raises a low-food alert");
    fnodes[0].hopper_mg = 180000;
    run(12000);
    CHECK(lv_subject_get_string(alert)[0] == '\0', "refilling clears it");

    printf("Light theme\n");
    lv_subject_set_int(lv_xml_get_subject(NULL, "dark"), 0);
    ui_show_page(0);
    run(300);
    shot("20_home_light");
    ui_show_page(1);
    run(300);
    shot("21_schedule_light");

    printf("Screen sleep\n");
    CHECK(backlight, "backlight on while active");
    run(6 * 60000);
    CHECK(!backlight, "backlight off after the idle timeout");

    CHECK(saves > 0, "state was saved");
    printf("%s (%d problem%s)\n", problems ? "FAILED" : "PASSED", problems, problems == 1 ? "" : "s");
    return problems ? 1 : 0;
}
