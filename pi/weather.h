/*
 * weather.h - what the stored hourly forecast means for control:
 * the season (which equipment auto mode allows) and the outside temp right now.
 * Pure functions: the HTTPS fetch is in net.cpp, weather.json in data_store.cpp.
 */

#ifndef WEATHER_H
#define WEATHER_H

#include <optional>
#include <string>
#include <vector>
#include "constants.h"    // weather_data, hvac_mode, thresholds

enum class season {
    winter,     // average low < WINTER_AVG_LOW: heating only
    shoulder,   // in between: both, the weather gate decides
    summer,     // average low > SUMMER_AVG_LOW: cooling only
    unknown,    // no full day of data
};

// average daily low over the full days (FULL_DAY_HOURS+) in the data, local dates.
// No expiry: stale data keeps its season until new data replaces it.
season season_from_weather(const weather_data& w);

// what may run: a manual mode as-is; auto from the season.
// Unknown season in auto allows heat only: AC in winter is the failure to avoid.
// Never returns auto_select.
hvac_mode resolve_mode(hvac_mode setting, season s);

// forecast temp for the hour containing now; nullopt if not covered (gate skipped)
std::optional<float> outside_temp_at(const weather_data& w, long long now_unix);

// time to try a fetch: at least WEATHER_RETRY since the last try, and either never
// fetched, or between midnight and WEATHER_FETCH_END_HOUR with no success yet today
// (local dates). last_attempt = 0 means no try yet.
bool weather_fetch_due(const weather_data& w, long long now_unix, long long last_attempt);

// the hours from an NWS /gridpoints/.../forecast/hourly reply: temp in F, plus the
// icon condition, isDaytime and shortForecast (each optional: a missing one leaves
// the default). nullopt if the reply isn't that shape or has no usable hours.
std::optional<std::vector<forecast_hour>> parse_nws_hourly(const std::string& body);

// the condition code from an NWS icon URL: ".../icons/land/night/rain_showers,20/tsra,40?size=small"
// -> "rain_showers" (the first, when the hour changes partway). "" if there isn't one.
std::string nws_icon_condition(const std::string& url);

// "2026-11-19T14:00:00-07:00" -> Unix seconds; nullopt if it isn't that format
std::optional<long long> parse_iso8601(const std::string& text);

#endif
