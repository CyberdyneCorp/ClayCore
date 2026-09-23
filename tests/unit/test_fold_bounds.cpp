// BOUNDS PER OPERATOR (fold-the-layers-with-an-operator, task 3.1).
//
// `tape.bounds` is what meshing marches and what a raycast clips against, and
// the only way it can be wrong that matters is by being too SMALL: that renders
// as missing surface, with no error. This change NARROWS it -- a subtract keeps
// its left operand's extent, an intersect the overlap -- so the first thing
// defended here is soundness, on the FIELD rather than on the arithmetic that
// produced the box: no sample with material lies outside it, across subtract
// and intersect in every blend profile, item rounding, smooth groups whose own
// ring reaches past their children, composed layers, and the two nested.
//
// The second thing defended is that the narrowing HAPPENS, as a count: each
// narrowing fixture's box is strictly smaller than the plain union of its
// items, so a combine_extent that quietly unioned again would fail here rather
// than pass as "sound".

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

Node sphere_at(cfloat3 pos, float r, Op op = Op::Add, BlendProfile profile = BlendProfile::Hard,
               float k = 0.0f, float rounding = 0.0f) {
    Node n;
    n.prim = Prim::sphere(r);
    n.xform.position = pos;
    n.op = op;
    n.blend = Blend{profile, k};
    n.rounding = rounding;
    return n;
}

Node box_at(cfloat3 pos, cfloat3 half, Op op = Op::Add) {
    Node n;
    n.prim = Prim::box(half);
    n.xform.position = pos;
    n.op = op;
    return n;
}

Node group_of(Op op, BlendProfile profile = BlendProfile::Hard, float k = 0.0f) {
    Node g;
    g.is_group = true;
    g.op = op;
    g.blend = Blend{profile, k};
    return g;
}

// A regular lattice over `box` grown by `margin` on every side, so a sample can
// land OUTSIDE the reported bounds -- which is the only place a failure lives.
std::vector<cfloat3> lattice_around(const math::Aabb& box, float margin, int n) {
    const cfloat3 lo = box.min - cf3(margin, margin, margin);
    const cfloat3 span = box.extent() + cf3(2 * margin, 2 * margin, 2 * margin);
    std::vector<cfloat3> pts;
    pts.reserve(static_cast<std::size_t>(n) * n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k)
                pts.push_back(lo + cf3(span.x * float(i) / float(n - 1),
                                       span.y * float(j) / float(n - 1),
                                       span.z * float(k) / float(n - 1)));
    return pts;
}

int material_outside(const Tape& t, const std::vector<cfloat3>& pts) {
    int out = 0;
    for (cfloat3 p : pts)
        if (t.eval(p).d <= 0.0f && !t.bounds.contains(p)) ++out;
    return out;
}

int material_inside(const Tape& t, const std::vector<cfloat3>& pts) {
    int in = 0;
    for (cfloat3 p : pts)
        if (t.eval(p).d <= 0.0f) ++in;
    return in;
}

// The plain union of every item's own geometry bound: what tape.bounds was for
// a document of items before a combine could narrow it.
void expand_items(const Layer& l, const std::vector<NodeId>& ids, math::Aabb* u) {
    for (NodeId id : ids) {
        const Node* n = l.sdf->find(id);
        if (!n) continue;
        if (n->is_group)
            expand_items(l, n->children, u);
        else
            u->expand(item_geometry_bound(*n, l));
    }
}

math::Aabb item_union(const Document& doc) {
    math::Aabb u;
    for (const Layer& l : doc.layers) expand_items(l, l.sdf->roots, &u);
    return u;
}

float volume(const math::Aabb& b) {
    const cfloat3 e = b.extent();
    return e.x * e.y * e.z;
}

// Sound, and not trivially so: the field has material somewhere on the lattice.
void check_sound(const Document& doc, int n = 48) {
    const Tape t = compile_document(doc);
    REQUIRE_FALSE(t.bounds.empty());
    const std::vector<cfloat3> pts = lattice_around(item_union(doc), 0.3f, n);
    CHECK(material_inside(t, pts) > 0);
    const int outside = material_outside(t, pts);
    CAPTURE(outside);
    CHECK(outside == 0);
}

