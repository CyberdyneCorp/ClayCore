// The two automask factors a host could not reach (reach-every-automask-from-a-
// host): CAVITY and SURFACE_GROUP.
//
// Their bits have been in clay_mesh_brush_desc since automasking shipped and
// setting them did nothing, because each consumes an input a struct of scalars
// cannot hold. The header said so and called the descriptor carrying them a
// follow-up. This is that follow-up's test.
//
// WHY A BINDING-LEVEL TEST AND NOT A UNIT ONE. The C++ suite already proves
// what a cavity gate DOES — automask.cpp's factors are covered there and the
// maths is not retested here. What was never covered is the only thing that was
// ever wrong: whether a host can switch one on. "The engine can do it, the host
// cannot reach it" is a defect that every internal test passes through, so the
// test for it has to be written against clay.h and nothing else.
//
// WHAT EACH CASE HAS TO SHOW:
//
//   1. The bit alone still does nothing, and the bit WITH a source does
//      something. Both halves, or the case cannot tell a working automask from
//      a stamp that was going to be small anyway.
//   2. The value reaches the WEIGHT rather than merely reaching the engine: a
//      cavity of 1 at full strength holds a vertex exactly still, and half
//      strength moves it part of the way.
//   3. Clearing the sources puts both factors back to inert, so a session can
//      let go of a document it no longer holds.
//   4. The lattices are asked at the vertex's WORLD position. This is the one
//      that was wrong in the engine, not just missing from the ABI.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() { REQUIRE(doc != nullptr); }
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

struct Mask {
    clay_mask* m = nullptr;
    explicit Mask(float cell) : m(clay_mask_create(cell)) { REQUIRE(m != nullptr); }
    ~Mask() { clay_mask_destroy(m); }
    Mask(const Mask&) = delete;
    Mask& operator=(const Mask&) = delete;
    operator clay_mask*() const { return m; }
};

struct Groups {
    clay_groups* g = nullptr;
    Groups(clay_document* doc, float cell) {
        REQUIRE(clay_document_groups(doc, cell, &g) == CLAY_OK);
        REQUIRE(g != nullptr);
    }
    ~Groups() { clay_groups_destroy(g); }
    Groups(const Groups&) = delete;
    Groups& operator=(const Groups&) = delete;
    operator clay_groups*() const { return g; }
};

// A flat grid on the XZ plane, centred on the origin. Flat so a Draw
// displacement is exactly the +Y component and nothing else has to be
// projected out of it.
clay_mesh* grid_mesh(int n, float half) {
    std::vector<float> p;
    std::vector<std::uint32_t> idx;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            p.push_back(-half + step * static_cast<float>(x));
            p.push_back(0.0f);
            p.push_back(-half + step * static_cast<float>(z));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(n); ++z)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(n); ++x) {
            const std::uint32_t a = z * stride + x, b = a + 1, c = a + stride, d = c + 1;
            for (std::uint32_t i : {a, c, b, b, c, d}) idx.push_back(i);
        }
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(p.data(), p.size() / 3, idx.data(), idx.size(), &m) ==
            CLAY_OK);
    return m;
}

clay_mesh_brush_desc draw_brush(float radius, float strength, std::uint32_t factors) {
    clay_mesh_brush_desc d;
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.radius = radius;
    d.strength = strength;
    d.automask_factors = factors;
    return d;
}

clay_automask_sources sources_of(const clay_mask* cavity, const clay_groups* groups,
                                 std::uint32_t active) {
    clay_automask_sources s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<std::uint32_t>(sizeof s);
    s.cavity = cavity;
    s.groups = groups;
    s.active_group = active;
    return s;
}

// How far the mesh moved off the plane, summed. A Draw on a flat grid displaces
// along +Y only, so this is the whole of what a stamp did and an automask can
// only ever reduce it.
double lift(const clay_mesh* m) {
    const float* p = clay_mesh_positions(m);
    const std::size_t n = clay_mesh_vertex_count(m);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += std::fabs(static_cast<double>(p[i * 3 + 1]));
    return sum;
}

