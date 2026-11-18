#ifndef VALIDATION_H
#define VALIDATION_H

#include "sensor.h"
#include "constants.h"    // SEC, MIN, sensor_status, sensor_health, sensor_node

// sensor validation
constexpr int SENSOR_WARMUP = 3 * MIN;          // local SCD41 settling time after a fresh start
constexpr float MIN_VALID_TEMP = 30;            // degrees F; outside this range = bad reading
constexpr float MAX_VALID_TEMP = 110;
constexpr float MAX_TEMP_RATE = 20.0f / 3;      // degrees F per minute (20 degrees in 3 mins = failing)
constexpr int CONSISTENT_READS = 3;             // good readings in a row before a sensor is trusted
constexpr int MAX_READING_AGE = 3 * MIN;        // keep using the last good reading this long after failures
constexpr int STUCK_READS = 3;                  // identical readings (temp, humidity and CO2) in a row = frozen sensor
constexpr int STUCK_RECOVERY_READS = 10;        // good readings in a row before a frozen sensor is trusted again

sensor_status local_sensor_status(const sensor_result& reading, long long started_at, long long now);

// what a sensor says about itself; satellites send this in their JSON
// status of the Pi's own SCD41: error if the read failed, warming during SENSOR_WARMUP

// apply one reading to a node and decide whether it is trusted (all nodes, local and satellite)
void update_node(sensor_node& node, const sensor_result& reading, sensor_status status, long long now);

#endif
