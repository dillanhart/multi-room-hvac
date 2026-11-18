#ifndef CONTROL_H
#define CONTROL_H

#include <optional>
#include <string>
#include <vector>
#include "sensor.h"
#include "constants.h"    // sensor_node

enum class hvac_command{
    emergency_heat,
    heat,
    cool,
    fan,
    none,
    no_data,
};

enum class hvac_status{
    em_heating,
    heating,
    cooling,
    circulating,
    idle,
};

// true from midnight until 8 am; 9 pm-midnight counts as day (user's schedule).
// Reads the wall clock: call it in main and pass the result in, so the logic stays testable.
bool is_night_now();

// points: the user's set points, or AWAY_POINTS while away (no fan).
// is_night: drops the whole band by night_difference and uses night sensor weights.
// allowed: resolve_mode() result (heat / cool / both); never gates em heat.
// outside_temp: forecast for this hour; nullopt skips the weather gate.
hvac_command get_HVAC_command(const std::vector<sensor_node>& sensors, const hvac_set_points& points,
                              bool is_night, hvac_mode allowed, std::optional<float> outside_temp);

hvac_status test_switch (hvac_status sys_status, hvac_command command,long long cycle_start, long long now_ms, long long cycle_end);

// switch the relays to new_status; on success stamps cycle_start/cycle_end and returns 0
int apply_status(hvac_status old_status, hvac_status new_status, long long now, long long& cycle_start, long long& cycle_end);


#endif
