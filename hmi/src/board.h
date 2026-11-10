/*
 * Waveshare ESP32-S3-Touch-LCD-4.3B board support: RGB panel, GT911 touch,
 * CH422G IO expander (backlight, touch reset) and PCF85063 RTC.
 * Pin map and panel timings are taken from Waveshare's demo for this board.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

esp_err_t board_init(void);

esp_lcd_panel_handle_t board_lcd(void);
void board_backlight(bool on);

/* True while the panel is touched; fills the coordinates of the first point. */
bool board_touch_read(uint16_t *x, uint16_t *y);

/* RTC holds local time. board_rtc_get() fails if the RTC lost power. */
bool board_rtc_get(uint32_t *local_epoch);
void board_rtc_set(uint32_t local_epoch);
