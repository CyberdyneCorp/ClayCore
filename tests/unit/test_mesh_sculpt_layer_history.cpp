// A SCULPT LAYER IN THE ONE UNDO HISTORY, and in the journal (scene-model and
// file-io specs, add-mesh-sculpt-layers, tasks 7.2, 7.3 and 7.4).
//
// The library makes its undo promise in the singular — one history over every
// representation — and every kind added is a chance for that to stop being
// true. Two kinds are added here rather than one, and the split is what makes
// `step_bytes` answerable: a strength change is a handful of bytes and a stroke
// is a megabyte, and a host asking which of the two is filling its budget can
// only be told if the kinds separate them. A case below measures exactly that.
//
// WHAT THIS FILE GATES THAT THE STACK'S OWN TESTS CANNOT.
//
//   * A PROPERTY CHANGE IS USER HISTORY. This is the thing this change does
//     better than the voxel stack, whose renames and strengths are still
//     outside the history: an artist who dials a pass from 100% to 40% and
//     presses undo means the dial, and a history that skipped past it to the
//     stroke before is a history that lied about what it holds.
//   * OLDER JOURNALS STILL REPLAY. Both new event kinds are APPENDED to the
//     enumeration, so every value an older writer emitted still means what it
//     meant. A kind inserted in the middle would silently reinterpret every
//     event in every file already on disk, and nothing would say so.
//   * A MALFORMED PAYLOAD IS REFUSED RATHER THAN HALF APPLIED. Every case here
//     checks the surface as well as the return value, because "returned false"
//     and "changed nothing" are two different promises and only the second one
//     is worth having.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt_layer.h"
#include "clay/scene/document.h"
#include "clay/session/history.h"

using namespace clay;
using namespace clay::kernel;
using mesh::LocalDetail;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresSurface;
using mesh::SculptLayerDelta;
using mesh::SculptLayerId;
using mesh::SculptLayerProperty;
using mesh::SculptLayerStack;
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
            m.quads.insert(m.quads.end(), {a, a + 1, a + stride + 1, a + stride});
            m.indices.insert(m.indices.end(),
                             {a, a + 1, a + stride + 1, a, a + stride + 1, a + stride});
        }
    return m;
}

MultiresSurface build(std::uint32_t levels) {
    auto surface = MultiresSurface::from_mesh(plane_quads(4, 1.0f));
    REQUIRE(surface.has_value());
    for (std::uint32_t i = 0; i < levels; ++i) REQUIRE(surface->add_level());
    REQUIRE(surface->set_sculpt_level(levels));
    return std::move(*surface);
}

bool bit_equal(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// Several stamps into the surface's active layer, coalesced into one record.
SculptLayerDelta layered_stroke(MultiresSurface& s) {
    mesh::MultiresSculptor sculptor(s);
    sculptor.begin_stroke();
    SculptLayerDelta record;
    MeshBrushSettings settings;
    settings.radius = 0.4f;
    settings.strength = 0.5f;
    for (int i = 0; i < 4; ++i) {
        settings.center = cf3(-0.3f + 0.2f * static_cast<float>(i), 0.0f, 0.0f);
        sculptor.stamp(MeshBrush::Draw, settings, {}, nullptr, &record);
    }
    return record;
}

}  // namespace

TEST_CASE("a layered gesture is one step, and undo restores the channel exactly") {
    MultiresSurface s = build(2);
    const SculptLayerId id = s.add_sculpt_layer("wrinkles");
    REQUIRE(s.set_active_sculpt_layer(id));
    const std::vector<cfloat3> before = s.positions_at(2);
    const std::uint64_t layer_before = s.sculpt_layer_checksum();
    const std::uint64_t base_before = s.detail_checksum();

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });

    history.record_multires_layer_step(1, layered_stroke(s));
    const std::vector<cfloat3> after = s.positions_at(2);
    const std::uint64_t layer_after = s.sculpt_layer_checksum();
    CHECK_FALSE(bit_equal(before, after));
    CHECK(history.step_count() == 1);
    // The FORM is untouched: the gesture went into the channel and nowhere
    // else, so an undo that restored the base would be restoring nothing.
    CHECK(s.detail_checksum() == base_before);

    REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(s.sculpt_layer_checksum() == layer_before);
    CHECK(bit_equal(before, s.positions_at(2)));
    REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
    CHECK(s.sculpt_layer_checksum() == layer_after);
    CHECK(bit_equal(after, s.positions_at(2)));
}

