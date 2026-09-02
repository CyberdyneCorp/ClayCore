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
#include "clay/parallel/work_class.h"

using clay::parallel::ThreadPool;
using clay::parallel::WorkClass;

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

// -- work classes -------------------------------------------------------------
//
// What is testable OFF Apple is the propagation, and it is the half that can
// actually be wrong. The platform call is one line with no branches; the rules
// about which class a given piece of code runs under have four of them, and a
// worker adopting a class and forgetting to put it back is invisible until some
// unrelated job runs at the wrong priority hours later.
//
// `current_work_class()` is readable everywhere for exactly this reason.

namespace {

// The distinct classes observed by the bodies of a dispatch, and how many
// times a body ran at all.
struct ClassTally {
    std::mutex mutex;
    std::vector<WorkClass> seen;
    std::atomic<int> bodies{0};

    void observe() {
        const WorkClass c = clay::parallel::current_work_class();
        ++bodies;
        std::lock_guard<std::mutex> lock(mutex);
        for (WorkClass s : seen)
            if (s == c) return;
        seen.push_back(c);
    }
    bool only(WorkClass c) const { return seen.size() == 1 && seen.front() == c; }
};

}  // namespace

TEST_CASE("an unclassified dispatch is UserInitiated") {
    // The migration rule. Every call site that existed before work classes did
    // keeps compiling and keeps meaning what it meant: somebody is waiting on
    // this. Porting a call site must not silently promote it to Interactive nor
    // demote it to Background.
    ClassTally tally;
    clay::parallel::for_range(4096, 1, [&](std::size_t b, std::size_t e) {
        (void)b;
        (void)e;
        tally.observe();
    });
    CHECK(tally.bodies.load() > 0);
    CHECK(tally.only(WorkClass::UserInitiated));
}

TEST_CASE("a declared class reaches every body that runs the job") {
    // Including the issuing thread's own participation: it runs chunks through
    // the same path as the workers, so it has to be scheduled the same way.
    for (WorkClass cls : {WorkClass::Interactive, WorkClass::UserInitiated,
                          WorkClass::Utility, WorkClass::Background}) {
        CAPTURE(static_cast<int>(cls));
        ClassTally tally;
        clay::parallel::for_range(1 << 16, 1,
                                  [&](std::size_t b, std::size_t e) {
                                      (void)b;
                                      (void)e;
                                      tally.observe();
                                  },
                                  cls);
        CHECK(tally.bodies.load() > 0);
        CHECK(tally.only(cls));
    }
}

TEST_CASE("a small batch that stays on the calling thread still takes its class") {
    // The serial fallback is a different branch from the pooled one, and a
    // class that applied to pooled work only would make scheduling depend on
    // the batch size — the same call fast on one input and starved on another.
    ClassTally tally;
    clay::parallel::for_range(1, 1,
                              [&](std::size_t b, std::size_t e) {
                                  (void)b;
                                  (void)e;
                                  tally.observe();
                              },
                              WorkClass::Interactive);
    CHECK(tally.bodies.load() == 1);
    CHECK(tally.only(WorkClass::Interactive));
}

TEST_CASE("a nested dispatch inherits its caller rather than its argument") {
    // A nested call runs INLINE, on a thread that is already inside somebody
    // else's job. Applying the nested class there would re-schedule the OUTER
    // work: a Utility helper called from an Interactive dab would drag the dab
    // down for as long as the helper ran. The argument is honoured for
    // top-level calls and deliberately ignored here.
    ClassTally outer, inner;
    clay::parallel::for_range(1 << 16, 1,
                              [&](std::size_t b, std::size_t e) {
                                  (void)b;
                                  (void)e;
                                  outer.observe();
                                  clay::parallel::for_range(64, 1,
                                                            [&](std::size_t, std::size_t) {
                                                                inner.observe();
                                                            },
                                                            WorkClass::Background);
                              },
                              WorkClass::Interactive);
    CHECK(outer.only(WorkClass::Interactive));
    CHECK(inner.bodies.load() > 0);
    CHECK(inner.only(WorkClass::Interactive));
}

TEST_CASE("a class does not outlive the job that asked for it") {
    // THE LEAK THIS EXISTS TO CATCH. The workers are persistent: one that
    // adopts Background and does not put it back keeps running everything
    // after it as Background, and nothing about the next job looks wrong.
    // Checked on the issuing thread, whose class is observable directly, and
    // then by dispatching again and seeing the default come back.
    const WorkClass before = clay::parallel::current_work_class();
    clay::parallel::for_range(1 << 16, 1, [](std::size_t, std::size_t) {},
                              WorkClass::Background);
    CHECK(clay::parallel::current_work_class() == before);

    ClassTally after;
    clay::parallel::for_range(1 << 16, 1, [&](std::size_t, std::size_t) { after.observe(); });
    CHECK(after.only(WorkClass::UserInitiated));
}

TEST_CASE("classifying a dispatch does not change what it computes") {
    // The pool's contract is that it changes speed and nothing else, and a
    // scheduling hint is exactly the kind of change that is supposed to be
    // invisible to results. Run the ragged case under every class.
    for (WorkClass cls : {WorkClass::Interactive, WorkClass::UserInitiated,
                          WorkClass::Utility, WorkClass::Background}) {
        CAPTURE(static_cast<int>(cls));
        const std::size_t n = 100003;  // prime: the last chunk is always ragged
        Tally tally(n);
        clay::parallel::for_range(n, 1,
                                  [&](std::size_t b, std::size_t e) {
                                      for (std::size_t i = b; i < e; ++i)
                                          tally.visits[i].fetch_add(1);
                                  },
                                  cls);
        CHECK(tally.each_exactly_once());
    }
}
