# Build Log

Chronological record of work actually performed: what was tried, what broke, what was learned. Design rationale and open decisions live in [idea.md](idea.md). Closed "Open" items are struck through with the date they were closed.

---

## Prior to 2026-09-21

- Pulled existing Honeywell RTH111B1001 and probed terminals. Found 4 wires: **R, G, W, Y**. No C-wire at thermostat. Rh/Rc jumper in place.
- Selected components: Pi 3 (Pi OS Lite x64), FL-3FF-S-Z opto-isolated relay board (low-level trigger), SCD41, ESP32 satellites.
- Ordered all parts.

---

## 2026-09-21 — Parts arrived; relay interface bench-verified

### GPIO assignment

Relays on **GPIO 4, 5, 6** (physical pins 7, 29, 31):

| GPIO | Phys | Channel |
|---|---|---|
| 4 | 7 | W (heat) |
| 5 | 29 | Y (cool) |
| 6 | 31 | G (fan) |

Chosen because BCM2837 GPIO 0–8 come out of reset with internal pull-**ups**, while GPIO 9–27 default to pull-**downs**. A pull-down on a low-level-trigger input is a weak energize signal at power-on; 4/5/6 make the chip's reset state reinforce fail-safe-off. Avoided GPIO 14 (UART TX driven high at boot) and GPIO 0/1 (HAT EEPROM probe).

Added to `/boot/firmware/config.txt` under `[all]`:

```
gpio=4,5,6=ip,pu
```

### Bench test — relay board

Screw terminals empty, nothing connected to HVAC. Board powered from Pi 5V.

- **Board pull-up dominates.** With the board connected, forcing `pinctrl set 4 ip pd` still reads `hi`; the board's ~1kΩ pull-up beats the Pi's ~50kΩ internal pull-down. Relays are held open by hardware, independent of software or whether Linux boots. (Pi 3 pull registers are write-only, so pull state can only be inferred from the pin level.)
- **All relays open in every tested state:** cold boot, `reboot`, `shutdown -h now` and pulling the power cord, each from energized.
- **All 3 channels switch cleanly** via `pinctrl set N op dl` / `op dh`. No buzzing or partial release, so the 5V-VCC vs 3.3V-logic marginal-release issue doesn't apply to this board.
- **`vcgencmd get_throttled` = `0x0`** with all three relays energized. Single supply off the Pi is sufficient.

**Conclusion:** the relay interface behaves exactly as the fail-safe design requires.

Headless dev access set up: key-based root SSH over Tailscale (node `thermostat`), password auth impossible.

---

## 2026-09-22 — Display bring-up

