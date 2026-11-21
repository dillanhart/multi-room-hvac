/*
 * net.cpp - cpp-httplib wiring: API routes and the NWS fetch. See net.h.
 * Try the API with:  curl http://<pi-address>:8080/status
 */

#define CPPHTTPLIB_OPENSSL_SUPPORT   // HTTPS for api.weather.gov (needs libssl-dev)
#include "third_party/httplib.h"

#include "net.h"
#include "api.h"
#include "constants.h"

#include <cstdio>
#include <ctime>         // time

// copy an api_reply into httplib's response
static void send(httplib::Response& res, const api_reply& reply) {
    res.status = reply.status;
    if (!reply.body.is_null()) res.set_content(reply.body.dump(), "application/json");
}

api_server::api_server(shared_state& s) : state(s), server(std::make_unique<httplib::Server>()) {
    httplib::Server& svr = *server;
    shared_state& st = state;

    // a slow or stuck client can't hold a worker thread forever
    svr.set_read_timeout(5);
    svr.set_write_timeout(5);
    svr.set_payload_max_length(16 * 1024);   // the biggest real body is a few dozen bytes

    // only clients on the LAN, loopback (display) or the tailnet
    svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (is_local_address(req.remote_addr)) return httplib::Server::HandlerResponse::Unhandled;
        send(res, {403, {{"error", "not a local address"}}});
        return httplib::Server::HandlerResponse::Handled;
    });

    svr.Get("/status", [&st](const httplib::Request&, httplib::Response& res) {
        send(res, get_status(st));
    });
    svr.Get("/readings", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, get_readings(st, req.get_param_value("view")));
    });
    svr.Get("/setpoints", [&st](const httplib::Request&, httplib::Response& res) {
        send(res, get_setpoints(st));
    });
    svr.Put("/setpoints", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, put_setpoints(st, req.body));
    });
    svr.Get("/vacations", [&st](const httplib::Request&, httplib::Response& res) {
        send(res, get_vacations(st));
    });
    svr.Post("/vacations", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, post_vacation(st, req.body));
    });
    svr.Put("/vacations/:id", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, put_vacation(st, req.path_params.at("id"), req.body));
    });
    svr.Delete("/vacations/:id", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, delete_vacation(st, req.path_params.at("id")));
    });
    svr.Put("/fan", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, put_fan(st, req.body));
    });
    svr.Get("/history", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, get_history(st, req.get_param_value("since")));
    });
    svr.Get("/weather", [&st](const httplib::Request& req, httplib::Response& res) {
        send(res, get_weather(st, req.get_param_value("hours"), time(nullptr)));
    });

    // unknown path or method: JSON like every other error
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty()) res.set_content(json{{"error", "not found"}}.dump(), "application/json");
    });
}

api_server::~api_server() { stop(); }

bool api_server::start(int port) {
    if (!server->bind_to_port("0.0.0.0", port)) {
        std::fprintf(stderr, "API: can't bind port %d\n", port);
        return false;
    }
    thread = std::thread([this] { server->listen_after_bind(); });
    std::printf("API listening on port %d\n", port);
    return true;
}

void api_server::stop() {
    server->stop();
    if (thread.joinable()) thread.join();
}

std::optional<std::string> fetch_nws_hourly(const std::string& gridpoint, const std::string& contact) {
    httplib::Client client("https://api.weather.gov");
    client.set_connection_timeout(WEATHER_TIMEOUT_SEC);
    client.set_read_timeout(WEATHER_TIMEOUT_SEC);

    httplib::Headers headers = {
        {"User-Agent", "(home-thermostat, " + contact + ")"},
        {"Accept", "application/geo+json"},
    };
    httplib::Result res = client.Get("/gridpoints/" + gridpoint + "/forecast/hourly", headers);
    if (!res) {
        std::fprintf(stderr, "weather: request failed (%s)\n", httplib::to_string(res.error()).c_str());
        return std::nullopt;
    }
    if (res->status != 200) {
        // 404 here can mean NWS redefined its grid: look the gridpoint up again
        std::fprintf(stderr, "weather: HTTP %d\n", res->status);
        return std::nullopt;
    }
    return res->body;
}
