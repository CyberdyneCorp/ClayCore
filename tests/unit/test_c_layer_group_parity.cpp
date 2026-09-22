// LAYER-VERSUS-GROUP PARITY, DRIVEN THROUGH THE C ABI
// (fold-the-layers-with-an-operator, the gate task 3.1 was blocked on).
//
// test_layer_parity.cpp states the claim the change stands on -- a composed
// layer of several items IS one group carrying the composition -- and states it
// in C++, against scene::Document. It never crosses clay.h, so a binding that
// implemented a layer's composition some other way is structurally invisible
// to it. The obvious other way is the wrong one: FLATTENING the layer's chain,
// writing the composition onto each of its items. Inside the layer's own chain
// that loses the layer outright -- B(Subtract) opening a chain is a carving node
// with nothing beneath it, and is skipped -- and moved into the chain below it
// is A, B(Subtract), C(Subtract), which subtracts twice where the layer form
// unions B with C first and subtracts once. Under a hard boolean those two
// agree; under a smooth one they do not.
//
// So every document here is built ONLY through clay.h, and read back only
// through it: the field and its colour by clay_eval_points, the safe step,
// exactness and extent by clay_tape_export + clay_tape_info. Each composed pair
// is paired with the FLAT spelling, which must differ, so a binding that
// flattened cannot pass by producing the flat document on both sides.

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

struct Sphere {
    float x, y, z, r;
    float rgb[3];
};

// The same four spheres test_layer_parity.cpp composes: a base of two, and a
// cutter of two that overlap the base and each other.
const Sphere kBase[2] = {{0.0f, 0.0f, 0.0f, 1.0f, {0.8f, 0.2f, 0.2f}},
                         {0.0f, 0.9f, 0.0f, 0.5f, {0.9f, 0.7f, 0.1f}}};
const Sphere kOver[2] = {{0.7f, 0.1f, 0.0f, 0.6f, {0.2f, 0.3f, 0.9f}},
                         {0.6f, 0.0f, 0.6f, 0.4f, {0.1f, 0.8f, 0.4f}}};

struct Comp {
    int32_t op = CLAY_OP_ADD;
    int32_t blend = CLAY_BLEND_HARD;
    float k = 0.0f;
    float rounding = 0.0f;
};

clay_item* sphere_item(const Sphere& s) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &s.r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {s.x, s.y, s.z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_color(it, s.rgb) == CLAY_OK);
    return it;
}

void add_sphere(clay_document* doc, clay_layer_id layer, const Sphere& s) {
    clay_item* it = sphere_item(s);
    REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
}

// The flat spelling's item: the composition written onto the item itself.
void add_sphere_with(clay_document* doc, clay_layer_id layer, const Sphere& s, const Comp& c) {
    clay_item* it = sphere_item(s);
    REQUIRE(clay_item_set_op(it, c.op) == CLAY_OK);
    REQUIRE(clay_item_set_blend(it, c.blend, c.k) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
}

clay_layer_id base_layer(clay_document* doc, const char* name) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    for (const Sphere& s : kBase) add_sphere(doc, layer, s);
    return layer;
}

// TWO LAYERS: the base, and a layer of the two cutter spheres composed over it.
void build_layered(clay_document* doc, const Comp& c) {
    base_layer(doc, "base");
    clay_layer_id over = 0;
    REQUIRE(clay_add_sdf_layer(doc, "over", &over) == CLAY_OK);
    for (const Sphere& s : kOver) add_sphere(doc, over, s);
    REQUIRE(clay_document_set_layer_composition(doc, over, c.op, c.blend, c.k, c.rounding) ==
            CLAY_OK);
}

// ONE LAYER: the base, then ONE GROUP carrying the composition and holding the
// cutter spheres -- the only correct one-layer equivalent of the layer above.
void build_grouped(clay_document* doc, const Comp& c) {
    const clay_layer_id only = base_layer(doc, "only");
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc, only, 0, -1, c.op, c.blend, c.k, c.rounding, &group) ==
            CLAY_OK);
    for (const Sphere& s : kOver) {
        clay_item* it = sphere_item(s);
        REQUIRE(clay_layer_add_item_in_group(doc, only, group, -1, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
    }
}

// ONE LAYER, FLAT: what a binding that flattened the layer's chain produces.
void build_flat(clay_document* doc, const Comp& c) {
    const clay_layer_id only = base_layer(doc, "only");
    for (const Sphere& s : kOver) add_sphere_with(doc, only, s, c);
}

std::vector<float> lattice(int n, float half) {
    std::vector<float> p;
    p.reserve(static_cast<std::size_t>(n) * n * n * 3);
    const float step = 2.0f * half / static_cast<float>(n - 1);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                p.push_back(-half + step * static_cast<float>(i));
                p.push_back(-half + step * static_cast<float>(j));
                p.push_back(-half + step * static_cast<float>(k));
            }
    return p;
}

