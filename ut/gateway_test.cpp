/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests of the Gateway, end to end over loopback sockets:

  test Diameter client --CCR--> [gateway server leg] --HTTP/2--> fake h2agent
  test HTTP/2 client --POST--> [gateway HTTP/2 server] --CCR--> test Diameter server
  test HTTP/2 client --> [gateway admin]

Inbound translation (and the 5012 answers), outbound translation (200, 502,
504), the admin interface and the fault injection, the capabilities AVPs.
*/

#include <gtest/gtest.h>
#include <nghttp2/asio_http2_server.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ert/diametercodec/codec/Message.hpp>
#include <ert/diametercodec/stack/Dictionary.hpp>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/diametercomm/DiameterServer.hpp>
#include <ert/h2diagent/Gateway.hpp>
#include <ert/http2comm/Http2Client.hpp>
#include <ert/http2comm/URLFunctions.hpp>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using ert::diametercomm::DiameterClient;
using ert::diametercomm::DiameterServer;
using ert::diametercomm::Peer;
namespace codec = ert::diametercodec::codec;

namespace {

constexpr uint32_t GxAppId = 16777238;
constexpr uint16_t GatewayDiameterPort = 16868;  // gateway server leg
constexpr uint16_t SutDiameterPort = 16869;      // test Diameter server (gateway client leg)
constexpr uint16_t FakeH2agentPort = 16800;
constexpr uint16_t GatewayHttp2Port = 16805;
constexpr uint16_t GatewayAdminPort = 16879;
constexpr uint16_t GatewayMetricsPort = 16885;

// A small Gx dictionary (the AVPs of a CCR / CCA and the capabilities ones).
const char* GxDictionary = R"json({
  "name": "DictionaryGxUt", "application-id": 16777238,
  "vendor": [{"name": "IETF", "code": 0}, {"name": "3GPP", "code": 10415}],
  "avp": [
    {"name": "Auth-Application-Id", "code": 258, "m-bit": true, "single": {"format": "Unsigned32"}},
    {"name": "Session-Id", "code": 263, "m-bit": true, "single": {"format": "UTF8String"}},
    {"name": "Origin-Host", "code": 264, "m-bit": true, "single": {"format": "DiameterIdentity"}},
    {"name": "Supported-Vendor-Id", "code": 265, "m-bit": true, "single": {"format": "Unsigned32"}},
    {"name": "Vendor-Id", "code": 266, "m-bit": true, "single": {"format": "Unsigned32"}},
    {"name": "Firmware-Revision", "code": 267, "single": {"format": "Unsigned32"}},
    {"name": "Result-Code", "code": 268, "m-bit": true, "single": {"format": "Unsigned32"}},
    {"name": "Product-Name", "code": 269, "single": {"format": "UTF8String"}},
    {"name": "Destination-Realm", "code": 283, "m-bit": true, "single": {"format": "DiameterIdentity"}},
    {"name": "Destination-Host", "code": 293, "m-bit": true, "single": {"format": "DiameterIdentity"}},
    {"name": "Origin-Realm", "code": 296, "m-bit": true, "single": {"format": "DiameterIdentity"}},
    {"name": "CC-Request-Number", "code": 415, "m-bit": true, "single": {"format": "Unsigned32"}},
    {"name": "CC-Request-Type", "code": 416, "m-bit": true, "single": {"format": "Enumerated", "enum": "1-4"}}
  ],
  "command": [
    {"name": "CC-Request", "code": 272, "r-bit": true, "avprule": [
      {"type": "Fixed", "name": "Session-Id"}, {"type": "Mandatory", "name": "Auth-Application-Id"},
      {"type": "Mandatory", "name": "Origin-Host"}, {"type": "Mandatory", "name": "Origin-Realm"},
      {"type": "Mandatory", "name": "Destination-Realm"}, {"type": "Mandatory", "name": "CC-Request-Type"},
      {"type": "Mandatory", "name": "CC-Request-Number"}, {"type": "Optional", "name": "Destination-Host"}]},
    {"name": "CC-Answer", "code": 272, "avprule": [
      {"type": "Fixed", "name": "Session-Id"}, {"type": "Mandatory", "name": "Auth-Application-Id"},
      {"type": "Mandatory", "name": "Origin-Host"}, {"type": "Mandatory", "name": "Origin-Realm"},
      {"type": "Mandatory", "name": "CC-Request-Type"}, {"type": "Mandatory", "name": "CC-Request-Number"},
      {"type": "Optional", "name": "Result-Code"}]}
  ]
})json";

