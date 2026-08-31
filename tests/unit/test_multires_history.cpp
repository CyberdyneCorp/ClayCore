// A multiresolution gesture in the ONE undo history (scene-model spec,
// add-mesh-multires).
//
// The library makes its undo promise in the singular — one history over every
// representation — and every representation added is a chance for that to stop
// being true. The cases here are the four that would break it: a gesture
// undoing on its own, one bracketed with a scene command undoing as ONE step, a
// journal from before this kind existed still replaying, and the step's size
// following the vertices EDITED rather than the depth of the hierarchy above
// them.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/multires_sculpt.h"
#include "clay/scene/document.h"
#include "clay/session/history.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresDelta;
using mesh::MultiresError;
using mesh::MultiresSculptor;
using mesh::MultiresSurface;
using session::History;

namespace {

Mesh plane_quads(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

MultiresSurface build(int n, std::uint32_t levels) {
    MultiresError err = MultiresError::None;
    auto surface = MultiresSurface::from_mesh(plane_quads(n, 2.0f), {}, &err);
    REQUIRE_MESSAGE(surface.has_value(), mesh::multires_error_text(err));
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level(&err));
    return std::move(*surface);
}

// One stroke at `level`, several stamps, coalesced into one record.
MultiresDelta sculpt_stroke(MultiresSurface& s, std::uint32_t level) {
    REQUIRE(s.set_sculpt_level(level));
    MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    MultiresDelta record;
    MeshBrushSettings settings;
    settings.radius = 0.7f;
    settings.strength = 0.5f;
    for (int i = 0; i < 4; ++i) {
        settings.center = cf3(-0.3f + 0.2f * static_cast<float>(i), 0.0f, 0.0f);
        sculptor.stamp(MeshBrush::Draw, settings, {}, &record);
    }
    return record;
}

bool same_positions(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

}  // namespace

TEST_CASE("multires history: a gesture at any level is one undoable step") {
    for (std::uint32_t level : {0u, 1u, 3u}) {
        MultiresSurface s = build(4, 3);
        s.positions_at(3);
        const std::vector<cfloat3> before = s.positions_at(3);
        const std::uint64_t sum_before = s.detail_checksum();

        scene::Document doc;
        History history;
        history.set_enabled(true);
        history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });

        history.record_multires_step(1, sculpt_stroke(s, level));
        const std::vector<cfloat3> after = s.positions_at(3);
        CHECK_FALSE(same_positions(before, after));
        // ONE step for the whole stroke, however many stamps it took.
        CHECK(history.step_count() == 1);

        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        CHECK(same_positions(s.positions_at(3), before));
        CHECK(s.detail_checksum() == sum_before);

        REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
        CHECK(same_positions(s.positions_at(3), after));
    }
}

TEST_CASE("multires history: a missing surface refuses rather than skipping") {
    MultiresSurface s = build(4, 2);
    scene::Document doc;
    History history;
    history.set_enabled(true);
    // No resolver at all.
    history.record_multires_step(1, sculpt_stroke(s, 2));
    REQUIRE(history.step_count() == 1);
    CHECK_FALSE(history.undo(doc, nullptr, nullptr, nullptr));
    // ...and the step is still there to try again once the surface is back.
    CHECK(history.step_count() == 1);
}

TEST_CASE("multires history: a scene command and a multires gesture undo as one") {
    MultiresSurface s = build(4, 2);
    const std::vector<cfloat3> before = s.positions_at(2);

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });

    const std::size_t layers_before = doc.layers.size();
    history.begin_group();
    scene::Layer added;
    added.name = "multires";
    REQUIRE(history.perform(doc, scene::AddLayerCmd{added, -1}));
    history.record_multires_step(1, sculpt_stroke(s, 2));
    history.end_group();

    CHECK(doc.layers.size() == layers_before + 1);
    CHECK_FALSE(same_positions(s.positions_at(2), before));
    CHECK(history.step_count() == 1);

    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(doc.layers.size() == layers_before);
    CHECK(same_positions(s.positions_at(2), before));
}

