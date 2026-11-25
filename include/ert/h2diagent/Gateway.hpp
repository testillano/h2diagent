/*
 _     ____     _ _
| |__ |___ \ __| (_) __ _  __ _  ___ _ __ | |_
| '_ \  __) / _` | |/ _` |/ _` |/ _ \ '_ \| __|
| | | |/ __/ (_| | | (_| | (_| |  __/ | | | |_
|_| |_|_____\__,_|_|\__,_|\__, |\___|_| |_|\__|
                           |___/

C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2024 Eduardo Ramos
*/

#pragma once

#include <nghttp2/asio_http2.h>
#include <nghttp2/asio_http2_client.h>
#include <nghttp2/asio_http2_server.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <ert/diametercodec/codec/Message.hpp>
#include <ert/diametercodec/stack/Dictionary.hpp>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercomm/DiameterServer.hpp>
#include <ert/h2diagent/DiameterClientPool.hpp>
#include <ert/metrics/Metrics.hpp>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ert {
namespace h2diagent {

namespace admin {
struct ConnectionInfo;
struct PeerOperation;
struct Response;
}  // namespace admin

/**
 * Configuration for the Gateway.
 */
struct GatewayConfig {
    // Diameter
    uint16_t diameterPort{3868};
    std::string diameterPeerHost;
    uint16_t diameterPeerPort{3868};
    std::string originHost;
    std::string originRealm;
    std::string productName{"h2diagent"};
    uint32_t watchdogIntervalSec{30};
    std::vector<std::string> dictionaryPaths;  // multiple dictionaries supported
    uint32_t diameterTimeoutMs{5000};          // transaction timeout (outbound send + inbound forward)
    // Diameter transport per role (single-homing when SCTP). Default: TCP.
    diametercomm::Transport diameterServerTransport{diametercomm::Transport::TCP};
    diametercomm::Transport diameterClientTransport{diametercomm::Transport::TCP};

    // Number of outbound Diameter client connections to the peer (pool size).
    // Default 1 = today's single-connection behaviour. >1 opens a round-robin
    // pool (TCP only; SCTP + N>1 is rejected at startup). See DiameterClientPool.
    int diameterConnections{1};

    // Local source address to bind the outbound Diameter client to. For SCTP this
    // single-homes the association so the INIT advertises ONLY this address,
    // avoiding non-routable local interfaces (docker0, bridges) that would stall
    // the handshake in COOKIE_WAIT.
    //   - empty (default): auto-derive the egress source toward the peer (the
    //     `src` of `ip route get <peer>`), i.e. the address traffic leaves from.
    //   - set: explicit override to PIN the egress address. Use it to force a
    //     specific interface when the default route reaches the peer by a path it
    //     does not answer (asymmetric routing, or a VIP reachable only via one
    //     interface) -- handy to trial-and-error a working source until the right
    //     route is found. The address must be local and able to egress to the peer.
    // Harmless for TCP (pins the egress source; no multihoming involved).
    std::string diameterClientLocalHost;

    // SCTP multihoming: listener addresses (--diameter-server-local-host; a comma
    // separated list binds several, IPv4 or IPv6), and the association options
    // (heartbeat interval, path max retransmissions, client primary peer address).
    // diameterPeerHost and diameterClientLocalHost may also be lists over SCTP.
    std::string diameterServerLocalHost{"0.0.0.0"};
    diametercomm::SctpOptions sctp;

    // Diameter TLS/TCP security (Phase 1). NOTE: SCTP is NOT secured (DTLS/SCTP
    // out of scope -- see README "gap"): TLS options are ignored on SCTP.
    // Server (inbound): TLS is enabled when BOTH key and crt are provided
    // (mirrors h2agent's --traffic-server-key/--traffic-server-crt). Server-auth
    // only (the gateway does not request a client certificate).
    std::string diameterServerKeyFile;
    std::string diameterServerCrtFile;
    std::string diameterServerKeyPassword;
    // Client (outbound): TLS is enabled by the secure flag (mirrors h2agent's
    // client "secure"). Optional CA verifies the remote server certificate;
    // optional client crt/key enable mutual TLS (mTLS).
    bool secureDiameterClient{false};
    std::string diameterClientCaFile;
    std::string diameterClientCrtFile;
    std::string diameterClientKeyFile;
    std::string diameterClientKeyPassword;

    // HTTP/2 client (towards h2agent)
    std::string h2agentHost{"localhost"};
    uint16_t h2agentPort{8000};

    // HTTP/2 server (for outbound triggers from h2agent): h2agent ports + 5
    // (h2agent 8000 / 8074 / 8080, h2diagent 8005 / 8079 / 8085) so both fit in a pod.
    uint16_t http2ServerPort{8005};