Panel is an **ILI9341** (2.4", 240x320; the listing said ST7735), **14-pin** module. **The LED pin is a separate backlight input that must be driven** — left floating, the panel is dark and looks dead even when everything else works.

### Rejected: `fbtft`

Tried first. Bound fine, but roughly the first 1/4 of the framebuffer never refreshed — on both the `write()` and `mmap` paths, at 16 and 32 MHz, and the dead region moved when `rotate` changed, so it tracked framebuffer coordinates rather than the glass. A dirty-tracking fault in a staging module; abandoned rather than debugged.

### Working configuration

Mainline DRM **`panel-mipi-dbi`** driver. Init commands live in `/lib/firmware/panel.bin`, built with `mipi-dbi-cmd` from <https://github.com/notro/panel-mipi-dbi> using the ILI9341 sequence from that project's wiki. `dtparam=spi=on` plus, under `[all]` in `config.txt`:

```
dtoverlay=mipi-dbi-spi,spi0-0,speed=32000000
dtparam=width=320,height=240
dtparam=reset-gpio=25,dc-gpio=24
dtparam=write-only
```

In `ili9341.txt`: `command 0x36 0x40` (MADCTL — landscape, RGB, X-flipped). In `cmdline.txt`: `fbcon=map:1`. `write-only` because SDO/MISO isn't wired.

Binds as `/dev/fb1`, 320x240 RGB565, stride 640. **Verified** with evenly spaced lines and white→red→green→blue colour bands.

### What cost most of the day

**`fbcon=map:1` is mandatory.** DRM only pushes pixels once a mode is set and the pipeline is enabled. Without a console mapped to the framebuffer, writes to `/dev/fb1` land in a shadow buffer and the panel sits on its power-on white looking dead. fbtft, by contrast, pushed blindly on a timer.

**`panel.bin` is read once at driver probe.** Every init change is edit → recompile → **reboot**, in that order. Recompiling *after* a reboot silently tests the previous build — this happened twice and sent the diagnosis the wrong way both times. Check `uptime -s` against the blob's timestamp.

**Test scripts must read geometry from the kernel** (`/sys/class/graphics/fb1/virtual_size`, `stride`), not hardcode it. Hardcoded dimensions that disagree with `config.txt` look exactly like a driver bug; this caused another wrong diagnosis.

**MADCTL and `width`/`height` must be paired, and the pairing is empirical** — test new combinations rather than reasoning from the MV bit.

| MADCTL | `width`/`height` | Result |
|---|---|---|
| `0x00` | 320/240 | landscape, mirrored |
| **`0x40`** | **320/240** | **landscape, correct — in use** |
| `0x80` | 320/240 | landscape, Y-flipped |
| `0xC0` | 320/240 | landscape, 180° from `0x00` |
| `0x20` | 240/320 | portrait, correct |

`0x40`/`0x80` differ only by which edge is "up", so a mounting flip is a one-byte change.

### Failure signatures

Mismatched geometry looks like a hardware fault but isn't:

- **Exactly 3/4 of the panel painted, rest stale** — declared dimensions disagree with the controller's addressing window.
- **A gradient that resets partway down** — address wrapping; the tail overwrites the top.
- **Lines with step discontinuities** — stride mismatch.
- **Mirrored image** — MX/MY bits clear.

### Plan for backlight and SCD41 (done 09-23)

- The Pi 3's two 3.3V pins (1, 17) are one rail; the limit is current (~500 mA for external loads), not pin count.
- Backlight moves off the 3.3V pin to **GPIO 12 (PWM0)** so it can be blanked or dimmed. It draws 60–100 mA against a 16 mA per-pin limit, so it needs a transistor. GPIO 12 resets with a pull-down, and backlight-off at boot is harmless.
- SCD41 on I2C1 (phys 3/5) at the 100 kHz default, the fastest it supports. Its ~205 mA measurement spike goes into the supply re-check.

### Open

- ~~`T_*` pins: does touch replace the pushbuttons?~~ Closed 09-24: no touch controller fitted; pushbuttons stay.
- SPI clock is `speed=8000000` in `config.txt`, but 32 MHz worked here. Lowered while chasing the blank panel; restore 32 MHz or find out why 8 MHz is needed.
- Idle blanking unverified: with `backlight-gpio` bound, DRM should power the backlight down instead of painting black, making `consoleblank=0` unnecessary.

---

## 2026-09-23 — Backlight on GPIO 12; SCD41 reading

### Backlight

Display LED on **GPIO 12 (phys 32)** through a transistor; **active-high**. `dtparam=backlight-gpio=12` under the `mipi-dbi-spi` block binds `/sys/class/backlight/backlight_gpio` (`bl_power`: `4` off, `0` on).

**`dtparam=` scoping is positional and cost a reboot cycle.** A `dtparam=` applies to the *preceding* `dtoverlay=`. Placed in the top block next to `dtparam=spi=on`, it's a base-DTB param; `backlight-gpio` isn't one, so it's silently ignored — no error, no log line, panel dark. Check names with `dtoverlay -h mipi-dbi-spi`.

### SCD41 on I2C1

Reading: **CO2 587 ppm, 25.9 °C, 43.8 %RH.**

- `/dev/i2c-1` didn't exist despite `dtparam=i2c_arm=on`: **`i2c-dev` wasn't loaded.** `modprobe i2c-dev`, added to `/etc/modules`. Installing `i2c-tools` doesn't pull it in.
- Bare breakout: no regulator, no level shifters, 10k pull-ups. **Run at 3.3V** — at 5V its V_IH is 3.25V and the Pi only drives 3.3V.

### Misplaced GND — consumed most of the session

Ground was in the wrong header position (now **phys 20**). Symptom: `i2cdetect` sees `0x62` reliably, but **every** command NACKs — every command, every sequence, across power cycles and delays.

The sensor was parasitically powered through the SDA/SCL pull-ups and its ESD diodes: enough for the I²C front end to ACK its address, nowhere near enough to execute anything. Voltage at the header looked fine throughout, because the header was fine.

**Rule: an address ACK does not prove the device is powered.** Address-ACK plus universal-NACK points at power or ground, not protocol.

Theories that cost time and were wrong, so they aren't re-run:

- **Supply voltage** — suspected a regulator expecting 5V. There is none.
- **Command legality** — most commands are rejected during periodic measurement. But `get_data_ready_status` and `stop_periodic_measurement` are legal then, and both NACKed too.
- **Signal integrity** — correctly skipped: the address ACK already exercises both lines end to end.

### Current check — closed

`vcgencmd get_throttled` = **`0x0`** with the backlight lit and the SCD41 measuring. **Single Pi supply is sufficient; no separate 5V brick.**

---

## 2026-09-24 — Touch not fitted; abandoned

Module is the **MSP2401 non-touch variant**: the `U1 / TOUCH` footprint is empty, though the header carries the `T_*` pins on both variants. **Touch abandoned** — physical control stays two pushbuttons. `SDO` and all `T_*` pins unconnected.

How it was established: every `T_*` wire continuity-checked, 3.3 V at the touch VCC pad, MOSI→MISO loopback on `/dev/spidev0.1` passed. X, Y, VBAT and TEMP all read exactly 0, and `T_IRQ` (analog, SPI-independent) never dropped under a hard press. With a passing loopback, that leaves only an absent chip.

- Wrong turn: VBAT was first read with `0xA0` (differential mode, no such channel), so that 0 meant nothing.
- **Never share MISO with the display's `SDO`** — it isn't reliably tri-stated. `write-only` stays.

**The Python bench test disabled the production display path.** To expose CE0 as `/dev/spidev0.0`, `dtoverlay=nospi10` and `dtoverlay=mipi-dbi-spi,…` were commented out. The `dtparam=` lines below them then bind to nothing, silently (same failure as 09-23). ~~**Still commented out as of 10-06 — restore both lines and reboot.**~~ Done 11-18: `mipi-dbi-spi` restored; `nospi10` was already inert (commented, under `[pi5]`).

Bench-only, not part of the build: `test_display.py`, `test_touch.py`, `test_spi_loopback.py`, `~/thermostat-venv` on the Pi.

### Header occupancy as wired

| Phys | GPIO | Used by |
|---|---|---|
| 1 | 3.3V | Display VCC |
| 2 | 5V | Relay board VCC |
| 3 | 2 | SCD41 SDA (I2C1) |
| 5 | 3 | SCD41 SCL (I2C1) |
| 6 | GND | Relay board GND |
| 7 | 4 | Relay W |
| 9 | GND | Display GND |
| 17 | 3.3V | SCD41 VCC — same rail as phys 1 |
| 18 | 24 | Display DC |
| 19 | 10 | Display SDI (MOSI) |
| 20 | GND | SCD41 GND |
| 21 | 9 | MISO — claimed by `dtparam=spi=on`, unused (`write-only`) |
| 22 | 25 | Display RESET |
| 23 | 11 | Display SCK (SCLK) |
| 24 | 8 | Display CS (CE0) |
| 26 | 7 | CE1 — claimed by `spi=on`, unused |
| 29 | 5 | Relay Y |
| 31 | 6 | Relay G |
| 32 | 12 | Display backlight (PWM0, via transistor, active-high) |

Off-limits: GPIO 14/15 (phys 8/10, UART), GPIO 0/1 (phys 27/28, HAT EEPROM probe).

**Planned regroup:** the four power/ground wires are the only movable ones, and they cross. Swap them so each device's wires sit together: display VCC 1→17, GND 9→20; SCD41 VCC 17→1, GND 20→9 (not 6, which is the relay board's). No `config.txt` change. ~~Not yet recorded as done.~~ Done 11-18, during the rewire for the new display.

---

## 2026-09-27 — `control.c`: libgpiod v2 relays; SCD41 read from C

- **Pi has libgpiod v2.** First draft used the v1 API (`gpiod_chip_get_line`, `gpiod_line_request_output`, …), which doesn't exist in v2; most online tutorials are v1. Ported: chip → line settings (output, `active_low`, initial off) → line config → request config → `gpiod_chip_request_lines`.
- **SCD41 over raw `/dev/i2c-1`:** stop periodic, start periodic, poll data-ready, read measurement; CRC-8 checked on every word. A new reading every ~5 s.

### Startup transient

- Every run starts at **~76 °F** and decays to **~69.2 °F** over **~3 min**; RH rises in step.
- First read as the board cooling. **Ruled out:** a second run right after a settled one jumped straight back to ~76.5 °F. The stop/start on open causes it.
- Settled noise ≈ **±0.05 °F**.

