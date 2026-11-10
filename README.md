# AIO-Feeder

Automatic fish feeder for several tanks. A touchscreen controller (the HMI)
manages one feeder node per tank over ESP-NOW.

| Part | Hardware | Firmware |
| --- | --- | --- |
| **HMI** | Waveshare ESP32-S3-Touch-LCD-4.3B | ESP-IDF 6.1 + LVGL 9 (PlatformIO) |
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
hmi/      ESP32-S3 touchscreen firmware            (not started yet)
web/      GitHub Pages: web flasher, demo, guide   (not started yet)
```

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
