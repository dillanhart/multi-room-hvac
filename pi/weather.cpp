#include "weather.h"

#include <algorithm>      // std::min
#include <ctime>          // localtime_r
#include <map>

season season_from_weather(const weather_data& w) {
    // local date (year*1000 + day of year) -> {low, hours seen}
    std::map<int, std::pair<float, int>> days;
    for (const auto& h : w.hourly) {
        std::time_t t = h.time;
        std::tm tm{};
        localtime_r(&t, &tm);
        int date = (tm.tm_year + 1900) * 1000 + tm.tm_yday;
        auto [it, inserted] = days.try_emplace(date, h.temp, 0);
        it->second.first = std::min(it->second.first, h.temp);
        it->second.second++;
    }

    // a partial day (the fetch started mid-morning) would miss the overnight low
    float sum = 0;
    int full_days = 0;
    for (const auto& [date, low_hours] : days) {
        if (low_hours.second < FULL_DAY_HOURS) continue;
        sum += low_hours.first;
        full_days++;
    }
    if (full_days == 0) return season::unknown;

    float avg_low = sum / full_days;
    if (avg_low < WINTER_AVG_LOW) return season::winter;
    if (avg_low > SUMMER_AVG_LOW) return season::summer;
    return season::shoulder;
}

hvac_mode resolve_mode(hvac_mode setting, season s) {
    if (setting != hvac_mode::auto_select) return setting;
    switch (s) {
        case season::winter:   return hvac_mode::heat;
        case season::summer:   return hvac_mode::cool;
        case season::shoulder: return hvac_mode::both;
        case season::unknown:  return hvac_mode::heat;
    }
    return hvac_mode::heat;   // unreachable; satisfies -Wreturn-type
}

std::optional<float> outside_temp_at(const weather_data& w, long long now_unix) {
    for (const auto& h : w.hourly)
        if (h.time <= now_unix && now_unix < h.time + 3600) return h.temp;
    return std::nullopt;
}

bool weather_due(const weather_data& w, long long now_unix) {
    return w.fetched_at == 0 || now_unix - w.fetched_at >= WEATHER_REFRESH;
}
