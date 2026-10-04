// An adaptive gesture in the one undo history (dynamic-topology spec,
// add-dynamic-topology).
//
// The library makes its undo promise in the SINGULAR — one history over every
// representation — and every representation added is a chance for that to stop
// being true. The cases here are the three that would break it: a topology
// gesture undoing on its own, one bracketed with a scene command undoing as ONE
// step, and a journal from before this kind existed still replaying. A fourth
// was added with #629: a sculptor held across the history keeps its index in
// step through undo, redo and journal replay.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/scene/document.h"
#include "clay/session/history.h"
#include "dynamic_index_audit.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;
using session::History;
using session::Step;

namespace {

Mesh cube_sphere(int n, float radius) {
    Mesh m;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const cfloat3 p = cf3(c[0], c[1], c[2]);
                const cfloat3 unit = p / clength(p);
                m.positions.push_back(unit * radius);
                m.normals.push_back(unit);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

// One adaptive stroke, recorded into a delta.
mesh::TopologyDelta sculpt_stroke(DynamicSurface& surface, std::size_t* out_split) {
    DynamicSculptor sculptor(surface);
    mesh::TopologyDelta record;
    mesh::MeshBrushSettings s;
    s.radius = 0.4f;
    s.strength = 0.4f;
    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 5.0f;
    std::size_t split = 0;
    for (int i = 0; i < 4; ++i) {
        s.center = cf3(-0.2f + 0.12f * static_cast<float>(i), 0.0f, 0.95f);
        split += sculptor.stamp(mesh::MeshBrush::Draw, s, topo, {}, &record).remesh.split;
    }
    if (out_split) *out_split = split;
    return record;
}

bool same_mesh(const Mesh& a, const Mesh& b) {
    if (a.positions.size() != b.positions.size() || a.indices.size() != b.indices.size())
        return false;
    for (std::size_t i = 0; i < a.positions.size(); ++i)
        if (a.positions[i].x != b.positions[i].x || a.positions[i].y != b.positions[i].y ||
            a.positions[i].z != b.positions[i].z)
            return false;
    for (std::size_t i = 0; i < a.indices.size(); ++i)
        if (a.indices[i] != b.indices[i]) return false;
    return true;
}

}  // namespace

TEST_CASE("dynamic history: an adaptive gesture is one undoable step") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*surface; });

    std::size_t split = 0;
    history.record_dynamic_mesh_step(1, sculpt_stroke(*surface, &split));
    REQUIRE(split > 0);
    const Mesh after = surface->to_mesh();
    CHECK_FALSE(same_mesh(before, after));

    // ONE step for the whole stroke, however many stamps and operations it took.
    CHECK(history.step_count() == 1);

    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), before));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);

    REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), after));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("dynamic history: a missing surface refuses rather than skipping") {
    // The rule every other kind follows: skipping would take the step off the
    // stack and leave the next undo reversing something older than the user
    // asked for.
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());
    scene::Document doc;
    History history;
    history.set_enabled(true);
    // No resolver at all.
    history.record_dynamic_mesh_step(1, sculpt_stroke(*surface, nullptr));
    REQUIRE(history.step_count() == 1);
    CHECK_FALSE(history.undo(doc, nullptr, nullptr, nullptr));
    // ...and the step is still there to try again once the surface is back.
    CHECK(history.step_count() == 1);
}

