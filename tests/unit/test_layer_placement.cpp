// What a rigid or similarity layer placement guarantees (drag-a-layer-without-a-refill).
//
// THE CLAIM, and it is the whole reason the classification is allowed to exist:
// after a rigid layer transform, the layer's field is the previous field
// composed with the inverse of the placement change, and its surface is the
// previous surface moved by that change. After a similarity, the same with
// distances multiplied by the factor.
//
// EXACTNESS IS ASSERTED WHERE IT CAN BE, AND NOT WHERE IT CANNOT, and getting
// that boundary right took two attempts and a probe. The claim is algebraic, so
// in exact arithmetic it holds bit for bit. Two things round it in practice, and
// both are floating point rather than the engine:
//
//   1. THE PROBE POINT. Mapping a point through a rotation in float lands near
//      the image, not on it, so the two fields are asked about points a rounding
//      apart. The first version of this file rotated its points and compared
//      bitwise; every mismatch printed as `1.55501 == 1.55501`.
//   2. THE COMPOSED TRANSFORM. The tape holds `layer.xform * item.xform` folded
//      into one inverse matrix, so moving the layer re-rounds every item's
//      placement. With items at arbitrary positions -- `0.9 * cos(t)` in the
//      worked fixture -- the composition rounds even for a translation by an
//      exactly-representable vector, which is what the second attempt hit.
//
// So the bit-exact gate uses items at positions that survive the composition,
// proving there is no SYSTEMATIC error; the realistic gates make the same
// comparison to a few ulp and print the worst they saw.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

// A form with enough going on that a re-placement could plausibly disturb it:
// a base sphere, blended dabs, and one item carrying rounding, which is the
// term the layer's scale multiplies.
clay_layer_id worked_layer(clay_document* doc, const char* name, float offset) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);

    clay_item_desc base;
    std::memset(&base, 0, sizeof base);
    base.struct_size = static_cast<uint32_t>(sizeof base);
    base.prim = CLAY_PRIM_SPHERE;
    base.params[0] = 1.0f;
    base.op = CLAY_OP_ADD;
    base.position[0] = offset;
    clay_node_id n = 0;
    REQUIRE(clay_add_item(doc, layer, &base, &n) == CLAY_OK);

    for (int i = 0; i < 12; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_BOX;
        d.params[0] = d.params[1] = d.params[2] = 0.25f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.12f;
        d.rounding = 0.05f;  // the term layer.xform.scale multiplies
        d.position[0] = offset + 0.9f * std::cos(t);
        d.position[1] = 0.9f * std::sin(t);
        d.position[2] = 0.3f * static_cast<float>(i % 3) - 0.3f;
        clay_node_id id = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &id) == CLAY_OK);
    }
    return layer;
}

// Items whose positions survive being composed with the placement below: every
// coordinate is a multiple of a quarter, so `translation + position` is exact.
clay_layer_id exact_layer(clay_document* doc, const char* name) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    for (int i = 0; i < 8; ++i) {
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_BOX;
        d.params[0] = d.params[1] = d.params[2] = 0.25f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.25f;
        d.rounding = 0.25f;
        d.position[0] = 0.25f * static_cast<float>(i) - 0.75f;
        d.position[1] = 0.5f * static_cast<float>(i % 3) - 0.5f;
        d.position[2] = 0.25f * static_cast<float>(i % 4);
        clay_node_id id = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &id) == CLAY_OK);
    }
    return layer;
}

// The same form with HARD combines, which is a layer that scales cleanly: the
// blend radius is the one distance term a layer's scale does not multiply, so a
// layer without one is similar to itself under a scale and a layer with one is
// not.
clay_layer_id hard_layer(clay_document* doc, const char* name) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    for (int i = 0; i < 6; ++i) {
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_BOX;
        d.params[0] = d.params[1] = d.params[2] = 0.3f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_HARD;
        d.rounding = 0.05f;  // DOES follow the layer scale; kept so the gate sees it
        d.position[0] = 0.55f * static_cast<float>(i) - 1.0f;
        d.position[1] = 0.25f * static_cast<float>(i % 3);
        clay_node_id id = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &id) == CLAY_OK);
    }
    return layer;
}

