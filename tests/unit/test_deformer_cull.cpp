// A warp the region cannot reach is not emitted (scene-model spec, issue #452).
//
// THE COST THIS REMOVES. `clay_layer_move_surface` records a grab as a warp on
// every item it reaches, and a warp is evaluated per sample for the life of the
// document. Measured on a worked ball: 512 probes taken WELL AWAY from twelve
// grabs cost 3.20x what the same probes cost with none — 0.1173 ms against
// 0.3758 — and each of those grabs had touched only 8 of the document's 97
// items. The work was being done for samples that provably could not be
// affected by it, because `cregion_weight` is zero outside the grab's radius.
//
// WHAT IS AND IS NOT FIXED HERE. This drops such a warp from a CULLED tape,
// which is what the per-brick paths compile — a refill, and meshing with
// gradient normals. It does nothing for a whole-document compile, because there
// is no region to test against, and the linear cost the issue measures on
// `clay_eval_points` therefore stands. That is stated in the issue's own terms:
// a warp is a domain deformation and two of them do not compose into one, so
// the accumulation is a property of the representation rather than a defect.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// A sphere carrying `grabs` warps, all of them clustered near +Z.
scene::Document ball_with_grabs(int grabs) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node n;
    n.id = l.sdf->reserve_id();
    n.prim = scene::Prim::sphere(1.0f);
    for (int i = 0; i < grabs; ++i)
        n.deformers.push_back(scene::Deformer::grab(cf3(0.02f * static_cast<float>(i), 0.0f, 1.0f),
                                                    0.3f, cf3(0.0f, 0.0f, 0.03f)));
    l.sdf->insert(n);
    return doc;
}

// How many deformer records the tape's one item carries. The count sits in the
// parameter block at a fixed offset after the transform, the prim params and
// the repeat record — see `emit_prim`.
std::size_t deformer_count(const scene::Tape& tape) {
    REQUIRE(tape.instrs.size() >= 1);
    const std::size_t at = tape.instrs[0].param_offset + 12 + 5 + CLAY_TAPE_PRIM_PARAMS + 7;
    REQUIRE(at < tape.params.size());
    return static_cast<std::size_t>(tape.params[at]);
}

std::vector<cfloat3> points_in(const math::Aabb& box, int side) {
    std::vector<cfloat3> pts;
    const cfloat3 e = box.extent();
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < side; ++k)
                pts.push_back(cf3(box.min.x + e.x * static_cast<float>(i) / (side - 1),
                                  box.min.y + e.y * static_cast<float>(j) / (side - 1),
                                  box.min.z + e.z * static_cast<float>(k) / (side - 1)));
    return pts;
}

}  // namespace

TEST_CASE("deformer cull: a region no warp reaches carries no warps") {
    const scene::Document doc = ball_with_grabs(12);
    const scene::Tape whole = scene::compile_document(doc);
    CHECK(deformer_count(whole) == 12);

    // A brick-sized region on the far side of the ball from every grab.
    const float b = 0.16f;
    const math::Aabb far_region{cf3(-0.75f, -0.30f, 0.10f),
                                cf3(-0.75f + b, -0.30f + b, 0.10f + b)};
    scene::CullRegion cull{far_region};
    const scene::Tape culled = scene::compile_document(doc, &cull);
    CHECK(deformer_count(culled) == 0);

    // And the field is UNCHANGED there, which is the whole claim: the warps
    // were not merely far away, they were the identity.
    for (cfloat3 p : points_in(far_region, 7)) {
        const kernel::CTapeValue a = whole.eval(cf3(p.x, p.y, p.z));
        const kernel::CTapeValue k = culled.eval(cf3(p.x, p.y, p.z));
        CHECK(a.d == k.d);
    }
}

TEST_CASE("deformer cull: a region a warp does reach keeps it") {
    const scene::Document doc = ball_with_grabs(12);
    const scene::Tape whole = scene::compile_document(doc);

    // Straddling the grabs. Every one of them is within reach here, so the
    // culled tape must carry all twelve -- a cull that dropped one would be a
    // different surface exactly where the artist was working.
    const float b = 0.16f;
    const math::Aabb near_region{cf3(-0.05f, -0.05f, 0.95f),
                                 cf3(-0.05f + b, -0.05f + b, 0.95f + b)};
    scene::CullRegion cull{near_region};
    const scene::Tape culled = scene::compile_document(doc, &cull);
    CHECK(deformer_count(culled) == 12);

    for (cfloat3 p : points_in(near_region, 7)) {
        const kernel::CTapeValue a = whole.eval(cf3(p.x, p.y, p.z));
        const kernel::CTapeValue k = culled.eval(cf3(p.x, p.y, p.z));
        CHECK(a.d == k.d);
    }
}

TEST_CASE("deformer cull: a whole-document compile keeps every warp") {
    // There is no region to test against, so nothing may be dropped. This is
    // the compile `clay_eval_points` uses and the one a host uploads.
    for (int n : {0, 1, 12}) {
        const scene::Tape tape = scene::compile_document(ball_with_grabs(n));
        CHECK(deformer_count(tape) == static_cast<std::size_t>(n));
    }
}

TEST_CASE("deformer cull: the chain's order is respected") {
    // A warp that is KEPT may move the point, so the warp after it has to be
    // tested against more room than the region alone. The fixture makes that
    // load-bearing: the first grab reaches the region and displaces along +X by
    // more than the gap to the second, which does NOT reach the region itself.
    // Dropping the second would change the field.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node n;
    n.id = l.sdf->reserve_id();
    n.prim = scene::Prim::sphere(1.0f);
    n.deformers.push_back(scene::Deformer::grab(cf3(0.0f, 0.0f, 1.0f), 0.30f, cf3(0.5f, 0, 0)));
    n.deformers.push_back(scene::Deformer::grab(cf3(0.75f, 0.0f, 1.0f), 0.20f, cf3(0, 0, 0.05f)));
    l.sdf->insert(n);

    const scene::Tape whole = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.1f, -0.1f, 0.9f), cf3(0.1f, 0.1f, 1.1f)};
    scene::CullRegion cull{region};
    const scene::Tape culled = scene::compile_document(doc, &cull);

    for (cfloat3 p : points_in(region, 7)) {
        const kernel::CTapeValue a = whole.eval(cf3(p.x, p.y, p.z));
        const kernel::CTapeValue k = culled.eval(cf3(p.x, p.y, p.z));
        CHECK(a.d == k.d);
    }
}
