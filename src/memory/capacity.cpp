#include "clay/memory/capacity.h"

#include <limits>

namespace clay {
namespace memory {

const char* budget_error_text(BudgetError error) {
    switch (error) {
        case BudgetError::None:
            return "none";
        case BudgetError::OverBudget:
            return "predicted peak exceeds the declared budget";
        case BudgetError::Overflow:
            return "the capacity estimate overflowed 64 bits";
    }
    return "none";
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    // The division test rather than a 128-bit multiply, because this has to
    // compile the same way on every target this library builds for, including
    // the ones without a widening intrinsic.
    if (a > std::numeric_limits<std::uint64_t>::max() / b) return false;
    *out = a * b;
    return true;
}

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
    *out = a + b;
    return true;
}

void CapacityBuilder::accumulate(std::uint64_t* target, std::uint64_t count,
                                 std::uint64_t stride) {
    if (overflow_) return;
    std::uint64_t product = 0;
    if (!checked_mul(count, stride, &product) || !checked_add(*target, product, target))
        overflow_ = true;
}

void CapacityBuilder::authoritative(std::uint64_t count, std::uint64_t stride) {
    accumulate(&authoritative_, count, stride);
}

void CapacityBuilder::runtime(std::uint64_t count, std::uint64_t stride) {
    accumulate(&runtime_, count, stride);
}

void CapacityBuilder::transient(std::uint64_t count, std::uint64_t stride) {
    accumulate(&transient_, count, stride);
}

void CapacityBuilder::authoritative_bytes(std::uint64_t bytes) { accumulate(&authoritative_, bytes, 1); }
void CapacityBuilder::runtime_bytes(std::uint64_t bytes) { accumulate(&runtime_, bytes, 1); }
void CapacityBuilder::transient_bytes(std::uint64_t bytes) { accumulate(&transient_, bytes, 1); }

CapacityEstimate CapacityBuilder::finish(std::uint64_t budget) const {
    CapacityEstimate out;
    if (overflow_) {
        // NOTHING IS REPORTED, deliberately. Handing back the partial sums
        // beside a refusal invites a caller to read them, and they are the
        // numbers that lost count.
        out.allowed = false;
        out.error = BudgetError::Overflow;
        return out;
    }
    std::uint64_t persistent = 0;
    std::uint64_t peak = 0;
    if (!checked_add(authoritative_, runtime_, &persistent) ||
        !checked_add(persistent, transient_, &peak)) {
        out.allowed = false;
        out.error = BudgetError::Overflow;
        return out;
    }
    out.authoritative_bytes = authoritative_;
    out.runtime_bytes = runtime_;
    out.persistent_bytes = persistent;
    out.peak_bytes = peak;
    // THE PEAK IS WHAT IS COMPARED, not the persistent result. An operation
    // whose result fits and whose high-water mark does not is exactly the
    // failure this refusal exists for.
    if (budget != 0 && peak > budget) {
        out.allowed = false;
        out.error = BudgetError::OverBudget;
    }
    return out;
}

}  // namespace memory
}  // namespace clay
