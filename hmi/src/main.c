/*
 * AIO-Feeder HMI firmware entry point.
 *
 * One task owns LVGL, the UI and the application model. Frames received over
 * ESP-NOW are queued by the Wi-Fi task and handled here, so none of that
 * code needs locking.
 */
#include "app.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "plat_esp.h"
#include "ui.h"

static const char *TAG = "main";

#define DRAW_BUF_LINES 60

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* Copies into the panel's frame buffer, then returns. */
    esp_lcd_panel_draw_bitmap(board_lcd(), area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0, y = 0;
    bool down = board_touch_read(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void drain_link(lv_timer_t *t)
{
    (void)t;
    uint8_t mac[6], data[250], len;
    while (plat_esp_receive(mac, data, &len)) {
        app_on_packet(mac, data, len);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AIO-Feeder HMI %s", APP_VERSION);
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(plat_esp_init());

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    size_t buf_size = BOARD_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t);
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(buf);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    app_init();
    ui_init();
    lv_timer_create(drain_link, 20, NULL);

    lv_timer_handler(); /* draw the first frame before the light comes on */
    board_backlight(true);

    /* After an update the bootloader runs the new firmware on trial. It got
     * as far as drawing the UI, so keep it; otherwise the next reset goes
     * back to the previous version. */
    esp_ota_mark_app_valid_cancel_rollback();

    while (true) {
        uint32_t wait = lv_timer_handler();
        if (wait > 20) {
            wait = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(wait ? wait : 1));
    }
}
