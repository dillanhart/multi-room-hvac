/*
 * data_store.cpp - load, validate, update and save the settings files.
 * See data_store.h for the flow.
 */

#include "data_store.h"

#include <algorithm>      // std::remove_if, std::any_of, std::max
#include <cerrno>
#include <cstdio>         // std::rename
#include <fstream>
#include <iostream>
#include <fcntl.h>        // open
#include <unistd.h>       // write, fsync, close, unlink

// ---------------------------------------------------------------- validation

bool valid_set_points(const hvac_set_points& p) {
    // night heat stays clear of MIN_HEAT (em heat) by the dead band
    return p.heat_point - p.night_difference > MIN_HEAT + MIN_DEAD_BAND
        && p.heat_point + MIN_DEAD_BAND <= p.ac_point
        && p.ac_point <= AC_MAX
        && p.night_difference >= 0;
}

bool valid_block(const vacation_block& b) {
    return b.vacation_end > b.vacation_start;
}

// ---------------------------------------------------------------- file I/O

// Write text to path so a power cut leaves either the old file or the new one,
// never half of each: write a temp file, fsync it, rename() it over the old one,
// then fsync the directory so the rename itself is on disk.
static bool write_file_atomic(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted by a signal: try again
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += n;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp.c_str()); return false; }
    if (close(fd) != 0) { unlink(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }

    size_t slash = path.rfind('/');
    std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash + 1);
    int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        fsync(dir_fd);   // best effort: the new file is already in place
        close(dir_fd);
    }
    return true;
}

// nullopt if the file is missing or isn't valid JSON
static std::optional<json> read_json_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    json data = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (data.is_discarded()) {
        std::cerr << path << ": not valid JSON\n";
        return std::nullopt;
    }
    return data;
}

const char* mode_name(hvac_mode m) {
    switch (m) {
        case hvac_mode::auto_select: return "auto";
        case hvac_mode::heat:        return "heat";
        case hvac_mode::cool:        return "cool";
        case hvac_mode::both:        return "both";
    }
    return "auto";
}

std::optional<hvac_mode> mode_from_name(const std::string& name) {
    for (hvac_mode m : {hvac_mode::auto_select, hvac_mode::heat, hvac_mode::cool, hvac_mode::both})
        if (name == mode_name(m)) return m;
    return std::nullopt;
}

// fan_allowed isn't saved: it's only ever false in AWAY_POINTS
static json points_to_json(const hvac_set_points& p, hvac_mode mode) {
    return {{"heat_point", p.heat_point}, {"ac_point", p.ac_point},
            {"night_difference", p.night_difference}, {"mode", mode_name(mode)}};
}

static json weather_to_json(const weather_data& w) {
    json hourly = json::array();
    for (const auto& h : w.hourly) hourly.push_back({{"time", h.time}, {"temp", h.temp}});
    return {{"fetched_at", w.fetched_at}, {"hourly", hourly}};
}

json vacation_json(const vacation_block& b) {
    return {{"block_id", b.block_id}, {"vacation_start", b.vacation_start}, {"vacation_end", b.vacation_end}};
}

static json vacations_to_json(const vacation_list& v) {
    json blocks = json::array();
    for (const auto& b : v.blocks) blocks.push_back(vacation_json(b));
    return {{"next_id", v.next_id}, {"blocks", blocks}};
}

// log only when the saved/unsaved state changes, not on every retry
static void note_save(bool ok, bool& unsaved, const std::string& path) {
    if (!ok && !unsaved) std::cerr << "could not save " << path << "; new values in use, retrying each pass\n";
    if (ok && unsaved)   std::cerr << "saved " << path << " after retrying\n";
    unsaved = !ok;
}

static request_result save_points(settings_store& s) {
    bool ok = write_file_atomic(s.hvac_path, points_to_json(s.points, s.mode).dump(4) + "\n");
    note_save(ok, s.points_unsaved, s.hvac_path);
    return ok ? request_result::saved : request_result::not_saved_yet;
}

static request_result save_vacations(settings_store& s) {
    bool ok = write_file_atomic(s.vacation_path, vacations_to_json(s.vacations).dump(4) + "\n");
    note_save(ok, s.vacations_unsaved, s.vacation_path);
    return ok ? request_result::saved : request_result::not_saved_yet;
}

static request_result save_weather(settings_store& s) {
    bool ok = write_file_atomic(s.weather_path, weather_to_json(s.weather).dump(4) + "\n");
    note_save(ok, s.weather_unsaved, s.weather_path);
    return ok ? request_result::saved : request_result::not_saved_yet;
}

// ---------------------------------------------------------------- startup

struct loaded_points {
    hvac_set_points points;
    hvac_mode mode = hvac_mode::auto_select;
};

