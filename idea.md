# Main Idea Page

Design rationale and open decisions. Work actually performed is recorded in [log.md](log.md).

## Table of contents

1) [What am I solving?](#what-am-i-solving)
2) [Hardware](#hardware) — Main board · HVAC interface · Satellites
3) [Software](#software) — Control loop · Program structure · Short-cycling · Failure handling · Persistent settings · Web UI ↔ Pi · Logging · Placement
4) [Constraints](#constraints)
5) [MVP](#mvp)

## What am I solving?

Four problems, roughly in priority order:

- **Office overheats during long gaming sessions.** Currently I walk downstairs and manually cycle the HVAC fan. I want that automatic.
- **Bedroom air goes stagnant overnight** with the door closed, even when the rest of the apartment is cool. Wants periodic fan cycling.
- **Sleep/wake temperature curve.** Cold while I'm in bed, warm by the time I need to get up.
- **Away/return scheduling.** I turn the HVAC down when I travel, and the first few hours back are miserable. I want remote control plus a scheduled pre-conditioning before I return.

Success criteria is intentionally subjective: does my room stay comfortable. Rooms are not expected to hold an exact setpoint — temperature roams within a range as the system cycles. Accuracy within about a degree is fine.

### Control surfaces

- **Web UI**, in two separate forms:
  - **Control UI** — reachable only on my Tailscale tailnet. No login, since the tailnet already restricts it to trusted devices. This is where trip/return scheduling lives.
  - **Demo UI** — public, read-only. Shows current temps and explains the system. Portfolio purposes.
- **Physical controls on the thermostat housing** — raise/lower buttons and a screen showing current and set temp. Serves as backup and as a debugging surface.
- **Voice control** — stretch goal, explicitly not in MVP. Mostly for fun, but also a path toward a self-hosted Alexa replacement later.

## Hardware

Raspberry Pi 3 as the main thermostat: interfaces with the existing thermostat wiring, drives a local display, receives readings from satellite sensors, and runs all control calculations.

### Main board

**Power.** Outlet-powered rather than power-stealing off the 24VAC loop. Simpler, needs no C-wire, and avoids the leakage-current tricks some furnace boards tolerate poorly. Consequence: the Pi's 5V domain and the HVAC's 24VAC domain are fully separate with no shared ground, which the relay interface must respect.

**Temperature sensing.** The Pi carries its own SCD41 (same part as the satellites — see [Satellites](#satellites)). It must sit a short cable-length away from the Pi board rather than mounted against it: the Pi runs measurably warmer than an idle ESP32, and the control scheme depends on a degree-level deadband. A placement constraint for the enclosure, which hasn't been designed yet.

**Display.** ILI9341 TFT, 2.4", 240x320, SPI. Color, so heat/cool/idle state can be color-coded. Driven either via the `fbtft` framebuffer overlay or a hand-written SPI init sequence.

**Buttons.** Two momentary pushbuttons (raise/lower) on GPIO with internal pull-ups. Not yet purchased.

### HVAC interface

**Existing thermostat (Honeywell RTH111B1001).** Four wires only: **R, G, W, Y**. No C-wire at the thermostat — it's battery-powered, so it never needed one. A C may still be landed at the furnace board; unverified. R is a single wire with the factory Rh/Rc jumper in place.

| Terminal | Function | Rating |
|---|---|---|
| R | 24VAC in from furnace transformer (switched, not sourced, by the thermostat) | — |
| W | Heat call | 1.0A @ 24VAC |
| Y | Cooling call (compressor contactor) | 1.0A @ 24VAC |
| G | Fan, independent of heat/cool | 0.5A @ 24VAC |

No O/B (heat pump reversing valve), no second stage — matches the scope here (single-stage furnace/AC/fan).

**Relay plan.** Three relays (W, Y, G). R feeds each relay's common; each normally-open contact goes out to its call wire. Each coil is driven independently, so any combination is possible (e.g. Y+G together for a cooling call).

Relays are wired **NO, never NC**, so fail-safe-off is a property of the hardware rather than of software: a crashed Pi, lost power, or a dead relay supply de-energizes every relay and drops every call signal.

Part is an **FL-3FF-S-Z**, 5V coil, on an opto-isolated board with **low-level trigger** — GPIO low energizes, GPIO high or floating releases. The board's per-channel pull-up holds the input high whenever nothing drives it, and the Pi's GPIOs come out of reset as high-impedance inputs, so relays stay open through boot, a crash, or a missing control program. *Verified on the bench — see [log.md](log.md).*

Still worth checking while the furnace is accessible: whether the control board has a spare C terminal, in case a real C-wire run is ever wanted.

**Wire gauge.** Any new or extended run uses 18 AWG thermostat cable, matching what's already in the wall. Ampacity doesn't require it — 22 AWG would carry these loads fine — but it keeps voltage drop negligible, holds up better at splices, and avoids a gauge mismatch at the junction.

### Freeze-protection backup

Rejected automatic failover hardware (watchdog + changeover relay bank detecting a hung Pi). Overkill for the only failure mode that matters: pipes freezing while away in winter if the Pi dies. A power outage needs no separate protection, since the furnace itself can't run without house AC regardless of what any thermostat says.

Instead the old Honeywell stays permanently wired **in parallel on W only** — not G or Y, since fan/cooling redundancy isn't a goal. R is shared (it's a supply rail, not switched, so no conflict). Two switches in parallel across R/W is the standard freeze-stat pattern: either one closing calls heat, and the Honeywell can only ever *add* a heat call, never block or override the Pi's. They can't fight.

The Honeywell sits at a low freeze-protection setpoint kept a few degrees **below** the Pi's away-mode setpoint (e.g. ~55-58°F vs ~60-62°F). Matching them exactly isn't unsafe, but it would put two independently-hysteresised controllers on the same threshold, and the Honeywell knows nothing about the Pi's minimum-off-time protection — it could fire during a window the Pi is deliberately sitting out. The gap means it almost never fires under normal operation.

Since it's now the actual last line of defense, **its battery health is safety-critical** and needs a recurring replacement reminder rather than reliance on its low-battery indicator.

*Pending bench test:* confirm normal Pi operation above the Honeywell's threshold never triggers it, and that dropping simulated temp below its setpoint fires the furnace through the shared W wire regardless of the Pi's state.

### Satellites

ESP32-WROOM-32 DevKitC-style boards (dual-core, 4MB flash, USB-C, CH340C). Wall-powered, so battery life isn't a constraint — "low power" here just means not running hardware hotter than it needs to be.

Each satellite uses a **Sensirion SCD41** (I2C) rather than a plain temp/humidity part. It gives temperature and humidity like a BME280 would, but also **CO2** — a far better proxy than humidity for the "stagnant air" the bedroom fan cycling is meant to fix. One sensor covers all three readings. Bought as a breakout module, not the bare IC: the bare part needs reflow plus specific decoupling and I2C pull-ups, not worth reproducing by hand for a first build.

No microphone or speaker in the MVP. Far-field pickup over HVAC noise would need substantially more compute per satellite plus a mic array — a separate hardware revision if ever pursued.

**The Pi pulls; satellites don't push.** Each satellite serves `GET /reading` over HTTP, returning JSON (temp/RH/CO2 plus the time it was measured). The Pi polls each one every ~60 s. Pulling means a request timeout *is* the "satellite unresponsive" signal — no separate liveness tracking. Every device is reachable with `curl` for debugging. Same JSON-over-HTTP approach as the web UI's link to the Pi (see [Web UI ↔ Pi](#web-ui--pi)), so there's one protocol across the system.

Addresses are fixed by **DHCP reservations on the router**, not static IPs configured on each board — one place to manage, and satellite firmware stays plain DHCP.

Each satellite keeps its SCD41 in periodic mode continuously and answers with the latest cached reading. It must never restart the sensor per request: the SCD41 reads ~7 °F high for ~3 minutes after every start (see [log.md](log.md), 2026-09-27).

**Each satellite owns its own warm-up** and reports it, so a Pi reboot while the satellites keep running doesn't force a needless 3-minute wait. The reply carries an explicit status rather than nulls, because the Pi must treat "warming" (expected, temporary, not a failure) differently from "error" (counts toward failure limits):

```json
{"status": "ok",      "temp": 71.2, "humidity": 44.1, "co2": 612}
{"status": "warming", "temp": null, "humidity": null, "co2": null}
{"status": "error",   "temp": null, "humidity": null, "co2": null}
```

No reply is a timeout in `net_request` and never reaches the JSON. Satellites reuse the Pi's `sensor.c` unchanged with the ESP32 Sensirion HAL, including the same "already measuring → already warm" check, since an ESP32 reset may not cut the sensor's power either. Satellites send raw readings; **validation happens on the Pi** (see [Failure handling](#failure-handling)).

## Software

### Control loop

The Pi reads its own sensor plus both satellites, then decides. Response logic is the same for every room, with per-zone parameters:

- **Office** — most aggressive about starting the AC, provided the rest of the apartment isn't already too cold.
- **Bedroom** — mainly keeping it from getting too hot; more control authority midnight to 8am.
- **Downstairs** — secondary to both, acting as an *energy reserve*. If the office is hot, pull cool air from downstairs with the fan before involving the AC.

The goal driving all of it: **stay comfortable for as little money as possible.** Try the fan first — move air from the reserve. Only bring in the furnace or AC if the fan alone isn't fixing it.

**Combining zones.** A weighted average of the three sensors drives the power-hungry decision: average outside the deadband → heat or AC; inside it → fan or nothing. The average alone can't drive the fan, though — office 78 °F plus downstairs 68 °F averages to ~73 °F, inside the deadband, which is exactly when the fan *should* run. So the fan decision uses the **spread between zones** (warmest − coolest, or each zone's distance from its own target) plus bedroom CO2. *Spread-based fan logic is a proposal, not yet settled.*

Weights change by time of day — the bedroom's extra authority midnight–8am is just a different weight table on a schedule. A zone that is stale or rejected is dropped and the remaining weights are rescaled to sum to 1, so the loop degrades to two sensors, or the Pi's own, rather than failing.

### Program structure

One single-threaded **event loop**. The loop runs forever, but it never busy-waits: each pass blocks in `poll()` on every input — HTTP sockets, button GPIO edge events (libgpiod v2 `gpiod_line_request_get_fd()`), later touch (`/dev/input/eventN`), and `signalfd` for SIGTERM. `poll()` returns the moment any of them is ready, so input is handled within milliseconds even while the furnace or AC is running.

- **Heating/cooling is a state, not a loop.** State is `IDLE / HEATING / COOLING / FAN`. Each pass checks whether it should change, acts, and returns. Nothing waits inside a handler.
- **`poll()` timeout = time until the earliest deadline**, not a fixed sleep. Each periodic job keeps its own next-due time (control step ~30 s, satellite poll ~60 s, display clock ~1 s if shown). Multiple `timerfd`s are an equivalent option.
- **The decision is a pure function**: `decide(state, readings, settings, now) → new state`. It touches no hardware, so the deadband and minimum-off-time logic can be tested on a desktop with fake readings.
- **Display redraws only when something changed** (a dirty flag set by new readings, setpoint changes or relay changes), once per pass, and only the changed region. A full 320×240 RGB565 frame is ~150 KB, ~150 ms at the current 8 MHz SPI. Short timeouts (~33 ms) only during animation or touch drag.
- **No blocking calls inside the loop.** `get_sensor_data()` does a single data-ready check and never waits (done 2026-10-03). Satellite fetches go through libcurl's multi interface (plugs into `poll()`), or at minimum a short timeout. The HTTP server has to run inside this loop: Mongoose is designed for that, and libmicrohttpd supports it in "external select" mode. *Library choice still open.*
- **On SIGTERM, switch every relay off before exiting** — the software half of the fail-safe.
- All timing on `CLOCK_MONOTONIC`, never wall-clock time.

### Avoiding short-cycling

The real risk is the AC and furnace fighting each other — AC overshoots cold, furnace compensates, overshoots warm, AC kicks back on. Three mechanisms, no physical mode switch:

1. **Deadband instead of a single setpoint.** E.g. AC on at 74°F / off at 71°F; furnace on at 69°F / off at 72°F. The gap between them is a band where neither runs.
2. **Seasonal lockout derived from the weather API**, not set by hand. Rolling outdoor average above ~65°F = AC-only; below ~55°F = furnace-only; between = swing season where either may run under the deadband. This is the main use of the weather API — delaying or skipping a cycle rather than reacting live.
3. **Hard minimum off-time per system** (~5 min) regardless of temperature, to protect the compressor.

### Failure handling

An unreachable weather API is not a failure needing fallback — its only role is to delay or skip decisions, so losing it just means reacting to raw indoor temperature.

#### Sensor validation (decided 2026-10-02)

Control logic sees only trustworthy numbers: it checks one flag per sensor, `health.trusted`, and an untrusted sensor is dropped from the weighted average (see [Combining zones](#control-loop)). Everything else lives in one layer.

- **Three layers.** `sensor.c` is a thin C driver: raw reading plus `read_ok` ("the I2C read worked", nothing more). `update_node()` in C++ validates *every* node the same way — the local SCD41 and both satellites — so the checks exist once. `control.cpp` checks `trusted`. Validation isn't in `sensor.c` because `sensor.c` only knows the local sensor, and some checks (no reply, stale data) are only visible from the Pi.
- **Two flags, not one.** `read_ok` and `trusted` are different questions: a read can succeed and still return garbage.
- **Range:** 50–110 °F.
- **Rate of change vs the *previous* reading**, trusted or not, in °F per minute; more than 20 °F in 3 min is a failing sensor. Comparing with the last *trusted* value instead would lock a sensor out forever after a real step change (e.g. a satellite moved near a window). As a rate rather than a fixed delta, a sensor returning after an hour offline doesn't look like a jump.
- **Re-trust after 3 consistent readings in a row**, so a spike that settles stays rejected and a real change is accepted after ~3 reads.
- **Blips:** on a failed read, keep using the last good reading for up to 3 min, then drop the sensor. Avoids the ~2 °F jump in the weighted average each time a sensor flickers. A rolling per-room offset estimate was considered and rejected: occupancy (body heat) makes the offset unstable in exactly the room that matters, and the extra complexity isn't worth it — the system is already equalizing when a jump happens, and min on/off times cap any extra cycling.
- **Weights when a sensor drops:** remaining weights are renormalized by dividing by their sum; their ratios are unchanged.
- **Warm-up, local sensor:** the SCD41 can't report how long it has been on. `scd41_start()` returns whether it started measuring or found it already running (the ASC-disable command is rejected while measuring). Already running → already warm, `started_at = now - SENSOR_WARMUP`; fresh start or error → wait the full 3 min. `started_at` must always be set explicitly: its default 0 on the monotonic clock means "since boot", i.e. falsely warm. Known gap: a crash within 3 min of a fresh start, then a restart, is treated as warm while the sensor still reads a few degrees high. Fix if needed: record the start time in `/run` (cleared on reboot) and wait the full warm-up when it's missing.
- **Warm-up, satellites:** reported by the satellite (`"status": "warming"`), see [Satellites](#satellites).

### Persistent settings

The Pi keeps its settings in a file so they survive a restart: heat setpoint, AC setpoint, and vacation mode (departure time, return time, away min/max temps, and a pre-conditioning lead time — how long before arrival away mode ends, which is what fixes the "miserable first hours back" problem).

- **Location:** `/var/lib/thermostat/` (standard for service state).
- **Atomic writes:** write to a temp file, `fsync`, then `rename()` over the old one. A rename is all-or-nothing, so a power cut mid-save can't corrupt the setpoints.
- **Validated on load**, falling back to built-in safe defaults (and logging it) if the file is missing or garbled.
- **Written only on change**, not every loop — SD card wear.
- **Only the Pi writes it.** The web UI sends a request; the Pi validates it (e.g. heat setpoint below AC setpoint by at least the deadband) and saves. One source of truth.
- **Times stored as UNIX timestamps (UTC)** — no time-zone or DST bugs.
- Format: `key=value` lines are easiest to parse by hand in C; JSON (via cJSON) if it's shared with the API code. *Open.*

### Web UI ↔ Pi

The Pi exposes a small **JSON-over-HTTP API**; the Ubuntu web app is its only client. Browsers never talk to the Pi directly. Endpoints along the lines of `GET /status`, `PUT /setpoints`, `PUT /vacation`, `GET /history`, `GET /events`.

The Pi's current sluggishness is VS Code's remote server and IntelliSense, not network load — an API called a few times a minute by one client costs effectively nothing.

Rejected alternatives:
- **MQTT (Mosquitto)** — the home-automation standard, decoupled, retained messages. Adds a broker to run, and a broker on Ubuntu would make satellite data depend on Ubuntu being up. Worth revisiting if voice control or more devices happen; the broker would then live on the Pi.
- **Raw TCP with a custom protocol** — means reinventing message framing, errors and debugging tools.

Rules regardless of transport:
- **The thermostat runs fully with Ubuntu down.** The web UI is only a remote control; the settings file keeps the last setpoints.
- **The public demo has no route to write endpoints.** It reads status the Ubuntu server has already stored; demo requests are never passed through to the Pi.
- **LAN-only still means anyone on the Wi-Fi.** Writes are accepted only from the Ubuntu server's IP, or the API listens only on the Pi's Tailscale interface (consistent with the control UI being tailnet-only).

### Logging

At minimum: timestamp, per-zone temp/humidity/CO2, current mode (AC/furnace/idle/lockout reason), and every state transition with its trigger reason. Needed specifically to verify the deadband and lockout logic actually prevent short-cycling.

**7-day temperature history on the website** (nice-to-have, not required). The Ubuntu server stores the history; the Pi does not.

- **The Pi keeps a RAM ring buffer** of recent readings (e.g. 24 h at 1-min intervals = 1,440 entries) and serves `GET /history?since=<unix ts>`. Ubuntu asks for everything after its last stored timestamp, so an Ubuntu outage backfills automatically instead of leaving a gap. RAM-only — no SD writes.
- **State transitions are a separate event buffer** (`GET /events?since=...`: time, old → new state, reason). Samples alone miss cycles shorter than the sample interval, and the transitions are what prove short-cycling protection works. Charted as shaded "AC on" / "heat on" bands behind the temperature lines.
- **Sample every 1 min** (5 at most), not 10 — 10-minute samples smooth over exactly the cycles worth seeing. ~4,300 rows/day across three zones is nothing.
- **Timestamps come from the Pi, in UTC**, taken when the reading is taken, not when Ubuntu fetches it. The Pi 3 has no battery-backed clock, so nothing is recorded until the clock has synced with a time server (`timedatectl` shows sync status).
- **Ubuntu side:** SQLite, one table (timestamp, zone, temp, RH, CO2, HVAC state). Keep everything (a year is tens of MB) and display the last 7 days. Charts with uPlot or Chart.js. The public demo reads the same database.

### Where software lives

**Satellites** run a lightweight program that serves their latest reading over HTTP when the Pi asks.

**The Pi** is the central hub: polls the satellites, reads its own sensor, fetches outside weather, drives the HVAC relays and the local display, and accepts commands from the web UI and physical buttons. No Docker — the project is simple and needs direct board IO, so containerizing costs more than it returns.

**Development** happens on the Ubuntu server, not on the Pi (decided 2026-10-02). `pi/` in the project folder mirrors `~/thermostat` on the Pi exactly and is the source of truth; `deploy.sh` rsyncs it over SSH and builds/runs it there. The Pi carries only what it runs — no images, docs or datasheets. A script beat deploying via GitHub: no credentials on the Pi, no commit per test. GitHub ([dillanhart/multi-room-hvac](https://github.com/dillanhart/multi-room-hvac)) is the portfolio record only. IntelliSense checks against a copy of the Pi's headers (`.sysroot/`), so a library missing on the Pi shows as an error on the dev machine, and `./deploy.sh build` compiles on the Pi as the final word.

**The Ubuntu server** hosts the web UI. This adds complexity, for two reasons: it allows a public demo site for portfolio purposes, and if the self-hosted Alexa idea goes ahead it'll need the Ollama install already running there — so working out cross-machine communication now makes that expansion easier.

## Constraints

- **C++ wherever possible.** A primary goal of the project is getting more familiar with it.
- **No AI-generated code.** Generative AI is used for planning and research only. Vibe-coding an app would defeat the point of learning C++.

## MVP

Main board (Pi) + **one** satellite (ESP32 + SCD41) + both web UIs. Anything less doesn't give real signal on whether the multi-zone approach works. Voice control and additional satellites come after.
