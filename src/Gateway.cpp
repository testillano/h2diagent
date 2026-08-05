/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

#include <nghttp2/asio_http2_client.h>
#include <nghttp2/asio_http2_server.h>

#include <ert/diametercodec/codec/Avp.hpp>
#include <ert/diametercodec/codec/Message.hpp>
#include <ert/h2diagent/Gateway.hpp>
#include <ert/tracing/Logger.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace ert {
namespace h2diagent {

namespace {

// Application-ID to interface name mapping
std::string appIdToInterface(uint32_t appId) {
    switch (appId) {
        case 16777238:
            return "gx";
        case 16777236:
            return "rx";
        case 16777302:
            return "sy";
        case 4:
            return "cc";  // Credit-Control (base)
        default:
            return "app-" + std::to_string(appId);
    }
}

// Command-code to abbreviation (request form)
std::string commandCodeToName(uint32_t code) {
    switch (code) {
        case 257:
            return "CER";
        case 258:
            return "RAR";
        case 265:
            return "AAR";
        case 271:
            return "ACR";
        case 272:
            return "CCR";
        case 274:
            return "ASR";
        case 275:
            return "STR";
        case 280:
            return "DWR";
        case 282:
            return "DPR";
        case 300:
            return "SNR";  // Sy Spending-Limit
        default:
            return "CMD-" + std::to_string(code);
    }
}

// Extract command code from Diameter binary header
uint32_t extractCommandCode(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[5]) << 16) | (uint32_t(msg[6]) << 8) | uint32_t(msg[7]);
}

// Extract application-id from Diameter binary header
uint32_t extractApplicationId(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[8]) << 24) | (uint32_t(msg[9]) << 16) | (uint32_t(msg[10]) << 8) | uint32_t(msg[11]);
}

// Extract hop-by-hop from Diameter binary header
uint32_t extractHopByHop(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[12]) << 24) | (uint32_t(msg[13]) << 16) | (uint32_t(msg[14]) << 8) | uint32_t(msg[15]);
}

// Extract end-to-end from Diameter binary header
uint32_t extractEndToEnd(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return 0;
    return (uint32_t(msg[16]) << 24) | (uint32_t(msg[17]) << 16) | (uint32_t(msg[18]) << 8) | uint32_t(msg[19]);
}

// Check if request (R-bit set)
bool isRequest(const std::vector<uint8_t>& msg) {
    if (msg.size() < 20) return false;
    return (msg[4] & 0x80) != 0;
}

}  // anonymous namespace

