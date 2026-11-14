// AIO-Feeder node firmware: the hardware side.
//
// All behaviour (pairing, the ESP-NOW link, the schedule, feeding, safety
// limits) lives in the shared state machine, feeder/node.h, which is also
// what the unit tests and the HMI simulator run. This file only connects it
// to the ESP8266: flash storage, the DS3231, the relay, the radio, the
// button, the LED and the optional scale.
//
// Safety: the relay is driven low before anything else at boot, every feed
// is clamped to compile-time hard limits (see platformio.ini), and an
// independent timer forces the relay off if the main loop ever stalls.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <espnow.h>

#include "config.h"
#include "feeder/node.h"
#include "rtc.h"
#include "scale.h"

static fd_node_t node;

// ---------------------------------------------------------------- storage

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (uint8_t i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

// Write to a temp file then rename, so a power cut never leaves half a file.
static bool saveBlob(const char *path, const void *data, size_t len)
{
    String tmp = String(path) + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) {
        return false;
    }
    uint16_t crc = crc16((const uint8_t *)data, len);
    bool ok = f.write((const uint8_t *)data, len) == len &&
              f.write((const uint8_t *)&crc, sizeof(crc)) == sizeof(crc);
    f.close();
    return ok && LittleFS.rename(tmp, path);
}

static bool loadBlob(const char *path, void *data, size_t len)
{
    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;
    }
    uint16_t crc = 0;
    bool ok = f.size() == len + sizeof(crc) && f.read((uint8_t *)data, len) == (int)len &&
              f.read((uint8_t *)&crc, sizeof(crc)) == (int)sizeof(crc);
    f.close();
    return ok && crc == crc16((const uint8_t *)data, len);
}

// The file names carry a layout version; bump it if the structs change.
static const char *CFG_FILE = "/cfg2.bin";
static const char *STATE_FILE = "/state3.bin";

static void ioSaveConfig(void *, const fd_node_cfg_t *cfg) { saveBlob(CFG_FILE, cfg, sizeof(*cfg)); }
static bool ioSaveState(void *, const fd_node_state_t *st) { return saveBlob(STATE_FILE, st, sizeof(*st)); }

// ------------------------------------------------------------------ clock

static bool rtcPresent = false;
static uint32_t cachedEpoch = 0;
static uint32_t lastClockReadMs = 0;

// Local epoch from the clock chip (rtc.h), read a couple of times a second.
// Without an RTC, fall back to uptime so that cooldown still works for
// manual feeds; scheduled feeds stay disabled.
//
// A failed I2C read (a loose wire) can come back as a wildly wrong date. That
// would reset the daily feed counters and the cooldown, or fire a schedule
// entry at the wrong time. A reading that disagrees with the time the node
// has been counting itself by more than CLOCK_JUMP_S is ignored, and the node
// keeps counting from its last good reading. If readings keep agreeing with
// each other the change is real (someone set the RTC), so it is accepted.
static const uint32_t CLOCK_JUMP_S = 300;
static const uint8_t CLOCK_JUMP_CONFIRMS = 10;

static uint32_t nowEpoch()
{
    if (!rtcPresent) {
        return millis() / 1000;
    }
    uint32_t t = millis();
    if (cachedEpoch == 0) {
        lastClockReadMs = t;
        cachedEpoch = rtcRead();
    } else if (t - lastClockReadMs >= CLOCK_READ_MS) {
        static uint32_t suspect = 0;
        static uint8_t suspectCount = 0;
        uint32_t expected = cachedEpoch + (t - lastClockReadMs) / 1000;
        uint32_t read = rtcRead();
        uint32_t diff = read > expected ? read - expected : expected - read;
        if (diff <= CLOCK_JUMP_S) {
            cachedEpoch = read;
            suspectCount = 0;
        } else {
            uint32_t apart = read > suspect ? read - suspect : suspect - read;
            suspectCount = (suspectCount > 0 && apart <= CLOCK_JUMP_S) ? suspectCount + 1 : 1;
            suspect = read;
            if (suspectCount >= CLOCK_JUMP_CONFIRMS) {
                cachedEpoch = read;
                suspectCount = 0;
            } else {
                cachedEpoch = expected; // keep counting on our own
            }
        }
        lastClockReadMs = t;
    }
    return cachedEpoch;
}

