#include "rtc.h"

#include <Wire.h>

#include "config.h"

#ifdef FD_RTC_DS1302

#include <RtcDS1302.h>
#include <ThreeWire.h>

// A DS1302 is less accurate than a DS3231 (an uncompensated 32.768 kHz
// crystal, so it can drift by minutes a month); the HMI re-sends the time
// regularly, which keeps it in step.
static ThreeWire wire(PIN_RTC_IO, PIN_RTC_SCLK, PIN_RTC_CE);
static RtcDS1302<ThreeWire> chip(wire);

// 1 Jan 2024: any earlier date means the chip lost its time.
static const uint32_t MIN_VALID_EPOCH = 1704067200u;

bool rtcBegin()
{
    chip.Begin();
    chip.SetIsWriteProtected(false);
    // The DS1302 has no presence signal. Its 31 bytes of RAM do: with no chip
    // wired, the data line floats and cannot hold two different patterns.
    static const uint8_t patterns[2] = { 0xA5, 0x5A };
    for (uint8_t p : patterns) {
        chip.SetMemory((uint8_t)0, p);
        if (chip.GetMemory((uint8_t)0) != p) {
            return false;
        }
    }
    return true;
}

bool rtcTimeValid() { return chip.GetIsRunning() && rtcRead() >= MIN_VALID_EPOCH; }

uint32_t rtcRead()
{
    RtcDateTime now = chip.GetDateTime();
    return now.Unix32Time();
}

void rtcWrite(uint32_t localEpoch)
{
    RtcDateTime dt;
    dt.InitWithUnix32Time(localEpoch);
    chip.SetIsWriteProtected(false);
    chip.SetIsRunning(true);
    chip.SetDateTime(dt);
}

#else // DS3231

#include <RTClib.h>

static RTC_DS3231 chip;

bool rtcBegin()
{
    Wire.begin(PIN_SDA, PIN_SCL);
    return chip.begin();
}

bool rtcTimeValid() { return !chip.lostPower(); }

uint32_t rtcRead() { return chip.now().unixtime(); }

void rtcWrite(uint32_t localEpoch) { chip.adjust(DateTime(localEpoch)); }

#endif