### Open

- Relay state after the process is killed (`kill -9` while energized) untested. Released lines may hold their last value.
- ~~Don't restart the sensor if it's already measuring.~~ Done 10-03: `scd41_start()` returns `SCD41_ALREADY_RUNNING`.
- ~~Gate control on settled readings after power-up.~~ Done 10-03: `SENSOR_WARMUP` plus 3 consistent reads before trust.
- Calibrate against a reference thermometer (`set_temperature_offset`).
- ~~`main` prints readings without checking `.valid`.~~ Done 10-03: control checks only `health.trusted`.

---

## 2026-09-28 — Software architecture decided (design only, nothing built)

Full rationale in [idea.md](idea.md) → Software. Decided:

- **JSON over HTTP everywhere.** Pi pulls satellites (`GET /reading`, ~60 s); the Ubuntu web app calls a JSON API on the Pi. MQTT and raw TCP rejected.
- **Fixed addresses via DHCP reservations** on the router.
- **Single-threaded event loop** blocking in `poll()`; timeout = earliest deadline. Heating/cooling is a state, not a nested loop.
- **Settings file on the Pi**, atomic write via `rename()`, Pi is sole writer.
- **History (nice-to-have):** Pi RAM ring buffer + event buffer with `?since=` backfill; Ubuntu stores in SQLite.

### Open

- Fan logic from the spread between zones: first cut in `get_HVAC_command` (`FAN_TRIGGER`), not settled.
- HTTP server library (Mongoose vs libmicrohttpd); settings file format.
- ~~Satellite filtering rules.~~ Decided 10-02: all validation on the Pi (`update_node`).
- ~~`get_sensor_data()` blocks up to 6 s.~~ Fixed 10-03.

---

## 2026-09-30 — Display as a separate program (design only, nothing built)

- **The display runs as its own program**, a client of the same JSON API as the web UI over `localhost`.
- **Pushbuttons move to the display program**, out of the control loop's `poll()`.
- **Why:** a display hang or crash can't stall HVAC control, and display and web UI changes both go through `validate_setpoints()`.

Open: GET polling interval; show "controller offline" instead of stale values; two systemd units (the control unit owns the watchdog).

---

## 2026-10-01 — Converted to C++; file split

- `control`, `relay`, `net` → `.cpp`. `sensor.c` and the Sensirion driver stay C; `extern "C"` guards in `sensor.h` so C++ links against them.
- C++ has no implicit int → enum: relay loops use `static_cast<relay>(r)`.
- `now_ms()`: `CLOCK_MONOTONIC` → `long long` ms (`int` ms overflows after ~24 days).
- `compile.py`: `gcc` for C, `g++ -std=c++20` for C++, link with `-lgpiod`. **Builds.**
- `main.cpp` = entry point + loop; `control.cpp` = decision logic. `all_off()` added to the relay code: all relays off, pins stay claimed (`relays_close()` also releases them).

---

## 2026-10-02 — Decision logic started

- `get_HVAC_command()`: `cool` if weighted temp > `ac_set`, `heat` if < `heat_set`.
- Weights: main **10** day / **8** night; office and bedroom **6** / **4**; **0** if invalid. `hour_of_day()`: day when `tm_hour > 8`.

### Open (found by reading)

- ~~Functions used before declaration; `control.h` empty.~~ Fixed same day.
- ~~Weighted temp never divided by the weight sum.~~ Fixed same day.
- ~~`get_HVAC_command` has no return when no heat or cool is needed.~~ Fixed same day.
- ~~`main.cpp` has a placeholder where the decision call goes.~~ Fixed same day.
- `hour_of_day()` counts every hour after 08:00 as day, including late evening.
- `control.cpp` includes `<chrono>` but uses `<ctime>` functions.
- `calibrate.c`: comment says default 425 ppm; `DEFAULT_TARGET_PPM` is 438.
- `FAN_CLEAR` unused (no fan hysteresis yet).

### Next

- `main.cpp`: fetch office/bedroom via `net_request`.
- Safety rules from `psuedo.txt`: ~~heat/cool interlock~~ (done by 10-05: `relay_set()` refuses Heat+Cool); min on/off and emergency heat in progress (10-05); progress faults and fan circulation not started.

---

## 2026-10-02 (later) — Dev workflow, GitHub repo, first clean build

### Dev workflow

- **`pi/` on the dev server is the source of truth**; the Pi's `~/thermostat` mirrors it. `deploy.sh` (Claude) rsyncs over SSH and builds/runs there. **Don't edit on the Pi** — the next deploy overwrites it.
- `./deploy.sh sysroot` copies the Pi's headers into `.sysroot/` for IntelliSense, so a library missing on the Pi shows as missing here. Gotcha: Debian 13's `asm/*` headers are symlinks into `/usr/lib/linux/uapi/`, which also has to be copied.

### GitHub

- Public repo **github.com/dillanhart/multi-room-hvac**, portfolio only; the Pi never pulls from it. Sensirion driver as a submodule. Datasheets, build output, `.claude/`, `.sysroot/` not tracked.
- Photo EXIF stripped before publishing (`IMG_1192.JPG` had GPS coordinates).

### C/C++ lessons from the build errors

- **`sensor.h` is shared by C and C++. Anything C++ in it breaks the C build** (`std::string` → `expected specifier-qualifier-list before 'std'`). C++ types moved to C++ headers.
- **Each `.cpp` sees only what it includes.** One missing include gave dozens of cascade errors. Fix the first error, rebuild, then read on.
- `std::max(a, b, c)` treats `c` as a comparator → `std::minmax_element`.
- `{"main", 10, 8}` silently filled the first struct fields (temp, humidity), leaving both weights 0 → designated initializers `{.name = "main", .weight_day = 10, …}`.
- `a, b, c = x;` is the comma operator — only `c` is assigned.
- Separate `main_node`/`office_node` variables were copies; `nodes` never saw their data. Use `nodes[i]`.
- `printf` needs `.c_str()` for `std::string`, and `\n` or nothing appears (line buffering).

### SCD41 read failure traced

Output was `already measuring` ×2 → `read failed` → `heat` / `10.0 F`.

