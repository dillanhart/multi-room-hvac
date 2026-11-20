#include "weather.h"

#include <algorithm>      // std::min
#include <cstdio>         // sscanf
#include <ctime>          // localtime_r, timegm
#include <iostream>
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

// local date as year * 1000 + day of year
static int local_date(long long unix_time) {
    std::time_t t = unix_time;
    std::tm tm{};
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 1000 + tm.tm_yday;
}

static int local_hour(long long unix_time) {
    std::time_t t = unix_time;
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm.tm_hour;
}

bool weather_fetch_due(const weather_data& w, long long now_unix, long long last_attempt) {
    if (last_attempt != 0 && now_unix - last_attempt < WEATHER_RETRY) return false;
    if (w.fetched_at == 0) return true;   // fresh install: don't wait for midnight
    return local_hour(now_unix) < WEATHER_FETCH_END_HOUR &&
           local_date(w.fetched_at) != local_date(now_unix);
}

std::optional<long long> parse_iso8601(const std::string& text) {
    std::tm tm{};
    char sign = 0;
    int off_h = 0, off_m = 0, used = 0;
    int got = std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c%2d:%2d%n",
                          &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                          &sign, &off_h, &off_m, &used);
    if (got != 9 || used != (int)text.size() || (sign != '+' && sign != '-')) return std::nullopt;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    // the fields are local to the given offset: read them as UTC, then remove the offset
    long long as_utc = timegm(&tm);
    long long offset = (off_h * 60 + off_m) * 60LL;
    return sign == '+' ? as_utc - offset : as_utc + offset;
}

// NWS gives "temperature": 41 with "temperatureUnit": "F" (or "C"); with the
// quantitative-value feature flag it's {"value": 5, "unitCode": "wmoUnit:degC"}
static std::optional<float> period_temp_f(const json& period) {
    const json& t = period.at("temperature");
    if (t.is_number()) {
        float value = t.get<float>();
        std::string unit = period.value("temperatureUnit", "F");
        return unit == "C" ? value * 9 / 5 + 32 : value;
    }
    if (t.is_object() && t.contains("value") && t["value"].is_number()) {
        float value = t["value"].get<float>();
        std::string unit = t.value("unitCode", "wmoUnit:degF");
        return unit == "wmoUnit:degC" ? value * 9 / 5 + 32 : value;
    }
    return std::nullopt;
}

std::optional<std::vector<hourly_temp>> parse_nws_hourly(const std::string& body) {
    json data = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (data.is_discarded()) return std::nullopt;
    try {
        const json& periods = data.at("properties").at("periods");
        if (!periods.is_array()) return std::nullopt;
        std::vector<hourly_temp> hours;
        for (const auto& p : periods) {
            std::optional<long long> start = parse_iso8601(p.at("startTime").get<std::string>());
            std::optional<float> temp = period_temp_f(p);
            if (start && temp) hours.push_back({*start, *temp});
        }
        if (hours.empty()) return std::nullopt;
        return hours;
    } catch (const json::exception& e) {
        std::cerr << "NWS reply: " << e.what() << "\n";
        return std::nullopt;
    }
}
