#include "clay/memory/scratch.h"

#include <algorithm>

namespace clay {
namespace memory {
namespace {

std::size_t align_up(std::size_t value, std::size_t align) {
    if (align <= 1) return value;
    const std::size_t rem = value % align;
    return rem == 0 ? value : value + (align - rem);
}

}  // namespace

void ScratchArena::configure(const SculptMemoryProfile& profile) {
    profile_ = profile;
    // A tightened hard bound applies from the NEXT stamp, not to storage a
    // stamp is standing on. The host's warning arrives on its own thread's
    // schedule and the arena's is the stroke.
    if (used_ == 0) {
        const std::size_t hard = hard_bound();
        if (hard != 0 && storage_.size() > hard) {
            storage_.resize(hard);
            storage_.shrink_to_fit();
        }
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
    const std::size_t begin = align_up(used_, align);
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
    storage_.resize(soft);
    storage_.shrink_to_fit();
}

std::size_t ScratchArena::trim(Pressure pressure) {
    // A stamp is standing on this storage. Releasing it would be a
    // use-after-free whose cause is a memory warning arriving mid-dab, which is
    // exactly when one does arrive.
    if (used_ != 0) return 0;
    const std::size_t before = storage_.capacity();
    if (pressure == Pressure::Critical) {
        storage_.clear();
        storage_.shrink_to_fit();
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