const ert::diametercodec::stack::Dictionary& dictionary() {
    static ert::diametercodec::stack::Dictionary dict;
    static std::once_flag once;
    std::call_once(once, []() { dict.load(nlohmann::json::parse(GxDictionary)); });
    return dict;
}

nlohmann::json ccr(const std::string& sessionId, const std::string& originHost) {
    return {{"Session-Id", sessionId},       {"Auth-Application-Id", GxAppId},     {"Origin-Host", originHost},
            {"Origin-Realm", "example.com"}, {"Destination-Realm", "example.com"}, {"CC-Request-Type", 1},
            {"CC-Request-Number", 0}};
}

nlohmann::json decode(const Peer::Buffer& msg) {
    codec::Message m;
    m.decode(msg.data(), msg.size(), dictionary());
    return m.toJson(dictionary());
}

// Answer of a received request: same header ids, Result-Code given.
Peer::Buffer answerTo(const Peer::Buffer& request, uint32_t resultCode) {
    const nlohmann::json req = decode(request);
    nlohmann::json ans = {{"Session-Id", req.value("Session-Id", "")},
                          {"Auth-Application-Id", GxAppId},
                          {"Origin-Host", "sut.example.com"},
                          {"Origin-Realm", "example.com"},
                          {"CC-Request-Type", req.value("CC-Request-Type", 1)},
                          {"CC-Request-Number", req.value("CC-Request-Number", 0)},
                          {"Result-Code", resultCode}};
    ans["_header"] = req["_header"];
    ans["_header"]["request"] = false;
    ans["_header"]["flags"] = 0;
    return codec::Message::fromJson(ans, dictionary()).encode(dictionary());
}

// HTTP/2 request to the gateway (admin or traffic): {status, body}.
std::pair<int, std::string> h2(uint16_t port, const std::string& method, const std::string& path,
                               const std::string& body = "") {
    // shared ownership is required: send() goes through async_send(), which keeps
    // the client alive with shared_from_this() (bad_weak_ptr on a stack object)
    auto client = std::make_shared<ert::http2comm::Http2Client>("ut", "127.0.0.1", std::to_string(port));
    nghttp2::asio_http2::header_map headers;
    if (!body.empty()) headers.emplace("content-type", nghttp2::asio_http2::header_value{"application/json", false});
    auto r = client->send(method, path, body, headers, std::chrono::milliseconds(3000));
    return {r.statusCode, r.body};
}

nlohmann::json h2json(uint16_t port, const std::string& path) {
    auto r = h2(port, "GET", path);
    EXPECT_EQ(r.first, 200) << path;
    return r.second.empty() ? nlohmann::json() : nlohmann::json::parse(r.second, nullptr, false);
}

template <class Predicate>
bool waitFor(Predicate p, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return p();
}

int openConnections(const std::string& leg) {
    auto r = h2(GatewayAdminPort, "GET", "/admin/v1/peers");
    if (r.first != 200) return 0;
    // named: a range-for over 'parse(...)["connections"]' would iterate a member of
    // a temporary already destroyed (no lifetime extension before C++23)
    const auto peers = nlohmann::json::parse(r.second, nullptr, false);
    if (!peers.contains("connections")) return 0;
    int n = 0;
    for (const auto& c : peers["connections"])
        if (c.value("leg", "") == leg && c.value("state", "") == "open") ++n;
    return n;
}

}  // namespace

// =============================================================================
// Environment: the gateway, a fake h2agent, a SUT Diameter server and client
// =============================================================================
class Gateway_test : public ::testing::Test {
   protected:
    struct Env {
        boost::asio::io_context io{2};
        std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;
        std::vector<std::thread> threads;

        // fake h2agent: inbound requests seen (path, body)
        nghttp2::asio_http2::server::http2 h2agent;
        std::mutex seenMutex;
        std::vector<std::pair<std::string, std::string>> seen;

        std::unique_ptr<DiameterServer> sut;        // the peer the gateway dials
        std::unique_ptr<DiameterClient> sutClient;  // the peer dialing the gateway
        std::unique_ptr<ert::h2diagent::Gateway> gateway;
        std::string dictionaryPath;
        std::string cerPath;
    };
    static Env* env;

