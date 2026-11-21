/*
 * data_store.h - saved settings: set points (hvac_points.json) and vacations (vacation.json)
 *
 * Flow for every change: validate -> update RAM -> atomic write to disk.
 * A rejected request changes nothing. A failed write keeps the new values in RAM
 * (they're already validated) and marks them unsaved; retry_unsaved() in the main
 * loop keeps trying. Files are only read at startup.
 *
 * Vacation times are Unix seconds everywhere: file, RAM and API. The website
 * converts to and from local time.
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
    not_found,      // no vacation block with that id; nothing changed
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

// {"vacation_start": 1798128000, "vacation_end": 1798750800} (Unix seconds)
// end = return time: the house is at the normal set points by then.
// On success the new block is store.vacations.blocks.back().
request_result add_vacation(settings_store& store, const json& request);

// {"vacation_start": ...} and/or {"vacation_end": ...}; unspecified times stay
request_result change_vacation(settings_store& store, int block_id, const json& request);

request_result remove_vacation(settings_store& store, int block_id);

// ---- main loop ----
// drop blocks whose return time has passed (saves if any were dropped)
void remove_finished_vacations(settings_store& store, long long now_unix);

// a fresh forecast from the nightly fetch: keeps the hours from the current one
// up to WEATHER_DAYS * 24, stamps fetched_at = now, saves. Never rejected.
request_result update_weather(settings_store& store, std::vector<forecast_hour> hourly, long long now_unix);

// retry any write that failed earlier
void retry_unsaved(settings_store& store);

// away from a block's start until PRECONDITION_LEAD before its end;
// overlapping blocks behave as one, earliest start to latest end
bool is_away(const vacation_list& vacations, long long now_unix);

// ---- API output ----
// {"heat_point", "ac_point", "night_difference", "mode"}: the mode as set, not resolved
json set_points_json(const settings_store& store);
// {"block_id", "vacation_start", "vacation_end"}
json vacation_json(const vacation_block& b);
// {"blocks": [...]}
json vacations_json(const settings_store& store);
// {"time", "temp", "condition", "is_day", "short_forecast"}: one hour, as in weather.json
json forecast_hour_json(const forecast_hour& h);

// "auto" / "heat" / "cool" / "both"
const char* mode_name(hvac_mode m);
std::optional<hvac_mode> mode_from_name(const std::string& name);

#endif
