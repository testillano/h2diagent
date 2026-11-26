/*
C++ HTTP/2 - DIAMETER Gateway Service (translation agent)
Unit tests for IoContextPool (the --diameter-io-threads contexts).

Properties verified:
  - size is clamped to >= 1; next() cycles over every context;
  - start() runs each context on its OWN thread (distinct from the caller and
    from the other contexts), and a context keeps running with no work queued;
  - stop() joins the threads, is idempotent, and is safe without start();
  - the destructor stops a started pool (no std::terminate on a joinable thread).
*/

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ert/h2diagent/IoContextPool.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using ert::h2diagent::IoContextPool;

namespace {
// Run 'fn' on 'io' and wait (bounded) for it: true when it ran. The state is
// shared so a handler that runs after a timeout touches nothing destroyed
// ('fn' must then outlive it too: the tests only pass captures of the test body).
template <class Fn>
bool runOn(boost::asio::io_context& io, Fn fn, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
    };
    auto state = std::make_shared<State>();
    boost::asio::post(io, [state, fn]() {
        fn();
        std::lock_guard<std::mutex> lock(state->m);
        state->done = true;
        state->cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(state->m);
    return state->cv.wait_for(lock, timeout, [&]() { return state->done; });
}
}  // namespace

TEST(IoContextPool, SizeClampedToOne) {
    IoContextPool pool(0);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool.contexts().size(), 1u);
}

TEST(IoContextPool, NextCyclesOverEveryContext) {
    IoContextPool pool(3);
    std::set<boost::asio::io_context*> seen;
    for (int i = 0; i < 6; ++i) seen.insert(&pool.next());
    EXPECT_EQ(seen.size(), 3u);
    // contexts() lists the same ones, in at() order
    const auto list = pool.contexts();
    ASSERT_EQ(list.size(), 3u);
    for (std::size_t i = 0; i < list.size(); ++i) {
        EXPECT_EQ(list[i], &pool.at(i));
        EXPECT_EQ(seen.count(list[i]), 1u);
    }
}

TEST(IoContextPool, AtOutOfRangeThrows) {
    IoContextPool pool(2);
    EXPECT_THROW(pool.at(2), std::out_of_range);
}

TEST(IoContextPool, EachContextRunsOnItsOwnThread) {
    IoContextPool pool(3);
    pool.start();
    std::set<std::thread::id> threads;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        std::thread::id id;
        ASSERT_TRUE(runOn(pool.at(i), [&]() { id = std::this_thread::get_id(); }));
        EXPECT_NE(id, std::this_thread::get_id());  // not the caller
        threads.insert(id);
    }
    EXPECT_EQ(threads.size(), 3u);  // one thread per context
    pool.stop();
}

TEST(IoContextPool, KeepsRunningWithoutWork) {
    // The work guard keeps run() alive with nothing queued: a handler posted
    // later still runs.
    IoContextPool pool(1);
    pool.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(runOn(pool.at(0), []() {}));
    pool.stop();
}

TEST(IoContextPool, StartTwiceDoesNotAddThreads) {
    IoContextPool pool(1);
    pool.start();
    std::thread::id first;
    ASSERT_TRUE(runOn(pool.at(0), [&]() { first = std::this_thread::get_id(); }));
    pool.start();  // already started: no-op
    std::thread::id second;
    ASSERT_TRUE(runOn(pool.at(0), [&]() { second = std::this_thread::get_id(); }));
    EXPECT_EQ(first, second);
    pool.stop();
}

TEST(IoContextPool, StopJoinsAndIsIdempotent) {
    IoContextPool pool(2);
    pool.start();
    ASSERT_TRUE(runOn(pool.at(0), []() {}));
    pool.stop();
    pool.stop();  // second stop: nothing to join
    // stopped: a handler posted now does not run
    std::atomic<bool> ran{false};
    boost::asio::post(pool.at(1), [&]() { ran = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(ran);
}

TEST(IoContextPool, StopWithoutStartIsSafe) {
    IoContextPool pool(2);
    pool.stop();
    SUCCEED();
}

TEST(IoContextPool, DestructorStopsAStartedPool) {
    std::atomic<int> ran{0};
    {
        IoContextPool pool(2);
        pool.start();
        ASSERT_TRUE(runOn(pool.at(0), [&]() { ran++; }));
        ASSERT_TRUE(runOn(pool.at(1), [&]() { ran++; }));
    }  // joinable threads would std::terminate here if not stopped
    EXPECT_EQ(ran.load(), 2);
}
