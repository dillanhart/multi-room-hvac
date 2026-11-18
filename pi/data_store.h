/*
 * data_store.h - saved settings: set points (hvac_points.json) and vacations (vacation.json)
 *
 * Flow for every change: validate -> update RAM -> atomic write to disk.
 * A rejected request changes nothing. A failed write keeps the new values in RAM
 * (they're already validated) and marks them unsaved; retry_unsaved() in the main
 * loop keeps trying. Files are only read at startup.
 *
 * Vacation times are Unix seconds in the file and in RAM; the API takes and shows
 * US local time ("12/25/2026 13:00").
 *
 * weather.json: a week of hourly forecast, written by the program once a day.
 * Kept apart from hvac_points.json (user settings) so the daily write can't
 * touch the set points, and swapping in AWAY_POINTS can't lose the season.
 */

#ifndef DATA_STORE_H
#define DATA_STORE_H

#include <optional>
#include <string>
#include <vector>
#include "constants.h"    // hvac_set_points, vacation_block, json, paths

struct vacation_list {
    int next_id = 0;                      // never reused, so an id always means one block
    std::vector<vacation_block> blocks;
};

enum class request_result {
    saved,          // applied and written to disk
    not_saved_yet,  // applied, but the write failed; retry_unsaved() keeps trying
    rejected,       // invalid; nothing changed
};

struct settings_store {
    // paths are fields (not the constants directly) so tests can use a temp dir
    std::string hvac_path = HVAC_POINTS_PATH;
    std::string vacation_path = VACATION_PATH;
    std::string weather_path = WEATHER_PATH;
    hvac_set_points points;
    hvac_mode mode = hvac_mode::auto_select;   // saved with the set points
    vacation_list vacations;
    weather_data weather;
    bool points_unsaved = false;      // covers mode too (same file)
    bool vacations_unsaved = false;
    bool weather_unsaved = false;
};

// ---- validation ----
bool valid_set_points(const hvac_set_points& p);
bool valid_block(const vacation_block& b);  // end after start

// ---- startup ----
// Missing/bad hvac_points.json -> struct defaults, auto mode (a file without "mode"
// is auto). Missing/bad vacation.json -> no vacations; a single bad block is dropped.
// Missing/bad weather.json -> no weather; a single bad hour is dropped.
void load_settings(settings_store& store);

// ---- requests (body already parsed) ----
// {"heat_point": 72}, {"ac_point": 75}, {"night_difference": 3},
// {"mode": "auto" | "heat" | "cool" | "both"}, or several at once.
// Heat alone pushes AC up to heat + MIN_DEAD_BAND if needed; AC alone pushes heat down.
request_result update_set_points(settings_store& store, const json& request);

// {"vacation_start": "12/24/2026 08:00", "vacation_end": "12/27/2026 13:00"}
// end = return time: the house is at the normal set points by then.
request_result add_vacation(settings_store& store, const json& request);

// {"block_id": 3, "vacation_start": ...} and/or "vacation_end"; unspecified times stay
request_result change_vacation(settings_store& store, const json& request);

request_result remove_vacation(settings_store& store, int block_id);

// ---- main loop ----
// drop blocks whose return time has passed (saves if any were dropped)
void remove_finished_vacations(settings_store& store, long long now_unix);

// a fresh forecast from the daily fetch: keeps the hours from the current one
// up to WEATHER_DAYS * 24, stamps fetched_at = now, saves. Never rejected.
request_result update_weather(settings_store& store, std::vector<hourly_temp> hourly, long long now_unix);

// retry any write that failed earlier
void retry_unsaved(settings_store& store);

// away from a block's start until PRECONDITION_LEAD before its end;
// overlapping blocks behave as one, earliest start to latest end
bool is_away(const vacation_list& vacations, long long now_unix);

// ---- API output ----
// current settings (mode as set and as resolved now), vacation times in US format,
// plus whether everything is on disk
json settings_json(const settings_store& store, long long now_unix);

// ---- US local time <-> Unix seconds ----
// "MM/DD/YYYY HH:MM", 24-hour, the Pi's time zone. nullopt for a malformed or
// impossible date (02/30, 13/01) or a time skipped by spring-forward DST.
std::optional<long long> parse_us_time(const std::string& text);
std::string format_us_time(long long unix_time);

#endif