- `get_sensor_data()` called `scd41_start()` a second time, and the second `sensirion_i2c_hal_init()` broke the bus. **Sensirion's Linux HAL caches the device address in a static and only calls `ioctl(I2C_SLAVE)` when it changes**, so a second `open()` gets a handle with no address set and every command NACKs. `sensirion_i2c_hal_free()` doesn't reset it either, so reconnecting could never recover.
- **10.0 F was the main node's day weight** (the brace-init bug above), not a reading.
- Fixes: `get_sensor_data()` only reads, bus never closed (user). `scd41_start()` opens the bus at most once and retries start every 3rd failure; `calculate_weighted_temperature` → `std::optional<float>`, `no_data` when empty (Claude).
- Verified on the Pi: real readings (68.4 °F) → `heat`, correctly below `heat_set` 70. Clean build, zero warnings.

---

## 2026-10-03 — Sensor validation implemented

Rationale in [idea.md](idea.md) → Failure handling. Implemented by Claude on top of the user's `sensor_health` / start-status / field-rename edits.

- **`sensor.c` is a thin driver.** `get_sensor_data()` makes one data-ready check and never waits, so the file ports to the ESP32 with only the HAL swapped.
- Fields renamed (user): `temp`, `hum`, `read_ok` (was `valid` — "the read worked", not "trust this").
- **`scd41_start()` returns `STARTED` / `ALREADY_RUNNING` / `ERROR`.** Already running → treated as warm.
- **`update_node()`** (all nodes): 50–110 °F range; rate vs the previous reading ≤ 20 °F / 3 min; 3 good reads in a row → `trusted`; on error, keep the last good reading up to 3 min.
- **Verified on the Pi:** `no_data` ×2 → trusted on the 3rd read → `heat` at 65.7 °F.

Note: trust takes 3 reads — 30 s at the current `sleep(10)`, ~3 min at `READ_INTERVAL` (1 min).

### Open

- Untested on hardware: fresh-start warming (needs a sensor power cycle), error-hold path, jump rejection.
- Satellite side: map the ESP32 JSON `"status"` to `sensor_status`, then `update_node(nodes[1..2], …)`.
- ~~`main` doesn't act on `no_data`.~~ Decided 10-05: `no_data` = request idle. Counting toward `MAX_SENSOR_FAILS` still open.
- ~~Uncommitted.~~ Committed `f0b92bb`.

### Constraint revised: AI use

Dropped "no AI-generated code" (idea.md → Constraints). New goal: learn C++ and re-sharpen design skills, using AI to augment that and cut tedium where it doesn't hinder learning. Authorship of AI-written changes stays marked in this log. On design problems: my attempt first, or pointers on what to research before any direct answer; AI raises alternatives and missed failure modes rather than building around my plan.

---

## 2026-10-04 to 10-06 — HVAC state machine (in progress)

Rationale in [idea.md](idea.md) → Avoiding short-cycling.

- **First attempt (10-04):** relay switching wired straight into `main` from the command, timed by `over_cycling()` / `cycle_safe()`. Review found `relays_close()` releasing the GPIO pins on the first `no_data` (relays dead for the rest of the run), `switch` cases falling through without `break`, and a brace-less `else` resetting `cycle_end` every tick. User fixed those, but the design couldn't work: one bool can't mean "may start", "may stop" and "must stop".
- **Rewritten as a state machine (user):** `hvac_status` enum (`em_heating`, `heating`, `cooling`, `circulating`, `idle`) and `test_switch(sys_status, command, …)` → next state. `starting` dropped (same as idle). `over_cycling` removed; committed `8368382`.
- Claude: `equipment_for` / `conflicting` (furnace vs AC), `status_for` (command → status), file-local helpers made `static`.

### Decided

- Relays follow **state**, not command: `main` compares `sys_status` to `test_switch`'s result and switches relays only on a change.
- Timestamps change only on transitions: idle → heat/cool/em_heat sets `cycle_start`; heat/cool/em_heat → idle/circulating sets `cycle_end`. `cycle_end` = boot time at startup.
- Min-off (10 min) shared by furnace and AC.
- **Heat ↔ cool goes straight to idle, skipping min-on.** Switching within minutes is anomalous; stop rather than keep heating. Min-off still protects the AC.
- Emergency heat bypasses min-off; max-run still applies.
- `no_data` = request idle.
- `hvac_command` (request) and `hvac_status` (equipment) kept as separate types.

### Open

- ~~`test_switch` branch on `conflicting` is inverted (same request → idle; heat ↔ cool takes the min-on path). Fix: `!conflicting`.~~ Done 10-06: `!conflicting` (lost to a Ctrl-Z, re-applied).
- ~~`test_switch` doesn't hold state during min-on (no `else`; `return_status` uninitialized).~~ Done 10-06: `return_status = sys_status` default.
- ~~Emergency-heat bypass missing from the idle branch.~~ Done 10-06: em-heat branch checked first in `test_switch`.
- Emergency-heat max-run restarts after one tick — needs a fault state, or a `PROGRESS_WINDOW` check.
- Heat ↔ cool skip: comment it in code; log it when it happens (the rate check allows a 3–4 °F spike across the deadband).
- ~~Fan: own timers or furnace/AC rules.~~ Done 10-07: circulating follows the same min-on/min-off/max-run rules (see next entry). idea.md still lists it open.
- ~~`main` not wired to `test_switch`; `printf("%s", command)` passes an enum.~~ Done 10-06: wired; printf removed.

---

## 2026-10-06 to 10-07 — State machine finished; `update_node` review

Switching logic reviewed by Claude (read + `-Wall -Wextra`), fixed by user/Claude as marked. Not run on hardware; `gpiod.h` is missing on the dev machine, so nothing was linked.

### Found in review, fixed

- `main` never assigned `sys_status`, so relays never turned off.
- The `command != emergency_heat` line zeroed `cycle_end` every pass, so `now > cycle_end + MIN_OFF_TIME` was never true and heat/cool never started from idle. (My first review wrongly called it harmless.) Deleted by user.
- Em-heat max-run used a stale `cycle_start` after a long idle, so em heat never started. User: cap only applies when already `em_heating`.
- `main` stamped no timers after the switch was moved out; stamping lost in the edit.
- Heat/cool stayed on when the status went to circulating (cases only turned things on).
- `MIN_ON_TIME` bypass let any status go to circulating at once; circulating → furnace/AC skipped `MIN_OFF_TIME`.
- Braces/`sleep(10)` deleted by accident in an edit; loop didn't compile.

### Changes

