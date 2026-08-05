// Brush stroke engine (brush-engine spec, add-brush-stroke-engine): stroke
// resolution, preset versioning, and the two consumers.

#include <doctest/doctest.h>

#include <cmath>

#include "clay/brush/stroke.h"
#include "clay/io/clayspace.h"
#include "clay/scene/commands.h"

using namespace clay;
using namespace clay::brush;
using kernel::cf3;

namespace {

// A straight run along +x, sampled every `step` units.
std::vector<StrokeSample> line(float length, float step, float pressure = 1.0f) {
    std::vector<StrokeSample> out;
    for (float d = 0.0f; d <= length + 1e-5f; d += step) {
        StrokeSample s;
        s.position = cf3(d, 0, 0);
        s.pressure = pressure;
        out.push_back(s);
    }
    return out;
}

StrokePreset simple(float radius = 0.25f, float spacing = 0.5f) {
    StrokePreset p;
    p.radius = radius;
    p.spacing = spacing;
    p.pressure.strength = 0.0f;  // isolate whatever a case is actually testing
    return p;
}

}  // namespace

TEST_CASE("stroke: spacing follows the path, not the samples") {
    StrokePreset p = simple();
    std::vector<Stamp> sparse = resolve_stroke(line(2.0f, 0.5f), p);
    std::vector<Stamp> dense = resolve_stroke(line(2.0f, 0.05f), p);

    REQUIRE(sparse.size() == dense.size());
    for (std::size_t i = 0; i < sparse.size(); ++i) {
        CHECK(sparse[i].position.x == doctest::Approx(dense[i].position.x));
        CHECK(sparse[i].radius == doctest::Approx(dense[i].radius));
    }
    // spacing 0.5 of the diameter 0.5 = 0.25 world units over a 2-unit run
    CHECK(sparse.size() == 9);
}

TEST_CASE("stroke: a tap still leaves a mark") {
    StrokePreset p = simple();
    std::vector<StrokeSample> one(1);
    one[0].position = cf3(1, 2, 3);
    std::vector<Stamp> stamps = resolve_stroke(one, p);
    REQUIRE(stamps.size() == 1);
    CHECK(stamps[0].position.x == doctest::Approx(1.0f));

    // A drag shorter than one spacing is still one stamp, not zero.
    CHECK(resolve_stroke(line(0.01f, 0.005f), p).size() == 1);
    CHECK(resolve_stroke({}, p).empty());
}

TEST_CASE("stroke: pressure drives size and strength") {
    StrokePreset p = simple();
    p.pressure.size = 1.0f;
    p.pressure.strength = 1.0f;

    std::vector<StrokeSample> ramp;
    for (int i = 0; i <= 20; ++i) {
        StrokeSample s;
        s.position = cf3(i * 0.1f, 0, 0);
        s.pressure = i / 20.0f;
        ramp.push_back(s);
    }
    std::vector<Stamp> stamps = resolve_stroke(ramp, p);
    REQUIRE(stamps.size() > 3);
    for (std::size_t i = 1; i < stamps.size(); ++i) {
        CHECK(stamps[i].radius >= stamps[i - 1].radius);
        CHECK(stamps[i].strength >= stamps[i - 1].strength);
    }
    CHECK(stamps.front().radius < stamps.back().radius);
}

TEST_CASE("stroke: taper closes the ends") {
    StrokePreset p = simple();
    p.taper_start = 0.25f;
    p.taper_end = 0.25f;
    std::vector<Stamp> stamps = resolve_stroke(line(4.0f, 0.1f), p);
    REQUIRE(stamps.size() > 8);

    const Stamp& middle = stamps[stamps.size() / 2];
    CHECK(stamps.front().radius < middle.radius);
    CHECK(stamps.back().radius < middle.radius);
    CHECK(middle.radius == doctest::Approx(p.radius));

    // Tapers that would overlap are split rather than multiplying each other
    // down to nothing in the middle.
    p.taper_start = 0.8f;
    p.taper_end = 0.8f;
    std::vector<Stamp> squeezed = resolve_stroke(line(4.0f, 0.1f), p);
    REQUIRE(!squeezed.empty());
    CHECK(squeezed[squeezed.size() / 2].radius > 0.0f);
}

