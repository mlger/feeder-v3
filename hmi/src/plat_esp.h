#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* NVS, clock from the RTC, Wi-Fi radio and ESP-NOW. Call after board_init(). */
esp_err_t plat_esp_init(void);

/* Next received ESP-NOW frame, if any. `data` must hold 250 bytes. */
bool plat_esp_receive(uint8_t mac[6], uint8_t *data, uint8_t *len);

/* Called by the network code when NTP has set the system clock. */
void plat_esp_time_synced(void);
