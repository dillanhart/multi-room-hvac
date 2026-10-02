"""SPI0/CE1 loopback sanity check — isolates the Pi's SPI stack from the
touch chip. Jumper phys 19 (MOSI) directly to phys 21 (MISO) on the
breadboard before running this; nothing else needs to be connected.

Run: source ~/thermostat-venv/bin/activate && python test_spi_loopback.py
"""

import spidev

spi = spidev.SpiDev()
spi.open(0, 1)  # bus 0, CE1 — same device the touch test uses
spi.max_speed_hz = 1000000
spi.mode = 0

test_bytes = [0x00, 0xFF, 0xA5, 0x5A, 0x12, 0x34]
for b in test_bytes:
    r = spi.xfer2([b])
    status = "OK" if r[0] == b else "MISMATCH"
    print(f"sent=0x{b:02X} received=0x{r[0]:02X}  {status}")

spi.close()
