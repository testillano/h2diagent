/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests for DiameterClientPool (the --diameter-connections pool).

These exercise the pool against a real local diametercomm::DiameterServer on
127.0.0.1, mirroring the diametercomm library's own client/server tests. The
key properties verified:
  - a pool of 1 behaves like a single connection (default, zero-risk);
  - a pool of N connects all N and stripes pick()/pickReady() round-robin;
  - anyReady()/size()/disconnectAll() behave as expected.
*/

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ert/diametercomm/DiameterServer.hpp>
#include <ert/h2diagent/DiameterClientPool.hpp>
#include <set>

using ert::diametercomm::DiameterServer;
using ert::diametercomm::Peer;
using ert::diametercomm::Transport;
using ert::h2diagent::DiameterClientPool;

class DiameterClientPool_test : public ::testing::Test {
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

// A pool of 1 is the default: it must connect the single client and always pick
// that same ready client -- i.e. behave like the historical single connection.
TEST_F(DiameterClientPool_test, PoolOfOne_ConnectsAndPicksSameReadyClient) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15901);

    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, /*connections=*/1);
    pool.setReconnectEnabled(false);
    EXPECT_EQ(pool.size(), 1u);

    pool.connectAll("127.0.0.1", 15901);
    runFor(std::chrono::milliseconds(500));

    EXPECT_TRUE(pool.anyReady());
    auto* a = pool.pickReady();
    auto* b = pool.pickReady();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);  // only one client -> always the same
    EXPECT_TRUE(a->isReady());

    pool.disconnectAll();
    server.close();
}

// A pool of N opens N independent connections to the same peer; the server sees
// N active peers and the pool reports size N and at least one ready.
TEST_F(DiameterClientPool_test, PoolOfN_AllConnectAndBecomeReady) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15902);

    const std::size_t N = 3;
    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, N);
    pool.setReconnectEnabled(false);
    EXPECT_EQ(pool.size(), N);

    pool.connectAll("127.0.0.1", 15902);
    runFor(std::chrono::milliseconds(700));

    EXPECT_TRUE(pool.anyReady());
    EXPECT_EQ(server.activePeerCount(), N);  // N separate CER/CEA from same origin-host

    pool.disconnectAll();
    server.close();
}

// pickReady() must stripe across all ready connections (round-robin), i.e. over
// 2N calls it returns each of the N distinct clients at least once.
TEST_F(DiameterClientPool_test, PickReady_CyclesAcrossReadyConnections) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15903);

    const std::size_t N = 3;
    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, N);
    pool.setReconnectEnabled(false);
    pool.connectAll("127.0.0.1", 15903);
    runFor(std::chrono::milliseconds(700));
    ASSERT_TRUE(pool.anyReady());

    std::set<ert::diametercomm::DiameterClient*> distinct;
    for (std::size_t i = 0; i < 2 * N; ++i) {
        auto* c = pool.pickReady();
        ASSERT_NE(c, nullptr);
        distinct.insert(c);
    }
    // All N connections are used by the round-robin striping.
    EXPECT_EQ(distinct.size(), N);

    pool.disconnectAll();
    server.close();
}

// Before connecting, no connection is ready and pickReady() returns nullptr.
TEST_F(DiameterClientPool_test, AnyReady_FalseBeforeConnect) {
    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 2);
    pool.setReconnectEnabled(false);
    EXPECT_FALSE(pool.anyReady());
    EXPECT_EQ(pool.pickReady(), nullptr);
    // pick() still returns a (not-ready) client -- selection is independent of readiness.
    EXPECT_NE(pool.pick(), nullptr);
}

// disconnectAll() closes every pooled connection: the server ends with 0 peers.
TEST_F(DiameterClientPool_test, DisconnectAll_ClosesEveryConnection) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15904);

    const std::size_t N = 2;
    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, N);
    pool.setReconnectEnabled(false);
    pool.connectAll("127.0.0.1", 15904);
    runFor(std::chrono::milliseconds(600));
    ASSERT_EQ(server.activePeerCount(), N);

    pool.disconnectAll(0);
    runFor(std::chrono::milliseconds(500));
    EXPECT_EQ(server.activePeerCount(), 0u);

    server.close();
}

