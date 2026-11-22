#include "api_client.h"

#include "third_party/httplib.h"   // plain HTTP only: no CPPHTTPLIB_OPENSSL_SUPPORT here
#include "display_constants.h"

#include <chrono>
#include <cstdio>
#include <ctime>

static long long mono_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

void api_client::start() {
    worker = std::thread(&api_client::run, this);
}

void api_client::stop() {
    {
        std::lock_guard<std::mutex> guard(lock);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
}

api_snapshot api_client::snapshot() {
    std::lock_guard<std::mutex> guard(lock);
    return latest;
}

void api_client::submit(const api_change& change) {
    {
        std::lock_guard<std::mutex> guard(lock);
        queue.push_back(change);
    }
    wake.notify_all();
}

void api_client::set_snapshot(const api_snapshot& s) {
    std::lock_guard<std::mutex> guard(lock);
    uint64_t version = latest.version + 1;
    latest = s;
    latest.version = version;
}

std::vector<api_change> api_client::take_submitted() {
    std::lock_guard<std::mutex> guard(lock);
    std::vector<api_change> taken;
    taken.swap(queue);
    return taken;
}

// ---------------------------------------------------------------- worker

static void setup(httplib::Client& client) {
    std::chrono::milliseconds timeout(API_TIMEOUT_MS);
    client.set_connection_timeout(timeout);
    client.set_read_timeout(timeout);
    client.set_write_timeout(timeout);
}

// body of a 200 reply, or nullopt (logged only when the error changes, not every poll)
static std::optional<std::string> get(httplib::Client& client, const std::string& path) {
    static std::string last_error;
    httplib::Result res = client.Get(path);
    std::string error;
    if (!res) error = "request failed (" + httplib::to_string(res.error()) + ")";
    else if (res->status != 200) error = "HTTP " + std::to_string(res->status);
    if (error != last_error) {
        if (!error.empty()) std::fprintf(stderr, "api: GET %s: %s\n", path.c_str(), error.c_str());
        else std::fprintf(stderr, "api: reachable again\n");
        last_error = error;
    }
    if (!error.empty()) return std::nullopt;
    return res->body;
}

static void put(httplib::Client& client, const std::string& path, const json& body) {
    httplib::Result res = client.Put(path, body.dump(), "application/json");
    if (!res)
        std::fprintf(stderr, "api: PUT %s: request failed (%s)\n", path.c_str(), httplib::to_string(res.error()).c_str());
    else if (res->status != 200)
        std::fprintf(stderr, "api: PUT %s %s: HTTP %d %s\n", path.c_str(), body.dump().c_str(), res->status,
                     res->body.c_str());
}

bool api_client::poll(bool with_weather) {
    httplib::Client client(API_HOST, API_PORT);
    setup(client);

    std::optional<status_reply> status;
    std::optional<std::string> readings_body, setpoints_body, weather_body;
    if (std::optional<std::string> body = get(client, "/status")) status = parse_status(*body);
    readings_body = get(client, "/readings");
    setpoints_body = get(client, "/setpoints");
    if (with_weather) weather_body = get(client, "/weather?hours=" + std::to_string(FORECAST_HOURS + 1));

    long long now = mono_ms();
    long long now_unix = time(nullptr);
    std::lock_guard<std::mutex> guard(lock);
    api_snapshot next = latest;

    if (status) {
        next.status = status->status;
        if (next.settings) next.settings->fan_on = status->fan_on;
    }
    if (readings_body) {
        next.main_temp = parse_main_temp(*readings_body);   // null / untrusted -> N/A at once
        reading_at_ms = now;
    } else if (now - reading_at_ms > READING_MAX_AGE_MS) {
        next.main_temp.reset();                              // control program gone: don't show an old temp
        next.status = display_status::unknown;
    }
    if (setpoints_body) {
        if (std::optional<settings_edit> s = parse_setpoints(*setpoints_body)) {
            s->fan_on = status ? status->fan_on : (next.settings ? next.settings->fan_on : false);
            next.settings = *s;
        }
    }
    if (weather_body) {
        if (std::optional<std::vector<forecast_entry>> w = parse_weather(*weather_body)) next.forecast = *w;
    }
    // hours that have passed since the last weather fetch
    std::erase_if(next.forecast, [&](const forecast_entry& h) { return h.time + 3600 <= now_unix; });

    bool changed = next.status != latest.status || next.main_temp != latest.main_temp ||
                   next.settings != latest.settings || next.forecast != latest.forecast;
    if (changed) {
        next.version = latest.version + 1;
        latest = next;
    }
    return !latest.forecast.empty();
}

void api_client::run() {
    long long next_poll = 0, next_weather = 0;
    while (true) {
        std::vector<api_change> todo;
        {
            std::unique_lock<std::mutex> guard(lock);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                std::max(0LL, next_poll - mono_ms()));
            wake.wait_until(guard, deadline, [&] { return stopping || !queue.empty(); });
            if (stopping) return;
            todo.swap(queue);
        }

        if (!todo.empty()) {
            httplib::Client client(API_HOST, API_PORT);
            setup(client);
            for (const api_change& c : todo) {
                if (c.setpoints) put(client, "/setpoints", *c.setpoints);
                if (c.fan) put(client, "/fan", *c.fan);
            }
        }

        long long now = mono_ms();
        // one hour more than shown is fetched: when an hour passes, poll() drops it and
        // the pages still have FORECAST_HOURS until the next weather fetch
        bool with_weather = now >= next_weather;
        if (todo.empty() && now < next_poll) continue;   // woken early with nothing to do
        bool have_forecast = poll(with_weather);
        if (with_weather) next_weather = now + (have_forecast ? WEATHER_POLL_INTERVAL_MS : WEATHER_EMPTY_POLL_MS);
        next_poll = now + POLL_INTERVAL_MS;
    }
}
