#include <gtest/gtest.h>
#include "history.h"

static history_entry at(long long time) {
    history_entry e;
    e.time = time;
    return e;
}

static std::vector<long long> times(const std::vector<history_entry>& entries) {
    std::vector<long long> t;
    for (const auto& e : entries) t.push_back(e.time);
    return t;
}

TEST(History, EmptyGivesNothing) {
    history_ring ring;
    EXPECT_TRUE(ring.entries_since(0).empty());
}

TEST(History, OldestFirstStrictlyAfterSince) {
    history_ring ring;
    for (long long t : {60, 120, 180}) ring.add(at(t));
    EXPECT_EQ(times(ring.entries_since(0)), (std::vector<long long>{60, 120, 180}));
    EXPECT_EQ(times(ring.entries_since(120)), (std::vector<long long>{180}));   // 120 itself already fetched
    EXPECT_TRUE(ring.entries_since(180).empty());
}

TEST(History, FullRingDropsTheOldest) {
    history_ring ring;
    for (long long i = 1; i <= HISTORY_SIZE + 5; i++) ring.add(at(i * 60));
    EXPECT_EQ(ring.size(), static_cast<size_t>(HISTORY_SIZE));

    std::vector<history_entry> all = ring.entries_since(0);
    ASSERT_EQ(all.size(), static_cast<size_t>(HISTORY_SIZE));
    EXPECT_EQ(all.front().time, 6 * 60);                       // 1..5 overwritten
    EXPECT_EQ(all.back().time, (HISTORY_SIZE + 5) * 60LL);
}

// the website asks for everything after its last stored row; across the wrap point too
TEST(History, SinceAcrossTheWrapPoint) {
    history_ring ring;
    for (long long i = 1; i <= HISTORY_SIZE + 2; i++) ring.add(at(i * 60));
    long long last_stored = HISTORY_SIZE * 60LL;
    EXPECT_EQ(times(ring.entries_since(last_stored)),
              (std::vector<long long>{(HISTORY_SIZE + 1) * 60LL, (HISTORY_SIZE + 2) * 60LL}));
}
