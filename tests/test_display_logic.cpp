// Tests pi/display/display_logic.cpp: the display program's pure logic.

#include <gtest/gtest.h>
#include "display_logic.h"
#include "data_store.h"   // valid_set_points: the display's rule must match control's

#include <cstdlib>        // setenv
#include <ctime>          // tzset
#include <filesystem>

static settings_edit edit(float heat, float ac, float night = 2, std::string mode = "auto", bool fan = false) {
    return {heat, ac, night, mode, fan};
}

// ---- set point steps ----

TEST(DisplayEdit, SameRuleAsControl) {
    for (float heat = 55; heat <= 95; heat += 0.5f)
        for (float ac = 55; ac <= 95; ac += 0.5f)
            for (float night : {-1.0f, 0.0f, 2.0f, 5.0f, 10.0f}) {
                hvac_set_points p{.heat_point = heat, .ac_point = ac, .night_difference = night};
                EXPECT_EQ(settings_valid(edit(heat, ac, night)), valid_set_points(p))
                    << heat << " " << ac << " " << night;
            }
}

TEST(DisplayEdit, PlainSteps) {
    settings_edit s = edit(68, 74);
    EXPECT_TRUE(step_heat(s, +1));
    EXPECT_TRUE(step_ac(s, -1));
    EXPECT_EQ(s, edit(69, 73));
    EXPECT_TRUE(step_night(s, +1));
    EXPECT_FLOAT_EQ(s.night_difference, 3);
}

TEST(DisplayEdit, HeatPushesAcAndBack) {
    settings_edit s = edit(70, 72);
    EXPECT_TRUE(step_heat(s, +1));
    EXPECT_EQ(s, edit(71, 73));    // AC pushed to keep the dead band
    EXPECT_TRUE(step_ac(s, -1));
    EXPECT_TRUE(step_ac(s, -1));
    EXPECT_EQ(s, edit(69, 71));    // heat pushed down
}

TEST(DisplayEdit, LimitsRefuseTheStep) {
    settings_edit s = edit(88, 90);
    EXPECT_FALSE(step_ac(s, +1));      // over AC_MAX
    EXPECT_FALSE(step_heat(s, +1));    // its push would put AC over AC_MAX
    EXPECT_EQ(s, edit(88, 90));

    s = edit(65, 72, 2);               // night heat 63 > 60 + 2
    EXPECT_FALSE(step_heat(s, -1));    // 64 - 2 = 62: not > 62
    EXPECT_FALSE(step_night(s, +1));
    EXPECT_FALSE(step_ac(s, -6));      // pushes heat to 64: same
    EXPECT_EQ(s, edit(65, 72, 2));

    s = edit(70, 72, 0);
    EXPECT_FALSE(step_night(s, -1));   // below 0
}

TEST(DisplayEdit, KeepsFractions) {
    settings_edit s = edit(70.5f, 74);
    EXPECT_TRUE(step_heat(s, +1));
    EXPECT_FLOAT_EQ(s.heat_point, 71.5f);
    EXPECT_FLOAT_EQ(s.ac_point, 74);
}

// ---- what Done sends ----

TEST(DisplayDone, NothingChangedSendsNothing) {
    settings_edit s = edit(70, 74);
    EXPECT_FALSE(setpoints_request(s, s));
    EXPECT_FALSE(fan_request(s, s));
}

TEST(DisplayDone, SetpointsSentWhole) {
    settings_edit before = edit(70, 74), after = edit(70, 74, 2, "heat");
    std::optional<json> body = setpoints_request(before, after);
    ASSERT_TRUE(body);
    EXPECT_EQ(*body, (json{{"heat_point", 70.0}, {"ac_point", 74.0}, {"night_difference", 2.0}, {"mode", "heat"}}));
    EXPECT_FALSE(fan_request(before, after));
}

TEST(DisplayDone, FanOnly) {
    settings_edit before = edit(70, 74), after = edit(70, 74, 2, "auto", true);
    EXPECT_FALSE(setpoints_request(before, after));
    ASSERT_TRUE(fan_request(before, after));
    EXPECT_EQ(*fan_request(before, after), (json{{"fan", "on"}}));
    EXPECT_EQ(*fan_request(after, before), (json{{"fan", "auto"}}));
}

// ---- API replies ----

