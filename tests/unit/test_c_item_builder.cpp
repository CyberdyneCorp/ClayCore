#include <doctest/doctest.h>

#include <fstream>
#include <cstdio>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay/scene/tape.h"
#include "desc_version.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// The C ABI item builder (c-abi spec: item builder for composed edits). Each
// case authors an edit twice — once through the C boundary, once on the scene
// model the way the Python bindings' Layer.add does — and requires the two
// documents to evaluate identically. That is the parity standard the spec
// states: a C consumer is not restricted to a subset of what pyclay drives.

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

std::vector<float> sample_points() {
    std::vector<float> pts;
    clay_test::Lcg rng(4242);
    for (int i = 0; i < 200; ++i) {
        cfloat3 p = rng.vec3(-2.5f, 2.5f);
        pts.push_back(p.x);
        pts.push_back(p.y);
        pts.push_back(p.z);
    }
    return pts;
}

// A sampled field: distance and colour at each point, so parity covers what
// an item carries, not only its shape.
struct Field {
    std::vector<float> distance;
    std::vector<float> color;
};

Field c_eval(clay_document* doc, const std::vector<float>& pts) {
    Field f;
    f.distance.assign(pts.size() / 3, 0.0f);
    f.color.assign(pts.size(), 0.0f);
    REQUIRE(clay_eval_points(doc, nullptr, pts.data(), f.distance.size(), f.distance.data(),
                             f.color.data()) == CLAY_OK);
    return f;
}

Field ref_eval(const scene::Document& doc, const std::vector<float>& pts) {
    scene::Tape tape = scene::compile_document(doc);
    Field f;
    for (std::size_t i = 0; i < pts.size(); i += 3) {
        kernel::CTapeValue v = tape.eval(cf3(pts[i], pts[i + 1], pts[i + 2]));
        f.distance.push_back(v.d);
        f.color.push_back(v.color.x);
        f.color.push_back(v.color.y);
        f.color.push_back(v.color.z);
    }
    return f;
}

// Compares two sampled fields and says what diverged: the point, both values
// and how many samples disagree. Only the first few are printed.
int compare_fields(const Field& got, const Field& want, const std::vector<float>& pts,
                   const char* what) {
    REQUIRE(got.distance.size() == want.distance.size());
    int diverged = 0;
    for (std::size_t i = 0; i < got.distance.size(); ++i) {
        bool same = got.distance[i] == doctest::Approx(want.distance[i]).epsilon(1e-6);
        for (int c = 0; c < 3; ++c)
            same = same && got.color[i * 3 + c] == doctest::Approx(want.color[i * 3 + c])
                                                       .epsilon(1e-6);
        if (same) continue;
        if (++diverged > 5) continue;
        FAIL_CHECK(what << " differs at (" << pts[i * 3] << ", " << pts[i * 3 + 1] << ", "
                        << pts[i * 3 + 2] << "): C ABI d=" << got.distance[i] << " rgb("
                        << got.color[i * 3] << ", " << got.color[i * 3 + 1] << ", "
                        << got.color[i * 3 + 2] << "), scene model d=" << want.distance[i]
                        << " rgb(" << want.color[i * 3] << ", " << want.color[i * 3 + 1] << ", "
                        << want.color[i * 3 + 2] << ")");
    }
    return diverged;
}

void check_same_field(clay_document* built, const scene::Document& reference) {
    std::vector<float> pts = sample_points();
    Field a = c_eval(built, pts);
    Field b = ref_eval(reference, pts);
    int diverged = compare_fields(a, b, pts, "field");
    INFO("diverging samples: " << diverged << " of " << a.distance.size());
    CHECK(diverged == 0);
}

// A document holding one item, built through the C ABI.
struct CDoc {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;

    CDoc() { REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK); }
    ~CDoc() { clay_document_destroy(doc); }
};

// Its scene-model twin, for the same layer name and insertion order.
struct RefDoc {
    scene::Document doc;
    scene::Layer& layer;

    RefDoc() : layer(doc.add_sdf_layer("body")) {}
    void add(const scene::Node& n) { layer.sdf->insert(n); }
};

}  // namespace

TEST_CASE("a composed C edit evaluates like the same edit on the scene model") {
    CDoc built;
    float size[3] = {0.4f, 0.9f, 0.4f};
    clay_item* item = clay_item_create(CLAY_PRIM_BOX, size, 3);
    REQUIRE(item != nullptr);
    float position[3] = {0.1f, 0.2f, -0.3f};
    float axis[3] = {0.0f, 1.0f, 0.0f};
    float color[3] = {0.2f, 0.4f, 0.6f};
    float twist[1] = {0.7f};
    float taper[4] = {-1.0f, 1.0f, 1.0f, 0.5f};
    float spacing[3] = {1.5f, 1.5f, 1.5f};
    float counts[3] = {2.0f, 0.0f, 1.0f};
    REQUIRE(clay_item_set_position(item, position) == CLAY_OK);
    REQUIRE(clay_item_set_rotation(item, axis, 0.6f) == CLAY_OK);
    REQUIRE(clay_item_set_scale(item, 1.2f) == CLAY_OK);
    REQUIRE(clay_item_set_color(item, color) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(item, 0.05f) == CLAY_OK);
    REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, 0.1f) == CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TWIST, twist, 1, CLAY_EASE_LINEAR) ==
            CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TAPER, taper, 4, CLAY_EASE_LINEAR) ==
            CLAY_OK);
    REQUIRE(clay_item_set_repeat_grid(item, spacing, counts) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, &node) == CLAY_OK);
    CHECK(node != 0);
    clay_item_destroy(item);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::box(cf3(0.4f, 0.9f, 0.4f));
    n.xform.position = cf3(0.1f, 0.2f, -0.3f);
    n.xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.6f);
    n.xform.scale = 1.2f;
    n.color = cf3(0.2f, 0.4f, 0.6f);
    n.rounding = 0.05f;
    n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
    n.deformers = {scene::Deformer::twist(0.7f), scene::Deformer::taper(-1.0f, 1.0f, 1.0f, 0.5f)};
    n.repeat = scene::Repeat::grid_finite(1.5f, cf3(2, 0, 1));
    reference.add(n);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("a radial array and a subtract combine survive the boundary") {
    CDoc built;
    float r[1] = {0.5f};
    clay_item* base = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(clay_item_set_repeat_radial(base, 6, 1.3f) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, base, nullptr) == CLAY_OK);
    clay_item_destroy(base);

    float bar[2] = {0.2f, 2.0f};
    clay_item* cut = clay_item_create(CLAY_PRIM_CAPPED_CYLINDER, bar, 2);
    REQUIRE(clay_item_set_op(cut, CLAY_OP_SUBTRACT) == CLAY_OK);
    REQUIRE(clay_item_set_blend(cut, CLAY_BLEND_CUBIC, 0.08f) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, cut, nullptr) == CLAY_OK);
    clay_item_destroy(cut);

    RefDoc reference;
    scene::Node sphere;
    sphere.prim = scene::Prim::sphere(0.5f);
    sphere.repeat = scene::Repeat::radial(6, 1.3f);
    reference.add(sphere);
    scene::Node cylinder;
    cylinder.prim = scene::Prim::capped_cylinder(0.2f, 2.0f);
    cylinder.op = scene::Op::Subtract;
    cylinder.blend = scene::Blend{scene::BlendProfile::Cubic, 0.08f};
    reference.add(cylinder);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("the deformer chain applies in call order") {
    auto build = [](bool twist_first) {
        clay_document* doc = clay_document_create();
        clay_layer_id layer = 0;
        clay_add_sdf_layer(doc, "body", &layer);
        float size[3] = {0.3f, 1.0f, 0.3f};
        clay_item* item = clay_item_create(CLAY_PRIM_BOX, size, 3);
        float twist[1] = {1.1f};
        float bend[1] = {0.7f};
        if (twist_first) {
            REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TWIST, twist, 1, 0) == CLAY_OK);
            REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_BEND, bend, 1, 0) == CLAY_OK);
        } else {
            REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_BEND, bend, 1, 0) == CLAY_OK);
            REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TWIST, twist, 1, 0) == CLAY_OK);
        }
        REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);
        return doc;
    };

    std::vector<float> pts = sample_points();
    clay_document* twist_then_bend = build(true);
    clay_document* bend_then_twist = build(false);
    Field a = c_eval(twist_then_bend, pts);
    Field b = c_eval(bend_then_twist, pts);
    int differing = 0;
    float widest = 0.0f;
    for (std::size_t i = 0; i < a.distance.size(); ++i) {
        float gap = std::fabs(a.distance[i] - b.distance[i]);
        if (gap > widest) widest = gap;
        if (a.distance[i] != doctest::Approx(b.distance[i]).epsilon(1e-4)) ++differing;
    }
    // the warps do not commute, as through pyclay
    INFO("twist-then-bend vs bend-then-twist: " << differing << " of " << a.distance.size()
                                                << " samples differ, widest gap " << widest);
    CHECK(differing > 0);
    CHECK(widest > 1e-3f);
    clay_document_destroy(twist_then_bend);
    clay_document_destroy(bend_then_twist);

    // and the order that was called is the order that applies
    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::box(cf3(0.3f, 1.0f, 0.3f));
    n.deformers = {scene::Deformer::twist(1.1f), scene::Deformer::bend(0.7f)};
    reference.add(n);
    clay_document* again = build(true);
    check_same_field(again, reference.doc);
    clay_document_destroy(again);
}

