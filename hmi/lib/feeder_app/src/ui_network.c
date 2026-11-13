/* The Network page: Wi-Fi, phone set-up, internet time and firmware updates. */
#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#define MAX_APS 8
#define SETUP_WINDOW_MS (5u * 60u * 1000u)
#define SETUP_LINGER_MS 3000u /* let the phone receive its answer */

static lv_subject_t s_setup_text;
static lv_subject_t s_wifi_status, s_ntp_on, s_ntp_status, s_utc_off, s_utc_txt, s_ota_status;

static lv_obj_t *wifi_list;
static plat_ap_t aps[MAX_APS];
static int  ap_count;
static bool scanning;
static char wifi_pick[33];
static lv_obj_t *setup_modal;     /* the phone set-up dialog while its access point is open */
static uint32_t setup_since_ms, setup_done_ms;
static bool sd_was_ready;

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
        ui_row_set_index(row, i);
    }
}

/* Close the phone set-up access point and its dialog. */
static void setup_stop(void)
{
    if (!setup_modal) {
        return;
    }
    plat_wifi_setup_end();
    if (ui_modal == setup_modal) {
        ui_modal_close();
    }
    setup_modal = NULL;
}

static void setup_tick(void)
{
    if (!setup_modal) {
        return;
    }
    uint32_t ms = plat_millis();
    if (ui_modal != setup_modal) {
        setup_stop(); /* another dialog replaced it */
    } else if (plat_wifi_setup_done()) {
        if (setup_done_ms == 0) {
            setup_done_ms = ms ? ms : 1;
        } else if (ms - setup_done_ms >= SETUP_LINGER_MS) {
            setup_stop();
        }
    } else if (ms - setup_since_ms >= SETUP_WINDOW_MS) {
        setup_stop();
    }
}

static void on_wifi_phone(lv_event_t *e)
{
    (void)e;
    char ssid[24], pass[24], text[200];
    if (app_ota_state() == APP_OTA_RUNNING) {
        ui_dialog_open(DLG_NONE, 0, "Busy", "A feeder update is running. Try again when it has finished.", NULL);
        return;
    }
    if (!plat_wifi_setup_begin(ssid, pass)) {
        ui_dialog_open(DLG_NONE, 0, "Not available", "The set-up access point could not be opened.", NULL);
        return;
    }
    snprintf(text, sizeof(text),
             "1. Scan the code, or join the Wi-Fi network\n    %s\n    password  %s\n"
             "2. Open  http://192.168.4.1  in the phone's browser.",
             ssid, pass);
    lv_subject_copy_string(&s_setup_text, text);

    ui_modal_close();
    ui_modal = setup_modal = lv_xml_create(lv_layer_top(), "setup_dialog", NULL);
    /* The standard Wi-Fi QR format; phone cameras offer to join. */
    char qr[96];
    int n = snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    lv_qrcode_update(lv_obj_find_by_name(ui_modal, "setup_qr"), qr, (uint32_t)n);
    setup_since_ms = plat_millis();
    setup_done_ms = 0;
}

static void on_setup_cancel(lv_event_t *e)
{
    (void)e;
    setup_stop();
}

void ui_network_refresh(void)
{
    setup_tick();
    if (plat_sd_ready() != sd_was_ready) {
        sd_was_ready = plat_sd_ready();
        ui_history_refresh();
    }

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

    const app_node_cfg_t *on = app_node(app_ota_node());
    if (app_ota_state() == APP_OTA_RUNNING) {
        snprintf(buf, sizeof(buf), "Updating %s. Leave it powered.", on ? on->name : "feeder");
    } else if (ws != PLAT_WIFI_CONNECTED) {
        snprintf(buf, sizeof(buf), "Needs Wi-Fi.");
    } else {
        /* Open to the whole network on purpose: say so. */
        snprintf(buf, sizeof(buf), "Open http://%s to upload firmware. There is no password: anyone on this network can.", ip);
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
            ui_dialog_open(DLG_NONE, 0, "Update not started", err, NULL);
        }
    }
}

/* State for the web page: uploads, feeders and the running update. */
void ui_network_publish_status(void)
{
    static const char *const ota_names[] = { "idle", "running", "done", "failed" };
    char json[700];
    int n = snprintf(json, sizeof(json),
                     "{\"version\":\"" APP_VERSION "\",\"node_fw\":%u,"
                     "\"ota\":\"%s\",\"ota_node\":%d,\"ota_msg\":\"%s\",\"nodes\":[",
                     (unsigned)plat_node_fw_size(),
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

static void wifi_password_entered(const char *text) { plat_wifi_connect(wifi_pick, text); }

static void on_wifi_pick(lv_event_t *e)
{
    int i = ui_row_index_of(e);
    if (i < 0 || i >= ap_count) {
        return;
    }
    snprintf(wifi_pick, sizeof(wifi_pick), "%s", aps[i].ssid);
    if (aps[i].secure) {
        ui_text_open("Password", "", 63, true, wifi_password_entered);
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
    if (ui_loading) {
        return;
    }
    app_settings_t *st = app_settings();
    if (st->utc_offset_min != off) {
        st->utc_offset_min = (int16_t)off;
        plat_set_utc_offset(st->utc_offset_min);
        ui_clock_redraw();
    }
    if (st->ntp != (uint8_t)lv_subject_get_int(&s_ntp_on)) {
        st->ntp = (uint8_t)lv_subject_get_int(&s_ntp_on);
        plat_ntp_enable(st->ntp);
    }
    app_settings_save();
}

void ui_network_register(void)
{
    const app_settings_t *st = app_settings();
    STR_SUBJECT(s_wifi_status, "wifi_status", 128, "");
    INT_SUBJECT(s_ntp_on, "ntp_on", st->ntp);
    STR_SUBJECT(s_ntp_status, "ntp_status", 128, "");
    INT_SUBJECT(s_utc_off, "utc_off", st->utc_offset_min);
    STR_SUBJECT(s_utc_txt, "utc_txt", 16, "");
    STR_SUBJECT(s_ota_status, "ota_status", 128, "");
    STR_SUBJECT(s_setup_text, "setup_text", 200, "");

    lv_xml_register_event_cb(NULL, "on_wifi_phone", on_wifi_phone);
    lv_xml_register_event_cb(NULL, "on_setup_cancel", on_setup_cancel);
    lv_xml_register_event_cb(NULL, "on_wifi_scan", on_wifi_scan);
    lv_xml_register_event_cb(NULL, "on_wifi_forget", on_wifi_forget);
    lv_xml_register_event_cb(NULL, "on_wifi_pick", on_wifi_pick);
}

void ui_network_attach(lv_obj_t *root)
{
    wifi_list = lv_obj_find_by_name(root, "wifi_list");
    lv_subject_add_observer(&s_utc_off, on_network_changed, NULL);
    lv_subject_add_observer(&s_ntp_on, on_network_changed, NULL);
}
