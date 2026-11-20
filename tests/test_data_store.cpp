#include <gtest/gtest.h>
#include "data_store.h"

#include <cstdlib>        // mkdtemp, setenv
#include <ctime>          // tzset
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// Each test gets its own empty temp dir for the files, and a fixed time zone
// (US Central) so nothing depends on the machine's.
class DataStore : public ::testing::Test {
protected:
    fs::path dir;
    settings_store store;

    void SetUp() override {
        setenv("TZ", "America/Chicago", 1);
        tzset();
        char tmpl[] = "/tmp/data_store_test_XXXXXX";
        dir = mkdtemp(tmpl);
        store.hvac_path = dir / "hvac_points.json";
        store.vacation_path = dir / "vacation.json";
        store.weather_path = dir / "weather.json";
    }

    settings_store restart() {
        settings_store r;
        r.hvac_path = store.hvac_path;
        r.vacation_path = store.vacation_path;
        r.weather_path = store.weather_path;
        load_settings(r);
        return r;
    }
    void TearDown() override { fs::remove_all(dir); }

    void write(const std::string& path, const std::string& text) { std::ofstream(path) << text; }
    std::string read(const std::string& path) {
        std::ifstream f(path);
        return {std::istreambuf_iterator<char>(f), {}};
    }
    // vacation times are plain Unix seconds; any values work
    static constexpr long long T0 = 1'800'000'000;
    static constexpr long long DAY = 24 * 3600;

    // a store with two set points, as if set through the API
    void set_points(float heat, float ac, float night = 2) { store.points = {heat, ac, night}; }
};

// ---- valid_set_points ----

TEST(ValidSetPoints, DefaultsAreValid) {
    EXPECT_TRUE(valid_set_points({}));
}

TEST(ValidSetPoints, Boundaries) {
    // night heat must be strictly above MIN_HEAT + MIN_DEAD_BAND (60 + 2)
    EXPECT_FALSE(valid_set_points({64, 72, 2}));
    EXPECT_TRUE(valid_set_points({64.5, 72, 2}));
    // AC exactly the dead band above heat is fine
    EXPECT_TRUE(valid_set_points({70, 72, 2}));
    EXPECT_FALSE(valid_set_points({70, 71.5, 2}));
    // AC max
    EXPECT_TRUE(valid_set_points({70, AC_MAX, 2}));
    EXPECT_FALSE(valid_set_points({70, AC_MAX + 0.5f, 2}));
    // night difference can't be negative
    EXPECT_TRUE(valid_set_points({70, 72, 0}));
    EXPECT_FALSE(valid_set_points({70, 72, -1}));
}

// ---- loading hvac_points.json ----

TEST_F(DataStore, MissingFilesGiveDefaults) {
    set_points(75, 80);   // would-be leftovers must not survive a load
    load_settings(store);
    hvac_set_points d;
    EXPECT_EQ(store.points.heat_point, d.heat_point);
    EXPECT_EQ(store.points.ac_point, d.ac_point);
    EXPECT_EQ(store.points.night_difference, d.night_difference);
    EXPECT_TRUE(store.vacations.blocks.empty());
    EXPECT_EQ(store.vacations.next_id, 0);
}

TEST_F(DataStore, LoadsValidPoints) {
    write(store.hvac_path, R"({"heat_point": 68, "ac_point": 76, "night_difference": 3})");
    load_settings(store);
    EXPECT_EQ(store.points.heat_point, 68);
    EXPECT_EQ(store.points.ac_point, 76);
    EXPECT_EQ(store.points.night_difference, 3);
}

TEST_F(DataStore, BadPointsFilesGiveDefaults) {
    for (const char* text : {
             "not json",
             R"({"heat_point": 68, "ac_point": 76})",                              // missing key
             R"({"heat_point": "68", "ac_point": 76, "night_difference": 3})",     // wrong type
             R"({"heat_point": 75, "ac_point": 72, "night_difference": 2})",       // heat above AC
         }) {
        write(store.hvac_path, text);
        load_settings(store);
        EXPECT_EQ(store.points.heat_point, hvac_set_points{}.heat_point) << text;
        EXPECT_EQ(store.points.ac_point, hvac_set_points{}.ac_point) << text;
    }
}

// ---- update_set_points ----

TEST_F(DataStore, HeatAloneLeavesRoomyAcAlone) {
    set_points(70, 72);
    EXPECT_EQ(update_set_points(store, {{"heat_point", 68}}), request_result::saved);
    EXPECT_EQ(store.points.heat_point, 68);
    EXPECT_EQ(store.points.ac_point, 72);
}

