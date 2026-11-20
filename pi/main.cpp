/*
 * main.cpp - Thermostat main loop for Raspberry Pi 3
 *
 * Reads the SCD41 sensor every 10 seconds and decides whether heat or
 * cooling is needed. The hardware details live in their own files:
 *   control.cpp - temperature decision logic
 *   sensor.c   - SCD41 CO2/temperature/humidity sensor
 *   relay.cpp  - Heat/Cool/Fan relays on the GPIO pins
 *   net.cpp    - TCP requests to other devices and the status listener
 *   data_store.cpp - saved set points, vacations, weather (JSON files)
 *   weather.cpp - season and outside temp from the stored forecast
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
#include "constants.h"
#include "validation.h"
#include "data_store.h"
#include "weather.h"

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
    // status, fan setting and cycle timers (ms timestamps: long long, int overflows after ~24 days)
    control_state state = initial_control_state(now);

    // settings: loaded once here, then changed at runtime (display / web UI)
    settings_store settings;
    load_settings(settings);
    bool away_mode = false;

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

        // vacations use wall-clock time (Unix seconds), not now_ms()
        long long now_unix = time(nullptr);
        remove_finished_vacations(settings, now_unix);
        away_mode = is_away(settings.vacations, now_unix);
        // TODO: if (weather_due(settings.weather, now_unix)) fetch, then update_weather()
        retry_unsaved(settings);

        // away: fixed low heat point, no fan (manual fan-on is kept, just held off);
        // mode/season and weather gate still apply
        const hvac_set_points& active = away_mode ? AWAY_POINTS : settings.points;
        hvac_mode allowed = resolve_mode(settings.mode, season_from_weather(settings.weather));
        control_step(state, nodes, active, is_night_now(), allowed,
                     outside_temp_at(settings.weather, now_unix), now);
        sleep(60);
    }
    return 0;
}
