#include "control.h"
#include "constants.h"    // sensor_node, MIN_HEAT, FAN_TRIGGER, cycle timings
#include "relay.h"        // relay_set, all_off

#include <algorithm>      // std::minmax_element
#include <ctime>          // std::time, std::localtime
#include <optional>
#include <vector>

bool is_night_now() {
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    return local_time->tm_hour < 8;
}


// calculate the weighted temp from trusted sensors;
// std::nullopt if no sensor is trusted
static std::optional<float> calculate_weighted_temperature(const std::vector<sensor_node>& nodes, bool is_night) {
    float total_weighted_temp = 0.0f;
    float total_weight = 0.0f;

    for (const auto& node : nodes) {
        if (node.health.trusted) {
            int weight = is_night ? node.weight_night : node.weight_day;
            total_weighted_temp += (node.data.temp * weight);
            total_weight += weight;
        }
    }

    if (total_weight == 0) return std::nullopt;
    return total_weighted_temp / total_weight;
}


control_state initial_control_state(long long now) {
    control_state state;
    state.cycle_start = now;
    state.cycle_end = now;
    return state;
}


bool co2_fan_call(const std::vector<sensor_node>& sensors, bool is_night, bool running) {
    // weighted average; a room without a trusted reading counts as a typical room
    float total = 0.0f;
    float total_weight = 0.0f;
    bool any_trusted = false;
    for (const auto& node : sensors) {
        int weight = node.weight_day;
        float ppm = node.health.trusted ? node.data.CO2 : CO2_FILL_PPM;
        total += ppm * weight;
        total_weight += weight;
        any_trusted = any_trusted || node.health.trusted;
    }
    if (!any_trusted || total_weight == 0) return false;
    float average = total / total_weight;

    // only a real (trusted) reading can start or hold the fan
    for (const auto& node : sensors) {
        if (!node.health.trusted) continue;
        float ppm = node.data.CO2;
        bool wants_fan = running ? ppm > CO2_STOP_RATIO * average
                                 : ppm > CO2_TRIGGER && ppm > CO2_START_RATIO * average;
        if (wants_fan) return true;
    }
    return false;
}


hvac_command get_HVAC_command(const std::vector<sensor_node>& sensors, const hvac_set_points& points,
                              bool is_night, hvac_mode allowed, std::optional<float> outside_temp,
                              fan_mode fan, bool co2_fan) {

    // manual fan and the CO2 rule; away (fan_allowed = false) holds both off
    bool fan_wanted = points.fan_allowed && (fan == fan_mode::on || co2_fan);

    // 1. Get the aggregated data; no valid sensors means no decision
    std::optional<float> weighted = calculate_weighted_temperature(sensors, is_night);
    // the manual fan doesn't need temperatures, so it outranks no_data
    if (!weighted) return fan_wanted ? hvac_command::fan : hvac_command::no_data;
    float weighted_temp = *weighted;

    // 2. Extract raw temps for the "Fan" check
    std::vector<float> raw_temps;
    for(const auto& s : sensors) {
        if(s.health.trusted) raw_temps.push_back(s.data.temp);
    }

    // 3. Active set points: at night the whole band drops
    float shift = is_night ? points.night_difference : 0;
    float heat_set = points.heat_point - shift;
    float ac_set = points.ac_point - shift;

    // 4. What may run: mode/season, then the weather gate (skipped if outside temp unknown)
    bool cool_ok = allowed != hvac_mode::heat &&
                   !(outside_temp && *outside_temp <= ac_set - WEATHER_GATE_MARGIN);
    bool heat_ok = allowed != hvac_mode::cool &&
                   !(outside_temp && *outside_temp >= heat_set + WEATHER_GATE_MARGIN);

    // 5. Decision Logic (em heat is never gated)
    if (weighted_temp < MIN_HEAT) return hvac_command::emergency_heat;
    if (weighted_temp > ac_set && cool_ok) return hvac_command::cool;
    if (weighted_temp < heat_set && heat_ok) return hvac_command::heat;

    if (fan_wanted) return hvac_command::fan;
    if (points.fan_allowed && !raw_temps.empty()) {
        auto [min_it, max_it] = std::minmax_element(raw_temps.begin(), raw_temps.end());
        if (*max_it - *min_it > FAN_TRIGGER) return hvac_command::fan;
    }

    return hvac_command::none;
}

enum class equipment { none, furnace, ac };

static equipment equipment_for(hvac_status s) {
    switch (s) {
        case hvac_status::heating:
        case hvac_status::em_heating:  return equipment::furnace;
        case hvac_status::cooling:     return equipment::ac;
        default:                       return equipment::none;  // idle, circulating
    }
}

// true if going from a to b means switching between furnace and AC
static bool conflicting(hvac_status a, hvac_status b) {
    equipment ea = equipment_for(a), eb = equipment_for(b);
    return ea != equipment::none && eb != equipment::none && ea != eb;
}

// the status a command is asking for; no_data asks for idle
// (no default: -Wswitch warns if a new command isn't mapped here)
static hvac_status status_for(hvac_command command) {
    switch (command) {
        case hvac_command::emergency_heat: return hvac_status::em_heating;
        case hvac_command::heat:           return hvac_status::heating;
        case hvac_command::cool:           return hvac_status::cooling;
        case hvac_command::fan:            return hvac_status::circulating;
        case hvac_command::none:
        case hvac_command::no_data:        return hvac_status::idle;
    }
    return hvac_status::idle;  // unreachable; satisfies -Wreturn-type
}

