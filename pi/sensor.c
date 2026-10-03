/*
 * sensor.c - SCD41 CO2/temperature/humidity sensor
 *
 * Wraps the Sensirion SCD4x driver (embedded-i2c-scd4x/, C version of the
 * library) so the rest of the program only sees struct sensor_result.
 */

#include <stdio.h>          // printf, fprintf
#include <stdbool.h>        // bool
#include <stdint.h>         // uint16_t, int32_t

#include "sensor.h"
#include "scd4x_i2c.h"
#include "sensirion_i2c_hal.h"

/*
 * scd41_start - open the I2C bus and make sure the SCD41 is measuring.
 *
 * The sensor keeps measuring after this program exits (until it loses
 * power), and stopping it would restart its few-minute warm-up. So if an
 * earlier run left it measuring, it is left running untouched. Otherwise
 * automatic self-calibration is turned off and periodic measurement (one
 * reading every 5 seconds) is started.
 *
 * Returns: SCD41_STARTED if this call started measuring (sensor needs its
 * warm-up), SCD41_ALREADY_RUNNING if an earlier run left it measuring
 * (already warm), or SCD41_ERROR (error is printed).
 */
int scd41_start(void) {
    static int bus_open = 0;
    int16_t error;

    // Open the bus only once. The Sensirion HAL remembers the device address
    // across opens, so a second open gets a handle that never has the address
    // set, and every command on it fails.
    if (!bus_open) {
        sensirion_i2c_hal_init();
        bus_open = 1;
    }
    scd4x_init(SCD41_I2C_ADDR_62);

    scd4x_wake_up();  // no reply expected, so ignore errors

    // ASC can only be changed while idle. The sensor rejects the command
    // while measuring, so a failure here means an earlier run left it going.
    // (If the bus is actually broken, the data-ready check fails next.)
    if (scd4x_set_automatic_self_calibration_enabled(0)) {
        printf("SCD41: already measuring, keeping it running\n");
        return SCD41_ALREADY_RUNNING;
    }
    error = scd4x_start_periodic_measurement();
    if (error) {
        fprintf(stderr, "SCD41: start_periodic_measurement: %d\n", error);
        return SCD41_ERROR;
    }
    return SCD41_STARTED;
}

/*
 * get_sensor_data - read CO2, temperature and humidity from the SCD41.
 *
 * Only reads, and never waits: call scd41_start() once first. The sensor
 * makes a measurement every 5 seconds, so when called every 10+ seconds a new
 * one is always ready; "not ready" means the sensor stopped measuring and
 * counts as a failure. The bus stays open on errors; after a few failures in
 * a row it restarts periodic measurement, in case the sensor lost power and
 * came back idle.
 *
 * Returns: a sensor_result; check .read_ok before using the values. This
 * only says the read worked - whether to trust the value is decided by
 * update_node() in control.cpp.
 */
struct sensor_result get_sensor_data(void) {
    static int fails = 0;  // failed reads in a row
    struct sensor_result result = { 0 };
    bool data_ready = false;
    uint16_t co2;
    int32_t temp_m_deg_c, humidity_m_percent;

    if (scd4x_get_data_ready_status(&data_ready))
        goto fail;
    if (!data_ready) {
        fprintf(stderr, "SCD41: no new measurement ready\n");
        goto fail;
    }

    // The library checks the CRCs and converts to milli-units
    if (scd4x_read_measurement(&co2, &temp_m_deg_c, &humidity_m_percent))
        goto fail;

    result.CO2 = co2;
    result.temp = temp_m_deg_c / 1000.0f * 9.0f / 5.0f + 32.0f;  // to Fahrenheit
    result.hum = humidity_m_percent / 1000.0f;
    result.read_ok = 1;
    fails = 0;
    return result;

fail:
    fails++;
    fprintf(stderr, "SCD41: read failed (%d in a row)\n", fails);
    // Every 3rd failure in a row, try to restart measuring. If the sensor is
    // already measuring it just rejects the command, which is harmless.
    if (fails % 3 == 0 && scd4x_start_periodic_measurement() == 0)
        fprintf(stderr, "SCD41: periodic measurement restarted\n");
    return result;
}