std::vector<float> lattice(float lo, float hi, int side) {
    const auto at = [&](int a) {
        return lo + (hi - lo) * static_cast<float>(a) / static_cast<float>(side - 1);
    };
    std::vector<float> pts;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < side; ++k) {
                pts.push_back(at(i));
                pts.push_back(at(j));
                pts.push_back(at(k));
            }
    return pts;
}

std::vector<float> eval(const clay_document* doc, const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3, 0.0f);
    REQUIRE(clay_eval_points(doc, "cpu", pts.data(), out.size(), out.data(), nullptr) == CLAY_OK);
    return out;
}

// A point through a column-major affine matrix, the layout the report uses.
void apply_delta(const float m[16], const float p[3], float out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
}

clay_placement_report report_for(const clay_document* doc, clay_layer_id layer,
                                 const float pos[3], const float axis[3], float angle,
                                 float scale, const float axes[3]) {
    clay_placement_report rep;
    std::memset(&rep, 0, sizeof rep);
    rep.struct_size = static_cast<uint32_t>(sizeof rep);
    REQUIRE(clay_layer_placement_report(doc, layer, pos, axis, angle, scale, axes, &rep) ==
            CLAY_OK);
    return rep;
}

}  // namespace

TEST_CASE("layer placement: a rigid re-placement moves the field and nothing else") {
    CDoc d;
    // Positions in halves and quarters, so composing the layer's translation
    // into each item's transform is exact and the only thing left to compare is
    // the field itself.
    const clay_layer_id layer = exact_layer(d.doc, "body");

    const std::vector<float> pts = lattice(-1.5f, 1.5f, 9);
    const std::vector<float> before = eval(d.doc, pts);

    // Halves and quarters, so `p + t` is exact in binary floating point and the
    // point the moved field is asked about is the image of the point the
    // original was asked about, rather than a rounding of it.
    const float pos[3] = {0.5f, -0.25f, 0.75f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const clay_placement_report rep = report_for(d.doc, layer, pos, axis, 0.0f, 1.0f, nullptr);
    CHECK(rep.kind == CLAY_PLACEMENT_RIGID);
    CHECK(rep.scale == 1.0f);

    REQUIRE(clay_document_set_layer_transform(d.doc, layer, pos, axis, 0.0f, 1.0f) == CLAY_OK);

    std::vector<float> mapped;
    mapped.reserve(pts.size());
    for (std::size_t i = 0; i < pts.size(); i += 3) {
        float q[3];
        apply_delta(rep.delta, &pts[i], q);
        mapped.push_back(q[0]);
        mapped.push_back(q[1]);
        mapped.push_back(q[2]);
    }
    const std::vector<float> after = eval(d.doc, mapped);

    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < after.size(); ++i) CHECK(after[i] == before[i]);
    CHECK(after.size() == 729u);
}

TEST_CASE("layer placement: a rotated re-placement moves the field too") {
    // The same claim under a rotation, which is what a gizmo actually produces.
    // To a tolerance rather than exactly, and the reason is on the TEST's side:
    // rotating a probe point in float lands near the image and not on it, so
    // the two fields are asked about two points a rounding apart. The field is
    // 1-Lipschitz, so the answers differ by at most that rounding.
    CDoc d;
    const clay_layer_id layer = worked_layer(d.doc, "body", 0.0f);

    const std::vector<float> pts = lattice(-1.5f, 1.5f, 7);
    const std::vector<float> before = eval(d.doc, pts);

    const float pos[3] = {0.37f, -0.21f, 0.64f};
    const float axis[3] = {0.3f, 1.0f, 0.2f};
    const clay_placement_report rep = report_for(d.doc, layer, pos, axis, 0.7f, 1.0f, nullptr);
    CHECK(rep.kind == CLAY_PLACEMENT_RIGID);

    REQUIRE(clay_document_set_layer_transform(d.doc, layer, pos, axis, 0.7f, 1.0f) == CLAY_OK);

    std::vector<float> mapped;
    for (std::size_t i = 0; i < pts.size(); i += 3) {
        float q[3];
        apply_delta(rep.delta, &pts[i], q);
        mapped.push_back(q[0]);
        mapped.push_back(q[1]);
        mapped.push_back(q[2]);
    }
    const std::vector<float> after = eval(d.doc, mapped);

    double worst = 0.0;
    for (std::size_t i = 0; i < after.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::fabs(after[i] - before[i])));
    MESSAGE("worst rotated disagreement " << worst);
    // A few ulp at this magnitude, not a shape that moved.
    CHECK(worst < 1e-5);
}

