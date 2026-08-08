// Mask field (voxel-engine spec, add-mask-field): painting, region ops, edit
// gating, and the invariant that made this worth specifying — a mask that
// survives a resolution change and a document round trip.

#include <doctest/doctest.h>

#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

using namespace clay;
using namespace clay::voxel;
using kernel::cf3;

namespace {

// A grid full of material over the cell box [lo, hi], so an edit's effect is
// measured by what is left rather than by what appeared.
VoxelGrid solid_block(float voxel_size, VoxelCoord lo, VoxelCoord hi) {
    VoxelGrid g(voxel_size);
    std::uint8_t idx = g.palette_add(cf3(1, 1, 1));
    g.fill_box(lo, hi, idx);
    return g;
}

}  // namespace

TEST_CASE("mask: painting and reading back") {
    MaskField m(0.1f);
    CHECK(m.empty());

    BrushParams p;
    p.size = 5;
    p.shape = BrushShape::Sphere;
    m.paint(cf3(0.05f, 0.05f, 0.05f), p, 1.0f);  // centred on cell (0,0,0)

    CHECK(m.sample(cf3(0.05f, 0.05f, 0.05f)) == doctest::Approx(1.0f));
    CHECK(m.sample(cf3(5.0f, 5.0f, 5.0f)) == doctest::Approx(0.0f));
    CHECK(m.painted_count() > 0);
    CHECK_FALSE(m.empty());
}

TEST_CASE("mask: the falloff grades the values") {
    MaskField m(0.1f);
    BrushParams p;
    p.size = 9;
    p.shape = BrushShape::Sphere;
    p.falloff = BrushFalloff::Smooth;
    m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);

    float centre = m.get({0, 0, 0});
    float mid = m.get({3, 0, 0});
    CHECK(centre == doctest::Approx(1.0f));
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);  // graded, not binary
}

TEST_CASE("mask: strength blends toward the target") {
    MaskField m(0.1f);
    BrushParams p;
    p.size = 3;
    p.strength = 0.5f;
    m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);
    CHECK(m.get({0, 0, 0}) == doctest::Approx(0.5f).epsilon(0.01f));
    m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);  // a second pass builds up
    CHECK(m.get({0, 0, 0}) == doctest::Approx(0.75f).epsilon(0.01f));
    p.strength = 1.0f;
    m.paint(VoxelCoord{0, 0, 0}, p, 0.0f);  // target 0 is the eraser
    CHECK(m.get({0, 0, 0}) == doctest::Approx(0.0f));
}

TEST_CASE("mask: region operations") {
    MaskField m(0.1f);
    BrushParams p;
    p.size = 5;
    p.falloff = BrushFalloff::Linear;
    m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);

    SUBCASE("invert round trips") {
        float before = m.get({1, 0, 0});
        m.invert();
        CHECK(m.get({1, 0, 0}) == doctest::Approx(1.0f - before).epsilon(0.01f));
        m.invert();
        CHECK(m.get({1, 0, 0}) == doctest::Approx(before).epsilon(0.01f));
    }
    SUBCASE("expand grows the painted region") {
        auto hi_before = m.bounds_max();
        REQUIRE(hi_before.has_value());
        m.expand(1);
        auto hi_after = m.bounds_max();
        REQUIRE(hi_after.has_value());
        CHECK(hi_after->x == hi_before->x + 1);
    }
    SUBCASE("contract shrinks it") {
        auto hi_before = m.bounds_max();
        REQUIRE(hi_before.has_value());
        m.contract(1);
        auto hi_after = m.bounds_max();
        REQUIRE(hi_after.has_value());
        CHECK(hi_after->x == hi_before->x - 1);
    }
    SUBCASE("smooth softens the centre without emptying the field") {
        float centre = m.get({0, 0, 0});
        m.smooth(1);
        CHECK(m.get({0, 0, 0}) < centre);
        CHECK(m.painted_count() > 0);
    }
    SUBCASE("clear empties it") {
        m.clear();
        CHECK(m.empty());
        CHECK(m.painted_count() == 0);
        CHECK(m.sample(cf3(0, 0, 0)) == doctest::Approx(0.0f));
    }
}