    static void SetUpTestSuite() {
        env = new Env;
        env->work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(env->io));
        for (int i = 0; i < 2; ++i) env->threads.emplace_back([]() { env->io.run(); });

        env->dictionaryPath = "/tmp/h2diagent-ut-gx-" + std::to_string(::getpid()) + ".json";
        std::ofstream(env->dictionaryPath) << GxDictionary;

        // fake h2agent: a CCA for every inbound CCR; 500 for the 'fail500' sessions,
        // a non json body for 'notjson'
        env->h2agent.handle(
            "/", [](const nghttp2::asio_http2::server::request& req, const nghttp2::asio_http2::server::response& res) {
                auto body = std::make_shared<std::string>();
                auto path = req.uri().raw_path;
                req.on_data([&res, body, path](const uint8_t* data, std::size_t len) {
                    if (len > 0) {
                        body->append(reinterpret_cast<const char*>(data), len);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(env->seenMutex);
                        env->seen.emplace_back(path, *body);
                    }
                    if (path.find("fail500") != std::string::npos) {
                        res.write_head(500);
                        res.end("");
                        return;
                    }
                    if (path.find("notjson") != std::string::npos) {
                        res.write_head(200);
                        res.end("not json");
                        return;
                    }
                    const auto req = nlohmann::json::parse(*body, nullptr, false);
                    nlohmann::json cca = {{"Session-Id", req.value("Session-Id", "")},
                                          {"Auth-Application-Id", GxAppId},
                                          {"Origin-Host", "agent.example.com"},
                                          {"Origin-Realm", "example.com"},
                                          {"CC-Request-Type", req.value("CC-Request-Type", 1)},
                                          {"CC-Request-Number", req.value("CC-Request-Number", 0)},
                                          {"Result-Code", 2001}};
                    nghttp2::asio_http2::header_map h;
                    h.emplace("content-type", nghttp2::asio_http2::header_value{"application/json", false});
                    res.write_head(200, h);
                    res.end(cca.dump());
                });
            });
        boost::system::error_code ec;
        env->h2agent.listen_and_serve(ec, "127.0.0.1", std::to_string(FakeH2agentPort), true);
        ASSERT_FALSE(ec) << ec.message();

        // SUT Diameter server (the gateway client leg dials it): answers 2001, nothing
        // for the 'noanswer' sessions
        Peer::Config sutConfig{"sut.example.com", "example.com", "127.0.0.1", 0, "Sut", 0, {GxAppId}};
        env->sut = std::make_unique<DiameterServer>(env->io, sutConfig);
        env->sut->setRequestCallback([](std::shared_ptr<Peer> peer, Peer::Buffer&& msg) {
            if (decode(msg).value("Session-Id", "").find("noanswer") != std::string::npos) return;
            env->sut->sendAnswer(peer, answerTo(msg, 2001));
        });
        env->sut->listen("127.0.0.1", SutDiameterPort);

        // the gateway, with every interface and an extra CER AVP
        env->cerPath = "/tmp/h2diagent-ut-cer-" + std::to_string(::getpid()) + ".json";
        std::ofstream(env->cerPath) << R"({"Supported-Vendor-Id": 10415, "Product-Name": "h2diagent-ut"})";
        ert::h2diagent::GatewayConfig config;
        config.diameterPort = GatewayDiameterPort;
        config.diameterPeerHost = "127.0.0.1";
        config.diameterPeerPort = SutDiameterPort;
        config.originHost = "gw.example.com";
        config.originRealm = "example.com";
        config.productName = "h2diagent-ut";
        config.watchdogIntervalSec = 0;
        config.dictionaryPaths = {env->dictionaryPath, "/tmp/h2diagent-ut-missing.json"};
        config.diameterTimeoutMs = 500;
        config.h2agentHost = "127.0.0.1";
        config.h2agentPort = FakeH2agentPort;
        config.http2ServerPort = GatewayHttp2Port;
        config.adminPort = GatewayAdminPort;
        config.prometheusPort = GatewayMetricsPort;
        config.metricsAdditionalLabelAvps = {"CC-Request-Type"};
        config.cerFile = env->cerPath;
        config.cerAvps = nlohmann::json::parse(R"({"Supported-Vendor-Id": 10415, "Product-Name": "h2diagent-ut"})");
        env->gateway = std::make_unique<ert::h2diagent::Gateway>(env->io, config);
        env->gateway->start();