TEST_F(DataStore, HeatPushesAcUp) {
    set_points(70, 72);
    EXPECT_EQ(update_set_points(store, {{"heat_point", 76}}), request_result::saved);
    EXPECT_EQ(store.points.heat_point, 76);
    EXPECT_EQ(store.points.ac_point, 76 + MIN_DEAD_BAND);   // from the new heat, not old AC
}

TEST_F(DataStore, AcPushesHeatDown) {
    set_points(70, 74);
    EXPECT_EQ(update_set_points(store, {{"ac_point", 71}}), request_result::saved);
    EXPECT_EQ(store.points.ac_point, 71);
    EXPECT_EQ(store.points.heat_point, 71 - MIN_DEAD_BAND);
}

TEST_F(DataStore, AcAloneLeavesRoomyHeatAlone) {
    set_points(70, 72);
    EXPECT_EQ(update_set_points(store, {{"ac_point", 78}}), request_result::saved);
    EXPECT_EQ(store.points.heat_point, 70);
    EXPECT_EQ(store.points.ac_point, 78);
}

TEST_F(DataStore, PushOutOfRangeIsRejectedAndChangesNothing) {
    set_points(70, 72);
    write(store.hvac_path, "untouched");
    // heat 89 would push AC to 91 > AC_MAX
    EXPECT_EQ(update_set_points(store, {{"heat_point", 89}}), request_result::rejected);
    // AC 63 would push heat to 61, too close to MIN_HEAT
    EXPECT_EQ(update_set_points(store, {{"ac_point", 63}}), request_result::rejected);
    EXPECT_EQ(store.points.heat_point, 70);
    EXPECT_EQ(store.points.ac_point, 72);
    EXPECT_EQ(read(store.hvac_path), "untouched");
}

TEST_F(DataStore, BothAtOnceAreNotPushed) {
    set_points(70, 72);
    // both given: validated as a pair, nothing pushed
    EXPECT_EQ(update_set_points(store, {{"heat_point", 74}, {"ac_point", 73}}), request_result::rejected);
    EXPECT_EQ(update_set_points(store, {{"heat_point", 68}, {"ac_point", 78}}), request_result::saved);
    EXPECT_EQ(store.points.heat_point, 68);
    EXPECT_EQ(store.points.ac_point, 78);
}

TEST_F(DataStore, NightDifferenceAlone) {
    set_points(70, 72);
    EXPECT_EQ(update_set_points(store, {{"night_difference", 5}}), request_result::saved);
    EXPECT_EQ(store.points.night_difference, 5);
    // 70 - 8 = 62, not above MIN_HEAT + MIN_DEAD_BAND
    EXPECT_EQ(update_set_points(store, {{"night_difference", 8}}), request_result::rejected);
    EXPECT_EQ(store.points.night_difference, 5);
}

TEST_F(DataStore, MalformedRequestsRejected) {
    set_points(70, 72);
    for (const json& request : {
             json::object(),                           // nothing to change
             json{{"heat", 68}},                       // unknown key (typo)
             json{{"heat_point", 68}, {"fan", 1}},     // one good key doesn't save a bad one
             json{{"heat_point", "68"}},               // string, not number
             json::array({68}),
         }) {
        EXPECT_EQ(update_set_points(store, request), request_result::rejected) << request;
    }
    EXPECT_FALSE(fs::exists(store.hvac_path));
}

TEST_F(DataStore, SavedPointsReloadAfterRestart) {
    set_points(70, 72);
    ASSERT_EQ(update_set_points(store, {{"heat_point", 67.5}}), request_result::saved);

    settings_store restarted;
    restarted.hvac_path = store.hvac_path;
    restarted.vacation_path = store.vacation_path;
    load_settings(restarted);
    EXPECT_EQ(restarted.points.heat_point, 67.5);
    EXPECT_EQ(restarted.points.ac_point, 72);
    EXPECT_FALSE(fs::exists(store.hvac_path + ".tmp"));   // temp file renamed away
}

// ---- failed writes ----

