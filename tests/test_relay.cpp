// Tests the real pi/relay.cpp (interlock, pin setup, all_off) against fake_gpiod.cpp.
// Built as its own binary: relay.cpp and fake_relay.cpp define the same functions.

#include <gtest/gtest.h>
#include "relay.h"
#include "fake_gpiod.h"

// BCM numbers the relays are wired to (relay.cpp's PIN_NUM_*)
constexpr unsigned int PIN_HEAT = 4;
constexpr unsigned int PIN_COOL = 5;
constexpr unsigned int PIN_FAN = 6;

// ---- relays_init ----

TEST(RelayInit, ClaimsPinsAsActiveLowOutputsStartingOff) {
    fake_gpiod_reset();
    ASSERT_EQ(relays_init(), 0);

    for (unsigned int pin : { PIN_HEAT, PIN_COOL, PIN_FAN }) {
        const fake_line& line = fake_gpiod_line(pin);
        EXPECT_TRUE(line.requested) << "GPIO" << pin;
        EXPECT_EQ(line.direction, GPIOD_LINE_DIRECTION_OUTPUT) << "GPIO" << pin;
        EXPECT_TRUE(line.active_low) << "GPIO" << pin;
        EXPECT_EQ(line.value, GPIOD_LINE_VALUE_INACTIVE) << "GPIO" << pin;
    }
    relays_close();
}

TEST(RelayInit, FailsCleanlyWhenChipWontOpen) {
    fake_gpiod_reset();
    fake_gpiod_fail_chip_open();

    EXPECT_EQ(relays_init(), -1);
    EXPECT_FALSE(fake_gpiod_request_held());
    EXPECT_EQ(relay_set(RELAY_FAN, 1), -1);  // nothing claimed, so nothing can be set
}

// ---- relay_set / all_off / relays_close ----

class Relays : public ::testing::Test {
protected:
    void SetUp() override {
        fake_gpiod_reset();
        ASSERT_EQ(relays_init(), 0);
    }
    void TearDown() override { relays_close(); }  // relay.cpp keeps state in statics
};

TEST_F(Relays, SetDrivesTheRightPin) {
    EXPECT_EQ(relay_set(RELAY_HEAT, 1), 0);
    EXPECT_EQ(fake_gpiod_line(PIN_HEAT).value, GPIOD_LINE_VALUE_ACTIVE);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
    EXPECT_EQ(fake_gpiod_line(PIN_COOL).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_EQ(fake_gpiod_line(PIN_FAN).value, GPIOD_LINE_VALUE_INACTIVE);

    EXPECT_EQ(relay_set(RELAY_HEAT, 0), 0);
    EXPECT_EQ(fake_gpiod_line(PIN_HEAT).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_EQ(relay_get(RELAY_HEAT), 0);
}

TEST_F(Relays, InterlockRefusesCoolWhileHeatIsOn) {
    ASSERT_EQ(relay_set(RELAY_HEAT, 1), 0);
    int cool_writes = fake_gpiod_line(PIN_COOL).writes;

    EXPECT_EQ(relay_set(RELAY_COOL, 1), RELAY_INTERLOCK);
    EXPECT_EQ(fake_gpiod_line(PIN_COOL).writes, cool_writes) << "pin must not be touched";
    EXPECT_EQ(fake_gpiod_line(PIN_COOL).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_EQ(relay_get(RELAY_COOL), 0);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1);
}

TEST_F(Relays, InterlockRefusesHeatWhileCoolIsOn) {
    ASSERT_EQ(relay_set(RELAY_COOL, 1), 0);
    int heat_writes = fake_gpiod_line(PIN_HEAT).writes;

    EXPECT_EQ(relay_set(RELAY_HEAT, 1), RELAY_INTERLOCK);
    EXPECT_EQ(fake_gpiod_line(PIN_HEAT).writes, heat_writes) << "pin must not be touched";
    EXPECT_EQ(fake_gpiod_line(PIN_HEAT).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_EQ(relay_get(RELAY_HEAT), 0);
}

TEST_F(Relays, InterlockAllowsTheOtherOnceOff) {
    ASSERT_EQ(relay_set(RELAY_HEAT, 1), 0);
    ASSERT_EQ(relay_set(RELAY_HEAT, 0), 0);
    EXPECT_EQ(relay_set(RELAY_COOL, 1), 0);
}

TEST_F(Relays, InterlockAllowsTurningOffAndFanWithEither) {
    ASSERT_EQ(relay_set(RELAY_HEAT, 1), 0);
    EXPECT_EQ(relay_set(RELAY_COOL, 0), 0);  // off is never refused
    EXPECT_EQ(relay_set(RELAY_FAN, 1), 0);
}

TEST_F(Relays, FailedWriteLeavesStateUnchanged) {
    fake_gpiod_fail_next_set(PIN_HEAT);

    EXPECT_EQ(relay_set(RELAY_HEAT, 1), -1);
    EXPECT_EQ(relay_get(RELAY_HEAT), 0);
    EXPECT_EQ(relay_set(RELAY_COOL, 1), 0) << "a failed heat write must not block cool";
}

TEST_F(Relays, OutOfRangeRelayIsRejected) {
    EXPECT_EQ(relay_set(NUM_RELAYS, 1), -1);
    EXPECT_EQ(relay_get(NUM_RELAYS), 0);
    EXPECT_STREQ(relay_name(NUM_RELAYS), "?");
}

TEST_F(Relays, AllOffTriesEveryRelayEvenIfOneFails) {
    ASSERT_EQ(relay_set(RELAY_HEAT, 1), 0);
    ASSERT_EQ(relay_set(RELAY_FAN, 1), 0);
    fake_gpiod_fail_next_set(PIN_HEAT);

    EXPECT_EQ(all_off(), -1);
    EXPECT_EQ(relay_get(RELAY_HEAT), 1) << "the failed write is still reported as on";
    EXPECT_EQ(fake_gpiod_line(PIN_FAN).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_EQ(relay_get(RELAY_FAN), 0);
}

TEST_F(Relays, CloseTurnsEverythingOffAndReleases) {
    ASSERT_EQ(relay_set(RELAY_COOL, 1), 0);
    ASSERT_EQ(relay_set(RELAY_FAN, 1), 0);

    relays_close();
    EXPECT_EQ(fake_gpiod_line(PIN_COOL).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_EQ(fake_gpiod_line(PIN_FAN).value, GPIOD_LINE_VALUE_INACTIVE);
    EXPECT_FALSE(fake_gpiod_request_held());
    EXPECT_FALSE(fake_gpiod_chip_open());
    EXPECT_EQ(relay_set(RELAY_FAN, 1), -1) << "pins released";
}
