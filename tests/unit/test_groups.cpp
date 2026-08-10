#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "clay.h"
#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

// Scene groups as a HOST sees them (c-abi spec: groups across the ABI). The
// engine has compiled groups since the first version; nothing outside the
// engine could build one, so nothing outside the engine pinned what one means.
// These build them through clay.h and check the field against the scene model,
// which is the only way the two can be shown not to have drifted.
//
// The first two cases are engine invariants the exposure depends on and so
// belong with them: a move that would close a cycle, and the dilation a
// group's own blend contributes to its influence bound.

using namespace clay;
using kernel::cf3;

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;

    CDoc() { REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK); }
    ~CDoc() { clay_document_destroy(doc); }
};

float eval_c(const clay_document* doc, kernel::cfloat3 p) {
    float point[3] = {p.x, p.y, p.z};
    float out = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", point, 1, &out, nullptr) == CLAY_OK);
    return out;
}

// A sphere of radius r at the origin, as the flat descriptor spells it.
clay_item_desc sphere_desc(float r, kernel::cfloat3 at, std::int32_t op = CLAY_OP_ADD) {
    clay_item_desc d{};
    d.struct_size = static_cast<std::uint32_t>(sizeof d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = r;
    d.position[0] = at.x;
    d.position[1] = at.y;
    d.position[2] = at.z;
    d.rotation[3] = 1.0f;
    d.scale = 1.0f;
    d.op = op;
    d.blend = CLAY_BLEND_HARD;
    d.color[0] = d.color[1] = d.color[2] = 0.7f;
    return d;
}

clay_item_desc box_desc(kernel::cfloat3 half, kernel::cfloat3 at, std::int32_t op) {
    clay_item_desc d = sphere_desc(0.0f, at, op);
    d.prim = CLAY_PRIM_BOX;
    d.params[0] = half.x;
    d.params[1] = half.y;
    d.params[2] = half.z;
    return d;
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

std::vector<clay_node_id> children_of(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id group) {
    std::size_t count = 0;
    REQUIRE(clay_layer_children(doc, layer, group, nullptr, &count) == CLAY_OK);
    std::vector<clay_node_id> ids(count, 0);
    if (count == 0) return ids;
    REQUIRE(clay_layer_children(doc, layer, group, ids.data(), &count) == CLAY_OK);
    CHECK(count == ids.size());
    return ids;
}

}  // namespace

TEST_CASE("a node cannot move into its own subtree") {
    // Regression: move() detached the node and then only checked that the new
    // parent existed and was a group, so moving a group under its own child
    // closed a cycle. The subtree left the root list, stopped evaluating, was
    // dropped by the next save (write_content walks from roots) and could no
    // longer be reached by remove() — and the call reported success.
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node g;
    g.is_group = true;
    scene::NodeId outer = l.sdf->insert(g);
    scene::NodeId inner = l.sdf->insert(g, outer);
    scene::NodeId deeper = l.sdf->insert(g, inner);
    scene::NodeId item = l.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)),
                                       deeper);

    CHECK_FALSE(l.sdf->move(outer, outer, -1));    // into itself
    CHECK_FALSE(l.sdf->move(outer, inner, -1));    // into its child
    CHECK_FALSE(l.sdf->move(outer, deeper, -1));   // into its grandchild
    // and nothing was disturbed on the way to saying no
    REQUIRE(l.sdf->roots.size() == 1);
    CHECK(l.sdf->roots[0] == outer);
    CHECK(l.sdf->find(outer)->children == std::vector<scene::NodeId>{inner});
    CHECK(l.sdf->find(deeper)->children == std::vector<scene::NodeId>{item});

    // the moves that are not cycles still work, in both directions
    CHECK(l.sdf->move(deeper, scene::kNoNode, -1));  // out to the layer root
    CHECK(l.sdf->move(outer, deeper, -1));           // outer is no longer above deeper
    CHECK(l.sdf->find(deeper)->children.size() == 2);
}

