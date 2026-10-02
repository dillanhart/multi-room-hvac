# Build Log

Chronological record of work actually performed. Design rationale and open decisions live in [idea.md](idea.md).

---

## Prior to 2026-09-21

- Pulled existing Honeywell RTH111B1001 and probed terminals. Found 4 wires: **R, G, W, Y**. No C-wire at thermostat. Rh/Rc jumper in place.
- Selected components: Pi 3 (Pi OS Lite x64), FL-3FF-S-Z opto-isolated relay board (low-level trigger), SCD41, ESP32 satellites.
- Ordered all parts.

---

## 2026-09-21 — Parts arrived; relay interface bench-verified

### Display identification

- Display received is the **2.4", 240x320** variant. Controller is **ILI9341**, not ST7735 — ST7735 cannot address 240x320. The listing's "ST7735 Drive IC" text is boilerplate spanning all size variants.
- **idea.md:60 is stale** — it describes the 1.8"/128x160 ST7735S option.

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

Tested with all screw terminals empty; nothing connected to HVAC. Board powered from Pi 5V, jumper left in factory position.

**Note:** Pi 1/2/3 pull registers are write-only, so `pinctrl get` reports pull as `--`. Pull state can only be inferred from the pin level.

Results:

- **Board pull-up dominates.** With board connected, forcing `pinctrl set 4 ip pd` still reads `hi`; disconnected, the level follows the internal pull. Board's ~1kΩ pull-up beats the Pi's ~50kΩ internal pull-down. Relays are held open by hardware, independent of software or whether Linux boots.
- **All relays open in every tested state:** cold boot, `reboot` from energized, `shutdown -h now` from energized, and power cord pull from energized.
- **All 3 channels switch correctly** via `pinctrl set N op dl` / `op dh`. Clean release on high — no buzzing or partial release, so the 5V-VCC vs 3.3V-logic marginal-release issue does not apply to this board.
- **Terminals labeled correctly** — COM/NO/NC match silkscreen.
- **`vcgencmd get_throttled` = `0x0`** with all three relays energized. No undervoltage. **Single supply off the Pi is sufficient; no separate 5V brick needed.** Re-check once the display backlight is wired.

**Conclusion:** relay interface behaves exactly as the fail-safe design requires. Cleared to proceed toward HVAC wiring.

### Dev access

Headless dev access working: root console autologin, key-based root SSH from both dev machines, root password left locked so no password auth is possible. SSH restricted to the Tailscale tailnet (node `thermostat`).

---

## 2026-09-22 — Display bring-up

Panel confirmed as **ILI9341**, and the module is the **14-pin** variant (not 7-pin): `VCC GND CS RESET DC SDI SCK LED SDO T_CLK T_CS T_DIN T_DO T_IRQ`. The **LED pin is a separate backlight input that must be driven** — left floating the panel is dark and looks dead even though everything else is working correctly.

| Display | Pi GPIO | Phys |
|---|---|---|
| VCC | 3.3V | 1 |
| GND | GND | 9 |
| CS | GPIO 8 (CE0) | 24 |
| RESET | GPIO 25 | 22 |
| DC | GPIO 24 | 18 |
| SDI | GPIO 10 (MOSI) | 19 |
| SCK | GPIO 11 (SCLK) | 23 |
| LED | 3.3V | 17 |

**`fbtft` was tried first and abandoned.** It bound fine, but roughly the first 1/4 of the framebuffer never refreshed — on both the `write()` and `mmap` paths, at 16 MHz as well as 32 MHz, and the dead region moved when `rotate` changed, so it tracked framebuffer coordinates rather than the glass. A dirty-tracking fault in a staging module, not worth debugging.

### Working configuration

Mainline DRM **`panel-mipi-dbi`** driver. Init commands live in a blob at `/lib/firmware/panel.bin`, built with `mipi-dbi-cmd` from <https://github.com/notro/panel-mipi-dbi> using the ILI9341 sequence from that project's wiki (Displays page). `dtparam=spi=on` plus, under `[all]` in `config.txt`:

```
dtoverlay=mipi-dbi-spi,spi0-0,speed=32000000
dtparam=width=320,height=240
dtparam=reset-gpio=25,dc-gpio=24
dtparam=write-only
```

In `ili9341.txt`: `command 0x36 0x40` (MADCTL — landscape, RGB, X-flipped).

In `cmdline.txt`: `fbcon=map:1`.

