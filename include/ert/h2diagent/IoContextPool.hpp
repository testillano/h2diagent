/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
https://github.com/testillano/h2diagent
Licensed under the MIT License. Copyright (c) 2024 Eduardo Ramos
*/

/**
 * @file IoContextPool.hpp
 * @brief N single-threaded io_contexts, one thread each (--diameter-io-threads).
 *
 * Why: one io_context run by many threads does not scale with Diameter traffic.
 * Only one thread at a time waits in its epoll reactor, and every handler of
 * every connection (two reads per message, writes, request timers) goes through
 * its single scheduler mutex. Under load the workers spend their time parked on
 * that mutex (the gateway stalls at a few cores whatever the machine size), the
 * sockets are read late and the latency grows. Each io_context here has its own
 * reactor and scheduler, and the Diameter client connections are spread over
 * them (DiameterClientPool), so the connections no longer contend with each
 * other: a connection and its timers live on one thread.
 *
 * Threading: next()/at()/size() are safe to call concurrently. start() and
 * stop() are meant for setup and teardown. stop() is idempotent; destroy the
 * objects using the contexts (sockets, timers) BEFORE the pool, after stop().
 */

#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <cstddef>
#include <ert/h2diagent/helpers.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace ert {
namespace h2diagent {

class IoContextPool {
   public:
    /** 'size' io_contexts (clamped to >= 1); no thread runs until start(). */
    explicit IoContextPool(std::size_t size) {
        if (size < 1) size = 1;
        units_.reserve(size);
        for (std::size_t i = 0; i < size; ++i) units_.push_back(std::make_unique<Unit>());
    }

    ~IoContextPool() { stop(); }

    IoContextPool(const IoContextPool&) = delete;
    IoContextPool& operator=(const IoContextPool&) = delete;

    std::size_t size() const { return units_.size(); }

    /** io_context at a position (0..size()-1). */
    boost::asio::io_context& at(std::size_t index) { return units_.at(index)->io; }

    /** Next io_context, round-robin. */
    boost::asio::io_context& next() { return units_[helpers::roundRobinIndex(cursor_, units_.size())]->io; }

    /** Every io_context, in order (e.g. to build a DiameterClientPool). */
    std::vector<boost::asio::io_context*> contexts() {
        std::vector<boost::asio::io_context*> list;
        list.reserve(units_.size());
        for (auto& u : units_) list.push_back(&u->io);
        return list;
    }

    /** One thread per io_context, kept alive by a work guard until stop(). */
    void start() {
        for (auto& u : units_) {
            if (u->thread.joinable()) continue;  // already started
            u->work = std::make_unique<WorkGuard>(u->io.get_executor());
            Unit* raw = u.get();
            u->thread = std::thread([raw]() { raw->io.run(); });
        }
    }

    /** Stop every io_context and join its thread (pending handlers are not run). */
    void stop() {
        for (auto& u : units_) {
            u->work.reset();
            u->io.stop();
        }
        for (auto& u : units_) {
            if (u->thread.joinable()) u->thread.join();
        }
    }

   private:
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    struct Unit {
        boost::asio::io_context io{1};  // concurrency hint: a single thread runs it
        std::unique_ptr<WorkGuard> work;
        std::thread thread;
    };
    std::vector<std::unique_ptr<Unit>> units_;
    std::atomic<std::size_t> cursor_{0};
};

}  // namespace h2diagent
}  // namespace ert
