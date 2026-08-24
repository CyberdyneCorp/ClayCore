#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay.h"
#include "clay/brush/stroke.h"

// The three channels a tablet reports and a stroke sample could not carry
// (roadmap P1: "azimuth, velocity, timestamp — five floats, and cheapest before
// hosts depend on the current sample layout").
//
// AZIMUTH is the one that unlocks a capability rather than refining one: tilt
// says how far the stylus leans, azimuth says WHICH WAY, and without it a rake
// or chisel brush is not expressible at all.

using namespace clay;
using namespace clay::brush;

namespace {

std::vector<StrokeSample> line(int n, float spacing, float azimuth = 0.0f,
                               float velocity = 0.0f) {
    std::vector<StrokeSample> out;
    for (int i = 0; i < n; ++i) {
        StrokeSample s;
        s.position = kernel::cf3(static_cast<float>(i) * spacing, 0.0f, 0.0f);
        s.pressure = 1.0f;
        s.tilt = 0.5f;
        s.azimuth = azimuth;
        s.velocity = velocity;
        s.timestamp = static_cast<double>(i) * 0.01;
        out.push_back(s);
    }
    return out;
}

StrokePreset base() {
    StrokePreset p;
    p.radius = 0.1f;
    p.spacing = 0.5f;
    return p;
}

}  // namespace

TEST_CASE("stroke input: azimuth turns the stamp, where the path cannot") {
    // The capability azimuth exists for. The path here runs along +x for every
    // case, so any difference in the stamp's rotation is the BARREL and nothing
    // else — which is exactly what rotate_along_stroke could not express.
    StrokePreset p = base();
    p.rotate_to_azimuth = true;

    const std::vector<Stamp> east = resolve_stroke(line(6, 0.05f, 0.0f), p);
    const std::vector<Stamp> north = resolve_stroke(line(6, 0.05f, 1.5708f), p);
    REQUIRE(!east.empty());
    REQUIRE(east.size() == north.size());

    // Same path, same pressure, same everything but the barrel — and the
    // stamps face differently.
    bool differs = false;
    for (std::size_t i = 0; i < east.size(); ++i)
        if (std::fabs(east[i].rotation.x - north[i].rotation.x) > 1e-4f ||
            std::fabs(east[i].rotation.y - north[i].rotation.y) > 1e-4f ||
            std::fabs(east[i].rotation.z - north[i].rotation.z) > 1e-4f ||
            std::fabs(east[i].rotation.w - north[i].rotation.w) > 1e-4f)
            differs = true;
    CHECK(differs);
}

TEST_CASE("stroke input: the barrel wins over the path where both are asked for") {
    // They are two answers to one question and a stamp cannot face two ways, so
    // this is settled by construction rather than by validation.
    StrokePreset both = base();
    both.rotate_along_stroke = true;
    both.rotate_to_azimuth = true;

    StrokePreset barrel = base();
    barrel.rotate_to_azimuth = true;

    const std::vector<Stamp> a = resolve_stroke(line(6, 0.05f, 1.5708f), both);
    const std::vector<Stamp> b = resolve_stroke(line(6, 0.05f, 1.5708f), barrel);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].rotation.x == doctest::Approx(b[i].rotation.x));
        CHECK(a[i].rotation.w == doctest::Approx(b[i].rotation.w));
    }
}

TEST_CASE("stroke input: azimuth interpolates the SHORT way round the wrap") {
    // Lerping an angle is wrong across the wrap — 350 degrees and 10 degrees
    // average to 180, which points the stamp backwards. This is the case that
    // catches it: two samples either side of 0.
    StrokePreset p = base();
    p.rotate_to_azimuth = true;
    p.spacing = 0.2f;

    std::vector<StrokeSample> path = line(2, 0.4f);
    path[0].azimuth = -3.0f;  // just under +pi going one way
    path[1].azimuth = 3.0f;   // just under -pi going the other

    const std::vector<Stamp> stamps = resolve_stroke(path, p);
    REQUIRE(stamps.size() >= 3);
    // The midpoint must be near the wrap (|angle| ~ pi), not near 0. A naive
    // lerp would put it at 0, which is a stamp facing the opposite way.
    const Stamp& mid = stamps[stamps.size() / 2];
    // Recover the angle the stamp was aligned to: it rotated +x, so the x
    // component of the rotated axis is cos(angle).
    const math::Quat& q = mid.rotation;
    const float cos_angle = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    CHECK(cos_angle < 0.0f);  // near pi, not near 0
}