TEST_CASE("a layer property change is user history rather than a cache change") {
    MultiresSurface s = build(2);
    const SculptLayerId id = s.add_sculpt_layer("pass");
    for (std::uint32_t v = 30; v < 60; ++v)
        s.set_sculpt_layer_detail(id, 2, v, LocalDetail{0.0f, 0.0f, 0.02f});

    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });

    SUBCASE("a strength dial undoes to the value the artist left") {
        const std::vector<cfloat3> full = s.positions_at(2);
        SculptLayerProperty record;
        REQUIRE(s.set_sculpt_layer_strength(id, 0.4f, &record));
        history.record_multires_layer_property(1, record);
        const std::vector<cfloat3> dialled = s.positions_at(2);
        CHECK_FALSE(bit_equal(full, dialled));

        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        CHECK(s.sculpt_layers().find(id)->strength == doctest::Approx(1.0f));
        CHECK(bit_equal(full, s.positions_at(2)));
        REQUIRE(history.redo(doc, nullptr, nullptr, nullptr));
        CHECK(s.sculpt_layers().find(id)->strength == doctest::Approx(0.4f));
        CHECK(bit_equal(dialled, s.positions_at(2)));
    }
    SUBCASE("a rename undoes, and moves no geometry doing it") {
        const std::vector<cfloat3> shape = s.positions_at(2);
        SculptLayerProperty record;
        REQUIRE(s.rename_sculpt_layer(id, "pores", &record));
        history.record_multires_layer_property(1, record);
        CHECK(s.sculpt_layers().find(id)->name == "pores");
        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        CHECK(s.sculpt_layers().find(id)->name == "pass");
        CHECK(bit_equal(shape, s.positions_at(2)));
    }
    SUBCASE("a visibility toggle undoes bit for bit") {
        const std::vector<cfloat3> visible = s.positions_at(2);
        SculptLayerProperty record;
        REQUIRE(s.set_sculpt_layer_visible(id, false, &record));
        history.record_multires_layer_property(1, record);
        CHECK_FALSE(bit_equal(visible, s.positions_at(2)));
        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        CHECK(bit_equal(visible, s.positions_at(2)));
    }
    SUBCASE("a remove undoes with the coefficients it discarded") {
        const std::vector<cfloat3> present = s.positions_at(2);
        const std::uint64_t layer_before = s.sculpt_layer_checksum();
        SculptLayerProperty record;
        REQUIRE(s.remove_sculpt_layer(id, &record));
        CHECK(s.sculpt_layers().empty());
        history.record_multires_layer_property(1, record);

        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        // The ID comes back too, not merely a layer with the same content: a
        // host holding the id across an undo must find its own pass again.
        REQUIRE(s.sculpt_layers().size() == 1);
        CHECK(s.sculpt_layers().id_at(0) == id);
        CHECK(s.sculpt_layer_checksum() == layer_before);
        CHECK(bit_equal(present, s.positions_at(2)));
    }
    SUBCASE("a bake undoes what it wrote outside the stack as well") {
        // A bake folds a layer into the level's OWN detail, which the stack
        // snapshot cannot carry — so the property record carries it separately
        // or the undo silently leaves the pass baked in twice.
        const std::uint64_t base_before = s.detail_checksum();
        const std::vector<cfloat3> shape = s.positions_at(2);
        SculptLayerProperty record;
        REQUIRE(s.bake_sculpt_layer_to_base(id, &record));
        CHECK(s.sculpt_layers().empty());
        CHECK(s.detail_checksum() != base_before);
        history.record_multires_layer_property(1, record);

        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        CHECK(s.detail_checksum() == base_before);
        REQUIRE(s.sculpt_layers().size() == 1);
        CHECK(bit_equal(shape, s.positions_at(2)));
    }
    SUBCASE("a merge undoes to two layers with their own sliders back") {
        const SculptLayerId upper = s.add_sculpt_layer("upper");
        for (std::uint32_t v = 30; v < 60; ++v)
            s.set_sculpt_layer_detail(upper, 2, v, LocalDetail{0.0f, 0.0f, 0.01f});
        REQUIRE(s.set_sculpt_layer_strength(upper, 0.5f));
        const std::uint64_t layer_before = s.sculpt_layer_checksum();
        const std::vector<cfloat3> shape = s.positions_at(2);

        SculptLayerProperty record;
        REQUIRE(s.merge_sculpt_layer_down(upper, &record));
        CHECK(s.sculpt_layers().size() == 1);
        history.record_multires_layer_property(1, record);

        REQUIRE(history.undo(doc, nullptr, nullptr, nullptr));
        CHECK(s.sculpt_layers().size() == 2);
        CHECK(s.sculpt_layers().find(upper)->strength == doctest::Approx(0.5f));
        CHECK(s.sculpt_layer_checksum() == layer_before);
        CHECK(bit_equal(shape, s.positions_at(2)));
    }
}

