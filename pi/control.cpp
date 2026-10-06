#include <stdio.h>
#include "sensor.h"
#include "control.h"
#include <string>
#include <vector>
#include <stdbool.h>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <optional>
#include <cmath>
#include "validation.h"
#include "constants.h"

// return int of the hour for determining dynamic weights
static bool hour_of_day (){
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    return local_time->tm_hour > 8;
}


// calculate the weighted temp from trusted sensors;
// std::nullopt if no sensor is trusted
static std::optional<float> calculate_weighted_temperature(const std::vector<sensor_node>& nodes) {
    float total_weighted_temp = 0.0f;
    float total_weight = 0.0f;
    bool is_day = hour_of_day();

    for (const auto& node : nodes) {
        if (node.health.trusted) {
            int weight = is_day ? node.weight_day : node.weight_night;
            total_weighted_temp += (node.data.temp * weight);
            total_weight += weight;
        }
    }

    if (total_weight == 0) return std::nullopt;
    return total_weighted_temp / total_weight;
}


hvac_command get_HVAC_command(const std::vector<sensor_node>& sensors, float heat_set, float ac_set) {
    
    // 1. Get the aggregated data; no valid sensors means no decision
    std::optional<float> weighted = calculate_weighted_temperature(sensors);
    if (!weighted) return hvac_command::no_data;
    float weighted_temp = *weighted;

    // 2. Extract raw temps for the "Fan" check
    // (Assuming we still need the raw min/max for the fan logic)
    std::vector<float> raw_temps;
    for(const auto& s : sensors) {
        if(s.health.trusted) raw_temps.push_back(s.data.temp);
    }

    // 3. Decision Logic
    if (weighted_temp > ac_set) return hvac_command::cool;
    if(weighted_temp < MIN_HEAT) return hvac_command::emergency_heat;
    if (weighted_temp < heat_set) return hvac_command::heat;
    
    if (!raw_temps.empty()) {
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
    // if the system is turning on has it been enough time since it turned off to switch
    if (sys_status == hvac_status::idle){
        if (now_ms > cycle_end + MIN_OFF_TIME || requested_status == hvac_status::circulating){
            return_status = requested_status;
        }
        else {return_status = sys_status;}
        }
    else {
        // if it is currently in an active situation (heating, cooling) then has it been running for long enough to turn off
        if(conflicting(sys_status, requested_status) ||  requested_status == hvac_status::circulating){
            if (now_ms > cycle_start + MIN_ON_TIME){
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