TEST_CASE("dynamic history: a scene command and a topology delta undo as one") {
    // THE CROSSING. A bracket that collected a scene command and an adaptive
    // gesture has to undo both together, or the layer goes and the sculpt it
    // contained comes back one press later.
    auto surface = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*surface; });

    const std::size_t layers_before = doc.layers.size();
    history.begin_group();
    scene::Layer added;
    added.name = "adaptive";
    REQUIRE(history.perform(doc, scene::AddLayerCmd{added, -1}));
    history.record_dynamic_mesh_step(1, sculpt_stroke(*surface, nullptr));
    history.end_group();

    CHECK(doc.layers.size() == layers_before + 1);
    CHECK_FALSE(same_mesh(surface->to_mesh(), before));
    // ONE step, not two.
    CHECK(history.step_count() == 1);

    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(doc.layers.size() == layers_before);
    CHECK(same_mesh(surface->to_mesh(), before));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("dynamic history: the journal round-trips, and an older one still replays") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*surface; });
    history.record_dynamic_mesh_step(1, sculpt_stroke(*surface, nullptr));
    const Mesh after = surface->to_mesh();

    std::size_t now_at = 0;
    const std::vector<std::uint8_t> journal = history.journal_since(0, &now_at);
    REQUIRE(!journal.empty());

    // Replay onto a fresh surface: the recovery has to reproduce the sculpt,
    // connectivity included, or a crash costs the artist the stroke.
    auto replayed = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(replayed.has_value());
    CHECK(same_mesh(replayed->to_mesh(), before));

    scene::Document replay_doc;
    History replay_history;
    replay_history.set_enabled(true);
    replay_history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*replayed; });
    session::History::ReplayResult result;
    REQUIRE(replay_history.replay(journal.data(), journal.size(), replay_doc, nullptr, nullptr,
                                  &result));
    CHECK(same_mesh(replayed->to_mesh(), after));
    CHECK(mesh::validate_dynamic_surface(*replayed).ok);

    // AN OLDER JOURNAL STILL REPLAYS. The new event kind is APPENDED to the
    // enumeration, so every value an older writer used still means what it
    // meant; a kind inserted in the middle would silently reinterpret every
    // event in every file already on disk.
    scene::Document old_doc;
    History old_history;
    old_history.set_enabled(true);
    scene::Layer plain;
    plain.name = "plain";
    REQUIRE(old_history.perform(old_doc, scene::AddLayerCmd{plain, -1}));
    std::size_t old_at = 0;
    const std::vector<std::uint8_t> old_journal = old_history.journal_since(0, &old_at);

    scene::Document into;
    History into_history;
    into_history.set_enabled(true);
    session::History::ReplayResult old_result;
    REQUIRE(into_history.replay(old_journal.data(), old_journal.size(), into, nullptr, nullptr,
                                &old_result));
    CHECK(into.layers.size() == old_doc.layers.size());
}

TEST_CASE("dynamic history: the step's payload is in the memory report") {
    // A budget blind to the largest payload a step can carry would evict
    // everything except the thing filling memory.
    auto surface = DynamicSurface::from_mesh(cube_sphere(5, 1.0f));
    REQUIRE(surface.has_value());
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*surface; });

    const History::Bytes empty = history.bytes();
    history.record_dynamic_mesh_step(1, sculpt_stroke(*surface, nullptr));
    const History::Bytes filled = history.bytes();
    CHECK(filled.undo > empty.undo);
    CHECK(filled.journal > empty.journal);
    // ...and by a real amount rather than the size of an empty Step.
    CHECK(filled.undo - empty.undo > 4096);
}

// -- a held sculptor across the history (#629) ---------------------------------
//
// The cases above hand History a bare surface and drop the sculptor that made
// the stroke. A C++ host does not: it keeps one sculptor per adaptive layer for
// the life of the document, and that sculptor's chunked index has to follow
// every undo, redo and journal replay. Before `set_dynamic_sculptor_resolver`
// none of them did, and on this fixture one undo left 23 live faces in no
// chunk and 1,333 dead entries.

namespace {

using clay_test::index_coverage;
using clay_test::IndexCoverage;

// A 16-stamp Draw arc over the north pole -- the stroke the replay tests use,
// so the counts in the issue are this fixture's counts.
mesh::TopologyDelta held_stroke(DynamicSculptor& sculptor) {
    mesh::TopologyDelta record;
    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 8.0f;
    for (int i = 0; i < 16; ++i) {
        mesh::MeshBrushSettings s;
        s.radius = 0.35f;
        s.strength = 0.3f;
        const float t = -0.3f + 0.6f * static_cast<float>(i) / 16.0f;
        s.center = cnormalize(cf3(t, 0.0f, 1.0f));
        sculptor.stamp(mesh::MeshBrush::Draw, s, topo, {}, &record);
    }
    return record;
}

void check_index_in_step(const DynamicSculptor& sculptor) {
    const IndexCoverage cov = index_coverage(sculptor);
    CHECK(cov.live_missing == 0);
    CHECK(cov.dead_indexed == 0);
}

}  // namespace

TEST_CASE("dynamic history: an undo through a held sculptor keeps its index in step") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();
    DynamicSculptor sculptor(*surface);

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver([&](scene::LayerId) -> DynamicSurface* { return &*surface; });
    history.set_dynamic_sculptor_resolver(
        [&](scene::LayerId) -> DynamicSculptor* { return &sculptor; });

    mesh::TopologyDelta record = held_stroke(sculptor);
    REQUIRE(record.face_count() > 0);
    const Mesh first = surface->to_mesh();
    history.record_dynamic_mesh_step(1, std::move(record));

    sculptor.bvh().clear_dirty();
    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), before));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    // EVERY LIVE FACE IS REACHABLE and nothing dead is indexed. A surface-only
    // revert read 23 missing and 1,333 dead here.
    check_index_in_step(sculptor);
    // ...and the host is told which chunks to re-upload.
    CHECK_FALSE(sculptor.bvh().dirty_leaves().empty());

    // The same stroke again, on the SAME sculptor, lands where the first did.
    held_stroke(sculptor);
    CHECK(same_mesh(surface->to_mesh(), first));
    check_index_in_step(sculptor);
}

