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

TEST(WeatherDue, OncePerDay) {
    EXPECT_TRUE(weather_due({}, 1'000'000));   // never fetched
    weather_data w{.fetched_at = 1'000'000, .hourly = {}};
    EXPECT_FALSE(weather_due(w, 1'000'000 + WEATHER_REFRESH - 1));
    EXPECT_TRUE(weather_due(w, 1'000'000 + WEATHER_REFRESH));
}