// The lift of one stamp at the origin under exactly these factors and sources.
// Each call builds its own mesh and sculptor, so nothing carries over.
// A session that declares a frame speaks WORLD in every call, the stamp's own
// centre included -- see "WHICH SPACE A SESSION SPEAKS" in clay.h. So a fixture
// that moves the mesh has to move the brush with it, or the stamp reaches
// nothing and every comparison against it passes for the wrong reason.
double stamp_lift(std::uint32_t factors, const clay_automask_sources* sources,
                  const clay_mesh_frame* frame = nullptr, float cavity_strength = 1.0f) {
    clay_mesh* m = grid_mesh(24, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
    if (frame) REQUIRE(clay_mesh_sculptor_set_world_frame(s, frame) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_set_automask_sources(s, sources) == CLAY_OK);
    clay_mesh_brush_desc d = draw_brush(0.6f, 0.5f, factors);
    if (frame) {
        d.center[0] = frame->position[0];
        d.center[1] = frame->position[1];
        d.center[2] = frame->position[2];
    }
    d.automask_cavity_strength = cavity_strength;
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
    const double moved_by = lift(m);
    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
    return moved_by;
}

}  // namespace

TEST_CASE("c abi: the cavity bit alone is inert, and with a source it holds the surface") {
    Doc doc;
    // A cavity of 1 everywhere the brush can reach. Painted rather than
    // measured, because what this case is about is whether the number reaches
    // the weight — a measured field would put a fixture's shape between the
    // claim and the evidence.
    Mask cavity(0.05f);
    const float lo[3] = {-2.0f, -2.0f, -2.0f};
    const float hi[3] = {2.0f, 2.0f, 2.0f};
    REQUIRE(clay_mask_fill(cavity, lo, hi, 1.0f) == CLAY_OK);

    const double plain = stamp_lift(0, nullptr);
    REQUIRE(plain > 0.0);

    // THE OLD BEHAVIOUR, kept as an assertion rather than as a memory: the bit
    // with no source changes nothing. A host that sets its automask preset
    // before it has baked a cavity mask gets the stamps it had.
    CHECK(stamp_lift(CLAY_AUTOMASK_CAVITY, nullptr) == doctest::Approx(plain));

    const clay_automask_sources with = sources_of(cavity, nullptr, 0);
    // And a source with no bit is equally inert: naming the inputs does not
    // switch anything on, which is what makes wiring this up once at session
    // start safe.
    CHECK(stamp_lift(0, &with) == doctest::Approx(plain));

    // Both: a full cavity at full strength multiplies every weight by zero.
    CHECK(stamp_lift(CLAY_AUTOMASK_CAVITY, &with) == doctest::Approx(0.0));
    MESSAGE("ungated lift " << plain << ", bit-only " << stamp_lift(CLAY_AUTOMASK_CAVITY, nullptr)
                            << ", source-only " << stamp_lift(0, &with) << ", both "
                            << stamp_lift(CLAY_AUTOMASK_CAVITY, &with));
}

