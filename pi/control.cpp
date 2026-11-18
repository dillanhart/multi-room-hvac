#include "control.h"
#include "constants.h"    // sensor_node, MIN_HEAT, FAN_TRIGGER, cycle timings
#include "relay.h"        // relay_set, all_off

#include <algorithm>      // std::minmax_element
#include <ctime>          // std::time, std::localtime
#include <optional>
#include <vector>

bool is_night_now() {
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    return local_time->tm_hour < 8;
}


// calculate the weighted temp from trusted sensors;
// std::nullopt if no sensor is trusted
static std::optional<float> calculate_weighted_temperature(const std::vector<sensor_node>& nodes, bool is_night) {
    float total_weighted_temp = 0.0f;
    float total_weight = 0.0f;

    for (const auto& node : nodes) {
        if (node.health.trusted) {
            int weight = is_night ? node.weight_night : node.weight_day;
            total_weighted_temp += (node.data.temp * weight);
            total_weight += weight;
        }
    }

    if (total_weight == 0) return std::nullopt;
    return total_weighted_temp / total_weight;
}


hvac_command get_HVAC_command(const std::vector<sensor_node>& sensors, const hvac_set_points& points,
                              bool is_night, hvac_mode allowed, std::optional<float> outside_temp) {

    // 1. Get the aggregated data; no valid sensors means no decision
    std::optional<float> weighted = calculate_weighted_temperature(sensors, is_night);
    if (!weighted) return hvac_command::no_data;
    float weighted_temp = *weighted;

    // 2. Extract raw temps for the "Fan" check
    std::vector<float> raw_temps;
    for(const auto& s : sensors) {
        if(s.health.trusted) raw_temps.push_back(s.data.temp);
    }

    // 3. Active set points: at night the whole band drops
    float shift = is_night ? points.night_difference : 0;
    float heat_set = points.heat_point - shift;
    float ac_set = points.ac_point - shift;

    // 4. What may run: mode/season, then the weather gate (skipped if outside temp unknown)
    bool cool_ok = allowed != hvac_mode::heat &&
                   !(outside_temp && *outside_temp <= ac_set - WEATHER_GATE_MARGIN);
    bool heat_ok = allowed != hvac_mode::cool &&
                   !(outside_temp && *outside_temp >= heat_set + WEATHER_GATE_MARGIN);

    // 5. Decision Logic (em heat is never gated)
    if (weighted_temp > ac_set && cool_ok) return hvac_command::cool;
    if (weighted_temp < MIN_HEAT) return hvac_command::emergency_heat;
    if (weighted_temp < heat_set && heat_ok) return hvac_command::heat;

    if (points.fan_allowed && !raw_temps.empty()) {
        auto [min_it, max_it] = std::minmax_element(raw_temps.begin(), raw_temps.end());
        if (*max_it - *min_it > FAN_TRIGGER) return hvac_command::fan;
    }

    return hvac_command::none;
}

enum class equipment { none, furnace, ac };

static equipment equipment_for(hvac_status s) {
    switch (s) {
        case hvac_status::heating:
        case hvac_status::em_heating:  return equipment::furnace;
        case hvac_status::cooling:     return equipment::ac;
        default:                       return equipment::none;  // idle, circulating
    }
}

// true if going from a to b means switching between furnace and AC
static bool conflicting(hvac_status a, hvac_status b) {
    equipment ea = equipment_for(a), eb = equipment_for(b);
    return ea != equipment::none && eb != equipment::none && ea != eb;
}

// the status a command is asking for; no_data asks for idle
// (no default: -Wswitch warns if a new command isn't mapped here)
static hvac_status status_for(hvac_command command) {
    switch (command) {
        case hvac_command::emergency_heat: return hvac_status::em_heating;
        case hvac_command::heat:           return hvac_status::heating;
        case hvac_command::cool:           return hvac_status::cooling;
        case hvac_command::fan:            return hvac_status::circulating;
        case hvac_command::none:
        case hvac_command::no_data:        return hvac_status::idle;
    }
    return hvac_status::idle;  // unreachable; satisfies -Wreturn-type
}

hvac_status test_switch (hvac_status sys_status, hvac_command command, long long cycle_start, long long now_ms, long long cycle_end){
    hvac_status requested_status = status_for(command);
    hvac_status return_status;
    
    // for emergency heating and circulating fan bypass min_off_time and max_run_time
    if (command == hvac_command::emergency_heat || command == hvac_command::fan){
        return_status = requested_status;

    // if the system is turning on has it been enough time since it turned off to switch
    } else if (sys_status == hvac_status::idle){
        if (now_ms > cycle_end + MIN_OFF_TIME || requested_status == hvac_status::circulating){
            return_status = requested_status;
        }
        else {return_status = sys_status;}
        }
    else {
        // if it is currently in an active situation (heating, cooling) then has it been running for long enough to turn off
        if(!conflicting(sys_status, requested_status)){
            return_status = sys_status;
            // circulating -> furnace/AC restarts the equipment, so it waits out MIN_OFF_TIME like idle does
            bool restart_blocked = equipment_for(sys_status) == equipment::none &&
                                   equipment_for(requested_status) != equipment::none &&
                                   now_ms <= cycle_end + MIN_OFF_TIME;
            if (now_ms > cycle_start + MIN_ON_TIME && !restart_blocked){
                return_status = requested_status;
                // if it has been running for too long, nevermind turn it off.
                if (now_ms > cycle_start + MAX_RUN_TIME){
                    return_status = hvac_status::idle;
                }
            }
        }else {return_status = hvac_status::idle;}
    }
    return return_status;
}

// switch the relays to match new_status; only if that worked, stamp the cycle timers
// (furnace/AC stopping stamps cycle_end, any non-idle status stamps cycle_start;
// em_heating -> heating keeps cycle_start so MAX_RUN_TIME keeps counting from
// when the furnace started)
// returns 0 on success, non-zero on failure (timers untouched, caller retries)
int apply_status(hvac_status old_status, hvac_status new_status, long long now, long long& cycle_start, long long& cycle_end){
    int relay_success = -1;
    switch (new_status){
        case hvac_status::idle:        relay_success = all_off(); break;
        case hvac_status::cooling:
            // fan may still be on from circulating; drop it first
            relay_success = relay_set(RELAY_FAN, 0);
            if (relay_success == 0) relay_success = relay_set(RELAY_COOL, 1);
            break;
        case hvac_status::em_heating:
        case hvac_status::heating:
            relay_success = relay_set(RELAY_FAN, 0);
            if (relay_success == 0) relay_success = relay_set(RELAY_HEAT, 1);
            break;
        case hvac_status::circulating:
            // heat/cool may still be on from the previous status; drop them first
            relay_success = all_off();
            if (relay_success == 0) relay_success = relay_set(RELAY_FAN, 1);
            break;
    }
    if (relay_success != 0) return relay_success;

    // the furnace/AC just stopped (to idle or to circulating): MIN_OFF_TIME starts now
    if (equipment_for(old_status) != equipment::none && equipment_for(new_status) == equipment::none)
        cycle_end = now;
    if (new_status != hvac_status::idle &&
        !(old_status == hvac_status::em_heating && new_status == hvac_status::heating))
        cycle_start = now;
    return 0;
}
