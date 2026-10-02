/*
 * sensor.c - SCD41 CO2/temperature/humidity sensor
 *
 * Wraps the Sensirion SCD4x driver (embedded-i2c-scd4x/, C version of the
 * library) so the rest of the program only sees struct sensor_result.
 */

#include <stdio.h>          // printf, fprintf
#include <stdbool.h>        // bool
#include <stdint.h>         // uint16_t, int32_t
#include <unistd.h>         // usleep

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
 * Returns: 0 on success, or the library's error code (error is printed).
 */
int scd41_start(void) {
    int16_t error;

    sensirion_i2c_hal_init();
    scd4x_init(SCD41_I2C_ADDR_62);

    scd4x_wake_up();  // no reply expected, so ignore errors

    // ASC can only be changed while idle. The sensor rejects the command
    // while measuring, so a failure here means an earlier run left it going.
    // (If the bus is actually broken, the data-ready check fails next.)
    if (scd4x_set_automatic_self_calibration_enabled(0)) {
        printf("SCD41: already measuring, keeping it running\n");
        return 0;
    }
    error = scd4x_start_periodic_measurement();
    if (error) {
        fprintf(stderr, "SCD41: start_periodic_measurement: %d\n", error);
        return error;
    }
    return 0;
}

/*
 * get_sensor_data - read CO2, temperature and humidity from the SCD41.
 *
 * The first call starts the sensor, then waits up to about 6 seconds for its
 * first measurement. Later calls return the newest measurement, waiting if
 * one isn't ready yet (the sensor makes one every 5 seconds). On any error the
 * bus is closed, so the next call starts over.
 *
 * Returns: a sensor_result; check .valid before using the values.
 */
struct sensor_result get_sensor_data(void) {
    static int started = 0;  // sensor stays running between calls
    struct sensor_result result = { 0 };
    bool data_ready = false;
    uint16_t co2;
    int32_t temp_m_deg_c, humidity_m_percent;

    if (!started) {
        if (scd41_start())
            goto fail;
        started = 1;
    }

    // Wait for a new measurement: poll every 100 ms, give up after 6 seconds
    for (int tries = 0; ; tries++) {
        if (scd4x_get_data_ready_status(&data_ready))
            goto fail;
        if (data_ready)
            break;
        if (tries >= 60) {
            fprintf(stderr, "SCD41: timed out waiting for a measurement\n");
            goto fail;
        }
        usleep(100000);
    }

    // The library checks the CRCs and converts to milli-units
    if (scd4x_read_measurement(&co2, &temp_m_deg_c, &humidity_m_percent))
        goto fail;

    result.CO2 = co2;
    result.temperature = temp_m_deg_c / 1000.0f * 9.0f / 5.0f + 32.0f;  // to Fahrenheit
    result.humidity = humidity_m_percent / 1000.0f;
    result.valid = 1;
    return result;

fail:
    fprintf(stderr, "SCD41: read failed, will reconnect on next call\n");
    sensirion_i2c_hal_free();
    started = 0;
    return result;
}
