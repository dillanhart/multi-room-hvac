"""Bench test for the touch controller (XPT2046-compatible) on SPI1.
See log.md (2026-11-18) for the pin mapping.

T_CLK/T_DIN/T_DO/T_CS are on SPI1 (GPIO 21/20/19/18), separate from the
display on SPI0. T_IRQ is GPIO16, active-low, pulled up by config.txt.
Needs `dtoverlay=spi1-1cs` (gives /dev/spidev1.0) and python3-spidev.

Raw reads only: no filtering or calibration. Prints X/Y/pressure while
pressed, plus the min/max seen, so pressing the four corners gives the
calibration range.

Run: python3 test_touch.py   (Ctrl+C to stop)
"""

import subprocess
import time

import spidev

IRQ_GPIO = 16

X_CHANNEL = 0xD0   # 12-bit, differential; power-down between reads keeps IRQ enabled
Y_CHANNEL = 0x90
Z1_CHANNEL = 0xB0  # pressure
Z2_CHANNEL = 0xC0

spi = spidev.SpiDev()
spi.open(1, 0)                # bus 1, CE0 = GPIO18 (T_CS)
spi.max_speed_hz = 1_000_000  # chip max is ~2 MHz
spi.mode = 0


def read(cmd):
    r = spi.xfer2([cmd, 0, 0])
    return ((r[1] << 8) | r[2]) >> 3  # 12-bit result


def pressed():
    out = subprocess.run(["pinctrl", "get", str(IRQ_GPIO)],
                         capture_output=True, text=True).stdout
    return "| lo" in out


lo = [4095, 4095]
hi = [0, 0]
was_pressed = False
try:
    while True:
        if pressed():
            x, y = read(X_CHANNEL), read(Y_CHANNEL)
            z1, z2 = read(Z1_CHANNEL), read(Z2_CHANNEL)
            lo = [min(lo[0], x), min(lo[1], y)]
            hi = [max(hi[0], x), max(hi[1], y)]
            print(f"x={x:4} y={y:4} z1={z1:4} z2={z2:4}   "
                  f"range x {lo[0]}-{hi[0]}  y {lo[1]}-{hi[1]}")
            was_pressed = True
        elif was_pressed:
            print("released")
            was_pressed = False
        time.sleep(0.1)
except KeyboardInterrupt:
    pass
finally:
    spi.close()
