// AIO-Feeder node firmware.
//
// The node keeps its own copy of the schedule and runs it from the DS3231, so
// fish are fed even when the HMI is off. The HMI is the source of truth: it
// pushes schedule, limits and time over ESP-NOW and receives status and feed
// events back.
//
// Safety: the relay is driven low before anything else at boot, every feed is
// clamped to compile-time hard limits (see platformio.ini), and an independent
// timer forces the relay off if the main loop ever stalls.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <RTClib.h>
#include <Ticker.h>
#include <Wire.h>
#include <espnow.h>

#include "config.h"
#include "feeder/core.h"

// ---------------------------------------------------------------- storage

static const uint32_t CFG_MAGIC = 0x46443031; // "FD01"

struct NodeConfig {
    uint32_t magic;
    uint8_t paired;
    uint8_t hmi_mac[6];
    uint8_t node_id;
    uint8_t channel;
    char name[FD_NAME_LEN];
    uint16_t sched_rev;
    fd_limits_t limits;
    uint8_t count;
    fd_sched_entry_t entries[FD_MAX_ENTRIES];
};

struct NodeState {
    uint32_t magic;
    fd_guard_t guard;
    uint32_t last_sched_minute; // epoch/60 of the last scheduled feed handled
};

static NodeConfig cfg;
static NodeState st;

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

static void defaultConfig()
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CFG_MAGIC;
    cfg.channel = 1;
    cfg.limits.gap_ds = 5;
    cfg.limits.cooldown_s = 600;
    cfg.limits.daily_max_on_s = 30;
}

static void saveConfig() { saveBlob("/cfg.bin", &cfg, sizeof(cfg)); }
static void saveState() { saveBlob("/state.bin", &st, sizeof(st)); }

static void loadAll()
{
    if (!loadBlob("/cfg.bin", &cfg, sizeof(cfg)) || cfg.magic != CFG_MAGIC ||
        cfg.count > FD_MAX_ENTRIES || cfg.channel < 1 || cfg.channel > WIFI_CHANNEL_MAX) {
        defaultConfig();
    }
    if (!loadBlob("/state.bin", &st, sizeof(st)) || st.magic != CFG_MAGIC) {
        memset(&st, 0, sizeof(st));
        st.magic = CFG_MAGIC;
    }
}

// ------------------------------------------------------------------ clock

static RTC_DS3231 rtc;
static bool rtcPresent = false;
static bool timeValid = false;

// Local epoch from the DS3231. Without an RTC, fall back to uptime so that
// cooldown still works for manual feeds; scheduled feeds stay disabled.
static uint32_t nowEpoch()
{
    if (rtcPresent) {
        return rtc.now().unixtime();
    }
    return millis() / 1000;
}

// Scheduled feeds are skipped for any minute at or before the last one that
// fed, which stops a repeat after a reboot or a small backwards correction
// (for example the end of daylight saving). If the clock is now far behind
// that mark, the mark came from a wrong clock; drop it so feeding resumes.
static void reconcileScheduleMark(uint32_t epoch)
{
    uint32_t minute = epoch / 60;
    if (minute + 120 < st.last_sched_minute) {
        st.last_sched_minute = minute;
        saveState();
    }
}

// ------------------------------------------------------------------ relay

static fd_pulse_t pulse;
static Ticker failsafe;
static volatile bool failsafeFired = false;
static uint8_t feedSource = FD_SOURCE_MANUAL;

static inline void relayWrite(bool on) { digitalWrite(PIN_RELAY, on ? HIGH : LOW); }

// Runs from the SDK timer, independent of loop().
static void IRAM_ATTR failsafeTrip()
{
    digitalWrite(PIN_RELAY, LOW);
    failsafeFired = true;
}

// ----------------------------------------------------------------- events

static const uint8_t EVENT_QUEUE = 16;
static fd_msg_feed_event_t events[EVENT_QUEUE];
static uint8_t evHead = 0, evCount = 0;
static uint16_t nextEventId = 1;
static uint32_t lastEventSendMs = 0;

static void pushEvent(uint8_t source, fd_result_t result, uint8_t pulsesDone, uint32_t onMs)
{
    if (evCount == EVENT_QUEUE) { // full: drop the oldest
        evHead = (evHead + 1) % EVENT_QUEUE;
        evCount--;
    }
    fd_msg_feed_event_t &e = events[(evHead + evCount) % EVENT_QUEUE];
    fd_hdr_init(&e.hdr, FD_MSG_FEED_EVENT, 0);
    e.node_id = cfg.node_id;
    e.event_id = nextEventId++;
    e.local_epoch = nowEpoch();
    e.source = source;
    e.result = result;
    e.pulses_done = pulsesDone;
    e.on_ms_total = onMs > 0xFFFF ? 0xFFFF : (uint16_t)onMs;
    evCount++;
    lastEventSendMs = 0; // send on the next loop
    Serial.printf("[feed] source=%u result=%u pulses=%u on=%lums\n", source, result, pulsesDone,
                  (unsigned long)onMs);
}

