/*
 * sensor.h - SCD41 CO2/temperature/humidity sensor
 *
 * The SCD41 is on the Pi's I2C bus (SDA = physical pin 3, SCL = physical
 * pin 5). The bus path (/dev/i2c-1) is set in the Sensirion Linux HAL,
 * sensirion_i2c_hal.c.
 */

#ifndef SENSOR_H
#define SENSOR_H

// sensor.c is still C: tell C++ callers not to mangle these names
#ifdef __cplusplus
extern "C" {
#endif

// One reading from the SCD41. Check read_ok before using the other fields.
struct sensor_result {
    float temp;  // degrees F
    float hum;     // % relative humidity
    int CO2;            // parts per million
    int read_ok;          // 1 = reading OK, 0 = read failed
};

enum scd41_start_status {
    SCD41_ERROR = -1,
    SCD41_STARTED = 0,          // was idle, measurement just started
    SCD41_ALREADY_RUNNING = 1,  // an earlier run left it measuring
};

// Open the I2C bus and make sure the SCD41 is measuring.
// Returns one of enum scd41_start_status.
int scd41_start(void);

// Read CO2, temperature and humidity without waiting. Call scd41_start() once before this.
struct sensor_result get_sensor_data(void);

#ifdef __cplusplus
}
#endif

#endif