TEST_CASE("stroke: jitter is reproducible and seed-dependent") {
    StrokePreset p = simple();
    p.jitter_position = 0.5f;
    p.jitter_size = 0.3f;
    p.seed = 11;

    std::vector<StrokeSample> path = line(3.0f, 0.1f);
    std::vector<Stamp> a = resolve_stroke(path, p);
    std::vector<Stamp> b = resolve_stroke(path, p);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].position.y == doctest::Approx(b[i].position.y));
        CHECK(a[i].radius == doctest::Approx(b[i].radius));
    }
    // It really is jittering: a straight run along +x leaves the axis.
    CHECK(std::any_of(a.begin(), a.end(), [](const Stamp& s) { return std::abs(s.position.y) > 0; }));

    p.seed = 12;
    std::vector<Stamp> c = resolve_stroke(path, p);
    REQUIRE(c.size() == a.size());
    CHECK(std::any_of(c.begin(), c.end(), [&](const Stamp& s) {
        std::size_t i = static_cast<std::size_t>(&s - &c[0]);
        return s.position.y != a[i].position.y;
    }));
}

TEST_CASE("stroke: steady stroke smooths a shaky path") {
    // A zigzag that goes nowhere off-axis on average.
    std::vector<StrokeSample> shaky;
    for (int i = 0; i <= 60; ++i) {
        StrokeSample s;
        s.position = cf3(i * 0.05f, (i % 2 ? 0.08f : -0.08f), 0);
        shaky.push_back(s);
    }
    // Mean deviation, not the worst: a lazy-mouse follower starts AT the
    // first sample by construction, so the very first stamp is never smoothed
    // and a max would be measuring that one point forever.
    auto excursion = [](const std::vector<Stamp>& stamps) {
        float total = 0.0f;
        for (const Stamp& s : stamps) total += std::abs(s.position.y);
        return stamps.empty() ? 0.0f : total / static_cast<float>(stamps.size());
    };

    StrokePreset p = simple(0.1f, 0.5f);
    float raw = excursion(resolve_stroke(shaky, p));
    p.steady = 0.8f;
    float steadied = excursion(resolve_stroke(shaky, p));
    CHECK(steadied < raw);
}

TEST_CASE("stroke: rotate-along-stroke follows the path") {
    StrokePreset p = simple();
    p.rotate_along_stroke = true;
    std::vector<StrokeSample> path;
    for (int i = 0; i <= 20; ++i) {
        StrokeSample s;
        s.position = cf3(0, i * 0.1f, 0);  // straight up +y
        path.push_back(s);
    }
    std::vector<Stamp> stamps = resolve_stroke(path, p);
    REQUIRE(!stamps.empty());
    // +X rotated by the stamp's orientation lands on the path direction.
    kernel::cfloat3 x = stamps[1].rotation.rotate(cf3(1, 0, 0));
    CHECK(x.y == doctest::Approx(1.0f).epsilon(0.01f));

    p.rotate_along_stroke = false;
    CHECK(resolve_stroke(path, p)[1].rotation.w == doctest::Approx(1.0f));
}

TEST_CASE("stroke: clamped accumulation is weaker per stamp than buildup") {
    StrokePreset p = simple();
    p.spacing = 0.2f;  // heavy overlap
    float buildup = resolve_stroke(line(2.0f, 0.1f), p)[3].strength;
    p.accumulation = Accumulation::Clamped;
    float clamped = resolve_stroke(line(2.0f, 0.1f), p)[3].strength;
    CHECK(clamped < buildup);
    CHECK(clamped > 0.0f);
}