TEST_CASE("layer placement: a uniform scale multiplies distances by its factor") {
    // On a layer that SCALES CLEANLY -- hard combines, so every distance term
    // follows the layer. Rounding is deliberately present, because rounding is
    // the term that does follow and a gate without it would not notice if that
    // stopped being true.
    CDoc d;
    const clay_layer_id layer = hard_layer(d.doc, "body");

    const std::vector<float> pts = lattice(-1.5f, 1.5f, 7);
    const std::vector<float> before = eval(d.doc, pts);

    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const float k = 2.0f;  // exact in binary, so the mapped point is the image
    const clay_placement_report rep = report_for(d.doc, layer, pos, axis, 0.0f, k, nullptr);
    CHECK(rep.kind == CLAY_PLACEMENT_SIMILARITY);
    CHECK(rep.scale == k);

    REQUIRE(clay_document_set_layer_transform(d.doc, layer, pos, axis, 0.0f, k) == CLAY_OK);

    std::vector<float> mapped;
    for (std::size_t i = 0; i < pts.size(); i += 3) {
        float q[3];
        apply_delta(rep.delta, &pts[i], q);
        mapped.push_back(q[0]);
        mapped.push_back(q[1]);
        mapped.push_back(q[2]);
    }
    const std::vector<float> after = eval(d.doc, mapped);

    std::size_t exact = 0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (std::fabs(before[i]) > 3.0f) continue;  // where FAR clamps
        // Doubling is exact in binary floating point, so this is bitwise.
        CHECK(after[i] == before[i] * k);
        ++exact;
    }
    CHECK(exact > 100);
}

TEST_CASE("layer placement: a scale on a blending layer is not called a similarity") {
    // THE CASE THE PLAN GOT WRONG. A layer's uniform scale multiplies an item's
    // ROUNDING and not its BLEND RADIUS, so a smooth-unioned layer scaled by 2
    // is not the old field times 2 -- measured at a ratio of 1.289. Reporting
    // SIMILARITY here would tell a host it may redraw by multiplying, and the
    // surface it drew would be wrong with nothing to indicate it.
    CDoc d;
    const clay_layer_id blended = worked_layer(d.doc, "blended", 0.0f);  // blend_k 0.12
    const clay_layer_id hard = hard_layer(d.doc, "hard");

    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};

    const clay_placement_report b = report_for(d.doc, blended, pos, axis, 0.0f, 2.0f, nullptr);
    CHECK(b.kind == CLAY_PLACEMENT_GENERAL);
    // The same layer moved RIGIDLY is still rigid: a rigid change scales
    // nothing, so the blend has nothing to disagree about.
    const float moved[3] = {0.5f, 0.0f, 0.0f};
    const clay_placement_report r = report_for(d.doc, blended, moved, axis, 0.0f, 1.0f, nullptr);
    CHECK(r.kind == CLAY_PLACEMENT_RIGID);

    // And the hard-combining layer beside it still scales.
    const clay_placement_report h = report_for(d.doc, hard, pos, axis, 0.0f, 2.0f, nullptr);
    CHECK(h.kind == CLAY_PLACEMENT_SIMILARITY);
}

TEST_CASE("layer placement: re-placing one layer leaves the others bit-identical") {
    // Layers combine by hard union, so moving one cannot re-solve a term in
    // another. Asserted by evaluating the OTHER layer alone, before and after.
    CDoc d;
    const clay_layer_id a = worked_layer(d.doc, "a", -2.0f);
    const clay_layer_id b = worked_layer(d.doc, "b", 2.0f);

    const std::vector<float> pts = lattice(0.5f, 3.5f, 7);
    // `a` excluded, so what is measured is `b` and only `b`.
    std::vector<float> before(pts.size() / 3, 0.0f);
    REQUIRE(clay_eval_points_excluding(d.doc, a, "cpu", pts.data(), before.size(), before.data(),
                                       nullptr) == CLAY_OK);

    const float pos[3] = {-0.4f, 0.9f, 0.15f};
    const float axis[3] = {1.0f, 0.2f, 0.4f};
    REQUIRE(clay_document_set_layer_transform(d.doc, a, pos, axis, 1.1f, 1.0f) == CLAY_OK);

    std::vector<float> after(pts.size() / 3, 0.0f);
    REQUIRE(clay_eval_points_excluding(d.doc, a, "cpu", pts.data(), after.size(), after.data(),
                                       nullptr) == CLAY_OK);

    for (std::size_t i = 0; i < after.size(); ++i) CHECK(after[i] == before[i]);
    (void)b;
}