- **User:** `!conflicting`, `return_status` default, em-heat branch (max-run only when already `em_heating`), removed `command != emergency_heat` line, `relay_success` handling in `main`.
- **Claude:**
  - `all_off()` returns 0 / -1 like `relay_set`, tries every relay even after a failure.
  - Case/switch moved from `main` to `apply_status()` in `control.cpp`. It stamps timers only after the relay write succeeds; replaces the save/restore variables. A failed write leaves `sys_status` unchanged, so the next pass retries.
  - Circulating case calls `all_off()` first.
  - `cycle_end` stamped when furnace/AC stops (to idle or circulating); `cycle_start` stamped on every non-idle change except em_heating → heating.
  - `restart_blocked` in `test_switch`: circulating → furnace/AC waits `cycle_end + MIN_OFF_TIME`.
  - Dropped the redundant `|| requested == circulating` (`conflicting()` is already false for idle/circulating).

### Decided

- Heat/cool → circulating needs `MIN_ON_TIME` first; once passed it isn't blocked by min-off. The restart is what min-off guards (heat → circulating → heat waits out `MIN_OFF_TIME`).
- em_heating → heating keeps `cycle_start`, so `MAX_RUN_TIME` counts from furnace start (furnace can't run 4 h straight). Heating → em_heating resets it; accepted.
- `em_heating` and `heating` stay separate statuses.
- Em heat while cooling needs `ac_set` < `MIN_HEAT`; left to setpoint validation rather than special-cased.
- No network auth on the setpoint API: access restricted by Tailscale/IP instead (idea.md).
- Weather gate: skip cooling when the next hour's outside temp is below `ac_set` (no AC in winter, none on a cool night with windows open); skip heating when it's above `heat_set`. Never gates em heat.
- Away mode gets an expiry.

### `update_node` review (Claude, standalone harness, not in repo)

Worked: 3-read trust, short error blips, step change rejected then re-accepted. Open:

- `MIN_VALID_TEMP` = 50 °F: a house at 49 °F reads as a sensor fault → `no_data` → idle, so heat stops while pipes freeze. Lower the bound.
- ~~`consistent` isn't reset when trust drops from age: after a 4 min outage the first good read is trusted at once, not after 3.~~ Fixed 10-08 by user (braces added after a test caught the first attempt); `RegainsTrustOnlyAfterConsistentReadsFollowingOutage` passes.
- Nothing flags a flat/stuck sensor (3 h of identical 68.0 °F stayed trusted). `PROGRESS_WINDOW` / `PROGRESS_MIN_DELTA` are defined in `constants.h` but unused; the check needs `sys_status` and `cycle_start`, which `update_node` doesn't see.
- Rate limit is 1.1 °F per 10 s read: a 1.5 °F step drops trust for ~30 s, and with one sensor that means idle.
- `fails_in_row` / `MAX_SENSOR_FAILS` unused; `started_at` not reset after `get_sensor_data` restarts measuring.

### Open

- Em-heat max-run restarts after one pass (needs fault state or progress check).
- Software watchdog (systemd `WatchdogSec`): the max-run check lives in the loop, so a hung loop holds a relay closed.
- `hour_of_day()` counts 9 pm–midnight as day.
- Away expiry and persisted settings must use wall-clock time (`now_ms()` resets at boot; the Pi has no RTC).
- Weather gate: fail open on a stale or failed fetch; consider a margin/ceiling so the gate can't hold the house far from setpoint.
- Remaining work: loop restructure (`poll()`, SIGTERM → relays off), ~~tests for `test_switch` (starting 10-07)~~ (done 10-08, exact boundaries still to write), setpoint validation, networking (API, satellite polling, persistence), weather gate, away/return, log ring, web apps.

## 2026-10-07 — Unit test harness (googletest)

Built by Claude; test cases are the user's.

- googletest v1.17.0 as a submodule at `tests/googletest`. `python3 tests/run_tests.py` builds and runs on the dev machine (gtest compiled once into `tests/build/`; extra args go to the binary, e.g. `--gtest_filter`).
- Links `pi/control.cpp` + `pi/validation.cpp` against `tests/fake_relay.cpp` (link seam: same `relay.h` API, no GPIO, can force a failure). First time the control code has linked off the Pi.
- Two example tests pass (`local_sensor_status` error; `apply_status` leaves timers alone on relay failure). `test_switch` / `update_node` cases still to write.
- `get_HVAC_command` can't be tested deterministically yet: `hour_of_day()` reads the wall clock.

## 2026-10-08 — `test_switch` tests: standard operation

Cases chosen by user, written by Claude. 12 pass (6 sequences × heat/cool), driven through `test_switch` + `apply_status` like `main()`'s loop:
restart via idle / via fan after min-off, heat ↔ cool via idle, restart via idle / via fan blocked within min-off, max-run turns off.

Not covered yet: min-on blocking, conflicting command (heat ↔ cool direct), em heat, fan max-run, `no_data`, exact boundaries. Found while writing: circulating → heating/cooling leaves the fan relay on.

### Later 10-08 — remaining `test_switch` situations

Written by Claude; user decided fan relay must be off during a heat/cool call. 28 pass, 2 fail, 1 disabled.

- Added: min-on blocks idle/fan, `no_data` → idle after min-on, conflicting call (heat ↔ cool), em heat (from idle, over heating, em → heating keeps max-run clock, max-run, cooling → em never closes both relays), fan only (from idle, min-on, max-run).
- **Failing (heat + cool):** fan → heat/cool leaves the fan relay on; `apply_status` needs to drop it.
- **Disabled:** em heat staying off after max-run (known open item; enabling it shows it fails today).
- Test current behavior, still undecided: conflicting call goes idle without min-on; fan max-run stops it for one pass.

### Later 10-08 — decisions on undecided tests

- Heat ↔ cool call going straight to idle (skips min-on): design decision, test kept.
- Fan stopping for one pass at max-run: bug, fix not chosen. Test now `DISABLED_MaxRunDoesNotFlipFlop` (fails if enabled; doesn't presume a fix).

## 2026-10-08 — Sensor health tests; TODO.md

Written by Claude; the outage case was the user's. 39 pass, 3 fail, 2 disabled.

- 13 `update_node` / `local_sensor_status` tests: 3-read trust, warming, error before any read, blip, outage past `MAX_READING_AGE`, regain after outage, `fails_in_row`, out of range, jump then new level accepted, slow change stays trusted.
- User's fix for trust coming back on the first read after an outage: **works** (`RegainsTrustOnlyAfterConsistentReadsFollowingOutage` passes).
- **But it breaks short blips:** the `if` has no braces, so `consistent = 0` runs on every error. Trust drops on the first good read after a one-read blip. `ShortErrorBlipKeepsTrust` fails; gcc warns `-Wmisleading-indentation`.
- [TODO.md](TODO.md) started: current bugs and to-dos, pulled from the open items above. Log keeps the history.

## 2026-10-08 — Rest of the testable code

Written by Claude. 88 tests: 86 pass, 2 fail (fan relay, known), 2 disabled.

- User added the missing braces in `update_node`'s error branch: `ShortErrorBlipKeepsTrust` now passes.
- `get_HVAC_command` (13): each command, deadband, spread fan, `no_data`, weighting, untrusted sensors left out of average and spread. Equal day/night weights so `hour_of_day()` can't affect results. Records current behavior on two undecided cases (weight-0 sensor, cool before em heat).
- `apply_status` direct (13): timer stamps for 12 switches (table-driven), fan never turned on if `all_off()` fails.
- `net.cpp` over localhost (7): status reply format, `nc`-style empty request, short buffer truncated + terminated, refused/bad address/zero-length buffer, `net_serve(-1)` keeps timing.
- Real `relay.cpp` (11), first time tested anywhere: a second binary (`relay_tests`) links it against `fake_gpiod.cpp` (copy of the Pi's `gpiod.h` in `tests/gpiod/`). Interlock both ways (pin never written), active-low outputs starting off, failed write, `all_off` keeps going past a failure, close releases.
- `update_node` (2): warming drops trust; same-timestamp reads.

### Later 10-08 — weight 0

- Decided: weight 0 = sensor deliberately ignored. `OnlyZeroWeightSensorIsNoData` relabeled by design.
- ~~Open: an ignored (weight 0) sensor still counts toward the fan spread.~~ Closed 10-08: weight 0 is never configured.

## 2026-10-08 — Fan design question; test coverage wrap-up

Design only, nothing built.

- **Fan keeps causing bugs** (relay left on, max-run blip, timer special cases). Raised by Claude: treat the fan as its own control alongside heat/cool (how real thermostats do auto/on; "orthogonal regions" in statecharts) instead of a `circulating` status in the HVAC state machine. Undecided; the fan max-run fix waits on it.
- **Deliberately not unit-tested:** day/night weighting (`hour_of_day()` reads the clock), `sensor.c` (needs a fake Sensirion I2C layer), `main()` (logic not callable until the loop restructure). Listed in TODO.md.
- TODO.md gained a "Decide" section for tests that record current behavior; only cool-before-em-heat remains.

### Open

- ~~Everything from 10-07 on is uncommitted: googletest submodule, `tests/`, TODO.md, `.gitignore`, user's `validation.cpp` fix.~~ Committed 10-08 on branch `unit-tests`.

## 2026-10-12 — Em heat / fan bypass; fan relay fix

Design changes by the user; test updates and the `apply_status` move by Claude. 89 pass, 0 fail, 0 disabled.

- **Em heat and fan now bypass min-off, min-on and max-run** in `test_switch`. Closes the em-heat restart-after-max-run and fan flip-flop bugs by removing max-run from both. Tests relabeled by design: `EmergencyHeat.RunsPastMaxRun`, `FanOnly.RunsPastMaxRun`, `TestSwitch.FanSkipsMinOn`.
- **Fan relay left on after circulating → heat/cool: fixed.** User's fix was in `main()`; moved into `apply_status` (fan off before heat/cool on, like the circulating case drops heat/cool first) so `RestartsViaFanAfterMinOff` covers it.
- 9 pm–midnight counting as day: by design (user's schedule).
- `MIN_VALID_TEMP` 50 → 30 °F. `control.cpp` includes trimmed.

## 2026-10-15 — Frozen sensor check; fault-detection design (in progress)

Frozen check written by Claude to the user's spec. 94 pass, 0 fail, 0 disabled.

- **Frozen sensor:** `STUCK_READS` (3) readings in a row with temp, humidity **and** CO2 all identical → untrusted, `consistent` set to `CONSISTENT_READS - STUCK_RECOVERY_READS` (−7), so 10 good reads to trust again. Staying frozen keeps resetting it; a rejected read doesn't shorten it. All three because a rounded temp alone can repeat by chance.
- Mostly guards satellites resending a stale value: the local SCD41 already fails on "no new measurement ready".
- Test fixture now bumps CO2 1 ppm per read (identical reads would trip the check).

Design discussion, not settled:

- Max-run as a fault detector → replace with a progress check (no room temp change while running). Old Honeywell on W is the freeze backstop, so the Pi may stop the furnace on a suspected fault.
- No supply-air sensor (user's call). Kitchen (oven) and office (PC) can only add heat: they can hide a dead furnace, not fake one.
- Dead furnace vs. losing to the cold: outdoor temp from NWS hourly forecast (`api.weather.gov`, hourly fetch; on failure use the multi-day forecast already fetched).
- Planned `error` status, auto-retry after 30-60 min.
- Overheating from a low-reading sensor: user's view is no separate ceiling needed (a low enough reading to call heat would be untrusted or near setpoint).

### Open

- Progress check + `error` status: held until after bench tests.
- Retry cap / alerting; iced AC coil needs thaw time before retry.
- Weather fetch: HTTPS + JSON library choice; NWS wants an identifying User-Agent.

## 2026-11-17 — Settings files (set points, vacations)

Design by the user; code and tests written by Claude. 113 pass (24 new), 0 fail.

- `data_store.h/.cpp`: load at startup, validate requests, atomic write, retry on failed write, vacation cleanup, `is_away()`, US time ↔ Unix. Replaces the user's draft `get_set_points` (its `process_request` didn't compile: `json::parse` on an already-parsed `json`).
- Set points: heat alone pushes AC to `heat + MIN_DEAD_BAND`, AC alone pushes heat down; any push out of range rejects the request. `AC_MAX` = 90. `MIN_HEAT + MIN_DEAD_BAND` floor on night heat kept, by design.
- Change order: validate → RAM → disk. A failed write keeps the new values in use and retries each pass (user's call: they're validated and are what's wanted).
- Vacations: Unix seconds on disk, US format (`MM/DD/YYYY HH:MM`) in the API. End after start; overlaps kept as separate blocks but act as one span; 2 h pre-conditioning lead, fixed. Finished blocks deleted. Away temps global (freeze protection only).
- Missing/bad `hvac_points.json` → defaults. Bad vacation block → dropped, others load (Claude's choice, not discussed).
- **Bug found:** `deploy.sh`'s `rsync --delete` would have overwritten the Pi's live settings with `pi/`'s example files on every deploy. Both files are now excluded.
- `main.cpp` loads settings and uses `settings.points`; cleanup + retry run each pass. Not built or run on the Pi yet.

### Open

- Away and night set points not applied in control yet (TODO.md).
- HTTP handlers exist; no server to call them.
- Wall clock before NTP sync could delete vacations early.

## 2026-11-18 — Night, away, mode, season lockout, weather gate

Design by the user; code and tests written by Claude. 155 pass (31 new), 0 fail.

- `get_HVAC_command(sensors, points, is_night, allowed, outside_temp)`: takes the set points struct; night drops the whole band; manual/season mode; 5° weather gate both ways on the night-adjusted points; em heat never gated. The clock is read in `main` (`is_night_now()`), which closes the "`hour_of_day()` reads the clock" bug; day/night weighting now tested.
- Away: `AWAY_POINTS` {62, 90, 0, fan off} replaces the user's points. Fan blocked by a `fan_allowed` bool (user rejected a magic threshold).
- Mode `auto`/`heat`/`cool`/`both` in `hvac_points.json`; manual stays until set back. Old files without it load as `auto`.
- `weather.json` (new, `weather.cpp`): a week of hourly forecast, refreshed daily. Season from the average daily low: < 55 heat only, > 65 cool only. No expiry (user: AC in winter is the failure that matters).
- Season data kept out of `hvac_set_points` (user had proposed it): the away swap would have dropped it, and the daily weather write would rewrite user settings.
- Claude's choices, not discussed: no weather at all → heat only in auto; days with < 20 hours left out of the average; 65 picked from the user's 60-65.

### Open

- Weather fetch not written (`weather_due()` / `update_weather()` ready). Until then auto = heat only.
- `SUMMER_AVG_LOW` 65 untested against a real season.

---

## 2026-11-18 (later) — New display module: ST7789V + touch; full rewire; bench re-check

Wiring plan, `config.txt`, init file and bench scripts by Claude; wiring, bring-up and testing by user.

New module: **ST7789V** panel (240x320, run as 320x240 landscape) **with a working touch controller** (answers XPT2046 commands). Replaces the MSP2401 ILI9341. **The 09-22 to 09-24 display notes are specific to that module** and don't carry over: SDO not releasing MISO, the MADCTL table, the LED current, the ILI9341 `panel.bin`.

### Header as wired

| Phys | GPIO | Used by |
|---|---|---|
| 1 | 3.3V | SCD41 VCC |
| 2 | 5V | Relay board VCC |
| 3 | 2 | SCD41 SDA (I2C1) |
| 5 | 3 | SCD41 SCL (I2C1) |
| 6 | GND | Relay board GND |
| 7 | 4 | Relay W |
| 9 | GND | SCD41 GND |
| 12 | 18 | T_CS (SPI1 CE0) |
| 17 | 3.3V | Display VCC |
| 18 | 24 | Display DC |
| 19 | 10 | Display SDI (SPI0 MOSI) |
| 20 | GND | Display GND |
| 22 | 25 | Display RESET |
| 23 | 11 | Display SCK (SPI0 SCLK) |
| 24 | 8 | Display CS (SPI0 CE0) |
| 29 | 5 | Relay Y |
| 31 | 6 | Relay G |
| 32 | 12 | Display LED (via transistor, active-high) |
| 35 | 19 | T_DO (SPI1 MISO) |
| 36 | 16 | T_IRQ (active-low, pull-up in `config.txt`) |
| 38 | 20 | T_DIN (SPI1 MOSI) |
| 40 | 21 | T_CLK (SPI1 SCLK) |

- **Touch on its own bus (SPI1).** T_CLK can't share SCK, and a bus has only one SCLK pin. Side effect: display SDO never shares MISO with touch. SDO unconnected; driver stays `write-only`.
- Power/ground regroup from 09-24 done: each device's wires sit together.
- Free GPIO: 7, 9, 13, 17, 22, 23, 26, 27 (7 and 9 are claimed by `spi=on`). Off-limits as before: 0/1, 14/15.

### `config.txt` (`[all]`)

```
gpio=4,5,6=ip,pu
gpio=16=ip,pu

dtoverlay=mipi-dbi-spi,spi0-0,speed=8000000
dtparam=width=320,height=240
dtparam=reset-gpio=25,dc-gpio=24
dtparam=write-only
dtparam=backlight-gpio=12

dtoverlay=spi1-1cs
```

`spi1-1cs` last, so the display's `dtparam=` lines still bind to `mipi-dbi-spi`.

### Display init (`/root/panel-mipi-dbi/st7789v.txt`)

Sequence: software reset, sleep out, `0x3A 0x55` (RGB565), MADCTL, inversion, normal mode, display on. No vendor code; datasheet commands plus power-on defaults.

- **Inversion OFF (`0x20`) is correct for this panel.** Claude assumed INVON (`0x21`), common on ST7789 IPS panels; `0x21` gave every colour as its complement (white→black, red→cyan).
- **MADCTL `0x60` with `width=320,height=240`: in use** (landscape, right side up). `0xA0` = upside down. Garbled, wrapping text seen along the way had a separate cause, since resolved; not the MADCTL pairing.
- Inline `# comments` in the init file are fine (`xxd` showed the expected bytes).

### What cost time

- **Results from a file that was never installed.** One `0x20` run was misread because the install state was unclear. Later, a pasted block run from `/root` instead of `/root/panel-mipi-dbi`: the build and copy failed, but the pasted lines after them still ran, **including the reboot**. The screen showed the old file.
- Fix: `/root/panel-mipi-dbi/install.sh` (`set -e`, `cd`s itself, shows `xxd` of the installed file, asks before rebooting). **Prove the file is applied** with a visible change (MADCTL flip) before trusting any result.
- The "backup the ILI9341 file" line was in the repeated block, so each rerun overwrote the backup with an ST7789V build. Backup deleted.
- `test_display.py` is ILI9341-only and bypasses the kernel driver; the wrong tool here. Display checks now write to `/dev/fb1` directly (colour bands; border + diagonal for geometry), reading the size from sysfs.

### Bench re-check — all passed

- **Relays:** `pinctrl get 4-6` = `ip pu | hi` at boot; each channel clicks, LED, COM–NO continuity only while energized.
- **Supply:** `vcgencmd get_throttled` = `0x0` with all three relays, backlight and SCD41 on.
- **SCD41:** `0x62` on `i2cdetect`, live readings via `i2ctransfer`; CO2 and RH jump when breathed on.
- **Touch:** T_IRQ goes `lo` on press; raw reads over `/dev/spidev1.0` at 1 MHz:

| Corner | raw X | raw Y |
|---|---|---|
| Top left | ~450 | ~520 |
| Top right | ~420 | ~3650 |
| Bottom left | ~3650 | ~430 |
| Bottom right | ~3550 | ~3500 |

Axes **swapped, not mirrored**: screen left→right = raw Y, top→bottom = raw X (touch layer is portrait-native). Holds for MADCTL `0x60`; a 180° flip reverses both. Stray **4095** and low readings at touch-down/lift: needs filtering (TI SBAA036; pressure from X, Z1, Z2 — Z1 alone ranged 108–1044 for similar presses).

### Open

- Touch filtering + calibration in the display program.
- Touch driver: kernel `ads7846` overlay targets SPI0 only (to verify), so either a custom overlay or userspace reads on `/dev/spidev1.0`.
- Pushbuttons: keep as a fallback or drop for touch.
- Still open from 09-22: SPI clock back to 32 MHz; idle blanking with `backlight-gpio`.

## 2026-11-19 — State machine: manual fan, CO2 circulation (rewrite pass 1 of 2)

Design by the user; code and tests written by Claude. 169 pass (25 new), 0 fail.

- Fan question from 10-08 settled (user): fan stays a `circulating` status, not a separate control. Manual `fan_mode` on/auto (RAM only) is an input: fan-on circulates whenever heat/cool/em heat isn't called, even with no sensor data; away holds it off without clearing it.
- **A fan call no longer cuts heat/cool short.** Replaces `FanSkipsMinOn`: heat/cool finish min-on, then go to circulating instead of idle. Circulating always runs 5 min; only em heat cuts in.
- CO2 rule (user): a trusted room > 1000 ppm and > 1.2 × weighted average CO2 starts the fan; it runs until every trusted room ≤ 1.1 × average. Rooms with no trusted reading count as 700 ppm, so a lone sensor can still trigger.
- `control_state` (status, fan, CO2 memory, timers) + `control_step()` replace the loose variables in `main`, so a whole pass is testable.
- Bug found while rewriting: circulating longer than max-run, then a heat call, went to idle for one pass (heat's max-run was checked against the fan's `cycle_start`). Max-run now applies to heating/cooling only.
- Claude's choices, not discussed: max-run with the fan called goes straight to circulating; heating/cooling also turn the *other* relay off first (cooling → em heat was refused by the interlock every pass); away clears the CO2 memory; only trusted rooms can trigger CO2.

### Open

- Pass 2: threads + cpp-httplib API, history ring, weather fetch thread, main loop.
- CO2 thresholds depend on SCD41 calibration (ASC off, no forced recalibration yet).

## 2026-11-19 (later) — Threads, HTTP API, history ring, weather fetch (rewrite pass 2 of 2)

Design by the user; code and tests written by Claude. 194 pass + 11 relay, 0 fail. Not yet built or run on the Pi.

- **Threads around one mutex** (`thermostat.h`): control pass every 60 s (`control_tick`), cpp-httplib v0.64.0 (vendored, `pi/third_party/`) serving the API, a weather thread, `main` waiting in `sigwait`. Replaces the `poll()` plan. ThreadSanitizer run on the API tests (incl. a 4-client stress test): no races.
- **API** (`api.cpp`, port 8080): handlers are plain functions (tested without a server); `net.cpp` is the only file that includes httplib. LAN / loopback / tailnet only. Smoke-tested locally with curl against every endpoint.
- **Vacations in Unix seconds** over the API (user); `parse_us_time` / `format_us_time` removed. Unknown block id → 404 (new `request_result::not_found`).
- **History**: 24 h RAM ring, one entry per pass, starts once NTP has synced. No separate events buffer (user: every change happens in a pass).
- **Weather**: nightly, hourly tries 00:00–08:00 until one works; gridpoint/contact from env vars (`thermostat.env`, git-ignored). `FULL_DAY_HOURS` 20 → 16 (user). Real NWS fetch tested locally (example gridpoint): 156 hours parsed.
- Found: `compile.py` never listed `validation.cpp`, which `main` calls (`update_node`), so a Pi build would fail at link. Fixed; it also skips unchanged objects now.
- User's edits during pass 1 review, tests updated to match: em heat checked before cool (closes the TODO "Decide" item); CO2 average always uses day weights.
- Claude's choices, not discussed: before NTP sync no away, no vacation clean-up and no weather gate (control still runs); a fresh install fetches weather at any hour; 30 s NWS connect/read timeout; SIGTERM/SIGINT turn the relays off before exit; API errors carry a short `{"error"}` reason.

### Open

- On the Pi: `apt install libssl-dev`, create `thermostat.env`, `./deploy.sh build`.
- Satellite polling; watcher program; systemd unit.

## 2026-11-20 — First run on the Pi: fan held off heat

Found by the user on the first run. Fix by Claude, rule by the user. 195 pass + 11 relay.

- Circulating blocked a heat call for its full 5 min min-on. Rule changed: the fan's min-on only holds off circulating → idle; heat/cool take over as soon as min-off allows.
- Also seen: after a program start, heat waits 10 min (boot stamps `cycle_end`: power-blip protection) while the fan may start at once. Expected, not a bug.

## 2026-11-20 (later) — Display screens designed; forecast conditions + `GET /weather`

Screens designed by the user (idea.md → Display screens). Backend by Claude. 202 pass + 11 relay. Not yet run on the Pi.

- SPI raised to 32 MHz (user).
- Icon set: Meteocons (MIT), day/night variants. The first pick, an Adobe Stock preview, was dropped: not licensed for a public repo.
- Forecast: drag-scrolling dropped for paging (resistive touch is noisy at touch-down and lift).
- `hourly_temp` → `forecast_hour`, now also stores `condition` (NWS icon code from the `icon` URL, e.g. `rain_showers`), `is_day`, `short_forecast`. Old `weather.json` files still load (fields default). Real NWS reply checked (example gridpoint): codes present (`skc`, `few`, `sct`, `bkn`, `ovc`, `rain`, `tsra`).
- `GET /weather?hours=N` (default 24, max 168): from the hour containing now; `[]` if never fetched.
- Claude's choices, not discussed: the icon *code* is the key for choosing icons, not the `shortForecast` text (fixed list vs free text; text kept as a fallback); the first code when an hour lists two; the code→icon mapping is left to the display program.

### Open

- Display program: language/UI library, touch driver (kernel `ads7846` vs `/dev/spidev1.0`), NWS code → Meteocons mapping, animation approach.
- Night screen: where its Done goes (assumed: back to set points, still pending).