// ------------------------------------------------------------------- feed

static fd_result_t tryFeed(uint8_t source, uint8_t on_ds, uint8_t pulses)
{
    if (pulse.active) {
        return FD_RES_BUSY;
    }
    uint32_t now = nowEpoch();
    fd_result_t res = fd_guard_check(&st.guard, &cfg.limits, now, &on_ds, &pulses);
    if (res != FD_RES_OK) {
        return res;
    }

    // Record the feed before energising the relay. If power fails mid-feed
    // the node must not repeat it on the next boot.
    fd_guard_commit(&st.guard, now, (uint32_t)on_ds * 100u * pulses);
    saveState();

    feedSource = source;
    failsafeFired = false;
    uint32_t total = fd_pulse_duration_ms(on_ds, pulses, cfg.limits.gap_ds);
    failsafe.once_ms(total + FAILSAFE_MARGIN_MS, failsafeTrip);
    fd_pulse_start(&pulse, millis(), on_ds, pulses, cfg.limits.gap_ds);
    relayWrite(pulse.relay_on);
    return FD_RES_OK;
}

static void feedLoop()
{
    if (failsafeFired) {
        failsafeFired = false;
        if (pulse.active) {
            uint8_t done = pulse.pulses_done;
            uint32_t on = pulse.on_ms_total;
            fd_pulse_abort(&pulse);
            relayWrite(false);
            pushEvent(feedSource, FD_RES_ABORTED, done, on);
        }
        return;
    }
    if (!pulse.active) {
        return;
    }
    relayWrite(fd_pulse_tick(&pulse, millis()));
    if (!pulse.active) {
        failsafe.detach();
        relayWrite(false);
        pushEvent(feedSource, FD_RES_OK, pulse.pulses_done, pulse.on_ms_total);
    }
}

static void scheduleLoop()
{
    static uint32_t lastPollMs = 0;
    if (millis() - lastPollMs < 1000) {
        return;
    }
    lastPollMs = millis();
    if (!timeValid) {
        return;
    }

    uint32_t now = nowEpoch();
    uint32_t minute = now / 60;
    // Each minute is handled once, and never a minute at or before one that
    // has already fed. This survives reboots and the clock being set back.
    if (minute <= st.last_sched_minute) {
        return;
    }
    uint16_t due = fd_sched_due(cfg.entries, cfg.count, now);
    if (due == 0) {
        return;
    }
    st.last_sched_minute = minute;
    bool started = false;
    for (uint8_t i = 0; i < cfg.count; i++) {
        if (!(due & (1u << i))) {
            continue;
        }
        fd_result_t res = started ? FD_RES_BUSY
                                  : tryFeed(i, cfg.entries[i].on_ds, cfg.entries[i].pulses);
        if (res == FD_RES_OK) {
            started = true; // tryFeed saved the state
        } else {
            pushEvent(i, res, 0, 0);
        }
    }
    if (!started) {
        saveState();
    }
}

// ---------------------------------------------------------------- ESP-NOW

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct RxPacket {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[250];
};
static const uint8_t RX_QUEUE = 4;
static RxPacket rxq[RX_QUEUE];
static volatile uint8_t rxHead = 0, rxTail = 0;

static uint8_t txSeq = 0;
static bool pairingMode = false;
static uint32_t pairingSinceMs = 0;
static bool linked = false;
static uint32_t lastAckMs = 0;
static uint32_t lastStatusMs = 0;
static uint32_t lastHopMs = 0;
static uint8_t curChannel = 1;
static uint8_t lastFeedSeq = 0;
static bool haveFeedSeq = false;
static uint8_t lastFeedCode = FD_RES_OK;

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

static void setChannel(uint8_t ch)
{
    curChannel = ch;
    wifi_set_channel(ch);
}

static void ensurePeer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist((uint8_t *)mac)) {
        esp_now_add_peer((uint8_t *)mac, ESP_NOW_ROLE_COMBO, 0, NULL, 0);
    }
}

static void sendTo(const uint8_t *mac, const void *msg, uint8_t len)
{
    ensurePeer(mac);
    esp_now_send((uint8_t *)mac, (uint8_t *)msg, len);
}

static void sendAck(const uint8_t *mac, const fd_hdr_t *req, uint8_t code)
{
    fd_msg_ack_t a;
    fd_hdr_init(&a.hdr, FD_MSG_ACK, req->seq);
    a.acked_type = req->type;
    a.code = code;
    a.event_id = 0;
    sendTo(mac, &a, sizeof(a));
}

