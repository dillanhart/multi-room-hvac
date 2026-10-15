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
constexpr long long ONE_PASS = 10 * SEC;  // main()'s sleep(10)

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

// by design: a fan call bypasses min-on and takes over at once
TEST_P(TestSwitch, FanSkipsMinOn) {
    ASSERT_EQ(run(e.command, PAST_MIN_OFF), e.status);

    EXPECT_EQ(run(hvac_command::fan, WITHIN_MIN_ON), hvac_status::circulating);
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

// Cooling -> em heat is left to setpoint validation (log.md); whatever happens,
// heat and cool must never be on together
TEST_F(EmergencyHeat, FromCoolingNeverClosesBothRelays) {
    ASSERT_EQ(run(hvac_command::cool, PAST_MIN_OFF), hvac_status::cooling);

    run(hvac_command::emergency_heat, WITHIN_MIN_ON);
    EXPECT_FALSE(relay_get(RELAY_HEAT) && relay_get(RELAY_COOL));
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

// by design: the fan has no max-run (this also closed the one-pass flip-flop at max-run)
TEST_F(FanOnly, RunsPastMaxRun) {
    ASSERT_EQ(run(hvac_command::fan, SEC), hvac_status::circulating);

    EXPECT_EQ(run(hvac_command::fan, PAST_MAX_RUN), hvac_status::circulating);
    EXPECT_EQ(run(hvac_command::fan, ONE_PASS), hvac_status::circulating);
    EXPECT_EQ(relay_get(RELAY_FAN), 1);
}

// ---- get_HVAC_command ----

// Every sensor here has weight_day == weight_night, so hour_of_day() (which reads
// the wall clock) can't change a result. Day/night weighting waits on that fix.
class HvacCommand : public ::testing::Test {
protected:
    const float HEAT_SET = 70;
    const float AC_SET = 73;

    static sensor_node sensor(float temp, bool trusted = true, int weight = 1) {
        sensor_node n = { .name = "s", .weight_day = weight, .weight_night = weight };
        n.data.temp = temp;
        n.data.read_ok = 1;
        n.health.trusted = trusted;
        return n;
    }

    hvac_command command(const std::vector<sensor_node>& sensors) {
        return get_HVAC_command(sensors, HEAT_SET, AC_SET);
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
    EXPECT_EQ(get_HVAC_command({ sensor(MIN_HEAT - 2) }, 50, MIN_HEAT - 5), hvac_command::cool);
}

// by design: weight 0 means "ignore this sensor", so if it's the only one, there's no data
TEST_F(HvacCommand, OnlyZeroWeightSensorIsNoData) {
    EXPECT_EQ(command({ sensor(75, true, 0) }), hvac_command::no_data);
}