// ============================================================================
// getDictionary - select dictionary by Application-Id
// ============================================================================
const diametercodec::stack::Dictionary& Gateway::getDictionary(uint32_t appId) const {
    auto it = dictionaries_.find(appId);
    if (it != dictionaries_.end()) return it->second;
    // Fallback: try appId 0 (base protocol / single-dict mode)
    auto base = dictionaries_.find(0);
    if (base != dictionaries_.end()) return base->second;
    // Last resort: return first available or default empty
    if (!dictionaries_.empty()) return dictionaries_.begin()->second;
    return defaultDictionary_;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

Gateway::Gateway(boost::asio::io_context& io, const GatewayConfig& config) : io_(io), config_(config) {}

Gateway::~Gateway() { stop(); }

// ============================================================================
// start
// ============================================================================
void Gateway::start() {
    LOGWARNING(ert::tracing::Logger::warning("Starting h2diagent gateway", ERT_FILE_LOCATION));

    // Load dictionaries (multi-stack: each file has its own application-id)
    for (const auto& dictPath : config_.dictionaryPaths) {
        std::ifstream ifs(dictPath);
        if (ifs.is_open()) {
            nlohmann::json dictJson = nlohmann::json::parse(ifs);

            // Extract application-id from the dictionary JSON
            uint32_t appId = 0;
            auto appIdIt = dictJson.find("application-id");
            if (appIdIt != dictJson.end()) {
                appId = appIdIt->get<uint32_t>();
            }

            // Load directly into map (Dictionary has internal Avp pointers
            // to itself, so it cannot be moved after load)
            dictionaries_[appId].load(dictJson);

            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Dictionary loaded: %s (application-id: %u)", dictPath.c_str(), appId),
                ERT_FILE_LOCATION));
        } else {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Failed to open dictionary: %s", dictPath.c_str()), ERT_FILE_LOCATION));
        }
    }

    if (dictionaries_.empty()) {
        LOGWARNING(
            ert::tracing::Logger::warning("No dictionaries loaded - codec operations will fail", ERT_FILE_LOCATION));
    }

    // --- Prometheus metrics ---
    if (config_.metricsEnabled) {
        metrics_ = new ert::metrics::Metrics;
        std::string bindAddressPort = "0.0.0.0:" + std::to_string(config_.prometheusPort);
        if (!metrics_->serve(bindAddressPort)) {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Failed to start prometheus on %s", bindAddressPort.c_str()),
                ERT_FILE_LOCATION));
            delete metrics_;
            metrics_ = nullptr;
        } else {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Prometheus metrics on %s", bindAddressPort.c_str()),
                ERT_FILE_LOCATION));
            // HTTP/2 metrics (Gateway-level instrumentation)
            ert::metrics::labels_t familyLabels = {};
            diameter_answers_sent_counter_family_ptr_ =
                &(metrics_->addCounterFamily("h2diagent_diameter_server_answers_sent_counter",
                                             "Diameter answers sent back to peer (Gateway-level)", familyLabels));
            h2_server_requests_received_counter_family_ptr_ = &(metrics_->addCounterFamily(
                "h2diagent_http2_server_requests_received_counter", "HTTP/2 server requests received", familyLabels));
            h2_server_responses_sent_counter_family_ptr_ = &(metrics_->addCounterFamily(
                "h2diagent_http2_server_responses_sent_counter", "HTTP/2 server responses sent", familyLabels));
            h2_client_requests_sent_counter_family_ptr_ =
                &(metrics_->addCounterFamily("h2diagent_http2_client_requests_sent_counter",
                                             "HTTP/2 client requests sent to h2agent", familyLabels));
            h2_client_responses_received_counter_family_ptr_ =
                &(metrics_->addCounterFamily("h2diagent_http2_client_responses_received_counter",
                                             "HTTP/2 client responses received from h2agent", familyLabels));
        }
    }

    // --- Diameter Server (inbound from Client) ---
    diametercomm::Peer::Config peerConfig;
    peerConfig.originHost = config_.originHost;
    peerConfig.originRealm = config_.originRealm;
    peerConfig.productName = config_.productName;
    peerConfig.watchdogIntervalSec = config_.watchdogIntervalSec;
    // Advertise all loaded application-ids in CER/CEA
    for (const auto& [appId, dict] : dictionaries_) {
        if (appId != 0) peerConfig.applicationIds.push_back(appId);
    }

    if (config_.diameterPort > 0) {
        diameterServer_ =
            std::make_unique<diametercomm::DiameterServer>(io_, peerConfig, config_.diameterServerTransport);
        diameterServer_->enableMetrics(metrics_, config_.productName);
        diameterServer_->setRequestCallback(
            [this](std::shared_ptr<diametercomm::Peer> peer, diametercomm::Peer::Buffer&& msg) {
                onDiameterRequest(std::move(peer), std::move(msg));
            });
        diameterServer_->listen("0.0.0.0", config_.diameterPort);

        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("Diameter server listening on port %d", config_.diameterPort),
            ERT_FILE_LOCATION));
    } else {
        LOGWARNING(ert::tracing::Logger::warning("Diameter server disabled (port 0)", ERT_FILE_LOCATION));
    }

    // --- Diameter Client (outbound to Server) ---
    if (!config_.diameterPeerHost.empty()) {
        diameterClient_ =
            std::make_unique<diametercomm::DiameterClient>(io_, peerConfig, config_.diameterClientTransport);
        diameterClient_->enableMetrics(metrics_, config_.productName);
        diameterClient_->setTimeoutCallback([this](uint32_t hbh) {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Diameter request timed out hbh=0x%08x", hbh), ERT_FILE_LOCATION));
        });
        diameterClient_->connect(config_.diameterPeerHost, config_.diameterPeerPort);

        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("Diameter client connecting to %s:%d", config_.diameterPeerHost.c_str(),
                                           config_.diameterPeerPort),
            ERT_FILE_LOCATION));
    }

    // --- HTTP/2 client towards h2agent (for inbound flow) ---
    // Only needed when acting as Diameter server (inbound: receives Diameter, forwards to h2agent)
    // Uses dedicated single-threaded io_context to avoid nghttp2 re-entrancy under load.
    if (config_.h2agentPort > 0 && config_.diameterPort > 0) {
        try {
            h2clientWork_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
                boost::asio::make_work_guard(h2clientIo_));
            h2clientSession_ = std::make_unique<nghttp2::asio_http2::client::session>(
                h2clientIo_, config_.h2agentHost, std::to_string(config_.h2agentPort));

            h2clientSession_->on_connect([this](auto) {
                LOGWARNING(ert::tracing::Logger::warning(
                    ert::tracing::Logger::asString("HTTP/2 client connected to %s:%d", config_.h2agentHost.c_str(),
                                                   config_.h2agentPort),
                    ERT_FILE_LOCATION));
                h2clientConnected_ = true;
            });

            h2clientSession_->on_error([this](const boost::system::error_code& ec) {
                LOGWARNING(ert::tracing::Logger::warning(
                    ert::tracing::Logger::asString("HTTP/2 client error to %s:%d: %s", config_.h2agentHost.c_str(),
                                                   config_.h2agentPort, ec.message().c_str()),
                    ERT_FILE_LOCATION));
                h2clientConnected_ = false;
            });
        } catch (const std::exception& e) {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Failed to create HTTP/2 client: %s", e.what()), ERT_FILE_LOCATION));
        }

        // Start the dedicated thread for the HTTP/2 client io_context
        h2clientThread_ = std::thread([this]() { h2clientIo_.run(); });
    }

    // --- HTTP/2 server for outbound triggers from h2agent ---
    if (config_.http2ServerPort > 0) {
        h2server_ = std::make_unique<nghttp2::asio_http2::server::http2>();
        h2server_->handle("/", [this](const nghttp2::asio_http2::server::request& req,
                                      const nghttp2::asio_http2::server::response& res) {
            // Collect request body
            auto body = std::make_shared<std::string>();
            auto method = std::make_shared<std::string>(req.method());
            auto uri = std::make_shared<std::string>(req.uri().path);
            auto headers = std::make_shared<nghttp2::asio_http2::header_map>(req.header());

            req.on_data([this, &res, body, method, uri, headers](const uint8_t* data, std::size_t len) {
                if (len > 0) {
                    body->append(reinterpret_cast<const char*>(data), len);
                } else {
                    // End of body - process the request
                    if (metrics_) {
                        auto& counter = h2_server_requests_received_counter_family_ptr_->Add(
                            {{"source", config_.productName}, {"method", *method}});
                        counter.Increment();
                    }
                    onH2agentOutboundRequest(
                        *method, *uri, *body, *headers,
                        [this, &res, method](int statusCode, const std::string& responseBody) {
                            if (metrics_) {
                                auto& counter = h2_server_responses_sent_counter_family_ptr_->Add(
                                    {{"source", config_.productName},
                                     {"method", *method},
                                     {"status_code", std::to_string(statusCode)}});
                                counter.Increment();
                            }
                            nghttp2::asio_http2::header_map h;
                            h.emplace("content-type", nghttp2::asio_http2::header_value{"application/json", false});
                            res.write_head(statusCode, h);
                            res.end(responseBody);
                        });
                }
            });
        });

        boost::system::error_code h2ec;
        h2server_->listen_and_serve(h2ec, "0.0.0.0", std::to_string(config_.http2ServerPort), true);
        if (h2ec) {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("HTTP/2 server failed to start on port %d: %s", config_.http2ServerPort,
                                               h2ec.message().c_str()),
                ERT_FILE_LOCATION));
        } else {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("HTTP/2 server listening on port %d", config_.http2ServerPort),
                ERT_FILE_LOCATION));
        }
    } else {
        LOGWARNING(ert::tracing::Logger::warning("HTTP/2 server disabled (port 0)", ERT_FILE_LOCATION));
    }

    LOGWARNING(ert::tracing::Logger::warning("h2diagent gateway started", ERT_FILE_LOCATION));
}