static uint8_t statusFlags()
{
    uint8_t f = 0;
    if (timeValid) f |= FD_FLAG_TIME_VALID;
    if (!rtcPresent) f |= FD_FLAG_RTC_MISSING;
    if (pulse.active) f |= FD_FLAG_FEEDING;
    if (pairingMode) f |= FD_FLAG_PAIRING;
    return f;
}

static void sendStatus()
{
    uint32_t now = nowEpoch();
    fd_guard_roll_day(&st.guard, now);
    fd_msg_status_t s;
    fd_hdr_init(&s.hdr, FD_MSG_STATUS, ++txSeq);
    s.node_id = cfg.node_id;
    s.flags = statusFlags();
    s.fw_major = FW_MAJOR;
    s.fw_minor = FW_MINOR;
    s.sched_rev = cfg.sched_rev;
    s.local_epoch = now;
    s.last_feed_epoch = st.guard.last_feed_epoch;
    s.on_ms_today = st.guard.on_ms_today;
    s.feeds_today = st.guard.feeds_today;
    s.uptime_s = millis() / 1000;
    sendTo(cfg.hmi_mac, &s, sizeof(s));
}

static void sendHello()
{
    fd_msg_hello_t h;
    fd_hdr_init(&h.hdr, FD_MSG_HELLO, ++txSeq);
    h.fw_major = FW_MAJOR;
    h.fw_minor = FW_MINOR;
    h.flags = statusFlags();
    sendTo(BROADCAST, &h, sizeof(h));
}

static void enterPairing()
{
    pairingMode = true;
    pairingSinceMs = millis();
    Serial.println("[link] pairing mode");
}

static void handlePacket(const RxPacket &p)
{
    uint8_t type = fd_proto_validate(p.data, p.len);
    if (type == 0) {
        return;
    }
    const fd_hdr_t *hdr = (const fd_hdr_t *)p.data;

    // PAIR is the only message accepted from an unknown sender, and only
    // while the user has put the node in pairing mode.
    if (type == FD_MSG_PAIR) {
        if (!pairingMode) {
            return;
        }
        const fd_msg_pair_t *m = (const fd_msg_pair_t *)p.data;
        memcpy(cfg.hmi_mac, p.mac, 6);
        cfg.paired = 1;
        cfg.node_id = m->node_id;
        memcpy(cfg.name, m->name, FD_NAME_LEN);
        cfg.name[FD_NAME_LEN - 1] = '\0';
        cfg.channel = curChannel;
        saveConfig();
        pairingMode = false;
        linked = true;
        lastAckMs = millis();
        sendAck(p.mac, hdr, FD_RES_OK);
        Serial.printf("[link] paired as node %u on channel %u\n", cfg.node_id, curChannel);
        return;
    }

    if (!cfg.paired || memcmp(p.mac, cfg.hmi_mac, 6) != 0) {
        return;
    }
    // Anything valid from our HMI proves the channel is right.
    lastAckMs = millis();
    if (!linked) {
        linked = true;
        if (cfg.channel != curChannel) {
            cfg.channel = curChannel;
            saveConfig();
        }
        Serial.printf("[link] up on channel %u\n", curChannel);
    }

    switch (type) {
    case FD_MSG_ACK: {
        const fd_msg_ack_t *a = (const fd_msg_ack_t *)p.data;
        if (a->acked_type == FD_MSG_FEED_EVENT && evCount > 0 &&
            a->event_id == events[evHead].event_id) {
            evHead = (evHead + 1) % EVENT_QUEUE;
            evCount--;
            lastEventSendMs = 0;
        }
        break;
    }
    case FD_MSG_TIME_SYNC: {
        const fd_msg_time_sync_t *m = (const fd_msg_time_sync_t *)p.data;
        if (rtcPresent) {
            rtc.adjust(DateTime(m->local_epoch));
            timeValid = true;
            reconcileScheduleMark(m->local_epoch);
            sendAck(p.mac, hdr, FD_RES_OK);
        } else {
            sendAck(p.mac, hdr, FD_RES_TIME_INVALID);
        }
        break;
    }
    case FD_MSG_SCHED_SET: {
        const fd_msg_sched_set_t *m = (const fd_msg_sched_set_t *)p.data;
        bool ok = true;
        for (uint8_t i = 0; i < m->count; i++) {
            ok = ok && fd_sched_entry_valid(&m->entries[i]);
        }
        if (ok) {
            cfg.sched_rev = m->rev;
            cfg.limits = m->limits;
            cfg.count = m->count;
            memset(cfg.entries, 0, sizeof(cfg.entries));
            memcpy(cfg.entries, m->entries, m->count * sizeof(fd_sched_entry_t));
            saveConfig();
        }
        sendAck(p.mac, hdr, ok ? FD_RES_OK : FD_RES_BAD_REQUEST);
        break;
    }
    case FD_MSG_FEED_NOW: {
        const fd_msg_feed_now_t *m = (const fd_msg_feed_now_t *)p.data;
        // A retransmission of the request we just handled gets the same
        // answer and must not feed twice.
        if (!(haveFeedSeq && hdr->seq == lastFeedSeq)) {
            lastFeedSeq = hdr->seq;
            haveFeedSeq = true;
            lastFeedCode = tryFeed(FD_SOURCE_MANUAL, m->on_ds, m->pulses);
        }
        sendAck(p.mac, hdr, lastFeedCode);
        break;
    }
    case FD_MSG_STATUS_REQ:
        sendStatus();
        lastStatusMs = millis();
        break;
    case FD_MSG_UNPAIR:
        sendAck(p.mac, hdr, FD_RES_OK);
        cfg.paired = 0;
        memset(cfg.hmi_mac, 0, 6);
        saveConfig();
        linked = false;
        enterPairing();
        break;
    default:
        break;
    }
}