Binds as `/dev/fb1`, 320x240 RGB565, stride 640. **Verified**: 8 evenly spaced lines full width, and 4 equal colour bands reading white→red→green→blue top to bottom.

`write-only` is required because SDO/MISO is not wired.

### Things that are not obvious and cost most of the day

**`fbcon=map:1` is mandatory.** DRM only pushes pixels once a mode is set and the pipeline is enabled. Without a console mapped to the framebuffer, writes to `/dev/fb1` land in a shadow buffer, nothing reaches the panel, and it sits on its power-on white looking dead. This is the main behavioural difference from fbtft, which pushed blindly on a timer regardless of pipeline state.

**`panel.bin` is read once at driver probe.** Every init-sequence change is edit → recompile → **reboot**, in that order. Check `uptime -s` against the blob's timestamp; recompiling *after* a reboot silently tests the previous build, which happened twice and sent the diagnosis in the wrong direction both times.

**Test scripts must read geometry from the kernel**, not hardcode it:

```python
w, h = map(int, open('/sys/class/graphics/fb1/virtual_size').read().strip().split(','))
stride = int(open('/sys/class/graphics/fb1/stride').read().strip())
```

A script with hardcoded dimensions that disagree with `config.txt` produces output indistinguishable from a driver bug. This also caused a wrong diagnosis.

**MADCTL and `width`/`height` must be paired correctly, and the pairing is empirical.** MV set (`0x20`) pairs with 240x320; MV clear pairs with 320x240. Which combination works is not derivable from the MV bit alone — test any new combination rather than reasoning about it.

| MADCTL | `width`/`height` | Result |
|---|---|---|
| `0x00` | 320/240 | landscape, mirrored |
| **`0x40`** | **320/240** | **landscape, correct — in use** |
| `0x80` | 320/240 | landscape, Y-flipped |
| `0xC0` | 320/240 | landscape, 180° from `0x00` |
| `0x20` | 240/320 | portrait, correct |

Bit 3 (`0x08`) is BGR; leaving it clear gives correct RGB on this panel. `0x40`/`0x80` differ only by which edge is "up", so a mounting flip is a one-byte change, not a reprint.

### Failure signatures, for future reference

Mismatched geometry produces symptoms that look like hardware faults but are not:

- **Exactly 3/4 of the panel painted, rest stale** — declared dimensions disagree with the controller's addressing window.
- **A gradient that resets partway down** — address wrapping; more rows sent than the window holds, so the tail overwrites the top.
- **Lines with step discontinuities** — stride mismatch; written rows straddle panel row boundaries.
- **Mirrored image** — MX/MY bits clear.

### Rejected: `fbtft`

Tried first. Bound fine, but roughly the first 1/4 of the framebuffer never refreshed — on both the `write()` and `mmap` paths, at 16 MHz as well as 32 MHz, and the dead region moved when `rotate` changed, so it tracked framebuffer coordinates rather than the glass. A dirty-tracking fault in a staging module. Abandoned rather than debugged; `panel-mipi-dbi` is mainline and better maintained.

### Planned — backlight pin move and SCD41 wiring (executed 2026-09-23, see below)

Prompted by the Pi 3 having only two 3.3V pins (1 and 17), both currently consumed by the display. **Not a real constraint:** pins 1 and 17 are two taps on the same regulator output, tied together on the PCB. Splitting one pin to several loads is electrically identical to using both. The limit is rail current (plan under ~500 mA for external loads), not pin count. A splice, terminal block or breakout gives as many 3.3V taps as needed.

Still worth freeing pin 17, because a hard-wired backlight can't be dimmed or blanked.

#### Current header occupancy

| Phys | GPIO | Used by |
|---|---|---|
| 1 | 3.3V | Display VCC — shared rail, more loads can hang off it |
| 7 | 4 | Relay W |
| 9 | GND | Display GND |
| 17 | 3.3V | Display LED — **to be freed** |
| 18 | 24 | Display DC |
| 19 | 10 | Display SDI (MOSI) |
| 21 | 9 | MISO — claimed by `dtparam=spi=on`, unused (`write-only`) |
| 22 | 25 | Display RESET |
| 23 | 11 | Display SCK (SCLK) |
| 24 | 8 | Display CS (CE0) |
| 26 | 7 | CE1 — claimed by `spi=on`, unused |
| 29 | 5 | Relay Y |
| 31 | 6 | Relay G |