        // the peer dialing the gateway server leg
        Peer::Config clientConfig{"client.example.com", "example.com", "127.0.0.1", 0, "UtClient", 0, {GxAppId}};
        env->sutClient = std::make_unique<DiameterClient>(env->io, clientConfig);
        env->sutClient->setReconnectEnabled(false);
        env->sutClient->connect("127.0.0.1", GatewayDiameterPort);

        ASSERT_TRUE(waitFor([]() { return openConnections("client") == 1 && openConnections("server") == 1; }))
            << h2(GatewayAdminPort, "GET", "/admin/v1/peers").second;
    }

    static void TearDownTestSuite() {
        env->sutClient->close();
        env->gateway->stop();
        env->sut->close();
        env->h2agent.stop();
        env->h2agent.join();
        env->work.reset();
        env->io.stop();
        for (auto& t : env->threads) t.join();
        std::remove(env->dictionaryPath.c_str());
        std::remove(env->cerPath.c_str());
        delete env;
        env = nullptr;
    }

    // Inbound CCR through the gateway server leg: the answer, decoded.
    static nlohmann::json inbound(const std::string& sessionId) {
        auto promise = std::make_shared<std::promise<Peer::Buffer>>();
        auto future = promise->get_future();
        env->sutClient->send(
            codec::Message::fromJson(
                [&]() {
                    auto j = ccr(sessionId, "client.example.com");
                    j["_header"] = {
                        {"version", 1}, {"command-code", 272}, {"request", true}, {"application-id", GxAppId}};
                    return j;
                }(),
                dictionary())
                .encode(dictionary()),
            [promise](const Peer::Buffer& answer) { promise->set_value(answer); }, 3000);
        if (future.wait_for(std::chrono::seconds(4)) != std::future_status::ready) return nullptr;
        return decode(future.get());
    }
};

Gateway_test::Env* Gateway_test::env = nullptr;

TEST_F(Gateway_test, InboundTranslationToH2agent) {
    const std::string sid = "client.example.com;ut-in;1";
    const auto cca = inbound(sid);
    ASSERT_FALSE(cca.is_null());
    EXPECT_EQ(cca["Result-Code"], 2001);
    EXPECT_EQ(cca["Session-Id"], sid);
    EXPECT_EQ(cca["Origin-Host"], "agent.example.com");

    // the h2agent got the per-session uri (Session-Id percent-encoded) and the json body
    std::lock_guard<std::mutex> lock(env->seenMutex);
    bool found = false;
    for (const auto& [path, body] : env->seen) {
        if (path != "/diameter/gx/CCR/" + ert::http2comm::URLFunctions::encode(sid)) continue;
        found = true;
        const auto j = nlohmann::json::parse(body);
        EXPECT_EQ(j["Session-Id"], sid);
        EXPECT_EQ(j["CC-Request-Type"], 1);
        EXPECT_TRUE(j.contains("_header"));
    }
    EXPECT_TRUE(found);
}

TEST_F(Gateway_test, InboundUnableToComply) {
    // the h2agent answers 500, or a body which is not a Diameter answer: 5012, Session-Id echoed
    for (const std::string sid : {"client.example.com;fail500;1", "client.example.com;notjson;1"}) {
        SCOPED_TRACE(sid);
        const auto answer = inbound(sid);
        ASSERT_FALSE(answer.is_null());
        EXPECT_EQ(answer["Result-Code"], 5012);
        EXPECT_EQ(answer["Session-Id"], sid);
    }
}

TEST_F(Gateway_test, OutboundTranslation) {
    const std::string sid = "gw.example.com;ut-out;1";
    auto r = h2(GatewayHttp2Port, "POST", "/diameter/gx/CCR", ccr(sid, "gw.example.com").dump());
    ASSERT_EQ(r.first, 200) << r.second;
    const auto cca = nlohmann::json::parse(r.second);
    EXPECT_EQ(cca["Result-Code"], 2001);
    EXPECT_EQ(cca["Session-Id"], sid);
    EXPECT_EQ(cca["Origin-Host"], "sut.example.com");

    // no answer in time: 504
    r = h2(GatewayHttp2Port, "POST", "/diameter/gx/CCR", ccr("gw.example.com;noanswer;1", "gw.example.com").dump());
    EXPECT_EQ(r.first, 504) << r.second;

    // a body which is not json: no Diameter request (an error answer, not 200)
    r = h2(GatewayHttp2Port, "POST", "/diameter/gx/CCR", "not json");
    EXPECT_NE(r.first, 200);
}