// Defensive: requesting 0 connections is clamped to 1 (never an empty pool).
TEST_F(DiameterClientPool_test, ZeroConnectionsClampedToOne) {
    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, /*connections=*/0);
    EXPECT_EQ(pool.size(), 1u);
}

// --- Peer-down gate behind the 502 Bad Gateway response ---------------------
//
// Gateway::onH2agentOutboundRequest decides to fail fast with 502 Bad Gateway
// (instead of blocking/dropping the POST) exactly when there is no open Diameter
// peer to the stack, i.e. when `!diameterClientPool_ || !diameterClientPool_->anyReady()`
// (and, as a race guard, when pickReady() yields nullptr). These tests lock that
// predicate for the two peer-down conditions that trigger the 502 at runtime:
// "peer never came up" and "peer dropped mid-session". The full HTTP response
// assertion (status 502 + x-diameter-peer-state header) is covered at
// component/integration level, since the handler needs the HTTP/2 server stack.

TEST_F(DiameterClientPool_test, BadGatewayGate_PeerNeverUp) {
    // Peer never established: the 502 gate (!anyReady() && pickReady()==nullptr) holds.
    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 2);
    pool.setReconnectEnabled(false);
    EXPECT_FALSE(pool.anyReady());         // -> gateway would answer 502
    EXPECT_EQ(pool.pickReady(), nullptr);  // -> race-guard branch would also answer 502
}

TEST_F(DiameterClientPool_test, BadGatewayGate_PeerDroppedMidSession) {
    // Peer established and then dropped (ASR/watchdog loss/stack restart): once the
    // connections are gone, the same 502 gate holds again for new requests.
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15905);

    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 2);
    pool.setReconnectEnabled(false);
    pool.connectAll("127.0.0.1", 15905);
    runFor(std::chrono::milliseconds(600));
    ASSERT_TRUE(pool.anyReady());  // peer up: requests flow normally (no 502)

    // Peer goes away:
    pool.disconnectAll(0);
    runFor(std::chrono::milliseconds(400));
    EXPECT_FALSE(pool.anyReady());  // -> gateway would now answer 502
    EXPECT_EQ(pool.pickReady(), nullptr);

    server.close();
}

// =============================================================================
// Cross-connection / cross-leg answer correlation
// =============================================================================
namespace {
Peer::Buffer appRequest() {
    Peer::Buffer msg(20, 0);
    msg[0] = 1;
    msg[3] = 20;
    msg[4] = 0x80;  // R-bit
    msg[6] = 1;
    msg[7] = 0x13;  // 275 (STR)
    msg[11] = 4;
    return msg;
}
Peer::Buffer answerFor(const Peer::Buffer& req) {
    Peer::Buffer a(req.begin(), req.begin() + 20);
    a[3] = 20;
    a[4] = 0x00;
    return a;
}
}  // namespace

// The stack answers on the OTHER pooled connection: the pool resolves it on
// the connection that sent the request (no timeout, no request delivery).
TEST_F(DiameterClientPool_test, AnswerOnOtherPooledConnectionIsResolved) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15906);
    std::vector<std::shared_ptr<Peer>> serverPeers;
    server.setPeerEventCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) serverPeers.push_back(p);
    });
    // Answer on whichever server-side peer did NOT receive the request.
    server.setRequestCallback([&](std::shared_ptr<Peer> p, Peer::Buffer&& msg) {
        for (auto& other : serverPeers) {
            if (other != p) {
                other->send(answerFor(msg));
                return;
            }
        }
    });

    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 2);
    pool.setReconnectEnabled(false);
    std::atomic<bool> deliveredAsRequest{false};
    std::atomic<int> offeredOutside{0};
    pool.setRequestCallback(
        [&](ert::diametercomm::DiameterClient*, std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });
    pool.setUnmatchedAnswerCallback([&](const Peer::Buffer&) {
        offeredOutside++;
        return false;
    });
    pool.connectAll("127.0.0.1", 15906);
    runFor(std::chrono::milliseconds(600));
    ASSERT_EQ(serverPeers.size(), 2u);

    std::atomic<int> answers{0};
    std::atomic<int> timeouts{0};
    auto* c = pool.pickReady();
    ASSERT_NE(c, nullptr);
    EXPECT_NE(
        c->send(appRequest(), [&](const Peer::Buffer&) { answers++; }, 1000, {}, [&](uint32_t) { timeouts++; }), 0u);
    runFor(std::chrono::milliseconds(400));

    EXPECT_EQ(answers.load(), 1);
    EXPECT_EQ(timeouts.load(), 0);
    EXPECT_EQ(offeredOutside.load(), 0);  // resolved inside the pool
    EXPECT_FALSE(deliveredAsRequest);

    pool.disconnectAll(0);
    runFor(std::chrono::milliseconds(200));
    server.close();
}