// The reason a schema version exists from the first release rather than being
// retrofitted: a preset library outliving the engine that wrote it.
TEST_CASE("stroke: presets survive engine versions") {
    StrokePreset p;
    p.spacing = 0.31f;
    p.pressure.size = 0.7f;
    p.pressure.strength = 0.4f;
    p.pressure.curve = 1.6f;
    p.jitter_position = 0.2f;
    p.jitter_rotation = 0.9f;
    p.seed = 4242;
    p.rotate_along_stroke = true;
    p.taper_start = 0.1f;
    p.taper_end = 0.2f;
    p.steady = 0.35f;
    p.accumulation = Accumulation::Clamped;
    p.radius = 0.6f;
    p.strength = 0.8f;

    std::vector<std::uint8_t> bytes = p.serialize();

    SUBCASE("round trip is exact and deterministic") {
        auto back = StrokePreset::deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->spacing == doctest::Approx(p.spacing));
        CHECK(back->pressure.curve == doctest::Approx(p.pressure.curve));
        CHECK(back->seed == p.seed);
        CHECK(back->rotate_along_stroke == p.rotate_along_stroke);
        CHECK(back->accumulation == p.accumulation);
        CHECK(back->radius == doctest::Approx(p.radius));
        CHECK(back->serialize() == bytes);

        // ...and it resolves the same stroke.
        std::vector<StrokeSample> path = line(3.0f, 0.1f);
        std::vector<Stamp> before = resolve_stroke(path, p);
        std::vector<Stamp> after = resolve_stroke(path, *back);
        REQUIRE(before.size() == after.size());
        for (std::size_t i = 0; i < before.size(); ++i)
            CHECK(before[i].position.x == doctest::Approx(after[i].position.x));
    }

    SUBCASE("a newer schema is refused, not read as a prefix") {
        std::vector<std::uint8_t> newer = bytes;
        newer[0] = static_cast<std::uint8_t>(kPresetVersion + 1);
        CHECK_FALSE(StrokePreset::deserialize(newer.data(), newer.size()).has_value());
    }

    SUBCASE("truncated and nonsense streams are refused") {
        for (std::size_t cut : {std::size_t(0), std::size_t(1), bytes.size() / 2, bytes.size() - 1})
            CHECK_FALSE(StrokePreset::deserialize(bytes.data(), cut).has_value());

        StrokePreset bad = p;
        bad.spacing = 0.0f;
        std::vector<std::uint8_t> zero_spacing = bad.serialize();
        CHECK_FALSE(
            StrokePreset::deserialize(zero_spacing.data(), zero_spacing.size()).has_value());
    }
}

TEST_CASE("stroke: applied to a voxel grid") {
    voxel::VoxelGrid grid(0.05f);
    std::uint8_t index = grid.palette_add(cf3(1, 1, 1));
    StrokePreset p = simple(0.15f, 0.5f);

    std::vector<Stamp> stamps = resolve_stroke(line(1.5f, 0.05f), p);
    REQUIRE(stamps.size() > 5);
    CHECK(apply_to_grid(grid, stamps, index) == stamps.size());
    CHECK(grid.occupied_count() > 0);

    // The stroke laid material along its whole path, not just at the ends.
    auto lo = grid.bounds_min();
    auto hi = grid.bounds_max();
    REQUIRE(lo.has_value());
    REQUIRE(hi.has_value());
    CHECK(hi->x - lo->x > 25);  // ~1.5 units at 0.05 per cell
}

TEST_CASE("stroke: a frozen region receives nothing") {
    voxel::MaskField mask(0.05f);
    for (int x = 0; x < 40; ++x)  // world x in [0, 2)
        for (int y = -20; y <= 20; ++y)
            for (int z = -20; z <= 20; ++z) mask.set({x, y, z}, 1.0f);

    StrokePreset p = simple(0.15f, 0.5f);
    // A run from x = -1.5 to x = 1.5: the second half is frozen.
    std::vector<StrokeSample> path;
    for (int i = 0; i <= 60; ++i) {
        StrokeSample s;
        s.position = cf3(-1.5f + i * 0.05f, 0, 0);
        path.push_back(s);
    }
    std::vector<Stamp> stamps = resolve_stroke(path, p);
    REQUIRE(stamps.size() > 8);

    SUBCASE("voxel stamps are dropped") {
        voxel::VoxelGrid a(0.05f), b(0.05f);
        std::uint8_t ia = a.palette_add(cf3(1, 1, 1)), ib = b.palette_add(cf3(1, 1, 1));
        std::size_t all = apply_to_grid(a, stamps, ia);
        std::size_t gated = apply_to_grid(b, stamps, ib, voxel::BrushShape::Sphere,
                                          voxel::BrushFalloff::Smooth, &mask);
        CHECK(gated < all);
        CHECK(gated > 0);
        // Nothing landed inside the frozen half.
        auto hi = b.bounds_max();
        REQUIRE(hi.has_value());
        CHECK(hi->x < 4);  // a stamp centred at x < 0 has a radius of ~3 cells
    }

    SUBCASE("SDF items are not emitted") {
        scene::Node templ;
        templ.prim = scene::Prim::sphere(1.0f);
        scene::SdfContent content;
        std::vector<scene::Node> all = stamps_to_nodes(content, stamps, templ);
        std::vector<scene::Node> gated = stamps_to_nodes(content, stamps, templ, &mask);
        CHECK(gated.size() < all.size());
        CHECK(!gated.empty());
        for (const scene::Node& n : gated) CHECK(n.xform.position.x < 0.0f);
    }
}