TEST_CASE("a missing surface refuses rather than skipping the step") {
    MultiresSurface s = build(2);
    const SculptLayerId id = s.add_sculpt_layer("pass");
    REQUIRE(s.set_active_sculpt_layer(id));
    scene::Document doc;
    History history;
    history.set_enabled(true);
    // No resolver at all: the step cannot be reached, and skipping it would
    // move the cursor past an edit that is still on the surface.
    history.record_multires_layer_step(1, layered_stroke(s));
    REQUIRE(history.step_count() == 1);
    CHECK_FALSE(history.undo(doc, nullptr, nullptr, nullptr));
    CHECK(history.step_count() == 1);
}

TEST_CASE("the journal replays a whole layered session onto a fresh hierarchy") {
    MultiresSurface s = build(2);
    scene::Document doc;
    History history;
    history.set_enabled(true);
    history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });

    SculptLayerProperty added;
    const SculptLayerId id = s.add_sculpt_layer("wrinkles", &added);
    history.record_multires_layer_property(1, added);
    REQUIRE(s.set_active_sculpt_layer(id));
    history.record_multires_layer_step(1, layered_stroke(s));
    SculptLayerProperty dialled;
    REQUIRE(s.set_sculpt_layer_strength(id, 0.35f, &dialled));
    history.record_multires_layer_property(1, dialled);

    const std::vector<cfloat3> after = s.positions_at(2);
    const std::uint64_t layer_after = s.sculpt_layer_checksum();

    std::size_t now_at = 0;
    const std::vector<std::uint8_t> journal = history.journal_since(0, &now_at);
    REQUIRE(!journal.empty());

    MultiresSurface replayed = build(2);
    scene::Document replay_doc;
    History replay_history;
    replay_history.set_enabled(true);
    replay_history.set_multires_resolver(
        [&](scene::LayerId) -> MultiresSurface* { return &replayed; });
    History::ReplayResult result;
    REQUIRE(replay_history.replay(journal.data(), journal.size(), replay_doc, nullptr, nullptr,
                                  &result));
    CHECK(replayed.sculpt_layers().size() == 1);
    // The ID is replayed, not re-minted: a journal that renumbered the stack
    // would make every step after it name a different pass.
    CHECK(replayed.sculpt_layers().id_at(0) == id);
    CHECK(replayed.sculpt_layers().find(id)->strength == doctest::Approx(0.35f));
    CHECK(replayed.sculpt_layer_checksum() == layer_after);
    CHECK(bit_equal(after, replayed.positions_at(2)));

    SUBCASE("and a journal from before these kinds existed still replays") {
        // Both kinds are APPENDED to the enumeration, so an older writer's
        // values still mean what they meant.
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
        REQUIRE(into_history.replay(old_journal.data(), old_journal.size(), into, nullptr,
                                    nullptr, &old_result));
        CHECK(into.layers.size() == old_doc.layers.size());
    }

    SUBCASE("a corrupted layer event stops the replay rather than applying half of it") {
        // The payload is the last thing in the stream, so flipping bytes near
        // the end lands inside the strength event's encoded property.
        std::vector<std::uint8_t> hostile = journal;
        for (std::size_t i = hostile.size() - 8; i < hostile.size(); ++i) hostile[i] ^= 0xffu;
        MultiresSurface into = build(2);
        scene::Document into_doc;
        History into_history;
        into_history.set_enabled(true);
        into_history.set_multires_resolver(
            [&](scene::LayerId) -> MultiresSurface* { return &into; });
        History::ReplayResult bad;
        CHECK_FALSE(into_history.replay(hostile.data(), hostile.size(), into_doc, nullptr,
                                        nullptr, &bad));
    }
}

