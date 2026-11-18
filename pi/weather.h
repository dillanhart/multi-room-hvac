/*
 * weather.h - what the stored hourly forecast means for control:
 * the season (which equipment auto mode allows) and the outside temp right now.
 * Pure functions: the fetch and weather.json live elsewhere (data_store.cpp).
 */

#ifndef WEATHER_H
#define WEATHER_H

#include <optional>
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

// true once a day has passed since the last successful fetch (or never fetched)
bool weather_due(const weather_data& w, long long now_unix);

#endif
