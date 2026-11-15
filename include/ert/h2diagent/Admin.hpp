/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file Admin.hpp
 * @brief Admin interface (/admin/v1): documents and request routing.
 *
 * The admin HTTP/2 server (--admin-port) is a thin transport over handle():
 * every answer is built here from snapshots given by the gateway, so the
 * routing and the documents are unit-tested without sockets.
 *
 *   GET /admin/v1/health                  200 {"status":"healthy"}
 *   GET /admin/v1/peers                   200 {"connections":[...]}
 *   GET /admin/v1/configuration           200 running options
 *   GET /admin/v1/logging                 200 current level (text)
 *   PUT /admin/v1/logging?level=<level>   200 / 400
 *
 * Another method on a known resource is 405; an unknown path is 404. The
 * interface is not authenticated (as the h2agent one).
 */

#pragma once

#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercomm/Peer.hpp>
#include <ert/h2diagent/Gateway.hpp>
#include <ert/tracing/Logger.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace ert {
namespace h2diagent {
namespace admin {

/** One Diameter connection, as listed by GET /admin/v1/peers. */
struct ConnectionInfo {
    std::string leg;  // "client" (opened by the gateway) | "server" (accepted)
    int index{0};     // client: pool position; server: listing position
    std::string transport;
    std::string state;  // closed | reconnecting | waitCEA | waitCER | open | closing
    std::string localAddress;
    std::string peerAddress;
    std::string peerOriginHost;
    std::string peerOriginRealm;
};

inline std::string transportName(diametercomm::Transport t) {
    return t == diametercomm::Transport::SCTP ? "sctp" : "tcp";
}

/** State name; a closed client connection with a reconnection armed is "reconnecting". */
inline std::string stateName(diametercomm::Peer::State s, bool reconnecting = false) {
    switch (s) {
        case diametercomm::Peer::State::Open:
            return "open";
        case diametercomm::Peer::State::WaitCEA:
            return "waitCEA";
        case diametercomm::Peer::State::WaitCER:
            return "waitCER";
        case diametercomm::Peer::State::Closing:
            return "closing";
        case diametercomm::Peer::State::Closed:
        default:
            return reconnecting ? "reconnecting" : "closed";
    }
}

/**
 * Gauge value of a connection state (h2diagent_diameter_client_connection_state):
 * 0 closed, 1 open, 2 reconnecting, 3 capabilities exchange (waitCEA/waitCER),
 * 4 closing (DPR sent).
 */
inline int stateValue(const std::string& state) {
    if (state == "open") return 1;
    if (state == "reconnecting") return 2;
    if (state == "waitCEA" || state == "waitCER") return 3;
    if (state == "closing") return 4;
    return 0;
}

/** An endpoint text from diametercomm, empty when not connected. */
inline std::string endpointOrEmpty(const std::string& endpoint) {
    return (endpoint.empty() || endpoint[0] == '<') ? "" : endpoint;
}

/** Client leg connection (the peer address falls back to the configured one). */
inline ConnectionInfo clientInfo(const diametercomm::DiameterClient& client, int index) {
    ConnectionInfo info;
    info.leg = "client";
    info.index = index;
    info.transport = transportName(client.transport());
    info.state = stateName(client.state(), client.reconnecting());
    if (auto peer = client.peer()) {
        info.localAddress = endpointOrEmpty(peer->localEndpoint());
        info.peerAddress = endpointOrEmpty(peer->remoteEndpoint());
        if (info.state == "open" || info.state == "closing") {
            info.peerOriginHost = peer->remoteOriginHost();
            info.peerOriginRealm = peer->remoteOriginRealm();
        }
    }
    if (info.peerAddress.empty() && !client.remoteHost().empty())
        info.peerAddress = client.remoteHost() + ":" + std::to_string(client.remotePort());
    return info;
}

/** Server leg connection (a peer connected to the gateway listener). */
inline ConnectionInfo serverInfo(const diametercomm::Peer& peer, int index) {
    ConnectionInfo info;
    info.leg = "server";
    info.index = index;
    info.transport = transportName(peer.transport());
    info.state = stateName(peer.state());
    info.localAddress = endpointOrEmpty(peer.localEndpoint());
    info.peerAddress = endpointOrEmpty(peer.remoteEndpoint());
    info.peerOriginHost = peer.remoteOriginHost();
    info.peerOriginRealm = peer.remoteOriginRealm();
    return info;
}

/** One connection as json (empty texts are omitted). */
inline nlohmann::json toJson(const ConnectionInfo& c) {
    nlohmann::json j = {{"leg", c.leg}, {"index", c.index}, {"transport", c.transport}, {"state", c.state}};
    if (!c.localAddress.empty()) j["localAddress"] = c.localAddress;
    if (!c.peerAddress.empty()) j["peerAddress"] = c.peerAddress;
    if (!c.peerOriginHost.empty()) j["peerOriginHost"] = c.peerOriginHost;
    if (!c.peerOriginRealm.empty()) j["peerOriginRealm"] = c.peerOriginRealm;
    return j;
}

inline nlohmann::json peersJson(const std::vector<ConnectionInfo>& connections) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& c : connections) list.push_back(toJson(c));
    return {{"connections", list}};
}

