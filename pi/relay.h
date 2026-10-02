/*
 * relay.h - Heat/Cool/Fan relays on the Pi's GPIO pins
 *
 * Call relays_init() once, then relay_set() to switch relays on and off.
 * The GPIO chip and pin handles stay inside relay.c.
 */

#ifndef RELAY_H
#define RELAY_H

enum relay {
    RELAY_HEAT,
    RELAY_COOL,
    RELAY_FAN,
    NUM_RELAYS
};

// Claim the relay pins as outputs (all relays start off).
// Returns 0 on success, -1 on failure (error is printed).
int relays_init(void);

// Turn one relay on (1) or off (0). Returns 0 on success, -1 on failure.
int relay_set(enum relay r, int on);

// Last value set with relay_set(): 1 = on, 0 = off.
int relay_get(enum relay r);

// Name of a relay ("Heat", "Cool", "Fan") for printing.
const char *relay_name(enum relay r);

// Turn every relay off and release the pins.
void relays_close(void);

void all_off(void);

#endif
