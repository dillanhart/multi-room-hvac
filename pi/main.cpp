/*
 * main.cpp - Thermostat main loop for Raspberry Pi 3
 *
 * Reads the SCD41 sensor every 10 seconds and decides whether heat or
 * cooling is needed. The hardware details live in their own files:
 *   control.cpp - temperature decision logic
 *   sensor.c   - SCD41 CO2/temperature/humidity sensor
 *   relay.cpp  - Heat/Cool/Fan relays on the GPIO pins
 *   net.cpp    - TCP requests to other devices and the status listener
 *
 * Build: python3 compile.py
 * Run:   ./thermostat_control   (user must be in the "gpio" group)
 */

#include <stdio.h>          // printf
#include <string>
#include <stdbool.h>        // bool, true, false
#include <time.h>           // clock_gettime
#include <unistd.h>         // sleep

#include "sensor.h"
#include "relay.h"

/*
 * now_ms - milliseconds since boot.
 * Uses the monotonic clock, so it never jumps if the system time changes.
 */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * main - claim the relay pins, then read the sensor and compare against
 * the set points forever.
 * Returns 1 if the relay pins can't be claimed.
 */
int main(void) {
    int sec = 1000;
    int min = 60 * sec;

    long long now = now_ms();

    // make sure sensor is reading correctly and prevent short cycling HVAC
    long long SENSOR_WARMUP = now + min * 3;
    int READ_INTERVAL = min;
    int MAX_SENSOR_FAILS = 5;
    int sensor_fails = 0;
    
    // avoid short cycling and constant running
    int MIN_ON_TIME = 5 * min;
    int MIN_OFF_TIME = 10 * min;
    int MAX_RUN_TIME = 120 * min;
    int cycle_start = 0;
    int cycle_end = 0;

    // make sure heat and sensor are working together
    int PROGRESS_WNIDOW = 20 * min;
    float PROGRESS_MIN_DELTA = 1; // temp must change by at least 1 degree within 20 mins


    // make sure the AC and Heat values are far enough apart
    float HYST = 1;
    float ac_set = 73;
    float heat_set = 70;
    bool ac_activation = true;
    float min_heat = 60;
    float MIN_DEAD_BAND = 3;

    // temp delta for fan to turn on and turn off
    float FAN_TRIGGER = 2;
    float FAN_CLEAR = 1;

    // Claim the three relay pins as outputs (all relays start off)
    if (relays_init())
        return 1;

    scd41_start();
    for(;;){
        sensor_result main_data = get_sensor_data();

        std::string command = "fuck me how do I program sometimes I swear to god this shit is so fucking confusing sometimes especially now that I am";
        }
    return 0;
}