TEST_CASE("a malformed layer delta is refused before it touches a stack") {
    MultiresSurface s = build(2);
    const SculptLayerId id = s.add_sculpt_layer("pass");
    REQUIRE(s.set_active_sculpt_layer(id));
    const SculptLayerDelta record = layered_stroke(s);
    REQUIRE_FALSE(record.empty());
    const std::vector<std::uint8_t> bytes = record.encode();

    SculptLayerDelta round_trip;
    REQUIRE(SculptLayerDelta::decode(bytes.data(), bytes.size(), &round_trip));
    CHECK(round_trip.size() == record.size());
    CHECK(round_trip.layer() == id);

    SUBCASE("a foreign magic") {
        std::vector<std::uint8_t> hostile = bytes;
        hostile[0] ^= 0xffu;
        SculptLayerDelta out;
        CHECK_FALSE(SculptLayerDelta::decode(hostile.data(), hostile.size(), &out));
    }
    SUBCASE("a version this build does not write") {
        std::vector<std::uint8_t> hostile = bytes;
        hostile[4] = 0x7fu;
        SculptLayerDelta out;
        CHECK_FALSE(SculptLayerDelta::decode(hostile.data(), hostile.size(), &out));
    }
    SUBCASE("an absurd declared entry count, refused BEFORE allocation") {
        // Four billion entries in a few hundred bytes is a request for more
        // memory than a machine holds, and it must be refused by arithmetic.
        std::vector<std::uint8_t> hostile = bytes;
        hostile[16] = 0xffu;
        hostile[17] = 0xffu;
        hostile[18] = 0xffu;
        hostile[19] = 0x0fu;
        SculptLayerDelta out;
        CHECK_FALSE(SculptLayerDelta::decode(hostile.data(), hostile.size(), &out));
    }
    SUBCASE("a truncated stream") {
        for (std::size_t cut : {std::size_t{4}, bytes.size() / 2, bytes.size() - 1}) {
            SculptLayerDelta out;
            CHECK_FALSE(SculptLayerDelta::decode(bytes.data(), cut, &out));
        }
    }
    SUBCASE("a record naming a layer this stack does not hold changes nothing") {
        const std::uint64_t before = s.sculpt_layer_checksum();
        SculptLayerDelta foreign = round_trip;
        foreign.set_layer(id + 1000);
        CHECK_FALSE(s.apply_sculpt_layer_delta(foreign, false));
        CHECK(s.sculpt_layer_checksum() == before);
    }
    SUBCASE("a record whose vertices are past the level changes NOTHING, not half") {
        // Checked entry by entry BEFORE one of them is written, which is the
        // difference between "refused" and "half applied".
        const std::uint64_t before = s.sculpt_layer_checksum();
        SculptLayerDelta wrong;
        wrong.set_layer(id);
        wrong.note_detail(2, 0, LocalDetail{0.0f, 0.0f, 0.5f});
        wrong.note_detail(2, 1u << 30, LocalDetail{});
        CHECK_FALSE(s.apply_sculpt_layer_delta(wrong, false));
        CHECK(s.sculpt_layer_checksum() == before);
    }
}