TEST_CASE("multires history: the journal round-trips, and an older one still replays") {
    MultiresSurface s = build(4, 2);
    const std::vector<cfloat3> before = s.positions_at(2);

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });
    history.record_multires_step(1, sculpt_stroke(s, 2));
    const std::vector<cfloat3> after = s.positions_at(2);

    std::size_t now_at = 0;
    const std::vector<std::uint8_t> journal = history.journal_since(0, &now_at);
    REQUIRE(!journal.empty());

    MultiresSurface replayed = build(4, 2);
    CHECK(same_positions(replayed.positions_at(2), before));

    scene::Document replay_doc;
    History replay_history;
    replay_history.set_enabled(true);
    replay_history.set_multires_resolver(
        [&](scene::LayerId) -> MultiresSurface* { return &replayed; });
    History::ReplayResult result;
    REQUIRE(replay_history.replay(journal.data(), journal.size(), replay_doc, nullptr, nullptr,
                                  &result));
    CHECK(same_positions(replayed.positions_at(2), after));

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
    History::ReplayResult old_result;
    REQUIRE(into_history.replay(old_journal.data(), old_journal.size(), into, nullptr, nullptr,
                                &old_result));
    CHECK(into.layers.size() == old_doc.layers.size());
}

TEST_CASE("multires history: the step's size follows the edit, not the depth") {
    // THE PROPERTY the kind exists for. A coarse stroke on a deeper hierarchy
    // moves many more derived vertices and must record exactly as many bytes.
    std::size_t undo_bytes[2] = {0, 0};
    std::size_t fine_vertices[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        MultiresSurface s = build(4, 2 + static_cast<std::uint32_t>(i) * 2);
        History history;
        history.set_enabled(true);
        history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });
        const History::Bytes empty = history.bytes();
        history.record_multires_step(1, sculpt_stroke(s, 0));
        const History::Bytes filled = history.bytes();
        undo_bytes[i] = filled.undo - empty.undo;
        fine_vertices[i] = s.positions_at(s.max_level()).size();
        CHECK(filled.journal > empty.journal);
    }
    CHECK(fine_vertices[1] > fine_vertices[0] * 10);
    CHECK(undo_bytes[0] == undo_bytes[1]);
    CHECK(undo_bytes[0] > 0);
}

TEST_CASE("multires history: a record decodes back to the same gesture") {
    MultiresSurface s = build(4, 2);
    const std::vector<cfloat3> before = s.positions_at(2);
    const MultiresDelta record = sculpt_stroke(s, 2);
    const std::vector<cfloat3> after = s.positions_at(2);

    const std::vector<std::uint8_t> bytes = record.encode();
    CHECK(record.encode() == bytes);

    MultiresDelta back;
    REQUIRE(MultiresDelta::decode(bytes.data(), bytes.size(), &back));
    CHECK(back.size() == record.size());
    CHECK(back.levels() == record.levels());

    REQUIRE(back.revert(s));
    CHECK(same_positions(s.positions_at(2), before));
    REQUIRE(back.apply(s));
    CHECK(same_positions(s.positions_at(2), after));

    MultiresDelta out;
    CHECK_FALSE(MultiresDelta::decode(nullptr, 0, &out));
    CHECK_FALSE(MultiresDelta::decode(bytes.data(), 4, &out));
    for (std::size_t cut = 8; cut < bytes.size(); cut += 17)
        CHECK_FALSE(MultiresDelta::decode(bytes.data(), cut, &out));
    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] ^= 0xFF;
    CHECK_FALSE(MultiresDelta::decode(wrong.data(), wrong.size(), &out));
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 77;
    CHECK_FALSE(MultiresDelta::decode(newer.data(), newer.size(), &out));
    // A count larger than the buffer could hold, refused before allocating.
    std::vector<std::uint8_t> hostile = bytes;
    hostile[8] = 0xff;
    hostile[9] = 0xff;
    hostile[10] = 0xff;
    hostile[11] = 0x0f;
    CHECK_FALSE(MultiresDelta::decode(hostile.data(), hostile.size(), &out));
}

TEST_CASE("multires history: a record refuses a surface it does not describe") {
    MultiresSurface deep = build(4, 3);
    const MultiresDelta record = sculpt_stroke(deep, 3);
    CHECK_FALSE(record.empty());

    // A shallower hierarchy has no level 3, and a record that applied anyway
    // would be writing coefficients at a level that does not exist.
    MultiresSurface shallow = build(4, 1);
    CHECK_FALSE(record.revert(shallow));
    CHECK_FALSE(record.apply(shallow));
}
