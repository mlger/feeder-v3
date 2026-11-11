/*
 * SD card logging.
 *
 * A background task owns the card: it mounts it when one is inserted,
 * notices when it is pulled out, and writes the lines the UI queues. The UI
 * thread therefore never waits on the card.
 *
 * Board wiring (from Waveshare's SD demo): SPI on GPIO 11/12/13. The card's
 * chip-select is on the IO expander and is simply held low, which is fine
 * because the card is the only device on this bus.
 */
#include "sdlog.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "plat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define PIN_MOSI GPIO_NUM_11
#define PIN_CLK  GPIO_NUM_12
#define PIN_MISO GPIO_NUM_13
#define CHECK_PERIOD_MS 5000

typedef struct {
    char file[24];
    char header[80];
    char line[168];
} sd_item_t;

static QueueHandle_t queue;
static atomic_bool ready;
static sdmmc_card_t *card;
static sdmmc_host_t host = SDSPI_HOST_DEFAULT();

static bool mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false, /* never wipe a user's card */
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SDSPI_SLOT_NO_CS;
    slot.host_id = host.slot;
    if (esp_vfs_fat_sdspi_mount(SDLOG_MOUNT, &host, &slot, &cfg, &card) != ESP_OK) {
        card = NULL;
        return false;
    }
    ESP_LOGI(TAG, "card mounted");
    return true;
}

static void unmount(void)
{
    esp_vfs_fat_sdcard_unmount(SDLOG_MOUNT, card);
    card = NULL;
    ESP_LOGW(TAG, "card removed");
}

static bool write_item(const sd_item_t *it)
{
    char path[48];
    struct stat st;
    snprintf(path, sizeof(path), SDLOG_MOUNT "/%s", it->file);
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        return false;
    }
    bool ok = true;
    if (fresh && it->header[0]) {
        ok = fprintf(f, "%s\n", it->header) > 0;
    }
    ok = ok && fprintf(f, "%s\n", it->line) > 0;
    return fclose(f) == 0 && ok;
}

static void sd_task(void *arg)
{
    (void)arg;
    sd_item_t it;
    for (;;) {
        bool got = xQueueReceive(queue, &it, pdMS_TO_TICKS(CHECK_PERIOD_MS)) == pdTRUE;
        if (!card) {
            atomic_store(&ready, mount());
        } else if (!got && sdmmc_get_status(card) != ESP_OK) {
            unmount();
            atomic_store(&ready, false);
        }
        if (got && card && !write_item(&it)) {
            ESP_LOGW(TAG, "write failed");
            unmount();
            atomic_store(&ready, false);
        }
    }
}

bool plat_sd_ready(void) { return atomic_load(&ready); }

void plat_sd_append(const char *file, const char *header, const char *line)
{
    if (!queue || !atomic_load(&ready)) {
        return;
    }
    sd_item_t it;
    snprintf(it.file, sizeof(it.file), "%s", file);
    snprintf(it.header, sizeof(it.header), "%s", header ? header : "");
    snprintf(it.line, sizeof(it.line), "%s", line);
    xQueueSend(queue, &it, 0);
}

void sdlog_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    if (spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus for the SD card did not start; logging is off");
        return;
    }
    queue = xQueueCreate(8, sizeof(sd_item_t));
    xTaskCreate(sd_task, "sd", 4096, NULL, 2, NULL);
}
