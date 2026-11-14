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
shared/   feeder_core library: wire protocol, schedule, safety logic and the
          feeder's whole state machine, with host unit tests
node/     ESP8266 node firmware (hardware glue around the shared state machine)
hmi/      ESP32-S3 touchscreen firmware
  ui/       the screens, as LVGL XML loaded at run time
  lib/      feeder_app: application model + UI glue (portable)
  src/      board support, ESP-NOW, storage (ESP-IDF)
  sim/      host simulator and the WebAssembly demo
web/      GitHub Pages site: web flasher, browser demo, guide
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

### Portions in grams

The feeder has no scale. To work in grams, calibrate each feeder once:
**Feeders > Calibrate** runs 10 pulses into a cup, you weigh the food and
enter the weight. The controller stores grams-per-pulse for that feeder and
from then on shows and takes portions in grams (schedule editor, Feed now,
Home, History, the daily limit and the SD-card log).

- Portions move in steps of one pulse. You can type any amount in tenths of
  a gram: the controller uses the nearest whole number of pulses (half a
  pulse rounds up) and says so, e.g. "asked 2.5 g, gives about 2.4 g".
- The grams you asked for are remembered with each feeding time. If you
  re-calibrate, those feeding times are re-planned and sent to the feeder
  again. Feeding times set in pulses are left alone.
- Feed now opens a dialog with the amount in grams; it starts at the amount
  you used last. The daily limit is also set in grams.
- Grams are only claimed for feeds that use the calibrated pulse length;
  the schedule editor fixes the pulse length for a calibrated feeder.
- The run is a normal manual feed: the cooldown and daily limit apply, and
  it counts towards the day's total.
- It is an estimate. Re-calibrate when the food changes. Entering 0 removes
  the calibration.
- The conversion lives on the controller only. Feeders still receive pulses,
  so their hard limits work exactly as before.

### Planning by flow rate (grams per second)

By default an amount is run as whole pulses. Under **Feeders > Calibrate**
a calibrated feeder has a switch, *Plan amounts by flow rate (g/s)*. With it
on, the controller works out the flow rate from the calibration (grams per
pulse over the pulse length, e.g. 0.6 g in 1.0 s is 0.6 g/s) and plans every
amount as relay time, in steps of 0.1 s: 2.5 g becomes one run of 4.2 s,
which gives about 2.5 g.

- Anything longer than the hard limit for one run (5.0 s) is split into equal
  runs: 7 g is 3 runs of 3.9 s. One feed can give up to 20 runs of 5.0 s.
- The schedule editor, Feed now, the daily limit, Home, History and the SD
  log all work in grams as before; the editor and Feed now say what they will
  run, e.g. "= 4.2 s, gives about 2.5 g".
- It is per feeder. Switching re-plans the feeding times that were set in
  grams and sends them to the feeder; ones set in pulses are left alone.
- It assumes the food flow is proportional to relay time. A very short run
  may drop less than the rate suggests because the valve needs a moment to
  open, so calibrate with a pulse length close to the ones you use. Weighed
  feeds, if you have a scale, keep the rate honest.
- The feeders need no update for this: they still receive an on-time and a
  number of runs.

### Optional scale under the hopper

A feeder can carry a load cell (HX711 on D6/D7) under its food hopper. It is
detected by itself; feeders without one behave exactly as before.

- Each feed is weighed as what the hopper lost, and History and the SD log
  record it as `weighed` instead of `estimated`.
- The Home card shows the food left and the weighed total for the day.
- Alerts: food low (under 20 g), and "little food came out" when a feed gives
  less than a quarter of what was expected (a jam or an empty hopper).
- Weighed feeds keep the grams-per-pulse estimate up to date, so a feeder
  with a scale needs no manual calibration.
- Set-up: **Feeders > Calibrate > Scale**, then **Zero** with the hopper
  empty and **Set weight** with a known weight in it.

The scale measures only. Feeds are still a fixed number of pulses; the
feeder does not keep pulsing until a target weight is reached.

Controller and feeders must run the same version from 0.6.0 on: the
messages between them changed to carry the weights.

### Network page

| Card | What it does |
| --- | --- |
| Wi-Fi | Scan, pick a network, type the password. The network is remembered. **Use phone** sets it up from a phone instead. |
| Internet time | Set your UTC offset, then switch on. The clock then follows NTP (`pool.ntp.org`). |
| Firmware updates | Shows the address of the controller's upload page. It is always open, with no password. |

Without Wi-Fi the clock comes from the board's RTC: set it once in
**Settings**. Either way every feeder is kept within 5 s of the controller.
There is no automatic daylight-saving change; adjust the UTC offset by hand.

Wi-Fi is optional. Feeders talk to the controller over ESP-NOW on whatever
channel the controller is on, and find it again by themselves when joining a
network moves it to another channel (this takes up to a minute).

## Firmware updates

Both devices can be updated without a cable once the controller is on Wi-Fi.

1. On the controller open **Network** and note the address under **Firmware updates**.
2. In a browser on the same network open that address.
3. **Controller:** upload `hmi/.pio/build/hmi/firmware.bin`. It restarts into
   the new version; if that version fails to start, the next reset returns to
   the previous one.
