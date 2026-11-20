#include <gtest/gtest.h>
#include "control.h"
#include "constants.h"
#include "relay.h"
#include "fake_relay.h"

// TEST_F(Fixture, Name): a fresh Fixture object is built for every test, and
// SetUp() runs first, so relay state from one test can't leak into the next.
class ApplyStatus : public ::testing::Test {
protected:
    void SetUp() override { fake_relay_reset(); }
};

TEST_F(ApplyStatus, RelayFailureLeavesTimersUntouched) {
    long long cycle_start = 100, cycle_end = 200;
    fake_relay_fail_next();

    EXPECT_NE(apply_status(hvac_status::idle, hvac_status::heating, 5000, cycle_start, cycle_end), 0);
    EXPECT_EQ(cycle_start, 100);
    EXPECT_EQ(cycle_end, 200);
    EXPECT_EQ(relay_get(RELAY_HEAT), 0);
}

// a failed all_off() must stop circulating before the fan is turned on
TEST_F(ApplyStatus, CirculatingDoesNotStartFanIfAllOffFails) {
    long long cycle_start = 100, cycle_end = 200;
    fake_relay_fail_next();  // first write in all_off()

    EXPECT_NE(apply_status(hvac_status::heating, hvac_status::circulating, 5000, cycle_start, cycle_end), 0);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);
    EXPECT_EQ(cycle_start, 100);
    EXPECT_EQ(cycle_end, 200);
}

// Which timers each switch stamps: cycle_end when the furnace/AC stops,
// cycle_start for any non-idle status except em_heating -> heating.
// Parameterized with a table: one row per switch.
struct TimerCase {
    const char *name;
    hvac_status from, to;
    bool stamps_start, stamps_end;
};

void PrintTo(const TimerCase& c, std::ostream* os) { *os << c.name; }

class ApplyStatusTimers : public ::testing::TestWithParam<TimerCase> {
protected:
    void SetUp() override { fake_relay_reset(); }
};

TEST_P(ApplyStatusTimers, StampsTheRightTimers) {
    const TimerCase& c = GetParam();
    const long long START = 100, END = 200, NOW = 5000;
    long long cycle_start = START, cycle_end = END;

    ASSERT_EQ(apply_status(c.from, c.to, NOW, cycle_start, cycle_end), 0);
    EXPECT_EQ(cycle_start, c.stamps_start ? NOW : START);
    EXPECT_EQ(cycle_end, c.stamps_end ? NOW : END);
}

INSTANTIATE_TEST_SUITE_P(Switches, ApplyStatusTimers, ::testing::Values(
    //          name                 from                       to                         start  end
    TimerCase{ "IdleToHeating",      hvac_status::idle,        hvac_status::heating,      true,  false },
    TimerCase{ "IdleToCooling",      hvac_status::idle,        hvac_status::cooling,      true,  false },
    TimerCase{ "IdleToCirculating",  hvac_status::idle,        hvac_status::circulating,  true,  false },
    TimerCase{ "HeatingToIdle",      hvac_status::heating,     hvac_status::idle,         false, true  },
    TimerCase{ "CoolingToIdle",      hvac_status::cooling,     hvac_status::idle,         false, true  },
    TimerCase{ "EmHeatingToIdle",    hvac_status::em_heating,  hvac_status::idle,         false, true  },
    TimerCase{ "HeatingToCirc",      hvac_status::heating,     hvac_status::circulating,  true,  true  },
    TimerCase{ "CoolingToCirc",      hvac_status::cooling,     hvac_status::circulating,  true,  true  },
    TimerCase{ "CircToIdle",         hvac_status::circulating, hvac_status::idle,         false, false },
    TimerCase{ "CircToHeating",      hvac_status::circulating, hvac_status::heating,      true,  false },
    TimerCase{ "EmHeatingToHeating", hvac_status::em_heating,  hvac_status::heating,      false, false },
    TimerCase{ "HeatingToEmHeating", hvac_status::heating,     hvac_status::em_heating,   true,  false }
), [](const ::testing::TestParamInfo<TimerCase>& info) { return std::string(info.param.name); });


// ---- test_switch ----

// Runs test_switch + apply_status the way main()'s loop does, so a test reads
// as a sequence of commands and checks the status (and relays) after each one.
class ThermostatLoop : public ::testing::Test {
protected:
    void SetUp() override { fake_relay_reset(); }