// Distance AND colour, interleaved, through the host's own evaluation call.
std::vector<float> field(const clay_document* doc, const std::vector<float>& pts) {
    const std::size_t n = pts.size() / 3;
    std::vector<float> d(n), rgb(n * 3);
    REQUIRE(clay_eval_points(doc, nullptr, pts.data(), n, d.data(), rgb.data()) == CLAY_OK);
    std::vector<float> out;
    out.reserve(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(d[i]);
        out.insert(out.end(), rgb.begin() + static_cast<std::ptrdiff_t>(i * 3),
                   rgb.begin() + static_cast<std::ptrdiff_t>(i * 3 + 3));
    }
    return out;
}

// A COUNT, never the vectors: a failing CHECK on two 16^3 lattices would print
// a megabyte of floats.
int differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) ++n;
    return n;
}

struct TapeFacts {
    int32_t exact = -1;
    float lipschitz = 0.0f;
    float step = 0.0f;
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};
};

TapeFacts facts(const clay_document* doc) {
    clay_tape* tape = nullptr;
    REQUIRE(clay_tape_export(doc, nullptr, nullptr, &tape) == CLAY_OK);
    TapeFacts f;
    REQUIRE(clay_tape_info(tape, &f.exact, &f.lipschitz, &f.step, f.lo, f.hi, nullptr) ==
            CLAY_OK);
    clay_tape_release(tape);
    return f;
}

void same_facts(const TapeFacts& a, const TapeFacts& b, bool compare_bounds) {
    CHECK(a.exact == b.exact);
    CHECK(a.lipschitz == doctest::Approx(b.lipschitz));
    CHECK(a.step == doctest::Approx(b.step));
    if (!compare_bounds) return;
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(a.lo[i] == doctest::Approx(b.lo[i]));
        CHECK(a.hi[i] == doctest::Approx(b.hi[i]));
    }
}

}  // namespace

TEST_CASE("c abi: a composed layer of several items is one group of them, not a flat chain") {
    const std::vector<float> pts = lattice(16, 1.6f);

    Comp c;
    // Whether the flat spelling must be a DIFFERENT document, which is what
    // gives the case teeth against a flattening binding. Asserted only for the
    // compositions with a blend or a mode radius of their own: the HARD
    // booleans are associative over this chain -- max(max(a, -c1), -c2) is
    // max(a, -min(c1, c2)) -- so for them the flat chain is the same field and
    // the parity above is the whole claim.
    // Whether the two forms' extents are compared. Only where the composition
    // has no support of its own: a GROUP adds no ring for its own combine, so a
    // smooth or extended group reports the plain union of its children where
    // the layer fold reports that union dilated (tape_build.cpp, compile_group).
    bool compare_bounds = true;
    SUBCASE("union") {}
    SUBCASE("smooth union") {
        c = {CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, 0.25f, 0.0f};
    }
    SUBCASE("chamfered union") {
        c = {CLAY_OP_ADD, CLAY_BLEND_CHAMFER, 0.3f, 0.0f};
    }
    SUBCASE("subtract") {
        c = {CLAY_OP_SUBTRACT, CLAY_BLEND_HARD, 0.0f, 0.0f};
    }
    SUBCASE("smooth subtract") {
        c = {CLAY_OP_SUBTRACT, CLAY_BLEND_QUADRATIC, 0.25f, 0.0f};
    }
    SUBCASE("intersect") {
        c = {CLAY_OP_INTERSECT, CLAY_BLEND_HARD, 0.0f, 0.0f};
    }
    SUBCASE("smooth intersect") {
        c = {CLAY_OP_INTERSECT, CLAY_BLEND_CUBIC, 0.2f, 0.0f};
    }
    SUBCASE("paint") {
        c = {CLAY_OP_PAINT, CLAY_BLEND_QUADRATIC, 0.3f, 0.0f};
    }
    SUBCASE("groove, with a rounding") {
        c = {CLAY_OP_GROOVE, CLAY_BLEND_HARD, 0.15f, 0.1f};
    }
    SUBCASE("shell") {
        c = {CLAY_OP_SHELL, CLAY_BLEND_HARD, 0.12f, 0.0f};
    }
    SUBCASE("incise, with a rounding") {
        c = {CLAY_OP_INCISE, CLAY_BLEND_HARD, 0.2f, 0.25f};
    }
    compare_bounds = c.k == 0.0f && c.rounding == 0.0f;
    const bool flat_differs = c.k > 0.0f;
    CAPTURE(c.op);
    CAPTURE(c.blend);

    Doc layered, grouped, flat;
    build_layered(layered.doc, c);
    build_grouped(grouped.doc, c);
    build_flat(flat.doc, c);

    const std::vector<float> layered_f = field(layered.doc, pts);
    const std::vector<float> grouped_f = field(grouped.doc, pts);
    CHECK(differing(layered_f, grouped_f) == 0);
    same_facts(facts(layered.doc), facts(grouped.doc), compare_bounds);

    // THE TEETH. The flat chain is what a flattening binding would have built on
    // BOTH sides; if it were the same document as the group, the check above
    // could not tell the two bindings apart.
    const int flat_diff = differing(field(flat.doc, pts), grouped_f);
    CAPTURE(flat_diff);
    if (flat_differs) CHECK(flat_diff > 0);
}