TEST_F(DataStore, FailedWriteKeepsNewValuesAndRetries) {
    store.hvac_path = (dir / "missing_dir" / "hvac_points.json").string();   // can't be written
    set_points(70, 72);

    EXPECT_EQ(update_set_points(store, {{"heat_point", 68}}), request_result::not_saved_yet);
    EXPECT_EQ(store.points.heat_point, 68);   // in use anyway
    EXPECT_TRUE(store.points_unsaved);

    retry_unsaved(store);                     // still failing: still unsaved
    EXPECT_TRUE(store.points_unsaved);

    // a second change while unsaved: the retry writes the latest values
    EXPECT_EQ(update_set_points(store, {{"heat_point", 66}}), request_result::not_saved_yet);
    fs::create_directory(dir / "missing_dir");
    retry_unsaved(store);
    EXPECT_FALSE(store.points_unsaved);

    settings_store restarted;
    restarted.hvac_path = store.hvac_path;
    restarted.vacation_path = store.vacation_path;
    load_settings(restarted);
    EXPECT_EQ(restarted.points.heat_point, 66);
}

// ---- vacations: requests ----

json vacation(long long start, long long end) {
    return {{"vacation_start", start}, {"vacation_end", end}};
}

TEST_F(DataStore, AddAssignsIncreasingIds) {
    EXPECT_EQ(add_vacation(store, vacation(T0, T0 + 7 * DAY)), request_result::saved);
    EXPECT_EQ(add_vacation(store, vacation(T0 + 20 * DAY, T0 + 22 * DAY)), request_result::saved);
    ASSERT_EQ(store.vacations.blocks.size(), 2u);
    EXPECT_EQ(store.vacations.blocks[0].block_id, 0);
    EXPECT_EQ(store.vacations.blocks[1].block_id, 1);
    EXPECT_EQ(store.vacations.blocks[0].vacation_end, T0 + 7 * DAY);
    EXPECT_EQ(store.vacations.next_id, 2);

    // ids aren't reused after a removal
    EXPECT_EQ(remove_vacation(store, 1), request_result::saved);
    add_vacation(store, vacation(T0 + 40 * DAY, T0 + 41 * DAY));
    EXPECT_EQ(store.vacations.blocks.back().block_id, 2);
}

TEST_F(DataStore, AddRejectsBadVacations) {
    for (const json& request : {
             vacation(T0 + 7 * DAY, T0),                                    // end first
             vacation(T0, T0),                                              // zero length
             json{{"vacation_start", T0}},                                  // no end
             json{{"vacation_start", "12/20/2026 08:00"}, {"vacation_end", T0}},  // text, not Unix
             json{{"vacation_start", 1.5}, {"vacation_end", T0}},           // not a whole number
             json{{"vacation_start", T0}, {"vacation_end", T0 + DAY}, {"block_id", 5}},
         }) {
        EXPECT_EQ(add_vacation(store, request), request_result::rejected) << request;
    }
    EXPECT_TRUE(store.vacations.blocks.empty());
    EXPECT_EQ(store.vacations.next_id, 0);
}

TEST_F(DataStore, ChangeOneTime) {
    add_vacation(store, vacation(T0, T0 + 7 * DAY));
    EXPECT_EQ(change_vacation(store, 0, {{"vacation_end", T0 + 8 * DAY}}), request_result::saved);
    EXPECT_EQ(store.vacations.blocks[0].vacation_start, T0);
    EXPECT_EQ(store.vacations.blocks[0].vacation_end, T0 + 8 * DAY);
}

TEST_F(DataStore, ChangeRejects) {
    add_vacation(store, vacation(T0, T0 + 7 * DAY));
    EXPECT_EQ(change_vacation(store, 9, {{"vacation_end", T0 + 8 * DAY}}), request_result::not_found);
    EXPECT_EQ(change_vacation(store, 0, {{"vacation_end", T0 - DAY}}), request_result::rejected);
    EXPECT_EQ(change_vacation(store, 0, json::object()), request_result::rejected);   // nothing to change
    EXPECT_EQ(change_vacation(store, 0, {{"block_id", 1}}), request_result::rejected);
    EXPECT_EQ(store.vacations.blocks[0].vacation_end, T0 + 7 * DAY);
}

TEST_F(DataStore, RemoveUnknownIdNotFound) {
    EXPECT_EQ(remove_vacation(store, 0), request_result::not_found);
}

// ---- vacations: file ----

TEST_F(DataStore, VacationsReloadAfterRestart) {
    add_vacation(store, vacation(T0, T0 + 7 * DAY));
    add_vacation(store, vacation(T0 + 20 * DAY, T0 + 22 * DAY));

    settings_store restarted;
    restarted.hvac_path = store.hvac_path;
    restarted.vacation_path = store.vacation_path;
    load_settings(restarted);
    ASSERT_EQ(restarted.vacations.blocks.size(), 2u);
    EXPECT_EQ(restarted.vacations.blocks[1].block_id, 1);
    EXPECT_EQ(restarted.vacations.blocks[1].vacation_start, T0 + 20 * DAY);
    EXPECT_EQ(restarted.vacations.next_id, 2);
}

