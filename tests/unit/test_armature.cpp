// Armatures: a tree of spheres skinned by one sphere-swept cone per
// node-parent pair (sdf-kernels + scene-model specs, add-armature).

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "clay/io/clayspace.h"
#include "clay/scene/armature.h"
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

// ---------------------------------------------------------------------------
// Tree edits (scene-model spec). The semantics live in pure functions over
// (nodes, parents); SetArmatureCmd is what installs the result.

namespace {

// A spine with two arms: node 0 root, 1 chest, 2 and 3 hanging off the chest.
void limbed(std::vector<scene::StrokePoint>& nodes, std::vector<std::uint32_t>& parents) {
    nodes = {node(cf3(0, 0, 0), 0.20f), node(cf3(0, 0.5f, 0), 0.18f),
             node(cf3(-0.4f, 0.4f, 0), 0.12f), node(cf3(0.4f, 0.4f, 0), 0.12f)};
    parents = {0, 0, 1, 1};
}

}  // namespace

TEST_CASE("armature: moving a node carries its subtree") {
    std::vector<scene::StrokePoint> nodes;
    std::vector<std::uint32_t> parents;
    limbed(nodes, parents);
    const kernel::cfloat3 arm_before = nodes[2].pos;

    // Move the CHEST. Both arms hang off it, so both must come along, and the
    // root must not — that asymmetry is the whole property.
    REQUIRE(scene::armature_move(nodes, parents, 1, cf3(0, 0.25f, 0)));
    CHECK(nodes[1].pos.y == doctest::Approx(0.75f));
    CHECK(nodes[2].pos.y == doctest::Approx(arm_before.y + 0.25f));
    CHECK(nodes[3].pos.y == doctest::Approx(arm_before.y + 0.25f));
    CHECK(nodes[0].pos.y == doctest::Approx(0.0f));  // the root stayed put
    // and their offsets relative to the node that moved are unchanged
    CHECK((nodes[2].pos.x - nodes[1].pos.x) == doctest::Approx(arm_before.x));
}

TEST_CASE("armature: deleting a node takes its subtree and renumbers the rest") {
    std::vector<scene::StrokePoint> nodes;
    std::vector<std::uint32_t> parents;
    limbed(nodes, parents);
    std::vector<std::int8_t> signs;
    // Delete the chest: both arms go with it, leaving only the root.
    REQUIRE(scene::armature_delete_subtree(nodes, parents, signs, 1));
    CHECK(nodes.size() == 1);
    CHECK(parents.size() == 1);
    CHECK(parents[0] == 0);
    CHECK(scene::armature_is_valid(nodes, parents));

    // Deleting a LEAF leaves its siblings, with the survivors renumbered so no
    // parent points past the end.
    limbed(nodes, parents);
    signs.clear();
    REQUIRE(scene::armature_delete_subtree(nodes, parents, signs, 2));
    CHECK(nodes.size() == 3);
    CHECK(scene::armature_is_valid(nodes, parents));
    CHECK(nodes[2].pos.x == doctest::Approx(0.4f));  // the other arm survived
    CHECK(parents[2] == 1);                          // still hanging off the chest
}

TEST_CASE("armature: a cycle is not a tree") {
    std::vector<scene::StrokePoint> nodes;
    std::vector<std::uint32_t> parents;
    limbed(nodes, parents);
    CHECK(scene::armature_is_valid(nodes, parents));
    parents = {1, 0, 1, 1};  // 0 -> 1 -> 0
    CHECK_FALSE(scene::armature_is_valid(nodes, parents));
}

TEST_CASE("armature: a mirrored insert adds both sides") {
    std::vector<scene::StrokePoint> nodes;
    std::vector<std::uint32_t> parents;
    nodes = {node(cf3(0, 0, 0), 0.2f)};
    parents = {0};

    // A shoulder off the root: two nodes, reflected in x.
    CHECK(scene::armature_add_child_mirrored(nodes, parents, 0, cf3(0.3f, 0.4f, 0), 0.12f) == 2);
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[1].pos.x == doctest::Approx(0.3f));
    CHECK(nodes[2].pos.x == doctest::Approx(-0.3f));

    // An elbow off the shoulder: its reflection hangs off the MIRRORED
    // shoulder, not off the one that was named.
    CHECK(scene::armature_add_child_mirrored(nodes, parents, 1, cf3(0.5f, 0.1f, 0), 0.09f) == 2);
    REQUIRE(nodes.size() == 5);
    CHECK(parents[3] == 1);
    CHECK(parents[4] == 2);
    CHECK(nodes[4].pos.x == doctest::Approx(-0.5f));

    // A node ON the plane is its own reflection, so it is added once.
    CHECK(scene::armature_add_child_mirrored(nodes, parents, 0, cf3(0, 0.9f, 0), 0.15f) == 1);
    CHECK(nodes.size() == 6);
    CHECK(scene::armature_is_valid(nodes, parents));
}

