#pragma once

#define SDLOG_MOUNT "/sdcard"

/* Start the SD card task. Implements plat_sd_ready() / plat_sd_append(). */
void sdlog_init(void);
