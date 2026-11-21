// Tests pi/thermostat.cpp (one control pass) and pi/api.cpp (the endpoint handlers).
// No HTTP: the handlers are called directly; net.cpp only wires them to routes.

#include <gtest/gtest.h>
#include "api.h"
#include "thermostat.h"
#include "fake_relay.h"
#include "relay.h"

#include <cstdlib>        // mkdtemp, setenv
#include <ctime>          // tzset
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

class Api : public ::testing::Test {
protected:
    fs::path dir;
    shared_state s;
    static constexpr long long NOW_UNIX = 1'800'000'000;
    static constexpr long long DAY = 24 * 3600;

    void SetUp() override {
        setenv("TZ", "America/Chicago", 1);
        tzset();
        fake_relay_reset();
        char tmpl[] = "/tmp/api_test_XXXXXX";
        dir = mkdtemp(tmpl);
        s.settings.hvac_path = dir / "hvac_points.json";
        s.settings.vacation_path = dir / "vacation.json";
        s.settings.weather_path = dir / "weather.json";
        s.control = initial_control_state(0);
    }
    void TearDown() override { fs::remove_all(dir); }

    // reported = false: never had a successful read (a missing satellite)
    static sensor_node node(const char* name, float temp, bool trusted = true, bool reported = true,
                            int co2 = 600, int weight = 1) {
        sensor_node n = { .name = name, .weight_day = weight, .weight_night = weight };
        n.data = {.temp = temp, .hum = 41.2f, .CO2 = co2, .read_ok = 1};
        n.health.trusted = trusted;
        n.health.has_last = reported;
        return n;
    }

    // main trusted, office never reported, bed reporting but untrusted
    std::vector<sensor_node> nodes(float main_temp = 71.5f) {
        return { node("main", main_temp), node("office", 0, false, false), node("bed", 80, false) };
    }

    void tick(const std::vector<sensor_node>& n, long long now_ms, bool synced = true, long long now_unix = NOW_UNIX) {
        control_tick(s, n, now_ms, now_unix, false, synced);
    }
};

// ---- control_tick ----

TEST_F(Api, HistoryWaitsForClockSync) {
    tick(nodes(), MIN, false);
    EXPECT_EQ(s.history.size(), 0u);

    tick(nodes(), 2 * MIN, true);
    std::vector<history_entry> h = s.history.entries_since(0);
    ASSERT_EQ(h.size(), 1u);
    EXPECT_EQ(h[0].time, NOW_UNIX);
    EXPECT_EQ(h[0].status, hvac_status::idle);
    EXPECT_EQ(h[0].mode, hvac_mode::auto_select);
    EXPECT_TRUE(h[0].sensors[0].present);
    EXPECT_FLOAT_EQ(h[0].sensors[0].temp, 71.5f);
    EXPECT_FALSE(h[0].sensors[1].present);    // never reported
    EXPECT_TRUE(h[0].sensors[2].present);     // sent, but flagged
    EXPECT_FALSE(h[0].sensors[2].trusted);
    ASSERT_TRUE(h[0].weighted_avg);
    EXPECT_FLOAT_EQ(*h[0].weighted_avg, 71.5f);   // untrusted bed left out, as in control
}

TEST_F(Api, HistoryRecordsTheStatusThisPassChoseAndTheModeAsSet) {
    s.settings.mode = hvac_mode::both;
    tick(nodes(68), MIN_OFF_TIME + SEC);   // past boot min-off: heat starts
    EXPECT_EQ(s.history.entries_since(0).back().status, hvac_status::heating);
    EXPECT_EQ(s.history.entries_since(0).back().mode, hvac_mode::both);
}

