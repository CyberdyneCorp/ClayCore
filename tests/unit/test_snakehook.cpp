// Snakehook (brush-engine spec, add-snakehook).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using brush::SnakehookSettings;
using kernel::cf3;

namespace {

// A drag leaving the top of a unit-ish ball and curling over.
std::vector<kernel::cfloat3> curling_drag(int samples = 12) {
    std::vector<kernel::cfloat3> path;
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples - 1);
        path.push_back(cf3(0.55f * std::sin(t * 1.9f), 0.55f + t * 0.85f, 0.25f * t * t));
    }
    return path;
}

scene::Document ball_with(const scene::Node& tendril, float r = 0.6f) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node ball;
    ball.prim = scene::Prim::sphere(r);
    l.sdf->insert(std::move(ball));
    scene::Node t = tendril;
    t.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.09f};
    l.sdf->insert(std::move(t));
    return doc;
}

SnakehookSettings settings_for(float base = 0.22f) {
    SnakehookSettings s;
    s.base_radius = base;
    return s;
}

}  // namespace

TEST_CASE("snakehook: a drag becomes a tendril") {
    auto node = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), settings_for());
    REQUIRE(node.has_value());
    CHECK(node->prim.type == scene::PrimType::Stroke);
    CHECK(node->stroke.size() == curling_drag().size() + 1);  // the anchor, then the drag

    scene::Tape tape = scene::compile_document(ball_with(*node));
    // Solid along the tendril, from inside the body out to near the tip.
    for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
        kernel::cfloat3 p = cf3(0.55f * std::sin(t * 1.9f), 0.55f + t * 0.85f, 0.25f * t * t);
        CAPTURE(t);
        CHECK(tape.eval(p).d < 0.0f);
    }
    // ...and empty well off it.
    CHECK(tape.eval(cf3(1.6f, 1.4f, 0)).d > 0.0f);
}

TEST_CASE("snakehook: the field stays exact") {
    // Unlike a loft or a sweep, a tendril costs the raymarcher nothing: the
    // stroke opcode is exact and a smooth union of exact fields stays usable.
    scene::Document plain;
    scene::Layer& l = plain.add_sdf_layer("l");
    scene::Node ball;
    ball.prim = scene::Prim::sphere(0.6f);
    l.sdf->insert(std::move(ball));
    const float before = kernel::csafe_step_scale(scene::compile_document(plain).info);

    auto node = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), settings_for());
    REQUIRE(node.has_value());
    const float after = kernel::csafe_step_scale(scene::compile_document(ball_with(*node)).info);

    INFO("step scale " << before << " -> " << after);
    CHECK(after == doctest::Approx(before));
}

TEST_CASE("snakehook: a tendril begins where the user touched") {
    const float r = 0.6f;
    // A drag whose first sample is already away from the picked point, which is
    // what a real one looks like: the pick reports the surface, and the first
    // sample arrives a frame later with the finger moving.
    std::vector<kernel::cfloat3> late = curling_drag();
    late.erase(late.begin());

    auto node = brush::snakehook(cf3(0, r, 0), cf3(0, -1, 0), late, settings_for());
    REQUIRE(node.has_value());
    CHECK(node->stroke.front().pos.x == doctest::Approx(0.0f));
    CHECK(node->stroke.front().pos.y == doctest::Approx(r));
    CHECK(node->stroke.front().radius == doctest::Approx(settings_for().base_radius));

    SUBCASE("and there is no break where it joins the body") {
        // Walked ALONG the path rather than straight up: this drag curls away
        // in x, so a vertical probe leaves the tendril within a few hundredths
        // and would be measuring empty space, not a gap.
        scene::Tape tape = scene::compile_document(ball_with(*node, r));
        CHECK(tape.eval(cf3(0, r - 0.2f, 0)).d < 0.0f);  // inside the body
        for (float t = 0.0f; t <= 0.3f; t += 0.01f) {
            kernel::cfloat3 p = cf3(0.55f * std::sin(t * 1.9f), 0.55f + t * 0.85f, 0.25f * t * t);
            CAPTURE(t);
            CHECK(tape.eval(p).d < 0.0f);
        }
    }
}

