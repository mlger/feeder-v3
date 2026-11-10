// Pin map and tunables for the NodeMCU V3 feeder node.
#pragma once

#include <Arduino.h>

#define FW_MAJOR 0
#define FW_MINOR 1

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

static const uint32_t STATUS_PERIOD_MS = 10000;   // heartbeat to the HMI
static const uint32_t LINK_TIMEOUT_MS = 35000;    // no ACK for this long = link lost
static const uint32_t HOP_PERIOD_MS = 400;        // channel dwell while searching
static const uint32_t EVENT_RETRY_MS = 2000;      // resend un-ACKed feed events
static const uint32_t PAIR_HOLD_MS = 3000;
static const uint32_t PAIR_WINDOW_MS = 120000;    // pairing mode timeout if already paired
static const uint32_t FAILSAFE_MARGIN_MS = 500;
static const uint8_t WIFI_CHANNEL_MAX = 13;