TEST_F(Gateway_test, AdminInterface) {
    EXPECT_EQ(h2json(GatewayAdminPort, "/admin/v1/health"), nlohmann::json({{"status", "healthy"}}));

    const auto peers = h2json(GatewayAdminPort, "/admin/v1/peers");
    ASSERT_EQ(peers["connections"].size(), 2u);
    for (const auto& c : peers["connections"]) {
        EXPECT_EQ(c["state"], "open");
        EXPECT_EQ(c["transport"], "tcp");
        EXPECT_EQ(c["watchdog"], "answer");
        EXPECT_EQ(c["peerOriginHost"], c["leg"] == "client" ? "sut.example.com" : "client.example.com");
    }

    const auto conf = h2json(GatewayAdminPort, "/admin/v1/configuration");
    EXPECT_EQ(conf["adminPort"], GatewayAdminPort);
    EXPECT_EQ(conf["http2ServerPort"], GatewayHttp2Port);
    EXPECT_EQ(conf["diameter"]["dictionaries"].size(), 1u);  // the missing one is not loaded
    EXPECT_EQ(conf["diameter"]["dictionaries"][0]["applicationId"], GxAppId);
    EXPECT_EQ(conf["diameter"]["cer"]["avps"]["Supported-Vendor-Id"], 10415);

    const auto saved = h2(GatewayAdminPort, "GET", "/admin/v1/logging").second;
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/logging?level=Error").first, 200);
    EXPECT_EQ(h2(GatewayAdminPort, "GET", "/admin/v1/logging").second, "Error");
    h2(GatewayAdminPort, "PUT", "/admin/v1/logging?level=" + saved);

    EXPECT_EQ(h2(GatewayAdminPort, "GET", "/admin/v1/unknown").first, 404);
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/client/7?action=close").first, 404);
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/server/7?watchdog=mute").first, 404);
}

TEST_F(Gateway_test, FaultInjection) {
    // watchdog flags on both legs
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/client/0?watchdog=mute").first, 200);
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/server/0?watchdog=mute").first, 200);
    const auto peers = h2json(GatewayAdminPort, "/admin/v1/peers");  // named: see openConnections()
    ASSERT_EQ(peers["connections"].size(), 2u);
    for (const auto& c : peers["connections"]) EXPECT_EQ(c["watchdog"], "mute");
    h2(GatewayAdminPort, "PUT", "/admin/v1/peers/client/0?watchdog=answer");
    h2(GatewayAdminPort, "PUT", "/admin/v1/peers/server/0?watchdog=answer");

    // abrupt close of the client leg: it reconnects
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/client/0?action=close").first, 200);
    EXPECT_TRUE(waitFor([]() { return openConnections("client") == 1; }, std::chrono::milliseconds(8000)));

    // DPR: no reconnection, the director gets 502; then connect
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/client/0?action=dpr&cause=BUSY").first, 200);
    EXPECT_TRUE(waitFor([]() { return openConnections("client") == 0; }));
    auto r = h2(GatewayHttp2Port, "POST", "/diameter/gx/CCR", ccr("gw.example.com;ut-502;1", "gw.example.com").dump());
    EXPECT_EQ(r.first, 502) << r.second;
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/client/0?action=connect").first, 200);
    EXPECT_TRUE(waitFor([]() { return openConnections("client") == 1; }, std::chrono::milliseconds(8000)));

    // CEA Result-Code of the server leg: shown, then back to 2001
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/configuration?ceaResultCode=5010").first, 200);
    EXPECT_EQ(h2json(GatewayAdminPort, "/admin/v1/configuration")["diameter"]["ceaResultCode"], 5010);
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/configuration?ceaResultCode=2001").first, 200);

    // the server leg peer: an abrupt close removes it from the listing
    EXPECT_EQ(h2(GatewayAdminPort, "PUT", "/admin/v1/peers/server/0?action=close").first, 200);
    EXPECT_TRUE(waitFor([]() { return openConnections("server") == 0; }));
    // the test client reconnects to keep the environment as found
    env->sutClient->connect("127.0.0.1", GatewayDiameterPort);
    EXPECT_TRUE(waitFor([]() { return openConnections("server") == 1; }));
}

