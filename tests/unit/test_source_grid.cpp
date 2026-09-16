#include <doctest/doctest.h>

#include "../../src/session/source_grid.h"
#include "clay/session/sdf_prefix_cache.h"

#include <bit>
#include <cstdint>
#include <cstring>

using namespace clay;
using field::FieldVolume;
using kernel::cf3;

TEST_CASE("source grid evaluates unique coordinates once and reconstructs every sample bit") {
    const std::array<std::array<int, 3>, 3> shapes{{{6, 6, 6}, {7, 8, 9}, {13, 13, 13}}};
    for (const auto &shape : shapes) {
        const FieldVolume::BrickGrid grid{
            cf3(-1.137f, .0123f, -77.9f), .017f, .051f, {shape[0], shape[1], shape[2]}};
        const auto bricks = std::size_t(shape[0]) * shape[1] * shape[2];
        const auto unique = std::size_t(shape[0] * 8 + 1) * (shape[1] * 8 + 1) * (shape[2] * 8 + 1);
        std::vector<float> out(bricks * field::kBrickSamples + 2, 123.0f);
        for (int axis = 0; axis < 3; ++axis) {
            std::size_t calls = 0;
            std::size_t evaluated = 0;
            const bool filled = session::detail::fill_unique_source_grid(
                grid, 0, bricks, out.data() + 1,
                [&](const float *points, std::size_t count, float *values) {
                    ++calls;
                    evaluated += count;
                    for (std::size_t i = 0; i < count; ++i)
                        values[i] = points[i * 3 + axis];
                });
            REQUIRE(filled);
            CHECK(calls == 1);
            CHECK(evaluated == unique);
            CHECK(evaluated < bricks * field::kBrickSamples);
            CHECK(evaluated * 4 <= bricks * field::kBrickSamples * 3);
            bool exact = true;
            for (std::size_t slot = 0; slot < bricks; ++slot) {
                for (int i = 0; i < field::kBrickSamples; ++i) {
                    const auto p = grid.sample_position(slot, i);
                    const float expected[3]{p.x, p.y, p.z};
                    exact &=
                        std::bit_cast<std::uint32_t>(out[1 + slot * field::kBrickSamples + i]) ==
                        std::bit_cast<std::uint32_t>(expected[axis]);
                }
            }
            CHECK(exact);
            CHECK(out.front() == 123.0f);
            CHECK(out.back() == 123.0f);
        }
    }
}

TEST_CASE("source grid declines ineligible windows without calling or writing") {
    const auto imax = std::numeric_limits<int>::max();
    struct Case {
        std::array<int, 3> shape;
        std::size_t first, count;
    };
    const Case cases[]{{{6, 6, 6}, 1, 216},
                       {{6, 6, 6}, 0, 215},
                       {{6, 6, 6}, 0, 217},
                       {{1, 1, 1}, 0, 1},
                       {{4, 4, 4}, 0, 64},
                       {{1, 1, 216}, 0, 216},
                       {{0, 6, 6}, 0, 216},
                       {{6, -1, 6}, 0, 216},
                       {{6, 6, 0}, 0, 216},
                       {{imax, 6, 6}, 0, 216},
                       {{6, imax, 6}, 0, 216},
                       {{6, 6, imax}, 0, 216},
                       {{6, 6, 6}, 0, std::numeric_limits<std::size_t>::max()},
                       {{6, 6, 6}, 0, 0}};
    for (const auto &c : cases) {
        const FieldVolume::BrickGrid grid{
            cf3(0, 0, 0), .02f, .06f, {c.shape[0], c.shape[1], c.shape[2]}};
        float output = 123.0f;
        bool called = false;
        CHECK_FALSE(session::detail::fill_unique_source_grid(
            grid, c.first, c.count, &output,
            [&](const float *, std::size_t, float *) { called = true; }));
        CHECK_FALSE(called);
        CHECK(output == 123.0f);
    }
}

TEST_CASE("source grid materialization matches duplicate-point source evaluation exactly") {
    scene::Document doc;
    auto &layer = doc.add_sdf_layer("body");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    layer.sdf->insert(base);
    scene::Node dab;
    dab.prim = scene::Prim::sphere(.25f);
    dab.xform.position = cf3(.2f, .1f, .9f);
    dab.blend = {scene::BlendProfile::Quadratic, .12f};
    layer.sdf->insert(dab);
    const auto source = session::SdfSourceField::open(doc, layer.id, nullptr);
    REQUIRE(source);
    const FieldVolume::BrickBlockFill duplicate = [&](const auto &grid, std::size_t first,
                                                      std::size_t count, float *out) {
        std::vector<float> points(count * field::kBrickSamples * 3);
        grid.sample_positions(first, count, points.data());
        source->fill_points(points.data(), count * field::kBrickSamples, out);
    };
    const math::Aabb bounds{cf3(-1.1f, -1.2f, -1.3f), cf3(1.2f, 1.3f, 1.4f)};
    for (bool partial_first : {false, true}) {
        auto actual = FieldVolume::empty_lattice(bounds, .04f, .12f);
        auto expected = actual;
        if (partial_first) {
            const auto local = FieldVolume::Region::ball(cf3(.1f, .2f, .8f), .2f);
            actual.materialize_region(local, source->block_fill());
            expected.materialize_region(local, duplicate);
        }
        const auto a = actual.materialize_region(FieldVolume::Region{bounds}, source->block_fill());
        const auto b = expected.materialize_region(FieldVolume::Region{bounds}, duplicate);
        CHECK(a.added == b.added);
        CHECK(a.kept == b.kept);
        CHECK(a.evaluated == b.evaluated);
        CHECK(std::bit_cast<std::uint32_t>(actual.sample_lipschitz()) ==
              std::bit_cast<std::uint32_t>(expected.sample_lipschitz()));
        const auto ab = actual.to_blob(), eb = expected.to_blob();
        REQUIRE(ab.size() == eb.size());
        CHECK(std::memcmp(ab.data(), eb.data(), ab.size() * sizeof(ab[0])) == 0);
    }
}