TEST_CASE("c abi: the cavity strength is a dial, not a switch") {
    Doc doc;
    Mask cavity(0.05f);
    const float lo[3] = {-2.0f, -2.0f, -2.0f};
    const float hi[3] = {2.0f, 2.0f, 2.0f};
    REQUIRE(clay_mask_fill(cavity, lo, hi, 1.0f) == CLAY_OK);
    const clay_automask_sources with = sources_of(cavity, nullptr, 0);

    const double plain = stamp_lift(0, nullptr);
    // weight *= 1 - cavity * strength, so a cavity of 1 at strength s scales
    // the whole stamp by (1 - s). Asserting the RATIO rather than a magnitude:
    // it is the thing the ABI carries, and it holds whatever the falloff does.
    //
    // STRENGTH ZERO IS NOT ASSERTED HERE. read_mesh_brush read a zero as "the
    // caller declared an older layout, take the engine default", which for this
    // field inverts a slider dragged to off; that is fixed in
    // place-the-automask-lattices, whose own regression test is the brush
    // PRESET round trip rather than a stamp -- the path that could observe it
    // while CAVITY was inert from C. Asserting it here as well would be a
    // second answer about it, and a redder one until that lands.
    CHECK(stamp_lift(CLAY_AUTOMASK_CAVITY, &with, nullptr, 0.25f) ==
          doctest::Approx(plain * 0.75).epsilon(0.001));
    CHECK(stamp_lift(CLAY_AUTOMASK_CAVITY, &with, nullptr, 0.5f) ==
          doctest::Approx(plain * 0.5).epsilon(0.001));
    MESSAGE("strength 0.25 -> " << stamp_lift(CLAY_AUTOMASK_CAVITY, &with, nullptr, 0.25f)
                                << ", 0.50 -> "
                                << stamp_lift(CLAY_AUTOMASK_CAVITY, &with, nullptr, 0.5f)
                                << ", ungated " << plain);
}

TEST_CASE("c abi: a surface-group automask keeps a stamp in the group it started in") {
    Doc doc;
    Groups groups(doc.doc, 0.05f);
    // Two halves of the plane, split at x = 0. The brush lands at the origin,
    // so its footprint straddles the border and half of what it reaches is in
    // the wrong group -- which is the only arrangement that can tell a working
    // gate from one that passes everything.
    const float left_lo[3] = {-2.0f, -1.0f, -2.0f};
    const float left_hi[3] = {0.0f, 1.0f, 2.0f};
    const float right_lo[3] = {0.0f, -1.0f, -2.0f};
    const float right_hi[3] = {2.0f, 1.0f, 2.0f};
    REQUIRE(clay_groups_fill(groups, left_lo, left_hi, 1) == CLAY_OK);
    REQUIRE(clay_groups_fill(groups, right_lo, right_hi, 2) == CLAY_OK);

    const double plain = stamp_lift(0, nullptr);
    const clay_automask_sources in_one = sources_of(nullptr, groups, 1);

    CHECK(stamp_lift(CLAY_AUTOMASK_SURFACE_GROUP, nullptr) == doctest::Approx(plain));
    const double gated = stamp_lift(CLAY_AUTOMASK_SURFACE_GROUP, &in_one);
    CHECK(gated > 0.0);       // the group it started in still moves
    CHECK(gated < plain);     // the other one does not

    // Naming a group nothing carries masks EVERYTHING, which is the honest
    // answer: "stay in group 7" over a surface with no group 7 reaches nothing.
    const clay_automask_sources in_none = sources_of(nullptr, groups, 7);
    CHECK(stamp_lift(CLAY_AUTOMASK_SURFACE_GROUP, &in_none) == doctest::Approx(0.0));

    // THE TWO HALVES PARTITION THE STAMP. Not that they are equal -- they are
    // not, and expecting them to be was wrong: the fill decides membership at
    // the CELL centre, the lattice cell is 0.05 and the grid spacing is 1/12,
    // so the column at x = 0 falls to one side. What must hold is that every
    // vertex the stamp reached is in exactly one group, and that is a sum.
    const clay_automask_sources in_two = sources_of(nullptr, groups, 2);
    const double other = stamp_lift(CLAY_AUTOMASK_SURFACE_GROUP, &in_two);
    CHECK(other > 0.0);
    CHECK(gated + other == doctest::Approx(plain));
    MESSAGE("group 1 " << gated << " + group 2 " << other << " = " << plain);
}

