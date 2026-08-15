// The CPU batch pool (evaluation-backends spec, add-mobile-thread-scheduling).
//
// The pool "changes speed and nothing else", which is the whole of its
// contract, so what these test is that the two hardening changes did not buy
// speed with correctness:
//
//   - every element of a range is computed EXACTLY once, including the ragged
//     and single-chunk cases, which is the requirement the join is allowed to
//     be rewritten under;
//   - a nested parallel_for runs INLINE rather than evicting the job the
//     calling thread is already inside.
//
// The nesting test is the one worth reading. Before the guard it did not
// deadlock or corrupt anything — it silently ran the outer loop on one thread,
// which is indistinguishable from parallelism that did not help. It is checked
// by observing that a nested call reports no pool concurrency of its own.

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include "clay/parallel/thread_pool.h"

using clay::parallel::ThreadPool;

namespace {

// How many distinct threads ran a range, and how many elements each index got.
struct Tally {
    std::vector<std::atomic<int>> visits;
    std::atomic<int> chunks{0};
    explicit Tally(std::size_t n) : visits(n) {
        for (auto& v : visits) v.store(0);
    }
    bool each_exactly_once() const {
        for (const auto& v : visits)
            if (v.load() != 1) return false;
        return true;
    }
};

}  // namespace

TEST_CASE("every element of a range is computed exactly once") {
    // The ragged cases matter more than the round ones: n that does not divide
    // by the chunk is where an off-by-one in the join or the claim shows up.
    for (std::size_t n : {std::size_t(1), std::size_t(2), std::size_t(7), std::size_t(64),
                          std::size_t(1000), std::size_t(4097)}) {
        for (std::size_t min_chunk : {std::size_t(1), std::size_t(3), std::size_t(4096)}) {
            Tally tally(n);
            ThreadPool::instance().parallel_for(n, min_chunk, [&](std::size_t b, std::size_t e) {
                tally.chunks.fetch_add(1);
                for (std::size_t i = b; i < e; ++i) tally.visits[i].fetch_add(1);
            });
            CAPTURE(n);
            CAPTURE(min_chunk);
            CHECK(tally.each_exactly_once());
        }
    }
}

TEST_CASE("an empty range runs nothing and returns") {
    std::atomic<int> calls{0};
    ThreadPool::instance().parallel_for(0, 1, [&](std::size_t, std::size_t) { calls.fetch_add(1); });
    CHECK(calls.load() == 0);
}

TEST_CASE("a nested parallel_for runs inline instead of evicting the outer job") {
    // The trap this guards: one `current_` slot, so a nested submission would
    // replace the job the calling worker is inside. The outer job stops being
    // advertised and its remaining chunks fall to whoever is already in it.
    //
    // Observed here as the property that matters rather than through the pool's
    // internals: the OUTER loop still covers every element exactly once even
    // though every one of its chunks issues a nested loop of its own.
    constexpr std::size_t kOuter = 512, kInner = 64;
    Tally outer(kOuter);
    std::vector<std::atomic<int>> inner(kOuter * kInner);
    for (auto& v : inner) v.store(0);

    ThreadPool::instance().parallel_for(kOuter, 1, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            outer.visits[i].fetch_add(1);
            // A nested batch, exactly as parallel brick meshing whose per-brick
            // work evaluates through the pool would issue.
            ThreadPool::instance().parallel_for(kInner, 1, [&](std::size_t ib, std::size_t ie) {
                for (std::size_t j = ib; j < ie; ++j) inner[i * kInner + j].fetch_add(1);
            });
        }
    });

    CHECK(outer.each_exactly_once());
    bool inner_once = true;
    for (const auto& v : inner)
        if (v.load() != 1) inner_once = false;
    CHECK(inner_once);
}

TEST_CASE("a nested call runs on the thread that issued it") {
    // The inline path is not merely correct, it is inline: the nested range is
    // handled by one thread, which is what keeps it from taking the workers the
    // outer job is still using.
    std::atomic<int> outer_threads{0};
    ThreadPool::instance().parallel_for(256, 1, [&](std::size_t, std::size_t) {
        outer_threads.fetch_add(1);
        std::vector<std::thread::id> seen;
        std::mutex m;
        ThreadPool::instance().parallel_for(4096, 1, [&](std::size_t, std::size_t) {
            std::lock_guard<std::mutex> lock(m);
            seen.push_back(std::this_thread::get_id());
        });
        // One chunk, one thread, and it is this one.
        CHECK(seen.size() == 1);
        CHECK(seen.front() == std::this_thread::get_id());
    });
    CHECK(outer_threads.load() > 0);
}

TEST_CASE("the pool does not lose a wakeup under repeated small dispatches") {
    // The join went from a yield-spin to a condition variable, so the failure
    // mode being ruled out is a hang rather than a wrong answer: a notify that
    // lands between the predicate check and the wait. Small ranges make the
    // race window as wide as it gets, because the issuing thread finishes its
    // own chunks almost immediately and reaches the wait while workers are
    // still starting.
    for (int round = 0; round < 2000; ++round) {
        std::atomic<std::size_t> sum{0};
        ThreadPool::instance().parallel_for(64, 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) sum.fetch_add(i, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == 64 * 63 / 2);
    }
}

TEST_CASE("results do not depend on how the work was split") {
    // "The pool changes speed and nothing else." Same range, same arithmetic,
    // every chunking the min_chunk knob can produce.
    const std::size_t n = 3001;
    std::vector<double> reference(n);
    for (std::size_t i = 0; i < n; ++i) reference[i] = static_cast<double>(i) * 1.5;

    for (std::size_t min_chunk : {std::size_t(1), std::size_t(7), std::size_t(512), n * 2}) {
        std::vector<double> out(n, 0.0);
        ThreadPool::instance().parallel_for(n, min_chunk, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) out[i] = static_cast<double>(i) * 1.5;
        });
        CAPTURE(min_chunk);
        CHECK(out == reference);
    }
}
