#include "thermostat.h"
#include "weather.h"      // resolve_mode, season_from_weather, outside_temp_at

history_entry make_history_entry(const std::vector<sensor_node>& nodes, std::optional<float> weighted_avg,
                                 hvac_status status, hvac_mode mode, long long now_unix) {
    history_entry e;
    e.time = now_unix;
    e.status = status;
    e.mode = mode;
    e.weighted_avg = weighted_avg;
    for (size_t i = 0; i < nodes.size() && i < e.sensors.size(); i++) {
        const sensor_node& n = nodes[i];
        if (!n.health.has_last) continue;   // never reported: stays "not present"
        e.sensors[i] = {.present = true, .temp = n.data.temp, .hum = n.data.hum,
                        .co2 = n.data.CO2, .trusted = n.health.trusted};
    }
    return e;
}

void control_tick(shared_state& s, const std::vector<sensor_node>& nodes,
                  long long now_ms, long long now_unix, bool is_night, bool clock_synced) {
    std::lock_guard<std::mutex> guard(s.lock);

    // before NTP sync the date can be anything: keep the vacations, don't go away
    if (clock_synced) remove_finished_vacations(s.settings, now_unix);
    bool away = clock_synced && is_away(s.settings.vacations, now_unix);
    retry_unsaved(s.settings);

    // away: fixed low heat point, no fan (manual fan-on is kept, just held off);
    // mode/season and weather gate still apply
    const hvac_set_points& points = away ? AWAY_POINTS : s.settings.points;
    hvac_mode active = resolve_mode(s.settings.mode, season_from_weather(s.settings.weather));
    std::optional<float> outside = clock_synced ? outside_temp_at(s.settings.weather, now_unix)
                                                : std::nullopt;

    control_step(s.control, nodes, points, is_night, active, outside, now_ms);

    s.latest = {.nodes = nodes, .weighted_avg = weighted_temperature(nodes, is_night),
                .away = away, .active_mode = active};
    if (clock_synced)
        s.history.add(make_history_entry(nodes, s.latest.weighted_avg, s.control.status,
                                         s.settings.mode, now_unix));
}