    // General
    uint16_t adminPort{8079};  // admin interface (/admin/v1); 0 disables it
    uint16_t prometheusPort{8085};
    bool metricsEnabled{true};
    // Optional: attach extra Prometheus label(s) to the Diameter CLIENT metrics,
    // each valued (per outbound request) from an AVP by name. Repeatable. Keep
    // them LOW-cardinality (e.g. CC-Request-Type). Empty = none. (The
    // application_id label is always present, taken from the message header.)
    std::vector<std::string> metricsAdditionalLabelAvps;
    // Optional: readable form of OctetString AVPs decoded towards h2agent
    // (--decode-as <AVP>=<form>, see DecodeAs.hpp). Empty = hex, as always.
    std::vector<std::pair<std::string, std::string>> decodeAs;
    // Optional: extra AVPs of the CER sent (client leg) / the CEA answered (server
    // leg), from --cer / --cea json files (AVP name -> value, gateway mapping;
    // see CapabilitiesAvps.hpp). null = none.
    // CEA Result-Code answered on the server leg (--cea-result-code): 2001
    // opens, another code rejects the capabilities exchange, -1 answers nothing.
    int32_t ceaResultCode{2001};
    std::string cerFile;
    std::string ceaFile;
    nlohmann::json cerAvps;
    nlohmann::json ceaAvps;
    int workers{0};  // 0 = nproc
};

/**
 * The Gateway: translates between Diameter and HTTP/2+JSON.
 *
 * Inbound flow:  Client -> Diameter -> h2diagent -> HTTP/2 POST -> h2agent
 * Outbound flow: h2agent client -> HTTP/2 POST -> h2diagent -> Diameter -> Server
 */
class Gateway {
   public:
    explicit Gateway(boost::asio::io_context& io, const GatewayConfig& config);
    ~Gateway();

    // Non-copyable
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    /**
     * Start all services (Diameter server/client, HTTP/2 client/server).
     * Throws std::runtime_error on an invalid --decode-as (resolved against
     * the loaded dictionaries, before any service starts).
     */
    void start();

    /**
     * Graceful shutdown.
     */
    void stop();

   private:
    // Inbound: Diameter request from a peer -> translate -> forward to h2agent,
    // then send the answer back on the SAME leg it arrived on. Diameter is
    // bidirectional (RFC 6733): a request can arrive either on the Diameter
    // SERVER leg (a peer connected to us) or, server-initiated, on the Diameter
    // CLIENT leg (a connection we opened). fromClient selects the answer sink;
    // originatingClient identifies WHICH pooled client received it (so the RAA
    // goes back on the exact same connection). nullptr => server leg.
    void onDiameterRequest(std::shared_ptr<diametercomm::Peer> peer, diametercomm::Peer::Buffer&& msg,
                           bool fromClient = false, diametercomm::DiameterClient* originatingClient = nullptr);

    // Send an already-encoded Diameter answer back on the leg the request came
    // from: the client leg (server-initiated request answered by us, on the
    // originatingClient connection) or the server leg (classic inbound). Routes
    // to the matching diametercomm object so the correct answers_sent metric
    // (client vs server) is incremented.
    void sendDiameterAnswer(const std::shared_ptr<diametercomm::Peer>& peer, diametercomm::Peer::Buffer&& answer,
                            bool fromClient, diametercomm::DiameterClient* originatingClient);

    // Send a minimal DIAMETER_UNABLE_TO_COMPLY (5012) answer for an inbound
    // request (h2agent timeout, non-200, or an answer that cannot be built),
    // echoing the request Session-Id when known (empty = omitted).
    void sendUnableToComply(const std::shared_ptr<diametercomm::Peer>& peer, bool fromClient,
                            diametercomm::DiameterClient* originatingClient, uint32_t commandCode, uint32_t appId,
                            uint32_t hbh, uint32_t e2e, const std::string& sessionId);

    using OutboundRespond = std::function<void(int, const std::string&, const nghttp2::asio_http2::header_map&)>;

    // Outbound: HTTP/2 request from h2agent -> translate -> send Diameter to Server
    void onH2agentOutboundRequest(const std::string& method, const std::string& uri, const std::string& body,
                                  const nghttp2::asio_http2::header_map& headers, OutboundRespond respond);

    // Send an encoded outbound request and ALWAYS answer the HTTP/2 client: 200
    // (answer), 502 (no peer / send failed / undecodable answer) or 504 (answer
    // timeout). It goes on a pooled client; a gateway without a client leg sends
    // it on the server leg instead (see sendOutboundServerLeg).
    void sendOutbound(diametercomm::Peer::Buffer&& encoded, uint32_t appId,
                      const ert::metrics::labels_t& additionalLabels, const std::string& destinationHost,
                      OutboundRespond respond);

    // Server-initiated request (e.g. RAR, ASR) of a gateway without a client leg:
    // sent on a server-leg connection of the peer named by the Destination-Host
    // (any open connection without it), round-robin (helpers::pickServerConnection).
    void sendOutboundServerLeg(diametercomm::Peer::Buffer&& encoded, uint32_t appId,
                               const ert::metrics::labels_t& additionalLabels, const std::string& destinationHost,
                               OutboundRespond respond);

    // Decode the Diameter answer of an outbound request and answer the HTTP/2
    // client: 200 with its json, or 502 with the raw bytes when undecodable.
    void respondOutboundAnswer(uint32_t appId, const diametercomm::Peer::Buffer& answerMsg,
                               const OutboundRespond& respond);