TEST_CASE("armature: a tree edit is one undoable command") {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    std::vector<std::uint32_t> parents;
    limbed(n.stroke, parents);
    n.armature_parents = parents;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::NodeId id = l.sdf->insert(n);

    std::vector<scene::StrokePoint> before = n.stroke;
    std::vector<scene::StrokePoint> edited = n.stroke;
    REQUIRE(scene::armature_move(edited, parents, 1, cf3(0, 0.25f, 0)));

    scene::SetArmatureCmd cmd{l.id, id, edited, parents, {}, 0.0f};
    std::optional<scene::Command> inverse = scene::apply(doc, scene::Command{cmd});
    REQUIRE(inverse.has_value());
    CHECK(l.sdf->find(id)->stroke[2].pos.y == doctest::Approx(0.65f));

    REQUIRE(scene::apply(doc, *inverse).has_value());
    const scene::Node* back = l.sdf->find(id);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CAPTURE(i);
        CHECK(back->stroke[i].pos.y == doctest::Approx(before[i].pos.y));
    }
    CHECK(back->armature_parents == parents);
}

TEST_CASE("armature: a cyclic tree is refused by the command, not stored") {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    std::vector<std::uint32_t> parents;
    limbed(n.stroke, parents);
    n.armature_parents = parents;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::NodeId id = l.sdf->insert(n);

    scene::SetArmatureCmd bad{l.id, id, n.stroke, {1, 0, 1, 1}, {}, 0.0f};
    CHECK_FALSE(scene::apply(doc, scene::Command{bad}).has_value());
    CHECK(l.sdf->find(id)->armature_parents == parents);  // unchanged
}

// ---------------------------------------------------------------------------
// Signs (add-armature-node-signs, #99). A negative node's link carves instead
// of skinning, after every positive link has folded.

namespace {

scene::Document signed_doc(std::vector<scene::StrokePoint> pts,
                           std::vector<std::uint32_t> parents,
                           std::vector<std::int8_t> signs, float blend_k) {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = std::move(pts);
    n.armature_parents = std::move(parents);
    n.armature_signs = std::move(signs);
    n.stroke_blend_k = blend_k;
    doc.add_sdf_layer("l").sdf->insert(n);
    return doc;
}

}  // namespace

TEST_CASE("armature: an all-positive rig is bit-identical to one without signs") {
    // The compatibility property signs must not disturb: an explicit all-+1
    // array is the SAME field as no array at all, exactly, at every blend —
    // which is what keeps a chain armature equal to its stroke.
    for (float k : {0.0f, 0.08f}) {
        scene::Tape bare = scene::compile_document(armature_doc(k));
        scene::Tape signed_t = scene::compile_document(
            signed_doc(chain_points(), {0, 0, 1, 2}, {1, 1, 1, 1}, k));
        CAPTURE(k);
        CHECK(worst_gap(bare, signed_t) == 0.0f);
    }
}

TEST_CASE("armature: a negative node's links carry no skin") {
    // The issue's first defect, stated as geometry. The workaround puts the
    // negative in as a separate subtract ball, so the TREE has to link A
    // straight to C through the hollow — and the sleeve of that link survives
    // the ball, bridging the opening. With the sign in the tree the artist
    // parents the chain naturally, A <- B(-) <- C, and B's links belong to
    // neither half: no sleeve is ever drawn through the hollow, and B's own
    // ball carves whatever else overlaps it.
    scene::Document doc = signed_doc(
        {node(cf3(-0.5f, 0, 0), 0.2f), node(cf3(0, 0, 0), 0.2f), node(cf3(0.5f, 0, 0), 0.2f)},
        {0, 0, 1}, {1, -1, 1}, 0.0f);
    scene::Tape t = scene::compile_document(doc);
    CHECK(t.eval(cf3(-0.5f, 0, 0)).d < 0.0f);  // A is material
    CHECK(t.eval(cf3(0.5f, 0, 0)).d < 0.0f);   // C is material
    CHECK(t.eval(cf3(0, 0, 0)).d > 0.0f);      // the hollow is hollow
    CHECK(t.eval(cf3(-0.25f, 0, 0)).d > 0.0f); // no sleeve toward A
    CHECK(t.eval(cf3(0.25f, 0, 0)).d > 0.0f);  // and none toward C

    // The same tree all-positive is the full sleeve — what the sign removed.
    scene::Document plain = signed_doc(
        {node(cf3(-0.5f, 0, 0), 0.2f), node(cf3(0, 0, 0), 0.2f), node(cf3(0.5f, 0, 0), 0.2f)},
        {0, 0, 1}, {1, 1, 1}, 0.0f);
    scene::Tape pt = scene::compile_document(plain);
    CHECK(pt.eval(cf3(-0.25f, 0, 0)).d < 0.0f);
    CHECK(pt.eval(cf3(0.25f, 0, 0)).d < 0.0f);
}