TEST_CASE("mask: region ops cost the painted cells, not the bounding box") {
    // expand/contract/smooth sized two dense buffers from the painted BOUNDING
    // BOX, which is not the paint: two small blobs far apart cost everything
    // between them — 45 seconds and 2 GB at a separation of 1000 — and the
    // coordinate difference overflowed int before that. A deserialized mask
    // may carry any chunk keys, so a file could reach it.
    voxel::BrushParams p;
    p.size = 5;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = voxel::BrushFalloff::Constant;
    p.strength = 1.0f;

    for (int sep : {50, 1000, 20000}) {
        voxel::MaskField m(0.1f);
        m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);
        m.paint(VoxelCoord{sep, sep, sep}, p, 1.0f);
        const std::size_t painted = m.painted_count();
        REQUIRE(painted > 0);

        m.expand(1);
        INFO("separation " << sep);
        // both blobs grew, and nothing in between was touched
        CHECK(m.painted_count() > painted);
        CHECK(m.get({sep / 2, sep / 2, sep / 2}) == doctest::Approx(0.0f));
        CHECK(m.get({0, 0, 0}) == doctest::Approx(1.0f));
        CHECK(m.get({sep, sep, sep}) == doctest::Approx(1.0f));
    }
}

TEST_CASE("mask: a frozen region survives an edit") {
    // Material everywhere over x in [-8, 8]; mask the x < 0 half solid, then
    // erase across the whole span.
    VoxelGrid g = solid_block(0.1f, {-8, -1, -1}, {8, 1, 1});
    std::size_t before = g.occupied_count();

    // Mask exactly the negative-x cells, so the boundary is unambiguous.
    MaskField m(0.1f);
    for (int x = -8; x < 0; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) m.set({x, y, z}, 1.0f);

    BrushParams p;
    p.size = 20;
    p.mask = &m;
    g.erase_brush(VoxelCoord{0, 0, 0}, p);

    CHECK(g.occupied_count() < before);
    // Everything masked is untouched...
    for (int x = -8; x < 0; ++x) CHECK(g.get({x, 0, 0}) != 0);
    // ...and the unmasked side within the footprint is gone.
    for (int x = 0; x <= 8; ++x) CHECK(g.get({x, 0, 0}) == 0);
}

TEST_CASE("mask: partial masking attenuates rather than freezing") {
    auto surviving = [](float mask_value) {
        VoxelGrid g = solid_block(0.1f, {-6, -6, -6}, {6, 6, 6});
        MaskField m(0.1f);
        if (mask_value > 0.0f)
            for (int x = -6; x <= 6; ++x)
                for (int y = -6; y <= 6; ++y)
                    for (int z = -6; z <= 6; ++z) m.set({x, y, z}, mask_value);
        BrushParams p;
        p.size = 13;
        p.seed = 7;
        p.mask = m.empty() ? nullptr : &m;
        g.erase_brush(VoxelCoord{0, 0, 0}, p);
        return g.occupied_count();
    };

    std::size_t none = surviving(0.0f);
    std::size_t half = surviving(0.5f);
    std::size_t full = surviving(1.0f);
    CHECK(none < half);
    CHECK(half < full);
    // A fully masked region is untouched by any edit, not merely mostly so.
    CHECK(full == solid_block(0.1f, {-6, -6, -6}, {6, 6, 6}).occupied_count());
}