/**
 * Running configuration (no secrets: key passwords are never shown).
 *
 * @param dictionaries loaded dictionaries: file path and application id
 */
inline nlohmann::json configurationJson(const GatewayConfig& c,
                                        const std::vector<std::pair<std::string, uint32_t>>& dictionaries) {
    nlohmann::json dicts = nlohmann::json::array();
    for (const auto& [path, appId] : dictionaries) dicts.push_back({{"file", path}, {"applicationId", appId}});
    nlohmann::json decodeAs = nlohmann::json::object();
    for (const auto& [avp, form] : c.decodeAs) decodeAs[avp] = form;

    nlohmann::json diameter = {{"serverPort", c.diameterPort},
                               {"serverTransport", transportName(c.diameterServerTransport)},
                               {"serverTls", !c.diameterServerKeyFile.empty() && !c.diameterServerCrtFile.empty()},
                               {"originHost", c.originHost},
                               {"originRealm", c.originRealm},
                               {"productName", c.productName},
                               {"watchdogIntervalSec", c.watchdogIntervalSec},
                               {"timeoutMs", c.diameterTimeoutMs},
                               {"dictionaries", dicts},
                               {"decodeAs", decodeAs}};
    // extra capabilities AVPs (--cer / --cea), as given
    if (!c.cerAvps.is_null()) diameter["cer"] = {{"file", c.cerFile}, {"avps", c.cerAvps}};
    if (!c.ceaAvps.is_null()) diameter["cea"] = {{"file", c.ceaFile}, {"avps", c.ceaAvps}};
    if (!c.diameterPeerHost.empty()) {
        diameter["peer"] = {{"host", c.diameterPeerHost},
                            {"port", c.diameterPeerPort},
                            {"transport", transportName(c.diameterClientTransport)},
                            {"connections", c.diameterConnections},
                            {"localHost", c.diameterClientLocalHost.empty() ? "auto" : c.diameterClientLocalHost},
                            {"tls", c.secureDiameterClient}};
    }

    return {{"diameter", diameter},
            {"h2agent", {{"host", c.h2agentHost}, {"port", c.h2agentPort}}},
            {"http2ServerPort", c.http2ServerPort},
            {"adminPort", c.adminPort},
            {"metrics",
             {{"enabled", c.metricsEnabled},
              {"prometheusPort", c.prometheusPort},
              {"additionalLabels", c.metricsAdditionalLabelAvps}}},
            {"workers", c.workers}};
}

/** Admin answer. */
struct Response {
    int status{200};
    std::string body;
    std::string contentType{"application/json"};
};

/** Value of a query parameter ("a=1&level=Debug"), empty when absent. */
inline std::string queryValue(const std::string& query, const std::string& name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(pos, end - pos);
        const std::size_t eq = pair.find('=');
        if (pair.substr(0, eq) == name) return eq == std::string::npos ? "" : pair.substr(eq + 1);
        pos = end + 1;
    }
    return "";
}

inline Response jsonResponse(int status, const nlohmann::json& body) {
    return {status, body.dump(), "application/json"};
}

inline Response result(int status, bool ok, const std::string& response) {
    return jsonResponse(status, {{"result", ok ? "true" : "false"}, {"response", response}});
}

/**
 * Route an admin request.
 *
 * @param peers snapshot of the Diameter connections (called for GET peers only)
 * @param configuration running configuration (called for GET configuration only)
 */
inline Response handle(const std::string& method, const std::string& path, const std::string& query,
                       const std::function<std::vector<ConnectionInfo>()>& peers,
                       const std::function<nlohmann::json()>& configuration) {
    static const std::string prefix = "/admin/v1/";
    std::string resource = path.compare(0, prefix.size(), prefix) == 0 ? path.substr(prefix.size()) : "";
    while (!resource.empty() && resource.back() == '/') resource.pop_back();

    if (resource == "health" || resource == "peers" || resource == "configuration") {
        if (method != "GET") return result(405, false, "method not allowed (GET)");
        if (resource == "health") return jsonResponse(200, {{"status", "healthy"}});
        if (resource == "peers") return jsonResponse(200, peersJson(peers ? peers() : std::vector<ConnectionInfo>{}));
        return jsonResponse(200, configuration ? configuration() : nlohmann::json::object());
    }
    if (resource == "logging") {
        if (method == "GET")
            return {200, ert::tracing::Logger::levelAsString(ert::tracing::Logger::getLevel()), "text/plain"};
        if (method != "PUT") return result(405, false, "method not allowed (GET, PUT)");
        const std::string level = queryValue(query, "level");
        const std::string previous = ert::tracing::Logger::levelAsString(ert::tracing::Logger::getLevel());
        if (level.empty() || !ert::tracing::Logger::setLevel(level))
            return result(400, false, "invalid level '" + level + "' (Debug|Informational|Notice|Warning|Error|...)");
        if (level != previous) {
            ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Log level changed: %s -> %s", previous.c_str(), level.c_str()),
                ERT_FILE_LOCATION);
        }
        return result(200, true, "log level: " + level);
    }
    return result(404, false, "unknown resource '" + path + "'");
}

}  // namespace admin
}  // namespace h2diagent
}  // namespace ert
