// Armatures: a tree of spheres skinned by one sphere-swept cone per
// node-parent pair (sdf-kernels + scene-model specs, add-armature).

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/io/clayspace.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"

using namespace clay;
using kernel::cf3;

namespace {

scene::StrokePoint node(kernel::cfloat3 p, float r) {
    scene::StrokePoint n;
    n.pos = p;
    n.radius = r;
    return n;
}

// The four points both a stroke and a chain armature are built from.
std::vector<scene::StrokePoint> chain_points() {
    return {node(cf3(-0.6f, 0.0f, 0.0f), 0.20f), node(cf3(-0.2f, 0.25f, 0.0f), 0.16f),
            node(cf3(0.25f, 0.20f, 0.0f), 0.13f), node(cf3(0.6f, -0.1f, 0.0f), 0.10f)};
}

scene::Document stroke_doc(float blend_k) {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::stroke();
    n.stroke = chain_points();
    n.stroke_blend_k = blend_k;
    doc.add_sdf_layer("l").sdf->insert(n);
    return doc;
}

scene::Document armature_doc(float blend_k) {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = chain_points();
    n.stroke_blend_k = blend_k;
    n.armature_parents = {0, 0, 1, 2};  // a line: node i hangs off i-1
    doc.add_sdf_layer("l").sdf->insert(n);
    return doc;
}

float worst_gap(const scene::Tape& a, const scene::Tape& b) {
    clay_test::Lcg rng(17);
    float worst = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        kernel::cfloat3 p = rng.vec3(-1.2f, 1.2f);
        worst = kernel::cmax(worst, std::fabs(a.eval(p).d - b.eval(p).d));
    }
    return worst;
}

}  // namespace

TEST_CASE("armature: a chain armature IS the stroke it came from") {
    // The compatibility property the whole design rests on. An armature is
    // ctape_stroke with the chain generalised, so a line-shaped tree has to
    // agree with the stroke exactly — otherwise the two have drifted and one
    // of them is wrong.
    //
    // It has to hold at every blend, and the blended case is the one that
    // catches real mistakes: with blend_k = 0 a redundant term is invisible
    // because min is idempotent, and with blend_k > 0 it moves the surface.
    for (float k : {0.0f, 0.04f, 0.08f, 0.15f}) {
        scene::Tape s = scene::compile_document(stroke_doc(k));
        scene::Tape a = scene::compile_document(armature_doc(k));
        CAPTURE(k);
        CHECK(worst_gap(s, a) < 1e-5f);
    }
}

TEST_CASE("armature: a root's sphere is not counted twice") {
    // A root is already inside every link that names it. Contributing it again
    // is harmless under a hard union and wrong under a soft one, which is what
    // the blended case above would catch — this pins the reason directly.
    scene::Document doc = armature_doc(0.12f);
    scene::Tape t = scene::compile_document(doc);
    // Just outside the root sphere, along the link to its child: a doubled
    // root would push this point inside.
    float d = t.eval(cf3(-0.6f, 0.0f, 0.235f)).d;
    CHECK(d > 0.0f);
}

TEST_CASE("armature: a single node is a sphere") {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = {node(cf3(0, 0, 0), 0.3f)};
    n.armature_parents = {0};
    doc.add_sdf_layer("l").sdf->insert(n);
    scene::Tape t = scene::compile_document(doc);
    CHECK(t.eval(cf3(0, 0, 0)).d == doctest::Approx(-0.3f).epsilon(0.01));
    CHECK(t.eval(cf3(0.5f, 0, 0)).d > 0.0f);
}

TEST_CASE("armature: a branch is a tree, not a chain") {
    // What a stroke cannot express: three children on one root. Each limb is
    // material, and two limbs are NOT joined to each other except through the
    // parent they share.
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = {node(cf3(0, 0, 0), 0.22f), node(cf3(0, 0.45f, 0), 0.16f),
                node(cf3(-0.45f, 0.30f, 0), 0.12f), node(cf3(0.45f, 0.30f, 0), 0.12f)};
    n.armature_parents = {0, 0, 0, 0};
    n.stroke_blend_k = 0.06f;
    doc.add_sdf_layer("l").sdf->insert(n);
    scene::Tape t = scene::compile_document(doc);

    CHECK(t.eval(cf3(0, 0, 0)).d < 0.0f);              // root
    CHECK(t.eval(cf3(0, 0.45f, 0)).d < 0.0f);          // spine tip
    CHECK(t.eval(cf3(-0.45f, 0.30f, 0)).d < 0.0f);     // left limb
    CHECK(t.eval(cf3(0.45f, 0.30f, 0)).d < 0.0f);      // right limb
    // Between the two limbs, above the root: empty, because the limbs join
    // through the parent rather than to each other.
    CHECK(t.eval(cf3(0, 0.62f, 0)).d > 0.0f);
}

TEST_CASE("armature: a malformed tree degrades rather than misbehaves") {
    // A parent index out of range reads as a root, in the kernel and in the
    // tape builder alike, so a bad tree is loose spheres rather than undefined
    // behaviour. A host that refuses the tree earlier is welcome to; the field
    // still has to mean something if one gets through.
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = {node(cf3(-0.3f, 0, 0), 0.2f), node(cf3(0.3f, 0, 0), 0.2f)};
    n.armature_parents = {99, 99};
    doc.add_sdf_layer("l").sdf->insert(n);
    scene::Tape t = scene::compile_document(doc);
    CHECK(t.eval(cf3(-0.3f, 0, 0)).d < 0.0f);
    CHECK(t.eval(cf3(0.3f, 0, 0)).d < 0.0f);
    CHECK(t.eval(cf3(0, 0, 0)).d > 0.0f);  // nothing joins them
}

TEST_CASE("armature: the tree survives a round trip") {
    io::ClaySpaceDoc file;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = {node(cf3(0, 0, 0), 0.2f), node(cf3(0, 0.4f, 0), 0.15f),
                node(cf3(0.4f, 0.2f, 0), 0.12f)};
    n.armature_parents = {0, 0, 0};
    n.stroke_blend_k = 0.05f;
    file.document.add_sdf_layer("l").sdf->insert(n);

    std::vector<std::uint8_t> bytes = io::save_clayspace(file);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    CHECK(io::save_clayspace(back) == bytes);

    scene::Tape a = scene::compile_document(file.document);
    scene::Tape b = scene::compile_document(back.document);
    CHECK(worst_gap(a, b) == 0.0f);
}

TEST_CASE("armature: its bound covers every node") {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = {node(cf3(0, 0, 0), 0.2f), node(cf3(0.8f, 0, 0), 0.1f)};
    n.armature_parents = {0, 0};
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(n);
    math::Aabb b = scene::layer_influence_bound(l);
    CHECK(b.min.x <= -0.2f);
    CHECK(b.max.x >= 0.9f);
}