TEST_F(DataStore, BadBlocksDroppedGoodOnesKept) {
    write(store.vacation_path, R"({
        "next_id": 1,
        "blocks": [
            {},
            {"block_id": 0, "vacation_start": 100, "vacation_end": 200},
            {"block_id": 3, "vacation_start": 500, "vacation_end": 400},
            {"block_id": 0, "vacation_start": 600, "vacation_end": 700},
            {"block_id": 4, "vacation_start": "12/20/2026 08:00", "vacation_end": 900},
            {"block_id": 5, "vacation_start": 800, "vacation_end": 900}
        ]})");
    load_settings(store);
    ASSERT_EQ(store.vacations.blocks.size(), 2u);   // 0 and 5
    EXPECT_EQ(store.vacations.blocks[0].block_id, 0);
    EXPECT_EQ(store.vacations.blocks[1].block_id, 5);
    EXPECT_EQ(store.vacations.next_id, 6);          // behind in the file; never reuse 5
}

TEST_F(DataStore, UnreadableVacationFileMeansNone) {
    for (const char* text : {"nope", "[]", R"({"next_id": 2})", R"({"blocks": 4})"}) {
        write(store.vacation_path, text);
        load_settings(store);
        EXPECT_TRUE(store.vacations.blocks.empty()) << text;
    }
}

// ---- vacations: main loop ----

TEST_F(DataStore, FinishedVacationsRemovedAndSaved) {
    store.vacations = {.next_id = 3, .blocks = {{0, 100, 200}, {1, 150, 300}, {2, 400, 500}}};
    remove_finished_vacations(store, 200);   // block 0 returns exactly now
    ASSERT_EQ(store.vacations.blocks.size(), 2u);
    EXPECT_EQ(store.vacations.blocks[0].block_id, 1);

    settings_store restarted;
    restarted.hvac_path = store.hvac_path;
    restarted.vacation_path = store.vacation_path;
    load_settings(restarted);
    EXPECT_EQ(restarted.vacations.blocks.size(), 2u);
}

TEST_F(DataStore, NothingFinishedMeansNoWrite) {
    store.vacations = {.next_id = 1, .blocks = {{0, 100, 200}}};
    remove_finished_vacations(store, 150);
    EXPECT_FALSE(fs::exists(store.vacation_path));
}

TEST(IsAway, StartToLeadBeforeReturn) {
    long long start = 1'000'000, end = start + 3 * 24 * 3600;
    vacation_list v{.next_id = 1, .blocks = {{0, start, end}}};
    EXPECT_FALSE(is_away(v, start - 1));
    EXPECT_TRUE(is_away(v, start));
    EXPECT_TRUE(is_away(v, end - PRECONDITION_LEAD - 1));
    EXPECT_FALSE(is_away(v, end - PRECONDITION_LEAD));   // pre-conditioning starts
    EXPECT_FALSE(is_away(v, end));
}

TEST(IsAway, OverlapsActAsOneSpan) {
    long long h = 3600;
    // A: 0-10 h, B: 5-20 h -> away from 0 until 20 h - lead
    vacation_list v{.next_id = 2, .blocks = {{0, 0, 10 * h}, {1, 5 * h, 20 * h}}};
    EXPECT_TRUE(is_away(v, 0));
    EXPECT_TRUE(is_away(v, 10 * h - PRECONDITION_LEAD));   // A pre-conditioning, but B still away
    EXPECT_TRUE(is_away(v, 20 * h - PRECONDITION_LEAD - 1));
    EXPECT_FALSE(is_away(v, 20 * h - PRECONDITION_LEAD));
}

TEST(IsAway, TripShorterThanLeadNeverAway) {
    vacation_list v{.next_id = 1, .blocks = {{0, 0, PRECONDITION_LEAD}}};
    EXPECT_FALSE(is_away(v, 0));
}

// ---- mode ----

TEST_F(DataStore, ModeDefaultsToAutoForOldFiles) {
    write(store.hvac_path, R"({"heat_point": 68, "ac_point": 76, "night_difference": 3})");
    store.mode = hvac_mode::cool;
    load_settings(store);
    EXPECT_EQ(store.mode, hvac_mode::auto_select);
    EXPECT_EQ(store.points.heat_point, 68);
}

