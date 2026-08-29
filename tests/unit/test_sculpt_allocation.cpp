// THE ALLOCATION GATE (meshing spec, add-shared-brush-kernels 4.5).
//
// "A stamp costs what it touches" is the rule the whole mesh brush path is
// built around, and every other test in this tree checks the SHAPE of that
// claim — that a walk is bounded, that a reset runs over a list rather than an
// array. None of them can see the one thing that quietly breaks it: a
// `std::vector` created inside the stamp path, which allocates and frees on
// every dab and is invisible to any test that only reads results. This file
// counts allocations and asserts there are none.
//
// HOW IT COUNTS. `operator new` and `operator delete` are replaceable
// functions, and replacing them here replaces them for the whole test binary.
// The counter is off until a scope turns it on, so nothing doctest or the
// fixture builders do is counted — only what happens between the two braces.
//
// WHY NOT MEASURE CAPACITIES INSTEAD. Comparing the scratch buffers' `data()`
// pointers before and after would catch a REALLOCATION and would miss a
// temporary that allocates and frees inside one stamp, which is precisely the
// defect this exists to catch — and precisely the defect it did catch, in the
// colour path, where the pre-stamp colour copy was a local.

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;
using mesh::VertexDeltas;

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

// A scope that counts. Deliberately narrow: the count has to cover one stamp
// and nothing around it, or a fixture's own allocations would drown the signal.
struct CountingScope {
    std::size_t before;
    CountingScope() : before(g_allocations.load()) { g_counting.store(true); }
    ~CountingScope() { g_counting.store(false); }
    std::size_t count() const { return g_allocations.load() - before; }
};

Mesh plane_grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(-half + step * static_cast<float>(x),
                                      ((x + z) & 1) ? 0.0625f : 0.0f,
                                      -half + step * static_cast<float>(z)));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// Warm the scratch, then measure one more stamp of the same footprint.
//
// `record` decides WHAT is being measured, and the distinction is the whole
// subtlety of this gate.
//
//   - Without one, the count is the sculptor's SCRATCH: the workset, the
//     neighbour arrays, the smoothing buffers, the walk's frontier. That must
//     be exactly zero, because none of it is anybody's payload — it is
//     storage the sculptor is supposed to own for its whole life.
//
//   - With one, an undo record legitimately grows the first time it sees a
//     vertex: it is keeping that vertex's before-state, which is the record's
//     CONTENT rather than churn. So the measured stamp has to be one whose
//     vertices the record has already noted, and then it must be zero too.
//
// Conflating the two is how this test first read, and it reported eight
// sixteen-byte allocations per stamp that were all hash-map nodes for vertices
// the record had never seen — a true measurement of the wrong thing.
std::size_t allocations_for_warm_stamp(MeshBrush verb, bool with_colors, bool with_record) {
    Mesh m = plane_grid(48, 1.0f);
    if (with_colors) m.colors.assign(m.positions.size(), cf3(0.5f, 0.5f, 0.5f));
    MeshSculptor sculptor(m);
    VertexDeltas record;
    VertexDeltas* rec = with_record ? &record : nullptr;

    MeshBrushSettings s;
    s.radius = 0.3f;
    s.strength = 0.25f;
    s.smooth_iterations = 2;
    s.direction = cf3(0.02f, 0.01f, 0.0f);
    s.geodesic = mesh::default_geodesic(verb);

    // WARM-UP. Growth on first encountering a footprint is permitted by the
    // requirement; steady repeated local sculpting is not.
    for (int i = 0; i < 6; ++i) {
        s.center = cf3(-0.05f + 0.02f * static_cast<float>(i), 0.0f, 0.0f);
        sculptor.stamp(verb, s, {}, rec);
    }
    // The stamp about to be measured, run several times first. Once is not
    // enough for every verb: Pinch gathers vertices tangentially toward the
    // centre, so its footprint CREEPS for a few stamps before it settles, and
    // one extra class in the workset is one legitimate growth of a buffer.
    // "Growth on first encountering a larger footprint is permitted; steady
    // repeated local sculpting is not" is the requirement, and this loop is
    // where the difference between the two is drawn.
    s.center = cf3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 8; ++i) sculptor.stamp(verb, s, {}, rec);

    CountingScope scope;
    sculptor.stamp(verb, s, {}, rec);
    return scope.count();
}

}  // namespace

// Replacing the global allocator. Both sized and unsized deletes, and the
// nothrow forms, or a mismatched pair is undefined behaviour that UBSan will
// find before a human does.
void* operator new(std::size_t n) {
    if (g_counting.load()) g_allocations.fetch_add(1);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_counting.load()) g_allocations.fetch_add(1);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return ::operator new(n, std::nothrow);
}
// GCC pairs `new`/`delete` by shape and cannot see that these two replacements
// are each other's counterpart, so it reports every `free` here as mismatched.
// A known false positive of replacing the global allocator, and the only way
// past it is to say so.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

TEST_CASE("allocation gate: a warm local stamp allocates nothing") {
    // Every verb, because the defect this catches is per-verb: it is a
    // temporary in one kernel's path, and fifteen clean verbs would hide it.
    // Two of them were real and this test is what found them — a per-dab copy
    // of the pre-stamp colours in the colour path, and a `std::priority_queue`
    // built inside the geodesic walk, which allocated on the DEFAULT footprint
    // of fourteen of the sixteen.
    const MeshBrush verbs[] = {MeshBrush::Grab,      MeshBrush::Draw,    MeshBrush::Inflate,
                               MeshBrush::Smooth,    MeshBrush::Pinch,   MeshBrush::Flatten,
                               MeshBrush::Clay,      MeshBrush::Crease,  MeshBrush::Scrape,
                               MeshBrush::Polish,    MeshBrush::Snakehook, MeshBrush::Relax,
                               MeshBrush::Nudge};
    for (MeshBrush v : verbs) {
        CAPTURE(static_cast<int>(v));
        CHECK(allocations_for_warm_stamp(v, false, false) == 0);
    }

    // The colour pair needs a colour attribute to write into.
    CHECK(allocations_for_warm_stamp(MeshBrush::Paint, true, false) == 0);
    CHECK(allocations_for_warm_stamp(MeshBrush::Smear, true, false) == 0);
}

TEST_CASE("allocation gate: a repeated stamp does not grow the undo record") {
    // With a gesture record attached, a stamp that reaches only vertices the
    // record has already noted must allocate nothing either. The record's
    // storage is coalesced per gesture — first `before`, last `after`, one
    // entry per vertex — so a stroke that goes back over its own path adds
    // nothing, which is the property that keeps an undo step bounded by what
    // the stroke REACHED rather than by how many stamps it took.
    const MeshBrush verbs[] = {MeshBrush::Draw, MeshBrush::Smooth, MeshBrush::Grab,
                               MeshBrush::Layer};
    for (MeshBrush v : verbs) {
        CAPTURE(static_cast<int>(v));
        CHECK(allocations_for_warm_stamp(v, false, true) == 0);
    }
}

TEST_CASE("allocation gate: the counter is discriminating") {
    // If the scope counted nothing at all, the test above would pass on a
    // sculptor that allocated on every vertex. One vector proves the
    // instrument works.
    CountingScope scope;
    std::vector<int> v;
    v.reserve(1024);
    CHECK(scope.count() > 0);
}
