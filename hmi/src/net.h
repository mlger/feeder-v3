#pragma once

#include "esp_err.h"

/* Wi-Fi station, internet time, the update web page and the temporary
 * access point for feeder updates. Starts the radio; ESP-NOW is brought up
 * by the caller afterwards. */
esp_err_t net_init(void);
