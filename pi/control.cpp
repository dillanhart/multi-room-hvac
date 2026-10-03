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


// status of the Pi's own SCD41. Satellites report their own status instead.
sensor_status local_sensor_status(const sensor_result& reading, long long started_at, long long now) {
    if (!reading.read_ok) return sensor_status::error;
    if (now - started_at < SENSOR_WARMUP) return sensor_status::warming;
    return sensor_status::ok;
}


// apply one reading to a node: range check, jump check against the previous
// reading, and CONSISTENT_READS good readings in a row before it is trusted
void update_node(sensor_node& node, const sensor_result& reading, sensor_status status, long long now) {
    sensor_health& h = node.health;
    h.status = status;

    if (status == sensor_status::warming) {
        h.trusted = false;
        h.consistent = 0;
        return;
    }

    if (status == sensor_status::error) {
        h.fails_in_row++;
        // a short blip: keep trusting the last good reading in node.data;
        // once it's too old, stop
        if (!h.has_last || now - h.last_read_at > MAX_READING_AGE)
            h.trusted = false;
        return;
    }

    h.fails_in_row = 0;

    bool in_range = reading.temp >= MIN_VALID_TEMP && reading.temp <= MAX_VALID_TEMP;

    // compare with the previous reading (trusted or not), as degrees per minute,
    // so a real change is accepted after CONSISTENT_READS instead of locking the sensor out
    bool rate_ok = true;
    if (h.has_last) {
        float minutes = std::max(now - h.last_read_at, (long long)SEC) / (float)MIN;
        rate_ok = std::fabs(reading.temp - h.last_temp) / minutes <= MAX_TEMP_RATE;
    }

    if (in_range && rate_ok) {
        h.consistent++;
    } else {
        if (h.consistent > 0)
            printf("%s: rejected %.1f F (%s)\n", node.name.c_str(), reading.temp,
                   in_range ? "jumped too fast" : "out of range");
        h.consistent = 0;
    }

    h.last_temp = reading.temp;
    h.last_read_at = now;
    h.has_last = true;
    node.data = reading;
    h.trusted = h.consistent >= CONSISTENT_READS;
}

// return int of the hour for determining dynamic weights
bool hour_of_day (){
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    return local_time->tm_hour > 8;
}


// calculate the weighted temp from trusted sensors;
// std::nullopt if no sensor is trusted
std::optional<float> calculate_weighted_temperature(const std::vector<sensor_node>& nodes) {
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


std::string get_HVAC_command(const std::vector<sensor_node>& sensors, 
                             float heat_set, 
                             float ac_set, 
                             float FAN_TRIGGER) {
    
    // 1. Get the aggregated data; no valid sensors means no decision
    std::optional<float> weighted = calculate_weighted_temperature(sensors);
    if (!weighted) return "no_data";
    float weighted_temp = *weighted;

    // 2. Extract raw temps for the "Fan" check
    // (Assuming we still need the raw min/max for the fan logic)
    std::vector<float> raw_temps;
    for(const auto& s : sensors) {
        if(s.health.trusted) raw_temps.push_back(s.data.temp);
    }

    // 3. Decision Logic
    if (weighted_temp > ac_set) return "cool";
    if (weighted_temp < heat_set) return "heat";
    
    if (!raw_temps.empty()) {
        auto [min_it, max_it] = std::minmax_element(raw_temps.begin(), raw_temps.end());
        if (*max_it - *min_it > FAN_TRIGGER) return "fan";
    }

    return "none";
}
