"""Bench test for the ILI9341 display wiring. See log.md for pin mapping.

Run: source ~/thermostat-venv/bin/activate && python test_display.py
"""

import board
import digitalio
from PIL import Image, ImageDraw
from adafruit_rgb_display import ili9341

cs_pin = digitalio.DigitalInOut(board.CE0)      # phys 24 / GPIO8
dc_pin = digitalio.DigitalInOut(board.D24)      # phys 18 / GPIO24
reset_pin = digitalio.DigitalInOut(board.D25)   # phys 22 / GPIO25
backlight = digitalio.DigitalInOut(board.D12)   # phys 32 / GPIO12, active-high

backlight.direction = digitalio.Direction.OUTPUT
backlight.value = True

spi = board.SPI()  # SCK = phys 23 / GPIO11, MOSI = phys 19 / GPIO10

disp = ili9341.ILI9341(
    spi,
    rotation=0,
    width=320,
    height=240,
    cs=cs_pin,
    dc=dc_pin,
    rst=reset_pin,
    baudrate=24000000,
)

width, height = 320, 240

image = Image.new("RGB", (width, height))
draw = ImageDraw.Draw(image)

# Three color bars: wrong wiring on DC/RESET/CS tends to show as garbage
# or a blank screen rather than a clean, mirrored, or rotated pattern.
draw.rectangle((0, 0, width // 3, height), fill=(255, 0, 0))
draw.rectangle((width // 3, 0, 2 * width // 3, height), fill=(0, 255, 0))
draw.rectangle((2 * width // 3, 0, width, height), fill=(0, 0, 255))
draw.text((10, height // 2 - 10), "R  G  B", fill=(255, 255, 255))

disp.image(image)
print(f"Sent {width}x{height} test pattern. Check the panel.")