// Rules, in order:
// - em heat starts at once from anything: no min-on, min-off or max-run.
// - heat/cool stop at MAX_RUN_TIME, to circulating if the fan is called, else idle.
//   (em_heating -> heating keeps cycle_start, so that heating counts from the em heat start)
// - idle -> circulating at once (fan skips min-off); idle -> heat/cool waits out min-off.
// - circulating runs at least MIN_ON_TIME (only em heat cuts it short); then heat/cool
//   still wait out min-off.
// - heat/cool/em heat finish min-on, then go where the command says: idle, or
//   circulating when the fan is called (a fan call never cuts a run short).
// - a heat <-> cool call stops the running side at once (skips min-on); the other side
//   then waits out min-off from idle.
hvac_status test_switch (hvac_status sys_status, hvac_command command, long long cycle_start, long long now_ms, long long cycle_end){
    hvac_status requested = status_for(command);

    if (requested == hvac_status::em_heating) return hvac_status::em_heating;

    bool min_on_done = now_ms > cycle_start + MIN_ON_TIME;
    bool min_off_done = now_ms > cycle_end + MIN_OFF_TIME;
    bool wants_equipment = equipment_for(requested) != equipment::none;

    bool capped = sys_status == hvac_status::heating || sys_status == hvac_status::cooling;
    if (capped && now_ms > cycle_start + MAX_RUN_TIME)
        return requested == hvac_status::circulating ? hvac_status::circulating : hvac_status::idle;

    if (requested == sys_status) return sys_status;

    switch (sys_status) {
        case hvac_status::idle:
            if (requested == hvac_status::circulating || min_off_done) return requested;
            return hvac_status::idle;

        case hvac_status::circulating:
            if (!min_on_done) return hvac_status::circulating;
            if (wants_equipment && !min_off_done) return hvac_status::circulating;
            return requested;

        case hvac_status::heating:
        case hvac_status::em_heating:
        case hvac_status::cooling:
            if (conflicting(sys_status, requested)) return hvac_status::idle;
            if (!min_on_done) return sys_status;
            return requested;
    }
    return sys_status;  // unreachable; satisfies -Wreturn-type
}

// switch the relays to match new_status; only if that worked, stamp the cycle timers
// (furnace/AC stopping stamps cycle_end, any non-idle status stamps cycle_start;
// em_heating -> heating keeps cycle_start so MAX_RUN_TIME keeps counting from
// when the furnace started)
// returns 0 on success, non-zero on failure (timers untouched, caller retries)
int apply_status(hvac_status old_status, hvac_status new_status, long long now, long long& cycle_start, long long& cycle_end){
    int relay_success = -1;
    switch (new_status){
        case hvac_status::idle:        relay_success = all_off(); break;
        case hvac_status::cooling:
            // fan may still be on from circulating; drop it (and heat, defensively) first
            relay_success = relay_set(RELAY_HEAT, 0);
            if (relay_success == 0) relay_success = relay_set(RELAY_FAN, 0);
            if (relay_success == 0) relay_success = relay_set(RELAY_COOL, 1);
            break;
        case hvac_status::em_heating:
        case hvac_status::heating:
            // cool can still be on when em heat takes over from cooling: without
            // this the heat relay is refused (RELAY_INTERLOCK) on every pass
            relay_success = relay_set(RELAY_COOL, 0);
            if (relay_success == 0) relay_success = relay_set(RELAY_FAN, 0);
            if (relay_success == 0) relay_success = relay_set(RELAY_HEAT, 1);
            break;
        case hvac_status::circulating:
            // heat/cool may still be on from the previous status; drop them first
            relay_success = all_off();
            if (relay_success == 0) relay_success = relay_set(RELAY_FAN, 1);
            break;
    }
    if (relay_success != 0) return relay_success;

    // the furnace/AC just stopped (to idle or to circulating): MIN_OFF_TIME starts now
    if (equipment_for(old_status) != equipment::none && equipment_for(new_status) == equipment::none)
        cycle_end = now;
    if (new_status != hvac_status::idle &&
        !(old_status == hvac_status::em_heating && new_status == hvac_status::heating))
        cycle_start = now;
    return 0;
}

hvac_command control_step(control_state& state, const std::vector<sensor_node>& sensors,
                          const hvac_set_points& points, bool is_night, hvac_mode allowed,
                          std::optional<float> outside_temp, long long now) {
    // away clears the CO2 memory, so a return home starts from the start line, not the stop line
    state.co2_circulating = points.fan_allowed &&
                            co2_fan_call(sensors, is_night, state.co2_circulating);

    hvac_command command = get_HVAC_command(sensors, points, is_night, allowed, outside_temp,
                                            state.fan, state.co2_circulating);
    hvac_status next = test_switch(state.status, command, state.cycle_start, now, state.cycle_end);

    if (next != state.status &&
        apply_status(state.status, next, now, state.cycle_start, state.cycle_end) == 0)
        state.status = next;
    return command;
}
