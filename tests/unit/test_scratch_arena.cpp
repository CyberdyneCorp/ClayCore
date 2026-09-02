// THE PER-STAMP WORKING SET, AND THE BLIND SPOT IT EXISTS TO COVER
// (sculpt-runtime spec, add-extreme-poly-runtime 4.8, and the third assertion
// of the allocation gate).
//
// The easy way to make a stamp allocation-free is to size its temporaries to
// the SURFACE once and reuse them forever. That passes an allocation counter,
// it passes a byte counter, and it is O(model) storage whose touch cost follows
// the model — which is precisely the rule the whole change is defending. This
// repository has been bitten by the shape twice by its own record.
//
// The only assertion that catches it is a HIGH-WATER MARK that does not move
// between a small fixture and a large one at the same footprint, and that is
// what this file is for: the arena reports one, and the last case here is the
// gate. The gate is stated on the arena rather than on `MeshSculptor` because
// the arena is where the mechanism is; the sculptor's own gather does not
// consume it yet (tasks 3.1 and 3.7), and asserting it there would be asserting
// something nothing implements.
//
// THE OTHER THREE PROPERTIES, in the order they matter:
//
//   - A WARM STROKE GROWS ONCE. `growths()` is the count of actual
//     reallocations, so "the second dab of a stroke allocates nothing" is a
//     number rather than a pointer comparison.
//   - THE ARENA NEVER GROWS INSIDE A STAMP. `allocate` returns null rather
//     than reallocating, because growing would invalidate every pointer the
//     stamp is already holding — a use-after-free whose trigger is a slightly
//     larger brush.
//   - PAST THE HARD BOUND THE WORK IS BLOCKED, NOT ALLOCATED. A 500k-vertex
//     footprint on a constrained profile must not become the peak that kills
//     the app.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/memory/budget.h"
#include "clay/memory/scratch.h"

using namespace clay;
using memory::MemoryClass;
using memory::MemoryLedger;
using memory::Pressure;
using memory::ScratchArena;
using memory::SculptMemoryProfile;

namespace {

// One stamp's worth of scratch, at a footprint expressed in vertices. This is
// the shape a gather actually has: an id per touched vertex, a weight, and a
// position.
constexpr std::size_t kBytesPerVertex =
    sizeof(std::uint32_t) + sizeof(float) + 3u * sizeof(float);

// Run one stamp against the arena and report whether the whole footprint fitted
// in a single pass.
bool one_stamp(ScratchArena& arena, std::size_t footprint) {
    const std::size_t bytes = footprint * kBytesPerVertex;
    const bool fits = arena.prepare(bytes);
    const std::size_t per_block =
        fits ? footprint : arena.block_elements(kBytesPerVertex, footprint);
    REQUIRE(per_block > 0);
    // ALL THREE ARRAYS, so what the stamp USES is what it PREPARED. An arena
    // settles back to what was used and not to what was asked for, so a helper
    // that prepared for three arrays and allocated two would make every
    // capacity assertion below measure the helper.
    std::uint32_t* ids = arena.allocate_array<std::uint32_t>(per_block);
    float* weights = arena.allocate_array<float>(per_block);
    float* positions = arena.allocate_array<float>(per_block * 3);
    REQUIRE(ids != nullptr);
    REQUIRE(weights != nullptr);
    REQUIRE(positions != nullptr);
    for (std::size_t i = 0; i < per_block; ++i) {
        ids[i] = static_cast<std::uint32_t>(i);
        weights[i] = 1.0f;
        positions[i * 3] = 0.0f;
    }
    arena.end_stamp();
    return fits;
}

SculptMemoryProfile constrained(std::uint64_t scratch_budget) {
    SculptMemoryProfile p;
    p.memory_class = MemoryClass::Constrained;
    p.scratch_budget = scratch_budget;
    return p;
}

}  // namespace