static bool ioSetClock(void *, uint32_t localEpoch)
{
    if (!rtcPresent) {
        return false;
    }
    rtcWrite(localEpoch);
    cachedEpoch = localEpoch;
    lastClockReadMs = millis();
    return true;
}

// ------------------------------------------------------------------ relay

static Ticker failsafe;

static void ioRelay(void *, bool on) { digitalWrite(PIN_RELAY, on ? HIGH : LOW); }

// Runs from the SDK timer, independent of loop().
static void IRAM_ATTR failsafeTrip()
{
    digitalWrite(PIN_RELAY, LOW);
    fd_node_failsafe_fired(&node);
}

static void ioArmFailsafe(void *, uint32_t ms)
{
    failsafe.detach();
    if (ms) {
        failsafe.once_ms(ms, failsafeTrip);
    }
}

// ---------------------------------------------------------------- ESP-NOW

struct RxPacket {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[250];
};
static const uint8_t RX_QUEUE = 4;
static RxPacket rxq[RX_QUEUE];
static volatile uint8_t rxHead = 0, rxTail = 0;

static void onRecv(uint8_t *mac, uint8_t *data, uint8_t len)
{
    uint8_t next = (rxHead + 1) % RX_QUEUE;
    if (next == rxTail || len > sizeof(rxq[0].data)) {
        return; // queue full or oversize: drop
    }
    memcpy(rxq[rxHead].mac, mac, 6);
    memcpy(rxq[rxHead].data, data, len);
    rxq[rxHead].len = len;
    rxHead = next;
}

static void ioSend(void *, const uint8_t mac[6], const void *msg, uint8_t len)
{
    if (!esp_now_is_peer_exist((uint8_t *)mac)) {
        esp_now_add_peer((uint8_t *)mac, ESP_NOW_ROLE_COMBO, 0, NULL, 0);
    }
    esp_now_send((uint8_t *)mac, (uint8_t *)msg, len);
}

static void ioSetChannel(void *, uint8_t ch) { wifi_set_channel(ch); }

// ------------------------------------------------------- firmware update

// Join the HMI's temporary access point, download the new firmware and
// reboot into it. The state machine has already switched the relay off.
// Any failure ends in a plain restart on the old firmware.
static void ioOta(void *, const fd_msg_ota_start_t *m)
{
    delay(50); // let the ACK leave before the radio changes mode
    digitalWrite(PIN_RELAY, LOW);
    digitalWrite(PIN_LED, LOW);
    Serial.printf("[ota] joining %s\n", m->ssid);

    esp_now_deinit();
    WiFi.begin(m->ssid, m->pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < OTA_JOIN_TIMEOUT_MS) {
        delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
        char url[48];
        snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u/node.bin", m->ip[0], m->ip[1], m->ip[2], m->ip[3],
                 m->port);
        Serial.printf("[ota] fetching %s\n", url);
        WiFiClient client;
        ESPhttpUpdate.rebootOnUpdate(true);
        // Checks the MD5 the HMI sends before switching to the new image.
        t_httpUpdate_return r = ESPhttpUpdate.update(client, url);
        Serial.printf("[ota] failed (%d): %s\n", r, ESPhttpUpdate.getLastErrorString().c_str());
    } else {
        Serial.println("[ota] could not join the access point");
    }
    delay(200);
    ESP.restart();
}

// ------------------------------------------------------------------ scale

static bool ioScaleFound(void *) { return scaleFound(); }
static bool ioScaleRead(void *, int32_t *mg) { return scaleRead(mg); }
static bool ioScaleSet(void *, uint8_t op, uint32_t knownMg)
{
    return op == FD_SCALE_ZERO ? scaleZero() : scaleSpan(knownMg);
}

// --------------------------------------------------------- button and LED

