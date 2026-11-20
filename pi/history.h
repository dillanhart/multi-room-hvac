/*
 * history.h - the last HISTORY_SIZE control passes, in RAM only (lost on restart).
 *
 * The control pass adds one entry a minute, and every status change happens in a
 * control pass, so every change is recorded. The web server keeps the long-term copy
 * and asks for what it's missing (entries_since); this ring only has to cover a
 * restart or outage of the server.
 *
 * Not thread-safe by itself: it lives in shared_state, behind that mutex.
 */

#ifndef HISTORY_H
#define HISTORY_H

#include <array>
#include <optional>
#include <vector>
#include "constants.h"    // HISTORY_SIZE, SENSOR_COUNT, hvac_mode
#include "control.h"      // hvac_status

struct history_sensor {
    bool present = false;   // false: never reported (shown as null)
    float temp = 0;
    float hum = 0;
    int co2 = 0;
    bool trusted = false;   // false: sent anyway, but control ignored it
};

struct history_entry {
    long long time = 0;     // Unix seconds, taken in the control pass
    hvac_status status = hvac_status::idle;
    hvac_mode mode = hvac_mode::auto_select;   // as set, not as resolved
    std::array<history_sensor, SENSOR_COUNT> sensors{};
    std::optional<float> weighted_avg;          // nullopt: no trusted sensor
};

class history_ring {
public:
    // adds one entry; once full, the oldest is overwritten
    void add(const history_entry& entry);

    // entries strictly newer than since, oldest first (since = 0 gives everything)
    std::vector<history_entry> entries_since(long long since) const;

    size_t size() const { return count; }

private:
    std::vector<history_entry> slots = std::vector<history_entry>(HISTORY_SIZE);
    size_t next = 0;    // slot the next entry goes in
    size_t count = 0;   // filled slots, up to HISTORY_SIZE
};

#endif
