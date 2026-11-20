/*
 * api.h - what each HTTP endpoint does, without the HTTP: every handler takes the
 * shared state plus the request's pieces and returns a status code and JSON body.
 * net.cpp wires them to cpp-httplib routes. Kept apart so the tests can call
 * them without a server, and without compiling httplib.
 *
 * Every handler takes shared_state::lock itself. GETs answer from the last control
 * pass (up to CONTROL_INTERVAL old); writes change settings now and the relays
 * follow at the next pass.
 *
 *   GET    /status                   {"status","mode","active_mode","fan","away"}
 *   GET    /readings?view=main|all   main (default): {"main": sensor|null}
 *                                    all: {"sensors": {"main","office","bed"}, "weighted_avg"}
 *   GET    /setpoints                {"heat_point","ac_point","night_difference","mode"}
 *   PUT    /setpoints                any subset of those -> the set points after any push
 *   GET    /vacations                {"blocks": [{"block_id","vacation_start","vacation_end"}]}
 *   POST   /vacations                {"vacation_start","vacation_end"} -> 201, the new block
 *   PUT    /vacations/{id}           either or both times -> the updated block
 *   DELETE /vacations/{id}           -> 204
 *   PUT    /fan                      {"fan": "on"|"auto"} -> {"fan"}
 *   GET    /history?since=<unix>     {"entries": [...]}, oldest first, strictly after since
 *
 * sensor = {"temp","humidity","co2","trusted"}; null if it has never reported.
 * Times are Unix seconds. Errors: 400 or 404 with {"error": "<reason>"}.
 * A change that's applied but not yet written to disk still succeeds (it's retried).
 */

#ifndef API_H
#define API_H

#include <optional>
#include <string>
#include "constants.h"    // json
#include "thermostat.h"   // shared_state

struct api_reply {
    int status = 200;
    json body;            // null with 204: no body
};

api_reply get_status(shared_state& s);
api_reply get_readings(shared_state& s, const std::string& view);   // "" = main
api_reply get_setpoints(shared_state& s);
api_reply put_setpoints(shared_state& s, const std::string& body);
api_reply get_vacations(shared_state& s);
api_reply post_vacation(shared_state& s, const std::string& body);
api_reply put_vacation(shared_state& s, const std::string& id, const std::string& body);
api_reply delete_vacation(shared_state& s, const std::string& id);
api_reply put_fan(shared_state& s, const std::string& body);
api_reply get_history(shared_state& s, const std::string& since);   // "" = everything

// loopback, private LAN ranges (10/8, 172.16/12, 192.168/16) or Tailscale (100.64/10)
bool is_local_address(const std::string& ip);

// names used in the JSON
const char* status_name(hvac_status s);
const char* fan_name(fan_mode f);

#endif