TEST(DisplayParse, Status) {
    std::optional<status_reply> r = parse_status(
        R"({"status": "em_heating", "mode": "auto", "active_mode": "heat", "fan": "on", "away": false})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, display_status::em_heating);
    EXPECT_TRUE(r->fan_on);
    EXPECT_EQ(parse_status(R"({"status": "new_thing", "fan": "auto"})")->status, display_status::unknown);
    for (const char* bad : {"", "[]", "{}", R"({"status": 3, "fan": "on"})"})
        EXPECT_FALSE(parse_status(bad)) << bad;
}

TEST(DisplayParse, MainTemp) {
    EXPECT_FLOAT_EQ(*parse_main_temp(R"({"main": {"temp": 71.4, "humidity": 40, "co2": 600, "trusted": true}})"), 71.4f);
    EXPECT_FALSE(parse_main_temp(R"({"main": {"temp": 71.4, "humidity": 40, "co2": 600, "trusted": false}})"));
    EXPECT_FALSE(parse_main_temp(R"({"main": null})"));
    EXPECT_FALSE(parse_main_temp("not json"));
}

TEST(DisplayParse, Setpoints) {
    std::optional<settings_edit> s = parse_setpoints(
        R"({"heat_point": 68, "ac_point": 74.5, "night_difference": 2, "mode": "both"})");
    ASSERT_TRUE(s);
    EXPECT_EQ(*s, edit(68, 74.5f, 2, "both"));
    EXPECT_FALSE(parse_setpoints(R"({"heat_point": 68})"));
}

TEST(DisplayParse, Weather) {
    std::optional<std::vector<forecast_entry>> w = parse_weather(R"({"fetched_at": 5, "hours": [
        {"time": 3600, "temp": 59.0, "condition": "skc", "is_day": false, "short_forecast": "Clear"},
        {"time": "bad", "temp": 1},
        {"time": 7200, "temp": 61.0}]})");
    ASSERT_TRUE(w);
    ASSERT_EQ(w->size(), 2u);
    EXPECT_EQ((*w)[0].condition, "skc");
    EXPECT_FALSE((*w)[0].is_day);
    EXPECT_EQ((*w)[1].condition, "");   // missing: defaults
    EXPECT_TRUE((*w)[1].is_day);
    EXPECT_TRUE(parse_weather(R"({"fetched_at": 0, "hours": []})")->empty());
    EXPECT_FALSE(parse_weather("{}"));
}

// ---- text ----

TEST(DisplayText, Numbers) {
    EXPECT_EQ(format_setpoint(71), "71");
    EXPECT_EQ(format_setpoint(70.5f), "70.5");
    EXPECT_EQ(format_temp(71.4f), "71");
    EXPECT_EQ(format_temp(71.6f), "72");
}

TEST(DisplayText, Hours) {
    setenv("TZ", "UTC", 1);
    tzset();
    EXPECT_EQ(format_hour(0), "12 AM");
    EXPECT_EQ(format_hour(9 * 3600), "9 AM");
    EXPECT_EQ(format_hour(12 * 3600), "12 PM");
    EXPECT_EQ(format_hour(15 * 3600 + 1800), "3 PM");
}

// ---- icons ----

TEST(DisplayIcons, DayNightAndUnknown) {
    EXPECT_EQ(icon_name("skc", true), "clear-day");
    EXPECT_EQ(icon_name("skc", false), "clear-night");
    EXPECT_EQ(icon_name("tsra_sct", false), "thunderstorms-night-rain");
    EXPECT_EQ(icon_name("rain", false), "rain");   // no night version
    EXPECT_EQ(icon_name("", true), "not-available");
    EXPECT_EQ(icon_name("something_new", true), "not-available");
}

TEST(DisplayIcons, EveryIconShips) {
    std::filesystem::path dir = std::filesystem::path(__FILE__).parent_path() / ".." / "pi" / "display" / "assets" / "icons";
    for (const std::string& name : all_icon_names())
        EXPECT_TRUE(std::filesystem::exists(dir / (name + ".json"))) << name;
}

// ---- touch ----

TEST(DisplayTouch, CornersMapToCorners) {
    touch_calibration cal{.raw_y_left = 500, .raw_y_right = 3600, .raw_x_top = 400, .raw_x_bottom = 3600,
                          .width = 320, .height = 240};
    screen_point p = touch_to_screen(400, 500, cal);
    EXPECT_EQ(p.x, 0);
    EXPECT_EQ(p.y, 0);
    p = touch_to_screen(3600, 3600, cal);
    EXPECT_EQ(p.x, 319);
    EXPECT_EQ(p.y, 239);
    p = touch_to_screen(2000, 2050, cal);
    EXPECT_EQ(p.x, 160);
    EXPECT_EQ(p.y, 120);
    p = touch_to_screen(100, 4000, cal);   // beyond the edges: clamped
    EXPECT_EQ(p.x, 319);
    EXPECT_EQ(p.y, 0);
}

TEST(DisplayTouch, FilterDropsStrayReads) {
    EXPECT_EQ(filter_samples({1000, 4095, 1010, 990, 20, 1005, 1000}, 100, 4000, 4, 60), 1000);
    EXPECT_FALSE(filter_samples({4095, 4095, 1000, 20, 0}, 100, 4000, 4, 60));       // too few left
    EXPECT_FALSE(filter_samples({1000, 1500, 2000, 2500, 3000}, 100, 4000, 4, 60));  // disagree
}