TEST_CASE("a group's influence bound covers what its blend reaches") {
    // Regression: the group path dilated by blend.support() alone, and a HARD
    // profile supports 0 — so a Paint group with k > 0 claimed a bound its own
    // colour reached past. The item path has always used max(support, k),
    // which is what the paint combine actually fades over (kernel/tape.h).
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node g;
    g.is_group = true;
    g.op = scene::Op::Paint;
    g.blend.profile = scene::BlendProfile::Hard;
    g.blend.k = 0.3f;
    scene::NodeId group = l.sdf->insert(g);
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)), group);

    math::Aabb b = scene::node_influence_bound(*l.sdf, group, l);
    REQUIRE_FALSE(b.empty());
    CHECK(b.max.x >= 0.5f + 0.3f);
    CHECK(b.min.x <= -(0.5f + 0.3f));
}

TEST_CASE("c groups: a sub-expression combines as a unit") {
    // (A intersect B) union C — the operation that is not expressible without
    // a group, because an op applies to the whole accumulated field and the
    // intersect would trim C as well.
    CDoc built;
    clay_node_id plate = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD,
                                 0.0f, 0.0f, &plate) == CLAY_OK);
    clay_item_desc shell = sphere_desc(0.9f, cf3(0, 0, 0));
    clay_item_desc cutter = box_desc(cf3(0.5f, 2.0f, 2.0f), cf3(0.5f, 0, 0), CLAY_OP_INTERSECT);
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, plate, -1, &shell, nullptr) == CLAY_OK);
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, plate, -1, &cutter, nullptr) ==
            CLAY_OK);
    // C: added AFTER the group, at the layer root, and untouched by the cut
    clay_item_desc other = sphere_desc(0.4f, cf3(-1.4f, 0, 0));
    REQUIRE(clay_add_item(built.doc, built.layer, &other, nullptr) == CLAY_OK);

    CHECK(eval_c(built.doc, cf3(0.5f, 0, 0)) < 0.0f);   // inside the trimmed plate
    CHECK(eval_c(built.doc, cf3(-0.5f, 0, 0)) > 0.0f);  // the intersect took this half
    CHECK(eval_c(built.doc, cf3(-1.4f, 0, 0)) < 0.0f);  // C survived it

    // The same tree on the scene model: the boundary must not have moved it.
    scene::Document ref;
    scene::Layer& l = ref.add_sdf_layer("body");
    scene::Node g;
    g.is_group = true;
    scene::NodeId group = l.sdf->insert(g);
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.9f), cf3(0, 0, 0)), group);
    l.sdf->insert(clay_test::item(scene::Prim::box(cf3(0.5f, 2.0f, 2.0f)), cf3(0.5f, 0, 0),
                                  scene::Op::Intersect),
                  group);
    l.sdf->insert(clay_test::item(scene::Prim::sphere(0.4f), cf3(-1.4f, 0, 0)));
    scene::Tape tape = scene::compile_document(ref);
    clay_test::Lcg rng(4242);
    for (int i = 0; i < 400; ++i) {
        kernel::cfloat3 p = rng.vec3(-2.5f, 2.5f);
        CHECK(eval_c(built.doc, p) == doctest::Approx(tape.eval(p).d));
    }
}

