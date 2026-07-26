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

#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include <ert/diametercomm/DiameterServer.hpp>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercodec/stack/Dictionary.hpp>

namespace ert
{
namespace h2diagent
{

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
    std::string dictionaryPath;

    // HTTP/2 client (towards h2agent)
    std::string h2agentHost{"localhost"};
    uint16_t h2agentPort{8000};

    // HTTP/2 server (for outbound triggers from h2agent)
    uint16_t http2ServerPort{8080};

    // General
    uint16_t adminPort{8074};
    uint16_t prometheusPort{9090};
    bool metricsEnabled{true};
    int workers{0}; // 0 = nproc
};

/**
 * The Gateway: translates between Diameter and HTTP/2+JSON.
 *
 * Inbound flow:  SUT -> Diameter -> h2diagent -> HTTP/2 POST -> h2agent
 * Outbound flow: h2agent client -> HTTP/2 POST -> h2diagent -> Diameter -> SUT
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

    // Inbound: Diameter request from client -> translate -> forward to h2agent
    void onDiameterRequest(std::shared_ptr<diametercomm::Peer> peer,
                           diametercomm::Peer::Buffer&& msg);

    // Outbound: HTTP/2 request from h2agent -> translate -> send Diameter to server
    void onH2agentOutboundRequest(/* TODO: http2 request params */);

    boost::asio::io_context& io_;
    GatewayConfig config_;

    // Components
    std::unique_ptr<diametercomm::DiameterServer> diameterServer_;
    std::unique_ptr<diametercomm::DiameterClient> diameterClient_;
    diametercodec::stack::Dictionary dictionary_;

    // TODO: HTTP/2 client (nghttp2 session towards h2agent)
    // TODO: HTTP/2 server (nghttp2 server for outbound triggers)
};

} // namespace h2diagent
} // namespace ert