    // same starting point as main(): idle, both timers stamped at boot
    long long now = 0;
    long long cycle_start = 0;
    long long cycle_end = 0;
    hvac_status status = hvac_status::idle;

    // wait `wait` ms, then run one loop pass with `command`; returns the new status
    hvac_status run(hvac_command command, long long wait) {
        now += wait;
        hvac_status next = test_switch(status, command, cycle_start, now, cycle_end);
        if (next != status && apply_status(status, next, now, cycle_start, cycle_end) == 0)
            status = next;
        return status;
    }
};

constexpr long long PAST_MIN_ON = MIN_ON_TIME + SEC;
constexpr long long PAST_MIN_OFF = MIN_OFF_TIME + SEC;
constexpr long long WITHIN_MIN_OFF = MIN_OFF_TIME / 2;
constexpr long long PAST_MAX_RUN = MAX_RUN_TIME + SEC;
constexpr long long WITHIN_MIN_ON = MIN_ON_TIME / 2;
constexpr long long ONE_PASS = MIN;  // main()'s 60 s tick

// Heat and cool tests are mirror images, so each one is written once and run
// for both (a value-parameterized test: TEST_P + INSTANTIATE_TEST_SUITE_P).
struct Equipment {
    const char *name;
    hvac_command command;   // asks for it
    hvac_status status;     // runs in it
    relay relay_pin;        // switches it
};
const Equipment HEAT = { "Heat", hvac_command::heat, hvac_status::heating, RELAY_HEAT };
const Equipment COOL = { "Cool", hvac_command::cool, hvac_status::cooling, RELAY_COOL };

// gtest calls this to print a parameter in failure messages
void PrintTo(const Equipment& e, std::ostream* os) { *os << e.name; }

class TestSwitch : public ThermostatLoop, public ::testing::WithParamInterface<Equipment> {
protected:
    const Equipment& e = GetParam();
    const Equipment& other = (e.status == hvac_status::heating) ? COOL : HEAT;
};

// ASSERT on the setup steps: if one fails, the rest of the sequence is meaningless

TEST_P(TestSwitch, RestartsViaIdleAfterMinOff) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);  // boot stamped cycle_end, so wait out min-off
    ASSERT_EQ(run(hvac_command::none, PAST_MIN_ON), hvac_status::idle);
    EXPECT_EQ(relay_get(e.relay_pin), 0);

    EXPECT_EQ(run(e.command, PAST_MIN_OFF), e.status);
    EXPECT_EQ(relay_get(e.relay_pin), 1);
}

TEST_P(TestSwitch, RestartsViaFanAfterMinOff) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);
    ASSERT_EQ(run(hvac_command::fan, PAST_MIN_ON), hvac_status::circulating);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
    EXPECT_EQ(relay_get(RELAY_FAN), 1);

    EXPECT_EQ(run(e.command, PAST_MIN_OFF), e.status);
    EXPECT_EQ(relay_get(e.relay_pin), 1);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);  // fan is off during a heat/cool call
}

TEST_P(TestSwitch, SwitchesToOtherViaIdleAfterMinOff) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);
    ASSERT_EQ(run(hvac_command::none, PAST_MIN_ON), hvac_status::idle);

    EXPECT_EQ(run(other.command, PAST_MIN_OFF), other.status);
    EXPECT_EQ(relay_get(other.relay_pin), 1);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
}

TEST_P(TestSwitch, RestartViaIdleBlockedWithinMinOff) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);
    ASSERT_EQ(run(hvac_command::none, PAST_MIN_ON), hvac_status::idle);

    EXPECT_EQ(run(e.command, WITHIN_MIN_OFF), hvac_status::idle);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
}

TEST_P(TestSwitch, RestartViaFanBlockedWithinMinOff) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);
    ASSERT_EQ(run(hvac_command::fan, PAST_MIN_ON), hvac_status::circulating);

    EXPECT_EQ(run(e.command, WITHIN_MIN_OFF), hvac_status::circulating);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
}

TEST_P(TestSwitch, TurnsOffAfterMaxRun) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(e.command, PAST_MAX_RUN), hvac_status::idle);  // still asking, but ran too long
    EXPECT_EQ(relay_get(e.relay_pin), 0);
}

