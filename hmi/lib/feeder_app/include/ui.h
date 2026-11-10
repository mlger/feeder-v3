#ifndef FEEDER_UI_H
#define FEEDER_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build the UI on the active screen. Call after lv_init(), a display exists
 * and app_init() has run. */
void ui_init(void);

/* Switch page: 0 Home, 1 Schedule, 2 Feeders, 3 History, 4 Settings, 5 Network. */
void ui_show_page(int page);

#ifdef __cplusplus
}
#endif

#endif