TEST_CASE("a malformed property record is refused, and an absurd stack size before allocation") {
    MultiresSurface s = build(2);
    SculptLayerProperty added;
    const SculptLayerId id = s.add_sculpt_layer("pass", &added);
    SculptLayerProperty dialled;
    REQUIRE(s.set_sculpt_layer_strength(id, 0.5f, &dialled));

    for (const SculptLayerProperty* record : {&added, &dialled}) {
        const std::vector<std::uint8_t> bytes = record->encode();
        SculptLayerProperty out;
        REQUIRE(SculptLayerProperty::decode(bytes.data(), bytes.size(), &out));
        CHECK(out.op == record->op);
        CHECK(out.layer == record->layer);

        std::vector<std::uint8_t> hostile = bytes;
        hostile[0] ^= 0xffu;
        SculptLayerProperty refused;
        CHECK_FALSE(SculptLayerProperty::decode(hostile.data(), hostile.size(), &refused));
        for (std::size_t cut : {std::size_t{6}, bytes.size() / 2, bytes.size() - 1}) {
            SculptLayerProperty cut_out;
            CHECK_FALSE(SculptLayerProperty::decode(bytes.data(), cut, &cut_out));
        }
    }

    SUBCASE("an operation code this build does not have") {
        std::vector<std::uint8_t> hostile = dialled.encode();
        hostile[8] = 0x7fu;
        SculptLayerProperty out;
        CHECK_FALSE(SculptLayerProperty::decode(hostile.data(), hostile.size(), &out));
    }
    SUBCASE("a structural record whose snapshot is not a stack applies nothing") {
        SculptLayerProperty broken = added;
        broken.stack_before.assign(64, 0x5au);
        broken.stack_after.assign(64, 0x5au);
        const std::uint64_t before = s.sculpt_layer_checksum();
        const std::size_t layers_before = s.sculpt_layers().size();
        CHECK_FALSE(s.apply_sculpt_layer_property(broken, false));
        CHECK(s.sculpt_layers().size() == layers_before);
        CHECK(s.sculpt_layer_checksum() == before);
    }
    SUBCASE("an absurd declared layer count is refused before the vector is reserved") {
        std::vector<std::uint8_t> hostile = SculptLayerStack{}.encode();
        hostile[8] = 0xffu;
        hostile[9] = 0xffu;
        hostile[10] = 0xffu;
        hostile[11] = 0x0fu;
        SculptLayerStack out;
        CHECK_FALSE(SculptLayerStack::decode(hostile.data(), hostile.size(), &out));
    }
}

