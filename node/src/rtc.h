// The real-time clock behind the node: a DS3231 (I2C, the default) or a
// DS1302 (three-wire; build the nodemcuv2_ds1302 environment). Both hold the
// node's local epoch: seconds since 1970-01-01 in the user's wall-clock time.
#pragma once

#include <Arduino.h>

bool rtcBegin();            // start the bus; true if a clock chip answers
bool rtcTimeValid();        // the chip has kept (or been given) a trustworthy time
uint32_t rtcRead();         // local epoch
void rtcWrite(uint32_t localEpoch);
