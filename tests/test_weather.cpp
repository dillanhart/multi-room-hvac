#include <gtest/gtest.h>
#include "weather.h"

#include <cstdlib>        // setenv
#include <ctime>          // tzset, mktime

// Local dates matter for daily lows, so pin the time zone.
class Weather : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("TZ", "America/Chicago", 1);
        tzset();
    }

    // local midnight of a date in Jan 2027 (no DST in the way)
    static long long midnight(int day) {
        std::tm tm{};
        tm.tm_year = 2027 - 1900;
        tm.tm_mon = 0;
        tm.tm_mday = day;
        tm.tm_isdst = -1;
        return std::mktime(&tm);
    }

    // `days` full local days from Jan 1, each with the given low at 5 am and low + 15 otherwise
    static weather_data week(std::vector<float> lows, int first_hour = 0) {
        weather_data w;
        for (size_t d = 0; d < lows.size(); d++)
            for (int h = (d == 0 ? first_hour : 0); h < 24; h++)
                w.hourly.push_back({midnight(1 + d) + h * 3600LL, h == 5 ? lows[d] : lows[d] + 15});
        return w;
    }
};

// ---- season ----

TEST_F(Weather, NoDataIsUnknown) {
    EXPECT_EQ(season_from_weather({}), season::unknown);
}

TEST_F(Weather, SeasonFromAverageLow) {
    EXPECT_EQ(season_from_weather(week({40, 40, 40, 40, 40, 40, 40})), season::winter);
    EXPECT_EQ(season_from_weather(week({60, 60, 60, 60, 60, 60, 60})), season::shoulder);
    EXPECT_EQ(season_from_weather(week({70, 70, 70, 70, 70, 70, 70})), season::summer);
}

TEST_F(Weather, ThresholdsAreExclusive) {
    EXPECT_EQ(season_from_weather(week({WINTER_AVG_LOW})), season::shoulder);
    EXPECT_EQ(season_from_weather(week({WINTER_AVG_LOW - 0.5f})), season::winter);
    EXPECT_EQ(season_from_weather(week({SUMMER_AVG_LOW})), season::shoulder);
    EXPECT_EQ(season_from_weather(week({SUMMER_AVG_LOW + 0.5f})), season::summer);
}

TEST_F(Weather, OneColdNightDoesNotFlipIt) {
    // average (30 + 6 * 62) / 7 = 57.4: still shoulder
    EXPECT_EQ(season_from_weather(week({30, 62, 62, 62, 62, 62, 62})), season::shoulder);
}

TEST_F(Weather, PartialDayLeftOut) {
    // first day starts at 10 am: its 5 am low (20) is missing, so it doesn't count at all
    weather_data w = week({20, 70, 70}, 10);
    EXPECT_EQ(season_from_weather(w), season::summer);
    // only a partial day: unknown
    EXPECT_EQ(season_from_weather(week({40}, 10)), season::unknown);
}

// ---- resolve_mode ----

TEST(ResolveMode, ManualWins) {
    EXPECT_EQ(resolve_mode(hvac_mode::cool, season::winter), hvac_mode::cool);
    EXPECT_EQ(resolve_mode(hvac_mode::both, season::unknown), hvac_mode::both);
    EXPECT_EQ(resolve_mode(hvac_mode::heat, season::summer), hvac_mode::heat);
}

TEST(ResolveMode, AutoFollowsSeason) {
    EXPECT_EQ(resolve_mode(hvac_mode::auto_select, season::winter), hvac_mode::heat);
    EXPECT_EQ(resolve_mode(hvac_mode::auto_select, season::shoulder), hvac_mode::both);
    EXPECT_EQ(resolve_mode(hvac_mode::auto_select, season::summer), hvac_mode::cool);
}

TEST(ResolveMode, AutoWithNoWeatherNeverCools) {
    EXPECT_EQ(resolve_mode(hvac_mode::auto_select, season::unknown), hvac_mode::heat);
}

// ---- outside temp / refresh ----