TEST_CASE("mask: every verb honours the mask") {
    // Not a per-verb behavioural check — the point is that gating lives in the
    // shared footprint walk, so no verb can quietly skip it.
    MaskField m(0.1f);
    for (int x = -10; x <= 10; ++x)
        for (int y = -10; y <= 10; ++y)
            for (int z = -10; z <= 10; ++z) m.set({x, y, z}, 1.0f);

    // Straddling the block's +x face, not buried inside it: an interior stamp
    // is a no-op for inflate and pinch, which would make the "does something"
    // half of the assertion below unsatisfiable.
    const VoxelCoord kEdge{4, 0, 0};
    BrushParams masked;
    masked.size = 9;
    masked.mask = &m;
    BrushParams open_brush = masked;
    open_brush.mask = nullptr;

    // Each verb is run twice: masked, and not. Asserting only the masked run
    // would pass for a verb that does nothing at all, which is the failure
    // mode a gating test is most likely to hide.
    auto gated = [&](auto&& verb) {
        VoxelGrid a = solid_block(0.1f, {-4, -4, -4}, {4, 4, 4});
        VoxelGrid b = a;
        std::vector<std::uint8_t> before = a.serialize();
        verb(a, masked);
        verb(b, open_brush);
        // Compared by content, not by occupied count: pinch and grab move
        // material rather than adding or removing it, and a count would call
        // that "unchanged".
        CHECK(b.serialize() != before);      // the verb does something
        return a.serialize() == before;      // ...but not through the mask
    };

    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) { g.erase_brush(kEdge, p); }));
    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_smooth(kEdge, p);
    }));
    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_inflate(kEdge, p, 1);
    }));
    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_flatten(kEdge, p, cf3(0, 1, 0), 0.0f);
    }));
    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_pinch(kEdge, p);
    }));
    CHECK(gated([&](VoxelGrid& g, const BrushParams& p) {
        g.sculpt_grab(kEdge, p, cf3(0.3f, 0, 0), false);
    }));
}

TEST_CASE("mask: an empty mask costs nothing and changes nothing") {
    MaskField m(0.1f);
    CHECK(m.serialize().size() == 8);  // cell size + zero chunks

    VoxelGrid a = solid_block(0.1f, {-4, -4, -4}, {4, 4, 4});
    VoxelGrid b = solid_block(0.1f, {-4, -4, -4}, {4, 4, 4});
    BrushParams p;
    p.size = 5;
    p.falloff = BrushFalloff::Smooth;
    p.seed = 3;
    a.erase_brush(VoxelCoord{0, 0, 0}, p);
    p.mask = &m;
    b.erase_brush(VoxelCoord{0, 0, 0}, p);
    CHECK(a.serialize() == b.serialize());
}

// The invariant this change exists to guarantee. 3DCoat's masks die on
// voxelization; ours are addressed in world units so they cannot.
TEST_CASE("mask: survives a resolution change") {
    MaskField m(0.1f);
    BrushParams p;
    p.size = 7;
    p.shape = BrushShape::Sphere;
    p.falloff = BrushFalloff::Smooth;
    m.paint(cf3(0.35f, 0.35f, 0.35f), p, 1.0f);

    const kernel::cfloat3 probes[] = {cf3(0.35f, 0.35f, 0.35f), cf3(0.15f, 0.35f, 0.35f),
                                      cf3(0.55f, 0.35f, 0.35f), cf3(2.0f, 2.0f, 2.0f)};
    float before[4];
    for (int i = 0; i < 4; ++i) before[i] = m.sample(probes[i]);
    CHECK(before[0] == doctest::Approx(1.0f));

    // The layer's voxel resolution changes underneath the mask: a coarse grid
    // and a fine one both consult it through world space.
    for (float voxel_size : {0.05f, 0.1f, 0.4f}) {
        VoxelGrid g(voxel_size);
        std::uint8_t idx = g.palette_add(cf3(1, 1, 1));
        g.fill_box({0, 0, 0}, {20, 20, 20}, idx);
        BrushParams e;
        e.size = 9;
        e.mask = &m;
        g.erase_brush(g.build_plane_pick(math::Ray{cf3(0.35f, 5.0f, 0.35f), cf3(0, -1, 0)}, 3)
                          .value_or(VoxelCoord{3, 3, 3}),
                      e);
        // The mask itself is untouched by anything the grid does with it.
        for (int i = 0; i < 4; ++i) CHECK(m.sample(probes[i]) == doctest::Approx(before[i]));
    }
}

