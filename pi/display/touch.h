/*
 * touch.h - the XPT2046-compatible resistive touch controller on SPI1, read from
 * userspace (/dev/spidev1.0) with T_IRQ (GPIO 16, low while pressed) as pen-down.
 * No kernel driver: the ads7846 overlay targets SPI0 (log.md, 2026-11-18).
 */

#ifndef TOUCH_H
#define TOUCH_H

#include "display_logic.h"   // screen_point

struct gpiod_chip;
struct gpiod_line_request;

class touch_panel {
public:
    bool open();    // false (and logged) if the SPI device or the IRQ line can't be opened
    void close();

    // one read, as LVGL polls it (every ~33 ms): true and the point while pressed.
    // A touch counts only once two filtered reads in a row agree, so the noisy
    // first contact never becomes a tap somewhere else.
    bool read(screen_point& point);

private:
    bool pen_down();
    int sample(unsigned char command);

    int spi = -1;
    gpiod_chip* chip = nullptr;
    gpiod_line_request* irq = nullptr;

    bool have_first = false;   // one good read since touch-down
    bool settled = false;      // two agreeing reads: reporting presses
    int first_x = 0, first_y = 0;
    screen_point last{};
};

#endif