    // True when outbound requests go on the server leg: no client leg configured
    // (no --diameter-peer-host) but a Diameter server listening.
    bool outboundOnServerLeg() const { return !diameterClientPool_ && diameterServer_; }

    // (Re)connect the HTTP/2 client to h2agent. Creates a fresh nghttp2 session.
    // Must run on the h2clientIo_ thread (the only thread that mutates the session).
    void connectH2Client();
    // Schedule a backoff reconnection of the HTTP/2 client to h2agent.
    // Must run on the h2clientIo_ thread.
    void scheduleH2ClientReconnect();

    // Admin interface (/admin/v1, see Admin.hpp) on --admin-port.
    void startAdminServer();
    // Diameter connections snapshot (client pool, then server leg).
    std::vector<admin::ConnectionInfo> connections() const;
    // Base protocol fault injection (PUT /admin/v1/peers/<leg>/<index>, PUT configuration).
    admin::Response peerOperation(const admin::PeerOperation& op);
    admin::Response setCeaResultCode(int32_t code);
    // Refresh the client connection state gauges (every second, on io_).
    void refreshConnectionGauges();
    void scheduleConnectionGauges();
    // SCTP path states last seen (gauges, Warning log on every change): "<leg>/<index>/<address>" -> state
    std::map<std::string, std::string> pathStates_;

    // Helper: map application-id to interface name
    std::string getInterfaceName(uint32_t appId) const;  // Helper: map command-code to command abbreviation
    std::string getCommandName(uint32_t commandCode, bool isRequest) const;
    // Helper: select dictionary by application-id
    const diametercodec::stack::Dictionary& getDictionary(uint32_t appId) const;

    boost::asio::io_context& io_;
    GatewayConfig config_;

    // Metrics
    ert::metrics::Metrics* metrics_{};

    // Diameter metrics (Gateway-level, complements diametercomm's enableMetrics)
    ert::metrics::counter_family_t* diameter_answers_sent_counter_family_ptr_{};

    // HTTP/2 metrics (instrumented directly in Gateway callbacks)
    ert::metrics::counter_family_t* h2_server_requests_received_counter_family_ptr_{};
    ert::metrics::counter_family_t* h2_server_responses_sent_counter_family_ptr_{};
    ert::metrics::counter_family_t* h2_client_requests_sent_counter_family_ptr_{};
    ert::metrics::counter_family_t* h2_client_responses_received_counter_family_ptr_{};

    // Components
    std::unique_ptr<diametercomm::DiameterServer> diameterServer_;
    std::unique_ptr<DiameterClientPool> diameterClientPool_;  // N outbound connections (N>=1)
    std::atomic<std::size_t> serverLegCursor_{0};             // round-robin of server-leg outbound requests

    // Multi-stack dictionaries indexed by Application-Id
    std::map<uint32_t, diametercodec::stack::Dictionary> dictionaries_;
    std::vector<std::pair<std::string, uint32_t>> loadedDictionaries_;  // file and application id (admin)
    // --decode-as resolved against the dictionaries at start (empty: plain hex)
    diametercodec::codec::DecodeOptions decodeOptions_;
    diametercodec::stack::Dictionary defaultDictionary_;  // fallback for unknown appId

    // HTTP/2 client session (towards h2agent)
    // Uses its own single-threaded io_context to avoid nghttp2 re-entrancy
    boost::asio::io_context h2clientIo_{1};
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> h2clientWork_;
    std::thread h2clientThread_;
    std::unique_ptr<nghttp2::asio_http2::client::session> h2clientSession_;
    std::atomic<bool> h2clientConnected_{false};

    // HTTP/2 client reconnection to h2agent. The nghttp2 client session does not
    // reconnect on its own, so on connection loss/failure we recreate it with an
    // exponential backoff. Timer and flags below are only touched on the
    // h2clientIo_ thread (except h2clientStopping_, which is atomic).
    std::unique_ptr<boost::asio::steady_timer> h2clientReconnectTimer_;
    std::chrono::milliseconds h2clientReconnectBackoff_{1000};
    static constexpr std::chrono::milliseconds h2clientReconnectInitial_{1000};
    static constexpr std::chrono::milliseconds h2clientReconnectMax_{10000};
    bool h2clientReconnectPending_{false};
    std::atomic<bool> h2clientStopping_{false};

    // HTTP/2 server (for outbound triggers)
    std::unique_ptr<nghttp2::asio_http2::server::http2> h2server_;

    // Admin HTTP/2 server (/admin/v1)
    std::unique_ptr<nghttp2::asio_http2::server::http2> adminServer_;

    // Client connection state gauge (one series per pooled connection) and its refresh timer
    ert::metrics::gauge_family_t* client_connection_state_gauge_family_ptr_{};
    ert::metrics::gauge_family_t* sctp_path_state_gauge_family_ptr_{};
    std::unique_ptr<boost::asio::steady_timer> gaugesTimer_;
    std::atomic<bool> gaugesStopping_{false};
};

}  // namespace h2diagent
}  // namespace ert
