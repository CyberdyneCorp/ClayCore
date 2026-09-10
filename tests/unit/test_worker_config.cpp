#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "clay.h"
#include "clay/parallel/thread_pool.h"
#include "clay/parallel/work_class.h"

// The worker pool a host can finally see (add-mobile-thread-scheduling, tasks
// 1.5, 1.7, 1.8, 1.10; issue #243).
//
// The library's contract says "the caller owns threading and queues" and the
// CPU backend did not keep it: a process-wide pool spawned a worker per core
// the first time anything evaluated, and a host that had sized its own pools
// could not see it, size it, or stop it competing.
//
// WHY THE SIZING TESTS DO NOT USE THE SINGLETON. `ThreadPool::instance()` is
// built once per process and its count freezes then, so a test that configured
// it would either be the first test to run or be testing nothing -- and which
// of those it is would depend on the shard order. Every case here that needs a
// particular worker count builds its OWN pool, which is what the explicit
// constructor is for. The ABI-level cases assert the refusals, which do not
// depend on the pool's state.

namespace {

// A deterministic per-index value, so a range run in any order by any number of
// threads produces the same array. Cheap but not free: a sum that the compiler
// could hoist would test the dispatch rather than the work.
std::uint64_t mix(std::size_t i) {
    std::uint64_t x = static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return x;
}

std::vector<std::uint64_t> run_over(std::size_t workers, std::size_t n, std::size_t min_chunk) {
    std::vector<std::uint64_t> out(n, 0);
    clay::parallel::ThreadPool pool(workers);
    pool.parallel_for(n, min_chunk, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) out[i] = mix(i);
    });
    return out;
}

}  // namespace

TEST_CASE("worker count zero gives results identical to the threaded path") {
    // Task 1.8. The golden corpus for a dispatch is the range itself: every
    // index written exactly once, with the same value, however many threads
    // touched it.
    //
    // Sizes chosen to straddle the decomposition: below min_chunk (runs inline
    // whatever the pool holds), around one chunk per worker, and far enough
    // above that the claim counter rebalances.
    for (const std::size_t n : {std::size_t(1), std::size_t(63), std::size_t(1024),
                                std::size_t(100000)}) {
        CAPTURE(n);
        const std::vector<std::uint64_t> serial = run_over(0, n, 1);
        REQUIRE(serial.size() == n);

        // Not just "some threads": several counts, because a bug that only
        // appears when the range does not divide evenly by the worker count
        // would hide behind a single well-chosen number.
        for (const std::size_t workers : {std::size_t(1), std::size_t(2), std::size_t(3),
                                          std::size_t(7)}) {
            CAPTURE(workers);
            const std::vector<std::uint64_t> threaded = run_over(workers, n, 1);
            REQUIRE(threaded.size() == n);
            CHECK(threaded == serial);
        }
    }

    // And the values are the ones the function actually produces, not merely
    // equal to each other -- two paths that both wrote zeroes would pass the
    // comparison above and mean nothing.
    const std::vector<std::uint64_t> serial = run_over(0, 257, 1);
    for (std::size_t i = 0; i < serial.size(); ++i) {
        CAPTURE(i);
        REQUIRE(serial[i] == mix(i));
    }
}

TEST_CASE("zero workers starts no threads and runs on the calling thread") {
    // The contract, not merely the result: a host that configures 0 is saying
    // "do not start threads", so the work must happen HERE. Asserted by
    // identity of the running thread rather than by counting threads, which is
    // what a caller can actually observe.
    clay::parallel::ThreadPool pool(0);
    CHECK(pool.worker_count() == 0);

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> ran_elsewhere{false};
    std::atomic<std::size_t> covered{0};
    pool.parallel_for(10000, 1, [&](std::size_t begin, std::size_t end) {
        if (std::this_thread::get_id() != caller) ran_elsewhere.store(true);
        covered.fetch_add(end - begin);
    });
    CHECK(covered.load() == 10000);
    CHECK_FALSE(ran_elsewhere.load());
}

TEST_CASE("a pool with workers really does use them") {
    // The control for the case above. Without it, "ran on the calling thread"
    // proves nothing -- a pool that never dispatches anything would pass it
    // too, and the serial-equals-threaded test would then be comparing serial
    // against serial.
    //
    // THE WORK HAS TO BE WORTH STEALING. The first version of this case used a
    // trivial body over a million indices and FAILED: the calling thread claims
    // chunks in the same loop the workers do, and with a body that costs
    // nothing it drained all 32 of them before a worker finished waking. That
    // is the dispatch behaving correctly -- a wakeup is not free and work that
    // is cheaper than one should not pay for it -- so the fixture is what was
    // wrong. Each chunk now costs enough that a worker can get there.
    clay::parallel::ThreadPool pool(3);
    REQUIRE(pool.worker_count() == 3);

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> ran_elsewhere{false};
    std::atomic<std::size_t> covered{0};
    std::vector<std::uint64_t> sink(64, 0);
    pool.parallel_for(64, 1, [&](std::size_t begin, std::size_t end) {
        if (std::this_thread::get_id() != caller) ran_elsewhere.store(true);
        for (std::size_t i = begin; i < end; ++i) {
            std::uint64_t acc = mix(i);
            for (int spin = 0; spin < 200000; ++spin) acc = mix(acc ^ spin);
            sink[i] = acc;  // kept, so the loop cannot be optimised away
        }
        covered.fetch_add(end - begin);
    });
    CHECK(covered.load() == 64);
    CHECK(ran_elsewhere.load());
}