Also off-limits: GPIO 14/15 (pins 8/10, UART), GPIO 0/1 (pins 27/28, HAT EEPROM probe).

#### Backlight → GPIO 12 (phys 32)

Pin 32 is free and is PWM0, so it supports either simple on/off or PWM dimming later. Chosen over GPIO 18 (phys 12) to keep GPIO 18 available, and over GPIO 13/19 for header position. GPIO 12 is in the 9–27 range, so it comes out of reset with an internal pull-**down** — unlike the relays, backlight-off at boot is the harmless state, so no pull override is needed in `config.txt`.

**Do not drive the LED pin from a GPIO directly.** These 2.4" modules pull roughly 60–100 mA through the backlight string, well over the 16 mA per-pin limit. A transistor is required.

Polarity needs a meter before the circuit is final: on the common TJCTM24024-style board the LED pin is the backlight **anode** (resistor → LED string → module GND), which means **high-side** switching — an N-channel low-side FET has nowhere to sit, because the LED cathodes are only reachable through the module's shared GND. Some variants instead put a transistor on-board and treat LED as a logic input.

- **Measure first:** continuity/diode from LED pin to GND with the module unpowered, and current into the LED pin at 3.3V.
- **If anode (expected):** logic-level P-channel FET (AO3401, DMG2301L) — source to 3.3V, drain to display LED, 10k gate pull-up to 3.3V so the backlight is off while the GPIO is hi-Z at boot. That circuit is active-**low**. Adding a small NPN/N-FET to invert the gate restores active-high, which is what `dtparam=backlight-gpio=` in the `mipi-dbi-spi` overlay expects.
- **If logic input:** wire GPIO 12 straight to it.

Using the overlay's `backlight-gpio` param hands blanking to DRM, which is a better fix for the `consoleblank=0` item below than disabling the blanker — the panel goes properly dark instead of displaying black. PWM dimming needs a `pwm` overlay plus `dtparam=audio=off` (analog audio shares PWM0/PWM1).

#### SCD41 → I2C1, phys 3 and 5

Both free. Nothing else on the bus yet; address is 0x62.

| SCD41 | Pi | Phys |
|---|---|---|
| VDD | 3.3V | 17 (shared with display VCC) |
| GND | GND | 6 |
| SDA | GPIO 2 | 3 |
| SCL | GPIO 3 | 5 |

- `dtparam=i2c_arm=on` in `config.txt`.
- **Leave the baudrate at the 100 kHz default** — the SCD41 does not support faster, and nothing else forces a raise.
- GPIO 2/3 have fixed 1.8k on-board pull-ups; the breakout carries its own. Paralleled they land lower than either alone but stay well inside spec at 100 kHz.
- Placement: `idea.md` requires the sensor a cable-length off the board so Pi self-heating doesn't poison a degree-level deadband. 100 kHz tolerates that easily — up to roughly a metre is uncontroversial.
- Current draw is ~15 mA average in periodic mode but spikes to ~205 mA during a measurement. Brief, and the breakout's decoupling absorbs most of it, but it belongs in the rail budget.

#### Revised current check

The pending `vcgencmd get_throttled` re-check should run with backlight **and** SCD41 both live, not backlight alone — the sensor's measurement spike is the larger transient of the two.

### Open

- `SDO` (MISO) unused. The `T_*` pins are an XPT2046 touch controller — open question whether touch replaces the two planned pushbuttons.
- SPI clock is currently `speed=8000000` in `config.txt`, but 2026-09-22 recorded 32 MHz as working. Lowered while chasing the blank panel; either restore 32 MHz or establish why 8 MHz is needed.
- Idle blanking still unverified. With `backlight-gpio` bound, DRM should power the backlight down properly instead of painting black, which would make `consoleblank=0` unnecessary — confirm rather than assume.

---

## 2026-09-23 — Backlight on GPIO 12; SCD41 reading

### Backlight moved off pin 17

Display LED now on **GPIO 12 (phys 32)** through a transistor; panel is **active-high**. Added under the `mipi-dbi-spi` block in `[all]`:

```
dtparam=backlight-gpio=12
```

Binds `/sys/class/backlight/backlight_gpio`. Blank/unblank via `bl_power` (`4` off, `0` on).