TEST_CASE("stroke input: velocity widens or narrows, and the sign is the caller's") {
    // Signed on purpose: a fast stroke is WIDER for a dry brush and THINNER for
    // an ink pen, and picking one would be wrong half the time.
    StrokePreset wider = base();
    wider.velocity_response.size = 1.0f;
    wider.velocity_response.reference = 1.0f;

    StrokePreset thinner = base();
    thinner.velocity_response.size = -0.5f;
    thinner.velocity_response.reference = 1.0f;

    const std::vector<Stamp> slow = resolve_stroke(line(6, 0.05f, 0.0f, 0.0f), wider);
    const std::vector<Stamp> fast = resolve_stroke(line(6, 0.05f, 0.0f, 1.0f), wider);
    const std::vector<Stamp> fast_thin = resolve_stroke(line(6, 0.05f, 0.0f, 1.0f), thinner);
    REQUIRE(!slow.empty());

    CHECK(fast[0].radius > slow[0].radius);       // dry brush
    CHECK(fast_thin[0].radius < slow[0].radius);  // ink pen
}

TEST_CASE("stroke input: a preset that asks for nothing is unaffected by the new channels") {
    // The compatibility claim, tested rather than asserted: the same path with
    // and without azimuth and velocity resolves identically when the preset
    // wants neither.
    const StrokePreset p = base();
    const std::vector<Stamp> plain = resolve_stroke(line(8, 0.05f, 0.0f, 0.0f), p);
    const std::vector<Stamp> rich = resolve_stroke(line(8, 0.05f, 2.0f, 5.0f), p);
    REQUIRE(plain.size() == rich.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        CHECK(plain[i].radius == doctest::Approx(rich[i].radius));
        CHECK(plain[i].strength == doctest::Approx(rich[i].strength));
        CHECK(plain[i].position.x == doctest::Approx(rich[i].position.x));
    }
}

TEST_CASE("stroke input: a version-1 preset still deserializes, taking the new defaults") {
    // Which are exactly "speed changes nothing and the stamp follows the path"
    // — the behaviour it had when it was written.
    StrokePreset p = base();
    p.rotate_to_azimuth = true;
    p.velocity_response.size = 0.5f;
    const std::vector<std::uint8_t> bytes = p.serialize();

    const std::optional<StrokePreset> back = StrokePreset::deserialize(bytes.data(), bytes.size());
    REQUIRE(back);
    CHECK(back->rotate_to_azimuth);
    CHECK(back->velocity_response.size == doctest::Approx(0.5f));

    // A version-1 stream is the same bytes with the v2 tail cut off and the
    // version rewritten.
    std::vector<std::uint8_t> v1(bytes.begin(), bytes.end() - 13);
    v1[0] = 1;
    v1[1] = 0;
    const std::optional<StrokePreset> old = StrokePreset::deserialize(v1.data(), v1.size());
    REQUIRE(old);
    CHECK_FALSE(old->rotate_to_azimuth);
    CHECK(old->velocity_response.size == doctest::Approx(0.0f));
}

TEST_CASE("c abi: the old flat resolve is sugar over the wider one") {
    // Widening the count*5 packing in place would change the stride under every
    // host already compiled against it, so the old call converts and delegates.
    clay_stroke_preset preset{};
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_stroke_preset_defaults(&preset) == CLAY_OK);
    preset.radius = 0.1f;
    preset.spacing = 0.5f;

    std::vector<float> flat;
    std::vector<clay_stroke_sample_full> wide;
    for (int i = 0; i < 6; ++i) {
        const float x = static_cast<float>(i) * 0.05f;
        flat.insert(flat.end(), {x, 0.0f, 0.0f, 1.0f, 0.5f});
        clay_stroke_sample_full s{};
        s.position[0] = x;
        s.pressure = 1.0f;
        s.tilt = 0.5f;
        wide.push_back(s);
    }

    std::size_t flat_count = 0, wide_count = 0;
    REQUIRE(clay_stroke_resolve(flat.data(), 6, &preset, nullptr, &flat_count) == CLAY_OK);
    REQUIRE(clay_stroke_resolve_full(wide.data(), 6, &preset, nullptr, &wide_count) == CLAY_OK);
    CHECK(flat_count == wide_count);
    CHECK(flat_count > 0);

    std::vector<clay_stamp> a(flat_count), b(wide_count);
    REQUIRE(clay_stroke_resolve(flat.data(), 6, &preset, a.data(), &flat_count) == CLAY_OK);
    REQUIRE(clay_stroke_resolve_full(wide.data(), 6, &preset, b.data(), &wide_count) == CLAY_OK);
    for (std::size_t i = 0; i < flat_count; ++i) {
        CHECK(a[i].position[0] == doctest::Approx(b[i].position[0]));
        CHECK(a[i].radius == doctest::Approx(b[i].radius));
    }
}