TEST_CASE("c groups: nesting, enumeration and the inline op") {
    CDoc built;
    clay_item_desc base = sphere_desc(0.8f, cf3(0, 0, 0));
    REQUIRE(clay_add_item(built.doc, built.layer, &base, nullptr) == CLAY_OK);

    clay_node_id outer = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_SUBTRACT,
                                 CLAY_BLEND_HARD, 0.0f, 0.0f, &outer) == CLAY_OK);
    clay_node_id inner = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, outer, -1, CLAY_OP_ADD,
                                 CLAY_BLEND_HARD, 0.0f, 0.0f, &inner) == CLAY_OK);
    clay_item_desc a = sphere_desc(0.35f, cf3(0.6f, 0, 0));
    clay_item_desc b = sphere_desc(0.35f, cf3(-0.6f, 0, 0));
    clay_node_id a_id = 0;
    clay_node_id b_id = 0;
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, inner, -1, &a, &a_id) == CLAY_OK);
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, inner, -1, &b, &b_id) == CLAY_OK);

    SUBCASE("children come back in order, by the size-query pattern") {
        CHECK(children_of(built.doc, built.layer, outer) == std::vector<clay_node_id>{inner});
        CHECK(children_of(built.doc, built.layer, inner) ==
              std::vector<clay_node_id>{a_id, b_id});

        // index places a child rather than appending it
        clay_item_desc c = sphere_desc(0.2f, cf3(0, 0.6f, 0));
        clay_node_id c_id = 0;
        REQUIRE(clay_add_item_in_group(built.doc, built.layer, inner, 1, &c, &c_id) == CLAY_OK);
        CHECK(children_of(built.doc, built.layer, inner) ==
              std::vector<clay_node_id>{a_id, c_id, b_id});
    }

    SUBCASE("a buffer that is too small says how big it must be, and writes nothing") {
        clay_node_id one[2] = {99, 99};
        std::size_t count = 1;
        CHECK(clay_layer_children(built.doc, built.layer, inner, one, &count) ==
              CLAY_ERROR_BUFFER_TOO_SMALL);
        CHECK(count == 2);
        CHECK(one[0] == 99);
    }

    SUBCASE("an item is not a group, and that is how a reloading host tells") {
        std::size_t count = 0;
        CHECK(clay_layer_children(built.doc, built.layer, a_id, nullptr, &count) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_children(built.doc, built.layer, 9999, nullptr, &count) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(clay_add_item_in_group(built.doc, built.layer, a_id, -1, &base, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("the nested subtract carves the two spheres out as one") {
        CHECK(eval_c(built.doc, cf3(0.6f, 0, 0)) > 0.0f);
        CHECK(eval_c(built.doc, cf3(-0.6f, 0, 0)) > 0.0f);
        CHECK(eval_c(built.doc, cf3(0, 0, 0)) < 0.0f);  // between them, untouched
    }
}

TEST_CASE("c groups: an inline group's children apply to the outer chain") {
    // Two documents that must agree: one where a subtract sits inside an
    // inline group, one where the same subtract sits at the layer root. The
    // inline op exists to make those the same thing.
    auto build = [](bool grouped) {
        clay_document* doc = clay_document_create();
        clay_layer_id layer = 0;
        REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
        clay_item_desc base = sphere_desc(0.9f, cf3(0, 0, 0));
        REQUIRE(clay_add_item(doc, layer, &base, nullptr) == CLAY_OK);
        clay_item_desc cut = box_desc(cf3(0.4f, 0.4f, 2.0f), cf3(0.5f, 0.2f, 0),
                                      CLAY_OP_SUBTRACT);
        clay_item_desc bump = sphere_desc(0.3f, cf3(0, 0.9f, 0));
        clay_node_id parent = 0;
        if (grouped)
            REQUIRE(clay_layer_add_group(doc, layer, 0, -1, CLAY_OP_INLINE, CLAY_BLEND_HARD,
                                         0.0f, 0.0f, &parent) == CLAY_OK);
        auto add = [&](const clay_item_desc& d) {
            return grouped ? clay_add_item_in_group(doc, layer, parent, -1, &d, nullptr)
                           : clay_add_item(doc, layer, &d, nullptr);
        };
        REQUIRE(add(cut) == CLAY_OK);
        REQUIRE(add(bump) == CLAY_OK);
        return doc;
    };
    clay_document* grouped = build(true);
    clay_document* flat = build(false);
    clay_test::Lcg rng(77);
    for (int i = 0; i < 400; ++i) {
        kernel::cfloat3 p = rng.vec3(-2.0f, 2.0f);
        CHECK(eval_c(grouped, p) == eval_c(flat, p));  // exact, not approximate
    }
    clay_document_destroy(grouped);
    clay_document_destroy(flat);
}

TEST_CASE("c groups: a carving group with nothing beneath it emits nothing") {
    // Two mechanisms, both in compile_group, and a host can hit either one.
    SUBCASE("nothing beneath the group in the chain") {
        CDoc built;
        clay_node_id carve = 0;
        REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_SUBTRACT,
                                     CLAY_BLEND_HARD, 0.0f, 0.0f, &carve) == CLAY_OK);
        clay_item_desc s = sphere_desc(0.5f, cf3(0, 0, 0));
        REQUIRE(clay_add_item_in_group(built.doc, built.layer, carve, -1, &s, nullptr) ==
                CLAY_OK);
        // A subtract against empty space is nothing at all, exactly as a
        // carving ITEM first in a chain is — not a hole in the far field.
        CHECK(eval_c(built.doc, cf3(0, 0, 0)) == CLAY_TAPE_FAR);
    }

    SUBCASE("nothing INSIDE the group") {
        CDoc built;
        clay_item_desc base = sphere_desc(0.8f, cf3(0, 0, 0));
        REQUIRE(clay_add_item(built.doc, built.layer, &base, nullptr) == CLAY_OK);
        const float inside = eval_c(built.doc, cf3(0, 0, 0));

        clay_node_id empty = 0;
        REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_SUBTRACT,
                                     CLAY_BLEND_HARD, 0.0f, 0.0f, &empty) == CLAY_OK);
        // An empty subtree is rolled back rather than left as a combine
        // against empty space, so the field is bit-identical to before.
        CHECK(eval_c(built.doc, cf3(0, 0, 0)) == inside);
        // and so is an add group with nothing in it
        clay_node_id also = 0;
        REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD,
                                     0.0f, 0.0f, &also) == CLAY_OK);
        CHECK(eval_c(built.doc, cf3(0, 0, 0)) == inside);
    }
}

