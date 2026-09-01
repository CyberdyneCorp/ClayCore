// Brush presets: the named families as data (add-shared-brush-kernels 6.x).
//
// The claim being tested is the brush-engine requirement's own: an
// artist-facing family is axis values over existing kernels and has no engine
// path of its own. If one of these needed a code change to express, the model
// would be missing an axis — so the library is the test.

#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "clay/brush/preset.h"

using namespace clay;
using namespace clay::kernel;
using brush::BrushPreset;

TEST_CASE("brush presets: the reference library covers the named families") {
    const std::vector<BrushPreset> lib = brush::reference_presets();
    REQUIRE(lib.size() >= 19);

    std::set<std::string> names;
    for (const BrushPreset& p : lib) {
        CAPTURE(p.name);
        CHECK_FALSE(p.name.empty());
        CHECK(names.insert(p.name).second);  // names are the library's keys
        CHECK(p.stroke.spacing > 0.0f);
        CHECK(p.stroke.radius > 0.0f);
        // Every preset resolves to a verb the vocabulary already has. A family
        // that needed a new one would be an engine path wearing a preset's
        // clothes.
        CHECK(static_cast<std::uint8_t>(p.model.verb) <=
              static_cast<std::uint8_t>(mesh::MeshBrush::Smear));
    }

    // The families the requirement names by hand.
    for (const char* wanted : {"Standard", "Clay", "Clay Buildup", "Clay Strips", "Inflate",
                               "Smooth", "Relax", "Move", "Move Topological", "Snake Hook",
                               "Pinch", "Dam Standard", "Flatten", "Scrape", "hPolish",
                               "Trim Dynamic", "Layer", "Nudge", "Rake"}) {
        CAPTURE(wanted);
        CHECK(brush::reference_preset(wanted).has_value());
    }
}

TEST_CASE("brush presets: the families that share a kernel differ only in axes") {
    // Clay and Clay Buildup are the row the model exists for: same kernel, same
    // frame, same footprint — a different STROKE. If these ever diverge in the
    // model, one of them has become a code path.
    const BrushPreset clay = *brush::reference_preset("Clay");
    const BrushPreset buildup = *brush::reference_preset("Clay Buildup");
    CHECK(clay.model.kernel == buildup.model.kernel);
    CHECK(clay.model.frame == buildup.model.frame);
    CHECK(clay.model.footprint == buildup.model.footprint);
    CHECK(buildup.stroke.spacing < clay.stroke.spacing);

    // Move and Move Topological differ in ONE axis — the footprint — which is
    // the whole of "measure the falloff along the surface".
    const BrushPreset move = *brush::reference_preset("Move");
    const BrushPreset topo = *brush::reference_preset("Move Topological");
    CHECK(move.model.kernel == topo.model.kernel);
    CHECK(topo.model.footprint == mesh::BrushFootprint::SurfaceWalk);
    CHECK(topo.settings.geodesic);

    // Trim Dynamic is Flatten with one setting changed: cut without fill.
    const BrushPreset trim = *brush::reference_preset("Trim Dynamic");
    const BrushPreset flatten = *brush::reference_preset("Flatten");
    CHECK(trim.model.kernel == flatten.model.kernel);
    CHECK(trim.settings.flatten_mode == field::FlattenMode::CutOnly);
    CHECK(flatten.settings.flatten_mode == field::FlattenMode::TwoSided);

    // Rake is Standard that follows the stylus barrel.
    const BrushPreset rake = *brush::reference_preset("Rake");
    CHECK(rake.model.kernel == mesh::model_of(mesh::MeshBrush::Draw).kernel);
    CHECK(rake.stroke.rotate_to_azimuth);
}

TEST_CASE("brush presets: a preset round-trips and resolves the same stroke") {
    std::vector<brush::StrokeSample> path;
    for (int i = 0; i < 16; ++i) {
        brush::StrokeSample s;
        s.position = cf3(-0.5f + 0.0625f * static_cast<float>(i), 0.0f, 0.0f);
        s.pressure = 0.5f + 0.03125f * static_cast<float>(i);
        path.push_back(s);
    }

    for (const BrushPreset& original : brush::reference_presets()) {
        CAPTURE(original.name);
        const std::vector<std::uint8_t> bytes = original.serialize();
        const std::optional<BrushPreset> back =
            BrushPreset::deserialize(bytes.data(), bytes.size());
        REQUIRE(back.has_value());

        CHECK(back->name == original.name);
        CHECK(back->model.verb == original.model.verb);
        CHECK(back->model.kernel == original.model.kernel);
        CHECK(back->model.frame == original.model.frame);
        CHECK(back->model.footprint == original.model.footprint);
        CHECK(back->settings.flatten_mode == original.settings.flatten_mode);
        CHECK(back->settings.polish_angle == original.settings.polish_angle);
        CHECK(back->settings.layer_height == original.settings.layer_height);
        CHECK(back->settings.strength == original.settings.strength);
        CHECK(back->settings.automask.factors == original.settings.automask.factors);

        // The round trip that matters is not field equality but BEHAVIOUR: the
        // stamps the two presets resolve have to be the same stamps.
        const std::vector<brush::Stamp> a = brush::resolve_stroke(path, original.stroke);
        const std::vector<brush::Stamp> b = brush::resolve_stroke(path, back->stroke);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].position.x == b[i].position.x);
            CHECK(a[i].radius == b[i].radius);
            CHECK(a[i].strength == b[i].strength);
            CHECK(a[i].deposit == b[i].deposit);
            CHECK(a[i].along == b[i].along);
        }
    }
}

