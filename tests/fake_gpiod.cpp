/*
 * fake_gpiod.cpp - stand-in for libgpiod in tests
 *
 * Implements only the libgpiod v2 calls relay.cpp makes. The structs gpiod.h
 * leaves opaque are defined here, holding just what the fake needs.
 */

#include <errno.h>
#include <gpiod.h>
#include "fake_gpiod.h"

constexpr unsigned int MAX_LINES = 64;

struct gpiod_chip { int unused; };
struct gpiod_line_settings {
    enum gpiod_line_direction direction;
    bool active_low;
    enum gpiod_line_value value;
};
// settings are copied onto the lines straight away, so these hold nothing
struct gpiod_line_config { int unused; };
struct gpiod_request_config { int unused; };
struct gpiod_line_request { int unused; };

static fake_line lines[MAX_LINES];
static gpiod_chip chip;
static gpiod_line_request request;
static bool chip_is_open = false;
static bool request_is_held = false;
static bool fail_chip_open = false;
static int fail_set_offset = -1;

void fake_gpiod_reset(void) {
    for (auto& line : lines)
        line = fake_line{};
    chip_is_open = false;
    request_is_held = false;
    fail_chip_open = false;
    fail_set_offset = -1;
}

const fake_line& fake_gpiod_line(unsigned int offset) { return lines[offset]; }
bool fake_gpiod_chip_open(void) { return chip_is_open; }
bool fake_gpiod_request_held(void) { return request_is_held; }
void fake_gpiod_fail_chip_open(void) { fail_chip_open = true; }
void fake_gpiod_fail_next_set(unsigned int offset) { fail_set_offset = (int)offset; }

// ---- libgpiod API ----

struct gpiod_chip *gpiod_chip_open(const char *) {
    if (fail_chip_open) {
        fail_chip_open = false;
        errno = ENOENT;  // like the real call, so relay.cpp's perror() makes sense
        return nullptr;
    }
    chip_is_open = true;
    return &chip;
}

void gpiod_chip_close(struct gpiod_chip *) { chip_is_open = false; }

struct gpiod_line_settings *gpiod_line_settings_new(void) {
    return new gpiod_line_settings{ GPIOD_LINE_DIRECTION_AS_IS, false, GPIOD_LINE_VALUE_INACTIVE };
}

void gpiod_line_settings_free(struct gpiod_line_settings *settings) { delete settings; }

int gpiod_line_settings_set_direction(struct gpiod_line_settings *settings, enum gpiod_line_direction direction) {
    settings->direction = direction;
    return 0;
}

void gpiod_line_settings_set_active_low(struct gpiod_line_settings *settings, bool active_low) {
    settings->active_low = active_low;
}

int gpiod_line_settings_set_output_value(struct gpiod_line_settings *settings, enum gpiod_line_value value) {
    settings->value = value;
    return 0;
}

struct gpiod_line_config *gpiod_line_config_new(void) { return new gpiod_line_config{}; }
void gpiod_line_config_free(struct gpiod_line_config *config) { delete config; }

int gpiod_line_config_add_line_settings(struct gpiod_line_config *, const unsigned int *offsets,
                                        size_t num_offsets, struct gpiod_line_settings *settings) {
    for (size_t i = 0; i < num_offsets; i++) {
        if (offsets[i] >= MAX_LINES)
            return -1;
        fake_line& line = lines[offsets[i]];
        line.direction = settings->direction;
        line.active_low = settings->active_low;
        line.value = settings->value;
        line.requested = true;  // simplification: claimed when configured, not at request time
    }
    return 0;
}

struct gpiod_request_config *gpiod_request_config_new(void) { return new gpiod_request_config{}; }
void gpiod_request_config_free(struct gpiod_request_config *config) { delete config; }
void gpiod_request_config_set_consumer(struct gpiod_request_config *, const char *) {}

struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *, struct gpiod_request_config *,
                                                    struct gpiod_line_config *) {
    request_is_held = true;
    return &request;
}

void gpiod_line_request_release(struct gpiod_line_request *) {
    request_is_held = false;
    for (auto& line : lines)
        line.requested = false;
}

int gpiod_line_request_set_value(struct gpiod_line_request *, unsigned int offset, enum gpiod_line_value value) {
    if (offset >= MAX_LINES || !lines[offset].requested)
        return -1;
    if ((int)offset == fail_set_offset) {
        fail_set_offset = -1;
        return -1;
    }
    lines[offset].value = value;
    lines[offset].writes++;
    return 0;
}
