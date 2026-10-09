/*
 * fake_relay.cpp - stand-in for pi/relay.cpp in tests
 *
 * Implements relay.h without GPIO, so control.cpp links on any machine.
 * The linker picks this file instead of relay.cpp (a "link seam"): the code
 * under test still calls relay_set() / all_off(), it just lands here.
 *
 * Tests use fake_relay.h to reset the state, read it, and force failures.
 */

#include "relay.h"
#include "fake_relay.h"

static const char *relay_names[NUM_RELAYS] = { "Heat", "Cool", "Fan" };
static int relay_state[NUM_RELAYS];
static bool fail_next_set = false;

void fake_relay_reset(void) {
    for (int r = 0; r < NUM_RELAYS; r++)
        relay_state[r] = 0;
    fail_next_set = false;
}

void fake_relay_fail_next(void) {
    fail_next_set = true;
}

int relays_init(void) {
    fake_relay_reset();
    return 0;
}

// same interlock rule as the real relay_set()
int relay_set(enum relay r, int on) {
    if (r < 0 || r >= NUM_RELAYS)
        return -1;
    if (fail_next_set) {
        fail_next_set = false;
        return -1;
    }
    if (on && ((r == RELAY_HEAT && relay_state[RELAY_COOL]) ||
               (r == RELAY_COOL && relay_state[RELAY_HEAT])))
        return RELAY_INTERLOCK;
    relay_state[r] = on ? 1 : 0;
    return 0;
}

int relay_get(enum relay r) {
    if (r < 0 || r >= NUM_RELAYS)
        return 0;
    return relay_state[r];
}

const char *relay_name(enum relay r) {
    if (r < 0 || r >= NUM_RELAYS)
        return "?";
    return relay_names[r];
}

void relays_close(void) {
    fake_relay_reset();
}

int all_off(void) {
    int result = 0;
    for (int r = 0; r < NUM_RELAYS; r++) {
        if (relay_set(static_cast<relay>(r), 0) != 0)
            result = -1;
    }
    return result;
}
