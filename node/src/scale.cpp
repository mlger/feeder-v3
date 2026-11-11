#include "scale.h"

#include <LittleFS.h>

static const uint8_t PIN_DT = D6;
static const uint8_t PIN_SCK = D7;

static const uint8_t WINDOW = 8;             // samples averaged (about 0.8 s at 10 samples/s)
static const uint32_t LOST_AFTER_MS = 2000;  // no sample for this long: no HX711
static const int32_t STEADY_MG = 300;        // spread allowed across the window once set up
static const int32_t STEADY_RAW = 3000;      // the same, in raw counts, before set-up
static const uint32_t MIN_SPAN_MG = 5000;    // smallest reference weight accepted
static const int32_t MIN_SPAN_RAW = 2000;    // smallest change that weight must cause

struct ScaleCal {
    uint32_t magic;
    int32_t zeroRaw;        // reading with an empty hopper
    float rawPerMg;         // counts per milligram; 0 = span not set
    uint8_t haveZero;
};
static const uint32_t CAL_MAGIC = 0x53434C31; // "SCL1"
static const char *CAL_FILE = "/scale.bin";

static ScaleCal cal;
static int32_t window[WINDOW];
static uint8_t count = 0, head = 0;
static uint32_t lastSampleMs = 0;
static bool everSampled = false;

// One 24-bit conversion, channel A at gain 128. Timing-critical: the HX711
// powers down if SCK stays high for more than 60 microseconds.
static int32_t readRaw()
{
    uint32_t v = 0;
    noInterrupts();
    for (uint8_t i = 0; i < 24; i++) {
        digitalWrite(PIN_SCK, HIGH);
        delayMicroseconds(1);
        v = (v << 1) | (digitalRead(PIN_DT) ? 1 : 0);
        digitalWrite(PIN_SCK, LOW);
        delayMicroseconds(1);
    }
    digitalWrite(PIN_SCK, HIGH); // 25th pulse: next conversion on channel A, gain 128
    delayMicroseconds(1);
    digitalWrite(PIN_SCK, LOW);
    interrupts();
    if (v & 0x800000) {
        v |= 0xFF000000; // sign-extend
    }
    return (int32_t)v;
}

static void saveCal()
{
    File f = LittleFS.open(CAL_FILE, "w");
    if (f) {
        f.write((const uint8_t *)&cal, sizeof(cal));
        f.close();
    }
}

void scaleBegin()
{
    pinMode(PIN_SCK, OUTPUT);
    digitalWrite(PIN_SCK, LOW);
    pinMode(PIN_DT, INPUT_PULLUP); // reads high (never "ready") with nothing connected

    memset(&cal, 0, sizeof(cal));
    File f = LittleFS.open(CAL_FILE, "r");
    if (f) {
        ScaleCal c;
        if (f.read((uint8_t *)&c, sizeof(c)) == (int)sizeof(c) && c.magic == CAL_MAGIC) {
            cal = c;
        }
        f.close();
    }
    cal.magic = CAL_MAGIC;
}

void scaleLoop()
{
    if (digitalRead(PIN_DT) == LOW) { // conversion ready
        int32_t raw = readRaw();
        // All ones or all zeros means a floating or shorted data line.
        if (raw != -1 && raw != 0) {
            window[head] = raw;
            head = (head + 1) % WINDOW;
            if (count < WINDOW) {
                count++;
            }
            lastSampleMs = millis();
            everSampled = true;
        }
    }
    if (everSampled && millis() - lastSampleMs > LOST_AFTER_MS) {
        everSampled = false;
        count = 0;
    }
}

bool scaleFound() { return everSampled; }

// Average of the window, and whether its spread is within `limit` counts.
static bool steadyRaw(int32_t *avg, int32_t limit)
{
    if (!everSampled || count < WINDOW) {
        return false;
    }
    int32_t lo = window[0], hi = window[0];
    int64_t sum = 0;
    for (uint8_t i = 0; i < WINDOW; i++) {
        lo = min(lo, window[i]);
        hi = max(hi, window[i]);
        sum += window[i];
    }
    *avg = (int32_t)(sum / WINDOW);
    return hi - lo <= limit;
}

bool scaleRead(int32_t *mg)
{
    if (!cal.haveZero || cal.rawPerMg == 0.0f) {
        return false;
    }
    int32_t raw;
    int32_t limit = (int32_t)fabsf(cal.rawPerMg * STEADY_MG);
    if (!steadyRaw(&raw, limit < 1 ? 1 : limit)) {
        return false;
    }
    *mg = (int32_t)lroundf((float)(raw - cal.zeroRaw) / cal.rawPerMg);
    return true;
}

bool scaleZero()
{
    int32_t raw;
    if (!steadyRaw(&raw, STEADY_RAW)) {
        return false;
    }
    cal.zeroRaw = raw;
    cal.haveZero = 1;
    saveCal();
    return true;
}

bool scaleSpan(uint32_t knownMg)
{
    int32_t raw;
    if (!cal.haveZero || knownMg < MIN_SPAN_MG || !steadyRaw(&raw, STEADY_RAW)) {
        return false;
    }
    int32_t delta = raw - cal.zeroRaw;
    if (abs(delta) < MIN_SPAN_RAW) {
        return false; // the weight made no real difference: wiring or mounting problem
    }
    cal.rawPerMg = (float)delta / (float)knownMg; // negative if the cell is mounted upside down; that is fine
    saveCal();
    return true;
}
