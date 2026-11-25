# Known bugs and to-do

The current list. Delete items when they're done; the history of how each was found and closed lives in [log.md](log.md), and design rationale in [idea.md](idea.md).

Tests: `python3 tests/run_tests.py`. A failing or `DISABLED_` test is named next to the bug it shows.

## Bugs

- `started_at` isn't reset when `get_sensor_data` restarts measuring, so the warm-up is skipped.

## Safety

- Progress check (HVAC running but room temps not moving): `PROGRESS_WINDOW` / `PROGRESS_MIN_DELTA` defined but unused. Held until after bench tests. Feeds a planned `error` status with auto-retry (30-60 min, longer than min-off); thresholds scaled by outdoor temp from the weather API.
- `fails_in_row` / `MAX_SENSOR_FAILS` unused.
- ~~Software watchdog (systemd `WatchdogSec`): a hung loop holds a relay closed past max-run.~~ Written 11-25 (`sd_notify` per pass, `WatchdogSec=60`). **Untested**: freeze the loop (e.g. `kill -STOP`) with the fan on, expect a kill + restart within ~60 s and the relay off.
- ~~Relay state after `kill -9` / a crash while energized is untested.~~ Tested 11-25 (user): **relay stays on**. Need something that turns the pins off after any exit (relay-off helper on `ExecStopPost=`, or a watcher). Fixed 11-25: `pinctrl_bcm2835.persist_gpio_outputs=n` in `cmdline.txt` → released pins revert to input, relay off on `kill -9` (verified). Not in the repo: redo after a reflash.
- Setpoint validation: `MIN_DEAD_BAND`, and `ac_set` < `MIN_HEAT` so em heat can't be called while cooling.
- Heat ↔ cool skipping min-on (by design): comment it in `test_switch`, log it when it happens.

## Untested on hardware

- Fresh-start warming (needs a sensor power cycle), error-hold path, jump rejection.
- Calibrate against a reference thermometer (`set_temperature_offset`).

## Tests to write

- `test_switch` at the exact boundaries (`MIN_ON_TIME`, `MIN_OFF_TIME`, `MAX_RUN_TIME`; code uses `>` / `<=`).
- `sensor.c` (start/retry logic needs a fake Sensirion I2C layer). `main()`'s loops are thin now (`control_tick` is tested); the httplib wiring in `net.cpp` is only smoke-tested by hand.

## Design, still open

- Zone-spread fan logic not settled; `FAN_CLEAR` hysteresis unused. (Fan-as-status settled 11-19: manual on/auto + CO2 rule.)

## Features

- **Before the next Pi build:** `sudo apt install libssl-dev` (HTTPS for the weather fetch), and create `pi/thermostat.env` from `thermostat.env.example` (gridpoint + contact). Then `./deploy.sh build`. Watch RAM while `net.cpp` compiles (httplib is ~24k lines; Pi 3 has 1 GB).
- Satellite polling: map ESP32 `"status"` to `sensor_status`, then `update_node(nodes[1..2], …)` in `control_loop`, outside the lock. Until then they're null in the API and left out of the averages.
- `SUMMER_AVG_LOW` (65) is a first guess (user: 60-65); tune after a season.
- Install `nlohmann-json3-dev` on the Pi if a rebuild ever needs it (present in `.sysroot` as of 11-17).
- Web apps (Ubuntu site, display program). ~~systemd unit (`EnvironmentFile=thermostat.env`, `WatchdogSec`).~~ Written 11-25: `pi/systemd/`.

## Display and touch

- ~~SPI clock is 8 MHz in `config.txt`; the old ILI9341 ran at 32 MHz. Raise it for the ST7789V.~~ Done 11-20 (user): 32 MHz.
- Idle blanking unverified with `backlight-gpio` bound.
- ~~Touch filtering: stray 4095 / low reads at touch-down and lift (TI SBAA036; pressure from X, Z1, Z2).~~ Written 11-21 (median of 7, range + spread check, two agreeing reads before a press). **Untested on the Pi.**
- ~~Touch calibration: swap axes for landscape, map raw ~430-3650 to screen pixels. Reverses if the display is flipped 180°.~~ Written 11-21 from the corner table (`TOUCH_CAL`). **Untested on the Pi**; tune if taps land off-target.
- ~~Touch driver: kernel `ads7846` overlay (SPI0 only?) needs a custom SPI1 overlay, or read `/dev/spidev1.0` from the display program.~~ 11-21: userspace, `/dev/spidev1.0` + GPIO 16 via libgpiod.
- Display program first run on the Pi: `./deploy.sh display-run` (as root for the console switch, or see log.md 11-21). First LVGL build is long; watch RAM.
- Display: a Done pressed while the control program is down is logged and lost (no retry). Decide if that's OK.
- Decide: pushbuttons as a touch fallback, or none (not yet purchased).