TEST_P(TestSwitch, MinOnBlocksIdle) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(hvac_command::none, WITHIN_MIN_ON), e.status);
    EXPECT_EQ(relay_get(e.relay_pin), 1);
    EXPECT_EQ(run(hvac_command::none, MIN_ON_TIME), hvac_status::idle);  // now past min-on
}

// a fan call never cuts a run short: it finishes min-on, then lands on
// circulating instead of idle
TEST_P(TestSwitch, FanCallWaitsOutMinOnThenCirculates) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(hvac_command::fan, WITHIN_MIN_ON), e.status);
    EXPECT_EQ(relay_get(e.relay_pin), 1);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);

    EXPECT_EQ(run(hvac_command::fan, MIN_ON_TIME), hvac_status::circulating);  // now past min-on
    EXPECT_EQ(relay_get(e.relay_pin), 0);
    EXPECT_EQ(relay_get(RELAY_FAN), 1);
}

// max-run with the fan called goes straight to circulating, not idle
TEST_P(TestSwitch, MaxRunWithFanCallCirculates) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(hvac_command::fan, PAST_MAX_RUN), hvac_status::circulating);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
    EXPECT_EQ(relay_get(RELAY_FAN), 1);
}

// no sensor data acts like "none": finish min-on, then idle
TEST_P(TestSwitch, NoDataGoesIdleAfterMinOn) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(hvac_command::no_data, WITHIN_MIN_ON), e.status);
    EXPECT_EQ(run(hvac_command::no_data, MIN_ON_TIME), hvac_status::idle);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
}

// by design: a conflicting call stops the equipment at once, without waiting
// out min-on; the other side then waits out min-off like any restart
TEST_P(TestSwitch, ConflictingCallGoesIdleThenWaitsMinOff) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(other.command, WITHIN_MIN_ON), hvac_status::idle);
    EXPECT_EQ(relay_get(e.relay_pin), 0);
    EXPECT_EQ(run(other.command, WITHIN_MIN_OFF), hvac_status::idle);
    EXPECT_EQ(run(other.command, MIN_OFF_TIME), other.status);  // now past min-off
    EXPECT_EQ(relay_get(other.relay_pin), 1);
}

// runs every TEST_P above once with HEAT and once with COOL
// (names like HeatAndCool/TestSwitch.TurnsOffAfterMaxRun/Heat)
INSTANTIATE_TEST_SUITE_P(HeatAndCool, TestSwitch, ::testing::Values(HEAT, COOL),
                         [](const ::testing::TestParamInfo<Equipment>& info) { return std::string(info.param.name); });

// ---- emergency heat ----

class EmergencyHeat : public ThermostatLoop {};

