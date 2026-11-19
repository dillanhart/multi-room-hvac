# Known bugs and to-do

The current list. Delete items when they're done; the history of how each was found and closed lives in [log.md](log.md), and design rationale in [idea.md](idea.md).

Tests: `python3 tests/run_tests.py`. A failing or `DISABLED_` test is named next to the bug it shows.

## Bugs

- **Rate limit too tight for one sensor.** 1.1 °F per 10 s read: a 1.5 °F step drops trust for ~30 s, and with one sensor that means idle.
- `started_at` isn't reset when `get_sensor_data` restarts measuring, so the warm-up is skipped.

## Safety

- Progress check (HVAC running but room temps not moving): `PROGRESS_WINDOW` / `PROGRESS_MIN_DELTA` defined but unused. Held until after bench tests. Feeds a planned `error` status with auto-retry (30-60 min, longer than min-off); thresholds scaled by outdoor temp from the weather API.
- `fails_in_row` / `MAX_SENSOR_FAILS` unused.
- Software watchdog (systemd `WatchdogSec`): a hung loop holds a relay closed past max-run.
- SIGTERM → relays off. Relay state after `kill -9` while energized is untested.
- Setpoint validation: `MIN_DEAD_BAND`, and `ac_set` < `MIN_HEAT` so em heat can't be called while cooling.
- Heat ↔ cool skipping min-on (by design): comment it in `test_switch`, log it when it happens.

## Untested on hardware

- Fresh-start warming (needs a sensor power cycle), error-hold path, jump rejection.
- Calibrate against a reference thermometer (`set_temperature_offset`).

## Tests to write

- `test_switch` at the exact boundaries (`MIN_ON_TIME`, `MIN_OFF_TIME`, `MAX_RUN_TIME`; code uses `>` / `<=`).
- `sensor.c` (start/retry logic needs a fake Sensirion I2C layer) and `main()` (logic can't be called from a test until the loop restructure).

## Decide (tests record current behavior)

- Cool is checked before emergency heat (only reachable with `ac_set` < `MIN_HEAT`). *Test: `HvacCommand.CoolCheckedBeforeEmergencyHeat`.*

## Design, still open

- Fan: `circulating` as a status in the HVAC state machine vs. a separate fan control alongside it. Zone-spread fan logic not settled; `FAN_CLEAR` hysteresis unused.
- HTTP server library (Mongoose vs libmicrohttpd).

## Features

- Loop restructure: `poll()` instead of `sleep()`.
- Networking: setpoint API (handlers exist in `data_store.h`: `update_set_points`, `add/change/remove_vacation`, `settings_json`; need the HTTP server), satellite polling (map ESP32 `"status"` to `sensor_status`, then `update_node(nodes[1..2], …)`).
- Weather fetch: once a day when `weather_due()`, hourly forecast → `update_weather()`. HTTPS + NWS User-Agent; the gate, season and storage are built and tested without it. Until it exists, auto mode is heat-only (no weather = no cooling); set the mode by hand.
- `SUMMER_AVG_LOW` (65) is a first guess (user: 60-65); tune after a season.
- Wall clock before NTP sync: no RTC, so right after boot `time()` can be wrong. A clock that runs ahead would delete vacations early (`remove_finished_vacations`). Consider waiting for sync (`timedatectl show -p NTPSynchronized`).
- Install `nlohmann-json3-dev` on the Pi if a rebuild ever needs it (present in `.sysroot` as of 11-17).
- Log ring, web apps.

## Display and touch

- SPI clock is 8 MHz in `config.txt`; the old ILI9341 ran at 32 MHz. Raise it for the ST7789V.
- Idle blanking unverified with `backlight-gpio` bound.
- Touch filtering: stray 4095 / low reads at touch-down and lift (TI SBAA036; pressure from X, Z1, Z2).
- Touch calibration: swap axes for landscape, map raw ~430-3650 to screen pixels. Reverses if the display is flipped 180°.
- Touch driver: kernel `ads7846` overlay (SPI0 only?) needs a custom SPI1 overlay, or read `/dev/spidev1.0` from the display program.
- Decide: pushbuttons as a touch fallback, or none (not yet purchased).