// ============================================================================
// stop
// ============================================================================
void Gateway::stop() {
    LOGWARNING(ert::tracing::Logger::warning("Stopping h2diagent gateway", ERT_FILE_LOCATION));

    if (diameterServer_) {
        diameterServer_->shutdown(0);
        diameterServer_.reset();
    }

    if (diameterClient_) {
        diameterClient_->disconnect(0);
        diameterClient_.reset();
    }

    if (h2server_) {
        h2server_->stop();
        h2server_->join();
        h2server_.reset();
    }

    if (h2clientSession_) {
        h2clientSession_->shutdown();
        h2clientSession_.reset();
    }
    if (h2clientWork_) {
        h2clientWork_.reset();  // release work guard
        h2clientIo_.stop();
    }
    if (h2clientThread_.joinable()) {
        h2clientThread_.join();
    }

    if (metrics_) {
        delete metrics_;
        metrics_ = nullptr;
    }
}

// ============================================================================
// onDiameterRequest - Inbound: Diameter -> JSON -> HTTP/2 -> h2agent
// ============================================================================
void Gateway::onDiameterRequest(std::shared_ptr<diametercomm::Peer> peer, diametercomm::Peer::Buffer&& msg) {
    if (msg.size() < 20) {
        LOGWARNING(
            ert::tracing::Logger::warning("Received malformed Diameter message (< 20 bytes)", ERT_FILE_LOCATION));
        return;
    }

    // Extract header fields for routing and correlation
    uint32_t commandCode = extractCommandCode(msg);
    uint32_t appId = extractApplicationId(msg);
    uint32_t hbh = extractHopByHop(msg);
    uint32_t e2e = extractEndToEnd(msg);
    bool reqFlag = isRequest(msg);

    std::string interface = appIdToInterface(appId);
    std::string command = commandCodeToName(commandCode);

    LOGINFORMATIONAL(ert::tracing::Logger::informational(
        ert::tracing::Logger::asString("Inbound Diameter %s %s/%s (%d bytes) hbh=0x%08x e2e=0x%08x",
                                       reqFlag ? "request" : "answer", interface.c_str(), command.c_str(),
                                       (int)msg.size(), hbh, e2e),
        ERT_FILE_LOCATION));

    // 1. Decode Diameter binary -> codec::Message -> JSON
    const auto& dict = getDictionary(appId);
    diametercodec::codec::Message diameterMsg;
    try {
        diameterMsg.decode(msg.data(), msg.size(), dict);
    } catch (const std::exception& e) {
        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("Failed to decode Diameter message: %s", e.what()), ERT_FILE_LOCATION));
        return;
    }

    nlohmann::json jsonBody = diameterMsg.toJson(dict);

    LOGDEBUG(ert::tracing::Logger::debug(
        ert::tracing::Logger::asString("Diameter -> JSON: %s", jsonBody.dump().c_str()), ERT_FILE_LOCATION));

    // 2. Build HTTP/2 POST to h2agent
    std::string path = "/diameter/" + interface + "/" + command;
    std::string fullUri = "http://" + config_.h2agentHost + ":" + std::to_string(config_.h2agentPort) + path;
    std::string bodyStr = jsonBody.dump();

    // 3. Send HTTP/2 request to h2agent and handle response
    if (!h2clientSession_ || !h2clientConnected_) {
        LOGWARNING(ert::tracing::Logger::warning("HTTP/2 client not connected to h2agent", ERT_FILE_LOCATION));
        return;
    }

    auto peerPtr = peer;  // capture for lambda

    nghttp2::asio_http2::header_map h;
    h.emplace("content-type", nghttp2::asio_http2::header_value{"application/json", false});
    h.emplace("x-diameter-command-code", nghttp2::asio_http2::header_value{std::to_string(commandCode), false});
    h.emplace("x-diameter-application-id", nghttp2::asio_http2::header_value{std::to_string(appId), false});
    h.emplace("x-diameter-hop-by-hop", nghttp2::asio_http2::header_value{std::to_string(hbh), false});
    h.emplace("x-diameter-end-to-end", nghttp2::asio_http2::header_value{std::to_string(e2e), false});
    h.emplace("x-diameter-flags", nghttp2::asio_http2::header_value{std::to_string(diameterMsg.getFlags()), false});

    // Post submit to the session's io_service to avoid nghttp2 re-entrancy assert
    auto h_ptr = std::make_shared<nghttp2::asio_http2::header_map>(std::move(h));
    auto bodyStr_ptr = std::make_shared<std::string>(std::move(bodyStr));
    auto fullUri_copy = fullUri;

    boost::asio::post(h2clientIo_, [this, peerPtr, hbh, e2e, commandCode, appId, h_ptr, bodyStr_ptr, fullUri_copy]() {
        boost::system::error_code ec;
        auto req = h2clientSession_->submit(ec, "POST", fullUri_copy, *bodyStr_ptr, std::move(*h_ptr));

        if (ec || !req) {
            LOGWARNING(ert::tracing::Logger::warning(
                ert::tracing::Logger::asString("Failed to submit HTTP/2 request: %s", ec.message().c_str()),
                ERT_FILE_LOCATION));
            return;
        }

        if (metrics_) {
            auto& counter =
                h2_client_requests_sent_counter_family_ptr_->Add({{"source", config_.productName}, {"method", "POST"}});
            counter.Increment();
        }

        // 4. On response: JSON -> Diameter answer -> send back to peer
        auto responseBody = std::make_shared<std::string>();
        auto timedOut = std::make_shared<std::atomic<bool>>(false);

        // Inbound timeout: if h2agent doesn't respond in time, send DIAMETER_UNABLE_TO_COMPLY
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        timer->expires_after(std::chrono::milliseconds(config_.diameterTimeoutMs));
        timer->async_wait(
            [this, timer, timedOut, peerPtr, hbh, e2e, commandCode, appId](const boost::system::error_code& ec) {
                if (ec) return;                        // cancelled (response arrived in time)
                if (timedOut->exchange(true)) return;  // already handled

                LOGWARNING(ert::tracing::Logger::warning(
                    ert::tracing::Logger::asString(
                        "Inbound timeout (%u ms) for command %u, sending DIAMETER_UNABLE_TO_COMPLY",
                        config_.diameterTimeoutMs, commandCode),
                    ERT_FILE_LOCATION));

                // Build minimal error answer
                const auto& dict = getDictionary(appId);
                nlohmann::json errorJson = {{"Result-Code", 5012},
                                            {"Origin-Host", config_.originHost},
                                            {"Origin-Realm", config_.originRealm},
                                            {"_header",
                                             {{"version", 1},
                                              {"flags", 0},
                                              {"command-code", commandCode},
                                              {"request", false},
                                              {"application-id", appId},
                                              {"hop-by-hop-id", hbh},
                                              {"end-to-end-id", e2e}}}};

                try {
                    diametercodec::codec::Message answerMsg = diametercodec::codec::Message::fromJson(errorJson, dict);
                    diametercodec::core::Buffer encoded = answerMsg.encode(dict);
                    diameterServer_->sendAnswer(peerPtr, std::move(encoded));
                } catch (...) {
                }
            });

        req->on_response([this, peerPtr, hbh, e2e, commandCode, appId, responseBody, timedOut,
                          timer](const nghttp2::asio_http2::client::response& res) {
            int statusCode = res.status_code();

            res.on_data([this, peerPtr, hbh, e2e, commandCode, appId, responseBody, timedOut, timer, statusCode](
                            const uint8_t* data, std::size_t len) {
                if (len > 0) {
                    responseBody->append(reinterpret_cast<const char*>(data), len);
                    return;
                }

                // End of response body - post processing outside nghttp2 callback to avoid re-entrancy
                boost::asio::post(
                    io_, [this, peerPtr, hbh, e2e, commandCode, appId, responseBody, timedOut, timer, statusCode]() {
                        timer->cancel();               // cancel the timeout
                        if (timedOut->load()) return;  // already sent error answer

                        LOGINFORMATIONAL(ert::tracing::Logger::informational(
                            ert::tracing::Logger::asString("h2agent responded %d (%zu bytes)", statusCode,
                                                           responseBody->size()),
                            ERT_FILE_LOCATION));

                        if (metrics_) {
                            auto& counter = h2_client_responses_received_counter_family_ptr_->Add(
                                {{"source", config_.productName},
                                 {"method", "POST"},
                                 {"status_code", std::to_string(statusCode)}});
                            counter.Increment();
                        }

                        if (statusCode != 200 || responseBody->empty()) {
                            LOGWARNING(ert::tracing::Logger::warning(
                                ert::tracing::Logger::asString("h2agent returned non-200 or empty: %d", statusCode),
                                ERT_FILE_LOCATION));

                            // Send DIAMETER_UNABLE_TO_COMPLY (5012)
                            const auto& dict = getDictionary(appId);
                            nlohmann::json errorJson = {{"Result-Code", 5012},
                                                        {"Origin-Host", config_.originHost},
                                                        {"Origin-Realm", config_.originRealm},
                                                        {"_header",
                                                         {{"version", 1},
                                                          {"flags", 0},
                                                          {"command-code", commandCode},
                                                          {"request", false},
                                                          {"application-id", appId},
                                                          {"hop-by-hop-id", hbh},
                                                          {"end-to-end-id", e2e}}}};
                            try {
                                diametercodec::codec::Message answerMsg =
                                    diametercodec::codec::Message::fromJson(errorJson, dict);
                                diametercodec::core::Buffer encoded = answerMsg.encode(dict);
                                diameterServer_->sendAnswer(peerPtr, std::move(encoded));
                            } catch (...) {
                            }
                            return;
                        }

                        // 5. Parse JSON response -> Diameter answer
                        try {
                            nlohmann::json respJson = nlohmann::json::parse(*responseBody);

                            LOGDEBUG(ert::tracing::Logger::debug(
                                ert::tracing::Logger::asString("JSON -> Diameter: %s", responseBody->c_str()),
                                ERT_FILE_LOCATION));

                            // Inject _header for the answer if not present
                            if (!respJson.contains("_header")) {
                                respJson["_header"] = {{"version", 1},
                                                       {"flags", 0},  // answer (no R-bit)
                                                       {"command-code", commandCode},
                                                       {"request", false},
                                                       {"application-id", appId},
                                                       {"hop-by-hop-id", hbh},
                                                       {"end-to-end-id", e2e}};
                            }

                            diametercodec::codec::Message answerMsg =
                                diametercodec::codec::Message::fromJson(respJson, getDictionary(appId));

                            // 6. Encode and send Diameter answer back to peer
                            diametercodec::core::Buffer encoded = answerMsg.encode(getDictionary(appId));

                            LOGINFORMATIONAL(ert::tracing::Logger::informational(
                                ert::tracing::Logger::asString("Sending Diameter answer (%zu bytes) hbh=0x%08x",
                                                               encoded.size(), hbh),
                                ERT_FILE_LOCATION));

                            diameterServer_->sendAnswer(peerPtr, std::move(encoded));

                        } catch (const std::exception& e) {
                            LOGWARNING(ert::tracing::Logger::warning(
                                ert::tracing::Logger::asString("Failed to build Diameter answer: %s", e.what()),
                                ERT_FILE_LOCATION));
                        }
                    });  // end io_.post() for response processing
            });
        });
    });  // end io_service().post()
}

