/*
 * calibrate.c - one-time CO2 calibration (forced recalibration) for the SCD41
 *
 * Put the sensor in fresh outdoor air (outside, or at a wide-open window with
 * no one breathing near it) and run this. It measures for 3 minutes, then
 * tells the sensor that the air it is in has the target CO2 level, and the
 * sensor corrects its readings from then on.
 *
 * Build: gcc -Wall -Iembedded-i2c-scd4x -o calibrate calibrate.c \
 *            embedded-i2c-scd4x/scd4x_i2c.c embedded-i2c-scd4x/sensirion_i2c.c \
 *            embedded-i2c-scd4x/sensirion_common.c \
 *            embedded-i2c-scd4x/sample-implementations/linux_user_space/sensirion_i2c_hal.c
 * Run:   ./calibrate [target_ppm]   (default 425 ppm, typical outdoor air)
 */

#include <stdbool.h>        // bool
#include <stdint.h>         // uint16_t, int32_t
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // atoi
#include <unistd.h>         // sleep, usleep

// Sensirion SCD4x driver (embedded-i2c-scd4x/, C version of the library)
#include "scd4x_i2c.h"
#include "sensirion_i2c_hal.h"

#define DEFAULT_TARGET_PPM 438
#define SETTLE_SECONDS     180  // datasheet: measure at least 3 minutes first

int main(int argc, char **argv) {
    int target = argc > 1 ? atoi(argv[1]) : DEFAULT_TARGET_PPM;
    uint16_t correction;
    int16_t error;

    if (target < 400 || target > 2000) {
        fprintf(stderr, "usage: %s [target_ppm]  (400-2000, default %d)\n",
                argv[0], DEFAULT_TARGET_PPM);
        return 1;
    }

    sensirion_i2c_hal_init();
    scd4x_init(SCD41_I2C_ADDR_62);
    scd4x_wake_up();  // no reply expected, so ignore errors

    // Restart measuring so we know it has run in this air for the full time
    scd4x_stop_periodic_measurement();  // may fail if already idle
    error = scd4x_start_periodic_measurement();
    if (error) {
        fprintf(stderr, "start_periodic_measurement: %d\n", error);
        return 1;
    }

    // Show readings while settling so you can see them level off
    printf("Measuring for %d seconds. Keep the sensor in fresh air...\n", SETTLE_SECONDS);
    for (int t = 0; t < SETTLE_SECONDS; t += 5) {
        uint16_t co2;
        int32_t temp_m_deg_c, humidity_m_percent;
        bool data_ready = false;

        sleep(5);
        if (!scd4x_get_data_ready_status(&data_ready) && data_ready &&
            !scd4x_read_measurement(&co2, &temp_m_deg_c, &humidity_m_percent))
            printf("  %3ds  CO2 = %u ppm\n", t + 5, co2);
    }

    // FRC only works while idle
    error = scd4x_stop_periodic_measurement();
    if (error) {
        fprintf(stderr, "stop_periodic_measurement: %d\n", error);
        return 1;
    }
    error = scd4x_perform_forced_recalibration((uint16_t)target, &correction);
    if (error || correction == 0xFFFF) {
        fprintf(stderr, "Forced recalibration failed (error %d, result 0x%04X)\n",
                error, correction);
        return 1;
    }
    printf("Calibrated to %d ppm (correction applied: %+d ppm)\n",
           target, (int)correction - 0x8000);

    // Leave it measuring (ASC off) so thermostat_control can pick it up without a warm-up
    scd4x_set_automatic_self_calibration_enabled(0);
    scd4x_start_periodic_measurement();
    sensirion_i2c_hal_free();
    return 0;
}