TEST_F(DataStore, ModeSavedAndReloaded) {
    EXPECT_EQ(update_set_points(store, {{"mode", "heat"}}), request_result::saved);
    EXPECT_EQ(store.mode, hvac_mode::heat);
    EXPECT_EQ(restart().mode, hvac_mode::heat);
    EXPECT_EQ(update_set_points(store, {{"mode", "auto"}, {"heat_point", 69}}), request_result::saved);
    settings_store r = restart();
    EXPECT_EQ(r.mode, hvac_mode::auto_select);
    EXPECT_EQ(r.points.heat_point, 69);
}

TEST_F(DataStore, BadModeRejectedAndChangesNothing) {
    for (const json& request : {json{{"mode", "off"}}, json{{"mode", 1}},
                                json{{"mode", "heat"}, {"heat_point", 95}}}) {
        EXPECT_EQ(update_set_points(store, request), request_result::rejected) << request;
    }
    EXPECT_EQ(store.mode, hvac_mode::auto_select);
}

TEST_F(DataStore, UnknownModeInFileGivesDefaults) {
    write(store.hvac_path, R"({"heat_point": 68, "ac_point": 76, "night_difference": 3, "mode": "warm"})");
    load_settings(store);
    EXPECT_EQ(store.mode, hvac_mode::auto_select);
    EXPECT_EQ(store.points.heat_point, hvac_set_points{}.heat_point);
}

// ---- weather.json ----

static std::vector<hourly_temp> hours(long long start, int count, float temp) {
    std::vector<hourly_temp> v;
    for (int i = 0; i < count; i++) v.push_back({start + i * 3600LL, temp});
    return v;
}

TEST_F(DataStore, WeatherSavedTrimmedAndReloaded) {
    long long hour = 1'795'000'000 / 3600 * 3600;   // on the hour
    long long now = hour + 30 * 60;
    // 10 days from 2 hours ago, given out of order
    std::vector<hourly_temp> fetched = hours(hour - 2 * 3600, 240, 40);
    std::reverse(fetched.begin(), fetched.end());

    EXPECT_EQ(update_weather(store, fetched, now), request_result::saved);
    ASSERT_EQ(store.weather.hourly.size(), static_cast<size_t>(WEATHER_DAYS * 24));
    EXPECT_EQ(store.weather.hourly.front().time, hour);   // current hour kept, past ones dropped
    EXPECT_EQ(store.weather.fetched_at, now);

    settings_store r = restart();
    EXPECT_EQ(r.weather.hourly.size(), store.weather.hourly.size());
    EXPECT_EQ(r.weather.hourly.front().time, hour);
    EXPECT_EQ(r.weather.fetched_at, now);
}

TEST_F(DataStore, OldWeatherStillLoads) {
    // no expiry: last month's data still decides the season after a restart
    update_weather(store, hours(1'000'000, 48, 40), 1'000'000);
    EXPECT_EQ(restart().weather.hourly.size(), 48u);
}

TEST_F(DataStore, BadWeatherFileOrHours) {
    write(store.weather_path, "nope");
    load_settings(store);
    EXPECT_TRUE(store.weather.hourly.empty());

    write(store.weather_path, R"({"fetched_at": 5, "hourly": [
        {"time": 7200, "temp": 41}, {"time": "x", "temp": 40}, {}, {"time": 3600, "temp": 39}]})");
    load_settings(store);
    ASSERT_EQ(store.weather.hourly.size(), 2u);
    EXPECT_EQ(store.weather.hourly[0].time, 3600);   // sorted
    EXPECT_EQ(store.weather.fetched_at, 5);
}

TEST_F(DataStore, WeatherWriteRetried) {
    store.weather_path = (dir / "missing_dir" / "weather.json").string();
    EXPECT_EQ(update_weather(store, hours(0, 24, 40), 0), request_result::not_saved_yet);
    EXPECT_EQ(store.weather.hourly.size(), 24u);   // in use anyway
    fs::create_directory(dir / "missing_dir");
    retry_unsaved(store);
    EXPECT_FALSE(store.weather_unsaved);
    EXPECT_EQ(restart().weather.hourly.size(), 24u);
}

TEST_F(DataStore, ApiJson) {
    set_points(70, 73, 2);
    store.mode = hvac_mode::cool;
    EXPECT_EQ(set_points_json(store),
              (json{{"heat_point", 70}, {"ac_point", 73}, {"night_difference", 2}, {"mode", "cool"}}));

    add_vacation(store, vacation(T0, T0 + DAY));
    EXPECT_EQ(vacations_json(store),
              (json{{"blocks", {{{"block_id", 0}, {"vacation_start", T0}, {"vacation_end", T0 + DAY}}}}}));
}
