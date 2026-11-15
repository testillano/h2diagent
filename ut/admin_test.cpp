/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests for the admin interface (Admin.hpp): routing, documents, the
connection snapshot over real local connections, and the state gauge values.
*/

#include <gtest/gtest.h>

#include <chrono>
#include <ert/diametercomm/DiameterServer.hpp>
#include <ert/h2diagent/Admin.hpp>
#include <ert/h2diagent/DiameterClientPool.hpp>

using ert::diametercomm::DiameterServer;
using ert::diametercomm::Peer;
using ert::diametercomm::Transport;
using ert::h2diagent::DiameterClientPool;
using ert::h2diagent::GatewayConfig;
namespace admin = ert::h2diagent::admin;

namespace {
std::vector<admin::ConnectionInfo> twoConnections() {
    admin::ConnectionInfo client{"client",     0, "tcp", "open", "10.0.0.1:40000", "10.0.0.2:3868", "peer.example.com",
                                 "example.com"};
    admin::ConnectionInfo server{"server", 0, "sctp", "waitCER", "10.0.0.1:3868", "10.0.0.3:50000", "", ""};
    return {client, server};
}

admin::Response get(const std::string& path, const std::string& query = "") {
    return admin::handle("GET", path, query, twoConnections, []() { return nlohmann::json{{"k", 1}}; });
}
}  // namespace

TEST(Admin, StateNamesAndGaugeValues) {
    EXPECT_EQ(admin::stateName(Peer::State::Open), "open");
    EXPECT_EQ(admin::stateName(Peer::State::WaitCEA), "waitCEA");
    EXPECT_EQ(admin::stateName(Peer::State::WaitCER), "waitCER");
    EXPECT_EQ(admin::stateName(Peer::State::Closing), "closing");
    EXPECT_EQ(admin::stateName(Peer::State::Closed), "closed");
    EXPECT_EQ(admin::stateName(Peer::State::Closed, true), "reconnecting");
    EXPECT_EQ(admin::stateName(Peer::State::Open, true), "open");  // the flag only qualifies a closed connection

    EXPECT_EQ(admin::stateValue("closed"), 0);
    EXPECT_EQ(admin::stateValue("open"), 1);
    EXPECT_EQ(admin::stateValue("reconnecting"), 2);
    EXPECT_EQ(admin::stateValue("waitCEA"), 3);
    EXPECT_EQ(admin::stateValue("waitCER"), 3);
    EXPECT_EQ(admin::stateValue("closing"), 4);
    EXPECT_EQ(admin::stateValue("unknown"), 0);
}

TEST(Admin, QueryValue) {
    EXPECT_EQ(admin::queryValue("level=Debug", "level"), "Debug");
    EXPECT_EQ(admin::queryValue("a=1&level=Error&b=2", "level"), "Error");
    EXPECT_EQ(admin::queryValue("levels=Debug", "level"), "");
    EXPECT_EQ(admin::queryValue("level", "level"), "");
    EXPECT_EQ(admin::queryValue("", "level"), "");
}

TEST(Admin, HealthPeersConfiguration) {
    auto r = get("/admin/v1/health");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(nlohmann::json::parse(r.body), nlohmann::json({{"status", "healthy"}}));
    EXPECT_EQ(r.contentType, "application/json");
    EXPECT_EQ(get("/admin/v1/health/").status, 200);  // trailing slash

    r = get("/admin/v1/peers");
    EXPECT_EQ(r.status, 200);
    const auto peers = nlohmann::json::parse(r.body);
    ASSERT_EQ(peers["connections"].size(), 2u);
    EXPECT_EQ(peers["connections"][0],
              nlohmann::json::parse(R"({"leg":"client","index":0,"transport":"tcp","state":"open",
                  "localAddress":"10.0.0.1:40000","peerAddress":"10.0.0.2:3868",
                  "peerOriginHost":"peer.example.com","peerOriginRealm":"example.com"})"));
    EXPECT_FALSE(peers["connections"][1].contains("peerOriginHost"));  // empty texts omitted

    r = get("/admin/v1/configuration");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(nlohmann::json::parse(r.body), nlohmann::json({{"k", 1}}));

    // no peers callback: an empty listing
    r = admin::handle("GET", "/admin/v1/peers", "", nullptr, nullptr);
    EXPECT_EQ(nlohmann::json::parse(r.body), nlohmann::json::parse(R"({"connections":[]})"));
}

TEST(Admin, NotFoundAndMethodNotAllowed) {
    EXPECT_EQ(get("/admin/v1/unknown").status, 404);
    EXPECT_EQ(get("/admin/v2/health").status, 404);
    EXPECT_EQ(get("/").status, 404);
    EXPECT_EQ(admin::handle("POST", "/admin/v1/peers", "", twoConnections, nullptr).status, 405);
    EXPECT_EQ(admin::handle("DELETE", "/admin/v1/health", "", nullptr, nullptr).status, 405);
    EXPECT_EQ(admin::handle("POST", "/admin/v1/logging", "level=Debug", nullptr, nullptr).status, 405);
}