TEST_CASE("variable-length payloads are copied into the builder") {
    CDoc built;
    float points[16] = {-0.8f, 0.0f, 0.0f, 0.20f, -0.2f, 0.4f, 0.0f, 0.15f,
                        0.3f,  0.1f, 0.0f, 0.18f, 0.9f,  0.5f, 0.0f, 0.10f};
    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(stroke != nullptr);
    REQUIRE(clay_item_set_stroke_points(stroke, points, 4) == CLAY_OK);
    REQUIRE(clay_item_set_stroke_blend_k(stroke, 0.05f) == CLAY_OK);
    for (int i = 0; i < 16; ++i) points[i] = 99.0f;  // the copy must be the builder's
    REQUIRE(clay_layer_add_item(built.doc, built.layer, stroke, nullptr) == CLAY_OK);
    clay_item_destroy(stroke);

    float depth[1] = {0.25f};
    float outline[10] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.6f, 0.2f, 0.0f, 0.7f, -0.6f, 0.3f};
    clay_item* lift = clay_item_create(CLAY_PRIM_EXTRUDE, depth, 1);
    REQUIRE(lift != nullptr);
    REQUIRE(clay_item_set_profile_polygon(lift, outline, 5) == CLAY_OK);
    float where[3] = {1.4f, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(lift, where) == CLAY_OK);
    for (int i = 0; i < 10; ++i) outline[i] = 99.0f;
    REQUIRE(clay_layer_add_item(built.doc, built.layer, lift, nullptr) == CLAY_OK);
    clay_item_destroy(lift);

    RefDoc reference;
    scene::Node chain;
    chain.prim = scene::Prim::stroke();
    chain.stroke = {{cf3(-0.8f, 0.0f, 0.0f), 0.20f},
                    {cf3(-0.2f, 0.4f, 0.0f), 0.15f},
                    {cf3(0.3f, 0.1f, 0.0f), 0.18f},
                    {cf3(0.9f, 0.5f, 0.0f), 0.10f}};
    chain.stroke_blend_k = 0.05f;
    reference.add(chain);
    scene::Node extrude;
    extrude.prim = scene::Prim::extrude(0.25f);
    extrude.profile = scene::Profile::polygon();
    extrude.profile_points = {kernel::cf2(-0.5f, -0.5f), kernel::cf2(0.5f, -0.5f),
                              kernel::cf2(0.6f, 0.2f), kernel::cf2(0.0f, 0.7f),
                              kernel::cf2(-0.6f, 0.3f)};
    extrude.xform.position = cf3(1.4f, 0.0f, 0.0f);
    reference.add(extrude);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("a lift profile and a transition op reach the document") {
    CDoc built;
    float r[1] = {0.9f};
    clay_item* base = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, base, nullptr) == CLAY_OK);
    clay_item_destroy(base);

    float offset[1] = {1.1f};
    float profile[1] = {0.3f};
    clay_item* revolve = clay_item_create(CLAY_PRIM_REVOLVE, offset, 1);
    REQUIRE(clay_item_set_profile(revolve, CLAY_PROFILE_CIRCLE, profile, 1) == CLAY_OK);
    REQUIRE(clay_item_set_op(revolve, CLAY_OP_TRANSITION_RADIAL) == CLAY_OK);
    REQUIRE(clay_item_set_transition_radial(revolve, 0.2f, 1.8f, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, revolve, nullptr) == CLAY_OK);
    clay_item_destroy(revolve);

    RefDoc reference;
    scene::Node sphere;
    sphere.prim = scene::Prim::sphere(0.9f);
    reference.add(sphere);
    scene::Node ring;
    ring.prim = scene::Prim::revolve(1.1f);
    ring.profile = scene::Profile::circle(0.3f);
    ring.op = scene::Op::TransitionRadial;
    ring.transition.r0 = 0.2f;
    ring.transition.r1 = 1.8f;
    reference.add(ring);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("the flat descriptor path is the builder with no modifiers") {
    CDoc flat;
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_TORUS;
    desc.params[0] = 0.8f;
    desc.params[1] = 0.2f;
    desc.position[0] = 0.3f;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.4f;
    desc.op = CLAY_OP_ADD;
    desc.blend = CLAY_BLEND_CHAMFER;
    desc.blend_k = 0.12f;
    desc.rounding = 0.03f;
    desc.color[0] = 0.5f;
    desc.color[1] = 0.1f;
    desc.color[2] = 0.9f;
    REQUIRE(clay_add_item(flat.doc, flat.layer, &desc, nullptr) == CLAY_OK);

    CDoc composed;
    float params[2] = {0.8f, 0.2f};
    clay_item* item = clay_item_create(CLAY_PRIM_TORUS, params, 2);
    float position[3] = {0.3f, 0.0f, 0.0f};
    float color[3] = {0.5f, 0.1f, 0.9f};
    REQUIRE(clay_item_set_position(item, position) == CLAY_OK);
    REQUIRE(clay_item_set_scale(item, 1.4f) == CLAY_OK);
    REQUIRE(clay_item_set_blend(item, CLAY_BLEND_CHAMFER, 0.12f) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(item, 0.03f) == CLAY_OK);
    REQUIRE(clay_item_set_color(item, color) == CLAY_OK);
    REQUIRE(clay_layer_add_item(composed.doc, composed.layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);

    std::vector<float> pts = sample_points();
    Field a = c_eval(flat.doc, pts);
    Field b = c_eval(composed.doc, pts);
    int diverged = 0;
    for (std::size_t i = 0; i < a.distance.size(); ++i) {
        if (a.distance[i] == b.distance[i]) continue;
        if (++diverged > 5) continue;
        FAIL_CHECK("flat descriptor and builder differ at ("
                   << pts[i * 3] << ", " << pts[i * 3 + 1] << ", " << pts[i * 3 + 2]
                   << "): flat " << a.distance[i] << ", builder " << b.distance[i]);
    }
    CHECK(diverged == 0);
}

TEST_CASE("one builder can be added to several layers") {
    CDoc built;
    clay_layer_id second = 0;
    REQUIRE(clay_add_sdf_layer(built.doc, "other", &second) == CLAY_OK);
    float r[1] = {0.4f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    float here[3] = {-0.7f, 0.0f, 0.0f};
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
    REQUIRE(clay_item_set_position(item, here) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, second, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);

    RefDoc reference;
    scene::Node a;
    a.prim = scene::Prim::sphere(0.4f);
    reference.add(a);
    scene::Layer& other = reference.doc.add_sdf_layer("other");
    scene::Node b;
    b.prim = scene::Prim::sphere(0.4f);
    b.xform.position = cf3(-0.7f, 0.0f, 0.0f);
    other.sdf->insert(b);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("the spec's composed edit: a primitive, twist then bend, a radial array") {
    // The scenario as the c-abi spec words it. Authoring it on the scene
    // model is the practical stand-in for authoring it through pyclay: the
    // Python bindings' Layer.add copies these same fields onto a Node.
    CDoc built;
    float size[3] = {0.25f, 0.9f, 0.25f};
    clay_item* item = clay_item_create(CLAY_PRIM_BOX, size, 3);
    REQUIRE(item != nullptr);
    float position[3] = {1.3f, 0.0f, 0.0f};
    float color[3] = {0.7f, 0.5f, 0.1f};
    float twist[1] = {0.9f};
    float bend[1] = {0.45f};
    REQUIRE(clay_item_set_position(item, position) == CLAY_OK);
    REQUIRE(clay_item_set_color(item, color) == CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TWIST, twist, 1, CLAY_EASE_LINEAR) ==
            CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_BEND, bend, 1, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_item_set_repeat_radial(item, 7, 1.3f) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::box(cf3(0.25f, 0.9f, 0.25f));
    n.xform.position = cf3(1.3f, 0.0f, 0.0f);
    n.color = cf3(0.7f, 0.5f, 0.1f);
    n.deformers = {scene::Deformer::twist(0.9f), scene::Deformer::bend(0.45f)};
    n.repeat = scene::Repeat::radial(7, 1.3f);
    reference.add(n);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("payload length is not fixed") {
    // Nothing in the builder caps a stroke or a polygon: both are as long as
    // the caller makes them, and both are copied in.
    const int kPoints = 512;
    const int kVertices = 300;
    std::vector<float> chain;
    for (int i = 0; i < kPoints; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kPoints - 1);
        chain.push_back(std::cos(t * 12.0f) * 0.8f);
        chain.push_back(t * 2.0f - 1.0f);
        chain.push_back(std::sin(t * 12.0f) * 0.8f);
        chain.push_back(0.05f + 0.1f * t);
    }
    std::vector<float> outline;
    for (int i = 0; i < kVertices; ++i) {
        float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(kVertices);
        float r = 0.6f + 0.1f * std::cos(a * 9.0f);  // a fluted disc, not a circle
        outline.push_back(r * std::cos(a));
        outline.push_back(r * std::sin(a));
    }

    CDoc built;
    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(stroke != nullptr);
    REQUIRE(clay_item_set_stroke_points(stroke, chain.data(), kPoints) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, stroke, nullptr) == CLAY_OK);
    clay_item_destroy(stroke);

    float depth[1] = {0.2f};
    float where[3] = {2.2f, 0.0f, 0.0f};
    clay_item* lift = clay_item_create(CLAY_PRIM_EXTRUDE, depth, 1);
    REQUIRE(lift != nullptr);
    REQUIRE(clay_item_set_profile_polygon(lift, outline.data(), kVertices) == CLAY_OK);
    REQUIRE(clay_item_set_position(lift, where) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, lift, nullptr) == CLAY_OK);
    clay_item_destroy(lift);

    RefDoc reference;
    scene::Node ref_stroke;
    ref_stroke.prim = scene::Prim::stroke();
    for (int i = 0; i < kPoints; ++i)
        ref_stroke.stroke.push_back(scene::StrokePoint{
            cf3(chain[i * 4], chain[i * 4 + 1], chain[i * 4 + 2]), chain[i * 4 + 3]});
    reference.add(ref_stroke);
    scene::Node ref_lift;
    ref_lift.prim = scene::Prim::extrude(0.2f);
    ref_lift.profile = scene::Profile::polygon();
    for (int i = 0; i < kVertices; ++i)
        ref_lift.profile_points.push_back(kernel::cf2(outline[i * 2], outline[i * 2 + 1]));
    ref_lift.xform.position = cf3(2.2f, 0.0f, 0.0f);
    reference.add(ref_lift);

    check_same_field(built.doc, reference.doc);
}

TEST_CASE("a descriptor that declares its size lands where the scene model does") {
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_ROUND_BOX;
    desc.params[0] = 0.5f;
    desc.params[1] = 0.3f;
    desc.params[2] = 0.4f;
    desc.params[3] = 0.06f;
    desc.position[1] = 0.2f;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.3f;
    desc.op = CLAY_OP_ADD;
    desc.blend = CLAY_BLEND_CIRCULAR;
    desc.blend_k = 0.07f;
    desc.rounding = 0.01f;
    desc.color[0] = 0.9f;
    desc.color[2] = 0.4f;
    desc.mirror = 0;

    CDoc built;
    REQUIRE(clay_add_item(built.doc, built.layer, &desc, nullptr) == CLAY_OK);
    // struct_size is not optional: a zeroed one is not "the original layout",
    // because a descriptor from ABI 0.1.0 is indistinguishable from it
    desc.struct_size = 0;
    CHECK(clay_add_item(built.doc, built.layer, &desc, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::round_box(cf3(0.5f, 0.3f, 0.4f), 0.06f);
    n.xform.position = cf3(0.0f, 0.2f, 0.0f);
    n.xform.scale = 1.3f;
    n.blend = scene::Blend{scene::BlendProfile::Circular, 0.07f};
    n.rounding = 0.01f;
    n.color = cf3(0.9f, 0.0f, 0.4f);
    reference.add(n);

    check_same_field(built.doc, reference.doc);
}

namespace {

// The ABI 0.1.0 layouts, spelled out rather than derived from clay.h — that
// is the point of the case below: a binary compiled against 0.1.0 carries
// these bytes, not today's. Both structs grew a leading struct_size in 0.2.0,
// which shifts every field, so such a caller must be rejected rather than
// read one slot late (and, since its object is shorter, read off the end).
struct ItemDescV0_1_0 {
    std::int32_t prim;
    float params[7];
    float position[3];
    float rotation[4];
    float scale;
    std::int32_t op;
    std::int32_t blend;
    float blend_k;
    float rounding;
    float color[3];
    std::int32_t mirror;
};

struct MeshParamsV0_1_0 {
    float voxel_size;
    std::int32_t resolution;
    std::int32_t decimate;
    float decimate_ratio;
};

static_assert(sizeof(ItemDescV0_1_0) == 96, "the ABI 0.1.0 clay_item_desc was 96 bytes");
static_assert(sizeof(MeshParamsV0_1_0) == 16, "the ABI 0.1.0 clay_mesh_params was 16 bytes");
static_assert(sizeof(ItemDescV0_1_0) < sizeof(clay_item_desc));
static_assert(sizeof(MeshParamsV0_1_0) < sizeof(clay_mesh_params));

}  // namespace

TEST_CASE("an ABI 0.1.0 descriptor is rejected, not misread") {
    CDoc built;
    // exactly as long as 0.1.0 made it, on the heap: reading past it is a
    // heap overflow the sanitizer build reports rather than a quiet misread
    std::vector<unsigned char> storage(sizeof(ItemDescV0_1_0), 0);
    auto* old_desc = reinterpret_cast<ItemDescV0_1_0*>(storage.data());
    old_desc->rotation[3] = 1.0f;
    old_desc->scale = 1.0f;

    // every 0.1.0 prim value: each puts a small integer where struct_size now
    // is, so none of them may be taken for a declared layout
    for (std::int32_t prim = 0; prim <= CLAY_PRIM_PYRAMID; ++prim) {
        CAPTURE(prim);
        old_desc->prim = prim;
        old_desc->params[0] = prim == CLAY_PRIM_SPHERE ? 0.0f : 1.0f;
        CHECK(clay_add_item(built.doc, built.layer,
                            reinterpret_cast<const clay_item_desc*>(old_desc),
                            nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }
    // nothing landed: the layer is still empty
    float at_origin[3] = {0.0f, 0.0f, 0.0f};
    float d = 0.0f;
    REQUIRE(clay_eval_points(built.doc, nullptr, at_origin, 1, &d, nullptr) == CLAY_OK);
    CHECK(d > 1e6f);

    // and the same for the meshing descriptor, whose first word was a voxel
    // size: a float's bit pattern is not a struct_size either
    float r[1] = {0.8f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, sphere, nullptr) == CLAY_OK);
    clay_item_destroy(sphere);
    std::vector<unsigned char> mesh_storage(sizeof(MeshParamsV0_1_0), 0);
    auto* old_params = reinterpret_cast<MeshParamsV0_1_0*>(mesh_storage.data());
    old_params->voxel_size = 0.02f;
    clay_mesh* mesh = nullptr;
    CHECK(clay_document_mesh(built.doc, reinterpret_cast<const clay_mesh_params*>(old_params),
                             &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
    old_params->voxel_size = 0.0f;  // "pick from resolution", a zero first word
    old_params->resolution = 64;
    CHECK(clay_document_mesh(built.doc, reinterpret_cast<const clay_mesh_params*>(old_params),
                             &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(mesh == nullptr);
}

namespace {

// A descriptor as it might ship, and the same one after fields were appended.
// No public descriptor has grown yet, so this stands in for the growth the
// convention exists to absorb.
struct SmallDesc {
    std::uint32_t struct_size;
    float a;
};

struct GrownDesc {
    std::uint32_t struct_size = 0;
    float a = 0.0f;
    float b = -1.0f;  // documented default of an appended field
    std::int32_t c = 7;
};

}  // namespace

TEST_CASE("the declared prefix is what the library reads") {
    // The rule the spec states, on a descriptor that really did grow: an
    // older caller's short declaration is read to its end and no further, and
    // a newer caller's unknown tail is ignored rather than misread.
    GrownDesc from_old;
    from_old.struct_size = sizeof(SmallDesc);
    from_old.a = 2.5f;
    from_old.b = 99.0f;  // bytes the caller never declared
    from_old.c = 99;
    GrownDesc out;
    REQUIRE(clay_abi::read_desc(&from_old, sizeof(SmallDesc), &out) == clay_abi::DescStatus::Ok);
    CHECK(out.a == 2.5f);
    CHECK(out.b == -1.0f);  // the undeclared tail takes its documented default
    CHECK(out.c == 7);
    CHECK(out.struct_size == sizeof(GrownDesc));

    GrownDesc from_new = from_old;
    from_new.struct_size = sizeof(GrownDesc) + 64;  // fields this build cannot name
    REQUIRE(clay_abi::read_desc(&from_new, sizeof(SmallDesc), &out) == clay_abi::DescStatus::Ok);
    CHECK(out.a == 2.5f);
    CHECK(out.b == 99.0f);  // everything this build knows is read
    CHECK(out.struct_size == sizeof(GrownDesc));

    GrownDesc bad = from_old;
    bad.struct_size = sizeof(SmallDesc) - 1;
    CHECK(clay_abi::read_desc(&bad, sizeof(SmallDesc), &out) == clay_abi::DescStatus::TooShort);
    bad.struct_size = 0;
    CHECK(clay_abi::read_desc(&bad, sizeof(SmallDesc), &out) == clay_abi::DescStatus::TooShort);
    // a float's bit pattern, which is what a pre-convention caller leaves there
    std::memcpy(&bad.struct_size, "\x0a\xd7\x23\x3d", 4);  // 0.04f
    CHECK(clay_abi::read_desc(&bad, sizeof(SmallDesc), &out) ==
          clay_abi::DescStatus::NotADescriptor);
}

TEST_CASE("a newer caller's longer descriptor is read to the fields this build knows") {
    // The reverse of the older-caller case: struct_size names a layout this
    // library has never seen, so the tail is ignored rather than misread.
    struct Padded {
        clay_item_desc desc;
        float tail[4];
    } padded;
    std::memset(&padded, 0, sizeof padded);
    padded.desc.struct_size = static_cast<uint32_t>(sizeof padded);
    padded.desc.prim = CLAY_PRIM_SPHERE;
    padded.desc.params[0] = 0.7f;
    padded.desc.rotation[3] = 1.0f;
    padded.desc.scale = 1.0f;
    for (int i = 0; i < 4; ++i) padded.tail[i] = 1234.5f;  // fields from the future

    CDoc built;
    REQUIRE(clay_add_item(built.doc, built.layer, &padded.desc, nullptr) == CLAY_OK);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::sphere(0.7f);
    // the flat descriptor carries an explicit colour, so a zeroed one is
    // black — unlike the builder, which leaves the scene model's default
    n.color = cf3(0.0f, 0.0f, 0.0f);
    reference.add(n);
    check_same_field(built.doc, reference.doc);
}

TEST_CASE("every failure returns a code and leaves the builder usable") {
    float size[3] = {0.4f, 0.4f, 0.4f};
    clay_item* item = clay_item_create(CLAY_PRIM_BOX, size, 3);
    REQUIRE(item != nullptr);

    // wrong parameter count, unknown enumerator, out-of-range value
    CHECK(clay_item_create(CLAY_PRIM_BOX, size, 2) == nullptr);
    CHECK(clay_item_create(CLAY_PRIM_LNORM_SPHERE + 1, size, 1) == nullptr);
    CHECK(clay_item_add_deformer(item, CLAY_DEFORM_DISPLACE, size, 1, 0) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_add_deformer(item, 99, size, 1, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_op(item, 255) == CLAY_ERROR_INVALID_ARGUMENT);  // Op::None is groups only
    CHECK(clay_item_set_blend(item, 5, 0.1f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_scale(item, -1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(std::strlen(clay_last_error()) > 0);

    // null handles are arguments, not crashes
    CHECK(clay_item_set_position(nullptr, size) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_item_set_color(item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_add_item(nullptr, 0, item, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // none of that disturbed the item: it still adds, and adds the box it was
    CDoc built;
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);
    clay_item_destroy(nullptr);  // destroying a null handle is a no-op

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::box(cf3(0.4f, 0.4f, 0.4f));
    reference.add(n);
    check_same_field(built.doc, reference.doc);
}

// -- one case per enumerator -------------------------------------------------

namespace {

// Every clay_prim value with parameters that describe a real shape, indexed
// by the enum. The C smoke test sweeps the same set for reachability from
// pure C; here each entry is built twice and the two fields compared, so a
// primitive whose parameters arrive wrong is caught rather than merely
// "finite and not constant".
struct ZooEntry {
    std::int32_t prim;
    std::size_t count;
    float params[7];
};

const ZooEntry kZoo[] = {{CLAY_PRIM_SPHERE, 1, {0.5f}},
                         {CLAY_PRIM_BOX, 3, {0.4f, 0.4f, 0.4f}},
                         {CLAY_PRIM_ROUND_BOX, 4, {0.4f, 0.3f, 0.3f, 0.05f}},
                         {CLAY_PRIM_BOX_FRAME, 4, {0.4f, 0.4f, 0.4f, 0.05f}},
                         {CLAY_PRIM_TORUS, 2, {0.5f, 0.15f}},
                         {CLAY_PRIM_CAPSULE, 7, {-0.3f, 0.0f, 0.0f, 0.3f, 0.0f, 0.0f, 0.2f}},
                         {CLAY_PRIM_CAPPED_CYLINDER, 2, {0.3f, 0.4f}},
                         {CLAY_PRIM_ROUNDED_CYLINDER, 3, {0.3f, 0.05f, 0.4f}},
                         {CLAY_PRIM_CAPPED_CONE, 3, {0.4f, 0.3f, 0.1f}},
                         {CLAY_PRIM_ROUND_CONE, 3, {0.3f, 0.1f, 0.5f}},
                         {CLAY_PRIM_ELLIPSOID, 3, {0.4f, 0.3f, 0.2f}},
                         {CLAY_PRIM_OCTAHEDRON, 1, {0.5f}},
                         {CLAY_PRIM_HEX_PRISM, 2, {0.3f, 0.2f}},
                         {CLAY_PRIM_PYRAMID, 1, {0.6f}},
                         {CLAY_PRIM_STROKE, 0, {0.0f}},
                         {CLAY_PRIM_EXTRUDE, 1, {0.2f}},
                         {CLAY_PRIM_REVOLVE, 1, {0.6f}},
                         {CLAY_PRIM_CAPPED_TORUS, 4, {0.8415f, 0.5403f, 0.5f, 0.15f}},
                         {CLAY_PRIM_LINK, 3, {0.3f, 0.4f, 0.15f}},
                         {CLAY_PRIM_CYLINDER_INFINITE, 3, {0.0f, 0.0f, 0.3f}},
                         {CLAY_PRIM_CONE, 3, {0.5f, 0.866f, 0.5f}},
                         {CLAY_PRIM_PLANE, 4, {0.0f, 1.0f, 0.0f, 2.0f}},
                         {CLAY_PRIM_CUT_SPHERE, 2, {0.5f, 0.1f}},
                         {CLAY_PRIM_CUT_HOLLOW_SPHERE, 3, {0.5f, 0.1f, 0.05f}},
                         {CLAY_PRIM_SOLID_ANGLE, 3, {0.5f, 0.866f, 0.5f}},
                         {CLAY_PRIM_TETRAHEDRON, 1, {0.4f}},
                         {CLAY_PRIM_DODECAHEDRON, 1, {0.4f}},
                         {CLAY_PRIM_ICOSAHEDRON, 1, {0.4f}},
                         {CLAY_PRIM_TRI_PRISM, 2, {0.3f, 0.2f}},
                         {CLAY_PRIM_OCTAHEDRON_CHEAP, 1, {0.4f}},
                         {CLAY_PRIM_LNORM_SPHERE, 2, {0.4f, 4.0f}}};

constexpr std::size_t kZooCount = sizeof kZoo / sizeof kZoo[0];
static_assert(kZooCount == CLAY_PRIM_LNORM_SPHERE + 1);

// The out-of-line payload the two lift primitives and the stroke need, on
// both sides of the comparison.
const float kZooChain[8] = {-0.4f, 0.0f, 0.0f, 0.15f, 0.4f, 0.0f, 0.0f, 0.15f};
const float kZooProfile[1] = {0.3f};

}  // namespace

TEST_CASE("every primitive builds the field the scene model builds") {
    for (std::size_t i = 0; i < kZooCount; ++i) {
        const ZooEntry& e = kZoo[i];
        CAPTURE(e.prim);
        REQUIRE(e.prim == static_cast<std::int32_t>(i));  // the table is indexed by the enum

        CDoc built;
        clay_item* one = clay_item_create(e.prim, e.count ? e.params : nullptr, e.count);
        REQUIRE(one != nullptr);
        scene::Node n;
        n.prim.type = static_cast<scene::PrimType>(e.prim);
        for (std::size_t k = 0; k < e.count; ++k) n.prim.params[k] = e.params[k];
        if (e.prim == CLAY_PRIM_STROKE) {
            REQUIRE(clay_item_set_stroke_points(one, kZooChain, 2) == CLAY_OK);
            n.stroke = {{cf3(kZooChain[0], kZooChain[1], kZooChain[2]), kZooChain[3]},
                        {cf3(kZooChain[4], kZooChain[5], kZooChain[6]), kZooChain[7]}};
        }
        if (e.prim == CLAY_PRIM_EXTRUDE || e.prim == CLAY_PRIM_REVOLVE) {
            REQUIRE(clay_item_set_profile(one, CLAY_PROFILE_HEXAGON, kZooProfile, 1) == CLAY_OK);
            n.profile = scene::Profile::hexagon(kZooProfile[0]);
        }
        REQUIRE(clay_layer_add_item(built.doc, built.layer, one, nullptr) == CLAY_OK);
        clay_item_destroy(one);

        RefDoc reference;
        reference.add(n);
        check_same_field(built.doc, reference.doc);
    }
}

TEST_CASE("every combine op builds the field the scene model builds") {
    // Including the eight extended modes, whose radius is blend_k and whose
    // channel half-width (groove, tongue) is the item's rounding.
    const std::int32_t kOps[] = {CLAY_OP_ADD,     CLAY_OP_SUBTRACT, CLAY_OP_INTERSECT,
                                 CLAY_OP_PAINT,   CLAY_OP_GROOVE,   CLAY_OP_TONGUE,
                                 CLAY_OP_PIPE,    CLAY_OP_ENGRAVE,  CLAY_OP_EMBOSS,
                                 CLAY_OP_INSET,   CLAY_OP_SHELL,    CLAY_OP_REPLACE};
    float base_params[1] = {0.9f};
    float item_params[3] = {0.35f, 0.35f, 1.2f};
    float where[3] = {0.4f, 0.0f, 0.0f};
    float rgb[3] = {0.2f, 0.7f, 0.3f};
    for (std::int32_t op : kOps) {
        CAPTURE(op);
        CDoc built;
        clay_item* base = clay_item_create(CLAY_PRIM_SPHERE, base_params, 1);
        REQUIRE(clay_layer_add_item(built.doc, built.layer, base, nullptr) == CLAY_OK);
        clay_item_destroy(base);
        clay_item* item = clay_item_create(CLAY_PRIM_BOX, item_params, 3);
        REQUIRE(item != nullptr);
        REQUIRE(clay_item_set_position(item, where) == CLAY_OK);
        REQUIRE(clay_item_set_color(item, rgb) == CLAY_OK);
        REQUIRE(clay_item_set_rounding(item, 0.06f) == CLAY_OK);
        REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, 0.12f) == CLAY_OK);
        REQUIRE(clay_item_set_op(item, op) == CLAY_OK);
        REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);

        RefDoc reference;
        scene::Node sphere;
        sphere.prim = scene::Prim::sphere(0.9f);
        reference.add(sphere);
        scene::Node n;
        n.prim = scene::Prim::box(cf3(0.35f, 0.35f, 1.2f));
        n.xform.position = cf3(0.4f, 0.0f, 0.0f);
        n.color = cf3(0.2f, 0.7f, 0.3f);
        n.rounding = 0.06f;
        n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.12f};
        n.op = static_cast<scene::Op>(op);
        reference.add(n);

        check_same_field(built.doc, reference.doc);
    }
}

TEST_CASE("both morphs carry their endpoints and their easing curve") {
    // The linear endpoints are asymmetric on purpose: swapping them, or
    // dropping the ease index, changes the field.
    float base_params[3] = {0.7f, 0.7f, 0.7f};
    float item_params[1] = {0.8f};
    float a[3] = {-1.2f, -0.4f, 0.3f};
    float b[3] = {0.9f, 1.5f, -0.2f};
    const std::int32_t kLinearEase = 2;  // smootherstep
    const std::int32_t kRadialEase = 4;  // out-quad

    auto build = [&](bool linear, std::int32_t ease) {
        clay_document* doc = clay_document_create();
        clay_layer_id layer = 0;
        REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
        clay_item* base = clay_item_create(CLAY_PRIM_BOX, base_params, 3);
        REQUIRE(clay_layer_add_item(doc, layer, base, nullptr) == CLAY_OK);
        clay_item_destroy(base);
        clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, item_params, 1);
        REQUIRE(item != nullptr);
        if (linear) {
            REQUIRE(clay_item_set_op(item, CLAY_OP_TRANSITION_LINEAR) == CLAY_OK);
            REQUIRE(clay_item_set_transition_linear(item, a, b, ease) == CLAY_OK);
        } else {
            REQUIRE(clay_item_set_op(item, CLAY_OP_TRANSITION_RADIAL) == CLAY_OK);
            REQUIRE(clay_item_set_transition_radial(item, 0.3f, 1.7f, ease) == CLAY_OK);
        }
        REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);
        return doc;
    };

    auto reference_for = [&](bool linear, std::uint8_t ease) {
        scene::Node n;
        n.prim = scene::Prim::sphere(0.8f);
        n.op = linear ? scene::Op::TransitionLinear : scene::Op::TransitionRadial;
        if (linear) {
            n.transition.a = cf3(a[0], a[1], a[2]);
            n.transition.b = cf3(b[0], b[1], b[2]);
        } else {
            n.transition.r0 = 0.3f;
            n.transition.r1 = 1.7f;
        }
        n.transition.ease = ease;
        return n;
    };

    for (bool linear : {true, false}) {
        CAPTURE(linear);
        std::int32_t ease = linear ? kLinearEase : kRadialEase;
        clay_document* built = build(linear, ease);
        RefDoc reference;
        scene::Node base;
        base.prim = scene::Prim::box(cf3(0.7f, 0.7f, 0.7f));
        reference.add(base);
        reference.add(reference_for(linear, static_cast<std::uint8_t>(ease)));
        check_same_field(built, reference.doc);

        // the curve is not decoration: the linear index gives a other field
        clay_document* eased_linearly = build(linear, CLAY_EASE_LINEAR);
        std::vector<float> pts = sample_points();
        Field with = c_eval(built, pts);
        Field without = c_eval(eased_linearly, pts);
        float widest = 0.0f;
        for (std::size_t i = 0; i < with.distance.size(); ++i)
            widest = std::fmax(widest, std::fabs(with.distance[i] - without.distance[i]));
        INFO("widest gap between ease " << ease << " and the linear curve: " << widest);
        CHECK(widest > 1e-3f);
        clay_document_destroy(built);
        clay_document_destroy(eased_linearly);
    }
}

TEST_CASE("a taper's easing curve reaches the document") {
    float size[3] = {0.4f, 1.0f, 0.4f};
    const std::int32_t kEase = 5;  // in-out quad
    float taper[4] = {-1.0f, 1.0f, 1.0f, 0.4f};

    auto build = [&](std::int32_t ease) {
        clay_document* doc = clay_document_create();
        clay_layer_id layer = 0;
        REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
        clay_item* item = clay_item_create(CLAY_PRIM_BOX, size, 3);
        REQUIRE(item != nullptr);
        REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TAPER, taper, 4, ease) == CLAY_OK);
        REQUIRE(clay_layer_add_item(doc, layer, item, nullptr) == CLAY_OK);
        clay_item_destroy(item);
        return doc;
    };

    clay_document* built = build(kEase);
    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::box(cf3(0.4f, 1.0f, 0.4f));
    n.deformers = {scene::Deformer::taper(-1.0f, 1.0f, 1.0f, 0.4f,
                                          static_cast<std::uint8_t>(kEase))};
    reference.add(n);
    check_same_field(built, reference.doc);

    clay_document* linear = build(CLAY_EASE_LINEAR);
    std::vector<float> pts = sample_points();
    Field a = c_eval(built, pts);
    Field b = c_eval(linear, pts);
    float widest = 0.0f;
    for (std::size_t i = 0; i < a.distance.size(); ++i)
        widest = std::fmax(widest, std::fabs(a.distance[i] - b.distance[i]));
    INFO("widest gap between ease " << kEase << " and the linear curve: " << widest);
    CHECK(widest > 1e-3f);
    clay_document_destroy(built);
    clay_document_destroy(linear);
}

TEST_CASE("the mirror flag applies through the layer's mirror axes") {
    CDoc built;
    REQUIRE(clay_set_layer_mirror(built.doc, built.layer, 1, 0, 0, 0.08f) == CLAY_OK);
    float size[3] = {0.3f, 0.5f, 0.3f};
    float where[3] = {0.9f, 0.1f, 0.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_BOX, size, 3);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_position(item, where) == CLAY_OK);
    REQUIRE(clay_item_set_mirror(item, 1) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);

    RefDoc reference;
    reference.layer.mirror_axes = scene::kMirrorX;
    reference.layer.mirror_k = 0.08f;
    scene::Node n;
    n.prim = scene::Prim::box(cf3(0.3f, 0.5f, 0.3f));
    n.xform.position = cf3(0.9f, 0.1f, 0.0f);
    n.mirror = true;
    reference.add(n);
    check_same_field(built.doc, reference.doc);

    // and an unmirrored item on the same layer is left alone
    CDoc plain;
    REQUIRE(clay_set_layer_mirror(plain.doc, plain.layer, 1, 0, 0, 0.08f) == CLAY_OK);
    clay_item* other = clay_item_create(CLAY_PRIM_BOX, size, 3);
    REQUIRE(clay_item_set_position(other, where) == CLAY_OK);
    REQUIRE(clay_item_set_mirror(other, 0) == CLAY_OK);
    REQUIRE(clay_layer_add_item(plain.doc, plain.layer, other, nullptr) == CLAY_OK);
    clay_item_destroy(other);
    std::vector<float> pts = sample_points();
    Field mirrored = c_eval(built.doc, pts);
    Field single = c_eval(plain.doc, pts);
    float widest = 0.0f;
    for (std::size_t i = 0; i < mirrored.distance.size(); ++i)
        widest = std::fmax(widest, std::fabs(mirrored.distance[i] - single.distance[i]));
    INFO("widest gap between a mirrored and an unmirrored item: " << widest);
    CHECK(widest > 1e-3f);
}

TEST_CASE("an infinite grid repeats the item forever") {
    CDoc built;
    float r[1] = {0.3f};
    float spacing[3] = {1.1f, 1.3f, 0.9f};  // an infinite grid may differ per axis
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_repeat_grid(item, spacing, nullptr) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::sphere(0.3f);
    n.repeat = scene::Repeat::grid_infinite(cf3(1.1f, 1.3f, 0.9f));
    reference.add(n);
    check_same_field(built.doc, reference.doc);  // sampled, not meshed: it is unbounded
}

TEST_CASE("a stroke built one point at a time is the stroke that was called for") {
    CDoc built;
    const float chain[3][4] = {{-0.6f, 0.0f, 0.0f, 0.20f},
                               {0.0f, 0.35f, 0.1f, 0.15f},
                               {0.7f, 0.1f, -0.2f, 0.10f}};
    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(stroke != nullptr);
    for (const auto& p : chain) {
        float position[3] = {p[0], p[1], p[2]};
        REQUIRE(clay_item_add_stroke_point(stroke, position, p[3]) == CLAY_OK);
    }
    REQUIRE(clay_item_set_stroke_blend_k(stroke, 0.06f) == CLAY_OK);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, stroke, nullptr) == CLAY_OK);
    clay_item_destroy(stroke);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::stroke();
    for (const auto& p : chain)
        n.stroke.push_back(scene::StrokePoint{cf3(p[0], p[1], p[2]), p[3]});
    n.stroke_blend_k = 0.06f;
    reference.add(n);
    check_same_field(built.doc, reference.doc);
}

TEST_CASE("a plane's normal is normalized, as the engine's own constructor does") {
    // An unnormalized normal is not a distance field: it scales the whole
    // field, and a plane is never culled, so it would corrupt every other
    // item. pyclay's Plane normalizes; a C author must land on the same one.
    float unnormalized[4] = {0.0f, 2.0f, 0.0f, 0.0f};
    CDoc built;
    clay_item* item = clay_item_create(CLAY_PRIM_PLANE, unnormalized, 4);
    REQUIRE(item != nullptr);
    REQUIRE(clay_layer_add_item(built.doc, built.layer, item, nullptr) == CLAY_OK);
    clay_item_destroy(item);

    CDoc flat;  // the flat descriptor is sugar over the builder, so it agrees
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_PLANE;
    desc.params[1] = 2.0f;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    REQUIRE(clay_add_item(flat.doc, flat.layer, &desc, nullptr) == CLAY_OK);

    RefDoc reference;
    scene::Node n;
    n.prim = scene::Prim::plane(cf3(0.0f, 2.0f, 0.0f), 0.0f);
    n.color = cf3(0.0f, 0.0f, 0.0f);  // the flat descriptor's zeroed colour
    reference.add(n);
    check_same_field(flat.doc, reference.doc);
    scene::Node lit;
    lit.prim = scene::Prim::plane(cf3(0.0f, 2.0f, 0.0f), 0.0f);
    RefDoc builder_reference;
    builder_reference.add(lit);
    check_same_field(built.doc, builder_reference.doc);

    // a direction of no length, and an angle pair that is not one
    float no_direction[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    CHECK(clay_item_create(CLAY_PRIM_PLANE, no_direction, 4) == nullptr);
    float not_an_angle[3] = {0.5f, 0.5f, 0.5f};
    CHECK(clay_item_create(CLAY_PRIM_CONE, not_an_angle, 3) == nullptr);
    CHECK(clay_item_create(CLAY_PRIM_SOLID_ANGLE, not_an_angle, 3) == nullptr);
    float not_an_aperture[4] = {0.5f, 0.5f, 0.5f, 0.15f};
    CHECK(clay_item_create(CLAY_PRIM_CAPPED_TORUS, not_an_aperture, 4) == nullptr);
}

// --- editing a placed node (add-edit-commands) -------------------------------

namespace {

// A document with one sphere, plus the node id, through the C ABI.
struct EditFixture {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    clay_node_id node = 0;

    EditFixture() {
        REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
        float params[1] = {0.5f};
        clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
        REQUIRE(item != nullptr);
        REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
        clay_item_destroy(item);
    }
    ~EditFixture() { clay_document_destroy(doc); }

    float at(float x, float y, float z) const {
        float p[3] = {x, y, z};
        float d = 0.0f;
        REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
        return d;
    }
};

const float kOrigin[3] = {0, 0, 0};
const float kUpAxis[3] = {0, 1, 0};

}  // namespace

TEST_CASE("a placed node can be retransformed, and keeps its id") {
    EditFixture f;
    CHECK(f.at(0, 0, 0) < 0.0f);
    CHECK(f.at(2, 0, 0) > 0.0f);

    const float moved[3] = {2, 0, 0};
    REQUIRE(clay_layer_set_transform(f.doc, f.layer, f.node, moved, kUpAxis, 0.0f, 1.0f) ==
            CLAY_OK);
    CHECK(f.at(2, 0, 0) < 0.0f);
    CHECK(f.at(0, 0, 0) > 0.0f);

    // the same id still resolves, which is what lets a UI hold a selection
    const float red[3] = {1, 0, 0};
    CHECK(clay_layer_set_color(f.doc, f.layer, f.node, red) == CLAY_OK);
}

TEST_CASE("editing a node matches the same edit through the scene API") {
    EditFixture f;
    const float moved[3] = {1.25f, -0.5f, 0.75f};
    REQUIRE(clay_layer_set_transform(f.doc, f.layer, f.node, moved, kUpAxis, 0.7f, 1.5f) ==
            CLAY_OK);
    REQUIRE(clay_layer_set_op_blend(f.doc, f.layer, f.node, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                    0.2f, 0.05f) == CLAY_OK);

    // the twin, built with the engine's own types
    scene::Document twin;
    scene::Layer& tl = twin.add_sdf_layer("body");
    scene::Node n = clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
    n.xform.position = cf3(1.25f, -0.5f, 0.75f);
    n.xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.7f);
    n.xform.scale = 1.5f;
    n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.2f};
    n.rounding = 0.05f;
    tl.sdf->insert(n);

    check_same_field(f.doc, twin);
}

TEST_CASE("replacing a primitive keeps the node's modifiers") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);

    float params[1] = {0.3f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, params, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_repeat_radial(item, 6, 1.0f) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    // CLAY_PRIM_BOX params are half-extents; pyclay's Box(size=) takes full
    // side lengths, so 0.2 here is the same box as clay.Box(size=(0.4,...)).
    float box[3] = {0.2f, 0.2f, 0.2f};
    REQUIRE(clay_layer_set_prim(doc, layer, node, CLAY_PRIM_BOX, box, 3) == CLAY_OK);

    // The repetition belongs to the node, so it survives: a lone box would
    // read -0.2 at the origin, and the array does not.
    float p[3] = {0, 0, 0};
    float d = 0.0f;
    REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
    CHECK(d == doctest::Approx(-0.2f).epsilon(1e-4));
    clay_document_destroy(doc);
}

TEST_CASE("a stroke can be extended and trimmed in place") {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "l", &layer) == CLAY_OK);

    clay_item* item = clay_item_create(CLAY_PRIM_STROKE, nullptr, 0);
    REQUIRE(item != nullptr);
    const float seed[8] = {-1, 0, 0, 0.3f, 0, 0, 0, 0.3f};
    REQUIRE(clay_item_set_stroke_points(item, seed, 2) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK);
    clay_item_destroy(item);

    auto at = [&](float x) {
        float p[3] = {x, 0, 0};
        float d = 0.0f;
        REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
        return d;
    };
    CHECK(at(1.0f) > 0.0f);

    const float extra[4] = {1.0f, 0.0f, 0.0f, 0.3f};
    REQUIRE(clay_layer_append_stroke(doc, layer, node, extra, 1) == CLAY_OK);
    CHECK(at(1.0f) < 0.0f);

    REQUIRE(clay_layer_trim_stroke(doc, layer, node, 1) == CLAY_OK);
    CHECK(at(1.0f) > 0.0f);
    clay_document_destroy(doc);
}

