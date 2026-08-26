// Continuing a fold instead of replaying it (sdf-kernels spec, #306).
//
// A dab's cost follows everything already sculpted, because a dirty brick
// re-evaluates every surviving item over its samples even though almost none of
// them changed. `compile_layer_suffix` emits only the appended items, and
// `eval_points_seeded` runs them onto the value the rest produced.
//
// The assertion is IDENTITY. Continuing a fold from the number it reached is
// not an approximation of running it again -- it is the same instructions over
// the same floats -- so anything short of bit-equality means the suffix is not
// the suffix.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// A sculpt: a base and `dabs` blended dabs walked over its surface.
scene::Document sculpt(int dabs, bool mirror = false) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("s");
    if (mirror) {
        l.mirror_axes = scene::kMirrorX;
        l.mirror_k = 0.05f;
    }
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i <= dabs; ++i) {
        scene::Node d;
        d.prim = scene::Prim::sphere(0.18f);
        const float a = 0.4f * std::sin(static_cast<float>(i) * 0.9f);
        const float b = 0.4f * std::cos(static_cast<float>(i) * 1.4f);
        d.xform.position = cf3(std::sqrt(std::max(0.0f, 1.0f - a * a - b * b)), a, b);
        d.op = (i % 4 == 0) ? scene::Op::Subtract : scene::Op::Add;
        d.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.06f};
        l.sdf->insert(d);
    }
    return doc;
}

std::vector<float> lattice(int n) {
    std::vector<float> p;
    p.reserve(static_cast<std::size_t>(n) * n * n * 3);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                const float s = 2.4f / static_cast<float>(n - 1);
                p.push_back(-1.2f + s * static_cast<float>(i));
                p.push_back(-1.2f + s * static_cast<float>(j));
                p.push_back(-1.2f + s * static_cast<float>(k));
            }
    return p;
}

std::vector<float> eval_whole(const scene::Tape& tape, const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = out.size();
    eval::PointResults r;
    r.distances = out.data();
    eval::eval_points_blocked(tape, q, r);
    return out;
}

}  // namespace

TEST_CASE("a seeded suffix is the whole document, bit for bit") {
    const std::vector<float> pts = lattice(24);
    const std::size_t count = pts.size() / 3;

    for (bool mirror : {false, true}) {
        CAPTURE(mirror);
        // The document as it was before the last few dabs, and as it is now.
        const int kept = 3;
        scene::Document before = sculpt(20 - kept, mirror);
        scene::Document after = sculpt(20, mirror);

        scene::TapeCheckpoint cp;
        const scene::Tape prefix = scene::compile_document_resumable(before, &cp);
        REQUIRE(cp.valid);

        // The ids the two documents differ by, at the tail of the layer.
        const std::vector<scene::NodeId>& roots = after.layers[0].sdf->roots;
        REQUIRE(roots.size() >= static_cast<std::size_t>(kept));
        const std::vector<scene::NodeId> appended(roots.end() - kept, roots.end());

        scene::Tape suffix;
        REQUIRE(scene::compile_layer_suffix(cp, after, appended, &suffix, nullptr));
        REQUIRE(suffix.instrs.size() > 0);
        REQUIRE(suffix.instrs.size() < prefix.instrs.size());  // it IS only the tail

        // The seed: what the prefix says at each point.
        const std::vector<float> seed = eval_whole(prefix, pts);

        std::vector<float> got(count, 0.0f);
        eval::PointQuery q;
        q.points_xyz = pts.data();
        q.count = count;
        eval::PointResults r;
        r.distances = got.data();
        eval::eval_points_seeded(suffix, q, seed.data(), r);

        // Against the whole document, compiled and walked in full.
        const std::vector<float> want = eval_whole(scene::compile_document(after), pts);
        CHECK(std::memcmp(got.data(), want.data(), count * sizeof(float)) == 0);
    }
}

TEST_CASE("a seeded suffix of nothing is the seed") {
    // An empty suffix is the prefix unchanged. Not "far outside", which is what
    // an empty tape means to an evaluator whose stack starts empty -- the whole
    // difference a seed makes.
    const std::vector<float> pts = lattice(8);
    const scene::Document doc = sculpt(6);
    const scene::Tape whole = scene::compile_document(doc);
    const std::vector<float> seed = eval_whole(whole, pts);

    std::vector<float> got(seed.size(), 12345.0f);
    eval::PointQuery q;
    q.points_xyz = pts.data();
    q.count = got.size();
    eval::PointResults r;
    r.distances = got.data();
    eval::eval_points_seeded(scene::Tape{}, q, seed.data(), r);
    CHECK(std::memcmp(got.data(), seed.data(), seed.size() * sizeof(float)) == 0);
}

TEST_CASE("compile_layer_suffix refuses what it cannot be sure of") {
    // The same refusals compile_document_append makes, for the same reason: a
    // wrong reuse is silent, and a refusal costs the full evaluation the caller
    // would have paid anyway.
    scene::Document doc = sculpt(8);
    scene::TapeCheckpoint cp;
    scene::compile_document_resumable(doc, &cp);
    REQUIRE(cp.valid);
    const std::vector<scene::NodeId>& roots = doc.layers[0].sdf->roots;
    scene::Tape out;

    SUBCASE("no checkpoint") {
        scene::TapeCheckpoint none;
        CHECK_FALSE(scene::compile_layer_suffix(none, doc, {roots.back()}, &out, nullptr));
    }
    SUBCASE("nothing appended") {
        CHECK_FALSE(scene::compile_layer_suffix(cp, doc, {}, &out, nullptr));
    }
    SUBCASE("the ids are not the tail") {
        // A real id, in the wrong place.
        CHECK_FALSE(scene::compile_layer_suffix(cp, doc, {roots.front()}, &out, nullptr));
    }
    SUBCASE("more ids than the layer holds") {
        std::vector<scene::NodeId> too_many(roots.size() + 3, roots.back());
        CHECK_FALSE(scene::compile_layer_suffix(cp, doc, too_many, &out, nullptr));
    }
    SUBCASE("the layer the checkpoint names is gone") {
        scene::TapeCheckpoint stale = cp;
        stale.layer = 987654;
        CHECK_FALSE(scene::compile_layer_suffix(stale, doc, {roots.back()}, &out, nullptr));
    }
    SUBCASE("and the tail it does accept works") {
        CHECK(scene::compile_layer_suffix(cp, doc, {roots.back()}, &out, nullptr));
    }
}
