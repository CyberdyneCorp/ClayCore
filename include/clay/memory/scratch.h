#pragma once

// THE PER-STAMP WORKING SET, BOUNDED (sculpt-runtime spec,
// add-extreme-poly-runtime).
//
// The rule the whole change serves is that a dab costs what it TOUCHES, and
// the scratch buffers are where that rule is quietly lost. A stamp's gather,
// its geodesic walk, its neighbour flattening and its normal recompute all
// want a temporary sized to the FOOTPRINT — and the easy way to make them
// allocation-free is to size them to the SURFACE once and reuse them forever.
// That passes an allocation counter, it passes a byte counter, and it is
// O(model) storage whose touch cost scales with the model. It is the exact
// shape this repository has been bitten by before, which is why the arena
// reports a high-water mark that a gate can assert does not move between a 1M
// and a 20M fixture at the same footprint.
//
// TWO BOUNDS, AND THEY DO DIFFERENT JOBS.
//
//   - The SOFT bound tracks the largest footprint of the last few stamps. It is
//     what the arena settles back to, so a stroke of similar dabs allocates on
//     its first stamp and never again, and a stroke that follows a huge one
//     does not keep paying for it.
//   - The HARD bound comes from the host's profile and is never exceeded. Past
//     it the work is processed in BLOCKS rather than allocated — which is what
//     stops a 500k-vertex footprint on a constrained profile from becoming the
//     peak that kills the app. A caller asks `block_elements` how many fit and
//     runs that many at a time.
//
// THE ARENA NEVER GROWS INSIDE A STAMP. `prepare` is the one call that may
// allocate and it happens before any pointer is handed out; `allocate` hands
// out spans of what is already there and returns null rather than reallocating.
// The alternative — growing on demand — would invalidate every pointer a stamp
// is already holding, which is a use-after-free whose trigger is a slightly
// larger brush.
//
// SHRINKING HAPPENS AT A STROKE BOUNDARY AND NOWHERE ELSE. Not on a timer, not
// on a high-water mark, and never inside a pointer event: releasing memory in
// the middle of a drag is a free and a fault the artist pays for on the next
// dab, and the engine does not own that moment. The host does.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "clay/memory/budget.h"

namespace clay {
namespace memory {

class ScratchArena {
  public:
    // How many recent stamps the soft bound remembers. Small on purpose: the
    // arena has to follow a stroke that changes brush size, and a long window
    // makes it hold the biggest dab of the session.
    static constexpr std::size_t kRecentStamps = 8;

    void configure(const SculptMemoryProfile& profile);
    const SculptMemoryProfile& profile() const { return profile_; }

    // -- one stamp ------------------------------------------------------------

    // Reset the bump pointer and make sure `bytes` are available. THE ONLY
    // CALL THAT MAY ALLOCATE. Returns false when `bytes` exceeds the hard
    // bound, which is not a failure: it is the caller's signal to process in
    // blocks, and `block_elements` says how big one is.
    bool prepare(std::size_t bytes);

    // A span of the prepared storage, or null when it does not fit. Never
    // reallocates, so every pointer handed out during one stamp stays valid
    // for the whole of it.
    void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));

    // The typed form, which is what every caller actually wants. The memory is
    // uninitialised and is NOT destroyed: this is for trivially copyable
    // scratch — indices, weights, positions — and nothing else.
    template <typename T>
    T* allocate_array(std::size_t count) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "the scratch arena does not run destructors: trivial types only");
        return static_cast<T*>(allocate(count * sizeof(T), alignof(T)));
    }

    // How many elements of `element_bytes` a single block may hold, given the
    // hard bound. Zero is impossible — a bound smaller than one element still
    // yields one, because processing zero elements per block is an infinite
    // loop and a bound that tight is a host mistake rather than a work plan.
    std::size_t block_elements(std::size_t element_bytes, std::size_t wanted) const;
    // How many passes `wanted` elements take at that block size. One when the
    // whole footprint fits, which is the ordinary case.
    std::size_t block_count(std::size_t element_bytes, std::size_t wanted) const;

    // Record what this stamp actually used. Feeds the soft bound and the
    // high-water mark; allocates nothing.
    void end_stamp();

    // Where this arena publishes its peak, so a host reads one struct rather
    // than asking four subsystems for one number each. Borrowed and never
    // owned: the host outlives the arena in every arrangement this library
    // supports, and an arena that owned its telemetry would reset a host's
    // numbers when a stroke replaced it.
    //
    // Null is the default and costs a null check per stamp. The alternative was
    // making the arena always accumulate into a member and letting the host
    // read it — which is what `high_water()` already is, and which does not
    // compose: the workset and the topology counts come from other objects, and
    // a host tuning a profile wants the four together.
    void set_telemetry(PeakTelemetry* telemetry) { telemetry_ = telemetry; }
    PeakTelemetry* telemetry() const { return telemetry_; }

    // THE ONE PLACE CAPACITY GOES DOWN. Called at a stroke boundary by whoever
    // owns the stroke, never by the arena on its own behalf.
    void end_stroke();

    // -- what a trim may take -------------------------------------------------

    // Release everything above the soft bound, or everything at all under
    // critical pressure. Returns the bytes released. Refuses while a stamp
    // holds pointers into the arena — `used() != 0` — because a trim that
    // freed live scratch would be a use-after-free the host cannot see.
    std::size_t trim(Pressure pressure);

    // -- introspection --------------------------------------------------------

    std::size_t capacity() const { return storage_.size(); }
    std::size_t used() const { return used_; }
    std::size_t soft_bound() const;
    std::size_t hard_bound() const;
    // The largest the arena has ever been. What the allocation gate asserts
    // does not follow the surface size.
    std::size_t high_water() const { return high_water_; }
    // How many times the buffer has actually been reallocated. A warm stroke
    // adds none, and a test can say so without instrumenting the allocator.
    std::size_t growths() const { return growths_; }
    std::size_t bytes() const;
    void report(MemoryLedger* ledger) const;

  private:
    SculptMemoryProfile profile_;
    std::vector<std::uint8_t> storage_;
    std::size_t used_ = 0;
    std::size_t recent_[kRecentStamps] = {};
    std::size_t recent_head_ = 0;
    std::size_t high_water_ = 0;
    std::size_t growths_ = 0;
    PeakTelemetry* telemetry_ = nullptr;
};

}  // namespace memory
}  // namespace clay