TEST_CASE("c abi: a sculptor's declared frame places the point the lattices are asked about") {
    // THE REGRESSION. Both lattices are world-addressed and a sculptor's
    // vertices are not, so a session that has declared where its mesh sits must
    // have its vertices placed before either lattice is asked. Sampling the
    // unplaced point is invisible on an untransformed layer -- which is every
    // other case in this file -- and wrong the moment one is placed.
    Doc doc;
    Groups groups(doc.doc, 0.05f);
    // Group 1 covers only where the mesh ENDS UP, five units along +X. The mesh
    // itself sits at the origin, where the lattice holds nothing.
    const float lo[3] = {4.0f, -1.0f, -2.0f};
    const float hi[3] = {6.0f, 1.0f, 2.0f};
    REQUIRE(clay_groups_fill(groups, lo, hi, 1) == CLAY_OK);

    clay_mesh_frame frame;
    std::memset(&frame, 0, sizeof frame);
    frame.struct_size = static_cast<std::uint32_t>(sizeof frame);
    frame.position[0] = 5.0f;
    frame.scale = 1.0f;  // all-zero rotation reads as identity

    const double plain = stamp_lift(0, nullptr, &frame);
    // Or the two zeroes below would agree with each other and prove nothing.
    REQUIRE(plain > 0.0);
    const clay_automask_sources in_one = sources_of(nullptr, groups, 1);

    // Placed: every vertex lands in group 1 and the whole stamp survives the
    // gate. Unplaced, the vertices would read group 0 at the origin, none of
    // them would match, and this would be 0.0.
    const double placed = stamp_lift(CLAY_AUTOMASK_SURFACE_GROUP, &in_one, &frame);
    CHECK(placed == doctest::Approx(plain));
    MESSAGE("placed sample " << placed << " of an ungated " << plain);

    // The same sources on a session that declares NO frame reach nothing, which
    // is the other half of the claim: the frame is what moved the sample, not
    // some accident of the lattice's extent.
    CHECK(stamp_lift(CLAY_AUTOMASK_SURFACE_GROUP, &in_one) == doctest::Approx(0.0));
}