TEST_F(EmergencyHeat, StartsFromIdleWithoutMinOff) {
    EXPECT_EQ(run(hvac_command::emergency_heat, SEC), hvac_status::em_heating);  // right after boot
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

TEST_F(EmergencyHeat, OverridesHeatingWithoutMinOn) {
    ASSERT_EQ(run(hvac_command::heat, PAST_MIN_OFF), hvac_status::heating);

    EXPECT_EQ(run(hvac_command::emergency_heat, WITHIN_MIN_ON), hvac_status::em_heating);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

// em_heating -> heating keeps cycle_start, so max-run counts from when the furnace started
TEST_F(EmergencyHeat, DropToHeatingKeepsMaxRunClock) {
    ASSERT_EQ(run(hvac_command::emergency_heat, SEC), hvac_status::em_heating);
    ASSERT_EQ(run(hvac_command::heat, PAST_MIN_ON), hvac_status::heating);

    // furnace has now run MAX_RUN_TIME + 1 s in total, but only ~115 min as "heating"
    EXPECT_EQ(run(hvac_command::heat, MAX_RUN_TIME - MIN_ON_TIME), hvac_status::idle);
    EXPECT_EQ(relay_get(RELAY_HEAT), 0);
}

// by design: em heat has no max-run; it runs as long as it's called for
TEST_F(EmergencyHeat, RunsPastMaxRun) {
    ASSERT_EQ(run(hvac_command::emergency_heat, SEC), hvac_status::em_heating);

    EXPECT_EQ(run(hvac_command::emergency_heat, PAST_MAX_RUN), hvac_status::em_heating);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

// Cooling -> em heat is only reachable with ac_set < MIN_HEAT, which setpoint
// validation blocks. If it happens anyway: cool drops, heat starts, never both on.
TEST_F(EmergencyHeat, FromCoolingNeverClosesBothRelays) {
    ASSERT_EQ(run(hvac_command::cool, PAST_MIN_OFF), hvac_status::cooling);

    EXPECT_EQ(run(hvac_command::emergency_heat, WITHIN_MIN_ON), hvac_status::em_heating);
    EXPECT_EQ(relay_get(RELAY_COOL), 0);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

// the one exception to circulating's 5 min minimum
TEST_F(EmergencyHeat, OverridesCirculatingWithoutMinOn) {
    ASSERT_EQ(run(hvac_command::fan, SEC), hvac_status::circulating);

    EXPECT_EQ(run(hvac_command::emergency_heat, SEC), hvac_status::em_heating);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

// ---- fan only ----

class FanOnly : public ThermostatLoop {};

TEST_F(FanOnly, StartsFromIdleWithoutMinOff) {
    EXPECT_EQ(run(hvac_command::fan, SEC), hvac_status::circulating);  // right after boot
    EXPECT_EQ(relay_get(RELAY_FAN), 1);
}

TEST_F(FanOnly, MinOnBlocksIdle) {
    ASSERT_EQ(run(hvac_command::fan, SEC), hvac_status::circulating);

    EXPECT_EQ(run(hvac_command::none, WITHIN_MIN_ON), hvac_status::circulating);
    EXPECT_EQ(run(hvac_command::none, MIN_ON_TIME), hvac_status::idle);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);
}

// circulating runs its full min-on before heat/cool takes over (min-off long past here)
TEST_F(FanOnly, MinOnBlocksHeat) {
    ASSERT_EQ(run(hvac_command::fan, PAST_MIN_OFF), hvac_status::circulating);

    EXPECT_EQ(run(hvac_command::heat, WITHIN_MIN_ON), hvac_status::circulating);
    EXPECT_EQ(run(hvac_command::heat, MIN_ON_TIME), hvac_status::heating);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

// circulating longer than MAX_RUN_TIME must not trip heat's max-run when heat is called
// (the old rule sent it to idle for a pass, because cycle_start was the fan's start)
TEST_F(FanOnly, LongCirculationThenHeatStartsHeating) {
    ASSERT_EQ(run(hvac_command::fan, PAST_MIN_OFF), hvac_status::circulating);
    ASSERT_EQ(run(hvac_command::fan, PAST_MAX_RUN), hvac_status::circulating);

    EXPECT_EQ(run(hvac_command::heat, ONE_PASS), hvac_status::heating);
}

// by design: the fan has no max-run (this also closed the one-pass flip-flop at max-run)
TEST_F(FanOnly, RunsPastMaxRun) {
    ASSERT_EQ(run(hvac_command::fan, SEC), hvac_status::circulating);

    EXPECT_EQ(run(hvac_command::fan, PAST_MAX_RUN), hvac_status::circulating);
    EXPECT_EQ(run(hvac_command::fan, ONE_PASS), hvac_status::circulating);
    EXPECT_EQ(relay_get(RELAY_FAN), 1);
}

// ---- get_HVAC_command ----

// Sensors default to weight_day == weight_night; night is passed in, so nothing
// here reads the clock. Defaults: day, both modes allowed, outside temp unknown.
class HvacCommand : public ::testing::Test {
protected:
    const float HEAT_SET = 70;
    const float AC_SET = 73;
    const hvac_set_points POINTS = {.heat_point = HEAT_SET, .ac_point = AC_SET, .night_difference = 2};

    static sensor_node sensor(float temp, bool trusted = true, int weight = 1) {
        sensor_node n = { .name = "s", .weight_day = weight, .weight_night = weight };
        n.data.temp = temp;
        n.data.read_ok = 1;
        n.health.trusted = trusted;
        return n;
    }

    hvac_command command(const std::vector<sensor_node>& sensors, bool night = false,
                         hvac_mode allowed = hvac_mode::both, std::optional<float> outside = std::nullopt,
                         fan_mode fan = fan_mode::auto_select, bool co2_fan = false) {
        return get_HVAC_command(sensors, POINTS, night, allowed, outside, fan, co2_fan);
    }

    hvac_command with_fan(const std::vector<sensor_node>& sensors, fan_mode fan, bool co2_fan = false) {
        return command(sensors, false, hvac_mode::both, std::nullopt, fan, co2_fan);
    }
};

TEST_F(HvacCommand, CoolAboveAcSet) {
    EXPECT_EQ(command({ sensor(75) }), hvac_command::cool);
}

TEST_F(HvacCommand, HeatBelowHeatSet) {
    EXPECT_EQ(command({ sensor(68) }), hvac_command::heat);
}

TEST_F(HvacCommand, EmergencyHeatBelowMinHeat) {
    EXPECT_EQ(command({ sensor(MIN_HEAT - 5) }), hvac_command::emergency_heat);
}

TEST_F(HvacCommand, NoneInsideDeadband) {
    EXPECT_EQ(command({ sensor(71.5) }), hvac_command::none);
}

TEST_F(HvacCommand, FanWhenSpreadExceedsTrigger) {
    // average 71.5 is inside the deadband; spread 3 > FAN_TRIGGER
    EXPECT_EQ(command({ sensor(70), sensor(73) }), hvac_command::fan);
}

TEST_F(HvacCommand, NoFanWhenSpreadUnderTrigger) {
    EXPECT_EQ(command({ sensor(71), sensor(72) }), hvac_command::none);
}

TEST_F(HvacCommand, NoDataWithoutSensors) {
    EXPECT_EQ(command({}), hvac_command::no_data);
}

TEST_F(HvacCommand, NoDataWhenNoSensorTrusted) {
    EXPECT_EQ(command({ sensor(75, false), sensor(65, false) }), hvac_command::no_data);
}

TEST_F(HvacCommand, AverageIsWeighted) {
    // plain average 74 would call cool; weighted (68*3 + 80*1) / 4 = 71 is inside
    // the deadband, so the 12 F spread calls fan
    EXPECT_EQ(command({ sensor(68, true, 3), sensor(80, true, 1) }), hvac_command::fan);
}

TEST_F(HvacCommand, UntrustedSensorLeftOutOfAverage) {
    EXPECT_EQ(command({ sensor(71.5), sensor(90, false) }), hvac_command::none);
}

TEST_F(HvacCommand, UntrustedSensorLeftOutOfSpread) {
    // 71.5 and 74.5 would average 73 (no cool) but spread 3 (fan)
    EXPECT_EQ(command({ sensor(71.5), sensor(74.5, false) }), hvac_command::none);
}

// current behavior: cool is checked first, so it wins over emergency heat.
// Only reachable with ac_set < MIN_HEAT, which setpoint validation should block.
TEST_F(HvacCommand, CoolCheckedBeforeEmergencyHeat) {
    hvac_set_points p = {.heat_point = 50, .ac_point = MIN_HEAT - 5, .night_difference = 0};
    EXPECT_EQ(get_HVAC_command({ sensor(MIN_HEAT - 2) }, p, false, hvac_mode::both, std::nullopt,
                               fan_mode::auto_select, false), hvac_command::cool);
}

// by design: weight 0 means "ignore this sensor", so if it's the only one, there's no data
TEST_F(HvacCommand, OnlyZeroWeightSensorIsNoData) {
    EXPECT_EQ(command({ sensor(75, true, 0) }), hvac_command::no_data);
}

// ---- night ----

TEST_F(HvacCommand, NightDropsWholeBand) {
    // day 70-73; night 68-71
    EXPECT_EQ(command({ sensor(69) }, false), hvac_command::heat);
    EXPECT_EQ(command({ sensor(69) }, true), hvac_command::none);
    EXPECT_EQ(command({ sensor(72) }, false), hvac_command::none);
    EXPECT_EQ(command({ sensor(72) }, true), hvac_command::cool);
}

TEST_F(HvacCommand, NightUsesNightWeights) {
    sensor_node main = sensor(68), bed = sensor(74);
    main.weight_day = 3; main.weight_night = 1;
    bed.weight_day = 1;  bed.weight_night = 3;
    // day (68*3 + 74) / 4 = 69.5 -> heat; night (68 + 74*3) / 4 = 72.5 > night AC 71 -> cool
    EXPECT_EQ(command({ main, bed }, false), hvac_command::heat);
    EXPECT_EQ(command({ main, bed }, true), hvac_command::cool);
}

// ---- mode (manual or from the season) ----

TEST_F(HvacCommand, HeatModeBlocksCooling) {
    EXPECT_EQ(command({ sensor(75) }, false, hvac_mode::heat), hvac_command::none);
    EXPECT_EQ(command({ sensor(68) }, false, hvac_mode::heat), hvac_command::heat);
}

TEST_F(HvacCommand, CoolModeBlocksHeating) {
    EXPECT_EQ(command({ sensor(68) }, false, hvac_mode::cool), hvac_command::none);
    EXPECT_EQ(command({ sensor(75) }, false, hvac_mode::cool), hvac_command::cool);
}

TEST_F(HvacCommand, EmergencyHeatIgnoresMode) {
    EXPECT_EQ(command({ sensor(MIN_HEAT - 1) }, false, hvac_mode::cool), hvac_command::emergency_heat);
}

TEST_F(HvacCommand, BlockedCallStillAllowsFan) {
    // too warm but cooling locked out; spread 3 still circulates
    EXPECT_EQ(command({ sensor(73), sensor(76) }, false, hvac_mode::heat), hvac_command::fan);
}

// ---- weather gate ----

TEST_F(HvacCommand, NoCoolingWhenOutsideIsCool) {
    // AC 73: blocked at outside <= 68
    EXPECT_EQ(command({ sensor(75) }, false, hvac_mode::both, 68), hvac_command::none);
    EXPECT_EQ(command({ sensor(75) }, false, hvac_mode::both, 68.5), hvac_command::cool);
}

TEST_F(HvacCommand, NoHeatingWhenOutsideIsWarm) {
    // heat 70: blocked at outside >= 75
    EXPECT_EQ(command({ sensor(68) }, false, hvac_mode::both, 75), hvac_command::none);
    EXPECT_EQ(command({ sensor(68) }, false, hvac_mode::both, 74.5), hvac_command::heat);
}

TEST_F(HvacCommand, GateUsesNightPoints) {
    // night AC 71: blocked at outside <= 66; 67 would be blocked by day (<= 68) but not at night
    EXPECT_EQ(command({ sensor(75) }, false, hvac_mode::both, 67), hvac_command::none);
    EXPECT_EQ(command({ sensor(75) }, true, hvac_mode::both, 67), hvac_command::cool);
}

TEST_F(HvacCommand, UnknownOutsideSkipsGate) {
    EXPECT_EQ(command({ sensor(75) }, false, hvac_mode::both, std::nullopt), hvac_command::cool);
}

TEST_F(HvacCommand, EmergencyHeatIgnoresWeather) {
    EXPECT_EQ(command({ sensor(MIN_HEAT - 1) }, false, hvac_mode::both, 100), hvac_command::emergency_heat);
}

// ---- away ----

TEST_F(HvacCommand, AwayHeatsOnlyBelow62NoFanNoNightShift) {
    auto away = [&](std::vector<sensor_node> s, bool night) {
        return get_HVAC_command(s, AWAY_POINTS, night, hvac_mode::both, std::nullopt,
                                fan_mode::auto_select, false);
    };
    EXPECT_EQ(away({ sensor(61) }, false), hvac_command::heat);
    EXPECT_EQ(away({ sensor(61) }, true), hvac_command::heat);
    EXPECT_EQ(away({ sensor(63) }, false), hvac_command::none);
    EXPECT_EQ(away({ sensor(85) }, false), hvac_command::none);
    EXPECT_EQ(away({ sensor(64), sensor(80) }, false), hvac_command::none);   // spread, but no fan
    EXPECT_EQ(away({ sensor(MIN_HEAT - 1) }, false), hvac_command::emergency_heat);
}


// ---- manual fan and CO2 calls (inputs to get_HVAC_command) ----

TEST_F(HvacCommand, ManualFanWhenNothingElseCalled) {
    EXPECT_EQ(with_fan({ sensor(71.5) }, fan_mode::on), hvac_command::fan);
    EXPECT_EQ(with_fan({ sensor(71.5) }, fan_mode::auto_select), hvac_command::none);
}

TEST_F(HvacCommand, HeatCoolAndEmHeatBeatManualFan) {
    EXPECT_EQ(with_fan({ sensor(68) }, fan_mode::on), hvac_command::heat);
    EXPECT_EQ(with_fan({ sensor(75) }, fan_mode::on), hvac_command::cool);
    EXPECT_EQ(with_fan({ sensor(MIN_HEAT - 1) }, fan_mode::on), hvac_command::emergency_heat);
}

// the manual fan doesn't need temperatures
TEST_F(HvacCommand, ManualFanBeatsNoData) {
    EXPECT_EQ(with_fan({}, fan_mode::on), hvac_command::fan);
    EXPECT_EQ(with_fan({ sensor(71.5, false) }, fan_mode::on), hvac_command::fan);
}

TEST_F(HvacCommand, Co2CallCirculates) {
    EXPECT_EQ(with_fan({ sensor(71.5) }, fan_mode::auto_select, true), hvac_command::fan);
    EXPECT_EQ(with_fan({ sensor(68) }, fan_mode::auto_select, true), hvac_command::heat);
}

TEST_F(HvacCommand, AwayHoldsOffManualAndCo2Fan) {
    auto away = [&](std::vector<sensor_node> s, fan_mode fan, bool co2) {
        return get_HVAC_command(s, AWAY_POINTS, false, hvac_mode::both, std::nullopt, fan, co2);
    };
    EXPECT_EQ(away({ sensor(65) }, fan_mode::on, false), hvac_command::none);
    EXPECT_EQ(away({ sensor(65) }, fan_mode::auto_select, true), hvac_command::none);
    EXPECT_EQ(away({}, fan_mode::on, false), hvac_command::no_data);
}

// ---- co2_fan_call ----

class Co2Fan : public ::testing::Test {
protected:
    static sensor_node room(int co2, bool trusted = true, int weight = 1) {
        sensor_node n = { .name = "s", .weight_day = weight, .weight_night = weight };
        n.data.temp = 71;
        n.data.CO2 = co2;
        n.data.read_ok = 1;
        n.health.trusted = trusted;
        return n;
    }
};

TEST_F(Co2Fan, StartsWhenOneRoomIsHighAndAboveTheOthers) {
    // average 833, start line 1000: 1300 is over both
    EXPECT_TRUE(co2_fan_call({ room(600), room(600), room(1300) }, false, false));
}

TEST_F(Co2Fan, NeedsTheTriggerToStart) {
    // 950 is well over 1.2 x the average (583) but under CO2_TRIGGER
    EXPECT_FALSE(co2_fan_call({ room(400), room(400), room(950) }, false, false));
}

TEST_F(Co2Fan, NoStartWhenEveryRoomIsHigh) {
    // all over 1000, but circulating can't help when the rooms don't differ
    EXPECT_FALSE(co2_fan_call({ room(1100), room(1100), room(1150) }, false, false));
}

// average 865: stop line 951, start line 1038. 995 is between them.
TEST_F(Co2Fan, BetweenTheLinesKeepsWhateverItWasDoing) {
    std::vector<sensor_node> rooms = { room(800), room(800), room(995) };
    EXPECT_FALSE(co2_fan_call(rooms, false, false));  // not running: needs the start line
    EXPECT_TRUE(co2_fan_call(rooms, false, true));    // running: under 1000 doesn't stop it
}

TEST_F(Co2Fan, StopsOnceTheRoomsEvenOut) {
    // average 833, stop line 917
    EXPECT_FALSE(co2_fan_call({ room(800), room(800), room(900) }, false, true));
}

// with the satellites missing, the other rooms count as CO2_FILL_PPM
TEST_F(Co2Fan, MissingRoomsCountAsFill) {
    // (1100 + 700 + 700) / 3 = 833, start line 1000
    EXPECT_TRUE(co2_fan_call({ room(1100), room(0, false), room(0, false) }, false, false));
}

TEST_F(Co2Fan, UntrustedRoomCantTrigger) {
    EXPECT_FALSE(co2_fan_call({ room(600), room(5000, false) }, false, false));
}

TEST_F(Co2Fan, NoTrustedRoomNoFan) {
    EXPECT_FALSE(co2_fan_call({ room(5000, false) }, false, true));
    EXPECT_FALSE(co2_fan_call({}, false, true));
}

TEST_F(Co2Fan, UsesNightWeights) {
    sensor_node main = room(600), bed = room(1150);
    main.weight_day = 1; main.weight_night = 1;
    bed.weight_day = 1;  bed.weight_night = 3;
    // day: average 875, start line 1050 -> on. night: average 1012, start line 1215 -> off
    EXPECT_TRUE(co2_fan_call({ main, bed }, false, false));
    EXPECT_FALSE(co2_fan_call({ main, bed }, true, false));
}

// ---- control_step: a whole pass, with the state carried between passes ----

class ControlStep : public ::testing::Test {
protected:
    void SetUp() override { fake_relay_reset(); }

    const hvac_set_points POINTS = {.heat_point = 70, .ac_point = 73, .night_difference = 2};
    control_state state = initial_control_state(0);
    long long now = 0;

    static sensor_node room(float temp, int co2 = 600) {
        sensor_node n = { .name = "s", .weight_day = 1, .weight_night = 1 };
        n.data.temp = temp;
        n.data.CO2 = co2;
        n.data.read_ok = 1;
        n.health.trusted = true;
        return n;
    }

    hvac_status step(const std::vector<sensor_node>& rooms, long long wait,
                     const hvac_set_points& points) {
        now += wait;
        control_step(state, rooms, points, false, hvac_mode::both, std::nullopt, now);
        return state.status;
    }
    hvac_status step(const std::vector<sensor_node>& rooms, long long wait) {
        return step(rooms, wait, POINTS);
    }
};

TEST_F(ControlStep, ManualFanReturnsToCirculatingAfterHeat) {
    state.fan = fan_mode::on;
    ASSERT_EQ(step({ room(68) }, PAST_MIN_OFF), hvac_status::heating);

    EXPECT_EQ(step({ room(71.5) }, PAST_MIN_ON), hvac_status::circulating);
    EXPECT_EQ(relay_get(RELAY_HEAT), 0);
    EXPECT_EQ(relay_get(RELAY_FAN), 1);
}

TEST_F(ControlStep, FanAutoAfterManualWaitsOutMinOn) {
    state.fan = fan_mode::on;
    ASSERT_EQ(step({ room(71.5) }, SEC), hvac_status::circulating);

    state.fan = fan_mode::auto_select;
    EXPECT_EQ(step({ room(71.5) }, WITHIN_MIN_ON), hvac_status::circulating);
    EXPECT_EQ(step({ room(71.5) }, MIN_ON_TIME), hvac_status::idle);
}

// away holds the manual fan off without forgetting it
TEST_F(ControlStep, AwayHoldsOffManualFanThenItComesBack) {
    state.fan = fan_mode::on;
    EXPECT_EQ(step({ room(65) }, SEC, AWAY_POINTS), hvac_status::idle);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);
    EXPECT_EQ(state.fan, fan_mode::on);

    EXPECT_EQ(step({ room(71.5) }, ONE_PASS), hvac_status::circulating);
}

TEST_F(ControlStep, Co2FanRunsUntilTheRoomsEvenOut) {
    // average 833, start 1000: on
    ASSERT_EQ(step({ room(71.5), room(71.5), room(71.5, 1300) }, SEC), hvac_status::circulating);
    // average 900, start 1080 / stop 990: 1100 keeps it going
    EXPECT_EQ(step({ room(71.5, 800), room(71.5, 800), room(71.5, 1100) }, PAST_MIN_ON), hvac_status::circulating);
    // average 833, stop 917: off
    EXPECT_EQ(step({ room(71.5, 800), room(71.5, 800), room(71.5, 900) }, ONE_PASS), hvac_status::idle);
    EXPECT_FALSE(state.co2_circulating);
}

TEST_F(ControlStep, AwayClearsCo2Memory) {
    ASSERT_EQ(step({ room(71.5), room(71.5), room(71.5, 1300) }, SEC), hvac_status::circulating);
    step({ room(71.5), room(71.5), room(71.5, 1300) }, ONE_PASS, AWAY_POINTS);
    EXPECT_FALSE(state.co2_circulating);
}

TEST_F(ControlStep, FailedRelaySwitchKeepsStatusForARetry) {
    fake_relay_fail_next();
    EXPECT_EQ(step({ room(68) }, PAST_MIN_OFF), hvac_status::idle);
    EXPECT_EQ(step({ room(68) }, ONE_PASS), hvac_status::heating);
}