TEST_F(Api, AwayOnlyOnceClockIsSynced) {
    s.settings.vacations = {.next_id = 1, .blocks = {{0, NOW_UNIX - DAY, NOW_UNIX + 3 * DAY}}};

    tick(nodes(65), MIN_OFF_TIME + SEC, false);   // wall clock unknown: normal set points, heats
    EXPECT_FALSE(s.latest.away);
    EXPECT_EQ(s.control.status, hvac_status::heating);

    tick(nodes(65), MIN_OFF_TIME + MIN_ON_TIME + 2 * SEC, true);   // away: 65 > 62, so stop
    EXPECT_TRUE(s.latest.away);
    EXPECT_EQ(s.control.status, hvac_status::idle);
}

TEST_F(Api, FinishedVacationsKeptUntilClockSynced) {
    s.settings.vacations = {.next_id = 1, .blocks = {{0, 100, 200}}};
    tick(nodes(), MIN, false);
    EXPECT_EQ(s.settings.vacations.blocks.size(), 1u);
    tick(nodes(), 2 * MIN, true);
    EXPECT_TRUE(s.settings.vacations.blocks.empty());
}

// ---- GET /status ----

TEST_F(Api, StatusBeforeFirstPass) {
    api_reply r = get_status(s);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, (json{{"status", "idle"}, {"mode", "auto"}, {"active_mode", "heat"},
                            {"fan", "auto"}, {"away", false}}));
}

TEST_F(Api, StatusShowsModeAsSetAndAsResolved) {
    s.settings.mode = hvac_mode::cool;
    tick(nodes(), MIN);
    EXPECT_EQ(get_status(s).body["mode"], "cool");
    EXPECT_EQ(get_status(s).body["active_mode"], "cool");
}

// ---- GET /readings ----

TEST_F(Api, ReadingsBeforeFirstPassAreNull) {
    EXPECT_TRUE(get_readings(s, "").body["main"].is_null());
    json all = get_readings(s, "all").body;
    EXPECT_TRUE(all["sensors"]["office"].is_null());
    EXPECT_TRUE(all["weighted_avg"].is_null());
}

TEST_F(Api, ReadingsMain) {
    tick(nodes(), MIN);
    api_reply r = get_readings(s, "main");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, (json{{"main", {{"temp", 71.5}, {"humidity", 41.2}, {"co2", 600}, {"trusted", true}}}}));
    EXPECT_EQ(get_readings(s, "").body, r.body);   // main is the default
}

TEST_F(Api, ReadingsAll) {
    tick(nodes(), MIN);
    json body = get_readings(s, "all").body;
    EXPECT_EQ(body["sensors"]["main"]["trusted"], true);
    EXPECT_TRUE(body["sensors"]["office"].is_null());
    EXPECT_EQ(body["sensors"]["bed"]["temp"], 80);
    EXPECT_EQ(body["sensors"]["bed"]["trusted"], false);
    EXPECT_EQ(body["weighted_avg"], 71.5);
}

TEST_F(Api, ReadingsNoTrustedSensorMeansNullAverage) {
    tick({ node("main", 71.5, false) }, MIN);
    EXPECT_TRUE(get_readings(s, "all").body["weighted_avg"].is_null());
}

TEST_F(Api, ReadingsBadView) {
    EXPECT_EQ(get_readings(s, "everything").status, 400);
}

// ---- set points ----

TEST_F(Api, PutSetpointsReturnsThePushedValues) {
    // heat alone up to 75 pushes AC (72) up to keep the dead band
    api_reply r = put_setpoints(s, R"({"heat_point": 75})");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["heat_point"], 75);
    EXPECT_EQ(r.body["ac_point"], 75 + MIN_DEAD_BAND);
    EXPECT_EQ(get_setpoints(s).body, r.body);
}

TEST_F(Api, PutSetpointsRejected) {
    json before = get_setpoints(s).body;
    for (const char* body : {"not json", R"({"heat_point": 95})", R"({"heat": 70})", R"({"mode": "off"})"}) {
        api_reply r = put_setpoints(s, body);
        EXPECT_EQ(r.status, 400) << body;
        EXPECT_TRUE(r.body.contains("error")) << body;
    }
    EXPECT_EQ(get_setpoints(s).body, before);
}

