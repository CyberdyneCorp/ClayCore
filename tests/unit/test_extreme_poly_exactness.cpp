// WHAT THE SURFACE TIER IS NOT ALLOWED TO CHANGE (sculpt-runtime and
// scene-model specs, add-extreme-poly-runtime 4.6, 6.5 and 7.6).
//
// The rest of this change is measured: a dab costs what it touches, the bytes
// follow the dirty chunks, a stamp allocates nothing. Those gates all fail
// LOUDLY — a ratio goes out of band, a counter is not zero. The failures this
// file is about are the quiet ones, and they are quiet because every one of
// them is a caching decision that was correct until something else moved:
//
//   1. UNDO STOPS BEING EXACT ACROSS A TRIM. `trim` runs when the operating
//      system says so, which is to say between any two calls a host makes,
//      including between a stroke and the undo of it. If releasing a cache
//      changes what undo restores by one ulp, the user gets a surface that is
//      not the one they had, and there is nothing on screen that says so.
//
//   2. WHAT IS SAVED DEPENDS ON WHAT IS CACHED. A trim before a save that
//      changed the bytes would mean the document on disk depends on whether a
//      memory warning arrived first. The assertion is byte equality of the
//      encoding across a critical trim, which is stronger than "it reloads
//      correctly" and is the only form that catches a rebuildable thing having
//      quietly become part of the file.
//
//   3. A STROKE STOPS BEING DETERMINISTIC UNDER PRESSURE. Two machines running
//      the same session diverge when one of them was low on memory. That is a
//      bug report nobody can reproduce.
//
//   4. THE LEVEL A HOST IS DRAWING IS NOT THE LEVEL BEING SCULPTED, and the
//      dirty stream forgets to say so. `test_chunk_transport.cpp` reconstructs
//      the level it edited; a host sculpting at level 1 while displaying
//      level 3 is the ordinary multiresolution workflow, and the display
//      level's chunks are marked by a DIFFERENT path — the propagation inside
//      `partial_evaluate` rather than the edit itself. Nothing covered that
//      path, and a stream that stays silent leaves the viewport showing the
//      surface from before the dab.
//
// Every comparison here is BIT equality rather than a tolerance, for the reason
// `test_memory_trim.cpp` gives: "identical" is the requirement's own word, and
// a tolerance passes a rebuild that re-ordered a float sum.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "clay/memory/budget.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using mesh::ChunkReadback;
using mesh::ChunkRevisions;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresDelta;
using mesh::MultiresError;
using mesh::MultiresSculptor;
using mesh::MultiresSurface;
using mesh::SurfaceView;

namespace {

// A cage at FIXED SPACING whose extent grows with `n`, as everywhere else in
// this change: more of the same geometry, never a more finely subdivided copy.
Mesh quad_field(int n, float spacing) {
    Mesh m;
    const float half = spacing * static_cast<float>(n) * 0.5f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + spacing * static_cast<float>(x), 0.0f,
                                      -half + spacing * static_cast<float>(z)));
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.quads.push_back(at(x, z));
            m.quads.push_back(at(x + 1, z));
            m.quads.push_back(at(x + 1, z + 1));
            m.quads.push_back(at(x, z + 1));
            m.indices.insert(m.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1), at(x, z),
                                               at(x + 1, z + 1), at(x, z + 1)});
        }
    return m;
}

