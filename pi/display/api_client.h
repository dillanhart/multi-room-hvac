/*
 * api_client.h - talks to the control program (pi/api.h) on a worker thread, so a
 * slow or stopped control program never freezes the screen. The UI reads the latest
 * results with snapshot() and queues the Done changes with submit().
 */

#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include "display_logic.h"

struct api_snapshot {
    display_status status = display_status::unknown;
    std::optional<float> main_temp;            // nullopt: N/A
    std::optional<settings_edit> settings;     // set points, mode and fan; nullopt until fetched
    std::vector<forecast_entry> forecast;      // from the hour containing now; empty: N/A
    uint64_t version = 0;                      // changes whenever anything above does
};

struct api_change {
    std::optional<json> setpoints;   // PUT /setpoints body
    std::optional<json> fan;         // PUT /fan body
};

class api_client {
public:
    void start();    // starts the worker thread
    void stop();     // asks it to finish and waits (at most one request timeout)

    api_snapshot snapshot();
    void submit(const api_change& change);   // sent in order by the worker, then everything refreshed

    // preview build (no thread, no network): set what the UI sees, read what it sent
    void set_snapshot(const api_snapshot& s);
    std::vector<api_change> take_submitted();

private:
    void run();
    bool poll(bool with_weather);   // true if there's a forecast afterwards

    std::mutex lock;
    std::condition_variable wake;
    bool stopping = false;
    std::vector<api_change> queue;
    api_snapshot latest;
    long long reading_at_ms = 0;   // monotonic ms of the last good /readings, for READING_MAX_AGE_MS
    std::thread worker;
};

#endif