TEST_CASE("armature: a negative child carves without eating its parent") {
    // An eye socket: a negative child of a positive head. The carve is the
    // NEGATIVE half's armature — here one root sphere — not a sweep at the
    // parent's radius, which would swallow the head whole. The head's own
    // skin survives everywhere the socket ball does not reach.
    scene::Document doc = signed_doc(
        {node(cf3(0, 0, 0), 0.3f), node(cf3(0.0f, 0.0f, 0.28f), 0.1f)},
        {0, 0}, {1, -1}, 0.0f);
    scene::Tape t = scene::compile_document(doc);
    CHECK(t.eval(cf3(0, 0, 0.28f)).d > 0.0f);   // the socket is hollow
    CHECK(t.eval(cf3(0, 0, -0.1f)).d < 0.0f);   // the head is still a head
    CHECK(t.eval(cf3(0.25f, 0, 0)).d < 0.0f);
}

TEST_CASE("armature: a negative subtree is a carving rig") {
    // The issue's third defect: no leaf restriction. Two negative nodes in a
    // parent-child pair carve their LINK — the same sphere-swept cone the
    // positive half skins with — so a deep hollow is one swept scoop. Their
    // positive host survives outside it, and a positive child of a negative
    // node keeps its own sphere.
    scene::Document doc = signed_doc(
        {node(cf3(0, 0, 0), 0.4f), node(cf3(0.0f, 0.1f, 0.25f), 0.12f),
         node(cf3(0.0f, -0.1f, 0.25f), 0.12f), node(cf3(0.6f, 0, 0), 0.15f)},
        {0, 0, 1, 2}, {1, -1, -1, 1}, 0.0f);
    scene::Tape t = scene::compile_document(doc);
    CHECK(t.eval(cf3(0, 0.1f, 0.25f)).d > 0.0f);   // the carve, at one end
    CHECK(t.eval(cf3(0, -0.1f, 0.25f)).d > 0.0f);  // ... at the other
    CHECK(t.eval(cf3(0, 0, 0.3f)).d > 0.0f);       // ... and along the LINK
    CHECK(t.eval(cf3(0, 0, -0.2f)).d < 0.0f);      // the host survives
    CHECK(t.eval(cf3(0.6f, 0, 0)).d < 0.0f);       // the positive grandchild keeps its sphere
}

TEST_CASE("armature: an all-negative rig is empty, not degenerate") {
    scene::Document doc = signed_doc(
        {node(cf3(0, 0, 0), 0.3f), node(cf3(0.5f, 0, 0), 0.2f)}, {0, 0}, {-1, -1}, 0.0f);
    scene::Tape t = scene::compile_document(doc);
    CHECK(t.eval(cf3(0, 0, 0)).d > 0.0f);
    CHECK(t.eval(cf3(0.5f, 0, 0)).d > 0.0f);
}