TEST_CASE("c abi: the frame may be declared after the sources") {
    // A closure that captured the frame it found would be correct only in the
    // order a host happened to use. Both orders have to land in the same place.
    Doc doc;
    Groups groups(doc.doc, 0.05f);
    const float lo[3] = {4.0f, -1.0f, -2.0f};
    const float hi[3] = {6.0f, 1.0f, 2.0f};
    REQUIRE(clay_groups_fill(groups, lo, hi, 1) == CLAY_OK);

    clay_mesh_frame frame;
    std::memset(&frame, 0, sizeof frame);
    frame.struct_size = static_cast<std::uint32_t>(sizeof frame);
    frame.position[0] = 5.0f;
    frame.scale = 1.0f;

    clay_mesh* m = grid_mesh(24, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
    const clay_automask_sources in_one = sources_of(nullptr, groups, 1);
    REQUIRE(clay_mesh_sculptor_set_automask_sources(s, &in_one) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_set_world_frame(s, &frame) == CLAY_OK);  // AFTER

    clay_mesh_brush_desc d = draw_brush(0.6f, 0.5f, CLAY_AUTOMASK_SURFACE_GROUP);
    d.center[0] = 5.0f;  // world, because this session declares a frame
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(moved > 0);
    CHECK(lift(m) == doctest::Approx(stamp_lift(0, nullptr, &frame)));

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: clearing the sources returns both factors to inert") {
    Doc doc;
    Mask cavity(0.05f);
    const float lo[3] = {-2.0f, -2.0f, -2.0f};
    const float hi[3] = {2.0f, 2.0f, 2.0f};
    REQUIRE(clay_mask_fill(cavity, lo, hi, 1.0f) == CLAY_OK);

    clay_mesh* m = grid_mesh(24, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
    const clay_automask_sources with = sources_of(cavity, nullptr, 0);
    REQUIRE(clay_mesh_sculptor_set_automask_sources(s, &with) == CLAY_OK);
    REQUIRE(clay_mesh_sculptor_set_automask_sources(s, nullptr) == CLAY_OK);

    clay_mesh_brush_desc d = draw_brush(0.6f, 0.5f, CLAY_AUTOMASK_CAVITY);
    std::size_t moved = 0;
    REQUIRE(clay_mesh_sculptor_stamp(s, &d, nullptr, nullptr, &moved) == CLAY_OK);
    CHECK(lift(m) == doctest::Approx(stamp_lift(0, nullptr)));

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a sources descriptor is versioned, and a mask that left its document is not one") {
    Doc doc;
    clay_mesh* m = grid_mesh(8, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);

    clay_automask_sources bad = sources_of(nullptr, nullptr, 0);
    bad.struct_size = 0;
    CHECK(clay_mesh_sculptor_set_automask_sources(s, &bad) == CLAY_ERROR_INVALID_ARGUMENT);
    bad.struct_size = 1u << 20;
    CHECK(clay_mesh_sculptor_set_automask_sources(s, &bad) == CLAY_ERROR_INVALID_ARGUMENT);

    // A BORROWED mask outlives its layer as a handle and not as a mask, and
    // saying so is the difference between a refused call and a stamp that
    // quietly stopped gating.
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc.doc, "body", &layer) == CLAY_OK);
    clay_mask* borrowed = nullptr;
    REQUIRE(clay_document_add_mask(doc.doc, layer, 0.05f, &borrowed) == CLAY_OK);
    const clay_automask_sources with = sources_of(borrowed, nullptr, 0);
    CHECK(clay_mesh_sculptor_set_automask_sources(s, &with) == CLAY_OK);
    REQUIRE(clay_document_remove_mask(doc.doc, layer) == CLAY_OK);
    CHECK(clay_mesh_sculptor_set_automask_sources(s, &with) == CLAY_ERROR_NOT_FOUND);

    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: the adaptive and multiresolution sculptors take the same sources") {
    // The claim the three setters make together is that a crevice is not a fact
    // about where a displacement is stored. What is checked here is that each
    // ACCEPTS the descriptor and that a null one is refused -- the maths of a
    // gate on an adaptive surface is the C++ suite's, and repeating it here
    // would be a second answer about it.
    Doc doc;
    Groups groups(doc.doc, 0.05f);
    const float lo[3] = {-2.0f, -1.0f, -2.0f};
    const float hi[3] = {2.0f, 1.0f, 2.0f};
    REQUIRE(clay_groups_fill(groups, lo, hi, 1) == CLAY_OK);
    const clay_automask_sources in_one = sources_of(nullptr, groups, 1);

    CHECK(clay_dynamic_sculptor_set_automask_sources(nullptr, &in_one) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_multires_sculptor_set_automask_sources(nullptr, &in_one) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    clay_mesh* m = grid_mesh(16, 1.0f);
    clay_dynamic_surface* surface = nullptr;
    clay_dynamic_surface_desc sd;
    std::memset(&sd, 0, sizeof sd);
    sd.struct_size = static_cast<std::uint32_t>(sizeof sd);
    REQUIRE(clay_dynamic_surface_defaults(&sd) == CLAY_OK);
    int32_t build_error = 0;
    REQUIRE(clay_dynamic_surface_from_mesh(m, &sd, &surface, &build_error) == CLAY_OK);
    clay_dynamic_sculptor* ds = nullptr;
    REQUIRE(clay_dynamic_sculptor_create(surface, &ds) == CLAY_OK);
    CHECK(clay_dynamic_sculptor_set_automask_sources(ds, &in_one) == CLAY_OK);
    CHECK(clay_dynamic_sculptor_set_automask_sources(ds, nullptr) == CLAY_OK);
    clay_dynamic_sculptor_destroy(ds);
    clay_dynamic_surface_destroy(surface);
    clay_mesh_destroy(m);
}

TEST_CASE("c abi: a node's colour reads back what its setter wrote") {
    Doc doc;
    REQUIRE(clay_document_enable_undo(doc.doc) == CLAY_OK);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc.doc, "body", &layer) == CLAY_OK);
    clay_item_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<std::uint32_t>(sizeof d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = 1.0f;
    d.op = CLAY_OP_ADD;
    d.color[0] = 0.1f;
    d.color[1] = 0.2f;
    d.color[2] = 0.3f;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc.doc, layer, &d, &node) == CLAY_OK);

    // AN ITEM'S COLOUR IS ITS DESCRIPTOR'S, and this is the reason the reader
    // earns its place rather than a formality: clay_item_desc carries a colour,
    // so a host that zeroed its descriptor placed a BLACK item and had no way
    // to find out. There is no "unset" for this reader to report.
    float rgb[3] = {-1.0f, -1.0f, -1.0f};
    REQUIRE(clay_layer_node_color(doc.doc, layer, node, rgb) == CLAY_OK);
    CHECK(rgb[0] == doctest::Approx(0.1f));
    CHECK(rgb[1] == doctest::Approx(0.2f));
    CHECK(rgb[2] == doctest::Approx(0.3f));

    const float wrote[3] = {0.25f, 0.5f, 0.75f};
    REQUIRE(clay_layer_set_color(doc.doc, layer, node, wrote) == CLAY_OK);
    REQUIRE(clay_layer_node_color(doc.doc, layer, node, rgb) == CLAY_OK);
    CHECK(rgb[0] == doctest::Approx(wrote[0]));
    CHECK(rgb[1] == doctest::Approx(wrote[1]));
    CHECK(rgb[2] == doctest::Approx(wrote[2]));

    // The reason the reader exists: a host had to keep this in a table beside
    // the .clay and keep that table correct across undo and redo on its own.
    int32_t stepped = 0;
    REQUIRE(clay_document_undo(doc.doc, &stepped) == CLAY_OK);
    CHECK(stepped == 1);
    REQUIRE(clay_layer_node_color(doc.doc, layer, node, rgb) == CLAY_OK);
    CHECK(rgb[0] == doctest::Approx(0.1f));
    REQUIRE(clay_document_redo(doc.doc, &stepped) == CLAY_OK);
    CHECK(stepped == 1);
    REQUIRE(clay_layer_node_color(doc.doc, layer, node, rgb) == CLAY_OK);
    CHECK(rgb[0] == doctest::Approx(wrote[0]));

    // A GROUP answers, where the transform reader refuses one: a shell or a
    // replace group paints a seed colour, so the value is a group's to hold.
    // clay_layer_add_group takes no colour, so this is the one node a C host
    // can make that carries the ENGINE's default rather than a descriptor's.
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc.doc, layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD, 0.0f, 0.0f,
                                 &group) == CLAY_OK);
    REQUIRE(clay_layer_node_color(doc.doc, layer, group, rgb) == CLAY_OK);
    CHECK(rgb[0] == doctest::Approx(0.7f));
    CHECK(rgb[1] == doctest::Approx(0.7f));
    CHECK(rgb[2] == doctest::Approx(0.7f));

    const float seed[3] = {1.0f, 0.0f, 0.0f};
    REQUIRE(clay_layer_set_color(doc.doc, layer, group, seed) == CLAY_OK);
    REQUIRE(clay_layer_node_color(doc.doc, layer, group, rgb) == CLAY_OK);
    CHECK(rgb[0] == doctest::Approx(1.0f));
    CHECK(rgb[1] == doctest::Approx(0.0f));

    // The two refusals every reader here makes.
    CHECK(clay_layer_node_color(doc.doc, layer + 99, node, rgb) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_color(doc.doc, layer, node + 99, rgb) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_layer_node_color(nullptr, layer, node, rgb) == CLAY_ERROR_INVALID_ARGUMENT);
}