TEST_CASE("layer visibility, transform and removal") {
    clay_document* doc = clay_document_create();
    clay_layer_id a = 0, b = 0;
    REQUIRE(clay_add_sdf_layer(doc, "a", &a) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(doc, "b", &b) == CLAY_OK);

    float r[1] = {1.0f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, a, sphere, &node) == CLAY_OK);
    clay_item_destroy(sphere);

    auto origin = [&]() {
        float p[3] = {0, 0, 0};
        float d = 0.0f;
        REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
        return d;
    };
    float visible = origin();
    CHECK(visible < 0.0f);

    REQUIRE(clay_document_set_layer_visible(doc, a, 0) == CLAY_OK);
    CHECK(origin() > 0.0f);
    REQUIRE(clay_document_set_layer_visible(doc, a, 1) == CLAY_OK);
    CHECK(origin() == doctest::Approx(visible));  // restored exactly

    const float lifted[3] = {0, 4, 0};
    REQUIRE(clay_document_set_layer_transform(doc, a, lifted, kUpAxis, 0.0f, 1.0f) == CLAY_OK);
    CHECK(origin() > 0.0f);

    REQUIRE(clay_document_move_layer(doc, a, 0) == CLAY_OK);
    REQUIRE(clay_document_remove_layer(doc, a) == CLAY_OK);
    CHECK(clay_document_set_layer_visible(doc, a, 1) == CLAY_ERROR_NOT_FOUND);
    clay_document_destroy(doc);
}

