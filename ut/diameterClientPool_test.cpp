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
