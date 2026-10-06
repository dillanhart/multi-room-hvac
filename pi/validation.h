#ifndef VALIDATION_H
#define VALIDATION_H

#include <string>
#include "sensor.h"

// all times in milliseconds
constexpr int SEC = 1000;
constexpr int MIN = 60 * SEC;

// sensor validation
constexpr int SENSOR_WARMUP = 3 * MIN;          // local SCD41 settling time after a fresh start
constexpr float MIN_VALID_TEMP = 50;            // degrees F; outside this range = bad reading
constexpr float MAX_VALID_TEMP = 110;
constexpr float MAX_TEMP_RATE = 20.0f / 3;      // degrees F per minute (20 degrees in 3 mins = failing)
constexpr int CONSISTENT_READS = 3;             // good readings in a row before a sensor is trusted
constexpr int MAX_READING_AGE = 3 * MIN;        // keep using the last good reading this long after failures

enum class sensor_status { ok, warming, error };

// keep metadata for the sensor to see if it is reading properly
struct sensor_health {
    sensor_status status = sensor_status::error;
    long long started_at = 0;      // local sensor only: when it started measuring (set from scd41_start)
    long long last_read_at = 0;    // time of the last successful read
    float last_temp = 0;           // that read's temperature, for the jump check
    bool has_last = false;         // false until the first successful read
    int fails_in_row = 0;
    int consistent = 0;            // good readings in a row
    bool trusted = false;          // the only thing control logic checks
};


struct sensor_node {
    std::string name;
    sensor_result data;            // last successful reading; use only when health.trusted
    int weight_day;
    int weight_night;
    sensor_health health;
};

sensor_status local_sensor_status(const sensor_result& reading, long long started_at, long long now);

// what a sensor says about itself; satellites send this in their JSON
// status of the Pi's own SCD41: error if the read failed, warming during SENSOR_WARMUP

// apply one reading to a node and decide whether it is trusted (all nodes, local and satellite)
void update_node(sensor_node& node, const sensor_result& reading, sensor_status status, long long now);

#endif