// =============================================================================
// Server-leg outbound requests: a gateway WITHOUT client leg (it plays the node
// the peers connect to, e.g. a PCRF) sends the director requests (e.g. a RAR)
// on the server leg, to the peer named by Destination-Host, round-robin.
// =============================================================================
namespace {
constexpr uint16_t ServerOnlyDiameterPort = 16968;  // server-only gateway listener
constexpr uint16_t ServerOnlyHttp2Port = 16905;     // its outbound trigger port
constexpr uint16_t NoPeersDiameterPort = 16970;     // server-only gateway nobody connects to
constexpr uint16_t NoPeersHttp2Port = 16906;
constexpr uint16_t UnusedH2agentPort = 16901;  // no inbound traffic in these tests
}  // namespace

class GatewayServerLeg_test : public ::testing::Test {
   protected:
    // A peer connected to the server-only gateway: answers every request it
    // receives (2001), except the 'noanswer' sessions, and counts them.
    struct ConnectedPeer {
        std::unique_ptr<DiameterClient> client;
        std::atomic<int> received{0};
    };
    struct Env {
        boost::asio::io_context io{2};
        std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;
        std::vector<std::thread> threads;
        std::string dictionaryPath;
        std::unique_ptr<ert::h2diagent::Gateway> gateway;         // with connected peers
        std::unique_ptr<ert::h2diagent::Gateway> gatewayNoPeers;  // listening, nobody connected
        // two connections of one peer (same Origin-Host, e.g. an AF) and one of another
        std::vector<std::unique_ptr<ConnectedPeer>> peers;
    };
    static Env* env;

    static ert::h2diagent::GatewayConfig serverOnlyConfig(uint16_t diameterPort, uint16_t http2Port) {
        ert::h2diagent::GatewayConfig config;
        config.diameterPort = diameterPort;  // no diameterPeerHost: no client leg
        config.originHost = "pcrf.example.com";
        config.originRealm = "example.com";
        config.productName = "h2diagent-ut-server-leg";
        config.watchdogIntervalSec = 0;
        config.dictionaryPaths = {env->dictionaryPath};
        config.diameterTimeoutMs = 500;
        config.h2agentHost = "127.0.0.1";
        config.h2agentPort = UnusedH2agentPort;
        config.http2ServerPort = http2Port;
        config.adminPort = 0;
        config.metricsEnabled = false;
        return config;
    }

    static void SetUpTestSuite() {
        env = new Env;
        env->work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(env->io));
        for (int i = 0; i < 2; ++i) env->threads.emplace_back([]() { env->io.run(); });

        env->dictionaryPath = "/tmp/h2diagent-ut-gx-server-leg-" + std::to_string(::getpid()) + ".json";
        std::ofstream(env->dictionaryPath) << GxDictionary;

        env->gateway = std::make_unique<ert::h2diagent::Gateway>(
            env->io, serverOnlyConfig(ServerOnlyDiameterPort, ServerOnlyHttp2Port));
        env->gateway->start();
        env->gatewayNoPeers =
            std::make_unique<ert::h2diagent::Gateway>(env->io, serverOnlyConfig(NoPeersDiameterPort, NoPeersHttp2Port));
        env->gatewayNoPeers->start();

        for (const std::string originHost : {"af.example.com", "af.example.com", "other.example.com"}) {
            auto peer = std::make_unique<ConnectedPeer>();
            Peer::Config config{originHost, "example.com", "127.0.0.1", 0, "UtPeer", 0, {GxAppId}};
            peer->client = std::make_unique<DiameterClient>(env->io, config);
            peer->client->setReconnectEnabled(false);
            ConnectedPeer* raw = peer.get();
            peer->client->setRequestCallback([raw](std::shared_ptr<Peer>, Peer::Buffer&& msg) {
                raw->received++;
                if (decode(msg).value("Session-Id", "").find("noanswer") != std::string::npos) return;
                raw->client->sendAnswer(answerTo(msg, 2001));
            });
            peer->client->connect("127.0.0.1", ServerOnlyDiameterPort);
            env->peers.push_back(std::move(peer));
        }
        ASSERT_TRUE(waitFor([]() {
            for (const auto& p : env->peers)
                if (!p->client->isReady()) return false;
            return true;
        }));
    }

    static void TearDownTestSuite() {
        for (auto& p : env->peers) p->client->close();
        env->gateway->stop();
        env->gatewayNoPeers->stop();
        env->work.reset();
        env->io.stop();
        for (auto& t : env->threads) t.join();
        std::remove(env->dictionaryPath.c_str());
        delete env;
        env = nullptr;
    }

    // Server-initiated request (the CCR of the ut dictionary plays it), with an
    // optional Destination-Host.
    static std::string request(const std::string& sessionId, const std::string& destinationHost) {
        auto j = ccr(sessionId, "pcrf.example.com");
        if (!destinationHost.empty()) j["Destination-Host"] = destinationHost;
        return j.dump();
    }

    static void resetCounts() {
        for (auto& p : env->peers) p->received = 0;
    }
};

