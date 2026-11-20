#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <vector>
#include "sensor.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// all times in milliseconds
constexpr int SEC = 1000;
constexpr int MIN = 60 * SEC;

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
constexpr float MIN_DEAD_BAND = 2;
constexpr float AC_MAX = 90;                       // highest accepted AC set point

// temp delta for fan to turn on and turn off
constexpr float FAN_TRIGGER = 2;
constexpr float FAN_CLEAR = 1;

// CO2 circulation (co2_fan_call): a trusted room over CO2_TRIGGER ppm and over
// CO2_START_RATIO x the weighted average CO2 starts the fan; it keeps running while
// a trusted room is over CO2_STOP_RATIO x the average (the gap stops flip-flopping).
// A room with no trusted reading counts as CO2_FILL_PPM in the average.
constexpr int CO2_TRIGGER = 1000;
constexpr float CO2_START_RATIO = 1.2f;
constexpr float CO2_STOP_RATIO = 1.1f;
constexpr int CO2_FILL_PPM = 700;

// weather gate: no cooling when outside is this far below the active AC point,
// no heating when it's this far above the active heat point (em heat never gated)
constexpr float WEATHER_GATE_MARGIN = 5;

// season lockout (auto mode), from the average daily low over the stored week
constexpr float WINTER_AVG_LOW = 55;   // average low below this: heating only
constexpr float SUMMER_AVG_LOW = 65;   // average low above this: cooling only
constexpr int WEATHER_DAYS = 7;                           // hourly forecast kept in weather.json
constexpr long long WEATHER_REFRESH = 24 * 60 * 60;       // fetch once a day (Unix seconds)
constexpr int FULL_DAY_HOURS = 20;     // a date needs this many hours to count toward the average

// saved settings live next to the program on the Pi (~/thermostat, see deploy.sh)
#define DATA_DIR "/home/thermostat/thermostat/"
constexpr const char* HVAC_POINTS_PATH = DATA_DIR "hvac_points.json";
constexpr const char* VACATION_PATH = DATA_DIR "vacation.json";
constexpr const char* WEATHER_PATH = DATA_DIR "weather.json";

// vacations: Unix seconds (wall clock), not ms
constexpr long long PRECONDITION_LEAD = 2 * 60 * 60;  // away mode ends this long before the return time

enum class sensor_status { ok, warming, error };

// keep metadata for the sensor to see if it is reading properly
struct sensor_health {
    sensor_status status = sensor_status::error;
    long long started_at = 0;      // local sensor only: when it started measuring (set from scd41_start)
    long long last_read_at = 0;    // time of the last successful read
    float last_temp = 0;           // that read's temperature, for the jump check
    bool has_last = false;         // false until the first successful read
    int fails_in_row = 0;
    int consistent = 0;            // good readings in a row; negative after a frozen sensor
    int identical_in_row = 0;      // readings in a row exactly matching the one before (incl. this one)
    bool trusted = false;          // the only thing control logic checks
};


struct sensor_node {
    std::string name;
    sensor_result data;
    int weight_day;
    int weight_night;
    sensor_health health;
};

struct hvac_set_points {
    float heat_point = 70;
    float ac_point = 72;
    float night_difference = 2;   // whole band drops by this at night
    bool fan_allowed = true;      // not saved or set through the API; false only in AWAY_POINTS
};

// used in place of the user's set points while away. Deliberately skips
// valid_set_points (62 - 0 is not > MIN_HEAT + MIN_DEAD_BAND): it's a fixed
// constant, not user input. Order still holds: Honeywell backstop < MIN_HEAT (em heat) < 62.
constexpr hvac_set_points AWAY_POINTS = {.heat_point = 62, .ac_point = 90, .night_difference = 0, .fan_allowed = false};

// which equipment may run. auto_select = decided by the season (weather);
// the rest are manual and stay until set back to auto. Saved in hvac_points.json.
enum class hvac_mode { auto_select, heat, cool, both };

struct hourly_temp {
    long long time;   // start of the hour, Unix seconds
    float temp;       // forecast outside temp, F
};

struct weather_data {
    long long fetched_at = 0;          // Unix seconds; 0 = never fetched
    std::vector<hourly_temp> hourly;   // sorted by time, up to WEATHER_DAYS * 24 entries
};

struct vacation_block {
    int block_id;
    long long vacation_start;   // departure, Unix seconds
    long long vacation_end;     // return: house is back at normal set points by then
};


#endif
