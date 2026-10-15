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



// status of the Pi's own SCD41. Satellites report their own status instead.
sensor_status local_sensor_status(const sensor_result& reading, long long started_at, long long now) {
    if (!reading.read_ok) return sensor_status::error;
    if (now - started_at < SENSOR_WARMUP) return sensor_status::warming;
    return sensor_status::ok;
}


// a real sensor jitters, so all three values exactly matching means a frozen value
static bool same_reading(const sensor_result& a, const sensor_result& b) {
    return a.temp == b.temp && a.hum == b.hum && a.CO2 == b.CO2;
}


// apply one reading to a node: range check, jump check against the previous
// reading, frozen check, and CONSISTENT_READS good readings in a row before it is trusted
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
        if (!h.has_last || now - h.last_read_at > MAX_READING_AGE){
            h.trusted = false;
            h.consistent = 0;
        }
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

    // node.data is the previous successful reading whenever has_last is set
    h.identical_in_row = (h.has_last && same_reading(reading, node.data)) ? h.identical_in_row + 1 : 1;

    // frozen: start the count below zero so it takes STUCK_RECOVERY_READS good reads
    // after the value moves again; staying frozen keeps resetting it
    constexpr int STUCK_PENALTY = CONSISTENT_READS - STUCK_RECOVERY_READS;
    if (h.identical_in_row >= STUCK_READS) {
        if (h.consistent > STUCK_PENALTY)
            printf("%s: frozen at %.2f F, %.1f %%, %d ppm\n", node.name.c_str(),
                   reading.temp, reading.hum, reading.CO2);
        h.consistent = STUCK_PENALTY;
    } else if (in_range && rate_ok) {
        h.consistent++;
    } else {
        if (h.consistent > 0)
            printf("%s: rejected %.1f F (%s)\n", node.name.c_str(), reading.temp,
                   in_range ? "jumped too fast" : "out of range");
        h.consistent = std::min(h.consistent, 0);  // keep a frozen sensor's penalty
    }

    h.last_temp = reading.temp;
    h.last_read_at = now;
    h.has_last = true;
    node.data = reading;
    h.trusted = h.consistent >= CONSISTENT_READS;
}