/*
 * fake_relay.h - test controls for fake_relay.cpp
 *
 * Read relay states with relay_get() from relay.h.
 */

#ifndef FAKE_RELAY_H
#define FAKE_RELAY_H

// all relays off, no pending failure; call at the start of each test
void fake_relay_reset(void);

// the next relay_set() returns -1 and changes nothing
void fake_relay_fail_next(void);

#endif
