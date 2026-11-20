/*
 * net.h - everything that touches the network, on top of cpp-httplib:
 * the HTTP API server (handlers in api.cpp) and the NWS forecast fetch.
 *
 * httplib.h is ~24k lines, so only net.cpp includes it; this header just
 * forward-declares the server, and the rest of the program never compiles it.
 */

#ifndef NET_H
#define NET_H

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include "thermostat.h"   // shared_state

namespace httplib { class Server; }

// The API server: runs on its own thread (plus httplib's worker pool).
class api_server {
public:
    explicit api_server(shared_state& s);
    ~api_server();   // stops the server if still running

    // bind 0.0.0.0:port and start serving; false if the port can't be bound
    bool start(int port);
    void stop();

private:
    shared_state& state;
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
};

// GET https://api.weather.gov/gridpoints/<gridpoint>/forecast/hourly
// gridpoint: "BOU/62,60" style; contact: put in the User-Agent, which NWS requires.
// The body on HTTP 200; nullopt on any failure (logged). Each connect/read gives up
// after WEATHER_TIMEOUT_SEC.
std::optional<std::string> fetch_nws_hourly(const std::string& gridpoint, const std::string& contact);

#endif