TEST_CASE("c groups: what a group may and may not carry") {
    CDoc built;
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD,
                                 0.0f, 0.0f, &group) == CLAY_OK);

    // compile_group emits no transition parameters, so a group carrying a
    // morph op would run on the compiler's defaults instead of the node's.
    CHECK(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_TRANSITION_LINEAR,
                               CLAY_BLEND_HARD, 0.0f, 0.0f, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_op_blend(built.doc, built.layer, group, CLAY_OP_TRANSITION_RADIAL,
                                  CLAY_BLEND_HARD, 0.0f, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    // an inline group reads no blend or rounding, so it takes none
    CHECK(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_INLINE,
                               CLAY_BLEND_QUADRATIC, 0.2f, 0.0f, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_INLINE, CLAY_BLEND_HARD,
                               0.0f, 0.05f, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD,
                               -1.0f, 0.0f, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_add_group(built.doc, built.layer, 0, -1, 99, CLAY_BLEND_HARD, 0.0f, 0.0f,
                               nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    // The inline op is the mirror image on an item: groups only.
    clay_item_desc s = sphere_desc(0.5f, cf3(0, 0, 0));
    clay_node_id item = 0;
    REQUIRE(clay_add_item(built.doc, built.layer, &s, &item) == CLAY_OK);
    CHECK(clay_layer_set_op_blend(built.doc, built.layer, item, CLAY_OP_INLINE, CLAY_BLEND_HARD,
                                  0.0f, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    // but a group takes it, and the switch is an ordinary undoable edit
    CHECK(clay_layer_set_op_blend(built.doc, built.layer, group, CLAY_OP_INLINE, CLAY_BLEND_HARD,
                                  0.0f, 0.0f) == CLAY_OK);

    // A group's transform never reaches its children, so recording one would
    // be an undoable, saved edit that changes nothing.
    const float origin[3] = {1.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    CHECK(clay_layer_set_transform(built.doc, built.layer, group, origin, axis, 0.0f, 1.0f) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_layer_set_transform(built.doc, built.layer, item, origin, axis, 0.0f, 1.0f) ==
          CLAY_OK);

    // and no node may move into its own subtree, whichever binding asks
    clay_node_id child = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, group, -1, CLAY_OP_ADD, CLAY_BLEND_HARD,
                                 0.0f, 0.0f, &child) == CLAY_OK);
    CHECK(clay_layer_move(built.doc, built.layer, group, child, -1) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(children_of(built.doc, built.layer, group) == std::vector<clay_node_id>{child});
}

TEST_CASE("c groups: an edit to a group undoes exactly") {
    CDoc built;
    REQUIRE(clay_document_enable_undo(built.doc) == CLAY_OK);
    clay_item_desc base = sphere_desc(0.8f, cf3(0, 0, 0));
    REQUIRE(clay_add_item(built.doc, built.layer, &base, nullptr) == CLAY_OK);
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_SUBTRACT,
                                 CLAY_BLEND_HARD, 0.0f, 0.0f, &group) == CLAY_OK);
    clay_item_desc cut = box_desc(cf3(0.3f, 0.3f, 2.0f), cf3(0.4f, 0, 0), CLAY_OP_ADD);
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, group, -1, &cut, nullptr) == CLAY_OK);

    const char* path = "c_groups_undo.clayspace";
    REQUIRE(clay_document_save(built.doc, path) == CLAY_OK);
    const std::vector<std::uint8_t> before = read_file(path);

    // A group's op is an ordinary field edit, so its inverse is the old value.
    REQUIRE(clay_layer_set_op_blend(built.doc, built.layer, group, CLAY_OP_ADD,
                                    CLAY_BLEND_QUADRATIC, 0.2f, 0.0f) == CLAY_OK);
    CHECK(eval_c(built.doc, cf3(0.4f, 0, 0)) < 0.0f);  // now added, not carved
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(built.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_save(built.doc, path) == CLAY_OK);
    CHECK(read_file(path) == before);  // bit-identical, not merely equivalent

    // and so is removing the whole group: the inverse carries the subtree back
    REQUIRE(clay_remove_node(built.doc, built.layer, group) == CLAY_OK);
    REQUIRE(clay_document_undo(built.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_save(built.doc, path) == CLAY_OK);
    CHECK(read_file(path) == before);
    std::remove(path);
}

TEST_CASE("c groups: a document of nested groups round trips bit-identically") {
    CDoc built;
    clay_node_id outer = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_CUBIC,
                                 0.12f, 0.03f, &outer) == CLAY_OK);
    clay_node_id inline_group = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, outer, -1, CLAY_OP_INLINE,
                                 CLAY_BLEND_HARD, 0.0f, 0.0f, &inline_group) == CLAY_OK);
    clay_node_id carve = 0;
    REQUIRE(clay_layer_add_group(built.doc, built.layer, inline_group, -1, CLAY_OP_SUBTRACT,
                                 CLAY_BLEND_QUADRATIC, 0.05f, 0.0f, &carve) == CLAY_OK);
    clay_item_desc a = sphere_desc(0.7f, cf3(0, 0, 0));
    clay_item_desc b = box_desc(cf3(0.3f, 0.3f, 0.9f), cf3(0.4f, 0.1f, 0), CLAY_OP_ADD);
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, inline_group, 0, &a, nullptr) ==
            CLAY_OK);
    REQUIRE(clay_add_item_in_group(built.doc, built.layer, carve, -1, &b, nullptr) == CLAY_OK);

    const char* path = "c_groups_round_trip.clayspace";
    REQUIRE(clay_document_save(built.doc, path) == CLAY_OK);
    const std::vector<std::uint8_t> bytes = read_file(path);
    clay_document* back = nullptr;
    REQUIRE(clay_document_load(path, &back) == CLAY_OK);

    // the tree came back, ids and order included
    CHECK(children_of(back, built.layer, outer) == std::vector<clay_node_id>{inline_group});
    CHECK(children_of(back, built.layer, inline_group).size() == 2);
    CHECK(children_of(back, built.layer, carve).size() == 1);

    clay_test::Lcg rng(31337);
    for (int i = 0; i < 300; ++i) {
        kernel::cfloat3 p = rng.vec3(-2.0f, 2.0f);
        CHECK(eval_c(back, p) == eval_c(built.doc, p));  // exact
    }
    const char* again = "c_groups_round_trip2.clayspace";
    REQUIRE(clay_document_save(back, again) == CLAY_OK);
    CHECK(read_file(again) == bytes);
    clay_document_destroy(back);
    std::remove(path);
    std::remove(again);
}