TEST_CASE("mask: round trips through serialization and the document format") {
    MaskField m(0.07f);
    BrushParams p;
    p.size = 11;
    p.shape = BrushShape::Sphere;
    p.falloff = BrushFalloff::Gaussian;
    m.paint(cf3(0.5f, 0.5f, 0.5f), p, 1.0f);
    m.paint(cf3(-0.9f, 0.2f, 0.1f), p, 0.6f);
    REQUIRE(m.painted_count() > 0);

    SUBCASE("raw stream") {
        std::vector<std::uint8_t> bytes = m.serialize();
        auto back = MaskField::deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->cell_size() == doctest::Approx(m.cell_size()));
        CHECK(back->painted_count() == m.painted_count());
        CHECK(back->serialize() == bytes);  // deterministic
        for (float x = -1.2f; x < 1.2f; x += 0.13f)
            CHECK(back->sample(cf3(x, 0.5f, 0.5f)) == doctest::Approx(m.sample(cf3(x, 0.5f, 0.5f))));
    }

    SUBCASE("a .clayspace document carries it") {
        io::ClaySpaceDoc doc;
        scene::LayerId layer = doc.document.add_sdf_layer("masked").id;
        doc.masks.emplace(layer, m);

        std::vector<std::uint8_t> bytes = io::save_clayspace(doc);
        io::ClaySpaceDoc back;
        REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
        REQUIRE(back.masks.count(layer) == 1);
        const MaskField& r = back.masks.at(layer);
        CHECK(r.cell_size() == doctest::Approx(m.cell_size()));
        CHECK(r.painted_count() == m.painted_count());
        for (float x = -1.2f; x < 1.2f; x += 0.13f)
            CHECK(r.sample(cf3(x, 0.5f, 0.5f)) == doctest::Approx(m.sample(cf3(x, 0.5f, 0.5f))));
    }

    SUBCASE("an empty mask writes no chunk") {
        io::ClaySpaceDoc doc;
        scene::LayerId layer = doc.document.add_sdf_layer("plain").id;
        doc.masks.emplace(layer, MaskField(0.1f));
        std::vector<std::uint8_t> with = io::save_clayspace(doc);

        io::ClaySpaceDoc bare;
        bare.document.add_sdf_layer("plain");
        CHECK(with.size() == io::save_clayspace(bare).size());
    }
}

TEST_CASE("mask: truncated streams are rejected") {
    MaskField m(0.1f);
    BrushParams p;
    p.size = 5;
    m.paint(VoxelCoord{0, 0, 0}, p, 1.0f);
    std::vector<std::uint8_t> bytes = m.serialize();
    for (std::size_t cut : {std::size_t(0), std::size_t(3), bytes.size() / 2, bytes.size() - 1})
        CHECK_FALSE(MaskField::deserialize(bytes.data(), cut).has_value());
}

// scene-model: a mask cannot change what a document evaluates to. It is not
// in scene::Document at all, which is the point — this asserts the structure
// rather than trusting it.
TEST_CASE("mask: evaluation is untouched by a mask") {
    io::ClaySpaceDoc doc;
    scene::Layer& layer = doc.document.add_sdf_layer("body");
    scene::Node node;
    node.prim = scene::Prim::sphere(1.0f);
    layer.sdf->insert(std::move(node));

    std::vector<kernel::cfloat3> probes;
    for (float x = -2.0f; x <= 2.0f; x += 0.31f)
        for (float y = -2.0f; y <= 2.0f; y += 0.37f) probes.push_back(cf3(x, y, 0.21f));

    auto evaluate = [&]() {
        scene::Tape tape = scene::compile_document(doc.document);
        eval::Backend* cpu = eval::Registry::instance().find("cpu");
        REQUIRE(cpu != nullptr);
        std::vector<float> out(probes.size());
        eval::PointQuery q{&probes[0].x, probes.size(), 1e-4f};
        eval::PointResults r{out.data(), nullptr, nullptr};
        REQUIRE(cpu->eval_points(tape, q, r) == eval::Status::Ok);
        return out;
    };

    std::vector<float> before = evaluate();

    voxel::MaskField m(0.1f);
    BrushParams p;
    p.size = 15;
    p.shape = BrushShape::Sphere;
    m.paint(cf3(0.5f, 0.5f, 0.0f), p, 1.0f);
    doc.masks.emplace(layer.id, std::move(m));

    CHECK(evaluate() == before);  // bit-identical, not merely close
}
