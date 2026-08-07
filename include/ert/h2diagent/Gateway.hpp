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
#include <ert/metrics/Metrics.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ert {
namespace h2diagent {

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

    // HTTP/2 client (towards h2agent)
    std::string h2agentHost{"localhost"};
    uint16_t h2agentPort{8000};

    // HTTP/2 server (for outbound triggers from h2agent)
    uint16_t http2ServerPort{8080};

    // General
    uint16_t adminPort{8074};
    uint16_t prometheusPort{8085};
    bool metricsEnabled{true};
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
     */
    void start();

    /**
     * Graceful shutdown.
     */
    void stop();

   private:
    // Inbound: Diameter request from Client -> translate -> forward to h2agent
    void onDiameterRequest(std::shared_ptr<diametercomm::Peer> peer, diametercomm::Peer::Buffer&& msg);

    // Outbound: HTTP/2 request from h2agent -> translate -> send Diameter to Server
    void onH2agentOutboundRequest(const std::string& method, const std::string& uri, const std::string& body,
                                  const nghttp2::asio_http2::header_map& headers,
                                  std::function<void(int, const std::string&)> respond);

    // (Re)connect the HTTP/2 client to h2agent. Creates a fresh nghttp2 session.
    // Must run on the h2clientIo_ thread (the only thread that mutates the session).
    void connectH2Client();
    // Schedule a backoff reconnection of the HTTP/2 client to h2agent.
    // Must run on the h2clientIo_ thread.
    void scheduleH2ClientReconnect();

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
    std::unique_ptr<diametercomm::DiameterClient> diameterClient_;

    // Multi-stack dictionaries indexed by Application-Id
    std::map<uint32_t, diametercodec::stack::Dictionary> dictionaries_;
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
};

}  // namespace h2diagent
}  // namespace ert