TEST(OutsideTemp, HourContainingNow) {
    weather_data w{.fetched_at = 0, .hourly = {{3600, 40}, {7200, 42}}};
    EXPECT_FALSE(outside_temp_at(w, 3599));
    EXPECT_EQ(outside_temp_at(w, 3600), 40);
    EXPECT_EQ(outside_temp_at(w, 7199), 40);
    EXPECT_EQ(outside_temp_at(w, 7200), 42);
    EXPECT_FALSE(outside_temp_at(w, 10800));   // past the data: gate skipped
}

// ---- fetch schedule: once a day at any hour, a try every WEATHER_RETRY until one works ----

TEST_F(Weather, NeverFetchedTriesAtAnyHour) {
    EXPECT_TRUE(weather_fetch_due({}, midnight(5) + 14 * 3600, 0));
}

TEST_F(Weather, TriesRetryApart) {
    long long now = midnight(5) + 3 * 3600;
    weather_data yesterday{.fetched_at = midnight(4) + 3600, .hourly = {}};
    EXPECT_FALSE(weather_fetch_due(yesterday, now, now - WEATHER_RETRY + 1));
    EXPECT_TRUE(weather_fetch_due(yesterday, now, now - WEATHER_RETRY));
}

TEST_F(Weather, AnyHourOfANewDay) {
    // missed the night (Pi off, NWS down): caught up during the day, not a day later
    weather_data yesterday{.fetched_at = midnight(4) + 3600, .hourly = {}};
    EXPECT_TRUE(weather_fetch_due(yesterday, midnight(5), 0));
    EXPECT_TRUE(weather_fetch_due(yesterday, midnight(5) + 8 * 3600, 0));
    EXPECT_TRUE(weather_fetch_due(yesterday, midnight(5) + 14 * 3600, 0));
    EXPECT_TRUE(weather_fetch_due(yesterday, midnight(6) - 1, 0));    // 11:59 pm
}

TEST_F(Weather, SuccessEndsTodaysTries) {
    weather_data today{.fetched_at = midnight(5) + 14 * 3600, .hourly = {}};   // 2 pm
    EXPECT_FALSE(weather_fetch_due(today, midnight(5) + 15 * 3600, 0));
    EXPECT_FALSE(weather_fetch_due(today, midnight(6) - 1, 0));
    EXPECT_TRUE(weather_fetch_due(today, midnight(6), 0));            // next day
}

// a fetch as late as 7 am still makes today a full day (17 hours >= FULL_DAY_HOURS),
// even though the 5 am low is missing: 30 + 15 = 45 is the lowest hour left
TEST_F(Weather, SevenAmFetchCountsToday) {
    EXPECT_EQ(season_from_weather(week({30}, 7)), season::winter);
    EXPECT_EQ(season_from_weather(week({30}, 9)), season::unknown);   // 15 hours: left out
}

// a fetch at 2 pm (allowed since 11-21): today's partial day, missing its morning low,
// is left out rather than lowering or raising the average; the days after it decide
TEST_F(Weather, AfternoonFetchUsesTheDaysAfterToday) {
    // today's 5 am low (30) is gone; its 2 pm-on hours are 45, which would read as shoulder
    EXPECT_EQ(season_from_weather(week({30, 40, 40, 40}, 14)), season::winter);   // avg of 40s
    EXPECT_EQ(season_from_weather(week({80, 40, 40, 40}, 14)), season::winter);   // today ignored either way
}

// ---- NWS reply ----

TEST_F(Weather, Iso8601WithOffset) {
    EXPECT_EQ(parse_iso8601("2027-01-05T14:00:00-06:00"), midnight(5) + 14 * 3600);   // Central, January
    EXPECT_EQ(parse_iso8601("1970-01-01T00:00:00+00:00"), 0);
    EXPECT_EQ(parse_iso8601("1970-01-01T01:30:00+01:30"), 0);
    for (const char* bad : {"2027-01-05T14:00:00", "2027-01-05T14:00:00Z", "2027-01-05 14:00:00-06:00",
                            "2027-01-05T14:00:00-06:00 junk", "", "soon"})
        EXPECT_FALSE(parse_iso8601(bad)) << bad;
}