MultiresSurface build(int n, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(quad_field(n, 0.25f), {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

// Everything rebuildable, evaluated — so a trim has something to release and
// the comparison afterwards is against a state that was really there.
void warm_every_level(MultiresSurface& surface) {
    for (std::uint32_t l = 0; l < surface.level_count(); ++l) {
        surface.positions_at(l);
        surface.normals_at(l);
        surface.chunks_at(l);
    }
}

// The whole hierarchy as a host would see it, level by level. Taken WARM, so
// that comparing it after a trim compares the rebuild against what was
// released rather than two evaluations of the same cache.
struct Snapshot {
    std::vector<std::vector<cfloat3>> positions;
    std::vector<std::vector<cfloat3>> normals;
    std::vector<cfloat3> base;
    std::uint64_t checksum = 0;
};

Snapshot snapshot(MultiresSurface& surface) {
    Snapshot s;
    for (std::uint32_t l = 0; l < surface.level_count(); ++l) {
        s.positions.push_back(surface.positions_at(l));
        s.normals.push_back(surface.normals_at(l));
    }
    s.base = surface.base_mesh().positions;
    s.checksum = surface.detail_checksum();
    return s;
}

bool identical(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// Reported as one CHECK per level plus one for the cage, so a failure names
// which level moved rather than saying the hierarchy is different.
void check_same(MultiresSurface& surface, const Snapshot& want, const std::string& what) {
    CAPTURE(what);
    REQUIRE(surface.level_count() == want.positions.size());
    for (std::uint32_t l = 0; l < surface.level_count(); ++l) {
        CAPTURE(l);
        CHECK(identical(surface.positions_at(l), want.positions[l]));
        CHECK(identical(surface.normals_at(l), want.normals[l]));
    }
    CHECK(identical(surface.base_mesh().positions, want.base));
    CHECK(surface.detail_checksum() == want.checksum);
}

// One stroke of four dabs at `level`, coalesced into one undo record — the same
// shape `test_multires_history.cpp` uses, because a single dab would not
// exercise the "first before, last after" coalescing that undo exactness across
// a trim is actually about.
std::size_t sculpt_stroke(MultiresSurface& s, std::uint32_t level, MultiresDelta* record,
                          const std::function<void()>& between = {}) {
    REQUIRE(s.set_sculpt_level(level));
    MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    MeshBrushSettings settings;
    settings.radius = 0.35f;
    settings.strength = 0.5f;
    std::size_t moved = 0;
    for (int i = 0; i < 4; ++i) {
        settings.center = cf3(-0.3f + 0.2f * static_cast<float>(i), 0.0f, 0.0f);
        moved += sculptor.stamp(MeshBrush::Draw, settings, {}, record);
        if (between) between();
    }
    return moved;
}

/* -- the transport, for the cross-level case ------------------------------- */

std::uint32_t bits_of(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// A triangle canonicalised by ROTATION, for the reason `test_chunk_transport`
// gives: the two paths number their vertices differently on purpose, and a sort
// would call an inside-out surface equal.
struct Tri {
    std::array<std::uint32_t, 9> bits{};
    bool operator<(const Tri& o) const { return bits < o.bits; }
    bool operator==(const Tri& o) const { return bits == o.bits; }
};

Tri make_tri(cfloat3 a, cfloat3 b, cfloat3 c) {
    const std::array<std::array<std::uint32_t, 3>, 3> corner = {
        {{bits_of(a.x), bits_of(a.y), bits_of(a.z)},
         {bits_of(b.x), bits_of(b.y), bits_of(b.z)},
         {bits_of(c.x), bits_of(c.y), bits_of(c.z)}}};
    std::size_t first = 0;
    for (std::size_t i = 1; i < 3; ++i)
        if (corner[i] < corner[first]) first = i;
    Tri t;
    for (std::size_t k = 0; k < 3; ++k) {
        const std::array<std::uint32_t, 3>& p = corner[(first + k) % 3];
        for (std::size_t j = 0; j < 3; ++j) t.bits[k * 3 + j] = p[j];
    }
    return t;
}

std::vector<Tri> triangles_of(const Mesh& m) {
    std::vector<Tri> out;
    out.reserve(m.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
        out.push_back(make_tri(m.positions[m.indices[i + 0]], m.positions[m.indices[i + 1]],
                               m.positions[m.indices[i + 2]]));
    std::sort(out.begin(), out.end());
    return out;
}

// A host's copy of one surface, keyed by chunk and patched from the dirty set.
class Reconstruction {
   public:
    void upload_all(const SurfaceView& view) {
        for (std::uint32_t i = 0; i < view.chunk_count(); ++i)
            if (view.chunks().chunk(i) != nullptr) copy_into(view, i);
    }

    std::size_t drain(const SurfaceView& view) {
        const std::vector<std::uint32_t> dirty = view.dirty_chunks();
        for (std::uint32_t id : dirty) {
            if (view.chunks().chunk(id) == nullptr) {
                chunks_.erase(id);
                continue;
            }
            copy_into(view, id);
        }
        return dirty.size();
    }

    std::vector<Tri> surface() const {
        std::vector<Tri> out;
        for (const auto& entry : chunks_)
            out.insert(out.end(), entry.second.begin(), entry.second.end());
        std::sort(out.begin(), out.end());
        return out;
    }

   private:
    void copy_into(const SurfaceView& view, std::uint32_t chunk) {
        const ChunkReadback sized =
            view.copy_chunk(chunk, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
        if (!sized.ok) return;
        std::vector<float> positions(static_cast<std::size_t>(sized.vertex_count) * 3, 0.0f);
        std::vector<std::uint32_t> indices(sized.index_count, 0u);
        const ChunkReadback got = view.copy_chunk(
            chunk, nullptr, positions.empty() ? nullptr : positions.data(), positions.size(),
            nullptr, 0, indices.empty() ? nullptr : indices.data(), indices.size());
        if (!got.ok || got.truncated) return;
        const auto at = [&](std::uint32_t i) {
            return cf3(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
        };
        std::vector<Tri> tris;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
            tris.push_back(make_tri(at(indices[i + 0]), at(indices[i + 1]), at(indices[i + 2])));
        chunks_[chunk] = std::move(tris);
    }

    std::map<std::uint32_t, std::vector<Tri>> chunks_;
};

}  // namespace

TEST_CASE("undo exactness: a critical trim between a stroke and its undo restores every level") {
    MultiresSurface s = build(6, 3);
    warm_every_level(s);
    const Snapshot before = snapshot(s);

    MultiresDelta step;
    REQUIRE(sculpt_stroke(s, 2, &step) > 0);
    warm_every_level(s);
    const Snapshot after = snapshot(s);
    REQUIRE_FALSE(identical(after.positions[3], before.positions[3]));

    // THE MEMORY WARNING, arriving where a host cannot control it: between the
    // stroke and the undo of it.
    const memory::TrimReport report = mesh::trim_surface(s, memory::Pressure::Critical);
    REQUIRE_FALSE(report.pinned);
    REQUIRE(report.total_released > 0);

    REQUIRE(step.revert(s));
    check_same(s, before, "after a trim and an undo");

    // And the redo, across a second trim, so the record is exact in both
    // directions rather than only in the one a test usually takes.
    REQUIRE(mesh::trim_surface(s, memory::Pressure::Critical).total_released >= 0u);
    REQUIRE(step.apply(s));
    check_same(s, after, "after a trim and a redo");
}

TEST_CASE("undo exactness: the step survives the round trip through bytes, trim or no trim") {
    // Two hierarchies that took the same stroke. One is undone with the record
    // in memory, the other with the record after a save and a reload — which is
    // what a crash journal replays.
    MultiresSurface live = build(6, 3);
    MultiresSurface reloaded = build(6, 3);
    warm_every_level(live);
    warm_every_level(reloaded);
    const Snapshot before = snapshot(live);

    MultiresDelta step;
    REQUIRE(sculpt_stroke(live, 2, &step) > 0);
    MultiresDelta same_step;
    REQUIRE(sculpt_stroke(reloaded, 2, &same_step) > 0);

    const std::vector<std::uint8_t> bytes = step.encode();
    REQUIRE_FALSE(bytes.empty());
    MultiresDelta decoded;
    REQUIRE(MultiresDelta::decode(bytes.data(), bytes.size(), &decoded));
    CHECK(decoded.size() == step.size());
    CHECK(decoded.levels() == step.levels());

    // A TRUNCATED RECORD IS REFUSED RATHER THAN HALF-APPLIED. A journal is read
    // after a crash, which is exactly when the last record is short.
    for (std::size_t drop : {std::size_t(1), bytes.size() / 2, bytes.size() - 1}) {
        MultiresDelta partial;
        CAPTURE(drop);
        CHECK_FALSE(MultiresDelta::decode(bytes.data(), bytes.size() - drop, &partial));
    }
    MultiresDelta from_nothing;
    CHECK_FALSE(MultiresDelta::decode(nullptr, 0, &from_nothing));

    REQUIRE(step.revert(live));
    REQUIRE(decoded.revert(reloaded));
    for (std::uint32_t l = 0; l < live.level_count(); ++l) {
        CAPTURE(l);
        CHECK(identical(live.positions_at(l), reloaded.positions_at(l)));
    }
    check_same(reloaded, before, "undone from a decoded record");
}

TEST_CASE("serialization round-trip: a critical trim does not change one byte of what is saved") {
    MultiresSurface s = build(6, 3);
    MultiresDelta step;
    REQUIRE(sculpt_stroke(s, 3, &step) > 0);
    warm_every_level(s);
    const Snapshot before = snapshot(s);

    const std::vector<std::uint8_t> warm = s.encode();
    REQUIRE_FALSE(warm.empty());

    const memory::TrimReport report = mesh::trim_surface(s, memory::Pressure::Critical);
    REQUIRE(report.total_released > 0);

    // THE FILE IS A FUNCTION OF THE DOCUMENT AND NOTHING ELSE. Byte equality
    // rather than "it reloads": a rebuildable thing that had quietly become
    // part of the encoding would still reload, and would still make the file
    // depend on whether a memory warning arrived before the save.
    const std::vector<std::uint8_t> cold = s.encode();
    CHECK(warm.size() == cold.size());
    CHECK(warm == cold);

    MultiresSurface out = build(2, 1);
    REQUIRE(MultiresSurface::decode(cold.data(), cold.size(), &out));
    REQUIRE(out.level_count() == s.level_count());
    warm_every_level(out);
    check_same(out, before, "decoded from the bytes written after a trim");
}

TEST_CASE("determinism: a trim between every dab does not change what the stroke commits") {
    MultiresSurface calm = build(6, 3);
    MultiresSurface pressed = build(6, 3);
    warm_every_level(calm);
    warm_every_level(pressed);

    MultiresDelta calm_step, pressed_step;
    const std::size_t calm_moved = sculpt_stroke(calm, 2, &calm_step);
    // The same stroke, with the operating system releasing every rebuildable
    // cache between one dab and the next.
    const std::size_t pressed_moved = sculpt_stroke(pressed, 2, &pressed_step, [&] {
        mesh::trim_surface(pressed, memory::Pressure::Critical);
    });

    REQUIRE(calm_moved > 0);
    // The same weld classes moved: a stroke that reached a different set under
    // pressure would still produce a plausible surface.
    CHECK(pressed_moved == calm_moved);
    CHECK(pressed_step.size() == calm_step.size());
    CHECK(pressed_step.levels() == calm_step.levels());

    warm_every_level(calm);
    warm_every_level(pressed);
    check_same(pressed, snapshot(calm), "a stroke taken under memory pressure");
}

TEST_CASE("regression: a trim between two dabs does not eat the next dab") {
    // THE DEFECT, stated as what it did rather than as where it was:
    // `cache_generation` was bumped when a level's cache was BUILT and not when
    // it was released, so between a `drop_*_caches` and the next build the
    // number had not moved. `MultiresSculptor::bind` compares exactly that
    // number to decide whether the `Mesh&` its `MeshSculptor` holds still
    // points into live storage, so it kept the stale one — a reference into a
    // freed `LevelCache`.
    //
    // It did not crash. The stamp wrote into released storage,
    // `absorb_level_edit` rebuilt the level from the authoritative detail
    // before reading the displacement back out of it, and the dab was simply
    // not there. The sculptor returned the weld-class count it believed it had
    // moved, so nothing above it could tell. With a trim after every dab the
    // symptom was every SECOND dab vanishing, because the dab after a rebind
    // paid for the rebuild and the one after that found the generation
    // unmoved again.
    //
    // The assertion is therefore on the DETAIL CHECKSUM after each dab, which
    // is what "the dab landed" means for a hierarchy, and not on the count the
    // sculptor reports — reading that count was how the defect stayed hidden.
    MultiresSurface s = build(6, 3);
    REQUIRE(s.set_sculpt_level(2));
    MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    MeshBrushSettings settings;
    settings.radius = 0.35f;
    settings.strength = 0.5f;

    std::uint64_t previous = s.detail_checksum();
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        settings.center = cf3(-0.3f + 0.2f * static_cast<float>(i), 0.0f, 0.0f);
        const std::size_t moved = sculptor.stamp(MeshBrush::Draw, settings);
        REQUIRE(moved > 0);
        const std::uint64_t now = s.detail_checksum();
        CHECK(now != previous);
        previous = now;
        // The memory warning, arriving between two dabs of one stroke, which
        // is where a host has no say in the matter.
        mesh::trim_surface(s, memory::Pressure::Critical);
        // And it stays landed across the release itself.
        CHECK(s.detail_checksum() == previous);
    }
}

TEST_CASE("chunk transport: the level a host is DRAWING sees a dab made at a coarser level") {
    // The ordinary multiresolution workflow: sculpt low, display high. The
    // display level's chunks are marked by the PROPAGATION rather than by the
    // edit, and a host that drains the level it is drawing is told about the
    // dab through that path alone.
    MultiresSurface s = build(10, 3);
    const std::uint32_t display = s.max_level(), sculpt = 1;

    Reconstruction host;
    {
        SurfaceView view = SurfaceView::over_level(s, display);
        REQUIRE(view.valid());
        host.upload_all(view);
    }
    const Mesh whole_before = s.mesh_at_level(display);
    REQUIRE_FALSE(whole_before.indices.empty());
    REQUIRE(host.surface() == triangles_of(whole_before));
    s.clear_dirty_chunks(display);

    MultiresDelta step;
    REQUIRE(sculpt_stroke(s, sculpt, &step) > 0);

    std::size_t drained = 0;
    std::size_t live = 0;
    {
        SurfaceView view = SurfaceView::over_level(s, display);
        live = view.chunks().live_count();
        drained = host.drain(view);
    }
    CAPTURE(drained);
    CAPTURE(live);
    // THE STREAM SAID SOMETHING. A silent stream is the failure this case
    // exists for, and it leaves the viewport showing the surface from before
    // the dab with nothing on screen to say so.
    REQUIRE(drained > 0);
    // And it said it LOCALLY: a coarse dab reaches further at a fine level than
    // a fine one does, but it is still a footprint and not the model.
    CHECK(drained < live);

    const Mesh whole_after = s.mesh_at_level(display);
    CHECK(triangles_of(whole_after) != triangles_of(whole_before));
    CHECK(host.surface() == triangles_of(whole_after));
}
