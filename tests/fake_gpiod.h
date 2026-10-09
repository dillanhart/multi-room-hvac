/*
 * fake_gpiod.h - test controls for fake_gpiod.cpp
 *
 * fake_gpiod.cpp stands in for libgpiod, one level below relay.cpp, so the
 * real relay.cpp (and its interlock) runs in tests without a GPIO chip.
 */

#ifndef FAKE_GPIOD_H
#define FAKE_GPIOD_H

#include <gpiod.h>

// what the fake remembers about one GPIO line (indexed by BCM number)
struct fake_line {
    bool requested = false;     // claimed by gpiod_chip_request_lines
    enum gpiod_line_direction direction = GPIOD_LINE_DIRECTION_AS_IS;
    bool active_low = false;
    enum gpiod_line_value value = GPIOD_LINE_VALUE_INACTIVE;  // logical value last written
    int writes = 0;             // successful gpiod_line_request_set_value calls
};

// forget everything: no lines, chip closed, no pending failures
void fake_gpiod_reset(void);

const fake_line& fake_gpiod_line(unsigned int offset);

bool fake_gpiod_chip_open(void);       // chip opened and not closed
bool fake_gpiod_request_held(void);    // lines requested and not released

void fake_gpiod_fail_chip_open(void);                  // next gpiod_chip_open returns NULL
void fake_gpiod_fail_next_set(unsigned int offset);    // next write to this line returns -1

#endif
