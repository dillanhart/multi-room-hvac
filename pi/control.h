#ifndef CONTROL_H
#define CONTROL_H

#include <optional>
#include <string>
#include <vector>
#include "sensor.h"
#include "constants.h"    // sensor_node, hvac_set_points, hvac_mode

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

// manual fan setting: on = circulate whenever heat/cool isn't running (held off while away);
// auto = only the automatic rules (zone spread, CO2). RAM only, starts auto.
enum class fan_mode { auto_select, on };

// everything control keeps from one pass to the next (RAM only)
struct control_state {
    hvac_status status = hvac_status::idle;
    fan_mode fan = fan_mode::auto_select;
    bool co2_circulating = false;   // last answer of co2_fan_call (its hysteresis memory)
    long long cycle_start = 0;      // ms: when the current non-idle status began
    long long cycle_end = 0;        // ms: when the furnace/AC last stopped (min-off counts from here)
};

// state at boot: idle with both timers stamped now, so a power blip mid-cycle
// still waits out a full min-off before the furnace/AC restarts
control_state initial_control_state(long long now);

// true from midnight until 8 am; 9 pm-midnight counts as day (user's schedule).
// Reads the wall clock: call it in main and pass the result in, so the logic stays testable.
bool is_night_now();

// weighted average temp of the trusted sensors (day or night weights);
// nullopt if none is trusted. The number get_HVAC_command decides on.
std::optional<float> weighted_temperature(const std::vector<sensor_node>& sensors, bool is_night);

// CO2 rule, with hysteresis. running = what this returned last pass.
// Start: a trusted room over CO2_TRIGGER and over CO2_START_RATIO x the average.
// Keep going while a trusted room is over CO2_STOP_RATIO x the average.
// Average: weighted with the day weights (also at night); a room with no trusted
// reading counts as CO2_FILL_PPM. is_night is currently unused.
bool co2_fan_call(const std::vector<sensor_node>& sensors, bool is_night, bool running);

// points: the user's set points, or AWAY_POINTS while away (fan_allowed = false holds off every fan call).
// is_night: drops the whole band by night_difference and uses night sensor weights.
// allowed: resolve_mode() result (heat / cool / both); never gates em heat.
// outside_temp: forecast for this hour; nullopt skips the weather gate.
// fan: manual setting; on asks for fan whenever heat/cool/em heat isn't called, even with no sensor data.
// co2_fan: co2_fan_call() result.
hvac_command get_HVAC_command(const std::vector<sensor_node>& sensors, const hvac_set_points& points,
                              bool is_night, hvac_mode allowed, std::optional<float> outside_temp,
                              fan_mode fan, bool co2_fan);

// the status to move to, given the current one and the timers (pure: no relays touched)
hvac_status test_switch (hvac_status sys_status, hvac_command command,long long cycle_start, long long now_ms, long long cycle_end);

// switch the relays to new_status; on success stamps cycle_start/cycle_end and returns 0
int apply_status(hvac_status old_status, hvac_status new_status, long long now, long long& cycle_start, long long& cycle_end);

// one control pass: CO2 rule -> command -> test_switch -> relays. Updates state;
// a failed relay switch leaves state.status as it was, so the next pass retries.
// Returns the command, for logging.
hvac_command control_step(control_state& state, const std::vector<sensor_node>& sensors,
                          const hvac_set_points& points, bool is_night, hvac_mode allowed,
                          std::optional<float> outside_temp, long long now);

#endif