TEST_CASE("an unknown id is refused and changes nothing") {
    EditFixture f;
    const clay_node_id ghost = 9999;
    float before = f.at(0, 0, 0);

    CHECK(clay_layer_set_transform(f.doc, f.layer, ghost, kOrigin, kUpAxis, 0.0f, 1.0f) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_set_color(f.doc, f.layer, ghost, kOrigin) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_set_op_blend(f.doc, f.layer, ghost, CLAY_OP_ADD, CLAY_BLEND_HARD, 0, 0) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_trim_stroke(f.doc, f.layer, ghost, 1) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_move(f.doc, f.layer, ghost, 0, -1) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_remove_layer(f.doc, 9999) == CLAY_ERROR_NOT_FOUND);

    CHECK(f.at(0, 0, 0) == doctest::Approx(before));  // document untouched

    // and a bad value is rejected before anything is applied
    CHECK(clay_layer_set_transform(f.doc, f.layer, f.node, kOrigin, kUpAxis, 0.0f, 0.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_op_blend(f.doc, f.layer, f.node, CLAY_OP_ADD, CLAY_BLEND_HARD, 0,
                                  -1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(f.at(0, 0, 0) == doctest::Approx(before));
}

// --- undo (add-undo-stack) ---------------------------------------------------

namespace {

// The strict form of "the document is unchanged": its serialized bytes.
std::vector<std::uint8_t> doc_bytes(clay_document* doc) {
    const char* path = "c_undo_snapshot.clayspace";
    REQUIRE(clay_document_save(doc, path) == CLAY_OK);
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    in.close();
    std::remove(path);
    return bytes;
}

}  // namespace

TEST_CASE("undo is off by default and edits still work") {
    EditFixture f;
    std::int32_t enabled = 1;
    std::size_t undo_depth = 99, redo_depth = 99;
    REQUIRE(clay_document_undo_state(f.doc, &enabled, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(enabled == 0);
    CHECK(undo_depth == 0);
    CHECK(redo_depth == 0);

    const float red[3] = {1, 0, 0};
    CHECK(clay_layer_set_color(f.doc, f.layer, f.node, red) == CLAY_OK);

    std::int32_t undone = 0;
    CHECK(clay_document_undo(f.doc, &undone) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("undo restores the document byte for byte") {
    EditFixture f;
    REQUIRE(clay_document_enable_undo(f.doc) == CLAY_OK);
    std::vector<std::uint8_t> before = doc_bytes(f.doc);

    const float moved[3] = {2, 0, 0};
    REQUIRE(clay_layer_set_transform(f.doc, f.layer, f.node, moved, kUpAxis, 0.0f, 1.0f) ==
            CLAY_OK);
    CHECK(doc_bytes(f.doc) != before);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(f.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    CHECK(doc_bytes(f.doc) == before);

    std::size_t undo_depth = 99, redo_depth = 99;
    REQUIRE(clay_document_undo_state(f.doc, nullptr, &undo_depth, &redo_depth) == CLAY_OK);
    CHECK(undo_depth == 0);
    CHECK(redo_depth == 1);

    std::int32_t redone = 0;
    REQUIRE(clay_document_redo(f.doc, &redone) == CLAY_OK);
    CHECK(redone == 1);
    CHECK(doc_bytes(f.doc) != before);
}

TEST_CASE("an empty undo stack reports rather than fails") {
    EditFixture f;
    REQUIRE(clay_document_enable_undo(f.doc) == CLAY_OK);

    std::int32_t undone = 1;
    CHECK(clay_document_undo(f.doc, &undone) == CLAY_OK);  // not an error
    CHECK(undone == 0);

    std::int32_t redone = 1;
    CHECK(clay_document_redo(f.doc, &redone) == CLAY_OK);
    CHECK(redone == 0);
}

TEST_CASE("grouped edits undo as one step") {
    EditFixture f;
    REQUIRE(clay_document_enable_undo(f.doc) == CLAY_OK);
    std::vector<std::uint8_t> before = doc_bytes(f.doc);

    REQUIRE(clay_document_begin_undo_group(f.doc) == CLAY_OK);
    const float moved[3] = {2, 0, 0};
    const float red[3] = {1, 0, 0};
    REQUIRE(clay_layer_set_transform(f.doc, f.layer, f.node, moved, kUpAxis, 0.0f, 1.0f) ==
            CLAY_OK);
    REQUIRE(clay_layer_set_color(f.doc, f.layer, f.node, red) == CLAY_OK);
    REQUIRE(clay_document_end_undo_group(f.doc) == CLAY_OK);

    std::size_t undo_depth = 0;
    REQUIRE(clay_document_undo_state(f.doc, nullptr, &undo_depth, nullptr) == CLAY_OK);
    CHECK(undo_depth == 1);

    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(f.doc, &undone) == CLAY_OK);
    CHECK(doc_bytes(f.doc) == before);
}

TEST_CASE("C undo matches the same sequence through the scene API") {
    EditFixture f;
    REQUIRE(clay_document_enable_undo(f.doc) == CLAY_OK);

    const float moved[3] = {1.5f, 0.25f, -0.5f};
    REQUIRE(clay_layer_set_transform(f.doc, f.layer, f.node, moved, kUpAxis, 0.4f, 1.0f) ==
            CLAY_OK);
    REQUIRE(clay_layer_set_op_blend(f.doc, f.layer, f.node, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                    0.15f, 0.0f) == CLAY_OK);
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(f.doc, &undone) == CLAY_OK);  // drop the op/blend edit

    // the twin: the transform applied, the op/blend edit never made
    scene::Document twin;
    scene::Layer& tl = twin.add_sdf_layer("body");
    scene::Node n = clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0));
    n.xform.position = cf3(1.5f, 0.25f, -0.5f);
    n.xform.rotation = math::Quat::from_axis_angle(cf3(0, 1, 0), 0.4f);
    tl.sdf->insert(n);

    check_same_field(f.doc, twin);
}
