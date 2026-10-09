#include <gtest/gtest.h>
#include "validation.h"

// TEST(SuiteName, TestName): one independent check. EXPECT_* records a failure
// and keeps going; ASSERT_* stops this test (use it when later lines would crash).

// ---- local_sensor_status ----

TEST(LocalSensorStatus, FailedReadIsError) {
    sensor_result reading = {};
    reading.read_ok = 0;
    EXPECT_EQ(local_sensor_status(reading, 0, 10 * MIN), sensor_status::error);
}

TEST(LocalSensorStatus, WarmingUntilWarmupPasses) {
    sensor_result reading = {};
    reading.read_ok = 1;
    EXPECT_EQ(local_sensor_status(reading, 0, SENSOR_WARMUP - SEC), sensor_status::warming);
    EXPECT_EQ(local_sensor_status(reading, 0, SENSOR_WARMUP), sensor_status::ok);
}

// ---- update_node ----

constexpr long long ONE_PASS = 10 * SEC;  // main()'s sleep(10)
constexpr float ROOM = 70;

// Feeds readings to one node the way main()'s loop does, one pass at a time.
class SensorHealth : public ::testing::Test {
protected:
    sensor_node node = { .name = "test", .weight_day = 1, .weight_night = 1 };
    long long now = 0;
    const sensor_health& h = node.health;

    // wait `wait` ms, then apply a successful read of `temp`
    void good(float temp = ROOM, long long wait = ONE_PASS) {
        now += wait;
        sensor_result r = { .temp = temp, .hum = 40, .CO2 = 600, .read_ok = 1 };
        update_node(node, r, sensor_status::ok, now);
    }

    // wait `wait` ms, then apply a failed read
    void error(long long wait = ONE_PASS) {
        now += wait;
        sensor_result r = {};
        update_node(node, r, sensor_status::error, now);
    }

    void warming(float temp = ROOM) {
        now += ONE_PASS;
        sensor_result r = { .temp = temp, .hum = 40, .CO2 = 600, .read_ok = 1 };
        update_node(node, r, sensor_status::warming, now);
    }

    // CONSISTENT_READS good reads at ROOM: the normal path to trusted
    void become_trusted() {
        for (int i = 0; i < CONSISTENT_READS; i++)
            good();
    }
};

TEST_F(SensorHealth, TrustedOnlyAfterConsistentReads) {
    for (int i = 0; i < CONSISTENT_READS - 1; i++) {
        good();
        EXPECT_FALSE(h.trusted) << "after good read " << i + 1;
    }
    good();
    EXPECT_TRUE(h.trusted);
}

TEST_F(SensorHealth, ErrorBeforeAnyReadIsNotTrusted) {
    error();
    EXPECT_FALSE(h.trusted);
    EXPECT_EQ(h.fails_in_row, 1);
}

TEST_F(SensorHealth, WarmingIsNeverTrusted) {
    for (int i = 0; i < CONSISTENT_READS + 2; i++)
        warming();
    EXPECT_FALSE(h.trusted);
}

TEST_F(SensorHealth, WarmingDropsTrust) {
    become_trusted();
    warming();
    EXPECT_FALSE(h.trusted);
}

// two reads with the same timestamp: the rate check must not divide by zero
TEST_F(SensorHealth, SameTimestampReadKeepsTrust) {
    become_trusted();
    good(ROOM, 0);
    EXPECT_TRUE(h.trusted);
    EXPECT_EQ(h.consistent, CONSISTENT_READS + 1);
}

TEST_F(SensorHealth, WarmingResetsTheCount) {
    good();
    good();
    warming();
    good();
    good();
    EXPECT_FALSE(h.trusted);  // only 2 in a row since warming
    good();
    EXPECT_TRUE(h.trusted);
}

// a short blip: keep trusting the last good reading
TEST_F(SensorHealth, ShortErrorBlipKeepsTrust) {
    become_trusted();

    error();
    EXPECT_TRUE(h.trusted) << "during the blip";
    EXPECT_FLOAT_EQ(node.data.temp, ROOM) << "last good reading is kept";

    good();
    EXPECT_TRUE(h.trusted) << "first good read after the blip";
}

TEST_F(SensorHealth, OutageLongerThanMaxAgeDropsTrust) {
    become_trusted();

    error();
    ASSERT_TRUE(h.trusted);
    error(MAX_READING_AGE);  // last good read is now older than MAX_READING_AGE
    EXPECT_FALSE(h.trusted);
}

// the old bug: trusted -> outage -> trusted again on the first good read
TEST_F(SensorHealth, RegainsTrustOnlyAfterConsistentReadsFollowingOutage) {
    become_trusted();
    error(MAX_READING_AGE + SEC);
    ASSERT_FALSE(h.trusted);

    for (int i = 0; i < CONSISTENT_READS - 1; i++) {
        good();
        EXPECT_FALSE(h.trusted) << "after good read " << i + 1;
    }
    good();
    EXPECT_TRUE(h.trusted);
}

TEST_F(SensorHealth, FailsInRowCountsAndResets) {
    become_trusted();
    error();
    error();
    EXPECT_EQ(h.fails_in_row, 2);
    good();
    EXPECT_EQ(h.fails_in_row, 0);
}

TEST_F(SensorHealth, OutOfRangeDropsTrust) {
    become_trusted();

    good(MIN_VALID_TEMP - 1, ONE_PASS * 10);  // far enough apart that the rate check passes
    EXPECT_FALSE(h.trusted);
}

TEST_F(SensorHealth, OutOfRangeNeverTrusted) {
    for (int i = 0; i < CONSISTENT_READS + 2; i++)
        good(MAX_VALID_TEMP + 1);
    EXPECT_FALSE(h.trusted);
}

// a jump faster than MAX_TEMP_RATE is rejected, then the new level is accepted
// after CONSISTENT_READS steady reads (a real change doesn't lock the sensor out)
TEST_F(SensorHealth, JumpDropsTrustThenNewLevelIsAccepted) {
    become_trusted();

    good(ROOM + 5);  // 5 F in 10 s = 30 F/min
    EXPECT_FALSE(h.trusted);

    for (int i = 0; i < CONSISTENT_READS - 1; i++) {
        good(ROOM + 5);
        EXPECT_FALSE(h.trusted) << "after steady read " << i + 1;
    }
    good(ROOM + 5);
    EXPECT_TRUE(h.trusted);
}

TEST_F(SensorHealth, SlowChangeStaysTrusted) {
    become_trusted();
    for (int i = 1; i <= 6; i++) {
        good(ROOM + i * 0.5f);  // 0.5 F per 10 s = 3 F/min, under MAX_TEMP_RATE
        EXPECT_TRUE(h.trusted) << "at " << ROOM + i * 0.5f << " F";
    }
}
