#include "display_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

// ---------------------------------------------------------------- API replies

static std::optional<json> parse(const std::string& body) {
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

display_status status_from_name(const std::string& name) {
    if (name == "idle") return display_status::idle;
    if (name == "heating") return display_status::heating;
    if (name == "em_heating") return display_status::em_heating;
    if (name == "cooling") return display_status::cooling;
    if (name == "circulating") return display_status::circulating;
    return display_status::unknown;
}

std::optional<status_reply> parse_status(const std::string& body) {
    std::optional<json> j = parse(body);
    if (!j) return std::nullopt;
    try {
        return status_reply{status_from_name(j->at("status").get<std::string>()),
                            j->at("fan").get<std::string>() == "on"};
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<float> parse_main_temp(const std::string& body) {
    std::optional<json> j = parse(body);
    if (!j || !j->contains("main")) return std::nullopt;
    const json& m = (*j)["main"];
    if (!m.is_object() || !m.contains("temp") || !m["temp"].is_number()) return std::nullopt;
    if (!m.value("trusted", false)) return std::nullopt;   // control isn't using it either
    return m["temp"].get<float>();
}

std::optional<settings_edit> parse_setpoints(const std::string& body) {
    std::optional<json> j = parse(body);
    if (!j) return std::nullopt;
    try {
        settings_edit s;
        s.heat_point = j->at("heat_point").get<float>();
        s.ac_point = j->at("ac_point").get<float>();
        s.night_difference = j->at("night_difference").get<float>();
        s.mode = j->at("mode").get<std::string>();
        return s;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::vector<forecast_entry>> parse_weather(const std::string& body) {
    std::optional<json> j = parse(body);
    if (!j || !j->contains("hours") || !(*j)["hours"].is_array()) return std::nullopt;
    std::vector<forecast_entry> hours;
    for (const auto& h : (*j)["hours"]) {
        try {
            hours.push_back({h.at("time").get<long long>(), h.at("temp").get<float>(),
                             h.value("condition", ""), h.value("is_day", true)});
        } catch (const json::exception&) {
            // skip a bad hour, keep the rest
        }
    }
    return hours;
}

// ---------------------------------------------------------------- set point edits

bool settings_valid(const settings_edit& s) {
    return s.heat_point - s.night_difference > MIN_HEAT + MIN_DEAD_BAND
        && s.heat_point + MIN_DEAD_BAND <= s.ac_point
        && s.ac_point <= AC_MAX
        && s.night_difference >= 0;
}

// apply the step to a copy; keep it only if the result is valid
static bool try_step(settings_edit& s, settings_edit next) {
    if (!settings_valid(next)) return false;
    s = next;
    return true;
}

bool step_heat(settings_edit& s, int direction) {
    settings_edit next = s;
    next.heat_point += direction;
    if (next.heat_point + MIN_DEAD_BAND > next.ac_point) next.ac_point = next.heat_point + MIN_DEAD_BAND;
    return try_step(s, next);
}

bool step_ac(settings_edit& s, int direction) {
    settings_edit next = s;
    next.ac_point += direction;
    if (next.ac_point - MIN_DEAD_BAND < next.heat_point) next.heat_point = next.ac_point - MIN_DEAD_BAND;
    return try_step(s, next);
}

bool step_night(settings_edit& s, int direction) {
    settings_edit next = s;
    next.night_difference += direction;
    return try_step(s, next);
}

std::optional<json> setpoints_request(const settings_edit& before, const settings_edit& after) {
    if (before.heat_point == after.heat_point && before.ac_point == after.ac_point &&
        before.night_difference == after.night_difference && before.mode == after.mode)
        return std::nullopt;
    // all four, so the control program applies no push of its own: the display already did
    return json{{"heat_point", after.heat_point}, {"ac_point", after.ac_point},
                {"night_difference", after.night_difference}, {"mode", after.mode}};
}

std::optional<json> fan_request(const settings_edit& before, const settings_edit& after) {
    if (before.fan_on == after.fan_on) return std::nullopt;
    return json{{"fan", after.fan_on ? "on" : "auto"}};
}

// ---------------------------------------------------------------- text

std::string format_setpoint(float value) {
    char text[16];
    if (std::fabs(value - std::round(value)) < 0.05f)
        std::snprintf(text, sizeof text, "%.0f", std::round(value));
    else
        std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

std::string format_temp(float value) {
    char text[16];
    std::snprintf(text, sizeof text, "%.0f", std::round(value));
    return text;
}

std::string format_hour(long long unix_time) {
    std::time_t t = unix_time;
    std::tm tm{};
    localtime_r(&t, &tm);
    int hour12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
    return std::to_string(hour12) + (tm.tm_hour < 12 ? " AM" : " PM");
}

// ---------------------------------------------------------------- icons

// NWS codes: https://api.weather.gov/icons. "-dn" = add "-day" or "-night".
static const std::map<std::string, std::string> ICONS = {
    {"skc", "clear-dn"},              {"few", "mostly-clear-dn"},
    {"sct", "partly-cloudy-dn"},      {"bkn", "overcast-dn"},
    {"ovc", "overcast"},
    {"wind_skc", "wind"}, {"wind_few", "wind"}, {"wind_sct", "wind"},
    {"wind_bkn", "wind"}, {"wind_ovc", "wind"},
    {"rain", "rain"},                 {"rain_showers", "partly-cloudy-dn-rain"},
    {"rain_showers_hi", "partly-cloudy-dn-rain"},
    {"tsra", "thunderstorms-rain"},   {"tsra_sct", "thunderstorms-dn-rain"},
    {"tsra_hi", "thunderstorms-dn-rain"},
    {"snow", "snow"},                 {"blizzard", "wind-snow"},
    {"rain_snow", "sleet"}, {"rain_sleet", "sleet"}, {"snow_sleet", "sleet"}, {"sleet", "sleet"},
    {"fzra", "sleet"}, {"rain_fzra", "sleet"}, {"snow_fzra", "sleet"},
    {"fog", "fog-dn"},                {"haze", "haze-dn"},
    {"smoke", "smoke"},               {"dust", "dust-dn"},
    {"tornado", "tornado"},           {"hurricane", "hurricane"},
    {"tropical_storm", "hurricane"},
    {"hot", "thermometer-warmer"},    {"cold", "thermometer-colder"},
};

static std::string fill_day_night(std::string name, bool is_day) {
    size_t at = name.find("-dn");
    if (at != std::string::npos) name.replace(at, 3, is_day ? "-day" : "-night");
    return name;
}

std::string icon_name(const std::string& condition, bool is_day) {
    auto it = ICONS.find(condition);
    return it == ICONS.end() ? "not-available" : fill_day_night(it->second, is_day);
}

std::vector<std::string> all_icon_names() {
    std::vector<std::string> names = {"not-available"};
    for (const auto& [code, name] : ICONS)
        for (bool day : {true, false}) names.push_back(fill_day_night(name, day));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// ---------------------------------------------------------------- touch

static int map_axis(int raw, int raw_at_0, int raw_at_max, int size) {
    double pos = double(raw - raw_at_0) / (raw_at_max - raw_at_0) * (size - 1);
    return std::clamp(int(std::lround(pos)), 0, size - 1);
}

screen_point touch_to_screen(int raw_x, int raw_y, const touch_calibration& cal) {
    return {map_axis(raw_y, cal.raw_y_left, cal.raw_y_right, cal.width),
            map_axis(raw_x, cal.raw_x_top, cal.raw_x_bottom, cal.height)};
}

std::optional<int> filter_samples(std::vector<int> samples, int min_valid, int max_valid,
                                  size_t min_count, int max_spread) {
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [&](int v) { return v < min_valid || v > max_valid; }),
                  samples.end());
    if (samples.size() < min_count) return std::nullopt;
    std::sort(samples.begin(), samples.end());
    // spread of the middle half: one wild sample left after the range check doesn't spoil it
    size_t quarter = samples.size() / 4;
    if (samples[samples.size() - 1 - quarter] - samples[quarter] > max_spread) return std::nullopt;
    return samples[samples.size() / 2];
}
