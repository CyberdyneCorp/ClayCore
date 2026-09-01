#include "clay/memory/scratch.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace clay {
namespace memory {
namespace {

std::uintptr_t align_up(std::uintptr_t value, std::size_t align) {
    if (align <= 1) return value;
    const std::uintptr_t rem = value % align;
    return rem == 0 ? value : value + (align - rem);
}

// KEEP `n` BYTES AND GIVE THE REST BACK, WITHOUT `shrink_to_fit`.
//
// This library compiles its core with `-fno-exceptions` (the top-level
// CMakeLists), and libstdc++ implements `vector::shrink_to_fit` through
// `__shrink_to_fit_aux`, whose no-exceptions form returns false without doing
// anything. It is a documented no-op in this build. So `resize(n)` followed by
// a shrink — the obvious way to write all three callers below — keeps the
// buffer and reports having released it. Measured before this was written: a
// critical trim of a 160 KB arena returned 0 and kept every byte, which is the
// worst possible answer for a call a host makes because the operating system
// has already warned it once.
//
// The portable release is a swap with a vector built at the size wanted, which
// is what the standard call does on the platforms where it does anything.
void shrink_to(std::vector<std::uint8_t>* storage, std::size_t n) {
    if (storage->capacity() <= n) return;
    std::vector<std::uint8_t> replacement;
    if (n != 0)
        replacement.assign(storage->begin(),
                           storage->begin() + static_cast<std::ptrdiff_t>(n));
    replacement.swap(*storage);
}

}  // namespace

void ScratchArena::configure(const SculptMemoryProfile& profile) {
    profile_ = profile;
    // A tightened hard bound applies from the NEXT stamp, not to storage a
    // stamp is standing on. The host's warning arrives on its own thread's
    // schedule and the arena's is the stroke.
    if (used_ == 0) {
        const std::size_t hard = hard_bound();
        if (hard != 0 && storage_.size() > hard) shrink_to(&storage_, hard);
    }
}

std::size_t ScratchArena::hard_bound() const {
    return static_cast<std::size_t>(profile_.scratch_budget);
}

std::size_t ScratchArena::soft_bound() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kRecentStamps; ++i) n = std::max(n, recent_[i]);
    return n;
}

bool ScratchArena::prepare(std::size_t bytes) {
    used_ = 0;
    const std::size_t hard = hard_bound();
    const bool fits = hard == 0 || bytes <= hard;
    const std::size_t target = fits ? bytes : hard;
    if (storage_.size() < target) {
        storage_.resize(target);
        ++growths_;
    }
    high_water_ = std::max(high_water_, storage_.size());
    return fits;
}

void* ScratchArena::allocate(std::size_t bytes, std::size_t align) {
    if (storage_.empty()) return nullptr;
    // ALIGNED AGAINST THE BASE POINTER, not against the offset. The storage is
    // a `std::vector<std::uint8_t>`, so its data is aligned for
    // `max_align_t` and no further — an offset-only alignment therefore
    // honours every request up to that and silently under-aligns anything
    // over-aligned, which is the one kind of request that asks because it
    // cannot cope without. For every alignment the base already satisfies this
    // computes the same offset it did before, so no existing caller moves.
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(storage_.data());
    const std::size_t begin = static_cast<std::size_t>(align_up(base + used_, align) - base);
    if (bytes > storage_.size() || begin > storage_.size() - bytes) return nullptr;
    used_ = begin + bytes;
    return storage_.data() + begin;
}

std::size_t ScratchArena::block_elements(std::size_t element_bytes, std::size_t wanted) const {
    if (element_bytes == 0) return wanted;
    const std::size_t hard = hard_bound();
    if (hard == 0) return wanted;
    const std::size_t fit = hard / element_bytes;
    if (fit == 0) return 1;
    return std::min(wanted == 0 ? fit : wanted, fit);
}

std::size_t ScratchArena::block_count(std::size_t element_bytes, std::size_t wanted) const {
    if (wanted == 0) return 0;
    const std::size_t per = block_elements(element_bytes, wanted);
    return (wanted + per - 1) / per;
}

void ScratchArena::end_stamp() {
    recent_[recent_head_] = used_;
    recent_head_ = (recent_head_ + 1) % kRecentStamps;
    used_ = 0;
}

void ScratchArena::end_stroke() {
    const std::size_t soft = soft_bound();
    if (storage_.size() <= soft) return;
    shrink_to(&storage_, soft);
}

std::size_t ScratchArena::trim(Pressure pressure) {
    // A stamp is standing on this storage. Releasing it would be a
    // use-after-free whose cause is a memory warning arriving mid-dab, which is
    // exactly when one does arrive.
    if (used_ != 0) return 0;
    const std::size_t before = storage_.capacity();
    if (pressure == Pressure::Critical) {
        shrink_to(&storage_, 0);
        // The recent window goes too: it is a prediction of the next stamp, and
        // under critical pressure the host would rather pay for the first stamp
        // after the trim than keep holding the prediction.
        for (std::size_t i = 0; i < kRecentStamps; ++i) recent_[i] = 0;
    } else {
        end_stroke();
    }
    const std::size_t after = storage_.capacity();
    return before > after ? before - after : 0;
}

std::size_t ScratchArena::bytes() const { return sizeof(ScratchArena) + storage_.capacity(); }

void ScratchArena::report(MemoryLedger* ledger) const {
    if (ledger != nullptr) ledger->add(MemoryCategory::Scratch, bytes());
}

}  // namespace memory
}  // namespace clay