TEST_CASE("c abi: three composed layers fold in order, as nested groups do") {
    // A - B + C against A + C - B: the same three layers in two orders, which a
    // host that treated layer order as cosmetic would draw identically. Each
    // must equal its one-layer spelling, and the two orders must differ.
    const std::vector<float> pts = lattice(16, 1.6f);
    const Comp sub{CLAY_OP_SUBTRACT, CLAY_BLEND_QUADRATIC, 0.2f, 0.0f};
    const Comp add{};
    const Sphere cap{0.9f, 0.3f, 0.0f, 0.35f, {0.3f, 0.9f, 0.3f}};

    auto layered = [&](clay_document* doc, bool cut_last) {
        base_layer(doc, "A");
        clay_layer_id b = 0, c = 0;
        auto add_b = [&] {
            REQUIRE(clay_add_sdf_layer(doc, "B", &b) == CLAY_OK);
            for (const Sphere& s : kOver) add_sphere(doc, b, s);
            REQUIRE(clay_document_set_layer_composition(doc, b, sub.op, sub.blend, sub.k,
                                                        sub.rounding) == CLAY_OK);
        };
        auto add_c = [&] {
            REQUIRE(clay_add_sdf_layer(doc, "C", &c) == CLAY_OK);
            add_sphere(doc, c, cap);
        };
        if (cut_last) {
            add_c();
            add_b();
        } else {
            add_b();
            add_c();
        }
    };
    auto grouped = [&](clay_document* doc, bool cut_last) {
        const clay_layer_id only = base_layer(doc, "only");
        auto group_b = [&] {
            clay_node_id g = 0;
            REQUIRE(clay_layer_add_group(doc, only, 0, -1, sub.op, sub.blend, sub.k, sub.rounding,
                                         &g) == CLAY_OK);
            for (const Sphere& s : kOver) {
                clay_item* it = sphere_item(s);
                REQUIRE(clay_layer_add_item_in_group(doc, only, g, -1, it, nullptr) == CLAY_OK);
                clay_item_destroy(it);
            }
        };
        auto group_c = [&] {
            clay_node_id g = 0;
            REQUIRE(clay_layer_add_group(doc, only, 0, -1, add.op, add.blend, add.k, add.rounding,
                                         &g) == CLAY_OK);
            clay_item* it = sphere_item(cap);
            REQUIRE(clay_layer_add_item_in_group(doc, only, g, -1, it, nullptr) == CLAY_OK);
            clay_item_destroy(it);
        };
        if (cut_last) {
            group_c();
            group_b();
        } else {
            group_b();
            group_c();
        }
    };

    Doc l_cut_first, g_cut_first, l_cut_last, g_cut_last;
    layered(l_cut_first.doc, false);
    grouped(g_cut_first.doc, false);
    layered(l_cut_last.doc, true);
    grouped(g_cut_last.doc, true);

    const std::vector<float> a = field(l_cut_first.doc, pts);
    const std::vector<float> b = field(l_cut_last.doc, pts);
    CHECK(differing(a, field(g_cut_first.doc, pts)) == 0);
    CHECK(differing(b, field(g_cut_last.doc, pts)) == 0);
    // Extents not compared: B is a SMOOTH subtract, and a group adds no ring
    // for its own combine where the layer fold does.
    same_facts(facts(l_cut_first.doc), facts(g_cut_first.doc), false);
    same_facts(facts(l_cut_last.doc), facts(g_cut_last.doc), false);
    // Teeth: the order is the modelling decision, so the two must differ.
    CHECK(differing(a, b) > 0);
}
