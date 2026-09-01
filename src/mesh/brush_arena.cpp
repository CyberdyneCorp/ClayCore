#include "clay/mesh/brush_arena.h"

#include <algorithm>
#include <new>

namespace clay {
namespace mesh {

namespace {

// The smallest block worth owning. A first stamp on a small footprint would
// otherwise take a few hundred bytes and then take another block on the next
// one, which is two growths to reach a size a single page would have covered.
constexpr std::size_t kMinimumBlockBytes = 4096;

std::size_t align_up(std::size_t value, std::size_t align) {
    return (value + align - 1) & ~(align - 1);
}

}  // namespace

std::size_t BrushScratchArena::used_bytes() const {
    // Everything in the blocks BELOW the bump pointer counts as spent, because
    // an overflow block leaves the tail of its predecessor unusable until the
    // next reset. Reporting only `offset_` would understate a stamp that
    // overflowed, which is the stamp whose size matters most.
    std::size_t used = offset_;
    for (std::size_t i = 0; i < block_ && i < blocks_.size(); ++i) used += blocks_[i].size;
    return used;
}

void* BrushScratchArena::take(std::size_t bytes, std::size_t align) {
    if (!blocks_.empty()) {
        Block& current = blocks_[block_];
        const std::size_t at = align_up(offset_, align);
        if (at + bytes <= current.size) {
            offset_ = at + bytes;
            high_water_ = std::max(high_water_, used_bytes());
            return current.bytes.get() + at;
        }
    }

    // A NEW BLOCK, never a reallocation of the current one: growing in place
    // would move every pointer this stamp has already been handed. The size is
    // the larger of what was asked for and what the arena already owns, so a
    // stroke that keeps overflowing converges in a few doublings rather than
    // one block per request.
    const std::size_t want = std::max({bytes + align, capacity_, kMinimumBlockBytes});
    Block block;
    block.bytes = std::make_unique<std::byte[]>(want);
    block.size = want;
    capacity_ += want;
    ++growths_;
    // Blocks below the new one are spent for the rest of this stamp: the bump
    // pointer only ever moves forward, and `reset` is what makes them available
    // again.
    blocks_.push_back(std::move(block));
    block_ = blocks_.size() - 1;
    offset_ = 0;

    const std::size_t at = align_up(offset_, align);
    offset_ = at + bytes;
    high_water_ = std::max(high_water_, used_bytes());
    return blocks_[block_].bytes.get() + at;
}

void BrushScratchArena::release(Mark m) {
    // A scope end, not a free. The pointer moves back and nothing is returned
    // to the system, which is what keeps a nested scope as cheap as the reset
    // it imitates.
    block_ = m.block;
    offset_ = m.offset;
}

void BrushScratchArena::reset() {
    block_ = 0;
    offset_ = 0;
    if (blocks_.size() <= 1) return;

    // FOLD THE OVERFLOW BACK IN. A stamp that needed a second block will need
    // roughly as much next time, and leaving the chain would make every
    // subsequent stamp walk it — and, worse, would let the arena report a
    // converged `growths()` while still handing out pointers from three
    // scattered allocations. One block of the same total means the next stamp
    // of the same footprint fits without taking anything, which is the
    // convergence the statistics are asserted on.
    //
    // This allocates, and it is charged to the stamp that overflowed rather
    // than to the one after it. That is the honest place for it: the growth
    // happened there.
    Block folded;
    folded.bytes = std::make_unique<std::byte[]>(capacity_);
    folded.size = capacity_;
    blocks_.clear();
    blocks_.push_back(std::move(folded));
    // `capacity_` is unchanged: the arena owns exactly what it owned, in one
    // piece instead of several. `growths_` is unchanged too — nothing new was
    // asked for, so a stroke that has stopped needing more storage reports a
    // count that has stopped climbing.
}

}  // namespace mesh
}  // namespace clay