const BlendProfile kProfiles[] = {BlendProfile::Hard, BlendProfile::Quadratic,
                                  BlendProfile::Cubic, BlendProfile::Circular,
                                  BlendProfile::Chamfer};

}  // namespace

TEST_CASE("fold bounds: combine_extent, row by row") {
    const math::Aabb a{cf3(0, 0, 0), cf3(2, 2, 2)};
    const math::Aabb b{cf3(1, 1, 1), cf3(5, 5, 5)};
    const math::Aabb far{cf3(10, 10, 10), cf3(11, 11, 11)};

    SUBCASE("a subtract keeps its left operand, gated or not, whatever it cuts with") {
        const math::Aabb r = combine_extent(Op::Subtract, a, b, 0.7f, false);
        CHECK(r.min.x == 0.0f);
        CHECK(r.max.x == 2.0f);
        CHECK(combine_extent(Op::Subtract, a, b, 0.0f, true).max.y == 2.0f);
    }
    SUBCASE("an intersect is the overlap, with the right operand's ring") {
        const math::Aabb hard = combine_extent(Op::Intersect, a, b, 0.0f, false);
        CHECK(hard.min.x == 1.0f);
        CHECK(hard.max.x == 2.0f);
        // The ring widens the right operand as it does for a union, so a layer
        // or group reports what the same combine spelled on an item reports.
        const math::Aabb ringed = combine_extent(Op::Intersect, a, b, 0.5f, false);
        CHECK(ringed.min.x == doctest::Approx(0.5f));
        CHECK(ringed.max.x == 2.0f);
    }
    SUBCASE("a gated intersect keeps the left operand: the protected region is untouched") {
        CHECK(combine_extent(Op::Intersect, a, b, 0.0f, true).min.x == 0.0f);
    }
    SUBCASE("a disjoint intersect keeps the left operand rather than going empty") {
        const math::Aabb r = combine_extent(Op::Intersect, a, far, 0.0f, false);
        CHECK_FALSE(r.empty());
        CHECK(r.max.x == 2.0f);
    }
    SUBCASE("everything else unions, with the right operand dilated by the ring") {
        for (Op op : {Op::Add, Op::Paint, Op::Groove, Op::Shell, Op::Relief, Op::Incise}) {
            CAPTURE(static_cast<int>(op));
            const math::Aabb r = combine_extent(op, a, b, 0.5f, false);
            CHECK(r.min.x == 0.0f);
            CHECK(r.max.x == 5.5f);
        }
    }
    SUBCASE("an empty left operand takes the right one") {
        CHECK(combine_extent(Op::Add, math::Aabb{}, b, 0.0f, false).max.x == 5.0f);
    }
}

TEST_CASE("fold bounds: an item subtract is bounded by what it cuts, in every profile") {
    // A small base carved by a LARGE cutter: the union is the cutter's box, and
    // everything that can be material is inside the base's.
    for (BlendProfile profile : kProfiles) {
        CAPTURE(static_cast<int>(profile));
        const float k = profile == BlendProfile::Hard ? 0.0f : 0.15f;
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(sphere_at(cf3(0, 0, 0), 0.5f, Op::Add, BlendProfile::Hard, 0, 0.05f));
        l.sdf->insert(sphere_at(cf3(1.6f, 0, 0), 1.3f, Op::Subtract, profile, k, 0.08f));
        check_sound(doc);
        const Tape t = compile_document(doc);
        CHECK(volume(t.bounds) < 0.5f * volume(item_union(doc)));
    }
}

TEST_CASE("fold bounds: an item intersect is bounded by the overlap, in every profile") {
    for (BlendProfile profile : kProfiles) {
        CAPTURE(static_cast<int>(profile));
        const float k = profile == BlendProfile::Hard ? 0.0f : 0.1f;
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(sphere_at(cf3(-0.4f, 0, 0), 0.7f));
        l.sdf->insert(sphere_at(cf3(0.5f, 0.1f, 0), 0.7f, Op::Intersect, profile, k, 0.05f));
        check_sound(doc);
        const Tape t = compile_document(doc);
        CHECK(volume(t.bounds) < 0.8f * volume(item_union(doc)));
    }
}