**`dtparam=` scoping is positional and cost a reboot cycle.** A `dtparam=` line applies to the *preceding* `dtoverlay=`. Placed in the top block alongside `dtparam=i2c_arm=on` and `dtparam=spi=on`, it is a **base** DTB param, `backlight-gpio` is not one, and it is silently ignored — no error, no log line. The panel stayed dark and GPIO 12 sat at its reset pull-down, identical to having made no change at all. It only takes effect below the `dtoverlay=mipi-dbi-spi` line. Verify param names with `dtoverlay -h mipi-dbi-spi`.

### SCD41 on I2C1

Reading: **CO2 587 ppm, 25.9 °C, 43.8 %RH.**

- `dtparam=i2c_arm=on` was already set, but `/dev/i2c-1` did not exist — the **`i2c-dev` module was not loaded**. `modprobe i2c-dev`, and `i2c-dev` appended to `/etc/modules` to persist. Installing `i2c-tools` does not pull this in.
- Board is a **bare breakout**: no regulator, no level shifters, `R1`/`R2` marked `103` (10k) as the only pull-ups, one decoupling cap. Pins labelled on both faces.
- **VCC = 3.3V.** Datasheet Table 4 permits 2.4–5.5V, so 5V would not damage it, but at 5V V_IH is 0.65 × 5 = 3.25V and the Pi only drives 3.3V — no margin. Stay at 3.3V.
- On-board 10k paralleled with the Pi's fixed 1.8k gives ~1.5k. Fine at 100 kHz.

### Misplaced GND — consumed most of the session

Ground was in the wrong header position (now **phys 20**). Symptom:

> `i2cdetect` reports `0x62` reliably, but **every** command NACKs — every command code, every sequence type, across cold power cycles, retry spacing and execution-time delays.

The sensor was parasitically powered through the SDA/SCL pull-ups and its own ESD protection diodes. That is enough for the I²C front end to decode its address and pull SDA low for the ACK, and nowhere near enough for the system controller to execute anything. Voltage measured at the *header* looked correct throughout, because the header was fine.

**Rule: an address ACK does not prove the device is powered.** It proves only that SDA, SCL and the pull-ups work. Address-ACK plus universal-NACK points at power or ground, not at protocol.

Time went into three theories that all turned out to be irrelevant, listed so they are not re-run:

- **Supply voltage** — suspected a regulator expecting 5V. Board has none; 3.3V was always correct.
- **Command legality** — datasheet Table 9 marks most commands as rejected while periodic measurement is running, so a sensor stuck in periodic mode would NACK `get_serial_number`. Probed with `get_data_ready_status` and `stop_periodic_measurement`, which *are* legal during measurement. Both NACKed, killing the theory.
- **Signal integrity / bus speed** — dropping to 10 kHz was considered and correctly skipped: the address ACK already exercises both lines end to end, so it cannot be a marginal-signalling problem.

### Current check — closed

`vcgencmd get_throttled` = **`0x0`** with backlight lit and SCD41 in periodic measurement. Confirms the 2026-09-21 finding holds with all loads live, including the sensor's 205 mA measurement spike. **Single Pi supply is sufficient; no separate 5V brick.**

### As-built header occupancy

Supersedes the planning table above.

| Phys | GPIO | Used by |
|---|---|---|
| 1 | 3.3V | Display VCC |
| 3 | 2 | SCD41 SDA (I2C1) |
| 5 | 3 | SCD41 SCL (I2C1) |
| 7 | 4 | Relay W |
| 9 | GND | Display GND |
| 17 | 3.3V | SCD41 VCC — same rail as phys 1 |
| 18 | 24 | Display DC |
| 19 | 10 | Display SDI (MOSI) |
| 20 | GND | **SCD41 GND** |
| 21 | 9 | MISO — claimed by `dtparam=spi=on`, unused (`write-only`) |
| 22 | 25 | Display RESET |
| 23 | 11 | Display SCK (SCLK) |
| 24 | 8 | Display CS (CE0) |
| 26 | 7 | CE1 — claimed by `spi=on`, unused |
| 29 | 5 | Relay Y |
| 31 | 6 | Relay G |
| 32 | 12 | Display backlight (PWM0, via transistor, active-high) |

Off-limits: GPIO 14/15 (phys 8/10, UART), GPIO 0/1 (phys 27/28, HAT EEPROM probe). Relay board power (5V phys 2, GND phys 6) was not recorded here; see 2026-09-24.

---

## 2026-09-24 — Touch not fitted; abandoned

