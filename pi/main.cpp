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
#include <vector>

#include "sensor.h"
#include "relay.h"
#include "control.h"

// all times in milliseconds (SEC, MIN and the sensor validation constants are in control.h)

// make sure sensor is reading correctly and prevent short cycling HVAC
constexpr int READ_INTERVAL = MIN;
constexpr int MAX_SENSOR_FAILS = 5;

// avoid short cycling and constant running
constexpr int MIN_ON_TIME = 5 * MIN;
constexpr int MIN_OFF_TIME = 10 * MIN;
constexpr int MAX_RUN_TIME = 120 * MIN;

// make sure heat and sensor are working together
constexpr int PROGRESS_WINDOW = 20 * MIN;
constexpr float PROGRESS_MIN_DELTA = 1; // temp must change by at least 1 degree within 20 mins

// make sure the AC and Heat values are far enough apart
constexpr float HYST = 1;
constexpr float MIN_HEAT = 60;
constexpr float MIN_DEAD_BAND = 3;

// temp delta for fan to turn on and turn off
constexpr float FAN_TRIGGER = 2;
constexpr float FAN_CLEAR = 1;

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
    long long now = now_ms();

    // [[maybe_unused]]: not wired in yet; remove the tag once each is used
    [[maybe_unused]] int sensor_fails = 0;
    [[maybe_unused]] long long cycle_start = 0;  // ms timestamps: long long, int overflows after ~24 days
    [[maybe_unused]] long long cycle_end = 0;

    // settings: these will change at runtime (display / web UI)
    float ac_set = 73;
    float heat_set = 70;
    [[maybe_unused]] bool ac_activation = true;

    // Claim the three relay pins as outputs (all relays start off)
    if (relays_init())
        return 1;

    int start_status = scd41_start();


    // Setup three sensor data
    //nodes[0] = main, [1] = office, [2] = bed

    std::vector<sensor_node> nodes = {
    {.name = "main", .weight_day = 10, .weight_night = 8},
    {.name = "office", .weight_day = 6, .weight_night = 4},
    {.name = "bed", .weight_day = 4, .weight_night = 8}
    };

    // The SCD41 can't report how long it has been on. If an earlier run left it
    // measuring it's already warm; otherwise (fresh start or error) wait the full warm-up.
    nodes[0].health.started_at = (start_status == SCD41_ALREADY_RUNNING)
                                 ? now - SENSOR_WARMUP
                                 : now;


    for(;;){
        now = now_ms();

        sensor_result reading = get_sensor_data();
        update_node(nodes[0], reading, local_sensor_status(reading, nodes[0].health.started_at, now), now);

        std::string command = get_HVAC_command(nodes, heat_set, ac_set, FAN_TRIGGER);
        const sensor_health& h = nodes[0].health;
        printf("%s | main %.1f F, %s, %s (%d good in a row)\n", command.c_str(), nodes[0].data.temp,
               h.status == sensor_status::ok ? "ok" : h.status == sensor_status::warming ? "warming" : "error",
               h.trusted ? "trusted" : "untrusted", h.consistent);
        sleep(10);
        }
    return 0;
}