TEST_CASE("the pool is sized from performance cores where the platform has them") {
    // Task 1.5. What is asserted is the RELATIONSHIP, not a number: the count
    // is machine-specific and a literal here would fail on the next box.
    const std::size_t perf = clay::parallel::platform_performance_cores();
    const std::size_t logical = std::thread::hardware_concurrency();
    const std::size_t planned = clay::parallel::ThreadPool::default_worker_count();

    if (perf == 0) {
        // No platform opinion: every core is a performance core, and the count
        // is what it was before this change existed.
        CHECK(planned == (logical > 1 ? logical - 1 : 0));
    } else {
        // One fewer than the fast cores, because the calling thread is a worker.
        CHECK(planned == (perf > 1 ? perf - 1 : 0));
        // And never more than the machine has, which is the oversubscription
        // this task exists to stop.
        CHECK(planned < logical);
    }
}

TEST_CASE("c abi: an out-of-range worker count is refused, not clamped") {
    // Task 1.7. A clamp would let a host believe a limit that was never
    // applied, which is the failure the entry point exists to end.
    clay_worker_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.workers = CLAY_MAX_WORKERS + 1u;
    CHECK(clay_configure_workers(&cfg) == CLAY_ERROR_INVALID_ARGUMENT);

    cfg.workers = 0xFFFFFFFFu;
    CHECK(clay_configure_workers(&cfg) == CLAY_ERROR_INVALID_ARGUMENT);

    CHECK(clay_configure_workers(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // A descriptor that does not declare its size is refused like every other
    // one in this ABI.
    clay_worker_config bad;
    std::memset(&bad, 0, sizeof bad);
    bad.struct_size = 0;
    CHECK(clay_configure_workers(&bad) != CLAY_OK);
}

TEST_CASE("c abi: the worker report answers without freezing what it reports") {
    // A query that froze the count would change its own answer by being asked,
    // so this must be callable repeatedly with the same result.
    clay_worker_report a;
    std::memset(&a, 0, sizeof a);
    a.struct_size = sizeof a;
    REQUIRE(clay_worker_report_get(&a) == CLAY_OK);

    clay_worker_report b;
    std::memset(&b, 0, sizeof b);
    b.struct_size = sizeof b;
    REQUIRE(clay_worker_report_get(&b) == CLAY_OK);

    CHECK(a.workers == b.workers);
    CHECK(a.frozen == b.frozen);

    // The machine's own numbers, related the way the sizing rule says.
    CHECK(a.logical_cores >= 1);
    if (a.performance_cores > 0) {
        CHECK(a.performance_cores <= a.logical_cores);
        CHECK(a.workers == (a.performance_cores > 1 ? a.performance_cores - 1 : 0));
    }

    CHECK(clay_worker_report_get(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("shutdown with work in flight, and workers that arrive late") {
    // Task 1.10. The shared_ptr job state exists so a worker still inside a job
    // cannot be reading a destroyed pool, and the join is what that has to hold
    // against. Built and destroyed repeatedly rather than once, because a
    // shutdown race that fires one time in fifty is not a shutdown race that
    // fires never.
    for (int round = 0; round < 50; ++round) {
        CAPTURE(round);
        std::atomic<std::size_t> covered{0};
        {
            clay::parallel::ThreadPool pool(4);
            // Dispatch, then let the pool go out of scope immediately after the
            // call returns -- the destructor runs while the workers are still
            // being woken down from the job they just finished.
            pool.parallel_for(20000, 1, [&](std::size_t begin, std::size_t end) {
                covered.fetch_add(end - begin, std::memory_order_relaxed);
            });
        }
        REQUIRE(covered.load() == 20000);
    }
}

TEST_CASE("a pool destroyed without ever dispatching still joins its workers") {
    // The late-worker path from the other side: threads are started in the
    // constructor and may not have reached the wait when the destructor takes
    // the lock. Nothing to observe but the absence of a hang or a crash, which
    // is what this asserts by completing.
    for (int round = 0; round < 100; ++round) {
        CAPTURE(round);
        clay::parallel::ThreadPool pool(4);
        CHECK(pool.worker_count() == 4);
    }
}