TEST_CASE("scratch arena: a warm stroke of similar dabs grows once and never again") {
    ScratchArena arena;
    CHECK(arena.growths() == 0u);
    CHECK(arena.capacity() == 0u);

    for (int i = 0; i < 32; ++i) CHECK(one_stamp(arena, 4000));
    // ONE growth for the whole stroke. The first dab pays; the rest do not.
    CHECK(arena.growths() == 1u);
    CHECK(arena.capacity() >= 4000u * kBytesPerVertex);
    CHECK(arena.used() == 0u);

    // A LARGER footprint is a legitimate growth — "growth on first encountering
    // a larger footprint is permitted; steady repeated local sculpting is not".
    CHECK(one_stamp(arena, 9000));
    CHECK(arena.growths() == 2u);
    for (int i = 0; i < 16; ++i) CHECK(one_stamp(arena, 9000));
    CHECK(arena.growths() == 2u);
}

TEST_CASE("scratch arena: it never grows inside a stamp, and says so instead") {
    ScratchArena arena;
    REQUIRE(arena.prepare(1024));
    const std::size_t capacity = arena.capacity();
    void* first = arena.allocate(512);
    REQUIRE(first != nullptr);

    // Past what was prepared. A reallocation here would move `first` under a
    // stamp that is still holding it, so the answer is null and the capacity
    // does not move.
    CHECK(arena.allocate(1024) == nullptr);
    CHECK(arena.capacity() == capacity);
    // And the pointer already handed out is still the one that was handed out.
    CHECK(arena.allocate(256) == static_cast<std::uint8_t*>(first) + 512);
    CHECK(arena.capacity() == capacity);

    // Alignment is honoured, and it comes out of the same block rather than a
    // new one.
    arena.end_stamp();
    REQUIRE(arena.prepare(1024));
    CHECK(arena.allocate(1, 1) != nullptr);
    void* aligned = arena.allocate(64, 64);
    REQUIRE(aligned != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 64u == 0u);
}

TEST_CASE("scratch arena: past the hard bound the work is blocked rather than allocated") {
    // A constrained host: 64 KiB of scratch, and a 500k-vertex footprint, which
    // is the largest row of the benchmark matrix.
    ScratchArena arena;
    arena.configure(constrained(64u * 1024u));
    CHECK(arena.hard_bound() == 64u * 1024u);

    const std::size_t footprint = 500000;
    CHECK_FALSE(one_stamp(arena, footprint));
    // THE BOUND HELD. This is the whole point: the peak did not become the
    // footprint.
    CHECK(arena.capacity() <= 64u * 1024u);
    CHECK(arena.high_water() <= 64u * 1024u);

    const std::size_t per_block = arena.block_elements(kBytesPerVertex, footprint);
    CHECK(per_block > 0u);
    CHECK(per_block * kBytesPerVertex <= 64u * 1024u);
    const std::size_t blocks = arena.block_count(kBytesPerVertex, footprint);
    CHECK(blocks > 1u);
    // Every element is covered, and the blocking is not an infinite loop.
    CHECK(blocks * per_block >= footprint);
    CHECK((blocks - 1u) * per_block < footprint);

    // A footprint that FITS takes one pass, which is the ordinary case and must
    // not have been made expensive by the fallback.
    CHECK(arena.block_count(kBytesPerVertex, 100) == 1u);
    // A bound tighter than one element still yields one, because a block of
    // zero elements is an infinite loop and a bound that tight is a host
    // mistake rather than a work plan.
    ScratchArena tiny;
    tiny.configure(constrained(1));
    CHECK(tiny.block_elements(kBytesPerVertex, 10) == 1u);
    CHECK(tiny.block_count(kBytesPerVertex, 10) == 10u);
}

