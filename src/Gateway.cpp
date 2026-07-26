/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <ert/h2diagent/Gateway.hpp>

#include <ert/tracing/Logger.hpp>

#include <fstream>
#include <nlohmann/json.hpp>

namespace ert
{
namespace h2diagent
{

Gateway::Gateway(boost::asio::io_context& io, const GatewayConfig& config)
    : io_(io)
    , config_(config)
{
}

Gateway::~Gateway() {
    stop();
}

void Gateway::start() {
    LOGWARNING(ert::tracing::Logger::warning(
        "Starting h2diagent gateway", ERT_FILE_LOCATION));

    // Load dictionary
    if (!config_.dictionaryPath.empty()) {
        std::ifstream ifs(config_.dictionaryPath);
        if (ifs.is_open()) {
            nlohmann::json dictJson = nlohmann::json::parse(ifs);
            dictionary_.load(dictJson);
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Dictionary loaded: %s",
                    config_.dictionaryPath.c_str()), ERT_FILE_LOCATION));
        } else {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Failed to open dictionary: %s",
                    config_.dictionaryPath.c_str()), ERT_FILE_LOCATION));
        }
    }

    // Configure Diameter peer
    diametercomm::Peer::Config peerConfig;
    peerConfig.originHost = config_.originHost;
    peerConfig.originRealm = config_.originRealm;
    peerConfig.productName = config_.productName;
    peerConfig.watchdogIntervalSec = config_.watchdogIntervalSec;

    // Start Diameter server (inbound from client)
    diameterServer_ = std::make_unique<diametercomm::DiameterServer>(io_, peerConfig);
    diameterServer_->setRequestCallback(
        [this](std::shared_ptr<diametercomm::Peer> peer, diametercomm::Peer::Buffer&& msg) {
            onDiameterRequest(std::move(peer), std::move(msg));
        });
    diameterServer_->listen("0.0.0.0", config_.diameterPort);

    LOGWARNING(ert::tracing::Logger::warning(
        ert::tracing::Logger::asString("Diameter server listening on port %d",
            config_.diameterPort), ERT_FILE_LOCATION));

    // Start Diameter client (outbound to server) if peer host configured
    if (!config_.diameterPeerHost.empty()) {
        diameterClient_ = std::make_unique<diametercomm::DiameterClient>(io_, peerConfig);
        diameterClient_->connect(config_.diameterPeerHost, config_.diameterPeerPort);

        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("Diameter client connecting to %s:%d",
                config_.diameterPeerHost.c_str(), config_.diameterPeerPort),
            ERT_FILE_LOCATION));
    }

    // TODO: Start HTTP/2 client towards h2agent
    // TODO: Start HTTP/2 server for outbound triggers

    LOGWARNING(ert::tracing::Logger::warning(
        "h2diagent gateway started", ERT_FILE_LOCATION));
}

void Gateway::stop() {
    LOGWARNING(ert::tracing::Logger::warning(
        "Stopping h2diagent gateway", ERT_FILE_LOCATION));

    if (diameterServer_) {
        diameterServer_->shutdown(0);
        diameterServer_.reset();
    }

    if (diameterClient_) {
        diameterClient_->disconnect(0);
        diameterClient_.reset();
    }
}

void Gateway::onDiameterRequest(std::shared_ptr<diametercomm::Peer> peer,
                                 diametercomm::Peer::Buffer&& msg) {
    // TODO: Phase 1 implementation:
    // 1. Decode Diameter message using diametercodec
    // 2. Convert to JSON (Message::toJson)
    // 3. Build HTTP/2 POST request (URI from command code + app-id)
    // 4. Send to h2agent
    // 5. On response: convert JSON -> Diameter answer (Message::fromJson)
    // 6. Send answer back via peer->send()

    LOGINFORMATIONAL(ert::tracing::Logger::informational(
        ert::tracing::Logger::asString("Received Diameter request (%zu bytes) from peer",
            msg.size()), ERT_FILE_LOCATION));

    (void)peer;
}

void Gateway::onH2agentOutboundRequest(/* TODO */) {
    // TODO: Phase 2 implementation:
    // 1. Parse HTTP/2 request body as JSON
    // 2. Convert JSON -> Diameter request (Message::fromJson)
    // 3. Send via diameterClient_
    // 4. On Diameter answer: convert to JSON, return HTTP/2 response

    LOGINFORMATIONAL(ert::tracing::Logger::informational(
        "Received outbound trigger from h2agent", ERT_FILE_LOCATION));
}

} // namespace h2diagent
} // namespace ert