// ---- vacations ----

TEST_F(Api, VacationLifecycle) {
    api_reply added = post_vacation(s, R"({"vacation_start": 1800000000, "vacation_end": 1800600000})");
    EXPECT_EQ(added.status, 201);
    EXPECT_EQ(added.body, (json{{"block_id", 0}, {"vacation_start", 1800000000}, {"vacation_end", 1800600000}}));

    api_reply changed = put_vacation(s, "0", R"({"vacation_end": 1800700000})");
    EXPECT_EQ(changed.status, 200);
    EXPECT_EQ(changed.body["vacation_end"], 1800700000);
    EXPECT_EQ(get_vacations(s).body["blocks"].size(), 1u);

    api_reply removed = delete_vacation(s, "0");
    EXPECT_EQ(removed.status, 204);
    EXPECT_TRUE(removed.body.is_null());
    EXPECT_EQ(get_vacations(s).body, (json{{"blocks", json::array()}}));
}

TEST_F(Api, VacationErrors) {
    EXPECT_EQ(post_vacation(s, "nope").status, 400);
    EXPECT_EQ(post_vacation(s, R"({"vacation_start": 1800600000, "vacation_end": 1800000000})").status, 400);
    post_vacation(s, R"({"vacation_start": 1800000000, "vacation_end": 1800600000})");

    EXPECT_EQ(put_vacation(s, "7", R"({"vacation_end": 1800700000})").status, 404);
    EXPECT_EQ(put_vacation(s, "abc", R"({"vacation_end": 1800700000})").status, 404);
    EXPECT_EQ(put_vacation(s, "0", R"({"vacation_end": 1})").status, 400);   // end before start
    EXPECT_EQ(put_vacation(s, "0", "nope").status, 400);
    EXPECT_EQ(delete_vacation(s, "7").status, 404);
    EXPECT_EQ(delete_vacation(s, "-1").status, 404);
}

// ---- fan ----

TEST_F(Api, FanOnAndAuto) {
    EXPECT_EQ(put_fan(s, R"({"fan": "on"})").body, (json{{"fan", "on"}}));
    EXPECT_EQ(s.control.fan, fan_mode::on);
    EXPECT_EQ(get_status(s).body["fan"], "on");
    EXPECT_EQ(s.control.status, hvac_status::idle);   // relays follow at the next pass

    tick(nodes(), MIN);
    EXPECT_EQ(s.control.status, hvac_status::circulating);

    EXPECT_EQ(put_fan(s, R"({"fan": "auto"})").status, 200);
    EXPECT_EQ(s.control.fan, fan_mode::auto_select);
}

TEST_F(Api, FanRejects) {
    for (const char* body : {"", R"({"fan": "off"})", R"({"fan": true})", R"({"fan": "on", "x": 1})", "[]"})
        EXPECT_EQ(put_fan(s, body).status, 400) << body;
    EXPECT_EQ(s.control.fan, fan_mode::auto_select);
}

// ---- history ----

TEST_F(Api, HistorySince) {
    for (int i = 1; i <= 3; i++) tick(nodes(), i * MIN, true, NOW_UNIX + i * 60);

    EXPECT_EQ(get_history(s, "").body["entries"].size(), 3u);
    json after = get_history(s, std::to_string(NOW_UNIX + 60)).body["entries"];
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[0]["time"], NOW_UNIX + 120);

    json e = after[0];
    EXPECT_EQ(e["status"], "idle");
    EXPECT_EQ(e["mode"], "auto");
    EXPECT_TRUE(e["sensors"]["office"].is_null());
    EXPECT_EQ(e["sensors"]["bed"]["trusted"], false);
    EXPECT_EQ(e["weighted_avg"], 71.5);
}

