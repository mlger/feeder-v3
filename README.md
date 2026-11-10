# AIO-Feeder

Automatic fish feeder for several tanks. A touchscreen controller (the HMI)
manages one feeder node per tank over ESP-NOW.

| Part | Hardware | Firmware |
| --- | --- | --- |
| **HMI** | Waveshare ESP32-S3-Touch-LCD-4.3B | ESP-IDF 6.1 + LVGL 9.4.0 with XML screens (PlatformIO) |
| **Node** | ESP8266 NodeMCU V3 + DS3231 + relay + 12 V solenoid (JF-0530B) | Arduino-ESP8266 (PlatformIO) |

## How it works

- The HMI is the source of truth. It pushes the schedule, the limits and the
  time to each node, and shows status, history and alerts.
- Each node stores the schedule and runs it from its own DS3231, so feeding
  continues when the HMI is off or out of range.
- A feed is `pulses` relay activations of `on_ds` x 0.1 s each.
- Nodes report every feed (or refused feed) back and resend until the HMI
  acknowledges it.

### Safety rules enforced by the node

- The relay is driven off before anything else at boot.
- Each pulse is clamped to a compile-time maximum (5.0 s by default, set by
  `FD_HARD_MAX_ON_DS` in `node/platformio.ini`). No message can exceed it.
- An independent timer forces the relay off if the main loop stalls.
- Cooldown between feeds and a daily on-time cap, both set from the HMI.
- A feed is recorded to flash before the relay is energised, so a power cut
  mid-feed never causes a repeat.
- Commands are accepted only from the paired HMI.
- Scheduled feeds do not run until the clock has been set.

## Repository layout

```
shared/   feeder_core library (wire protocol, schedule, safety logic) + host unit tests
node/     ESP8266 node firmware
hmi/      ESP32-S3 touchscreen firmware
  ui/       the screens, as LVGL XML loaded at run time
  lib/      feeder_app: application model + UI glue (portable)
  src/      board support, ESP-NOW, storage (ESP-IDF)
  sim/      host simulator: runs the real UI against fake feeders
web/      GitHub Pages: web flasher, demo, guide   (not started yet)
```

LVGL is pinned to 9.4.0 because it is the last release that can load XML at
run time; later releases removed the loader.

## Build

Requires PlatformIO Core 6.2.0.

```bash
pio test -d shared
```

```bash
pio run -d node
```

```bash
pio run -d node -t upload
```

```bash
pio run -d hmi
```

```bash
pio run -d hmi -t upload
```

### HMI simulator

The simulator builds the HMI application for the PC with a headless display.
It pairs two simulated feeders, edits a schedule, runs scheduled and manual
feeds, checks the results and writes a screenshot of every screen.

```bash
git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git /tmp/lvgl
```

```bash
cmake -S hmi/sim -B build/sim -DLVGL_DIR=/tmp/lvgl && cmake --build build/sim
```

```bash
mkdir -p build/shots && build/sim/feeder_sim build/shots
```

## Using the HMI

| Page | What it does |
| --- | --- |
| Home | One card per tank: state, last and next feed, today's total, **Feed now** |
| Schedule | Feeding times per tank (time, days, pulse length, pulses) and the safety limits |
| Feeders | Paired feeders (rename, remove) and feeders in pairing mode (add) |
| History | The last 64 feeds and refused feeds |
| Settings | Clock, manual feed portion, theme, screen-off time |

To add a feeder, open **Feeders**, hold the feeder's FLASH button for 3 s and
tap **Add** when it appears. The HMI then sends it the time and its schedule.

The HMI clock comes from the board's RTC. Set it once in **Settings**; every
feeder is kept within 5 s of it. Wi-Fi, NTP, OTA updates and SD-card logging
are not implemented yet.

## Node wiring (NodeMCU V3)

| Signal | Pin | Notes |
| --- | --- | --- |
| Relay IN (active high) | D5 / GPIO14 | Add a 10 k pull-down to GND so the relay stays off during reset |
| DS3231 SDA | D2 / GPIO4 | DS3231 powered from 3V3 |
| DS3231 SCL | D1 / GPIO5 | |
| Pair button | FLASH / GPIO0 | Hold 3 s to enter pairing mode |
| Status LED | on-board / GPIO2 | See below |

The relay contacts switch the solenoid's own 12 V supply. Fit a flyback diode
(for example 1N4007) across the solenoid coil, cathode to +12 V. Check the
coil current against the relay's contact rating and the 12 V supply.

### Status LED

| Pattern | Meaning |
| --- | --- |
| Fast blink | Pairing mode, waiting for an HMI |
| Slow blink | Paired, searching for the HMI (schedule still runs) |
| Double flash | Linked, but the clock is not set |
| Short flash every 2 s | Linked and healthy |
| Solid | Feeding |

## Protocol

See [`shared/lib/feeder_core/include/feeder/proto.h`](shared/lib/feeder_core/include/feeder/proto.h).
The HMI may be on any Wi-Fi channel; an unlinked node walks channels 1-13
until its HMI answers.