TEST_CASE("scratch arena: capacity comes down at a stroke boundary and nowhere else") {
    ScratchArena arena;
    // One huge dab, then a stroke of small ones.
    CHECK(one_stamp(arena, 40000));
    const std::size_t peak = arena.capacity();
    REQUIRE(peak >= 40000u * kBytesPerVertex);

    for (std::size_t i = 0; i < ScratchArena::kRecentStamps + 2; ++i) CHECK(one_stamp(arena, 500));
    // STILL HOLDING IT. Releasing in the middle of a drag is a free and a fault
    // the artist pays for on the next dab, and the engine does not own that
    // moment.
    CHECK(arena.capacity() == peak);

    const std::size_t held_bytes = arena.bytes();
    arena.end_stroke();
    // Now it settles back to what the recent stamps actually needed. Asserted
    // on `bytes()` as well as on `capacity()`, and that is not belt and braces:
    // `capacity()` is the vector's SIZE and `bytes()` is what it is actually
    // holding, so a shrink that resized without releasing moves the first and
    // not the second. See the regression case at the end of this file.
    CHECK(arena.capacity() < peak);
    CHECK(arena.bytes() < held_bytes);
    CHECK(arena.capacity() >= 500u * kBytesPerVertex);
    // And the HIGH-WATER MARK does not come down with it: it is a record of
    // what was needed, not of what is held.
    CHECK(arena.high_water() >= peak);
}

TEST_CASE("scratch arena: a trim refuses while a stamp is standing on the storage") {
    ScratchArena arena;
    CHECK(one_stamp(arena, 8000));
    arena.end_stroke();
    const std::size_t held = arena.capacity();
    REQUIRE(held > 0u);

    REQUIRE(arena.prepare(8000 * kBytesPerVertex));
    REQUIRE(arena.allocate(1024) != nullptr);
    REQUIRE(arena.used() != 0u);
    // A memory warning arriving mid-dab is exactly when one arrives, and
    // freeing here would be a use-after-free the host cannot see.
    CHECK(arena.trim(Pressure::Critical) == 0u);
    CHECK(arena.capacity() == held);

    arena.end_stamp();
    CHECK(arena.used() == 0u);
    const std::size_t before_trim = arena.bytes();
    const std::size_t released = arena.trim(Pressure::Critical);
    CHECK(released > 0u);
    CHECK(arena.capacity() == 0u);
    // AND THE MEMORY IS ACTUALLY GONE. The figure it reported is the figure it
    // released, rather than a subtraction of two numbers that both describe
    // storage it is still holding.
    CHECK(arena.bytes() + released == before_trim);
    CHECK(arena.bytes() == sizeof(ScratchArena));
    // The recent window went with it: it is a prediction of the next stamp, and
    // under critical pressure the host would rather pay for the first stamp
    // after the trim than keep holding the prediction.
    CHECK(arena.soft_bound() == 0u);

    // It still works afterwards, which is the difference between releasing a
    // cache and breaking one.
    CHECK(one_stamp(arena, 8000));
    CHECK(arena.capacity() >= 8000u * kBytesPerVertex);
}

TEST_CASE("scratch arena: it reports itself into the ledger under one category") {
    ScratchArena arena;
    CHECK(one_stamp(arena, 5000));
    MemoryLedger ledger;
    arena.report(&ledger);
    CHECK(ledger.of(memory::MemoryCategory::Scratch) == arena.bytes());
    CHECK(arena.bytes() >= arena.capacity());
    // Scratch is rebuildable — it is the one line a host may take back with no
    // consequence at all.
    CHECK(memory::category_is_rebuildable(memory::MemoryCategory::Scratch));
    CHECK(ledger.rebuildable() >= arena.bytes());
    CHECK(ledger.essential() == 0u);
}