// Nobody in the pool owns the answer: it is offered to the outside callback
// (the gateway's server leg) and never delivered as a request.
TEST_F(DiameterClientPool_test, UnknownAnswerOfferedOutsideNotDeliveredAsRequest) {
    DiameterServer server(io_, serverConfig());
    server.listen("127.0.0.1", 15907);
    server.setPeerEventCallback([&](std::shared_ptr<Peer> p, Peer::State s) {
        if (s == Peer::State::Open) {
            auto a = answerFor(appRequest());
            a[15] = 0x42;  // hbh nobody sent
            a[19] = 0x43;
            p->send(a);
        }
    });

    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 1);
    pool.setReconnectEnabled(false);
    std::atomic<bool> deliveredAsRequest{false};
    std::atomic<int> offeredOutside{0};
    pool.setRequestCallback(
        [&](ert::diametercomm::DiameterClient*, std::shared_ptr<Peer>, Peer::Buffer&&) { deliveredAsRequest = true; });
    pool.setUnmatchedAnswerCallback([&](const Peer::Buffer&) {
        offeredOutside++;
        return false;
    });
    pool.connectAll("127.0.0.1", 15907);
    runFor(std::chrono::milliseconds(600));

    EXPECT_EQ(offeredOutside.load(), 1);
    EXPECT_FALSE(deliveredAsRequest);

    pool.disconnectAll(0);
    runFor(std::chrono::milliseconds(200));
    server.close();
}

// Gateway wiring: an answer that arrives on the SERVER leg for a request sent
// on the client pool is resolved via pool.tryResolve (the NetLoc STA case).
TEST_F(DiameterClientPool_test, ServerLegAnswerResolvedByPoolTryResolve) {
    DiameterServer sut(io_, serverConfig());
    sut.listen("127.0.0.1", 15908);
    DiameterServer gwServer(io_, clientConfig());
    gwServer.listen("127.0.0.1", 15909);
    auto sutToGw = std::make_shared<Peer>(io_, serverConfig());
    sut.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&& msg) { sutToGw->send(answerFor(msg)); });

    DiameterClientPool pool(io_, clientConfig(), Transport::TCP, 1);
    pool.setReconnectEnabled(false);
    std::atomic<bool> gwServerGotRequest{false};
    gwServer.setRequestCallback([&](std::shared_ptr<Peer>, Peer::Buffer&&) { gwServerGotRequest = true; });
    gwServer.setUnmatchedAnswerCallback(
        [&](std::shared_ptr<Peer>, const Peer::Buffer& a) { return pool.tryResolve(a); });

    sutToGw->connect("127.0.0.1", 15909);
    pool.connectAll("127.0.0.1", 15908);
    runFor(std::chrono::milliseconds(600));

    std::atomic<int> answers{0};
    auto* c = pool.pickReady();
    ASSERT_NE(c, nullptr);
    c->send(appRequest(), [&](const Peer::Buffer&) { answers++; }, 1000);
    runFor(std::chrono::milliseconds(400));

    EXPECT_EQ(answers.load(), 1);
    EXPECT_FALSE(gwServerGotRequest);
    EXPECT_FALSE(pool.tryResolve(answerFor(appRequest())));  // nothing pending any more

    pool.disconnectAll(0);
    sutToGw->close();
    runFor(std::chrono::milliseconds(200));
    gwServer.close();
    sut.close();
}