TEST(NwsHourly, ReadsPeriods) {
    std::string body = R"({"properties": {"periods": [
        {"number": 1, "startTime": "1970-01-01T00:00:00+00:00", "temperature": 41, "temperatureUnit": "F"},
        {"number": 2, "startTime": "1970-01-01T01:00:00+00:00", "temperature": 5, "temperatureUnit": "C"},
        {"number": 3, "startTime": "1970-01-01T02:00:00+00:00",
         "temperature": {"value": 10, "unitCode": "wmoUnit:degC"}}
    ]}})";
    std::optional<std::vector<forecast_hour>> hours = parse_nws_hourly(body);
    ASSERT_TRUE(hours);
    ASSERT_EQ(hours->size(), 3u);
    EXPECT_EQ((*hours)[0].time, 0);
    EXPECT_FLOAT_EQ((*hours)[0].temp, 41);
    EXPECT_FLOAT_EQ((*hours)[1].temp, 41);   // 5 C
    EXPECT_EQ((*hours)[2].time, 7200);
    EXPECT_FLOAT_EQ((*hours)[2].temp, 50);   // 10 C
}

TEST(NwsHourly, SkipsBadPeriodsKeepsGoodOnes) {
    std::string body = R"({"properties": {"periods": [
        {"startTime": "not a time", "temperature": 41},
        {"startTime": "1970-01-01T01:00:00+00:00", "temperature": null},
        {"startTime": "1970-01-01T02:00:00+00:00", "temperature": 39, "temperatureUnit": "F"}
    ]}})";
    std::optional<std::vector<forecast_hour>> hours = parse_nws_hourly(body);
    ASSERT_TRUE(hours);
    ASSERT_EQ(hours->size(), 1u);
    EXPECT_EQ((*hours)[0].time, 7200);
}

TEST(NwsHourly, ReadsIconDaytimeAndText) {
    std::string body = R"({"properties": {"periods": [
        {"startTime": "1970-01-01T00:00:00+00:00", "temperature": 41, "isDaytime": false,
         "icon": "https://api.weather.gov/icons/land/night/rain_showers,20?size=small",
         "shortForecast": "Slight Chance Rain Showers"},
        {"startTime": "1970-01-01T01:00:00+00:00", "temperature": 42}
    ]}})";
    std::optional<std::vector<forecast_hour>> hours = parse_nws_hourly(body);
    ASSERT_TRUE(hours);
    ASSERT_EQ(hours->size(), 2u);
    EXPECT_EQ((*hours)[0].condition, "rain_showers");
    EXPECT_FALSE((*hours)[0].is_day);
    EXPECT_EQ((*hours)[0].short_forecast, "Slight Chance Rain Showers");
    // all optional: missing ones keep the defaults, the hour is still used
    EXPECT_EQ((*hours)[1].condition, "");
    EXPECT_TRUE((*hours)[1].is_day);
    EXPECT_EQ((*hours)[1].short_forecast, "");
}

TEST(NwsIcon, Condition) {
    EXPECT_EQ(nws_icon_condition("https://api.weather.gov/icons/land/day/skc?size=small"), "skc");
    EXPECT_EQ(nws_icon_condition("https://api.weather.gov/icons/land/night/tsra_sct,40?size=small"), "tsra_sct");
    // the hour changes partway: the first
    EXPECT_EQ(nws_icon_condition("https://api.weather.gov/icons/land/day/rain,30/snow,60?size=small"), "rain");
    EXPECT_EQ(nws_icon_condition("https://api.weather.gov/icons/land/day/fog"), "fog");
    for (const char* bad : {"", "https://api.weather.gov/icons/land/", "not a url"})
        EXPECT_EQ(nws_icon_condition(bad), "") << bad;
}

TEST(NwsHourly, NotAForecast) {
    for (const char* body : {"", "<html>busy</html>", "{}", R"({"properties": {}})",
                             R"({"properties": {"periods": []}})", R"({"properties": {"periods": 3}})",
                             R"({"properties": {"periods": [{"temperature": 41}]}})"})
        EXPECT_FALSE(parse_nws_hourly(body)) << body;
}
