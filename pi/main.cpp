/*
 * main.cpp - Thermostat control program for Raspberry Pi 3
 *
 * Threads (shared state and locking rule: thermostat.h):
 *   control - every CONTROL_INTERVAL (60 s): read the sensors, decide, switch relays, record history
 *   weather - nightly NWS forecast fetch (only if THERMOSTAT_NWS_GRIDPOINT/_CONTACT are set)
 *   http    - the JSON API on API_PORT (net.cpp / api.cpp); answers at once from the last pass
 *   main    - this one: waits for SIGTERM/SIGINT, then stops the rest and turns the relays off
 *
 * The other files:
 *   control.cpp    - state machine: command, switching rules, relays
 *   thermostat.cpp - one control pass under the lock
 *   sensor.c       - SCD41 CO2/temperature/humidity sensor
 *   validation.cpp - sensor trust
 *   relay.cpp      - Heat/Cool/Fan relays on the GPIO pins
 *   data_store.cpp - saved set points, vacations, weather (JSON files)
 *   weather.cpp    - season, outside temp, fetch schedule, NWS parsing
 *   history.cpp    - 24 h RAM ring of control passes
 *
 * Build: python3 compile.py
 * Run:   ./thermostat_control   (user must be in the "gpio" group)
 * Env:   THERMOSTAT_NWS_GRIDPOINT ("BOU/62,60" style) and THERMOSTAT_NWS_CONTACT
 *        (email or URL for the NWS User-Agent); compile.py loads them from thermostat.env
 */

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>          // getenv
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>        // pthread_sigmask
#include <sys/timex.h>      // ntp_adjtime

#include "constants.h"
#include "control.h"
#include "data_store.h"
#include "net.h"
#include "relay.h"
#include "sensor.h"
#include "thermostat.h"
#include "validation.h"
#include "weather.h"

/*
 * now_ms - milliseconds since boot.
 * Uses the monotonic clock, so it never jumps if the system time changes.
 */
static long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * clock_synced - true once NTP (systemd-timesyncd) has set the wall clock.
 * The Pi has no battery clock: until then time() is the last shutdown time.
 */
static bool clock_synced() {
    struct timex tx = {};
    int state = ntp_adjtime(&tx);
    return state != TIME_ERROR && !(tx.status & STA_UNSYNC);
}

// lets main wake the sleeping threads at shutdown instead of waiting out their sleep
struct stop_signal {
    std::mutex m;
    std::condition_variable cv;
    bool stopping = false;

    // sleep until `until`; true if told to stop meanwhile
    template <class Clock, class Duration>
    bool wait_until(std::chrono::time_point<Clock, Duration> until) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_until(lk, until, [this] { return stopping; });
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(m); stopping = true; }
        cv.notify_all();
    }
};

/*
 * control_loop - one pass every CONTROL_INTERVAL, on a fixed schedule:
 * the next deadline is set from the previous one, not from when a pass ends.
 */
static void control_loop(shared_state& shared, stop_signal& stop) {
    long long started = now_ms();
    int start_status = scd41_start();

    std::vector<sensor_node> nodes = {
        {.name = SENSOR_NAMES[0], .weight_day = 10, .weight_night = 8},
        {.name = SENSOR_NAMES[1], .weight_day = 6, .weight_night = 4},
        {.name = SENSOR_NAMES[2], .weight_day = 4, .weight_night = 8},
    };
    // The SCD41 can't report how long it has been on. If an earlier run left it
    // measuring it's already warm; otherwise (fresh start or error) wait the full warm-up.
    nodes[0].health.started_at = (start_status == SCD41_ALREADY_RUNNING)
                                 ? started - SENSOR_WARMUP
                                 : started;
    // nodes[1..2] (satellites) aren't polled yet: never updated, so never trusted,
    // left out of the averages, and null in the API

    bool was_synced = false;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        long long now = now_ms();
        sensor_result reading = get_sensor_data();
        update_node(nodes[0], reading, local_sensor_status(reading, nodes[0].health.started_at, now), now);

        bool synced = clock_synced();
        if (synced && !was_synced) std::printf("clock synced: history recording starts\n");
        was_synced = synced;

        control_tick(shared, nodes, now, time(nullptr), is_night_now(), synced);

        // a pass that ran late (or a suspended clock) doesn't cause a burst of catch-up passes
        next = std::max(next + std::chrono::milliseconds(CONTROL_INTERVAL), std::chrono::steady_clock::now());
        if (stop.wait_until(next)) return;
    }
}

/*
 * weather_loop - checks once a minute whether a fetch is due (weather_fetch_due);
 * the fetch itself runs without the lock, so a slow NWS never holds up control or the API.
 */
static void weather_loop(shared_state& shared, stop_signal& stop,
                         const std::string& gridpoint, const std::string& contact) {
    long long last_attempt = 0;
    for (;;) {
        if (stop.wait_until(std::chrono::steady_clock::now() + std::chrono::minutes(1))) return;
        if (!clock_synced()) continue;   // the schedule is by local hour and date

        long long now_unix = time(nullptr);
        bool due;
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            due = weather_fetch_due(shared.settings.weather, now_unix, last_attempt);
        }
        if (!due) continue;
        last_attempt = now_unix;

        std::optional<std::string> body = fetch_nws_hourly(gridpoint, contact);
        if (!body) continue;   // logged in fetch_nws_hourly; next try in WEATHER_RETRY
        std::optional<std::vector<forecast_hour>> hours = parse_nws_hourly(*body);
        if (!hours) {
            std::fprintf(stderr, "weather: reply had no usable hours\n");
            continue;
        }
        std::lock_guard<std::mutex> guard(shared.lock);
        update_weather(shared.settings, std::move(*hours), time(nullptr));
        std::printf("weather: %zu hours fetched\n", shared.settings.weather.hourly.size());
    }
}

/*
 * main - claim the relays, start the threads, then wait for a stop signal.
 * Returns 1 if the relay pins can't be claimed.
 */
int main() {
    // Block the stop signals before any thread starts: threads inherit the mask,
    // so only main's sigwait() below ever sees them (otherwise the kernel picks
    // a thread at random and the default action kills the process mid-pass).
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);   // a client hanging up mid-reply mustn't kill the program

    shared_state shared;
    load_settings(shared.settings);
    shared.control = initial_control_state(now_ms());

    // Claim the three relay pins as outputs (all relays start off)
    if (relays_init())
        return 1;

    stop_signal stop;
    std::thread control(control_loop, std::ref(shared), std::ref(stop));

    const char* gridpoint = std::getenv("THERMOSTAT_NWS_GRIDPOINT");
    const char* contact = std::getenv("THERMOSTAT_NWS_CONTACT");
    std::thread weather;
    if (gridpoint && *gridpoint && contact && *contact)
        weather = std::thread(weather_loop, std::ref(shared), std::ref(stop),
                              std::string(gridpoint), std::string(contact));
    else
        std::fprintf(stderr, "weather: THERMOSTAT_NWS_GRIDPOINT / _CONTACT not set, no forecast "
                             "(auto mode stays heat only until weather.json has data)\n");

    // the thermostat runs without the API if the port is taken
    api_server server(shared);
    server.start(API_PORT);

    int sig = 0;
    sigwait(&stop_signals, &sig);
    std::printf("signal %d: stopping\n", sig);

    server.stop();
    stop.stop();
    control.join();
    if (weather.joinable()) weather.join();   // waits out a fetch in progress (WEATHER_TIMEOUT_SEC at most)
    relays_close();   // every relay off, pins released
    return 0;
}
