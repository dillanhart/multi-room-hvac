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
- **Physical controls on the thermostat housing** — a touchscreen showing current and set temp (raise/lower pushbuttons possibly kept as a fallback). Serves as backup and as a debugging surface.
- **Voice control** — stretch goal, explicitly not in MVP. Mostly for fun, but also a path toward a self-hosted Alexa replacement later.

## Hardware

Raspberry Pi 3 as the main thermostat: interfaces with the existing thermostat wiring, drives a local display, receives readings from satellite sensors, and runs all control calculations.

### Main board

**Power.** Outlet-powered rather than power-stealing off the 24VAC loop. Simpler, needs no C-wire, and avoids the leakage-current tricks some furnace boards tolerate poorly. Consequence: the Pi's 5V domain and the HVAC's 24VAC domain are fully separate with no shared ground, which the relay interface must respect.

**Temperature sensing.** The Pi carries its own SCD41 (same part as the satellites — see [Satellites](#satellites)). It must sit a short cable-length away from the Pi board rather than mounted against it: the Pi runs measurably warmer than an idle ESP32, and the control scheme depends on a degree-level deadband. A placement constraint for the enclosure, which hasn't been designed yet.

**Display.** ST7789V TFT, 240x320 (run as 320x240 landscape), SPI0. Color, so heat/cool/idle state can be color-coded. Driven by the mainline `panel-mipi-dbi` DRM driver (`fbtft` was tried and rejected on the earlier ILI9341 module — see [log.md](log.md), 2026-09-22). Runs as its own program, separate from control (see [Program structure](#program-structure)). Replaced the ILI9341 module on 2026-11-18, which had no touch controller fitted.

**Touch.** Resistive, XPT2046-compatible controller on its own bus (SPI1) with a pen-down IRQ on GPIO 16, so touch reads never share a bus with display updates. Raw readings need filtering (stray values at touch-down/lift) and a calibration that swaps the axes for landscape. Kernel driver vs. userspace reads: *open*.

**Buttons.** Two momentary pushbuttons (raise/lower) on GPIO with internal pull-ups, read by the display program. Not yet purchased. *Open:* drop them for touch, or keep them as a fallback if the touchscreen fails.

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

**Combining zones.** A weighted average of the three sensors drives the power-hungry decision: average outside the deadband → heat or AC; inside it → fan or nothing. The average alone can't drive the fan, though — office 78 °F plus downstairs 68 °F averages to ~73 °F, inside the deadband, which is exactly when the fan *should* run. So the fan decision uses the **spread between zones** (warmest − coolest, or each zone's distance from its own target) plus bedroom CO2. *Spread-based fan logic is not yet settled; a first cut (warmest − coolest > `FAN_TRIGGER`) is in `get_HVAC_command`.*

Weights change by time of day — the bedroom's extra authority midnight–8am is just a different weight table on a schedule. A zone that is stale or untrusted is dropped and the remaining weights are divided by their sum (ratios unchanged), so the loop degrades to two sensors, or the Pi's own, rather than failing.

### Program structure

**Two programs** (decided 2026-09-30): the control program owns the relays and sensors; the display program owns the screen and touch input (and pushbuttons, if kept) and is a client of the same JSON API as the web UI, over `localhost`. A display hang or crash can't stall HVAC control, and every setting change goes through one validation path.

The control program runs **threads around one shared state** (decided 2026-11-19, replacing the single-threaded `poll()` loop planned earlier): a control thread runs one pass every 60 s (read sensors, decide, switch relays, record history); cpp-httplib's own threads answer API requests at once from the last pass's data; a weather thread does the nightly fetch; `main` waits for SIGTERM/SIGINT. One mutex guards everything shared (settings, control state, last-pass snapshot, history ring), and nothing slow (network, I2C) runs while holding it. Requests only change settings; relays change only in the control pass, so a request takes effect within 60 s and every status change lands in history. cpp-httplib won over Mongoose (GPL, single loop) and libmicrohttpd (verbose C callbacks): simplest handlers, plus an HTTPS client for NWS.

- **Heating/cooling is a state, not a loop.** States are `idle / heating / em_heating / cooling / circulating` (`hvac_status`). Each pass checks whether it should change, acts, and returns. Nothing waits inside a handler.
- **Fixed schedule:** the next pass is due 60 s after the previous one was due, not 60 s after it finished.
- **The decision is a pure function**: `test_switch(state, command, timestamps, now) → new state`. It touches no hardware, so the timing rules can be tested on a desktop with fake inputs.
- **The display redraws only when something changed**, and only the changed region. A full 320×240 RGB565 frame is ~150 KB, ~150 ms at the current 8 MHz SPI.
- **No slow calls in the control pass.** `get_sensor_data()` does a single data-ready check and never waits (done 2026-10-03). The weather fetch has its own thread so a slow NWS can't delay control; satellite polling will need a short timeout.
- **On SIGTERM/SIGINT, switch every relay off before exiting** — the software half of the fail-safe. A crash or `kill -9` is left to a planned watcher program.
- All timing on `CLOCK_MONOTONIC`, never wall-clock time.

### Avoiding short-cycling

The real risk is the AC and furnace fighting each other — AC overshoots cold, furnace compensates, overshoots warm, AC kicks back on. Three mechanisms, no physical mode switch:

1. **Deadband instead of a single setpoint.** E.g. AC on at 74°F / off at 71°F; furnace on at 69°F / off at 72°F. The gap between them is a band where neither runs.
2. **Seasonal lockout derived from the weather API**, not set by hand. Rolling outdoor average above ~65°F = AC-only; below ~55°F = furnace-only; between = swing season where either may run under the deadband. This is the main use of the weather API — delaying or skipping a cycle rather than reacting live.
3. **Cycle timers**, regardless of temperature, to protect the furnace and compressor (below).

#### Cycle timing (decided 2026-10-05)

| Timer | Value | Rule |
|---|---|---|
| Min off | 10 min | Furnace or AC can't start until 10 min after either stopped. One shared lockout: furnace stopped 5 min ago → AC waits 5 more. |
| Min on | 5 min | A running cycle can't be stopped before 5 min. |
| Max run | 2 h | A cycle running 2 h is forced to idle. Not reaching the setpoint in 2 h suggests a sensor or equipment fault. |

- **The relays follow a state, not the command.** The temperature logic produces a request (`hvac_command`); `test_switch` turns current state + request + timers into the next state (`hvac_status`); relays change only when the state changes. Request and state are separate types: `no_data` is a request with no matching state, and a future fault/lockout state would be a state nothing requests.
- **Timestamps change only on transitions.** idle → heat/cool/em_heat sets `cycle_start`; heat/cool/em_heat → idle/circulating sets `cycle_end`. Updating them every tick instead (the first attempt) meant the lockout never expired. `cycle_end` starts at boot time, so a power blip mid-cycle still gets a full min-off before restarting.
- **Heat ↔ cool always goes through idle**, so min-off applies before the other system starts. It skips min-on: a switch within minutes means something is wrong, and stopping beats continuing to heat. Rare in practice, since the temperature has to cross the whole deadband.
- **Emergency heat** (below `MIN_HEAT`, 60 °F) starts at once from any status (skips min-on, min-off, mode, season, gate) and has no max-run: it runs as long as it's called for. Only the furnace's own high-limit and the sensor checks guard a sensor stuck low.
- **No sensor data** = request idle: a running cycle finishes its min-on, then stops; idle stays idle.
- **Fan** (decided 2026-11-19): `circulating` is a status. It skips min-off and has no max-run. Its min-on only holds off a stop to idle: heat/cool take over as soon as min-off allows (changed 11-20 after a bench run), em heat at once. A fan call never cuts heat/cool short: they finish min-on, then go to circulating instead of idle. Calls come from manual fan-on (RAM only, held off while away), the zone temperature spread, and CO2: a room > 1000 ppm and > 1.2 × weighted average starts it, ≤ 1.1 × stops it; rooms without a trusted reading count as 700 ppm.

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
- **Warm-up, local sensor:** the SCD41 can't report how long it has been on. `scd41_start()` returns whether it started measuring or found it already running (the ASC-disable command is rejected while measuring). Already running → already warm, `started_at = now - SENSOR_WARMUP`; fresh start or error → wait the full 3 min. `started_at` must always be set explicitly: its default 0 on the monotonic clock means "since boot", i.e. falsely warm. Known gap: a crash within 3 min of a fresh start, then a restart, is treated as warm while the sensor still reads a few degrees high. Fix if needed: record the start time in `/run` (cleared on reboot) and wait the full warm-up when it's missing.
- **Warm-up, satellites:** reported by the satellite (`"status": "warming"`), see [Satellites](#satellites).

### Persistent settings

The Pi keeps its settings in two JSON files so they survive a restart (built 2026-11-17, `data_store.cpp`):

- **`hvac_points.json`:** `heat_point`, `ac_point`, `night_difference` (heat drops by this at night).
- **`vacation.json`:** `next_id` plus a list of blocks (`block_id`, `vacation_start` = departure, `vacation_end` = return). Ids are never reused, so blocks can be edited or removed by id later.
- **Location:** `~/thermostat/` next to the program (`DATA_DIR`). `deploy.sh` excludes both files, since an rsync push would otherwise overwrite live settings with the repo's examples.
- **Format: JSON** (nlohmann/json), shared with the API code.
- **Times:** Unix seconds in the file and the API (changed 2026-11-19; the API used to take US local time strings). The website converts in the house's time zone, which the Pi's matches.
- **Read only at startup.** Missing or invalid `hvac_points.json` → struct defaults. Missing/invalid `vacation.json` → no vacations; a single invalid block is dropped and the rest load.
- **Every change:** validate → update RAM → atomic write (temp file, `fsync`, `rename()`, `fsync` the directory). A rejected request changes neither RAM nor file.
- **Failed write:** the new values are already validated and are what the user wants, so they stay in use. An unsaved flag is retried every loop pass, writing whatever is current (no queue). Logged when it fails and when it recovers. Only loss: a restart before a retry succeeds.
- **Only the Pi writes it.** The web UI sends a request; the Pi validates and saves. One source of truth.

**Set point rules:** night heat (`heat − night_difference`) > `MIN_HEAT + MIN_DEAD_BAND`, so night heat can't drift into em-heat range. `heat + MIN_DEAD_BAND ≤ ac`, `ac ≤ 90`, `night_difference ≥ 0`. A request sends only what changes. Heat alone pushes AC up to `heat + MIN_DEAD_BAND` if it would cross; AC alone pushes heat down. A push out of range rejects the whole request.

**Vacation rules:** end must be after start. Overlapping blocks are kept separate (so each can be edited) and act as one span from earliest start to latest end: away = any block with `start ≤ now < end − 2 h`. Fixed 2 h pre-conditioning lead (`PRECONDITION_LEAD`): a shorter delay saves little, since the house is near target anyway; lengthen it if 2 h proves too short. Blocks are deleted once their return time passes.

**Away temperatures are global**, not per vacation. Only freeze protection matters (summers don't get hot enough). While away, the caller passes `AWAY_POINTS` (heat 62, AC 90, no night shift, `fan_allowed = false`) to `get_HVAC_command` in place of the user's set points. It's a constant, so it skips set point validation (62 isn't above `MIN_HEAT + MIN_DEAD_BAND`). Order still holds: Honeywell backstop < em heat (60) < away heat (62). A bool, not a magic fan threshold, turns the fan off.

**Mode** (decided 2026-11-17): `auto` / `heat` / `cool` / `both`, saved in `hvac_points.json`, set through `PUT /setpoints`. Manual modes stay until set back to `auto`. This is how the user overrides a wrong season, or sets one when the weather API is down. Kept outside `hvac_set_points` so the away swap can't drop it.

**Season lockout (auto mode):** `weather.json` holds up to a week of hourly NWS forecast (`forecast/hourly` gives ~156 hours), written only by the program (kept apart from user settings). Fetched nightly: one try an hour from midnight to 8 am until one succeeds (any hour on a fresh install), straight from the known gridpoint; gridpoint and contact come from environment variables so they stay out of git. Season = average daily low over the full days in it (≥ 16 hours, so a fetch up to 8 am still counts that day): below 55 → heat only; above 65 → cool only; between → both, weather gate decides. The 7-day average and the band in between keep it from flip-flopping, so there's no separate hysteresis. **No expiry:** stale data keeps its season, because AC in winter is the failure that matters. No data at all → heat only (Claude's choice; manual mode overrides).

**Weather gate:** no cooling when the forecast for the current hour is ≥ 5° below the active AC point; no heating when it's ≥ 5° above the active heat point. "Active" is after the night shift. Unknown outside temp → gate skipped (fail open, 10-10). Em heat ignores mode, season and gate.

**Night:** midnight to 8 am (9 pm-midnight is day: user's awake). The whole band drops by `night_difference`, and night sensor weights apply. `is_night_now()` is called in `main` and passed in, so the logic is testable.

### Web UI ↔ Pi

The Pi exposes a small **JSON-over-HTTP API**; the Ubuntu web app and the local display program are its clients. Browsers never talk to the Pi directly. Endpoints (built 2026-11-19, listed in `pi/api.h`): `GET /status`, `GET /readings?view=main|all`, `GET`/`PUT /setpoints`, `GET`/`POST /vacations`, `PUT`/`DELETE /vacations/{id}`, `PUT /fan`, `GET /history?since=`, on port 8080. No sign-in by design: any client on the LAN, loopback or the tailnet may write; the Pi has no ports open to the internet.

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

- **The Pi keeps a RAM ring buffer** of the last 24 h, one entry per control pass (1,440): time, status, mode as set, each sensor's temp/RH/CO2 with its trusted flag (null if it never reported), and the weighted average control used. Served by `GET /history?since=<unix ts>`; Ubuntu asks for everything after its last stored row, so an Ubuntu outage of up to a day backfills instead of leaving a gap. RAM-only — no SD writes.
- **No separate event buffer** (decided 2026-11-19, replacing the planned `GET /events`): the status can only change in a control pass, and every pass is recorded, so the samples already contain every transition.
- **Timestamps are Unix seconds from the Pi**, taken in the control pass. The Pi 3 has no battery-backed clock, so nothing is recorded until NTP has synced (`ntp_adjtime`, `STA_UNSYNC` clear). Until then control still runs, but without anything that needs the date: no away, no vacation clean-up, no weather gate, no fetch.
- **Ubuntu side:** SQLite, one table (timestamp, zone, temp, RH, CO2, HVAC state). Keep everything (a year is tens of MB) and display the last 7 days. Charts with uPlot or Chart.js. The public demo reads the same database.

### Where software lives

**Satellites** run a lightweight program that serves their latest reading over HTTP when the Pi asks.

**The Pi** is the central hub: polls the satellites, reads its own sensor, fetches outside weather, drives the HVAC relays and the local display, and accepts commands from the web UI and the touchscreen. No Docker — the project is simple and needs direct board IO, so containerizing costs more than it returns.

**Development** happens on the Ubuntu server, not on the Pi (decided 2026-10-02). `pi/` in the project folder mirrors `~/thermostat` on the Pi exactly and is the source of truth; `deploy.sh` rsyncs it over SSH and builds/runs it there. The Pi carries only what it runs — no images, docs or datasheets. A script beat deploying via GitHub: no credentials on the Pi, no commit per test. GitHub ([dillanhart/multi-room-hvac](https://github.com/dillanhart/multi-room-hvac)) is the portfolio record only. IntelliSense checks against a copy of the Pi's headers (`.sysroot/`), so a library missing on the Pi shows as an error on the dev machine, and `./deploy.sh build` compiles on the Pi as the final word.

**The Ubuntu server** hosts the web UI. This adds complexity, for two reasons: it allows a public demo site for portfolio purposes, and if the self-hosted Alexa idea goes ahead it'll need the Ollama install already running there — so working out cross-machine communication now makes that expansion easier.

## Constraints

- **C++ wherever possible.** A primary goal of the project is getting more familiar with it.
- **AI augments the work; it doesn't replace the learning.** Goals: learn C++, re-sharpen program-design skills, and use AI to speed that up and take on the tedium (boilerplate, tooling, mechanical edits), as long as it doesn't get in the way of learning. Design decisions stay mine, and AI-written code gets read and understood, not just accepted.
  - **Design problems: my attempt first.** I come up with a design before getting one. If I'm stuck, AI points me at what to research or which question to ask, rather than handing over the answer. A few minutes of struggling is part of learning; a direct answer only after that.
  - **AI raises what I haven't considered.** Alternatives, trade-offs and missed failure modes get brought up, not silently built around. Pressing ahead on a design only because I didn't think of a better one isn't learning.
  - *Revised 2026-10-03; originally "no AI-generated code", which was too ambitious alongside full-time school, 25 h/week of work, a new language and first-time hardware, and isn't what a modern employer is looking for — using AI well is.*

## MVP

Main board (Pi) + **one** satellite (ESP32 + SCD41) + both web UIs. Anything less doesn't give real signal on whether the multi-zone approach works. Voice control and additional satellites come after.
