/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file DiameterClientPool.hpp
 * @brief Pool of N outbound Diameter client connections to the same peer.
 *
 * A thin wrapper over N independent diametercomm::DiameterClient objects, all
 * connected to the SAME peer (same origin-host). Outbound requests are striped
 * round-robin across the pool; each answer/RAR returns on the connection that
 * carries it, so per-transaction correlation stays PER-CONNECTION and is fully
 * reused from DiameterClient (its own pending_/pendingMutex_). The pool only
 * chooses WHICH client sends.
 *
 * Rationale (see the connection-pool proposal):
 * - N == 1 is byte-for-byte today's single-connection behaviour (zero risk).
 * - N  > 1 adds robustness (a dropped connection leaves N-1 serving) and
 *   headroom for a faster SUT. TCP only (SCTP + N>1 is rejected at CLI level).
 *
 * Threading: pick() is safe to call concurrently (round-robin cursor is a
 * relaxed atomic). Configuration setters and connectAll()/disconnectAll() are
 * meant to be called during setup/teardown, not concurrently with traffic.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ert/diametercomm/DiameterClient.hpp>
#include <ert/h2diagent/helpers.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ert {
namespace h2diagent {

/**
 * Pool of N Diameter client connections to a single peer.
 */
class DiameterClientPool {
   public:
    using Buffer = diametercomm::Peer::Buffer;
    // Request callback carries the ORIGINATING client so the caller can route
    // the answer (e.g. an inbound RAR's RAA) back on the SAME connection.
    using PoolRequestCallback =
        std::function<void(diametercomm::DiameterClient*, std::shared_ptr<diametercomm::Peer>, Buffer&&)>;

    /**
     * Build a pool of 'connections' clients, all sharing the given peer config
     * and transport. 'connections' must be >= 1 (validated at CLI level).
     */
    DiameterClientPool(boost::asio::io_context& io, const diametercomm::Peer::Config& config,
                       diametercomm::Transport transport, std::size_t connections)
        : io_(io) {
        if (connections < 1) connections = 1;
        clients_.reserve(connections);
        for (std::size_t i = 0; i < connections; ++i) {
            clients_.push_back(std::make_unique<diametercomm::DiameterClient>(io_, config, transport));
        }
    }

    // Non-copyable
    DiameterClientPool(const DiameterClientPool&) = delete;
    DiameterClientPool& operator=(const DiameterClientPool&) = delete;

    /** Number of connections in the pool. */
    std::size_t size() const { return clients_.size(); }

    /** Enable metrics on every pooled connection (same source label). */
    void enableMetrics(ert::metrics::Metrics* metrics, const std::string& source) {
        for (auto& c : clients_) c->enableMetrics(metrics, source);
    }

    /**
     * Wire the request callback on every connection. The pool augments the
     * per-client callback with the originating DiameterClient pointer so the
     * caller can answer back on the exact connection that received the request.
     */
    void setRequestCallback(PoolRequestCallback cb) {
        requestCb_ = std::move(cb);
        for (auto& c : clients_) {
            diametercomm::DiameterClient* raw = c.get();
            c->setRequestCallback([this, raw](std::shared_ptr<diametercomm::Peer> peer, Buffer&& msg) {
                if (requestCb_) requestCb_(raw, std::move(peer), std::move(msg));
            });
        }
    }

    /** Wire the timeout callback on every connection. */
    void setTimeoutCallback(diametercomm::DiameterClient::TimeoutCallback cb) {
        for (auto& c : clients_) c->setTimeoutCallback(cb);
    }

    /** Configure reconnection on every connection (each reconnects independently). */
    void setReconnectEnabled(bool enabled) {
        for (auto& c : clients_) c->setReconnectEnabled(enabled);
    }
    void setReconnectBackoff(std::chrono::milliseconds initial, std::chrono::milliseconds max) {
        for (auto& c : clients_) c->setReconnectBackoff(initial, max);
    }

    /** Connect every pooled client to the peer (independent CER/CEA each). */
    void connectAll(const std::string& host, uint16_t port) {
        for (auto& c : clients_) c->connect(host, port);
    }

    /** Graceful disconnect (DPR/DPA) on every connection. */
    void disconnectAll(uint32_t cause = 0) {
        for (auto& c : clients_) c->disconnect(cause);
    }

    /**
     * Select the next connection for an outbound request (round-robin).
     * Returns nullptr only for an empty pool (never happens: size >= 1).
     */
    diametercomm::DiameterClient* pick() {
        if (clients_.empty()) return nullptr;
        std::size_t idx = helpers::roundRobinIndex(cursor_, clients_.size());
        return clients_[idx].get();
    }

    /**
     * Like pick(), but prefers a READY (Open) connection: starts at the
     * round-robin position and returns the first ready client scanning forward,
     * so a request is not dropped just because the round-robin cursor landed on
     * a connection that is momentarily reconnecting (graceful degradation).
     * Returns nullptr only if NO connection is ready.
     */
    diametercomm::DiameterClient* pickReady() {
        if (clients_.empty()) return nullptr;
        std::size_t n = clients_.size();
        std::size_t start = helpers::roundRobinIndex(cursor_, n);
        for (std::size_t i = 0; i < n; ++i) {
            diametercomm::DiameterClient* c = clients_[(start + i) % n].get();
            if (c->isReady()) return c;
        }
        return nullptr;
    }

    /**
     * True if at least one pooled connection is ready (Open). The gateway can
     * still send while some connections reconnect: pick() may return a
     * not-yet-ready client, whose send() will fail cleanly and increment the
     * unsent metric, but overall the pool is "usable" if any is ready.
     */
    bool anyReady() const {
        for (const auto& c : clients_) {
            if (c->isReady()) return true;
        }
        return false;
    }

   private:
    boost::asio::io_context& io_;
    std::vector<std::unique_ptr<diametercomm::DiameterClient>> clients_;
    std::atomic<std::size_t> cursor_{0};
    PoolRequestCallback requestCb_;
};

}  // namespace h2diagent
}  // namespace ert
