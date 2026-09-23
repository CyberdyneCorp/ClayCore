// AN INFINITE GRID'S MATERIAL IS EVERYWHERE, and `tape.bounds` has to say so.
//
// `tape.bounds` is where the field can hold material: what meshing marches,
// what a raycast clips against and what a host plans bricks over
// (clay_tape_info). An item repeated on an infinite grid has a geometry bound
// of ONE CELL while its copies fill space. #637 stopped that cell narrowing an
// intersect (item_material_extent), but where the lattice reached the result
// unconfined -- a lattice minus a sphere, a lattice on its own -- the tape still
// fell back to the plain union of item bounds, i.e. the one cell, and every
// copy outside it sat outside the reported bounds: surface a planner never
// visits and a ray clipped away before it could hit.
//
// Defended on the FIELD, as a count: sample a region several cells wide and
// require zero material samples outside the reported box, for a lattice minus
// a sphere under every blend profile, with rounding, inside a group, as a
// layer, through every compile entry point, and resumed by
// compile_document_append. The confined cases -- an intersect with something
// finite, a finite shape minus the lattice -- must stay finite and narrow.

#include <doctest/doctest.h>

#include <vector>

#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::scene;
using kernel::cf3;
using kernel::cfloat3;

namespace {

constexpr float kSpacing = 1.5f;

Node sphere_at(cfloat3 pos, float r, Op op = Op::Add, BlendProfile profile = BlendProfile::Hard,
               float k = 0.0f) {
    Node n;
    n.prim = Prim::sphere(r);
    n.xform.position = pos;
    n.op = op;
    n.blend = Blend{profile, k};
    return n;
}

Node box_at(cfloat3 pos, cfloat3 half, Op op = Op::Add) {
    Node n;
    n.prim = Prim::box(half);
    n.xform.position = pos;
    n.op = op;
    return n;
}

// A box of half-size 0.4 on a 1.5 grid: every cell holds material, none of it
// touches a neighbour, and the cell at the origin is [-0.4, 0.4]^3.
Node lattice(float rounding = 0.0f) {
    Node n = box_at(cf3(0, 0, 0), cf3(0.4f, 0.4f, 0.4f));
    n.rounding = rounding;
    n.repeat = Repeat::grid_infinite(cf3(kSpacing, kSpacing, kSpacing));
    return n;
}

Node group_of(Op op) {
    Node g;
    g.is_group = true;
    g.op = op;
    return g;
}

// Five cells either way on every axis: far past the one cell the lattice's
// geometry bound covers, so a one-cell box leaves most of the material out.
std::vector<cfloat3> samples(int n = 32) {
    const float half = 5.0f * kSpacing;
    std::vector<cfloat3> pts;
    pts.reserve(static_cast<std::size_t>(n) * n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k)
                pts.push_back(cf3(-half + 2 * half * float(i) / float(n - 1),
                                  -half + 2 * half * float(j) / float(n - 1),
                                  -half + 2 * half * float(k) / float(n - 1)));
    return pts;
}

struct Census {
    int inside = 0;   // material samples
    int outside = 0;  // ...of which outside the reported bounds
};

Census census(const Tape& t) {
    Census c;
    for (cfloat3 p : samples()) {
        if (t.eval(p).d > 0.0f) continue;
        ++c.inside;
        if (!t.bounds.contains(p)) ++c.outside;
    }
    return c;
}

void require_compiled(const Tape& t) {
    REQUIRE_FALSE(t.empty());
    REQUIRE_FALSE(t.bounds.empty());
}

// Sound, and not trivially so: the field holds material in many cells.
void check_sound(const Tape& t) {
    require_compiled(t);
    const Census c = census(t);
    CAPTURE(c.inside);
    CAPTURE(c.outside);
    CHECK(c.inside > 100);
    CHECK(c.outside == 0);
}

// The document every case below varies: a lattice with a sphere cut out of it.
Document lattice_minus_sphere(BlendProfile profile, float k, float rounding = 0.0f) {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(lattice(rounding));
    l.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f, Op::Subtract, profile, k));
    return doc;
}

void check_profiles() {
    const BlendProfile profiles[] = {BlendProfile::Hard, BlendProfile::Quadratic,
                                     BlendProfile::Cubic, BlendProfile::Circular,
                                     BlendProfile::Chamfer};
    for (BlendProfile p : profiles) {
        CAPTURE(static_cast<int>(p));
        const float k = p == BlendProfile::Hard ? 0.0f : 0.2f;
        check_sound(compile_document(lattice_minus_sphere(p, k)));
    }
}