Module is the **MSP2401 non-touch variant**. The `U1 / TOUCH` footprint on the PCB back (beside the T_IRQ/T_DO pins) is where the XPT2046 sits on the MSP2402; the header carries `T_CLK T_CS T_DIN T_DO T_IRQ` on both variants and the listing photo is shared. **Touch is abandoned for the final build** — physical control stays the two planned pushbuttons. `SDO` and all `T_*` pins are left unconnected.

Diagnostic trail, so none of it is re-run:

- Every `T_*` wire continuity-verified breadboard → pad. 3.3 V measured at the touch-side VCC pad. GPIO 7 owned by `spi0 CS1`, active-low, idles high. MOSI→MISO loopback on `/dev/spidev0.1` passed.
- X (`0xD0`), Y (`0x90`), VBAT (`0xA7`), TEMP (`0x87`) all read exactly 0 every sample; `T_IRQ` never dropped under hard press. IRQ is analog and SPI-independent — its silence plus a passing loopback leaves only an absent chip.
- Wrong turns: VBAT was first read with `0xA0` (differential mode, no such channel) — that 0 meant nothing. T_CLK/T_DIN were found unplugged mid-session after a grounding test; reconnected, no change. Display `SDO` was briefly tied to MISO alongside `T_DO`, then removed — these modules' SDO is not reliably tri-stated, so **never share MISO with it**; `write-only` stays.
- Listing parameter table says "ST7789V" — boilerplate, same as the title's "ST7735" (2026-09-21). Panel is ILI9341 by behaviour.

**The Python bench test disabled the production display path.** To expose CE0 as `/dev/spidev0.0`, `dtoverlay=nospi10` and `dtoverlay=mipi-dbi-spi,spi0-0,speed=8000000` were commented out of `config.txt`. The `dtparam=` lines beneath them (width/height, reset-gpio/dc-gpio, write-only, backlight-gpio) then bind to nothing, silently — same failure mode as 2026-09-23. The fb1 console has been dark since. **Restore both lines and reboot.**

