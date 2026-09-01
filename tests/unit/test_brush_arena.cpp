// THE ARENA'S OWN CONTRACT (meshing spec, add-shared-brush-runtime 6.1).
//
// `tests/unit/test_sculpt_allocation.cpp` gates the arena's PURPOSE — that a
// warm stamp allocates nothing — and it cannot gate the arena's BEHAVIOUR,
// because an arena that never reset and grew without bound would satisfy it
// perfectly: allocating nothing after warm-up is exactly what unbounded
// retention looks like from an allocation counter. That is the failure
// `growths()` and `high_water_bytes()` exist to make visible, and this file is
// where they are read.
//
// SO THE ASSERTIONS HERE ARE ABOUT THE STATISTICS AS MUCH AS THE STORAGE. Bump
// order and alignment are the easy half and a broken allocator would be found
// by any caller; a `high_water_bytes()` that quietly reported a CURRENT usage,
// or a `reset()` that freed the block it is documented to keep, would be found
// by nothing else in the tree until a stroke on a real surface started
// allocating per dab again.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/mesh/brush_arena.h"
#include "clay/mesh/work_item.h"

using clay::mesh::BrushArenaScope;
using clay::mesh::BrushScratchArena;
using clay::mesh::ScratchVector;
using clay::mesh::WorkItemId;