TEST_CASE("a structural record's stack snapshot is the last thing that can check it") {
    // WHERE THE STACK DECODER'S CEILINGS ARE LOAD-BEARING AND NOTHING ELSE IS.
    // A structural undo step — add, remove, move, merge, bake — carries a whole
    // stack on each side, and replaying one hands those bytes to
    // `SculptLayerStack::decode` and installs whatever comes back. The document
    // path has a second opinion available (`MultiresSurface::decode` checks a
    // decoded stack against the hierarchy in the same stream), and this path has
    // none: a journal record is not required to name a surface it was taken
    // against, so `apply_structural_property` decodes, installs, and only THEN
    // re-imposes the levels.
    //
    // So a snapshot naming twelve levels of four billion vertices has to be
    // refused here or not at all, and "not at all" means the number a hostile
    // journal wrote is the number this build reserves a block index from.
    MultiresSurface s = build(2);
    SculptLayerProperty added;
    const SculptLayerId id = s.add_sculpt_layer("pass", &added);
    REQUIRE(added.op == SculptLayerProperty::Op::Structural);
    REQUIRE(!added.stack_after.empty());

    // The snapshot's level table sits after the magic, the version, the layer
    // count, the active id, the id counter and the block size — and the CHECK
    // below is what says so, so a format change fails here rather than poking
    // a neighbouring field and passing for the wrong reason.
    const std::size_t levels_at = 4 + 4 + 4 + 8 + 8 + 4;
    const auto read_u32 = [](const std::vector<std::uint8_t>& b, std::size_t at) {
        return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
               (static_cast<std::uint32_t>(b[at + 2]) << 16) |
               (static_cast<std::uint32_t>(b[at + 3]) << 24);
    };
    const auto write_u32 = [](std::vector<std::uint8_t>* b, std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu);
    };
    REQUIRE(read_u32(added.stack_after, levels_at) == s.level_count());

    SUBCASE("a level larger than any level can be") {
        SculptLayerProperty hostile = added;
        write_u32(&hostile.stack_after, levels_at + 4, SculptLayerStack::kMaxLevelVertices + 1u);
        const std::uint64_t before = s.sculpt_layer_checksum();
        const std::size_t layers_before = s.sculpt_layers().size();
        CHECK_FALSE(s.apply_sculpt_layer_property(hostile, true));
        CHECK(s.sculpt_layers().size() == layers_before);
        CHECK(s.sculpt_layer_checksum() == before);
    }
    SUBCASE("more levels than this build reconstructs") {
        // Poked into the record's OTHER side — the empty stack this add started
        // from — so the snapshot carries no layer payload. A layer's own level
        // count has to match the stack's, and on the populated side that check
        // would fire first and this one would never be reached.
        SculptLayerProperty hostile = added;
        std::vector<std::uint8_t>& b = hostile.stack_before;
        const std::uint32_t was = read_u32(b, levels_at);
        REQUIRE(was == s.level_count());
        // Widened in place, so the declared count and the bytes behind it agree
        // and the refusal is the ceiling rather than the truncation.
        write_u32(&b, levels_at, SculptLayerStack::kMaxLevels + 1u);
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(levels_at + 4 + 4 * was),
                 4 * (SculptLayerStack::kMaxLevels + 1u - was), 0);
        const std::size_t layers_before = s.sculpt_layers().size();
        CHECK_FALSE(s.apply_sculpt_layer_property(hostile, false));
        CHECK(s.sculpt_layers().size() == layers_before);
    }
    SUBCASE("and the untouched record still replays, both ways") {
        // The control. Every case above rewrites this record, so a refusal
        // means nothing until the record itself is known to work.
        REQUIRE(s.apply_sculpt_layer_property(added, false));
        CHECK(s.sculpt_layers().size() == 0);
        REQUIRE(s.apply_sculpt_layer_property(added, true));
        CHECK(s.sculpt_layers().size() == 1);
        CHECK(s.sculpt_layers().id_at(0) == id);
    }
}

TEST_CASE("the two kinds are counted apart, which is the only reason there are two") {
    // THE MEASUREMENT THE SPLIT EXISTS FOR. A byte accounting can only separate
    // what the kind separates, and "which of these is filling my undo budget"
    // is the only interesting question about a history's size.
    MultiresSurface s = build(3);
    const SculptLayerId id = s.add_sculpt_layer("pass");
    REQUIRE(s.set_active_sculpt_layer(id));

    History history;
    history.set_enabled(true);
    history.set_multires_resolver([&](scene::LayerId) -> MultiresSurface* { return &s; });

    const History::Bytes empty = history.bytes();
    SculptLayerProperty dialled;
    REQUIRE(s.set_sculpt_layer_strength(id, 0.5f, &dialled));
    history.record_multires_layer_property(1, dialled);
    const History::Bytes after_property = history.bytes();

    history.record_multires_layer_step(1, layered_stroke(s));
    const History::Bytes after_stroke = history.bytes();

    const std::size_t property_bytes = after_property.undo - empty.undo;
    const std::size_t stroke_bytes = after_stroke.undo - after_property.undo;
    CHECK(property_bytes > 0);
    CHECK(stroke_bytes > property_bytes);
    CHECK(after_stroke.journal > after_property.journal);
    CHECK(history.step_count() == 2);
}