TEST_CASE("armature: signs survive the round trip and the minor-7 escape hatch") {
    io::ClaySpaceDoc file;
    scene::Node n;
    n.prim = scene::Prim::armature();
    n.stroke = {node(cf3(0, 0, 0), 0.2f), node(cf3(0, 0.4f, 0), 0.15f),
                node(cf3(0.4f, 0.2f, 0), 0.12f)};
    n.armature_parents = {0, 0, 0};
    n.armature_signs = {1, -1, 1};
    n.stroke_blend_k = 0.05f;
    scene::Layer& fl = file.document.add_sdf_layer("l");
    scene::NodeId id = fl.sdf->insert(n);

    // The issue's second defect: the sign has to survive a save. Byte-identity
    // on the reserialise pins the whole record, signs included.
    std::vector<std::uint8_t> bytes = io::save_clayspace(file);
    io::ClaySpaceDoc back;
    REQUIRE(io::load_clayspace(bytes.data(), bytes.size(), &back).ok());
    CHECK(io::save_clayspace(back) == bytes);
    const scene::Layer* bl = back.document.find_layer(fl.id);
    REQUIRE(bl);
    const scene::Node* rn = bl->sdf->find(id);
    REQUIRE(rn);
    CHECK(rn->armature_signs == std::vector<std::int8_t>{1, -1, 1});
    CHECK(worst_gap(scene::compile_document(file.document),
                    scene::compile_document(back.document)) == 0.0f);

    // Writing AT minor 7 drops exactly the signs: the bytes equal what the
    // same document without signs always produced, and reload yields the
    // all-positive rig.
    scene::Document plain;
    scene::Node p = n;
    p.armature_signs.clear();
    plain.add_sdf_layer("l").sdf->insert(p);
    CHECK(scene::serialize_document(file.document, 7) == scene::serialize_document(plain, 7));
    std::vector<std::uint8_t> old_bytes = scene::serialize_document(file.document, 7);
    std::optional<scene::Document> old_doc =
        scene::deserialize_document(old_bytes.data(), old_bytes.size(), 7);
    REQUIRE(old_doc.has_value());
    const scene::Layer* ol = old_doc->find_layer(fl.id);
    REQUIRE(ol);
    const scene::Node* on = ol->sdf->find(id);
    REQUIRE(on);
    CHECK(on->armature_signs.empty());
}

TEST_CASE("armature: a sign edit is one undoable command") {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    std::vector<std::uint32_t> parents;
    limbed(n.stroke, parents);
    n.armature_parents = parents;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::NodeId id = l.sdf->insert(n);

    std::vector<std::int8_t> signs;
    REQUIRE(scene::armature_set_sign(signs, n.stroke.size(), 2, -1));
    scene::SetArmatureCmd cmd{l.id, id, n.stroke, parents, signs, 0.0f};
    std::optional<scene::Command> inverse = scene::apply(doc, scene::Command{cmd});
    REQUIRE(inverse.has_value());
    CHECK(l.sdf->find(id)->armature_signs == std::vector<std::int8_t>{1, 1, -1, 1});

    REQUIRE(scene::apply(doc, *inverse).has_value());
    CHECK(l.sdf->find(id)->armature_signs.empty());  // exactly what was there
}

TEST_CASE("armature: a bad sign is refused by the command, not stored") {
    scene::Document doc;
    scene::Node n;
    n.prim = scene::Prim::armature();
    std::vector<std::uint32_t> parents;
    limbed(n.stroke, parents);
    n.armature_parents = parents;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::NodeId id = l.sdf->insert(n);

    // A magnitude is not a sign, and a fifth sign names a node that is not
    // there. Both are refused with the document unchanged.
    scene::SetArmatureCmd magnitude{l.id, id, n.stroke, parents, {1, 2, 1, 1}, 0.0f};
    CHECK_FALSE(scene::apply(doc, scene::Command{magnitude}).has_value());
    scene::SetArmatureCmd surplus{l.id, id, n.stroke, parents, {1, 1, 1, 1, -1}, 0.0f};
    CHECK_FALSE(scene::apply(doc, scene::Command{surplus}).has_value());
    CHECK(l.sdf->find(id)->armature_signs.empty());
}

TEST_CASE("armature: deleting a subtree keeps the survivors' signs") {
    std::vector<scene::StrokePoint> nodes;
    std::vector<std::uint32_t> parents;
    limbed(nodes, parents);
    std::vector<std::int8_t> signs{1, 1, -1, 1};
    // Delete the OTHER arm (node 3): the negative arm at index 2 survives with
    // its sign, under the same renumbering the parents get.
    REQUIRE(scene::armature_delete_subtree(nodes, parents, signs, 3));
    CHECK(signs == std::vector<std::int8_t>{1, 1, -1});

    // Delete the negative arm: the survivors are all positive.
    limbed(nodes, parents);
    signs = {1, 1, -1, 1};
    REQUIRE(scene::armature_delete_subtree(nodes, parents, signs, 2));
    CHECK(signs == std::vector<std::int8_t>{1, 1, 1});
}
