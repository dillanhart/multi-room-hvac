/*
 * thermostat.h - the state shared between the threads, and one control pass.
 *
 * Threads (started in main.cpp):
 *   control  - every CONTROL_INTERVAL: read sensors, then control_tick()
 *   weather  - nightly NWS fetch (net.cpp), then update_weather()
 *   http     - cpp-httplib's pool; each request runs an api.cpp handler
 *   main     - waits for SIGTERM/SIGINT, then stops the others
 *
 * Locking rule: every field of shared_state is read and written only while
 * holding shared_state::lock. Nothing slow (network, sensor I2C) happens while
 * holding it; the slowest thing is a settings file write (fsync), a few ms.
 * The sensor nodes vector belongs to the control thread; the API sees the copy
 * in `latest`.
 */

#ifndef THERMOSTAT_H
#define THERMOSTAT_H

#include <mutex>
#include <optional>
#include <vector>
#include "constants.h"
#include "control.h"
#include "data_store.h"
#include "history.h"

// what the last control pass saw and decided; served by the GET handlers
struct snapshot {
    std::vector<sensor_node> nodes;               // empty until the first pass
    std::optional<float> weighted_avg;            // the number control used; nullopt = no trusted sensor
    bool away = false;
    hvac_mode active_mode = hvac_mode::heat;      // after resolve_mode
};

struct shared_state {
    std::mutex lock;
    settings_store settings;   // set points, mode, vacations, weather (handlers write, control reads)
    control_state control;     // status and timers (control); fan (handlers write)
    snapshot latest;           // control writes, handlers read
    history_ring history;      // control writes, handlers read
};

// One control pass. Takes the lock itself. nodes must already hold this pass's readings.
// clock_synced: wall clock set by NTP. Until then nothing that needs the date runs:
// no history, no vacation clean-up or away, no weather gate (control still runs).
void control_tick(shared_state& s, const std::vector<sensor_node>& nodes,
                  long long now_ms, long long now_unix, bool is_night, bool clock_synced);

// the history row for this pass (pure; used by control_tick)
history_entry make_history_entry(const std::vector<sensor_node>& nodes, std::optional<float> weighted_avg,
                                 hvac_status status, hvac_mode mode, long long now_unix);

#endif
