/*
 * Shared between the UI source files. Not part of the public interface
 * (that is ui.h).
 *
 *   ui.c           start-up, the periodic tick, dialogs, and the Home,
 *                  Feeders, History and Settings pages
 *   ui_schedule.c  the Schedule page and the feeding-time editor
 *   ui_network.c   the Network page: Wi-Fi, internet time, updates
 *   ui_calibrate.c the grams calibration dialog
 *   ui_feed.c      the Feed now dialog
 *   ui_format.c    text formatting
 */
#ifndef FEEDER_UI_INTERNAL_H
#define FEEDER_UI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "lvgl.h"
#include "plat.h"

/* Create a subject and make it visible to the XML under `name`. */
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

/* ---- ui.c ---- */

extern bool ui_loading;     /* true while code (not the user) sets subjects */
extern lv_obj_t *ui_modal;  /* the one open dialog or editor, or NULL */

typedef enum { DLG_NONE, DLG_REMOVE, DLG_DELETE_ENTRY, DLG_CLEAR_LOG } dlg_action_t;

void ui_modal_close(void);
/* Message box. With DLG_NONE it only has an OK button; otherwise OK runs
 * the action with `arg`. */
void ui_dialog_open(dlg_action_t action, int arg, const char *title, const char *text, const char *ok_text);
/* One line typed on the on-screen keyboard; `done` gets the text on OK. */
void ui_text_open(const char *label, const char *initial, uint32_t max_len, bool password,
                  void (*done)(const char *text));

/* List rows remember which item they show. */
void ui_row_set_index(lv_obj_t *row, int idx);
int  ui_row_index_of(lv_event_t *e);

void ui_history_refresh(void);
void ui_clock_redraw(void); /* after the time or the UTC offset changed */

/* ---- ui_schedule.c ---- */

void ui_schedule_register(void);          /* subjects and callbacks; before the XML loads */
void ui_schedule_attach(lv_obj_t *root);  /* after the screen exists */
void ui_schedule_refresh(void);
void ui_schedule_tick(void);
int  ui_sched_node_idx(void);             /* feeder slot chosen on the Schedule page */

/* ---- ui_feed.c ---- */

void ui_feed_register(void);
void ui_feed_open(int idx); /* the Feed now dialog for one feeder */

/* ---- ui_calibrate.c ---- */

void ui_calibrate_register(void);
void ui_calibrate_open(int idx);
/* A manual feed got its answer. Returns true if it belonged to a running
 * calibration, which then shows its own message. */
bool ui_calibrate_feed_result(int idx, uint8_t result);
void ui_calibrate_tick(void);                        /* keeps the scale dialog's reading live */
void ui_scale_result(int idx, uint8_t result);       /* a scale set-up command was answered */

/* ---- ui_network.c ---- */

void ui_network_register(void);
void ui_network_attach(lv_obj_t *root);
void ui_network_refresh(void);
void ui_network_publish_status(void);

/* ---- ui_format.c ---- */

extern const char *const ui_dow3[7];
extern const char *const ui_mon3[12];

/* "Mon 09 Nov 18:00"; with `relative`, "today 18:00" when close to now. */
void ui_fmt_when(char *buf, size_t n, uint32_t epoch, bool relative);
void ui_fmt_ds(char *buf, size_t n, int ds);            /* tenths of a second -> "1.5 s" */
void ui_fmt_days(char *buf, size_t n, uint8_t mask);    /* "Every day", "Weekdays", "Mon Wed" */
void ui_fmt_portion(char *buf, size_t n, uint8_t on_ds, uint8_t pulses);
void ui_fmt_plan_note(char *buf, size_t n, int idx, uint32_t asked_mg, const char *verb);
void ui_fmt_grams(char *buf, size_t n, uint32_t mg);     /* "1.8 g" */
/* A portion on feeder `idx`: grams if it is calibrated for that pulse
 * length ("1.8 g"), otherwise pulses and seconds. */
void ui_fmt_amount(char *buf, size_t n, int idx, uint8_t on_ds, uint8_t pulses);

#endif