TEST_CASE("stroke: a partial mask attenuates rather than dropping") {
    voxel::MaskField mask(0.05f);
    for (int x = -40; x <= 40; ++x)
        for (int y = -20; y <= 20; ++y)
            for (int z = -20; z <= 20; ++z) mask.set({x, y, z}, 0.5f);

    StrokePreset p = simple(0.15f, 0.5f);
    std::vector<Stamp> stamps = resolve_stroke(line(1.0f, 0.05f), p);
    scene::Node templ;
    templ.prim = scene::Prim::sphere(1.0f);

    scene::SdfContent content;
    std::vector<scene::Node> gated = stamps_to_nodes(content, stamps, templ, &mask);
    CHECK(gated.size() == stamps.size());  // nothing dropped at half mask
}

// The interface promise: a stroked edit is an ordinary edit, so undo and the
// document format apply to it without knowing this module exists.
TEST_CASE("stroke: a stroked SDF edit is an ordinary edit list") {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("body");

    scene::Node templ;
    templ.prim = scene::Prim::sphere(1.0f);
    templ.blend = scene::Blend{};
    std::vector<scene::Node> nodes =
        stamps_to_nodes(*layer.sdf, resolve_stroke(line(1.0f, 0.05f), simple()), templ);
    REQUIRE(nodes.size() > 3);

    std::vector<std::uint8_t> before = scene::serialize_document(doc);

    // One node per stamp, bundled into a single undo group: that is what
    // makes a whole stroke one undo step, and it is the stack's existing
    // grouping rather than anything this engine had to add.
    scene::UndoStack undo;
    undo.begin_group();
    for (const scene::Node& n : nodes) {
        scene::AddNodeCmd add;
        add.layer = layer.id;
        add.subtree = {n};
        CHECK(undo.perform(doc, scene::Command{add}));
    }
    undo.end_group();

    CHECK(doc.find_layer(layer.id)->sdf->roots.size() == nodes.size());
    CHECK(undo.undo_depth() == 1);  // the stroke is one step, not one per stamp

    CHECK(undo.undo(doc));
    CHECK(scene::serialize_document(doc) == before);  // undo restored it exactly
    CHECK(undo.redo(doc));
    CHECK(doc.find_layer(layer.id)->sdf->roots.size() == nodes.size());
}

TEST_CASE("stroke: a stroked voxel edit round trips through the document") {
    io::ClaySpaceDoc doc;
    scene::Layer& layer = doc.document.add_sdf_layer("blocks");
    layer.kind = scene::LayerKind::Voxel;
    layer.sdf.reset();
    voxel::VoxelGrid grid(0.05f);
    std::uint8_t index = grid.palette_add(cf3(0.9f, 0.4f, 0.2f));
    apply_to_grid(grid, resolve_stroke(line(1.0f, 0.05f), simple(0.15f)), index);
    REQUIRE(grid.occupied_count() > 0);
    doc.voxel_layers.emplace(layer.id, std::move(grid));

    std::vector<std::uint8_t> bytes = io::save_clayspace(doc);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    REQUIRE(back.voxel_layers.count(layer.id) == 1);
    CHECK(back.voxel_layers.at(layer.id).serialize() ==
          doc.voxel_layers.at(layer.id).serialize());
}
