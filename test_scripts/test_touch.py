"""Bench test for the XPT2046 touch controller wiring. See log.md for pin mapping.

T_CLK/T_DIN/T_DO share the display's SPI0 bus (SCK/MOSI/MISO);
T_CS is on CE1, T_IRQ is a dedicated GPIO.

Uses raw spidev rather than a touch driver library, so it only exercises
the wiring, not any higher-level calibration/driver logic.

Run: source ~/thermostat-venv/bin/activate && python test_touch.py
Press Ctrl+C to stop.
"""

import time

import board
import digitalio
import spidev

IRQ_PIN = board.D17  # phys 11 / GPIO17, active-low on touch

X_CHANNEL = 0xD0
Y_CHANNEL = 0x90
# Single-ended, internal reference on (SER/DFR=1, PD=11) — a live chip returns
# stable non-zero values here whether or not the panel is touched.
VBAT_CHANNEL = 0xA7
TEMP_CHANNEL = 0x87

irq = digitalio.DigitalInOut(IRQ_PIN)
irq.direction = digitalio.Direction.INPUT
irq.pull = digitalio.Pull.UP

spi = spidev.SpiDev()
spi.open(0, 1)  # bus 0, CE1 (T_CS)
spi.max_speed_hz = 1000000
spi.mode = 0


def read_channel(channel):
    r = spi.xfer2([channel, 0, 0])
    return ((r[1] << 8) | r[2]) >> 3


POLL_ONLY = True  # bypass IRQ gating, read channels continuously regardless

print("Polling touch channels directly, IRQ ignored (Ctrl+C to stop)...")
print("Press and release the panel repeatedly and watch for x/y to change.")
try:
    while True:
        if POLL_ONLY or not irq.value:
            x = read_channel(X_CHANNEL)
            y = read_channel(Y_CHANNEL)
            vbat = read_channel(VBAT_CHANNEL)
            temp = read_channel(TEMP_CHANNEL)
            print(f"x={x} y={y} vbat={vbat} temp={temp} irq={irq.value}")
        time.sleep(0.2)
except KeyboardInterrupt:
    pass
finally:
    spi.close()