4. **Feeders:** upload `node/.pio/build/nodemcuv2/firmware.bin`, then press
   **Update** next to each feeder.

A feeder has no network connection of its own. For an update the controller
opens a temporary access point with a random password, tells the feeder over
ESP-NOW, and serves it the file. The feeder checks the file's MD5 before
switching, and restarts on its old firmware if anything goes wrong. It is off
the air for about a minute and skips any feed due in that time.

The update page is open: plain HTTP, no password and no switch to turn on.
Anyone who can reach the controller on your network can replace the firmware
of the controller or of a feeder, so only use it on a network you trust.

### Wi-Fi set-up from a phone

Press **Use phone** on the Network page. The controller opens a temporary
access point, `AIO-Feeder-Setup`, with a random 8-digit password, and shows
both along with a QR code. Join it with a phone, open `http://192.168.4.1`,
choose the home network and type its password. The access point closes once
the details are sent, on **Cancel**, or after 5 minutes. It is never open
unless someone starts it at the touchscreen.

## SD card log

With a FAT-formatted card in the controller's slot, every feed and every
refused feed is appended to `feedlog.csv`:

```
time,feeder,source,result,seconds,pulses,grams,grams_are,grams_asked
2026-11-09 08:00:00,"Goldfish",schedule 1,fed,1.0,1,0.6,weighed,0.6
2026-11-09 08:01:12,"Tetras",manual,Skipped: cooldown,0.0,0,,,
```

The History page shows whether a card is in use, and the update web page has
a link to download the file. The card can be inserted or removed while the
controller runs. Events that happen while no card is inserted are not written
later; the on-screen history still keeps the last 64. The controller never
formats a card.

## Versions and releases

The version is written once, in `VERSION` at the repository root. Both
firmwares, the simulator and the web site take it from there.

To publish a release, set `VERSION`, commit, and push a matching tag:

```bash
git tag v$(cat VERSION) && git push origin v$(cat VERSION)
```

Or, without a local clone: on GitHub open **Actions > build > Run workflow**,
choose `main` and tick **Publish a release**.

Either way CI attaches the controller and feeder firmware files to a GitHub
Release. Those are the files to upload on the controller's update page.

## Security notes

This is built for a home network, and its protections are physical:

- The controller's web page is plain HTTP with no login, and firmware
  uploads (controller and feeders) are always open to anyone on the same
  network. This is deliberate. Keep the controller on a network you trust,
  and never forward its port to the internet.
- The two temporary access points (phone set-up, feeder update) use a fresh
  random password each time and exist only while they are needed.
- ESP-NOW traffic between controller and feeders is not encrypted. A feeder
  only obeys the controller it was paired with, pairing needs its button,
  and its hard limits, cooldown and daily cap bound what any command can do.
- Do not expose the controller to the internet.

## Web site

The site in `web/` has three pages: an install page that flashes either
board from the browser (ESP Web Tools, Chrome or Edge on a desktop), the
controller running as a WebAssembly demo against simulated feeders, and a
build guide.

CI assembles the site on every push and publishes it from the default
branch. To switch publishing on, open the repository's **Settings > Pages**
and set **Source** to **GitHub Actions**. The site then appears at
`https://mlger.github.io/feeder-v3/`.

The install page has a **Version** picker: the latest build and every GitHub
release. CI downloads the files of each release when it assembles the site.
Releases made from 0.9.0 on also carry a complete controller image
(`aio-feeder-controller-full-v<version>.bin`: bootloader, partition table and
app). For older releases the site combines their app image with the bootloader
and partition table of the latest build, which is safe because the partition
layout has not changed. Install the controller and the feeders from the same
version.

To build it locally (the demo needs Emscripten; on Ubuntu 24.04
`sudo apt install emscripten`):

```bash
emcmake cmake -S hmi/sim -B build/web -DLVGL_DIR=/tmp/lvgl -DCMAKE_BUILD_TYPE=Release && cmake --build build/web
```

```bash
python3 web/build_site.py --out _site --version dev --hmi-dir hmi/.pio/build/hmi --node-bin node/.pio/build/nodemcuv2/firmware.bin --demo-dir build/web
```

```bash
python3 -m http.server -d _site 8000
```

## Node wiring (NodeMCU V3)

| Signal | Pin | Notes |
| --- | --- | --- |
| Relay IN (active high) | D5 / GPIO14 | Add a 10 k pull-down to GND so the relay stays off during reset |
| DS3231 SDA | D2 / GPIO4 | DS3231 powered from 3V3 |
| DS3231 SCL | D1 / GPIO5 | |
| DS1302 CLK / DAT / RST | D1 / D2 / D0 | Only with a DS1302 module instead of the DS3231; build the `nodemcuv2_ds1302` environment (`pio run -d node -e nodemcuv2_ds1302`). Power from 3V3 |
| HX711 DT (optional scale) | D6 / GPIO12 | Load cell under the hopper |
| HX711 SCK (optional scale) | D7 / GPIO13 | |
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
