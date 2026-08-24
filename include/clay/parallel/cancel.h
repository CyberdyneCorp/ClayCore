#pragma once

// A COOPERATIVE CANCELLATION TOKEN, and why it lives here.
//
// This library has a formal name for an operation that takes seconds:
// `BudgetClass::operation` in the device harness — "an explicit user action:
// consolidate, mask extrude, an export". It budgets for them and gates
// releases on them. On the reference iPad, mask_extrude measures 4403 ms and
// sdf_consolidate 661 ms.
//
// Every one of those was a synchronous call a host entered and could not
// leave. There is a third budget class and there was no exit from it.
//
// WHY THIS MODULE. The token has to be visible to `scene` (consolidate),
// `field` (relax, flatten) and `brush` (mask extrude), and `parallel` is the
// only module all three already depend on — check_layering.py allows `field`
// exactly {parallel, kernel, math}. It also belongs beside the thread pool for
// a second reason: the pool's join rule is the one way to get this
// catastrophically wrong. See below.
//
// WHY A TOKEN AND NOT A CALLBACK. clay.h contains no function pointers at all
// — checked, not assumed. A progress callback would be the first one every FFI
// consumer has to marshal: a C# host pinning a delegate against its GC, a Rust
// host guaranteeing no panic unwinds across the boundary. A callback would
// also fire on a POOL WORKER rather than the caller's thread, so the header
// would need a rule about what it may touch, and the honest answer is "almost
// nothing". A token inverts that: the engine writes, the host reads, both sides
// plain atomics.
//
// THE RULE THE POOL IMPOSES. `parallel_for` joins by waiting for
// `done >= num_tasks`, and `run()` increments `done` only AFTER `fn` returns.
// So a cancelled chunk must RETURN NORMALLY. It must not throw: an exception
// unwinding out of `fn` skips the increment, `done` never reaches `num_tasks`,
// and the issuing thread waits on a condition variable forever. That is a hang,
// not a slow cancel, and it is the single most likely way to get this wrong.
//
// The consequence is that cancelling does not stop chunks being claimed — every
// remaining one still calls `fn`, which returns after one relaxed load. That is
// fine (the cost is `num_tasks` relaxed loads) and it is why the pool itself
// needs no cancellation concept.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace clay {
namespace parallel {

// What an operation is doing, for a host that wants to draw a bar. Written by
// the engine, read by the host, never by a callback.
//
// NO TIME ESTIMATE, deliberately. A multi-phase operation's phases have very
// different per-unit costs — redistancing and a colour fill are not the same
// work per brick — so a remaining-time figure derived from a fraction would be
// wrong in the direction that annoys users most. The host has the wall clock.
struct Progress {
    std::uint32_t phase = 0;        // 0-based; 0 when nothing is running
    std::uint32_t phase_count = 0;  // 0 when nothing is running
    float fraction = 0.0f;          // within the current phase, monotonic
    std::uint64_t done = 0;         // honest units where there are any
    std::uint64_t total = 0;        // 0 when the operation has no honest unit
    bool running = false;
};

class CancelToken {
  public:
    // Cancelling is the ONE thing that is safe to do to a live operation from
    // another thread. Everything else in this library requires the host to
    // serialize its calls, const readers included, which is exactly why a host
    // could not previously drive a progress bar for a running operation.
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }

    // Checked by the engine at bounded intervals of work. Relaxed because a
    // cancel that is observed one chunk late is indistinguishable from one
    // issued a moment later.
    bool cancelled() const { return cancelled_.load(std::memory_order_relaxed); }

    // Reusable. A host holding one token per document and cancelling once must
    // not pay an allocation per cancel on the interactive path, so the state is
    // cleared rather than the token being one-shot.
    void reset() {
        cancelled_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        phase_.store(0, std::memory_order_relaxed);
        phase_count_.store(0, std::memory_order_relaxed);
        fraction_.store(0.0f, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
    }

    // -- the engine's side ---------------------------------------------------

    void begin(std::uint32_t phase_count) {
        phase_count_.store(phase_count, std::memory_order_relaxed);
        phase_.store(0, std::memory_order_relaxed);
        fraction_.store(0.0f, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
    }
    void enter_phase(std::uint32_t phase, std::uint64_t total = 0) {
        phase_.store(phase, std::memory_order_relaxed);
        fraction_.store(0.0f, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        total_.store(total, std::memory_order_relaxed);
    }
    void advance(std::uint64_t done, float fraction) {
        done_.store(done, std::memory_order_relaxed);
        fraction_.store(fraction, std::memory_order_relaxed);
    }
    void end() { running_.store(false, std::memory_order_release); }

    // -- the host's side -----------------------------------------------------

    // Safe from a thread other than the one running the operation, and safe
    // when nothing is running — in which case it reports that rather than a
    // stale fraction from a previous call.
    Progress progress() const {
        Progress p;
        p.running = running_.load(std::memory_order_acquire);
        if (!p.running) return p;
        p.phase = phase_.load(std::memory_order_relaxed);
        p.phase_count = phase_count_.load(std::memory_order_relaxed);
        p.fraction = fraction_.load(std::memory_order_relaxed);
        p.done = done_.load(std::memory_order_relaxed);
        p.total = total_.load(std::memory_order_relaxed);
        return p;
    }

  private:
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> phase_{0};
    std::atomic<std::uint32_t> phase_count_{0};
    std::atomic<float> fraction_{0.0f};
    std::atomic<std::uint64_t> done_{0};
    std::atomic<std::uint64_t> total_{0};
};

// Sugar for the checkpoints, so a call site reads as one condition rather than
// a null check plus a load. A null token is "not cancellable", which is what
// every existing call site passes and what keeps them behaving as before.
inline bool cancelled(const CancelToken* token) {
    return token != nullptr && token->cancelled();
}

// Scopes `begin`/`end` so an early return — which is what a cancel IS — cannot
// leave a token claiming an operation is still running.
class ProgressScope {
  public:
    ProgressScope(CancelToken* token, std::uint32_t phase_count) : token_(token) {
        if (token_) token_->begin(phase_count);
    }
    ~ProgressScope() {
        if (token_) token_->end();
    }
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

    void phase(std::uint32_t index, std::uint64_t total = 0) {
        if (token_) token_->enter_phase(index, total);
    }
    void advance(std::uint64_t done, float fraction) {
        if (token_) token_->advance(done, fraction);
    }
    bool cancelled() const { return parallel::cancelled(token_); }

  private:
    CancelToken* token_;
};

}  // namespace parallel
}  // namespace clay