void check_grouped() {
    SUBCASE("the subtract inside a group with the lattice") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        const NodeId g = l.sdf->insert(group_of(Op::Add));
        l.sdf->insert(lattice(), g);
        l.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f, Op::Subtract), g);
        check_sound(compile_document(doc));
    }
    SUBCASE("a subtracting group applied to the lattice") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(lattice());
        const NodeId g = l.sdf->insert(group_of(Op::Subtract));
        l.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f), g);
        check_sound(compile_document(doc));
    }
    SUBCASE("a subtracting layer over a lattice layer") {
        Document doc;
        doc.add_sdf_layer("base").sdf->insert(lattice());
        Layer& cut = doc.add_sdf_layer("cut");
        cut.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f));
        cut.composition.op = Op::Subtract;
        check_sound(compile_document(doc));
    }
}

// Every compile entry point reports the same answer for the same field.
void check_entry_points() {
    const Document doc = lattice_minus_sphere(BlendProfile::Hard, 0.0f);
    const Layer& l = doc.layers.front();
    check_sound(compile_layer(l));
    check_sound(compile_document_part(doc, l.id, /*below=*/false));
    Tape prefix;
    REQUIRE(compile_layer_prefix(doc, 1, &prefix));
    check_sound(prefix);
    check_sound(compile_item(l, *l.sdf->find(l.sdf->roots.front())));
}

// The lattice compiled resumably, then the subtract appended beside it.
Tape append_subtract(Document* doc, bool into_group) {
    Layer& l = doc->add_sdf_layer("l");
    NodeId parent = kNoNode;
    if (into_group) parent = l.sdf->insert(group_of(Op::Add));
    l.sdf->insert(lattice(), parent);
    TapeCheckpoint cp;
    const Tape prefix = compile_document_resumable(*doc, &cp);
    const NodeId added = l.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f, Op::Subtract), parent);
    Tape reused;
    REQUIRE(compile_document_append(prefix, cp, *doc, {added}, &reused, nullptr));
    return reused;
}

// The append path finishes `tape.bounds` from the checkpoint's own extents; a
// lattice in the prefix has to reach the appended subtract's result there too.
void check_resumed(bool into_group) {
    Document doc;
    const Tape reused = append_subtract(&doc, into_group);
    const Tape full = compile_document(doc);
    CHECK(reused.bounds.min.x == full.bounds.min.x);
    CHECK(reused.bounds.max.x == full.bounds.max.x);
    check_sound(reused);
    check_sound(full);
}

}  // namespace

TEST_CASE("lattice bounds: a lattice minus a sphere holds no material outside its bounds") {
    SUBCASE("every blend profile") { check_profiles(); }
    SUBCASE("a rounded lattice") {
        check_sound(compile_document(lattice_minus_sphere(BlendProfile::Quadratic, 0.2f, 0.1f)));
    }
    SUBCASE("the lattice on its own") {
        Document doc;
        doc.add_sdf_layer("l").sdf->insert(lattice());
        check_sound(compile_document(doc));
    }
    SUBCASE("unioned with a finite shape") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f));
        l.sdf->insert(lattice());
        check_sound(compile_document(doc));
    }
}

TEST_CASE("lattice bounds: groups, layers and every entry point agree") {
    check_grouped();
    SUBCASE("entry points") { check_entry_points(); }
}

TEST_CASE("lattice bounds: a resumed subtract reports the full compile's box") {
    SUBCASE("at the root") { check_resumed(false); }
    SUBCASE("inside a group") { check_resumed(true); }
}

// The unbounded answer is reserved for material that really is unbounded:
// a lattice something finite CONFINES stays finite and narrow.
TEST_CASE("lattice bounds: a confined lattice stays finite") {
    SUBCASE("a finite shape minus the lattice keeps the shape's box") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(box_at(cf3(0, 0, 0), cf3(2, 2, 2)));
        Node cutter = lattice();
        cutter.op = Op::Subtract;
        l.sdf->insert(cutter);
        const Tape t = compile_document(doc);
        CHECK_FALSE(t.bounds.is_infinite());
        CHECK(t.bounds.max.x == doctest::Approx(2.0f));
        check_sound(t);
    }
    SUBCASE("a lattice intersected with a box is bounded by the box") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(lattice());
        l.sdf->insert(box_at(cf3(0, 0, 0), cf3(3, 3, 3), Op::Intersect));
        const Tape t = compile_document(doc);
        CHECK_FALSE(t.bounds.is_infinite());
        CHECK(t.bounds.max.x == doctest::Approx(3.0f));
        check_sound(t);
    }
}