TEST_CASE("allocation gate, third assertion: the high-water mark follows the footprint") {
    // THE DEFECT THIS CATCHES AND THE OTHER TWO CANNOT. A buffer sized to the
    // SURFACE, allocated once during warm-up and reused forever, performs no
    // allocation on a warm stamp and costs no bytes on one. It passes an
    // allocation counter and it passes a byte counter. It is still O(model)
    // storage whose touch cost scales with the model, and the only thing that
    // sees it is a peak that does not move between a small model and a large
    // one at the same footprint.
    //
    // The two arenas below are "the same stroke on a 1M-vertex model and on a
    // 20M-vertex one": identical footprints, and nothing about the model is
    // handed to the arena at all — which is the property, expressed as an API
    // shape rather than as a measurement.
    const std::size_t footprints[5] = {1000, 5000, 20000, 100000, 500000};

    for (std::size_t footprint : footprints) {
        CAPTURE(footprint);
        ScratchArena small_model;
        ScratchArena large_model;
        for (int i = 0; i < 12; ++i) {
            CHECK(one_stamp(small_model, footprint));
            CHECK(one_stamp(large_model, footprint));
        }
        // THE GATE.
        CHECK(small_model.high_water() == large_model.high_water());
        CHECK(small_model.high_water() == footprint * kBytesPerVertex);
        CHECK(small_model.growths() == large_model.growths());
        CHECK(small_model.growths() == 1u);
    }

    // And the peak telemetry the gate reads is a high-water mark rather than
    // an average, which is the other half of the same statement: an average
    // over a stroke of small dabs hides the one big one that decided the peak.
    memory::PeakTelemetry peak;
    peak.observe_scratch(4096);
    peak.observe_scratch(64);
    CHECK(peak.scratch_bytes == 4096u);
    peak.observe_workset(2000);
    peak.observe_workset(10);
    CHECK(peak.workset_vertices == 2000u);
    peak.observe_dirty(17);
    peak.observe_dirty(2);
    CHECK(peak.dirty_chunks == 17u);
    peak.observe_topology(9);
    peak.observe_topology(1);
    CHECK(peak.topology_ops == 9u);
    peak.reset();
    CHECK(peak.scratch_bytes == 0u);
    CHECK(peak.workset_vertices == 0u);
    CHECK(peak.dirty_chunks == 0u);
    CHECK(peak.topology_ops == 0u);
}

TEST_CASE("regression: releasing scratch does not go through shrink_to_fit") {
    // THE BUG THIS PINS, found by the trim case above and true of every
    // release path this arena had.
    //
    // `std::vector::shrink_to_fit` is implemented in libstdc++ through
    // `__shrink_to_fit_aux`, whose no-exceptions form returns false without
    // doing anything — and this library compiles its core with
    // `-fno-exceptions`. So `resize(n); shrink_to_fit();` is `resize(n)`, and
    // every one of `configure`, `end_stroke` and `trim` gave back nothing while
    // reporting that it had. A 160 KB arena under CRITICAL pressure — the last
    // stop before the operating system kills the process — returned 0 and kept
    // every byte.
    //
    // It is invisible to `capacity()`, which is the vector's SIZE and does move.
    // Only a figure derived from what is actually held sees it, which is why
    // every assertion here is on `bytes()`.
    //
    // Four other files in this tree still call `shrink_to_fit` and are
    // therefore still no-ops — `field/volume.cpp`, `brick/cache.cpp`,
    // `voxel/sculpt.cpp` and `bindings/c/clay_c.cpp`. They predate this change
    // and are not touched by it; this case exists so that the two paths the
    // memory tier depends on cannot quietly go back.
    ScratchArena arena;
    CHECK(one_stamp(arena, 20000));
    const std::size_t grown = arena.bytes();
    REQUIRE(grown > 20000u * kBytesPerVertex);

    // 1. A STROKE BOUNDARY GIVES BACK WHAT THE STROKE STOPPED NEEDING.
    for (std::size_t i = 0; i < ScratchArena::kRecentStamps; ++i) CHECK(one_stamp(arena, 100));
    arena.end_stroke();
    CHECK(arena.bytes() < grown);

    // 2. A CONSTRAINED PROFILE ARRIVING LATE GIVES BACK WHAT IS OVER ITS BOUND.
    CHECK(one_stamp(arena, 20000));
    arena.end_stroke();
    const std::size_t before = arena.bytes();
    SculptMemoryProfile tight = constrained(4096);
    arena.configure(tight);
    CHECK(arena.bytes() < before);
    CHECK(arena.bytes() <= sizeof(ScratchArena) + 4096u);

    // 3. AND CRITICAL PRESSURE GIVES BACK ALL OF IT.
    CHECK(arena.trim(Pressure::Critical) > 0u);
    CHECK(arena.bytes() == sizeof(ScratchArena));
}
