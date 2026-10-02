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

// One reading from the SCD41. Check valid before using the other fields.
struct sensor_result {
    float temperature;  // degrees F
    float humidity;     // % relative humidity
    int CO2;            // parts per million
    int valid;          // 1 = reading OK, 0 = read failed
};

// Open the I2C bus and make sure the SCD41 is measuring.
// Returns 0 on success, or the library's error code.
int scd41_start(void);

// Read CO2, temperature and humidity. Starts the sensor on first use.
struct sensor_result get_sensor_data(void);

#ifdef __cplusplus
}
#endif

#endif
