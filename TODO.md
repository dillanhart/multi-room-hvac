# Known bugs and to-do

The current list. Delete items when they're done; the history of how each was found and closed lives in [log.md](log.md), and design rationale in [idea.md](idea.md).

Tests: `python3 tests/run_tests.py`. A failing or `DISABLED_` test is named next to the bug it shows.

## Bugs

- **Fan relay stays on during heat/cool.** `apply_status` turns on Heat or Cool without turning off Fan after circulating. *Test: `TestSwitch.RestartsViaFanAfterMinOff` (fails, heat + cool).*
- **Em heat restarts one pass after max-run.** Needs a fault state that stays off, or a progress check. *Test: `EmergencyHeat.DISABLED_StaysOffRightAfterMaxRun`.*
- **Fan max-run stops it for one pass, then idle → fan restarts it.** Fix not chosen. *Test: `FanOnly.DISABLED_MaxRunDoesNotFlipFlop`.*
- **Cold house reads as a sensor fault.** `MIN_VALID_TEMP` = 50 °F: below it → untrusted → `no_data` → idle, so heat stops when it's needed most (frozen pipes).
- **`hour_of_day()` counts 9 pm–midnight as day.** It also reads the wall clock, so `get_HVAC_command` can't be tested deterministically.
- **Rate limit too tight for one sensor.** 1.1 °F per 10 s read: a 1.5 °F step drops trust for ~30 s, and with one sensor that means idle.
- `started_at` isn't reset when `get_sensor_data` restarts measuring, so the warm-up is skipped.
- `calibrate.c`: comment says default 425 ppm, `DEFAULT_TARGET_PPM` is 438.

## Safety

- Stuck/flat sensor detection: `PROGRESS_WINDOW` / `PROGRESS_MIN_DELTA` defined but unused (needs `sys_status` + `cycle_start`, which `update_node` doesn't see).
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
- `get_HVAC_command` day/night weighting (blocked on `hour_of_day()` reading the clock; everything else is tested with equal day/night weights).
- `sensor.c` (start/retry logic needs a fake Sensirion I2C layer) and `main()` (logic can't be called from a test until the loop restructure).

## Decide (tests record current behavior)

- Cool is checked before emergency heat (only reachable with `ac_set` < `MIN_HEAT`). *Test: `HvacCommand.CoolCheckedBeforeEmergencyHeat`.*

## Design, still open

- Fan: `circulating` as a status in the HVAC state machine vs. a separate fan control alongside it. Zone-spread fan logic not settled; `FAN_CLEAR` hysteresis unused.
- HTTP server library (Mongoose vs libmicrohttpd); settings file format.

## Features

- Loop restructure: `poll()` instead of `sleep()`.
- Networking: setpoint API, satellite polling (map ESP32 `"status"` to `sensor_status`, then `update_node(nodes[1..2], …)`), persisted settings.
- Weather gate: fail open on a stale or failed fetch; margin/ceiling so it can't hold the house far from setpoint.
- Away mode with expiry. Away expiry and persisted settings need wall-clock time (`now_ms()` resets at boot; no RTC).
- Log ring, web apps.
- Buttons (not yet purchased).

## Display

- SPI clock is 8 MHz in `config.txt`, but 32 MHz worked; restore it or find out why 8 is needed.
- Idle blanking unverified with `backlight-gpio` bound.

## Cleanup

- `control.cpp` includes `<chrono>` but uses `<ctime>` functions.
