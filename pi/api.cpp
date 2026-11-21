#include "api.h"

#include <arpa/inet.h>    // inet_pton
#include <charconv>       // std::from_chars
#include <cmath>          // std::round
#include <cstdint>

// ---------------------------------------------------------------- helpers

const char* status_name(hvac_status s) {
    switch (s) {
        case hvac_status::em_heating:  return "em_heating";
        case hvac_status::heating:     return "heating";
        case hvac_status::cooling:     return "cooling";
        case hvac_status::circulating: return "circulating";
        case hvac_status::idle:        return "idle";
    }
    return "idle";
}

const char* fan_name(fan_mode f) {
    return f == fan_mode::on ? "on" : "auto";
}

static api_reply error(int status, const std::string& reason) {
    return {status, {{"error", reason}}};
}

// floats like 71.2f print as 71.19999694824219; one decimal is all a sensor has
static double round1(float x) {
    return std::round(x * 10.0) / 10.0;
}

static json optional_temp(std::optional<float> t) {
    return t ? json(round1(*t)) : json(nullptr);
}

// the whole string must be a number: "12", not "12abc" or ""
template <typename T>
static std::optional<T> parse_number(const std::string& text) {
    T value{};
    auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (err != std::errc() || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

static std::optional<json> parse_body(const std::string& body) {
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

static json sensor_json(const sensor_node& n) {
    if (!n.health.has_last) return nullptr;   // never reported
    return {{"temp", round1(n.data.temp)}, {"humidity", round1(n.data.hum)},
            {"co2", n.data.CO2}, {"trusted", n.health.trusted}};
}

// node i from the last pass, or null before the first pass
static json node_json(const snapshot& snap, size_t i) {
    return i < snap.nodes.size() ? sensor_json(snap.nodes[i]) : json(nullptr);
}

// ---------------------------------------------------------------- GET

api_reply get_status(shared_state& s) {
    std::lock_guard<std::mutex> guard(s.lock);
    return {200, {{"status", status_name(s.control.status)},
                  {"mode", mode_name(s.settings.mode)},
                  {"active_mode", mode_name(s.latest.active_mode)},
                  {"fan", fan_name(s.control.fan)},
                  {"away", s.latest.away}}};
}

api_reply get_readings(shared_state& s, const std::string& view) {
    std::lock_guard<std::mutex> guard(s.lock);
    if (view.empty() || view == "main")
        return {200, {{"main", node_json(s.latest, 0)}}};
    if (view != "all")
        return error(400, "view must be main or all");

    json sensors = json::object();
    for (size_t i = 0; i < SENSOR_COUNT; i++) sensors[SENSOR_NAMES[i]] = node_json(s.latest, i);
    return {200, {{"sensors", sensors}, {"weighted_avg", optional_temp(s.latest.weighted_avg)}}};
}

api_reply get_setpoints(shared_state& s) {
    std::lock_guard<std::mutex> guard(s.lock);
    return {200, set_points_json(s.settings)};
}

api_reply get_vacations(shared_state& s) {
    std::lock_guard<std::mutex> guard(s.lock);
    return {200, vacations_json(s.settings)};
}

static json history_json(const history_entry& e) {
    json sensors = json::object();
    for (size_t i = 0; i < SENSOR_COUNT; i++) {
        const history_sensor& h = e.sensors[i];
        sensors[SENSOR_NAMES[i]] = h.present
            ? json{{"temp", round1(h.temp)}, {"humidity", round1(h.hum)}, {"co2", h.co2}, {"trusted", h.trusted}}
            : json(nullptr);
    }
    return {{"time", e.time}, {"status", status_name(e.status)}, {"mode", mode_name(e.mode)},
            {"sensors", sensors}, {"weighted_avg", optional_temp(e.weighted_avg)}};
}

api_reply get_history(shared_state& s, const std::string& since) {
    long long after = 0;
    if (!since.empty()) {
        std::optional<long long> t = parse_number<long long>(since);
        if (!t) return error(400, "since must be Unix seconds");
        after = *t;
    }
    std::vector<history_entry> entries;
    {
        // copy out, then build the JSON without holding the lock
        std::lock_guard<std::mutex> guard(s.lock);
        entries = s.history.entries_since(after);
    }
    json list = json::array();
    for (const auto& e : entries) list.push_back(history_json(e));
    return {200, {{"entries", list}}};
}

api_reply get_weather(shared_state& s, const std::string& hours, long long now_unix) {
    int count = WEATHER_DEFAULT_HOURS;
    if (!hours.empty()) {
        std::optional<int> n = parse_number<int>(hours);
        if (!n || *n < 1 || *n > WEATHER_DAYS * 24)
            return error(400, "hours must be 1 to " + std::to_string(WEATHER_DAYS * 24));
        count = *n;
    }
    std::lock_guard<std::mutex> guard(s.lock);
    // the stored list keeps past hours until the next fetch: start at the one containing now
    json list = json::array();
    for (const auto& h : s.settings.weather.hourly) {
        if (h.time + 3600 <= now_unix) continue;
        if ((int)list.size() == count) break;
        json j = forecast_hour_json(h);
        j["temp"] = round1(h.temp);
        list.push_back(j);
    }
    return {200, {{"fetched_at", s.settings.weather.fetched_at}, {"hours", list}}};
}

// ---------------------------------------------------------------- writes

api_reply put_setpoints(shared_state& s, const std::string& body) {
    std::optional<json> request = parse_body(body);
    if (!request) return error(400, "body is not valid JSON");

    std::lock_guard<std::mutex> guard(s.lock);
    if (update_set_points(s.settings, *request) == request_result::rejected)
        return error(400, "rejected: unknown field or mode, or the set points break a limit "
                          "(night heat must stay above em heat + dead band, AC at most 90)");
    return {200, set_points_json(s.settings)};
}

api_reply post_vacation(shared_state& s, const std::string& body) {
    std::optional<json> request = parse_body(body);
    if (!request) return error(400, "body is not valid JSON");

    std::lock_guard<std::mutex> guard(s.lock);
    if (add_vacation(s.settings, *request) == request_result::rejected)
        return error(400, "rejected: needs vacation_start and vacation_end in Unix seconds, end after start");
    return {201, vacation_json(s.settings.vacations.blocks.back())};
}

api_reply put_vacation(shared_state& s, const std::string& id, const std::string& body) {
    std::optional<int> block_id = parse_number<int>(id);
    if (!block_id) return error(404, "no such vacation block");
    std::optional<json> request = parse_body(body);
    if (!request) return error(400, "body is not valid JSON");

    std::lock_guard<std::mutex> guard(s.lock);
    switch (change_vacation(s.settings, *block_id, *request)) {
        case request_result::not_found: return error(404, "no such vacation block");
        case request_result::rejected:
            return error(400, "rejected: vacation_start and/or vacation_end in Unix seconds, end after start");
        default: break;
    }
    for (const auto& b : s.settings.vacations.blocks)
        if (b.block_id == *block_id) return {200, vacation_json(b)};
    return error(404, "no such vacation block");   // unreachable: it was just changed
}

api_reply delete_vacation(shared_state& s, const std::string& id) {
    std::optional<int> block_id = parse_number<int>(id);
    if (!block_id) return error(404, "no such vacation block");

    std::lock_guard<std::mutex> guard(s.lock);
    if (remove_vacation(s.settings, *block_id) == request_result::not_found)
        return error(404, "no such vacation block");
    return {204, nullptr};
}

api_reply put_fan(shared_state& s, const std::string& body) {
    std::optional<json> request = parse_body(body);
    if (!request || !request->is_object() || request->size() != 1 ||
        !request->contains("fan") || !(*request)["fan"].is_string())
        return error(400, "body must be {\"fan\": \"on\"} or {\"fan\": \"auto\"}");

    std::string value = (*request)["fan"].get<std::string>();
    if (value != "on" && value != "auto")
        return error(400, "body must be {\"fan\": \"on\"} or {\"fan\": \"auto\"}");

    std::lock_guard<std::mutex> guard(s.lock);
    s.control.fan = value == "on" ? fan_mode::on : fan_mode::auto_select;
    return {200, {{"fan", fan_name(s.control.fan)}}};
}

// ---------------------------------------------------------------- access

bool is_local_address(const std::string& ip) {
    if (ip == "::1") return true;
    // an IPv4 client on a dual-stack socket shows up as ::ffff:a.b.c.d
    std::string v4 = ip.rfind("::ffff:", 0) == 0 ? ip.substr(7) : ip;

    in_addr addr{};
    if (inet_pton(AF_INET, v4.c_str(), &addr) != 1) return false;
    uint32_t a = ntohl(addr.s_addr);
    auto in = [&](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
    return in(0x7F000000, 8)     // 127.0.0.0/8 loopback (the display program)
        || in(0x0A000000, 8)     // 10.0.0.0/8
        || in(0xAC100000, 12)    // 172.16.0.0/12
        || in(0xC0A80000, 16)    // 192.168.0.0/16
        || in(0x64400000, 10);   // 100.64.0.0/10 Tailscale
}