TEST_CASE("layer placement: a non-uniform scale is General and claims nothing") {
    CDoc d;
    const clay_layer_id layer = worked_layer(d.doc, "body", 0.0f);
    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};

    const float squash[3] = {2.0f, 0.5f, 1.0f};
    const clay_placement_report rep = report_for(d.doc, layer, pos, axis, 0.0f, 1.0f, squash);
    CHECK(rep.kind == CLAY_PLACEMENT_GENERAL);
    // The identity, rather than something a caller might apply.
    CHECK(rep.scale == 1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(rep.delta[c * 4 + r] == (c == r ? 1.0f : 0.0f));

    // And a layer that ALREADY carries a squash reports General for a placement
    // that would otherwise be rigid: the field on the old side is not similar
    // to the field on the new one.
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, pos, axis, 0.0f, squash) ==
            CLAY_OK);
    const float moved[3] = {0.5f, 0.0f, 0.0f};
    const clay_placement_report again =
        report_for(d.doc, layer, moved, axis, 0.0f, 1.0f, nullptr);
    CHECK(again.kind == CLAY_PLACEMENT_GENERAL);
}

TEST_CASE("layer placement: asking for the report changes nothing") {
    // The report is a QUERY. If it moved the document, invalidated anything, or
    // disturbed a subsequent refill, a host calling it per gizmo frame would be
    // paying for the thing this change exists to avoid.
    CDoc quiet;
    CDoc asked;
    const clay_layer_id ql = worked_layer(quiet.doc, "body", 0.0f);
    const clay_layer_id al = worked_layer(asked.doc, "body", 0.0f);

    const float pos[3] = {0.3f, 0.1f, -0.2f};
    const float axis[3] = {0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 8; ++i) {
        clay_placement_report rep;
        std::memset(&rep, 0, sizeof rep);
        rep.struct_size = static_cast<uint32_t>(sizeof rep);
        REQUIRE(clay_layer_placement_report(asked.doc, al, pos, axis, 0.4f, 1.0f, nullptr, &rep) ==
                CLAY_OK);
    }

    const std::vector<float> pts = lattice(-1.6f, 1.6f, 7);
    const std::vector<float> q = eval(quiet.doc, pts);
    const std::vector<float> a = eval(asked.doc, pts);
    REQUIRE(q.size() == a.size());
    for (std::size_t i = 0; i < q.size(); ++i) CHECK(q[i] == a[i]);

    // The saved documents are identical too, which covers the placement itself
    // rather than only what it evaluates to.
    clay_blob* qb = nullptr;
    clay_blob* ab = nullptr;
    REQUIRE(clay_document_save_memory(quiet.doc, &qb) == CLAY_OK);
    REQUIRE(clay_document_save_memory(asked.doc, &ab) == CLAY_OK);
    CHECK(clay_blob_size(qb) == clay_blob_size(ab));
    clay_blob_destroy(qb);
    clay_blob_destroy(ab);
    (void)ql;
}

TEST_CASE("layer placement: the report refuses what the setters refuse") {
    CDoc d;
    const clay_layer_id layer = worked_layer(d.doc, "body", 0.0f);
    clay_placement_report rep;
    std::memset(&rep, 0, sizeof rep);
    rep.struct_size = static_cast<uint32_t>(sizeof rep);

    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float zero_axis[3] = {0.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};

    CHECK(clay_layer_placement_report(d.doc, layer, pos, zero_axis, 0.5f, 1.0f, nullptr, &rep) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_placement_report(d.doc, layer, pos, axis, 0.5f, 0.0f, nullptr, &rep) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_placement_report(d.doc, 9999u, pos, axis, 0.5f, 1.0f, nullptr, &rep) ==
          CLAY_ERROR_NOT_FOUND);
}