TEST_F(Api, HistoryBadSince) {
    for (const char* since : {"yesterday", "12abc", "1.5"})
        EXPECT_EQ(get_history(s, since).status, 400) << since;
}

// ---- threads ----

// The control pass and the HTTP handlers on several threads at once. Passing here
// proves little by itself; build with -fsanitize=thread to catch an unlocked access.
TEST_F(Api, WeatherFromTheCurrentHour) {
    long long hour = NOW_UNIX / 3600 * 3600;
    for (int i = -2; i < 30; i++)   // two past hours: kept in the store until the next fetch
        s.settings.weather.hourly.push_back({hour + i * 3600LL, 40.0f + i});
    s.settings.weather.hourly[2].condition = "bkn";
    s.settings.weather.hourly[2].is_day = false;
    s.settings.weather.hourly[2].short_forecast = "Mostly Cloudy";
    s.settings.weather.fetched_at = 123;

    api_reply r = get_weather(s, "", NOW_UNIX + 1800);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["fetched_at"], 123);
    ASSERT_EQ(r.body["hours"].size(), static_cast<size_t>(WEATHER_DEFAULT_HOURS));
    const json& first = r.body["hours"][0];
    EXPECT_EQ(first["time"], hour);   // the hour containing now
    EXPECT_EQ(first["temp"], 40.0);
    EXPECT_EQ(first["condition"], "bkn");
    EXPECT_EQ(first["is_day"], false);
    EXPECT_EQ(first["short_forecast"], "Mostly Cloudy");

    r = get_weather(s, "5", NOW_UNIX);
    ASSERT_EQ(r.body["hours"].size(), 5u);
    EXPECT_EQ(r.body["hours"][4]["time"], hour + 4 * 3600);

    r = get_weather(s, "168", NOW_UNIX);   // asks for more than there is
    EXPECT_EQ(r.body["hours"].size(), 30u);
}

TEST_F(Api, WeatherNeverFetched) {
    api_reply r = get_weather(s, "5", NOW_UNIX);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["fetched_at"], 0);
    EXPECT_TRUE(r.body["hours"].empty());
}

TEST_F(Api, WeatherBadHours) {
    for (const char* bad : {"0", "-1", "169", "5x", "five"}) {
        api_reply r = get_weather(s, bad, NOW_UNIX);
        EXPECT_EQ(r.status, 400) << bad;
        EXPECT_TRUE(r.body.contains("error"));
    }
}

TEST_F(Api, ConcurrentRequestsAndPasses) {
    std::vector<std::thread> clients;
    for (int c = 0; c < 4; c++) {
        clients.emplace_back([this, c] {
            for (int i = 0; i < 200; i++) {
                get_status(s);
                get_readings(s, "all");
                get_history(s, "");
                put_fan(s, i % 2 ? R"({"fan": "on"})" : R"({"fan": "auto"})");
                if (c == 0 && i % 20 == 0)
                    post_vacation(s, R"({"vacation_start": 1900000000, "vacation_end": 1900600000})");
            }
        });
    }
    for (int i = 1; i <= 200; i++) tick(nodes(), i * MIN, true, NOW_UNIX + i * 60);
    for (auto& t : clients) t.join();

    EXPECT_EQ(s.history.size(), 200u);
    EXPECT_EQ(get_vacations(s).body["blocks"].size(), 10u);
}

// ---- who may connect ----

TEST(LocalAddress, LanLoopbackAndTailnet) {
    for (const char* ip : {"127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.20",
                           "100.64.0.1", "100.127.255.255", "::1", "::ffff:192.168.1.20"})
        EXPECT_TRUE(is_local_address(ip)) << ip;
}

TEST(LocalAddress, EverythingElse) {
    for (const char* ip : {"8.8.8.8", "172.32.0.1", "100.128.0.1", "192.169.0.1", "11.0.0.1",
                           "2001:db8::1", "", "garbage"})
        EXPECT_FALSE(is_local_address(ip)) << ip;
}
