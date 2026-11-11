#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "feeder/core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_9
#define I2C_HZ  400000
#define I2C_TIMEOUT_MS 100

#define TOUCH_INT GPIO_NUM_4

/* CH422G: one I2C address sets the mode, another the output latch. */
#define CH422G_ADDR_MODE 0x24
#define CH422G_ADDR_OUT  0x38
#define CH422G_MODE_OUTPUT 0x01
#define EXIO_TP_RST  (1u << 1)
#define EXIO_BL      (1u << 2)
#define EXIO_LCD_RST (1u << 3)
#define EXIO_SD_CS   (1u << 4)

#define GT911_ADDR_A 0x5D
#define GT911_ADDR_B 0x14
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT1 0x8150

#define PCF85063_ADDR 0x51
#define PCF85063_REG_SECONDS 0x04

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev_exio_mode, dev_exio_out, dev_touch, dev_rtc;
static esp_lcd_panel_handle_t panel;
/* Backlight off; SD chip-select held low (selected), see sdlog.c. */
static uint8_t exio = EXIO_TP_RST | EXIO_LCD_RST;

static esp_err_t add_dev(uint16_t addr, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static esp_err_t exio_write(void)
{
    return i2c_master_transmit(dev_exio_out, &exio, 1, I2C_TIMEOUT_MS);
}

/* ---------------------------------------------------------------- touch */

static esp_err_t touch_init(void)
{
    /* The level of INT while RESET rises selects the GT911 address; low = 0x5D. */
    const gpio_config_t out = { .pin_bit_mask = 1ULL << TOUCH_INT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(TOUCH_INT, 0);
    exio &= (uint8_t)~EXIO_TP_RST;
    exio_write();
    vTaskDelay(pdMS_TO_TICKS(100));
    exio |= EXIO_TP_RST;
    exio_write();
    vTaskDelay(pdMS_TO_TICKS(200));
    const gpio_config_t in = { .pin_bit_mask = 1ULL << TOUCH_INT, .mode = GPIO_MODE_INPUT };
    gpio_config(&in);

    uint16_t addr = GT911_ADDR_A;
    if (i2c_master_probe(bus, GT911_ADDR_A, I2C_TIMEOUT_MS) != ESP_OK) {
        if (i2c_master_probe(bus, GT911_ADDR_B, I2C_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGE(TAG, "GT911 touch controller not found");
            return ESP_ERR_NOT_FOUND;
        }
        addr = GT911_ADDR_B;
    }
    ESP_LOGI(TAG, "GT911 at 0x%02X", addr);
    return add_dev(addr, &dev_touch);
}

bool board_touch_read(uint16_t *x, uint16_t *y)
{
    static bool down;
    static uint16_t last_x, last_y;
    if (!dev_touch) {
        return false;
    }
    const uint8_t reg[2] = { GT911_REG_STATUS >> 8, GT911_REG_STATUS & 0xFF };
    uint8_t status = 0;
    if (i2c_master_transmit_receive(dev_touch, reg, 2, &status, 1, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (status & 0x80) { /* new data; otherwise keep the previous state */
        if ((status & 0x0F) > 0) {
            const uint8_t preg[2] = { GT911_REG_POINT1 >> 8, GT911_REG_POINT1 & 0xFF };
            uint8_t p[4];
            if (i2c_master_transmit_receive(dev_touch, preg, 2, p, 4, I2C_TIMEOUT_MS) == ESP_OK) {
                last_x = (uint16_t)(p[0] | (p[1] << 8));
                last_y = (uint16_t)(p[2] | (p[3] << 8));
                down = last_x < BOARD_LCD_H_RES && last_y < BOARD_LCD_V_RES;
            }
        } else {
            down = false;
        }
        const uint8_t clear[3] = { GT911_REG_STATUS >> 8, GT911_REG_STATUS & 0xFF, 0 };
        i2c_master_transmit(dev_touch, clear, 3, I2C_TIMEOUT_MS);
    }
    *x = last_x;
    *y = last_y;
    return down;
}

/* ------------------------------------------------------------------ RTC */

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool board_rtc_get(uint32_t *local_epoch)
{
    if (!dev_rtc) {
        return false;
    }
    const uint8_t reg = PCF85063_REG_SECONDS;
    uint8_t b[7];
    if (i2c_master_transmit_receive(dev_rtc, &reg, 1, b, 7, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (b[0] & 0x80) { /* oscillator-stop flag: time is not trustworthy */
        return false;
    }
    fd_civil_t c = {
        .second = bcd2bin(b[0] & 0x7F),
        .minute = bcd2bin(b[1] & 0x7F),
        .hour = bcd2bin(b[2] & 0x3F),
        .day = bcd2bin(b[3] & 0x3F),
        .month = bcd2bin(b[5] & 0x1F),
        .year = (uint16_t)(2000 + bcd2bin(b[6])),
    };
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 || c.hour > 23 || c.minute > 59 ||
        c.year < 2024) {
        return false;
    }
    *local_epoch = fd_civil_to_epoch(&c);
    return true;
}

void board_rtc_set(uint32_t local_epoch)
{
    if (!dev_rtc) {
        return;
    }
    fd_civil_t c;
    fd_civil_from_epoch(local_epoch, &c);
    /* Writing seconds with bit 7 clear also clears the oscillator-stop flag. */
    const uint8_t b[8] = {
        PCF85063_REG_SECONDS, bin2bcd(c.second), bin2bcd(c.minute), bin2bcd(c.hour),
        bin2bcd(c.day), c.dow, bin2bcd(c.month), bin2bcd((uint8_t)(c.year % 100)),
    };
    i2c_master_transmit(dev_rtc, b, sizeof(b), I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ LCD */

static esp_err_t lcd_init(void)
{
    const esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        /* The frame buffer is in PSRAM; bounce buffers keep the pixel clock
         * fed while Wi-Fi and flash writes compete for the bus. */
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 10,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10, /* B3-B7 */
            GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, /* G2-G7 */
            GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40, /* R3-R7 */
        },
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &panel), TAG, "RGB panel");
    return esp_lcd_panel_init(panel);
}

esp_lcd_panel_handle_t board_lcd(void) { return panel; }

void board_backlight(bool on)
{
    if (on) {
        exio |= EXIO_BL;
    } else {
        exio &= (uint8_t)~EXIO_BL;
    }
    exio_write();
}

/* ----------------------------------------------------------------- init */

esp_err_t board_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(add_dev(CH422G_ADDR_MODE, &dev_exio_mode), TAG, "CH422G");
    ESP_RETURN_ON_ERROR(add_dev(CH422G_ADDR_OUT, &dev_exio_out), TAG, "CH422G");

    const uint8_t mode = CH422G_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(dev_exio_mode, &mode, 1, I2C_TIMEOUT_MS), TAG,
                        "CH422G not answering");
    ESP_RETURN_ON_ERROR(exio_write(), TAG, "CH422G output");

    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD");

    /* Touch and RTC are not fatal: the UI still shows what is wrong. */
    if (touch_init() != ESP_OK) {
        dev_touch = NULL;
    }
    if (i2c_master_probe(bus, PCF85063_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
        add_dev(PCF85063_ADDR, &dev_rtc);
    } else {
        ESP_LOGW(TAG, "PCF85063 RTC not found");
    }
    return ESP_OK;
}