// ============================================================================
// onH2agentOutboundRequest - Outbound: h2agent -> JSON -> Diameter -> Server
// ============================================================================
void Gateway::onH2agentOutboundRequest(const std::string& method, const std::string& uri, const std::string& body,
                                       const nghttp2::asio_http2::header_map& headers,
                                       std::function<void(int, const std::string&)> respond) {
    LOGINFORMATIONAL(ert::tracing::Logger::informational(
        ert::tracing::Logger::asString("Outbound trigger: %s %s (%zu bytes)", method.c_str(), uri.c_str(), body.size()),
        ERT_FILE_LOCATION));

    if (!diameterClient_ || !diameterClient_->isReady()) {
        LOGWARNING(
            ert::tracing::Logger::warning("Diameter client not connected for outbound request", ERT_FILE_LOCATION));
        respond(503, R"({"error":"Diameter peer not connected"})");
        return;
    }

    if (body.empty()) {
        respond(400, R"({"error":"Empty request body"})");
        return;
    }

    // 1. Parse JSON body -> Diameter request
    uint32_t commandCode = 0;
    uint32_t appId = 0;
    try {
        nlohmann::json reqJson = nlohmann::json::parse(body);

        LOGDEBUG(ert::tracing::Logger::debug(
            ert::tracing::Logger::asString("Outbound JSON -> Diameter: %s", body.c_str()), ERT_FILE_LOCATION));

        // Inject _header from URI path if not present
        // URI format: /diameter/<interface>/<command>
        if (!reqJson.contains("_header")) {
            // Extract command info from URI (e.g., /diameter/gx/CCR)

            // Parse interface from URI
            // Expected: /diameter/<interface>/<command>
            size_t firstSlash = uri.find('/', 1);  // skip leading /
            if (firstSlash != std::string::npos) {
                size_t secondSlash = uri.find('/', firstSlash + 1);
                if (secondSlash != std::string::npos) {
                    std::string iface = uri.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                    std::string cmd = uri.substr(secondSlash + 1);

                    // Reverse-map interface to app-id
                    if (iface == "gx")
                        appId = 16777238;
                    else if (iface == "rx")
                        appId = 16777236;
                    else if (iface == "sy")
                        appId = 16777302;
                    else if (iface == "cc")
                        appId = 4;

                    // Reverse-map command abbreviation to code
                    if (cmd == "CCR" || cmd == "CCA")
                        commandCode = 272;
                    else if (cmd == "AAR" || cmd == "AAA")
                        commandCode = 265;
                    else if (cmd == "RAR" || cmd == "RAA")
                        commandCode = 258;
                    else if (cmd == "ASR" || cmd == "ASA")
                        commandCode = 274;
                    else if (cmd == "STR" || cmd == "STA")
                        commandCode = 275;
                    else if (cmd == "ACR" || cmd == "ACA")
                        commandCode = 271;
                    else if (cmd == "SNR" || cmd == "SNA")
                        commandCode = 300;
                }
            }

            reqJson["_header"] = {
                {"version", 1},
                {"flags", 0x80},  // R-bit set (request)
                {"command-code", commandCode},
                {"request", true},
                {"application-id", appId},
                {"hop-by-hop-id", 0},  // will be assigned by peer
                {"end-to-end-id", 0}   // will be assigned by peer
            };
        }

        diametercodec::codec::Message diameterReq =
            diametercodec::codec::Message::fromJson(reqJson, getDictionary(appId));

        // 2. Encode to binary
        diametercodec::core::Buffer encoded = diameterReq.encode(getDictionary(appId));

        LOGINFORMATIONAL(ert::tracing::Logger::informational(
            ert::tracing::Logger::asString("Sending Diameter request (%zu bytes) to peer", encoded.size()),
            ERT_FILE_LOCATION));

        // 3. Send via Diameter client with response callback
        diameterClient_->send(
            std::move(encoded),
            [this, respond, appId](const diametercomm::Peer::Buffer& answerMsg) {
                // 4. Diameter answer received -> decode -> JSON -> HTTP/2 response
                try {
                    diametercodec::codec::Message answer;
                    answer.decode(answerMsg.data(), answerMsg.size(), getDictionary(appId));

                    nlohmann::json respJson = answer.toJson(getDictionary(appId));

                    LOGINFORMATIONAL(ert::tracing::Logger::informational(
                        ert::tracing::Logger::asString("Diameter answer -> JSON response (%zu bytes)",
                                                       respJson.dump().size()),
                        ERT_FILE_LOCATION));

                    respond(200, respJson.dump());

                } catch (const std::exception& e) {
                    LOGWARNING(ert::tracing::Logger::warning(
                        ert::tracing::Logger::asString("Failed to decode Diameter answer: %s", e.what()),
                        ERT_FILE_LOCATION));
                    respond(502, R"({"error":"Failed to decode Diameter answer"})");
                }
            },
            config_.diameterTimeoutMs);

    } catch (const std::exception& e) {
        LOGWARNING(ert::tracing::Logger::warning(
            ert::tracing::Logger::asString("Failed to parse outbound JSON: %s", e.what()), ERT_FILE_LOCATION));
        respond(400, std::string(R"({"error":"Invalid JSON: )") + e.what() + "\"}");
    }
}

// ============================================================================
// Helpers
// ============================================================================
std::string Gateway::getInterfaceName(uint32_t appId) const { return appIdToInterface(appId); }

std::string Gateway::getCommandName(uint32_t commandCode, bool isReq) const {
    std::string name = commandCodeToName(commandCode);
    if (!isReq && name.size() >= 1) {
        // Change last char: CCR->CCA, AAR->AAA, etc.
        name.back() = 'A';
    }
    return name;
}

}  // namespace h2diagent
}  // namespace ert
