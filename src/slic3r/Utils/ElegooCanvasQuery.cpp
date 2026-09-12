// The networking half of the CANVAS support declared in ElegooCanvas.hpp.
//
// Kept apart from ElegooCanvas.cpp on purpose: everything in that file is pure
// string/JSON handling, so the unit tests can link it without dragging in
// boost::beast and a WebSocket stack. Only this translation unit talks to a
// printer, and nothing in the test suite links it.

#include "ElegooCanvas.hpp"

#include "WebSocketClient.hpp"

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <chrono>

namespace Slic3r {

namespace {

// The same endpoint ElegooLink::upload already uses to start a print.
const char* const ELEGOO_WS_PORT = "3030";
const char* const ELEGOO_WS_PATH = "/websocket";
// Command 324 asks for the material state of the attached CANVAS units.
const int         CANVAS_CMD = 324;
// The printer answers the request with an ACK first; the material payload
// follows. Read a few frames before giving up, which also covers unrelated
// status pushes arriving in between.
const int         CANVAS_MAX_FRAMES   = 5;
const int         CANVAS_RECV_TIMEOUT = 5; // seconds, per frame

} // namespace

bool canvas_query(const std::string& host, CanvasInfo& info, std::string& error)
{
    info = CanvasInfo();

    WebSocketClient ws;
    try {
        ws.connect(host, ELEGOO_WS_PORT, ELEGOO_WS_PATH);
    } catch (const std::exception& e) {
        error = std::string("WebSocket connect failed: ") + e.what();
        return false;
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string request_id = boost::uuids::to_string(boost::uuids::random_generator()());

    const nlohmann::json request = {
        {"Id", ""},
        {"Data", {
            {"Cmd", CANVAS_CMD},
            {"Data", nlohmann::json::object()},
            {"RequestID", request_id},
            {"MainboardID", ""},
            {"TimeStamp", now_ms},
            {"From", 1}
        }}
    };

    try {
        ws.send(request.dump());
    } catch (const std::exception& e) {
        error = std::string("WebSocket send failed: ") + e.what();
        return false;
    }

    std::string last_error = "no CANVAS payload in reply";
    for (int attempt = 0; attempt < CANVAS_MAX_FRAMES; ++attempt) {
        std::string msg;
        try {
            msg = ws.receive(CANVAS_RECV_TIMEOUT);
        } catch (const std::exception& e) {
            error = std::string("WebSocket receive failed: ") + e.what();
            return false;
        }

        BOOST_LOG_TRIVIAL(trace) << "canvas_query: frame #" << attempt << " len=" << msg.size();

        std::string parse_error;
        if (canvas_parse_payload(msg, info, parse_error))
            return true;
        last_error = parse_error;
    }

    error = last_error;
    return false;
}

} // namespace Slic3r