TEST_CASE("snakehook: the taper follows arc length, not sample count") {
    // A hand moves at an uneven speed. Tapering by index would let how fast the
    // gesture was decide how thick the tendril is.
    std::vector<kernel::cfloat3> even = curling_drag(12);

    // The same path, sampled with the points bunched at the start.
    std::vector<kernel::cfloat3> bunched;
    for (int i = 0; i < 6; ++i) {
        const float t = 0.12f * static_cast<float>(i) / 5.0f;
        bunched.push_back(cf3(0.55f * std::sin(t * 1.9f), 0.55f + t * 0.85f, 0.25f * t * t));
    }
    for (int i = 0; i < 8; ++i) {
        const float t = 0.12f + 0.88f * static_cast<float>(i) / 7.0f;
        bunched.push_back(cf3(0.55f * std::sin(t * 1.9f), 0.55f + t * 0.85f, 0.25f * t * t));
    }

    auto a = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), even, settings_for());
    auto b = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), bunched, settings_for());
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    scene::Tape ta = scene::compile_document(ball_with(*a));
    scene::Tape tb = scene::compile_document(ball_with(*b));
    float worst = 0.0f;
    for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
        kernel::cfloat3 p = cf3(0.55f * std::sin(t * 1.9f) + 0.12f, 0.55f + t * 0.85f,
                                0.25f * t * t);
        worst = std::max(worst, std::abs(ta.eval(p).d - tb.eval(p).d));
    }
    INFO("worst disagreement between an even and a bunched drag: " << worst);
    CHECK(worst < 0.03f);
}

TEST_CASE("snakehook: it tapers, and the tip is a point rather than a vanishing") {
    SnakehookSettings s = settings_for(0.25f);
    s.tip_fraction = 0.0f;  // ask for nothing at all
    s.min_tip_radius = 0.02f;
    auto node = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), s);
    REQUIRE(node.has_value());

    // Monotone from the base to the tip.
    for (std::size_t i = 2; i < node->stroke.size(); ++i)
        CHECK(node->stroke[i].radius <= node->stroke[i - 1].radius + 1e-5f);
    CHECK(node->stroke.front().radius == doctest::Approx(0.25f));
    CHECK(node->stroke.back().radius == doctest::Approx(0.02f));

    SUBCASE("and a fatter tip stays fatter the whole way") {
        SnakehookSettings fat = s;
        fat.tip_fraction = 0.6f;
        auto thick = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), fat);
        REQUIRE(thick.has_value());
        CHECK(thick->stroke.back().radius > node->stroke.back().radius);
    }

    SUBCASE("and the taper curve shapes it") {
        // The radius goes as (1 - t) raised to the curve, so ABOVE 1 thins away
        // quickly — a whip — and BELOW 1 holds the thickness and then drops,
        // which is the shape of a horn.
        const std::size_t mid = node->stroke.size() / 2;
        SnakehookSettings whip = s;
        whip.taper_curve = 3.0f;
        auto thin = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), whip);
        REQUIRE(thin.has_value());
        CHECK(thin->stroke[mid].radius < node->stroke[mid].radius);

        SnakehookSettings horn = s;
        horn.taper_curve = 0.4f;
        auto held = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), horn);
        REQUIRE(held.has_value());
        CHECK(held->stroke[mid].radius > node->stroke[mid].radius);
    }
}

TEST_CASE("snakehook: a tap still leaves a mark") {
    // The same rule resolve_stroke follows: a drag shorter than a step is a
    // small tendril rather than nothing.
    std::vector<kernel::cfloat3> tap = {cf3(0, 0.6f, 0), cf3(0, 0.61f, 0)};
    auto node = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), tap, settings_for());
    REQUIRE(node.has_value());
    CHECK(node->stroke.size() == 3);
    scene::Tape tape = scene::compile_document(ball_with(*node));
    CHECK(tape.eval(cf3(0, 0.62f, 0)).d < 0.0f);

    SUBCASE("even a single-point drag") {
        auto one = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), {cf3(0, 0.6f, 0)},
                                    settings_for());
        REQUIRE(one.has_value());
        CHECK(one->stroke.size() == 2);
    }
}

TEST_CASE("snakehook: degenerate input is refused where the item is built") {
    const std::vector<kernel::cfloat3> path = curling_drag();
    CHECK_FALSE(brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), {}, settings_for()).has_value());
    CHECK_FALSE(
        brush::snakehook(cf3(0, 0.6f, 0), cf3(0, 0, 0), path, settings_for()).has_value());

    SnakehookSettings zero = settings_for();
    zero.base_radius = 0.0f;
    CHECK_FALSE(brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), path, zero).has_value());
}

TEST_CASE("snakehook: the tendril is an ordinary item") {
    auto node = brush::snakehook(cf3(0, 0.6f, 0), cf3(0, -1, 0), curling_drag(), settings_for());
    REQUIRE(node.has_value());

    // It serializes and reloads like any other stroke, because it IS one.
    scene::Document doc = ball_with(*node);
    std::vector<std::uint8_t> bytes = scene::serialize_document(doc);
    auto back = scene::deserialize_document(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    scene::Tape before = scene::compile_document(doc);
    scene::Tape after = scene::compile_document(*back);
    for (float t = 0.0f; t <= 1.0f; t += 0.11f) {
        kernel::cfloat3 p = cf3(0.55f * std::sin(t * 1.9f), 0.55f + t * 0.85f, 0.25f * t * t);
        CHECK(after.eval(p).d == doctest::Approx(before.eval(p).d));
    }
}
