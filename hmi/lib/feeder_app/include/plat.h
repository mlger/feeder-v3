/*
 * Platform services the portable HMI application needs. Implemented once for
 * the ESP32-S3 board (src/plat_esp.c) and once for the host simulator
 * (sim/plat_sim.c).
 */
#ifndef FEEDER_PLAT_H
#define FEEDER_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t plat_millis(void);

/* Local wall-clock time as seconds since 1970 (see feeder/proto.h). */
bool     plat_time_valid(void);
uint32_t plat_local_epoch(void);
void     plat_set_local_epoch(uint32_t local_epoch);

/* Send one ESP-NOW frame. Delivery is best effort. */
void plat_send(const uint8_t mac[6], const void *msg, uint8_t len);

/* Small named blobs that survive power loss. */
bool plat_load(const char *key, void *buf, size_t len);
void plat_save(const char *key, const void *buf, size_t len);

void plat_backlight(bool on);

#ifdef __cplusplus
}
#endif

#endif