static void buttonLoop()
{
    static uint32_t downSince = 0;
    static bool fired = false;
    if (digitalRead(PIN_BUTTON) == LOW) {
        if (downSince == 0) {
            downSince = millis() ? millis() : 1;
        } else if (!fired && millis() - downSince >= PAIR_HOLD_MS) {
            fired = true;
            fd_node_enter_pairing(&node, millis());
            Serial.println("[link] pairing mode");
        }
    } else {
        downSince = 0;
        fired = false;
    }
}

static void ledLoop()
{
    uint32_t ms = millis();
    bool on = false;
    switch (fd_node_led(&node)) {
    case FD_LED_FEEDING:   on = true; break;                 // solid
    case FD_LED_PAIRING:   on = (ms / 100) % 2; break;       // fast blink
    case FD_LED_SEARCHING: on = (ms / 500) % 2; break;       // slow blink
    case FD_LED_NO_TIME:   on = (ms % 2000) < 100 || ((ms % 2000) >= 250 && (ms % 2000) < 350); break;
    case FD_LED_OK:        on = (ms % 2000) < 60; break;     // heartbeat
    }
    digitalWrite(PIN_LED, on ? LOW : HIGH);
}

// Report link changes on the serial port; useful when bringing a node up.
static void logLoop()
{
    static int last = -1;
    int led = (int)fd_node_led(&node);
    if (led != last) {
        static const char *const names[] = {"ok", "feeding", "pairing", "searching for HMI", "clock not set"};
        Serial.printf("[node] %s (channel %u)\n", names[led], node.channel);
        last = led;
    }
}

// ------------------------------------------------------------------ setup

void setup()
{
    // Relay off before anything else.
    digitalWrite(PIN_RELAY, LOW);
    pinMode(PIN_RELAY, OUTPUT);
    digitalWrite(PIN_RELAY, LOW);

    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    Serial.begin(115200);
#ifdef APP_VERSION
    Serial.printf("\nAIO-Feeder node %s\n", APP_VERSION);
#else
    Serial.printf("\nAIO-Feeder node v%d.%d\n", FW_MAJOR, FW_MINOR);
#endif

    if (!LittleFS.begin()) {
        LittleFS.format();
        LittleFS.begin();
    }
    fd_node_cfg_t cfg;
    fd_node_state_t st;
    if (!loadBlob(CFG_FILE, &cfg, sizeof(cfg))) {
        fd_node_default_config(&cfg);
    }
    if (!loadBlob(STATE_FILE, &st, sizeof(st))) {
        memset(&st, 0, sizeof(st));
    }

    scaleBegin();

    rtcPresent = rtcBegin();
    bool timeValid = rtcPresent && rtcTimeValid();
    Serial.printf("[rtc] present=%d valid=%d\n", rtcPresent, timeValid);

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != 0) {
        Serial.println("[link] esp_now_init failed, restarting");
        delay(1000);
        ESP.restart();
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onRecv);

    static const fd_node_io_t io = {
        nullptr, ioSend, ioSetChannel, ioRelay, ioSaveConfig, ioSaveState, ioSetClock, ioArmFailsafe, ioOta,
        ioScaleFound, ioScaleRead, ioScaleSet,
    };
    fd_node_init(&node, &io, &cfg, &st, FW_MAJOR, FW_MINOR, rtcPresent, timeValid, millis(), nowEpoch());
    Serial.printf("[link] mac=%s paired=%d channel=%u entries=%u rev=%u\n", WiFi.macAddress().c_str(),
                  node.cfg.paired, node.cfg.channel, node.cfg.count, node.cfg.sched_rev);
}

void loop()
{
    while (rxTail != rxHead) {
        const RxPacket &p = rxq[rxTail];
        fd_node_rx(&node, millis(), nowEpoch(), p.mac, p.data, p.len);
        rxTail = (rxTail + 1) % RX_QUEUE;
    }
    scaleLoop();
    fd_node_tick(&node, millis(), nowEpoch());
    buttonLoop();
    ledLoop();
    logLoop();
    yield();
}