static void linkLoop()
{
    while (rxTail != rxHead) {
        handlePacket(rxq[rxTail]);
        rxTail = (rxTail + 1) % RX_QUEUE;
    }

    uint32_t ms = millis();

    if (pairingMode && cfg.paired && ms - pairingSinceMs > PAIR_WINDOW_MS) {
        pairingMode = false;
        setChannel(cfg.channel);
    }

    if (pairingMode || !cfg.paired) {
        // Walk the channels announcing ourselves until an HMI adopts us.
        if (ms - lastHopMs >= HOP_PERIOD_MS) {
            lastHopMs = ms;
            setChannel(curChannel >= WIFI_CHANNEL_MAX ? 1 : curChannel + 1);
            sendHello();
        }
        return;
    }

    if (linked && ms - lastAckMs > LINK_TIMEOUT_MS) {
        linked = false;
        Serial.println("[link] lost, searching");
    }

    if (!linked) {
        // The HMI follows its Wi-Fi router's channel, which can change.
        if (ms - lastHopMs >= HOP_PERIOD_MS) {
            lastHopMs = ms;
            setChannel(curChannel >= WIFI_CHANNEL_MAX ? 1 : curChannel + 1);
            sendStatus();
        }
        return;
    }

    if (ms - lastStatusMs >= STATUS_PERIOD_MS) {
        lastStatusMs = ms;
        sendStatus();
    }
    if (evCount > 0 && (lastEventSendMs == 0 || ms - lastEventSendMs >= EVENT_RETRY_MS)) {
        lastEventSendMs = ms ? ms : 1;
        fd_msg_feed_event_t &e = events[evHead];
        e.hdr.seq = ++txSeq;
        e.node_id = cfg.node_id;
        sendTo(cfg.hmi_mac, &e, sizeof(e));
    }
}

// --------------------------------------------------------------- UI (LED)

static void buttonLoop()
{
    static uint32_t downSince = 0;
    if (digitalRead(PIN_BUTTON) == LOW) {
        if (downSince == 0) {
            downSince = millis() ? millis() : 1;
        } else if (!pairingMode && millis() - downSince >= PAIR_HOLD_MS) {
            enterPairing();
        }
    } else {
        downSince = 0;
    }
}

static void ledLoop()
{
    uint32_t ms = millis();
    bool on;
    if (pulse.active) {
        on = true;                    // solid while feeding
    } else if (pairingMode || !cfg.paired) {
        on = (ms / 100) % 2;          // fast blink: pairing
    } else if (!linked) {
        on = (ms / 500) % 2;          // slow blink: searching for the HMI
    } else if (!timeValid) {
        on = (ms % 2000) < 100 || ((ms % 2000) >= 250 && (ms % 2000) < 350); // double flash
    } else {
        on = (ms % 2000) < 60;        // heartbeat: all good
    }
    digitalWrite(PIN_LED, on ? LOW : HIGH);
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
    Serial.printf("\nAIO-Feeder node v%d.%d\n", FW_MAJOR, FW_MINOR);

    if (!LittleFS.begin()) {
        LittleFS.format();
        LittleFS.begin();
    }
    loadAll();

    Wire.begin(PIN_SDA, PIN_SCL);
    rtcPresent = rtc.begin();
    timeValid = rtcPresent && !rtc.lostPower();
    Serial.printf("[rtc] present=%d valid=%d\n", rtcPresent, timeValid);
    if (timeValid) {
        reconcileScheduleMark(nowEpoch());
    }

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
    setChannel(cfg.channel);
    Serial.printf("[link] mac=%s paired=%d channel=%u entries=%u rev=%u\n",
                  WiFi.macAddress().c_str(), cfg.paired, cfg.channel, cfg.count, cfg.sched_rev);

    if (!cfg.paired) {
        enterPairing();
    }
}

void loop()
{
    feedLoop();
    scheduleLoop();
    linkLoop();
    buttonLoop();
    ledLoop();
    yield();
}
