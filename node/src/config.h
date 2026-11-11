// Pin map and tunables for the NodeMCU V3 feeder node.
#pragma once

#include <Arduino.h>

#define FW_MAJOR 0
#define FW_MINOR 3

// Relay input, active high. GPIO14 (D5) has no boot-strap function and stays
// low through reset. Fit a 10 k pull-down from this pin to GND so the relay
// stays off even while the ESP8266 is in reset or being flashed.
static const uint8_t PIN_RELAY = D5;

// DS3231 on the default I2C pins.
static const uint8_t PIN_SDA = D2;
static const uint8_t PIN_SCL = D1;

// On-board FLASH button (GPIO0): hold 3 s to enter pairing mode.
static const uint8_t PIN_BUTTON = D3;

// On-board LED (GPIO2), active low.
static const uint8_t PIN_LED = D4;

static const uint32_t PAIR_HOLD_MS = 3000;
static const uint32_t CLOCK_READ_MS = 500;          // how often the DS3231 is read
static const uint32_t OTA_JOIN_TIMEOUT_MS = 20000;
