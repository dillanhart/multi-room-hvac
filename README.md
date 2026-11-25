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
- **Display:** ST7789V 320x240 TFT over SPI, using the mainline `panel-mipi-dbi` DRM driver, with a resistive touch controller on a second SPI bus.

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

### Running as services

```sh
./deploy.sh build && ./deploy.sh display-build
./deploy.sh services   # install pi/systemd/*.service, enable, (re)start; asks for sudo
```

Both programs restart on a crash and on a hang (systemd watchdog). Logs: `journalctl -u thermostat -u thermostat-display -f`.

On the Pi:

```sh
systemctl is-enabled thermostat thermostat-display    # "enabled" = starts at boot
systemctl status thermostat thermostat-display
journalctl -u thermostat -u thermostat-display -f     # live logs
sudo systemctl stop thermostat thermostat-display     # before ./deploy.sh run / display-run
sudo systemctl start thermostat thermostat-display
```

Weather settings (`thermostat.env`) are loaded by the unit's `EnvironmentFile=`, or by `compile.py`. Running `./thermostat_control` by hand does **not** load them. Checks:

```sh
journalctl -u thermostat -b | grep weather   # "... not set" = not loaded; "N hours fetched" = a fetch ran
sudo tr '\0' '\n' < /proc/$(systemctl show -p MainPID --value thermostat)/environ | grep NWS
```

Running by hand with them (services stopped):

```sh
cd ~/thermostat
set -a; . ./thermostat.env; set +a   # export every KEY=VALUE in the file
./thermostat_control
```

### Relay and watchdog checks

```sh
curl -X PUT localhost:8080/fan -d '{"fan":"on"}'    # fan on at the next pass (<= 20 s); "auto" to undo
gpioinfo gpiochip0 | grep -E 'line +[456]:'         # relay pins: GPIO4 heat, 5 cool, 6 fan
pgrep -af thermostat_control                        # PID (pkill -x can't match: names are cut to 15 chars)
kill -9 <PID>                                       # crash: relay should drop (persist_gpio_outputs=n)
kill -STOP <PID>                                    # hang: systemd should kill + restart within ~60 s
gpioset -t0 -c gpiochip0 6=1                        # force the fan relay off by hand (active-low)
cat /sys/module/pinctrl_bcm2835/parameters/persist_gpio_outputs   # should be N
```

### Pi setup outside the repo

- `pinctrl_bcm2835.persist_gpio_outputs=n` at the end of the line in `/boot/firmware/cmdline.txt`. Add it with
  `sudo cp /boot/firmware/cmdline.txt /boot/firmware/cmdline.txt.bak && sudo sed -i '1 s/$/ pinctrl_bcm2835.persist_gpio_outputs=n/' /boot/firmware/cmdline.txt`, check it's still one line, reboot. The file must stay a single line; restore the `.bak` from another machine if the Pi won't boot.
  Without it, a crash or `kill -9` leaves a relay pin driving the relay on; with it, a released pin reverts to input and the pull-ups hold the relays off. Redo after reflashing the SD card.
- Packages: `libgpiod-dev`, `libssl-dev`, `libsystemd-dev`.
