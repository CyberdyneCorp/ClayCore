#pragma once

// WHAT AN OPERATION WILL COST, ASKED BEFORE IT IS PAID (sculpt-runtime spec,
// add-extreme-poly-runtime).
//
// `MultiresPreflight` is already the right shape — persistent and peak
// separately, a typed refusal, no side effects, no allocation — and it is the
// shape of exactly one operation. Four more need it: converting between
// representations, flattening a layer stack, a global remesh, serializing a
// large surface. Each of those has a transient peak that EXCEEDS its result:
// the parent level's connectivity has to exist while the child is generated,
// a conversion holds both representations at once, a serializer holds the blob
// beside the thing it encodes. The peak, not the steady state, is what kills an
// app on a memory-constrained device, and an engine that discovers this by
// being terminated cannot tell the user what happened.
//
// ONE ESTIMATOR, FIVE CALLERS, and the reason is arithmetic rather than tidiness.
// Five bespoke estimates is five places for `vertices * bytes_per_vertex` to
// wrap and report a SMALL number — and the failure mode of that bug is that the
// operation is ALLOWED, which is the precise outcome the requirement exists to
// prevent. Every multiply and add here is checked, an overflow latches, and a
// latched estimate refuses. A saturating maximum would have been the other
// answer and it is worse: `SIZE_MAX` bytes compares as over budget only while
// there IS a budget, so an unbudgeted caller would sail through on a number
// that means "we lost count".

#include <cstdint>

namespace clay {
namespace memory {

// Why an estimate refused. Distinct from a domain error — the caller's model is
// fine, the machine is not — so a host can say "this model is too big for this
// device" rather than "this model is broken".
enum class BudgetError : std::uint32_t {
    None = 0,
    // The predicted peak exceeds the declared budget. Refused whole; nothing
    // was allocated.
    OverBudget = 1,
    // The arithmetic itself overflowed 64 bits. Reported as a refusal rather
    // than as a number, because the number would be wrong in the one direction
    // that matters.
    Overflow = 2,
};

const char* budget_error_text(BudgetError error);

// `a * b`, or false and `*out` untouched. Free-standing because the sizes it
// multiplies come from four modules that may not include each other.
bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t* out);
bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t* out);

// What an operation would cost, in the three figures a host acts on.
//
// `peak` is not a fourth independent number: it is what is held AT ONCE, which
// is the persistent result plus whatever the operation needs only while it
// runs. Reporting only the persistent cost is what makes an operation that
// "fits" terminate the process half way through.
struct CapacityEstimate {
    std::uint64_t authoritative_bytes = 0;  // kept, and the user's work
    std::uint64_t runtime_bytes = 0;        // kept, and rebuildable
    std::uint64_t persistent_bytes = 0;     // the two above
    std::uint64_t peak_bytes = 0;           // the high-water mark during the call

    bool allowed = true;
    BudgetError error = BudgetError::None;
};

// Accumulates an operation's three parts and finishes into an estimate.
//
// TRANSIENT IS A THIRD KIND, not a rounding of the other two: it is the memory
// that exists only between the first line of the operation and the last, and it
// is the whole reason the peak is asked for separately.
class CapacityBuilder {
  public:
    // `count * stride` bytes of each kind. An overflow latches and every
    // subsequent call is a no-op, so the first bad multiply is the one
    // reported rather than the last.
    void authoritative(std::uint64_t count, std::uint64_t stride);
    void runtime(std::uint64_t count, std::uint64_t stride);
    void transient(std::uint64_t count, std::uint64_t stride);

    // Bytes whose shape is not count-times-stride — a blob, a fixed header.
    void authoritative_bytes(std::uint64_t bytes);
    void runtime_bytes(std::uint64_t bytes);
    void transient_bytes(std::uint64_t bytes);

    bool overflowed() const { return overflow_; }

    // `budget` of zero means "no budget", which is what a desktop host and
    // every existing caller pass, and what `SculptMemoryProfile` means by a
    // zero byte field. An overflow refuses at ANY budget including no budget:
    // an estimate nobody can compute is not one anybody may rely on.
    CapacityEstimate finish(std::uint64_t budget = 0) const;

  private:
    void accumulate(std::uint64_t* target, std::uint64_t count, std::uint64_t stride);

    std::uint64_t authoritative_ = 0;
    std::uint64_t runtime_ = 0;
    std::uint64_t transient_ = 0;
    bool overflow_ = false;
};

}  // namespace memory
}  // namespace clay
