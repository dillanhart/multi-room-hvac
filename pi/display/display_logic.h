/*
 * display_logic.h - the display program's decisions, with no LVGL, network or
 * hardware: set point steps, icon choice, API reply parsing, touch filtering.
 * Tested in tests/test_display_logic.cpp alongside the control program.
 */

#ifndef DISPLAY_LOGIC_H
#define DISPLAY_LOGIC_H

#include <optional>
#include <string>
#include <vector>
#include "constants.h"    // json, MIN_HEAT, MIN_DEAD_BAND, AC_MAX (shared with control)

// ---------------------------------------------------------------- what the API says

enum class display_status { idle, heating, em_heating, cooling, circulating, unknown };

// everything the set point, mode and night screens edit. Starts as a copy of the
// API's values; sent back only by the set point screen's Done.
struct settings_edit {
    float heat_point = 0;
    float ac_point = 0;
    float night_difference = 0;
    std::string mode = "auto";   // "auto" | "heat" | "cool" | "both"
    bool fan_on = false;

    bool operator==(const settings_edit&) const = default;
};

struct forecast_entry {
    long long time;          // start of the hour, Unix seconds
    float temp;              // F
    std::string condition;   // NWS icon code; "" = unknown
    bool is_day;

    bool operator==(const forecast_entry&) const = default;
};

// GET /status -> status and fan; nullopt if the reply isn't that shape
struct status_reply {
    display_status status;
    bool fan_on;
};
std::optional<status_reply> parse_status(const std::string& body);

// GET /readings -> the main sensor's temp; nullopt for no reading, an untrusted
// one, or a reply that isn't that shape (all shown as N/A)
std::optional<float> parse_main_temp(const std::string& body);

// GET /setpoints -> heat, AC, night difference, mode (fan_on left false: it's in /status)
std::optional<settings_edit> parse_setpoints(const std::string& body);

// GET /weather -> the hours in order; nullopt if the reply isn't that shape
std::optional<std::vector<forecast_entry>> parse_weather(const std::string& body);

display_status status_from_name(const std::string& name);

// ---------------------------------------------------------------- set point edits

// same rule as valid_set_points() in data_store.cpp (a test keeps them in step)
bool settings_valid(const settings_edit& s);

// one arrow tap: +1 or -1 degree. Heat moving into AC pushes AC (and the reverse),
// keeping MIN_DEAD_BAND, as PUT /setpoints would. A step that would break a limit
// (AC over AC_MAX, night heat too close to em heat, night difference below 0)
// changes nothing and returns false.
bool step_heat(settings_edit& s, int direction);
bool step_ac(settings_edit& s, int direction);
bool step_night(settings_edit& s, int direction);

// what Done sends: the PUT /setpoints body if any of its fields changed, the
// PUT /fan body if the fan changed; nullopt = nothing to send for that endpoint
std::optional<json> setpoints_request(const settings_edit& before, const settings_edit& after);
std::optional<json> fan_request(const settings_edit& before, const settings_edit& after);

// ---------------------------------------------------------------- text

// 71 -> "71", 70.5 -> "70.5" (set points can be fractional via the web UI)
std::string format_setpoint(float value);
// room and forecast temps: whole degrees, 71.4 -> "71"
std::string format_temp(float value);
// local time: "3 PM", "12 AM"
std::string format_hour(long long unix_time);

// ---------------------------------------------------------------- icons

// NWS icon code + day/night -> Meteocons icon name (a file in assets/icons/,
// without ".json"). Unknown or empty codes -> "not-available".
std::string icon_name(const std::string& condition, bool is_day);
// every name icon_name() can return: the files that must ship in assets/icons/
std::vector<std::string> all_icon_names();

// ---------------------------------------------------------------- touch

// raw XPT2046 readings at the panel's edges (log.md, 2026-11-18). The touch layer
// is portrait-native: screen x follows raw Y, screen y follows raw X.
struct touch_calibration {
    int raw_y_left, raw_y_right;    // raw Y at screen x = 0 and x = width - 1
    int raw_x_top, raw_x_bottom;    // raw X at screen y = 0 and y = height - 1
    int width, height;
};

struct screen_point { int x, y; };

// raw reading -> screen pixel, clamped to the screen
screen_point touch_to_screen(int raw_x, int raw_y, const touch_calibration& cal);

// one axis from a burst of raw samples: drops out-of-range values (the stray 4095 and
// near-0 reads at touch-down and lift), then the median, if enough samples are left
// and they agree within max_spread. nullopt = a bad burst, ignore it.
std::optional<int> filter_samples(std::vector<int> samples, int min_valid, int max_valid,
                                  size_t min_count, int max_spread);

#endif