static loaded_points load_points(const std::string& path) {
    std::optional<json> data = read_json_file(path);
    if (!data) {
        std::cerr << path << ": missing or unreadable, using default set points\n";
        return {};
    }
    try {
        loaded_points l;
        l.points.heat_point = data->at("heat_point").get<float>();
        l.points.ac_point = data->at("ac_point").get<float>();
        l.points.night_difference = data->at("night_difference").get<float>();
        if (data->contains("mode")) {   // older files have no mode: auto
            std::optional<hvac_mode> m = mode_from_name(data->at("mode").get<std::string>());
            if (!m) {
                std::cerr << path << ": unknown mode, using defaults\n";
                return {};
            }
            l.mode = *m;
        }
        if (valid_set_points(l.points)) return l;
        std::cerr << path << ": set points out of range, using defaults\n";
    } catch (const json::exception& e) {   // missing key or wrong type
        std::cerr << path << ": " << e.what() << ", using default set points\n";
    }
    return {};
}

static vacation_list load_vacations(const std::string& path) {
    vacation_list v;
    std::optional<json> data = read_json_file(path);
    if (!data) return v;   // no file = no vacations planned

    if (!data->is_object() || !data->contains("blocks") || !(*data)["blocks"].is_array()) {
        std::cerr << path << ": no \"blocks\" list, ignoring vacations\n";
        return v;
    }
    if (data->contains("next_id") && (*data)["next_id"].is_number_integer())
        v.next_id = (*data)["next_id"].get<int>();

    for (const auto& item : (*data)["blocks"]) {
        try {
            vacation_block b;
            b.block_id = item.at("block_id").get<int>();
            b.vacation_start = item.at("vacation_start").get<long long>();
            b.vacation_end = item.at("vacation_end").get<long long>();
            bool duplicate = std::any_of(v.blocks.begin(), v.blocks.end(),
                                         [&](const vacation_block& o) { return o.block_id == b.block_id; });
            if (!valid_block(b) || duplicate) {
                std::cerr << path << ": dropping invalid vacation block " << b.block_id << "\n";
                continue;
            }
            v.blocks.push_back(b);
            // a hand edit could leave next_id behind; never hand out an id already used
            v.next_id = std::max(v.next_id, b.block_id + 1);
        } catch (const json::exception& e) {
            std::cerr << path << ": dropping unreadable vacation block: " << e.what() << "\n";
        }
    }
    return v;
}

// keep hours from the one containing now_unix onward, sorted, at most WEATHER_DAYS * 24
static void trim_weather(std::vector<hourly_temp>& hourly, long long now_unix) {
    std::sort(hourly.begin(), hourly.end(),
              [](const hourly_temp& a, const hourly_temp& b) { return a.time < b.time; });
    hourly.erase(std::remove_if(hourly.begin(), hourly.end(),
                                [&](const hourly_temp& h) { return h.time + 3600 <= now_unix; }),
                 hourly.end());
    if (hourly.size() > static_cast<size_t>(WEATHER_DAYS * 24)) hourly.resize(WEATHER_DAYS * 24);
}

static weather_data load_weather(const std::string& path) {
    weather_data w;
    std::optional<json> data = read_json_file(path);
    if (!data) return w;   // never fetched: season unknown, gate skipped

    if (!data->is_object() || !data->contains("hourly") || !(*data)["hourly"].is_array()) {
        std::cerr << path << ": no \"hourly\" list, ignoring weather\n";
        return w;
    }
    if (data->contains("fetched_at") && (*data)["fetched_at"].is_number_integer())
        w.fetched_at = (*data)["fetched_at"].get<long long>();

    for (const auto& item : (*data)["hourly"]) {
        try {
            w.hourly.push_back({item.at("time").get<long long>(), item.at("temp").get<float>()});
        } catch (const json::exception& e) {
            std::cerr << path << ": dropping unreadable hour: " << e.what() << "\n";
        }
    }
    // no trimming by age here: old data keeps its season (no expiry, by design)
    std::sort(w.hourly.begin(), w.hourly.end(),
              [](const hourly_temp& a, const hourly_temp& b) { return a.time < b.time; });
    return w;
}

void load_settings(settings_store& store) {
    loaded_points l = load_points(store.hvac_path);
    store.points = l.points;
    store.mode = l.mode;
    store.vacations = load_vacations(store.vacation_path);
    store.weather = load_weather(store.weather_path);
    store.points_unsaved = false;
    store.vacations_unsaved = false;
    store.weather_unsaved = false;
}

// ---------------------------------------------------------------- requests

// true if request is an object, has at least one key, and every key is in allowed
static bool only_keys(const json& request, std::initializer_list<const char*> allowed) {
    if (!request.is_object() || request.empty()) return false;
    for (const auto& [key, value] : request.items()) {
        bool known = std::any_of(allowed.begin(), allowed.end(),
                                 [&](const char* a) { return key == a; });
        if (!known) return false;   // typo like "heat" shouldn't be silently ignored
    }
    return true;
}

