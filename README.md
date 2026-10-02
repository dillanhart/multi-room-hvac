# multi-room-hvac

A Raspberry Pi thermostat in C/C++ that controls a home furnace, AC and fan using temperature readings from several rooms, with fail-safe-off relay hardware.

**Status:** in progress. Hardware is bench-verified; the control logic is being written.

## Why

A single wall thermostat only knows the temperature in the hallway. This one is built to fix specific problems in my apartment: an office that overheats during long sessions, a bedroom that goes stagnant overnight, a sleep/wake temperature schedule, and pre-conditioning before I get home from a trip.

## How it works

- **Main controller:** Raspberry Pi 3 with an SCD41 temperature/humidity/CO2 sensor on I2C, driving the furnace's W (heat), Y (cool) and G (fan) lines through an opto-isolated relay board.
- **Room satellites:** ESP32 boards with their own SCD41s, polled over HTTP/JSON on the local network.
- **Control:** room readings are combined into a weighted temperature (weights shift between day and night), with hysteresis, minimum on/off times, a heat/cool interlock, and fault detection when heating or cooling isn't changing the temperature.
- **Fail-safe by hardware:** relays are wired normally-open on GPIOs whose reset state holds them off, so a crash, reboot or power loss drops every call to the furnace. The original thermostat stays wired in parallel on heat as freeze protection.
- **Display:** 2.4" ILI9341 TFT over SPI, using the mainline `panel-mipi-dbi` DRM driver.

## Repository layout

| Path | Contents |
|---|---|
| [pi/](pi/) | Everything deployed to the Pi: control loop, relays, sensor, networking, build script |
| [pi/embedded-i2c-scd4x](https://github.com/Sensirion/embedded-i2c-scd4x) | Sensirion's SCD4x driver (submodule) |
| [test_scripts/](test_scripts/) | Python bench tests used during display and SPI bring-up |
| [images/](images/) | Wiring and hardware photos |
| [idea.md](idea.md) | Design rationale and open decisions |
| [log.md](log.md) | Dated build log, including dead ends and what they taught me |
| [deploy.sh](deploy.sh) | Syncs `pi/` to the Pi over SSH and optionally builds and runs it |

## Build and deploy

Development happens on a separate machine. `pi/` is synced to the Pi and built there:

```sh
git clone --recurse-submodules https://github.com/dillanhart/multi-room-hvac
./deploy.sh diff   # preview what would change on the Pi
./deploy.sh run    # sync, build with compile.py, and run
```

On the Pi, `compile.py` builds with `gcc`/`g++ -std=c++20` and links against `libgpiod` (v2).