TEST_CASE("fold bounds: a chain that carves then adds again") {
    // A - B + C: the add after the subtract must bring C's extent back.
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(sphere_at(cf3(0, 0, 0), 0.5f));
    l.sdf->insert(sphere_at(cf3(0.9f, 0, 0), 0.8f, Op::Subtract, BlendProfile::Quadratic, 0.1f));
    l.sdf->insert(sphere_at(cf3(0, 1.2f, 0), 0.3f, Op::Add, BlendProfile::Cubic, 0.1f));
    check_sound(doc);
    const Tape t = compile_document(doc);
    CHECK(t.bounds.max.y > 1.5f);   // C is in
    CHECK(t.bounds.max.x < 1.0f);   // B is not: its box reaches x = 1.7
}

TEST_CASE("fold bounds: a smooth GROUP's own ring is inside the box") {
    // THE GAP THIS CLOSES, beside the narrowing: a group used to add no ring for
    // its own combine, so its box was the plain union of its children. Two slabs
    // that abut, the right one in a smooth-union group: the bridge over the
    // shared edge stands proud of BOTH item boxes, and only the ring holds it.
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(box_at(cf3(-0.5f, 0, 0), cf3(0.5f, 0.1f, 0.1f)));
    const NodeId g = l.sdf->insert(group_of(Op::Add, BlendProfile::Quadratic, 0.2f));
    l.sdf->insert(box_at(cf3(0.5f, 0, 0), cf3(0.5f, 0.1f, 0.1f)), g);
    const Tape t = compile_document(doc);
    const std::vector<cfloat3> pts = lattice_around(item_union(doc), 0.2f, 61);
    CHECK(material_outside(t, pts) == 0);
    // Teeth: the same material IS outside the plain union of the items.
    int escaped = 0;
    const math::Aabb plain = item_union(doc);
    for (cfloat3 p : pts)
        if (t.eval(p).d <= 0.0f && !plain.contains(p)) ++escaped;
    CAPTURE(escaped);
    CHECK(escaped > 0);
}

TEST_CASE("fold bounds: groups narrow as items do, and nest") {
    for (BlendProfile profile : kProfiles) {
        CAPTURE(static_cast<int>(profile));
        const float k = profile == BlendProfile::Hard ? 0.0f : 0.12f;

        SUBCASE("a subtracting group of two cutters") {
            Document doc;
            Layer& l = doc.add_sdf_layer("l");
            l.sdf->insert(sphere_at(cf3(0, 0, 0), 0.6f));
            const NodeId g = l.sdf->insert(group_of(Op::Subtract, profile, k));
            l.sdf->insert(sphere_at(cf3(1.4f, 0, 0), 1.0f), g);
            l.sdf->insert(sphere_at(cf3(0, 1.5f, 0), 1.1f), g);
            check_sound(doc);
            CHECK(volume(compile_document(doc).bounds) < 0.5f * volume(item_union(doc)));
        }
        SUBCASE("an intersecting group inside a smooth union group") {
            Document doc;
            Layer& l = doc.add_sdf_layer("l");
            l.sdf->insert(sphere_at(cf3(-0.8f, 0, 0), 0.4f));
            const NodeId outer = l.sdf->insert(group_of(Op::Add, BlendProfile::Quadratic, 0.1f));
            l.sdf->insert(sphere_at(cf3(0.3f, 0, 0), 0.6f), outer);
            const NodeId inner = l.sdf->insert(group_of(Op::Intersect, profile, k), outer);
            l.sdf->insert(sphere_at(cf3(0.8f, 0.2f, 0), 0.5f), inner);
            check_sound(doc);
        }
    }
}