namespace {

// A pointer as an integer, for the ordering and overlap questions. Comparing
// unrelated pointers with `<` is unspecified in C++ and UBSan objects to it;
// through `uintptr_t` the question is a well-defined one about addresses.
std::uintptr_t addr(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

}  // namespace

TEST_CASE("arena: allocations bump forward and do not overlap") {
    BrushScratchArena arena;

    // Sized so all three fit one minimum block: the ordering claim is about
    // the bump pointer inside a block, and an overflow would move the second
    // allocation to a fresh block where "forward" means nothing.
    float* a = arena.allocate<float>(16);
    float* b = arena.allocate<float>(16);
    float* c = arena.allocate<float>(16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);

    CHECK(addr(b) >= addr(a) + 16 * sizeof(float));
    CHECK(addr(c) >= addr(b) + 16 * sizeof(float));

    // Writing through each and reading all three back is what says they are
    // genuinely distinct storage rather than three names for one block — the
    // defect an address comparison alone would miss if `take` returned the
    // same pointer and merely advanced a counter.
    for (int i = 0; i < 16; ++i) {
        a[i] = 1.0f;
        b[i] = 2.0f;
        c[i] = 3.0f;
    }
    for (int i = 0; i < 16; ++i) {
        CHECK(a[i] == 1.0f);
        CHECK(b[i] == 2.0f);
        CHECK(c[i] == 3.0f);
    }
}

TEST_CASE("arena: a zero-count request is a null and costs nothing") {
    // The callers ask for `workset.size()` elements and a workset can be empty.
    // Returning a valid pointer to nothing would be defensible; returning null
    // and taking no storage is what the header promises, and a caller that
    // dereferenced it would be dereferencing a zero-length buffer either way.
    BrushScratchArena arena;
    CHECK(arena.allocate<float>(0) == nullptr);
    CHECK(arena.capacity_bytes() == 0);
    CHECK(arena.growths() == 0);
}

TEST_CASE("arena: every block is aligned for what was asked for") {
    BrushScratchArena arena;

    // Interleaved deliberately. A one-byte allocation between two wide ones is
    // what leaves the bump pointer at an odd offset, and an `align_up` that was
    // wrong would be invisible on a run of same-sized requests.
    for (int round = 0; round < 8; ++round) {
        auto* one = arena.allocate<std::uint8_t>(3);
        auto* four = arena.allocate<std::uint32_t>(5);
        auto* eight = arena.allocate<std::uint64_t>(2);
        auto* item = arena.allocate<WorkItemId>(7);
        auto* vec = arena.allocate<clay::kernel::cfloat3>(3);
        REQUIRE(one != nullptr);
        CHECK(addr(four) % alignof(std::uint32_t) == 0);
        CHECK(addr(eight) % alignof(std::uint64_t) == 0);
        CHECK(addr(item) % alignof(WorkItemId) == 0);
        CHECK(addr(vec) % alignof(clay::kernel::cfloat3) == 0);
    }
}

TEST_CASE("arena: reset returns the pointer and KEEPS the storage") {
    BrushScratchArena arena;
    float* first = arena.allocate<float>(64);
    REQUIRE(first != nullptr);

    const std::size_t capacity = arena.capacity_bytes();
    const std::size_t growths = arena.growths();
    REQUIRE(capacity > 0);
    REQUIRE(arena.used_bytes() == 64 * sizeof(float));

    arena.reset();

    // THE WHOLE POINT OF THE TYPE, in three lines: the usage is back to zero,
    // the storage is still owned, and nothing was counted as a growth. An arena
    // that freed here would allocate again on the next stamp, which is the
    // behaviour the allocation gate forbids and the reason `reset` is not
    // spelled `clear`.
    CHECK(arena.used_bytes() == 0);
    CHECK(arena.capacity_bytes() == capacity);
    CHECK(arena.growths() == growths);

    // And the same request is served from the same block, at the same address.
    float* second = arena.allocate<float>(64);
    CHECK(second == first);
    CHECK(arena.capacity_bytes() == capacity);
    CHECK(arena.growths() == growths);
}

TEST_CASE("arena: high water is a maximum and not a current") {
    // THE DISTINCTION THE STATISTIC EXISTS FOR. A current usage reads zero
    // between stamps whatever the arena is doing, so it can never say whether
    // the arena has converged. This is the assertion that would fail if
    // somebody replaced `high_water_bytes()` with the obvious cheaper thing.
    BrushScratchArena arena;

    arena.allocate<float>(2048);  // 8 KiB, the peak
    const std::size_t peak = arena.high_water_bytes();
    REQUIRE(peak >= 2048 * sizeof(float));
    arena.reset();

    CHECK(arena.used_bytes() == 0);
    CHECK(arena.high_water_bytes() == peak);

    // A SMALLER stamp must not lower it. This is the direction a "current
    // usage" implementation gets wrong, and the direction a host budgeting
    // against the number cares about.
    arena.allocate<float>(4);
    CHECK(arena.high_water_bytes() == peak);
    arena.reset();
    CHECK(arena.high_water_bytes() == peak);

    // A LARGER one must raise it.
    arena.allocate<float>(4096);
    CHECK(arena.high_water_bytes() > peak);
}

TEST_CASE("arena: growths stop over a stroke of similar stamps") {
    // THE CONVERGENCE CLAIM, which is the one an allocation count cannot make.
    //
    // A stroke is modelled as it actually runs: a stamp allocates several
    // buffers sized to its footprint, and then resets. The footprint here wanders
    // slightly, as a real one does, but does not grow — so after the first few
    // stamps the arena must have stopped taking storage entirely.
    BrushScratchArena arena;

    auto stamp = [&](std::size_t items) {
        arena.reset();
        arena.allocate<WorkItemId>(items);
        arena.allocate<float>(items);
        arena.allocate<clay::kernel::cfloat3>(items);
        arena.allocate<std::uint32_t>(items);
    };

    for (int i = 0; i < 8; ++i) stamp(900 + static_cast<std::size_t>(i % 5));

    const std::size_t warm_growths = arena.growths();
    const std::size_t warm_capacity = arena.capacity_bytes();
    REQUIRE(warm_growths > 0);  // it did have to take storage at some point

    for (int i = 0; i < 64; ++i) stamp(900 + static_cast<std::size_t>(i % 5));

    // Not "grew slowly" — did not grow at all. Scratch that leaks a little per
    // stamp shows up here and in no other gate in this repository.
    CHECK(arena.growths() == warm_growths);
    CHECK(arena.capacity_bytes() == warm_capacity);
}

TEST_CASE("arena: the growth counter is discriminating") {
    // The companion to the assertion above, on the same argument every other
    // self-check in this suite makes: a `growths()` hard-wired to a constant
    // would pass it. A footprint that keeps climbing must keep the counter
    // climbing.
    // Asserted as a CONTRAST rather than against a magnitude, because the
    // growth policy takes the larger of what was asked for and what it already
    // owns — so a climbing footprint converges in a few doublings and a bare
    // number here would be a test of the doubling constant rather than of the
    // counter. Twelve stamps at the same LARGEST size take one growth; twelve
    // that climb to it take several, and only a counter that actually counts
    // can tell the two runs apart.
    const std::size_t largest = 12 * 4096;

    BrushScratchArena climbing;
    for (int i = 1; i <= 12; ++i) {
        climbing.reset();
        climbing.allocate<float>(static_cast<std::size_t>(i) * 4096);
    }

    BrushScratchArena settled;
    for (int i = 1; i <= 12; ++i) {
        settled.reset();
        settled.allocate<float>(largest);
    }

    CAPTURE(climbing.growths());
    CAPTURE(settled.growths());
    CHECK(climbing.growths() > settled.growths());
    CHECK(settled.growths() == 1);

    // AND THE HIGH WATER OVERSTATES THE STAMP THAT OVERFLOWED, deliberately:
    // an overflow block leaves the tail of its predecessor unusable until the
    // next reset, so `used_bytes` counts the blocks below the bump pointer as
    // spent. A host budgeting against the number wants that direction — it can
    // only ever be conservative — and a run that never overflowed reports the
    // exact figure, which is the pair asserted here.
    CHECK(settled.high_water_bytes() == largest * sizeof(float));
    CHECK(climbing.high_water_bytes() > settled.high_water_bytes());
}

TEST_CASE("arena: an overflowing stamp keeps the pointers it already handed out") {
    // THE ONE REAL HAZARD OF A BUMP ALLOCATOR, and the reason `take` opens a
    // new block instead of reallocating: a stamp holds several pointers at once
    // — the automask's depth array while its frontier is being filled — and a
    // reallocation of the block under them is a use-after-free deep inside a
    // brush that no test which only reads the LAST allocation would notice.
    BrushScratchArena arena;
    std::uint32_t* held = arena.allocate<std::uint32_t>(64);
    REQUIRE(held != nullptr);
    for (int i = 0; i < 64; ++i) held[i] = static_cast<std::uint32_t>(i * 7 + 1);

    // Far past the first block, so the arena must take another.
    const std::size_t before = arena.growths();
    std::uint32_t* big = arena.allocate<std::uint32_t>(1 << 16);
    REQUIRE(big != nullptr);
    CHECK(arena.growths() > before);

    for (int i = 0; i < 64; ++i) CHECK(held[i] == static_cast<std::uint32_t>(i * 7 + 1));

    // And the reset folds the chain back into one block, so the next stamp of
    // the same size fits without taking anything more.
    const std::size_t capacity = arena.capacity_bytes();
    const std::size_t growths = arena.growths();
    arena.reset();
    CHECK(arena.capacity_bytes() == capacity);
    CHECK(arena.growths() == growths);
    arena.allocate<std::uint32_t>(64);
    arena.allocate<std::uint32_t>(1 << 16);
    CHECK(arena.growths() == growths);
}

TEST_CASE("arena: a nested scope makes the high water a peak rather than a sum") {
    // WHY `BrushArenaScope` EXISTS. Without it the automask's five arrays stay
    // reserved while the region sort runs, although neither can see the other,
    // and the arena converges on the SUM of everything a stamp allocates. This
    // asserts the difference in the only quantity that can see it.
    const std::size_t chunk = 4096;

    std::size_t summed = 0;
    {
        BrushScratchArena arena;
        arena.allocate<float>(chunk);
        arena.allocate<float>(chunk);
        arena.allocate<float>(chunk);
        summed = arena.high_water_bytes();
    }

    std::size_t peaked = 0;
    {
        BrushScratchArena arena;
        for (int i = 0; i < 3; ++i) {
            BrushArenaScope scope(arena);
            arena.allocate<float>(chunk);
        }
        peaked = arena.high_water_bytes();
    }

    CAPTURE(summed);
    CAPTURE(peaked);
    CHECK(peaked < summed);
    CHECK(peaked >= chunk * sizeof(float));
}

TEST_CASE("arena: a scope restores the position exactly") {
    BrushScratchArena arena;
    float* before = arena.allocate<float>(8);
    const std::size_t used = arena.used_bytes();
    {
        BrushArenaScope scope(arena);
        arena.allocate<float>(8);
        CHECK(arena.used_bytes() > used);
    }
    CHECK(arena.used_bytes() == used);

    // The next allocation reuses the storage the scope gave back, which is the
    // property that makes a scope free rather than merely tidy.
    float* after = arena.allocate<float>(8);
    CHECK(addr(after) == addr(before) + 8 * sizeof(float));
}

// -- the growable view --------------------------------------------------------

TEST_CASE("scratch vector: fills to its capacity and refuses past it") {
    BrushScratchArena arena;
    ScratchVector<std::uint32_t> v = arena.vector<std::uint32_t>(4);

    CHECK(v.empty());
    CHECK(v.capacity() == 4);
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(v.push_back(i));
    CHECK(v.size() == 4);
    CHECK_FALSE(v.overflowed());

    // PAST THE BOUND IT REFUSES AND SAYS SO. It does not grow — the next arena
    // allocation already sits behind it — and it does not silently drop, which
    // would be a hole in an automask that nothing reported.
    CHECK_FALSE(v.push_back(99));
    CHECK(v.overflowed());
    CHECK(v.size() == 4);
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(v[i] == i);
}

TEST_CASE("scratch vector: clear keeps the block and the overflow flag is sticky") {
    BrushScratchArena arena;
    ScratchVector<std::uint32_t> v = arena.vector<std::uint32_t>(2);
    v.push_back(1);
    v.push_back(2);
    REQUIRE_FALSE(v.push_back(3));
    REQUIRE(v.overflowed());

    v.clear();
    CHECK(v.empty());
    CHECK(v.capacity() == 2);
    // STICKY ON PURPOSE. `overflowed()` reports that a bound the caller stated
    // was wrong, which is a defect in the bound rather than a condition that
    // passes when the buffer is emptied — a flag cleared here would let a flood
    // that overflowed mid-pass look clean at the end of it.
    CHECK(v.overflowed());
}

TEST_CASE("scratch vector: assign_all fills the capacity and counts it") {
    // What the automask's dense per-slot arrays want: `depth` and `reached` are
    // indexed rather than appended to, so they need a size equal to the
    // capacity before the first write.
    BrushScratchArena arena;
    ScratchVector<float> v = arena.vector<float>(6);
    v.assign_all(-1.0f);
    CHECK(v.size() == 6);
    for (std::size_t i = 0; i < v.size(); ++i) CHECK(v[i] == -1.0f);

    v[3] = 2.5f;
    CHECK(v[3] == 2.5f);
    CHECK(v[2] == -1.0f);
}

TEST_CASE("scratch vector: iteration covers the size and not the capacity") {
    BrushScratchArena arena;
    ScratchVector<std::uint32_t> v = arena.vector<std::uint32_t>(8);
    v.push_back(10);
    v.push_back(20);

    std::size_t seen = 0;
    std::uint32_t sum = 0;
    for (std::uint32_t x : v) {
        ++seen;
        sum += x;
    }
    CHECK(seen == 2);
    CHECK(sum == 30);
}

// -- the neutral identity -----------------------------------------------------

TEST_CASE("work item: the low half is the dense index in all three identities") {
    // THE ENCODING RULE THE COMPOSITION DEPENDS ON. `compose_workset` publishes
    // `slot[item.key()]` and that one spelling has to be correct on a weld
    // class, an adaptive vertex and a level vertex alike — which is only true
    // because the generation and the level live in the HIGH half. This asserts
    // the rule directly, because everything that relies on it relies on it
    // silently.
    const WorkItemId cls = WorkItemId::weld_class(1234);
    CHECK(cls.key() == 1234u);
    CHECK(cls.as_weld_class() == 1234u);

    clay::mesh::VertexId v;
    v.slot = 77;
    v.generation = 9;
    const WorkItemId sv = WorkItemId::surface_vertex(v);
    CHECK(sv.key() == 77u);
    CHECK(sv.as_surface_vertex().slot == 77u);
    CHECK(sv.as_surface_vertex().generation == 9u);

    const WorkItemId lv = WorkItemId::level_vertex(3, 4096);
    CHECK(lv.key() == 4096u);
    CHECK(lv.level() == 3u);
    CHECK(lv.level_vertex_index() == 4096u);
}

TEST_CASE("work item: a generation is carried and distinguishes a retired handle") {
    // The reason the identity is 64 bits at all. Two `VertexId`s in the same
    // slot at different generations are DIFFERENT items, and a workset that
    // could not tell them apart would let a retired handle sitting in a write
    // region read as a live one.
    clay::mesh::VertexId live;
    live.slot = 5;
    live.generation = 2;
    clay::mesh::VertexId stale;
    stale.slot = 5;
    stale.generation = 1;

    CHECK(WorkItemId::surface_vertex(live) != WorkItemId::surface_vertex(stale));
    CHECK(WorkItemId::surface_vertex(live).key() == WorkItemId::surface_vertex(stale).key());
}

TEST_CASE("work item: ordering is by the dense half first") {
    // Determinism depends on this: every representation's own sort orders by
    // the dense index, and ordering by the generation first would make a sort
    // depend on the HISTORY of edits rather than on the surface.
    clay::mesh::VertexId a;
    a.slot = 4;
    a.generation = 9;
    clay::mesh::VertexId b;
    b.slot = 5;
    b.generation = 1;
    CHECK(WorkItemId::surface_vertex(a) < WorkItemId::surface_vertex(b));

    clay::mesh::VertexId c;
    c.slot = 4;
    c.generation = 10;
    CHECK(WorkItemId::surface_vertex(a) < WorkItemId::surface_vertex(c));
}