TEST_CASE("dynamic history: a redo through a held sculptor keeps its index in step") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver([&](scene::LayerId) -> DynamicSurface* { return &*surface; });
    history.set_dynamic_sculptor_resolver(
        [&](scene::LayerId) -> DynamicSculptor* { return &sculptor; });

    history.record_dynamic_mesh_step(1, held_stroke(sculptor));
    const Mesh first = surface->to_mesh();

    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    check_index_in_step(sculptor);
    sculptor.bvh().clear_dirty();
    REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), first));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    check_index_in_step(sculptor);
    CHECK_FALSE(sculptor.bvh().dirty_leaves().empty());
}

TEST_CASE("dynamic history: a journal replayed under a held sculptor leaves its index in step") {
    // Recovery is the same question as undo: a host that rebuilt its sculptor
    // before replaying must not have to call `rebuild_index` to find out the
    // replay moved its surface.
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    History history;
    history.set_enabled(true);
    history.record_dynamic_mesh_step(1, held_stroke(sculptor));
    std::size_t now_at = 0;
    const std::vector<std::uint8_t> journal = history.journal_since(0, &now_at);
    REQUIRE(!journal.empty());

    auto replayed = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(replayed.has_value());
    DynamicSculptor held(*replayed);
    scene::Document replay_doc;
    History replay_history;
    replay_history.set_enabled(true);
    replay_history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*replayed; });
    replay_history.set_dynamic_sculptor_resolver(
        [&](scene::LayerId) -> DynamicSculptor* { return &held; });
    session::History::ReplayResult result;
    REQUIRE(replay_history.replay(journal.data(), journal.size(), replay_doc, nullptr, nullptr,
                                  &result));
    CHECK(same_mesh(replayed->to_mesh(), surface->to_mesh()));
    CHECK(mesh::validate_dynamic_surface(*replayed).ok);
    check_index_in_step(held);

    // The next stroke on the held sculptor lands where one on a freshly indexed
    // copy of the recovered surface does. Compared against that, not against
    // the original session: a replay rebuilds the pools' free lists rather than
    // restoring them (see `restore_kind`), so a recovered surface hands out
    // different slots and a stroke on it is not bit-identical to the original
    // session's -- with or without an index, measured.
    auto reference = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(reference.has_value());
    History reference_history;
    reference_history.set_enabled(true);
    reference_history.set_dynamic_resolver(
        [&](scene::LayerId) -> DynamicSurface* { return &*reference; });
    scene::Document reference_doc;
    REQUIRE(reference_history.replay(journal.data(), journal.size(), reference_doc, nullptr,
                                     nullptr, &result));
    DynamicSculptor fresh(*reference);
    held_stroke(fresh);
    held_stroke(held);
    CHECK(same_mesh(replayed->to_mesh(), reference->to_mesh()));
    check_index_in_step(held);
}

TEST_CASE("dynamic history: with no sculptor for the layer, the surface path still undoes") {
    // A sculptor resolver that does not know the layer falls through to the
    // surface resolver, exactly as a host that never set one does. Keeping the
    // index is then the host's job, and `rebuild_index` is how it does it.
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();
    DynamicSculptor sculptor(*surface);

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_resolver([&](scene::LayerId) -> DynamicSurface* { return &*surface; });
    history.set_dynamic_sculptor_resolver(
        [](scene::LayerId) -> DynamicSculptor* { return nullptr; });

    history.record_dynamic_mesh_step(1, held_stroke(sculptor));
    const Mesh first = surface->to_mesh();
    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), before));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    sculptor.rebuild_index();
    check_index_in_step(sculptor);
    REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), first));
}

TEST_CASE("dynamic history: the sculptor resolver alone is enough") {
    // The sculptor names its surface, so a host holding one per layer does not
    // have to register the surface a second time.
    auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    REQUIRE(surface.has_value());
    const Mesh before = surface->to_mesh();
    DynamicSculptor sculptor(*surface);

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_dynamic_sculptor_resolver(
        [&](scene::LayerId) -> DynamicSculptor* { return &sculptor; });

    history.record_dynamic_mesh_step(1, held_stroke(sculptor));
    const Mesh first = surface->to_mesh();
    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), before));
    check_index_in_step(sculptor);
    REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
    CHECK(same_mesh(surface->to_mesh(), first));
    check_index_in_step(sculptor);
}