TEST_CASE("fold bounds: composed layers narrow as the group they equal does") {
    for (BlendProfile profile : kProfiles) {
        CAPTURE(static_cast<int>(profile));
        const float k = profile == BlendProfile::Hard ? 0.0f : 0.12f;
        for (Op op : {Op::Subtract, Op::Intersect}) {
            CAPTURE(static_cast<int>(op));
            Document doc;
            doc.add_sdf_layer("base").sdf->insert(sphere_at(cf3(0, 0, 0), 0.6f));
            Layer& over = doc.add_sdf_layer("over");
            over.sdf->insert(sphere_at(cf3(1.0f, 0, 0), 0.8f, Op::Add, BlendProfile::Hard, 0,
                                       0.05f));
            over.sdf->insert(sphere_at(cf3(0.9f, 0.6f, 0), 0.5f));
            over.composition.op = op;
            over.composition.blend = Blend{profile, k};
            check_sound(doc);
            CHECK(volume(compile_document(doc).bounds) < 0.8f * volume(item_union(doc)));
        }
    }
    SUBCASE("and a smooth union layer above a subtract keeps the ring") {
        Document doc;
        doc.add_sdf_layer("base").sdf->insert(sphere_at(cf3(0, 0, 0), 0.6f));
        Layer& cut = doc.add_sdf_layer("cut");
        cut.sdf->insert(sphere_at(cf3(0.8f, 0, 0), 0.5f));
        cut.composition.op = Op::Subtract;
        Layer& add = doc.add_sdf_layer("add");
        add.sdf->insert(box_at(cf3(0, 0.75f, 0), cf3(0.3f, 0.15f, 0.3f)));
        add.composition.op = Op::Add;
        add.composition.blend = Blend{BlendProfile::Quadratic, 0.15f};
        check_sound(doc, 56);
    }
}

// AN INFINITE GRID'S GEOMETRY BOUND IS ONE CELL, while its copies fill space.
// As a union operand that cell is the box tape.bounds has always marched; as a
// narrowing one it is wrong -- an intersect bounded by it kept one sphere of a
// lattice the field holds everywhere inside the box: 936 material samples
// outside the reported bounds on this fixture before item_material_extent.
TEST_CASE("fold bounds: an intersect with an infinite grid keeps the whole left operand") {
    Node lattice = sphere_at(cf3(0, 0, 0), 0.3f);
    lattice.repeat = Repeat::grid_infinite(cf3(1, 1, 1));
    SUBCASE("as an item") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(box_at(cf3(0, 0, 0), cf3(2, 2, 2)));
        lattice.op = Op::Intersect;
        l.sdf->insert(lattice);
        check_sound(doc);
    }
    SUBCASE("inside an intersecting group") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(box_at(cf3(0, 0, 0), cf3(2, 2, 2)));
        const NodeId g = l.sdf->insert(group_of(Op::Intersect));
        l.sdf->insert(lattice, g);
        check_sound(doc);
    }
    SUBCASE("as an intersecting layer") {
        Document doc;
        doc.add_sdf_layer("base").sdf->insert(box_at(cf3(0, 0, 0), cf3(2, 2, 2)));
        Layer& over = doc.add_sdf_layer("over");
        over.sdf->insert(lattice);
        over.composition.op = Op::Intersect;
        check_sound(doc);
    }
    SUBCASE("resumed: an append into the intersecting group reports the full compile's box") {
        Document doc;
        Layer& l = doc.add_sdf_layer("l");
        l.sdf->insert(box_at(cf3(0, 0, 0), cf3(2, 2, 2)));
        const NodeId g = l.sdf->insert(group_of(Op::Intersect));
        l.sdf->insert(sphere_at(cf3(0, 0, 0), 0.4f), g);
        TapeCheckpoint cp;
        const Tape prefix = compile_document_resumable(doc, &cp);
        const NodeId added = l.sdf->insert(lattice, g);
        Tape reused;
        REQUIRE(compile_document_append(prefix, cp, doc, {added}, &reused, nullptr));
        const Tape full = compile_document(doc);
        CHECK(reused.bounds.min.x == full.bounds.min.x);
        CHECK(reused.bounds.max.x == full.bounds.max.x);
        CHECK(full.bounds.max.x == doctest::Approx(2.0f));
        check_sound(doc);
    }
}

// ...and where nothing confines the lattice, the box is the one-cell union it
// always was rather than an infinite one the mesher would refuse outright.
TEST_CASE("fold bounds: an unconfined infinite grid keeps the box it always reported") {
    Document doc;
    Layer& l = doc.add_sdf_layer("l");
    Node lattice = box_at(cf3(0, 0, 0), cf3(2, 2, 2));
    lattice.repeat = Repeat::grid_infinite(cf3(5, 5, 5));
    l.sdf->insert(lattice);
    l.sdf->insert(sphere_at(cf3(0, 0, 0), 1.0f, Op::Subtract));
    const Tape t = compile_document(doc);
    REQUIRE_FALSE(t.bounds.empty());
    CHECK_FALSE(t.bounds.is_infinite());
    CHECK(t.bounds.max.x == doctest::Approx(2.0f));
}
