/*
 * relay.c - Heat/Cool/Fan relays on the Pi's GPIO pins
 *
 * Drives three relays through the Pi's GPIO pins using libgpiod v2.
 * Run as a user in the "gpio" group.
 */

#include <gpiod.h>          // libgpiod v2: talks to /dev/gpiochipN
#include <stdio.h>          // printf, perror

#include "relay.h"

// GPIO chip that owns the 40-pin header on the Pi 3
#define CHIP_PATH "/dev/gpiochip0"

// Relay pins use BCM GPIO numbers, not physical header pin numbers
#define PIN_NUM_H 4  // Heat relay (BCM GPIO4 = physical pin 7)
#define PIN_NUM_C 5  // Cool relay (BCM GPIO5 = physical pin 29)
#define PIN_NUM_F 6  // Fan relay  (BCM GPIO6 = physical pin 31)

// 1 = relay board is active-low (relay closes when the pin is driven LOW).
// With this set, libgpiod inverts the pin for us, so in the code below
// "ACTIVE" always means "relay on" regardless of the board's wiring.
#define RELAYS_ACTIVE_LOW 1

// Relay tables, indexed by enum relay
static const unsigned int relay_pins[NUM_RELAYS] = { PIN_NUM_H, PIN_NUM_C, PIN_NUM_F };
static const char *relay_names[NUM_RELAYS] = { "Heat", "Cool", "Fan" };

static struct gpiod_chip *chip;
static struct gpiod_line_request *request;
static int relay_state[NUM_RELAYS];  // 1 = on, matches what was last set

/*
 * request_relays - take control of all three relay pins as outputs.
 *
 * libgpiod v2 needs three config objects to request pins:
 *   1. line_settings  - how each pin behaves (direction, active-low, start value)
 *   2. line_config    - which pins get those settings
 *   3. request_config - who is asking (the "consumer" name shown in gpioinfo)
 * These are only needed while making the request, so they are freed before
 * returning. The returned request is what you use to actually switch the pins.
 *
 * chip: an open GPIO chip from gpiod_chip_open()
 * Returns: a line request on success, or NULL on failure (errno is set).
 *          Free it with gpiod_line_request_release() when done.
 */
static struct gpiod_line_request *request_relays(struct gpiod_chip *chip) {
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_request *request = NULL;

    // 1. Settings: output pins, inverted if the board is active-low
    settings = gpiod_line_settings_new();
    if (!settings)
        goto out;
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_active_low(settings, RELAYS_ACTIVE_LOW);
    // Start with every relay open (off) so nothing turns on at startup
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    // 2. Line config: apply those settings to all three relay pins
    line_cfg = gpiod_line_config_new();
    if (!line_cfg)
        goto out;
    if (gpiod_line_config_add_line_settings(line_cfg, relay_pins, NUM_RELAYS, settings))
        goto out;

    // 3. Request config: label the pins as ours ("thermostat" in gpioinfo)
    req_cfg = gpiod_request_config_new();
    if (!req_cfg)
        goto out;
    gpiod_request_config_set_consumer(req_cfg, "thermostat");

    // Ask the kernel for the pins. Fails if another program already holds them.
    request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

out:
    // Clean up the config objects (the *_free functions accept NULL safely)
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    return request;
}

/*
 * relays_init - open the GPIO chip and claim the relay pins.
 * Returns: 0 on success, -1 on failure (error is printed).
 */
int relays_init(void) {
    // Open the GPIO controller for the 40-pin header
    chip = gpiod_chip_open(CHIP_PATH);
    if (!chip) {
        perror("gpiod_chip_open " CHIP_PATH);
        return -1;
    }

    // Claim the three relay pins as outputs (all relays start off)
    request = request_relays(chip);
    if (!request) {
        perror("gpiod_chip_request_lines");
        gpiod_chip_close(chip);
        chip = NULL;
        return -1;
    }
    return 0;
}

/*
 * relay_set - turn one relay on (closed) or off (open).
 *
 * r:       which relay: RELAY_HEAT, RELAY_COOL or RELAY_FAN
 * on:      1 = turn the relay on, 0 = turn it off
 * Returns: 0 on success, -1 on failure (error is printed),
 *          RELAY_INTERLOCK if Heat and Cool would both be on (nothing is changed).
 *
 * Interlock: Heat and Cool are never on together. Turning one on while the
 * other is on is refused here, so no caller can bypass it. A refusal means the
 * caller failed to turn the other relay off first (a logic bug, not a normal
 * "not yet"), so it is reported on stderr and the caller must check for it.
 *
 * ACTIVE/INACTIVE are logical values; the active-low setting from
 * request_relays() handles whether that means a HIGH or LOW pin.
 */
int relay_set(enum relay r, int on) {
    enum gpiod_line_value value = on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;

    if (!request || r < 0 || r >= NUM_RELAYS)
        return -1;

    if (on && ((r == RELAY_HEAT && relay_state[RELAY_COOL]) ||
               (r == RELAY_COOL && relay_state[RELAY_HEAT]))) {
        fprintf(stderr, "INTERLOCK VIOLATION: refused %s on, %s is still on\n",
                relay_names[r], relay_names[r == RELAY_HEAT ? RELAY_COOL : RELAY_HEAT]);
        return RELAY_INTERLOCK;
    }
    if (gpiod_line_request_set_value(request, relay_pins[r], value)) {
        perror("gpiod_line_request_set_value");
        return -1;
    }
    relay_state[r] = on ? 1 : 0;
    printf("%s relay (GPIO%u) %s\n", relay_names[r], relay_pins[r],
           on ? "CLOSED (on)" : "OPEN (off)");
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

/*
 * relays_close - turn everything off and give the pins back to the kernel.
 */
void relays_close(void) {
    if (request) {
        for (int r = 0; r < NUM_RELAYS; r++)
            relay_set(static_cast<relay>(r), 0);
        gpiod_line_request_release(request);
        request = NULL;
    }
    if (chip) {
        gpiod_chip_close(chip);
        chip = NULL;
    }
}

void all_off(void){
    for (int r = 0; r < NUM_RELAYS; r++)relay_set(static_cast<relay>(r),0);
}