TEST_CASE("brush presets: a newer version is refused, not reinterpreted") {
    const BrushPreset p = *brush::reference_preset("Standard");
    std::vector<std::uint8_t> bytes = p.serialize();
    REQUIRE(bytes.size() > 6);

    // Bump the version past what this build understands. Reading a prefix of a
    // newer layout gives a brush that is not the brush somebody saved, which is
    // worse than an error because it looks like it worked.
    bytes[4] = static_cast<std::uint8_t>(brush::kBrushPresetVersion + 1);
    CHECK_FALSE(BrushPreset::deserialize(bytes.data(), bytes.size()).has_value());

    // A wrong magic, a truncated buffer and an empty one are all refused too,
    // and none of them produces a partially populated preset.
    std::vector<std::uint8_t> wrong = p.serialize();
    wrong[0] ^= 0xFF;
    CHECK_FALSE(BrushPreset::deserialize(wrong.data(), wrong.size()).has_value());
    for (std::size_t cut = 1; cut < bytes.size(); cut += 7) {
        const std::vector<std::uint8_t> full = p.serialize();
        CHECK_FALSE(BrushPreset::deserialize(full.data(), cut).has_value());
    }
    CHECK_FALSE(BrushPreset::deserialize(nullptr, 0).has_value());
}

TEST_CASE("brush presets: no image bytes cross the format") {
    // A preset library must cost kilobytes and a host must own its own resource
    // cache, so alpha content is borrowed for a call and never embedded.
    const std::vector<float> alpha(64 * 64, 1.0f);
    BrushPreset p = *brush::reference_preset("Standard");
    p.settings.alpha = alpha.data();
    p.settings.alpha_width = 64;
    p.settings.alpha_height = 64;

    const std::vector<std::uint8_t> bytes = p.serialize();
    // 64x64 floats is 16 KB; the whole preset is a small fraction of one.
    CHECK(bytes.size() < 512);

    const std::optional<BrushPreset> back = BrushPreset::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(back->settings.alpha == nullptr);
    CHECK_FALSE(back->settings.has_alpha());
}

// -- the azimuth crosses the format (add-shared-brush-runtime) ----------------
//
// THE HOLE THIS CLOSES, stated as the hole rather than as the fix. Every
// reference preset has `stamp_azimuth == 0`, so the round-trip case above
// walks the whole library and cannot see a field that is never written: a
// preset carrying the default round-trips to the default whether the schema
// knows about it or not. That is how `stamp_azimuth` reached
// `MeshBrushSettings`, reached the C ABI, reached Python, and did not reach the
// one format an artist would save it in.
//
// The test therefore sets a NON-DEFAULT azimuth, which is the only value that
// distinguishes a schema that carries it from one that silently drops it.
TEST_CASE("brush presets: a turned brush stays turned across the format") {
    BrushPreset p = *brush::reference_preset("Standard");
    // Exactly representable, so the comparison below is `==` and not an
    // approximation: a format that loses the field gives 0, and a format that
    // rounds it is a different defect worth seeing separately.
    p.settings.stamp_azimuth = 0.75f;

    const std::vector<std::uint8_t> bytes = p.serialize();
    const std::optional<BrushPreset> back = BrushPreset::deserialize(bytes.data(), bytes.size());
    REQUIRE(back.has_value());
    CHECK(back->settings.stamp_azimuth == 0.75f);

    // And the default survives as the default rather than as a NaN or a
    // reinterpreted neighbour — the azimuth is the one field the engine
    // BRANCHES on at exactly zero (design D5), so a round trip that returned
    // -0.0f or 1e-45f would take the rotation path on a brush nobody turned.
    BrushPreset unturned = *brush::reference_preset("Standard");
    const std::vector<std::uint8_t> plain = unturned.serialize();
    const std::optional<BrushPreset> plain_back =
        BrushPreset::deserialize(plain.data(), plain.size());
    REQUIRE(plain_back.has_value());
    CHECK(plain_back->settings.stamp_azimuth == 0.0f);
    CHECK_FALSE(std::signbit(plain_back->settings.stamp_azimuth));
}

// The other half of a version bump, and the half that is easy to skip because
// version 1 was the only version this format had ever had: the header promises
// deserialize "accepts this version and any earlier one, taking defaults for
// whatever the older schema did not carry", and until now nothing could check
// it. A version-1 record is a version-2 record without its last four bytes.
TEST_CASE("brush presets: a version-1 record still loads, at the default azimuth") {
    BrushPreset p = *brush::reference_preset("Rake");
    p.settings.stamp_azimuth = 1.25f;

    std::vector<std::uint8_t> v1 = p.serialize();
    REQUIRE(v1.size() > 6 + sizeof(float));
    // Take the appended field away and say so in the version, which is exactly
    // what a library saved by the previous build holds.
    v1.resize(v1.size() - sizeof(float));
    v1[4] = 1;
    v1[5] = 0;

    const std::optional<BrushPreset> back = BrushPreset::deserialize(v1.data(), v1.size());
    REQUIRE(back.has_value());
    // The default, not the 1.25f the newer record carried and not a refusal:
    // an unrotated basis is what a version-1 preset was in fact saved with.
    CHECK(back->settings.stamp_azimuth == 0.0f);
    // Everything version 1 DID carry is unaffected — a truncation that shifted
    // the earlier fields would show up here rather than as a missing azimuth.
    CHECK(back->name == p.name);
    CHECK(back->model.verb == p.model.verb);
    CHECK(back->model.frame == p.model.frame);
    CHECK(back->settings.strength == p.settings.strength);
    CHECK(back->settings.automask.factors == p.settings.automask.factors);

    // A version-1 record that is ALSO short of its own fields is still refused.
    // The version gate must not become a way to smuggle a truncated buffer past
    // the `r.ok` check.
    for (std::size_t cut = 8; cut < v1.size(); cut += 5)
        CHECK_FALSE(BrushPreset::deserialize(v1.data(), cut).has_value());
}