TEST(Admin, Logging) {
    const auto saved = ert::tracing::Logger::getLevel();
    auto r = admin::handle("PUT", "/admin/v1/logging", "level=Error", nullptr, nullptr);
    EXPECT_EQ(r.status, 200);
    r = get("/admin/v1/logging");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "Error");
    EXPECT_EQ(r.contentType, "text/plain");

    EXPECT_EQ(admin::handle("PUT", "/admin/v1/logging", "level=Loud", nullptr, nullptr).status, 400);
    EXPECT_EQ(admin::handle("PUT", "/admin/v1/logging", "", nullptr, nullptr).status, 400);
    EXPECT_EQ(get("/admin/v1/logging").body, "Error");  // unchanged by the invalid ones
    ert::tracing::Logger::setLevel(saved);
}

TEST(Admin, ConfigurationHasNoSecrets) {
    GatewayConfig c;
    c.originHost = "gw.example.com";
    c.diameterPeerHost = "peer.example.com";
    c.diameterConnections = 2;
    c.diameterServerKeyFile = "/k.pem";
    c.diameterServerCrtFile = "/c.pem";
    c.diameterServerKeyPassword = "s3cret";
    c.diameterClientKeyPassword = "s3cret";
    c.decodeAs = {{"Framed-IP-Address", "ipv4"}};
    const auto j = admin::configurationJson(c, {{"/opt/stacks/gx.json", 16777238}});

    EXPECT_EQ(j.dump().find("s3cret"), std::string::npos);
    EXPECT_EQ(j["diameter"]["originHost"], "gw.example.com");
    EXPECT_EQ(j["diameter"]["serverTls"], true);
    EXPECT_EQ(j["diameter"]["peer"]["connections"], 2);
    EXPECT_EQ(j["diameter"]["peer"]["localHost"], "auto");
    EXPECT_EQ(j["diameter"]["dictionaries"][0]["applicationId"], 16777238);
    EXPECT_EQ(j["diameter"]["decodeAs"]["Framed-IP-Address"], "ipv4");
    // the default ports: h2agent ones + 5
    EXPECT_EQ(j["http2ServerPort"], 8005);
    EXPECT_EQ(j["adminPort"], 8079);
    EXPECT_EQ(j["metrics"]["prometheusPort"], 8085);

    EXPECT_FALSE(j["diameter"].contains("cer"));  // no --cer / --cea
    c.cerFile = "/config/cer.json";
    c.cerAvps = {{"Supported-Vendor-Id", 10415}};
    const auto withCer = admin::configurationJson(c, {});
    EXPECT_EQ(withCer["diameter"]["cer"]["file"], "/config/cer.json");
    EXPECT_EQ(withCer["diameter"]["cer"]["avps"]["Supported-Vendor-Id"], 10415);
    EXPECT_FALSE(withCer["diameter"].contains("cea"));

    c.diameterPeerHost.clear();
    EXPECT_FALSE(admin::configurationJson(c, {})["diameter"].contains("peer"));  // no client leg
}

class AdminConnections_test : public ::testing::Test {
   protected:
    boost::asio::io_context io_;
    Peer::Config serverConfig() {
        return {"server.example.com", "example.com", "127.0.0.1", 0, "TestServer", 0, {16777238}};
    }
    Peer::Config clientConfig() {
        return {"client.example.com", "example.com", "127.0.0.1", 0, "TestClient", 0, {16777238}};
    }
    void runFor(std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
    }
};

// The snapshot over real connections: client leg (pool), server leg, and a client
// connection that lost its peer (reconnecting: gauge value 2).
TEST_F(AdminConnections_test, ClientAndServerLegs) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15911);

    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 1);
    // before connecting: closed, no addresses
    auto info = admin::clientInfo(pool.client(0), 0);
    EXPECT_EQ(info.state, "closed");
    EXPECT_EQ(info.peerAddress, "");

    pool.setReconnectEnabled(true);
    pool.setReconnectBackoff(std::chrono::milliseconds(5000), std::chrono::milliseconds(5000));
    pool.connectAll("127.0.0.1", 15911);
    runFor(std::chrono::milliseconds(400));

    info = admin::clientInfo(pool.client(0), 0);
    EXPECT_EQ(info.leg, "client");
    EXPECT_EQ(info.transport, "tcp");
    EXPECT_EQ(info.state, "open");
    EXPECT_EQ(info.peerAddress, "127.0.0.1:15911");
    EXPECT_EQ(info.localAddress.rfind("127.0.0.1:", 0), 0u) << info.localAddress;
    EXPECT_EQ(info.peerOriginHost, "server.example.com");
    EXPECT_EQ(info.peerOriginRealm, "example.com");
    EXPECT_EQ(admin::stateValue(info.state), 1);

    auto peers = server.peers();
    ASSERT_EQ(peers.size(), 1u);
    const auto serverSide = admin::serverInfo(*peers[0], 0);
    EXPECT_EQ(serverSide.leg, "server");
    EXPECT_EQ(serverSide.state, "open");
    EXPECT_EQ(serverSide.localAddress, "127.0.0.1:15911");
    EXPECT_EQ(serverSide.peerAddress, info.localAddress);
    EXPECT_EQ(serverSide.peerOriginHost, "client.example.com");

    // the peer goes away: the client connection is reconnecting, the configured
    // peer address is still shown, the remote identity is not
    server.close();
    runFor(std::chrono::milliseconds(300));
    info = admin::clientInfo(pool.client(0), 0);
    EXPECT_EQ(info.state, "reconnecting");
    EXPECT_EQ(admin::stateValue(info.state), 2);
    EXPECT_EQ(info.peerAddress, "127.0.0.1:15911");
    EXPECT_EQ(info.peerOriginHost, "");

    pool.setReconnectEnabled(false);
    pool.disconnectAll();
}