GatewayServerLeg_test::Env* GatewayServerLeg_test::env = nullptr;

TEST_F(GatewayServerLeg_test, DestinationHostRoutesRoundRobinToItsConnections) {
    resetCounts();
    for (int i = 0; i < 4; ++i) {
        const std::string sid = "pcrf.example.com;ut-rar;" + std::to_string(i);
        auto r = h2(ServerOnlyHttp2Port, "POST", "/diameter/gx/CCR", request(sid, "af.example.com"));
        ASSERT_EQ(r.first, 200) << r.second;
        const auto answer = nlohmann::json::parse(r.second);
        EXPECT_EQ(answer["Result-Code"], 2001);
        EXPECT_EQ(answer["Session-Id"], sid);
    }
    // only the af.example.com connections, alternated; never the other peer
    EXPECT_EQ(env->peers[0]->received.load(), 2);
    EXPECT_EQ(env->peers[1]->received.load(), 2);
    EXPECT_EQ(env->peers[2]->received.load(), 0);
}

TEST_F(GatewayServerLeg_test, DestinationHostCaseInsensitive) {
    resetCounts();
    auto r =
        h2(ServerOnlyHttp2Port, "POST", "/diameter/gx/CCR", request("pcrf.example.com;ut-case;1", "OTHER.example.COM"));
    ASSERT_EQ(r.first, 200) << r.second;
    EXPECT_EQ(env->peers[2]->received.load(), 1);
}

TEST_F(GatewayServerLeg_test, NoDestinationHostAnyOpenConnection) {
    resetCounts();
    for (int i = 0; i < 3; ++i) {
        auto r = h2(ServerOnlyHttp2Port, "POST", "/diameter/gx/CCR",
                    request("pcrf.example.com;ut-any;" + std::to_string(i), ""));
        ASSERT_EQ(r.first, 200) << r.second;
    }
    // round-robin over the three connections
    for (const auto& p : env->peers) EXPECT_EQ(p->received.load(), 1);
}

TEST_F(GatewayServerLeg_test, UnknownDestinationHost) {
    // never sent to another node: 502, nothing received
    resetCounts();
    auto r = h2(ServerOnlyHttp2Port, "POST", "/diameter/gx/CCR",
                request("pcrf.example.com;ut-unknown;1", "pcef.example.com"));
    EXPECT_EQ(r.first, 502) << r.second;
    const auto body = nlohmann::json::parse(r.second, nullptr, false);
    EXPECT_EQ(body.value("destinationHost", ""), "pcef.example.com");
    EXPECT_EQ(body.value("peerState", ""), "closed");
    for (const auto& p : env->peers) EXPECT_EQ(p->received.load(), 0);
}

TEST_F(GatewayServerLeg_test, AnswerTimeout) {
    resetCounts();
    auto r =
        h2(ServerOnlyHttp2Port, "POST", "/diameter/gx/CCR", request("pcrf.example.com;noanswer;1", "af.example.com"));
    EXPECT_EQ(r.first, 504) << r.second;
    EXPECT_EQ(env->peers[0]->received.load() + env->peers[1]->received.load(), 1);
}

TEST_F(GatewayServerLeg_test, NoConnectedPeer) {
    // listening, nobody connected: 502 with or without Destination-Host
    for (const std::string dest : {"", "af.example.com"}) {
        SCOPED_TRACE(dest);
        auto r = h2(NoPeersHttp2Port, "POST", "/diameter/gx/CCR", request("pcrf.example.com;ut-nopeer;1", dest));
        EXPECT_EQ(r.first, 502) << r.second;
    }
}