request_result update_set_points(settings_store& store, const json& request) {
    if (!only_keys(request, {"heat_point", "ac_point", "night_difference", "mode"}))
        return request_result::rejected;
    for (const auto& [key, value] : request.items())
        if (key != "mode" && !value.is_number()) return request_result::rejected;

    hvac_mode next_mode = store.mode;
    if (request.contains("mode")) {
        if (!request["mode"].is_string()) return request_result::rejected;
        std::optional<hvac_mode> m = mode_from_name(request["mode"].get<std::string>());
        if (!m) return request_result::rejected;
        next_mode = *m;
    }

    bool has_heat = request.contains("heat_point");
    bool has_ac = request.contains("ac_point");

    hvac_set_points next = store.points;   // work on a copy: RAM only changes if it all passes
    if (has_heat) next.heat_point = request["heat_point"].get<float>();
    if (has_ac) next.ac_point = request["ac_point"].get<float>();
    if (request.contains("night_difference")) next.night_difference = request["night_difference"].get<float>();

    // one side moved into the other: push the other side to keep the dead band
    if (has_heat && !has_ac && next.heat_point + MIN_DEAD_BAND > next.ac_point)
        next.ac_point = next.heat_point + MIN_DEAD_BAND;
    if (has_ac && !has_heat && next.ac_point - MIN_DEAD_BAND < next.heat_point)
        next.heat_point = next.ac_point - MIN_DEAD_BAND;

    if (!valid_set_points(next)) return request_result::rejected;   // includes a push out of range

    store.points = next;
    store.mode = next_mode;
    return save_points(store);
}

// read "vacation_start"/"vacation_end" (Unix seconds) from request into b if present;
// false if one is present but isn't a whole number
static bool read_times(const json& request, vacation_block& b) {
    for (auto [key, field] : {std::pair{"vacation_start", &b.vacation_start},
                              std::pair{"vacation_end", &b.vacation_end}}) {
        if (!request.contains(key)) continue;
        if (!request[key].is_number_integer()) return false;
        *field = request[key].get<long long>();
    }
    return true;
}

request_result add_vacation(settings_store& store, const json& request) {
    if (!only_keys(request, {"vacation_start", "vacation_end"}) ||
        !request.contains("vacation_start") || !request.contains("vacation_end"))
        return request_result::rejected;

    vacation_block b{.block_id = store.vacations.next_id, .vacation_start = 0, .vacation_end = 0};
    if (!read_times(request, b) || !valid_block(b)) return request_result::rejected;

    store.vacations.blocks.push_back(b);
    store.vacations.next_id++;
    return save_vacations(store);
}

static vacation_block* find_block(settings_store& store, int block_id) {
    for (auto& b : store.vacations.blocks)
        if (b.block_id == block_id) return &b;
    return nullptr;
}

request_result change_vacation(settings_store& store, int block_id, const json& request) {
    vacation_block* existing = find_block(store, block_id);
    if (!existing) return request_result::not_found;
    if (!only_keys(request, {"vacation_start", "vacation_end"})) return request_result::rejected;

    vacation_block next = *existing;
    if (!read_times(request, next) || !valid_block(next)) return request_result::rejected;

    *existing = next;
    return save_vacations(store);
}

request_result remove_vacation(settings_store& store, int block_id) {
    auto& blocks = store.vacations.blocks;
    auto it = std::remove_if(blocks.begin(), blocks.end(),
                             [&](const vacation_block& b) { return b.block_id == block_id; });
    if (it == blocks.end()) return request_result::not_found;
    blocks.erase(it, blocks.end());
    return save_vacations(store);
}

// ---------------------------------------------------------------- main loop

void remove_finished_vacations(settings_store& store, long long now_unix) {
    auto& blocks = store.vacations.blocks;
    auto it = std::remove_if(blocks.begin(), blocks.end(),
                             [&](const vacation_block& b) { return b.vacation_end <= now_unix; });
    if (it == blocks.end()) return;   // nothing finished: no write
    blocks.erase(it, blocks.end());
    save_vacations(store);
}

request_result update_weather(settings_store& store, std::vector<hourly_temp> hourly, long long now_unix) {
    trim_weather(hourly, now_unix);
    store.weather = {.fetched_at = now_unix, .hourly = std::move(hourly)};
    return save_weather(store);
}

void retry_unsaved(settings_store& store) {
    if (store.points_unsaved) save_points(store);
    if (store.vacations_unsaved) save_vacations(store);
    if (store.weather_unsaved) save_weather(store);
}

bool is_away(const vacation_list& vacations, long long now_unix) {
    // any block covering now is enough, so overlaps need no merging
    return std::any_of(vacations.blocks.begin(), vacations.blocks.end(), [&](const vacation_block& b) {
        return b.vacation_start <= now_unix && now_unix < b.vacation_end - PRECONDITION_LEAD;
    });
}

// ---------------------------------------------------------------- API output

json set_points_json(const settings_store& store) {
    return points_to_json(store.points, store.mode);
}

json vacations_json(const settings_store& store) {
    json blocks = json::array();
    for (const auto& b : store.vacations.blocks) blocks.push_back(vacation_json(b));
    return {{"blocks", blocks}};
}
