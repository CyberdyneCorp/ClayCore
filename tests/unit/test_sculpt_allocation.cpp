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
#include "clay/mesh/topology_ops.h"

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
// Bytes as well as count, because the two catch different defects. A sweep of
// the whole surface into a vector is ONE allocation however big the surface is;
// only its size gives it away.
std::atomic<std::size_t> g_bytes{0};

// A scope that counts. Deliberately narrow: the count has to cover one stamp
// and nothing around it, or a fixture's own allocations would drown the signal.
struct CountingScope {
    std::size_t before;
    std::size_t before_bytes;
    CountingScope() : before(g_allocations.load()), before_bytes(g_bytes.load()) {
        g_counting.store(true);
    }
    ~CountingScope() { g_counting.store(false); }
    std::size_t count() const { return g_allocations.load() - before; }
    std::size_t bytes() const { return g_bytes.load() - before_bytes; }
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
    if (g_counting.load()) g_bytes.fetch_add(n);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_counting.load()) g_allocations.fetch_add(1);
    if (g_counting.load()) g_bytes.fetch_add(n);
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

// -- the topology operators must not read the whole surface -------------------

namespace {

// A patch at FIXED spacing: `n` grows the surface's extent, not its density, so
// an operator run in the middle of it has an identical neighbourhood at every
// size and the only thing that changes is how much surface surrounds it.
mesh::Mesh flat_patch(int n, float spacing) {
    mesh::Mesh m;
    const float half = spacing * static_cast<float>(n) * 0.5f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + spacing * static_cast<float>(x), 0.0f,
                                      -half + spacing * static_cast<float>(z)));
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

// The edge at the middle of the patch, whichever slot currently holds it.
//
// Re-found each round rather than cached, because a collapse retires the handle
// the previous round used.
bool middle_edge(const mesh::DynamicSurface& surface, std::uint32_t vertex, mesh::EdgeId* out) {
    bool found = false;
    surface.edges().for_each_live([&](mesh::EdgeId id, const mesh::DynamicEdge&) {
        if (found || surface.is_boundary_edge(id)) return;
        if (surface.origin_of(surface.halfedge_of(id)).slot == vertex) {
            *out = id;
            found = true;
        }
    });
    return found;
}

// One split-then-collapse round in the middle of the patch. Returns false if
// either operator was refused, so a size that cannot run the experiment fails
// loudly rather than reporting zero bytes.
bool split_collapse_round(mesh::DynamicSurface& surface, std::uint32_t vertex) {
    mesh::EdgeId target;
    if (!middle_edge(surface, vertex, &target)) return false;
    const mesh::SplitResult split = mesh::split_edge(surface, target);
    if (split.result != mesh::TopologyResult::Ok) return false;
    // Collapse one of the new midpoint's edges, undoing the round's growth so
    // the pools stay the same size across rounds.
    std::vector<mesh::HalfEdgeId> ring;
    if (!surface.outgoing_halfedges(split.vertex, &ring) || ring.empty()) return false;
    for (mesh::HalfEdgeId h : ring)
        if (mesh::collapse_edge(surface, surface.edge_of(h)).result == mesh::TopologyResult::Ok)
            return true;
    return false;
}

// The bytes one warm split-and-collapse round asks for.
//
// WARM, for the reason every other measurement in this file is warm. A pool
// that has never grown reallocates its whole backing vector on the first
// `create`, and `from_mesh` reserves an exact fit — so the very first split at
// any size is charged for the entire surface, which is amortised vector growth
// and not the property under test. After a few rounds the retired slots are on
// the free list and a round reuses them.
std::size_t bytes_for_warm_split_and_collapse(int n) {
    auto surface = mesh::DynamicSurface::from_mesh(flat_patch(n, 0.02f));
    REQUIRE(surface.has_value());
    const auto middle = static_cast<std::uint32_t>(n / 2);
    const auto stride = static_cast<std::uint32_t>(n + 1);
    const std::uint32_t vertex = middle * stride + middle;

    for (int i = 0; i < 8; ++i) REQUIRE(split_collapse_round(*surface, vertex));

    CountingScope scope;
    const bool ok = split_collapse_round(*surface, vertex);
    const std::size_t bytes = scope.bytes();
    REQUIRE(ok);
    return bytes;
}

}  // namespace

TEST_CASE("allocation gate: a topology operator's cost does not follow the surface") {
    // THE REGRESSION FOR A DEFECT NO CORRECTNESS TEST COULD SEE, and none did.
    //
    // `split_edge` and `collapse_edge` each need to re-seat the outgoing handle
    // on a handful of vertices, and each has to hand `reseat_outgoing` some
    // live half-edges to choose from. The first version of both got that list
    // by sweeping EVERY LIVE HALF-EDGE in the surface. It is correct — the
    // right half-edges are certainly in there — the whole suite passed on it,
    // and the scaling test in `test_dynamic_scale.cpp` passed on it too,
    // because that test measures what a stamp TOUCHES and a stamp that reads a
    // million half-edges to find four still touches four.
    //
    // Only the clock knew: on a 320k-face patch a stamp took 750 ms against
    // 1.3 ms on a 20k one, at an identical brush footprint. A wall-clock
    // assertion is not something this suite can carry, so the property is
    // asserted here instead, in the quantity that actually grew — the bytes the
    // operator asks for. Sweeping the surface allocates in proportion to it;
    // naming the neighbourhood does not.
    const std::size_t small = bytes_for_warm_split_and_collapse(40);   // ~3.2k faces
    const std::size_t large = bytes_for_warm_split_and_collapse(200);  // ~80k faces
    CAPTURE(small);
    CAPTURE(large);

    // Twenty-five times the surface. An operator whose appetite follows the
    // surface shows up as roughly that ratio; one that does not is flat. The
    // bound is generous on purpose — this is testing an ASYMPTOTE, and a future
    // change that adds a fixed buffer should not have to edit a number.
    CHECK(large < small * 2 + 4096);
}
