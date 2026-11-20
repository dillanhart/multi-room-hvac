# Known bugs and to-do

The current list. Delete items when they're done; the history of how each was found and closed lives in [log.md](log.md), and design rationale in [idea.md](idea.md).

Tests: `python3 tests/run_tests.py`. A failing or `DISABLED_` test is named next to the bug it shows.

## Bugs

- `started_at` isn't reset when `get_sensor_data` restarts measuring, so the warm-up is skipped.

## Safety

- Progress check (HVAC running but room temps not moving): `PROGRESS_WINDOW` / `PROGRESS_MIN_DELTA` defined but unused. Held until after bench tests. Feeds a planned `error` status with auto-retry (30-60 min, longer than min-off); thresholds scaled by outdoor temp from the weather API.
- `fails_in_row` / `MAX_SENSOR_FAILS` unused.
- Software watchdog (systemd `WatchdogSec`): a hung loop holds a relay closed past max-run.
- Relay state after `kill -9` / a crash while energized is untested (clean SIGTERM/SIGINT now turns every relay off). Planned: a separate watcher program that turns the pins off when the control program exits; it can only claim the pins after the program has released them.
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
- Web apps (Ubuntu site, display program). systemd unit (`EnvironmentFile=thermostat.env`, `WatchdogSec`).

## Display and touch

- SPI clock is 8 MHz in `config.txt`; the old ILI9341 ran at 32 MHz. Raise it for the ST7789V.
- Idle blanking unverified with `backlight-gpio` bound.
- Touch filtering: stray 4095 / low reads at touch-down and lift (TI SBAA036; pressure from X, Z1, Z2).
- Touch calibration: swap axes for landscape, map raw ~430-3650 to screen pixels. Reverses if the display is flipped 180°.
- Touch driver: kernel `ads7846` overlay (SPI0 only?) needs a custom SPI1 overlay, or read `/dev/spidev1.0` from the display program.
- Decide: pushbuttons as a touch fallback, or none (not yet purchased).