Bench-only, not part of the build: `test_display.py`, `test_touch.py`, `test_spi_loopback.py`, and `~/thermostat-venv` on the Pi (`adafruit-circuitpython-rgb-display pillow spidev`; `lgpio` needs `apt install swig python3-dev liblgpio-dev` first — Bookworm's Python is externally managed, so venv, not `--break-system-packages`). If the Python path is ever needed again: `ILI9341(rotation=0, width=320, height=240)` matches MADCTL `0x40`; the library's default portrait geometry wraps at 240 px.

Resolves the 2026-09-22 "Open" item on `T_*`.

### Header occupancy as wired today

Adds the relay board's power pins, which no earlier table recorded.

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

Off-limits: GPIO 14/15 (phys 8/10, UART), GPIO 0/1 (phys 27/28, HAT EEPROM probe). Backlight transistor's own 3.3V/GND taps not recorded.

### Planned — regroup power/ground wires by device

Every signal pin is fixed (I2C, SPI0, relays on 4/5/6 for the reset-pull argument, backlight on PWM0). The four 3.3V/GND wires are the only movable ones, and they are the ones crossing: display power leaves from columns 1 and 5 to reach a display cluster in columns 9–12, and the SCD41's power does the reverse. Swap them. Pins 1/17 are one rail; all GNDs are common. No `config.txt` change, no reboot.

| Wire | From | To |
|---|---|---|
| Display VCC | 1 | 17 |
| Display GND | 9 | 20 |
| SCD41 VCC | 17 | 1 |
| SCD41 GND | 20 | 9 |

SCD41 GND goes to 9 rather than 6 because 6 is the relay board's. Result: SCD41 on 1-3-5-9, all inside row, columns 1–5; display on 17–24 contiguous (21 empty); relays on 2/6/7 plus 29/31.

#### Header after regrouping

| Phys | GPIO | Used by |
|---|---|---|
| 1 | 3.3V | SCD41 VCC |
| 2 | 5V | Relay board VCC |
| 3 | 2 | SCD41 SDA (I2C1) |
| 5 | 3 | SCD41 SCL (I2C1) |
| 6 | GND | Relay board GND |
| 7 | 4 | Relay W |
| 9 | GND | SCD41 GND |
| 17 | 3.3V | Display VCC — same rail as phys 1 |
| 18 | 24 | Display DC |
| 19 | 10 | Display SDI (MOSI) |
| 20 | GND | Display GND |
| 21 | 9 | MISO — claimed by `dtparam=spi=on`, unused (`write-only`) |
| 22 | 25 | Display RESET |
| 23 | 11 | Display SCK (SCLK) |
| 24 | 8 | Display CS (CE0) |
| 26 | 7 | CE1 — claimed by `spi=on`, unused |
| 29 | 5 | Relay Y |
| 31 | 6 | Relay G |
| 32 | 12 | Display backlight (PWM0, via transistor, active-high) |

Off-limits: GPIO 14/15 (phys 8/10, UART), GPIO 0/1 (phys 27/28, HAT EEPROM probe).

Display-side header order (`VCC GND CS RESET DC SDI SCK LED`) still can't be made monotonic against the Pi's fixed MOSI/SCLK/CE0 positions by moving DC/RESET, so those stay on 24/25.

---

## 2026-09-27 — `control.c`: libgpiod v2 relays; SCD41 read from C

### libgpiod

- Pi has **libgpiod v2**. First draft used the v1 API (`gpiod_chip_open_by_name`, `gpiod_chip_get_line`, `gpiod_line_request_output`, …) — none exist in v2; compile failed with implicit-declaration errors. Most online tutorials are v1.
- Ported to v2: `gpiod_chip_open("/dev/gpiochip0")` → line settings (output, `active_low`, initial `INACTIVE`) → line config → request config (consumer `thermostat`) → `gpiod_chip_request_lines`. `ACTIVE` = relay closed.
- Build: `gcc -Wall -o thermostat_control control.c -lgpiod`.
- **Not yet tested:** relay state after the process is killed (`kill -9` while energized). Released lines may hold their last value.

### SCD41 over `/dev/i2c-1`

- Raw i2c-dev, address `0x62`. On open: stop periodic (`0x3F86`), 500 ms, start periodic (`0x21B1`). Polls data-ready (`0xE4B8`, low 11 bits ≠ 0), then read measurement (`0xEC05`). CRC-8 (poly `0x31`, init `0xFF`) checked on every word.
- Readings arrive every ~5 s (sensor period), not per loop `sleep(1)`.

### Startup transient

- Every run starts at **~76 °F** and decays to **~69.2 °F** over **~3 min** (35–40 readings). RH rises in step (35 → 43 %). CO2 first 1–2 readings high (481, 456), then ~390–440.
- Initially read as the board cooling (moved from near the Pi, fan on it). **Ruled out:** a second run launched immediately after a settled first run jumped straight back to ~76.5 °F.
- Cause is the stop/start in `scd41_open`, not physics. Internal mechanism unknown — check datasheet for post-start stabilization.
- Settled noise ≈ **±0.05 °F**. Late drift 69.2 → 69.7 °F tracked rising CO2 — likely real room change.
- Absolute accuracy **not verified** — no reference thermometer yet.

### Open

- Don't restart the sensor if it's already measuring (it keeps running after the program exits). Confirm data-ready is accepted during periodic mode.
- After a real power-up, gate control on settled readings (~5 min, or until the rate of change is small).
- Calibrate against a reference thermometer; `set_temperature_offset` (default ≈ 4 °C already applied).
- `main` prints readings without checking `.valid`.

---

## 2026-09-28 — Software architecture decided (design only, nothing built)

Full rationale in [idea.md](idea.md) → Software. Decided:

- **JSON over HTTP everywhere.** Pi pulls satellites (`GET /reading`, ~60 s); Ubuntu web app calls a JSON API on the Pi. Supersedes idea.md's earlier "satellites post, HTTP vs MQTT TBD". MQTT and raw TCP rejected.
- **Fixed addresses via DHCP reservations** on the router, not static IPs on the devices.
- **Single-threaded event loop** blocking in `poll()` (sockets, GPIO edge events, `signalfd`); timeout = earliest deadline. Heating/cooling is a state, not a nested loop.
- **Settings file on the Pi** (setpoints + vacation), atomic write via `rename()`, Pi is sole writer.
- **History (nice-to-have):** Pi RAM ring buffer + event buffer with `?since=` backfill; Ubuntu stores in SQLite.

Open: fan logic based on the spread between zones (proposed); HTTP server library (Mongoose vs libmicrohttpd); settings file format; satellite filtering rules. `get_sensor_data()` blocks up to 6 s — must be reworked before it goes in the loop.

---

## 2026-09-30 — Display as a separate program (design only, nothing built)

Idea came ~1:30 p.m. in the Culver's drive-thru. Decided:

- **Display runs as its own program**, not inside the control loop. It's a client of the same JSON API as the web UI: GET for status, POST for setting changes.
- **Pushbuttons move to the display program**, so GPIO button events leave the control loop's `poll()` (2026-09-28 design).
- **Why:** a display hang or crash can't stall HVAC control. Display and web UI changes go through one entry point (`validate_setpoints()`), so neither bypasses validation or holds stale settings.
- Display talks to the controller over `localhost`; no new network exposure.

Open: GET polling interval (a few seconds); show "controller offline" when GET fails instead of stale values; two systemd units (control unit owns the watchdog).

---

## 2026-10-01 — Converted to C++; file split; `now_ms()`

### Relay code, as understood

- `relay_set(RELAY_HEAT, 1)` = heat on. `enum relay` in `relay.h` (`HEAT=0, COOL=1, FAN=2, NUM_RELAYS=3`) indexes the `relay_pins` / `relay_names` tables in `relay.cpp`.
- Pins BCM GPIO 4/5/6. Board is active-low; libgpiod inverts.
- Shell test: `gpioset`, **value 0 = on**.

### Monotonic time

- `#include <chrono>` is C++-only; failed to compile as C.
- Added `now_ms()` in main: `clock_gettime(CLOCK_MONOTONIC)` → `long long` ms. `int` ms overflows after ~24 days.

### C → C++

- `control`, `relay`, `net` → `.cpp`. `sensor.c` and the Sensirion vendor driver stay C.
- C++ has no implicit int → enum: relay-off loops use `static_cast<relay>(r)`.
- `extern "C"` guards added to `sensor.h` so C++ links against `sensor.c`.
- Structured bindings must name all four `sensor_result` fields: `auto [temp, hum, co2, valid]`.

### Build

- `compile.py` rewritten: `gcc` for C, `g++ -std=c++20` for C++, objects in `build/*.o`, link with `g++ … -lgpiod`, then runs `./thermostat_control`. **Builds.**

### File layout

- Old control file → `main.cpp` (entry point + main loop). New `control.cpp` for temperature decision logic; empty `control.h`. `main.cpp` added to `compile.py`.
- `all_off()` added to `relay.cpp` / `relay.h`: all relays off, pins stay claimed. `relays_close()` also releases them.

---

## 2026-10-02 — Decision logic started (not compiled)

### `control.cpp`

- `get_HVAC_command(main, office, bed, heat_set, ac_set, FAN_TRIGGER, FAN_CLEAR)` → `"cool"` if weighted temp > `ac_set`, `"heat"` if < `heat_set`.
- Weights: main **10** day / **8** night; office and bedroom **6** day / **4** night; **0** if invalid.
- `test_valid_temp()`: 50–110 °F and `valid == 1`.
- `get_weighted_temp()` combines the three readings.
- `hour_of_day()`: day when `tm_hour > 8`.

### `main.cpp`

- Loop reads the main sensor into a `sensor_result`. Decision call and relay switching not wired up. Setpoint/timing constants declared in `main()`, unused.

### Open (found by reading; `control.cpp` not compiled since these edits)

- `test_valid_temp`, `get_weighted_temp`, `hour_of_day` used before declaration and `control.h` is empty → won't compile. Declare in `control.h` or move above `get_HVAC_command`.
- `get_weighted_temp` accumulates into an uninitialized variable and never divides by the weight sum, so it isn't an average. All-invalid → weight sum 0.
- `get_HVAC_command` has no return when neither heat nor cool is needed (UB). `FAN_TRIGGER` / `FAN_CLEAR` unused.
- `hour_of_day()` counts every hour after 08:00 as day, including late evening.
- `control.cpp` includes `<chrono>` but uses `std::time` / `std::localtime` (`<ctime>`).
- `main.cpp` loop has a placeholder string where the decision call goes.
- `calibrate.c`: header comment says default target 425 ppm; `DEFAULT_TARGET_PPM` is 438.

### Next

- Fill in `control.h`; fix the weighted average and missing return; move decision constants from `main()` into `control.cpp`.
- `main.cpp`: fetch office/bedroom via `net_request`, call `get_HVAC_command`, act via `relay_set` / `all_off`.
- Implement the safety rules from `psuedo.txt`: min on/off times, heat/cool interlock, progress faults, emergency heat, fan circulation.
