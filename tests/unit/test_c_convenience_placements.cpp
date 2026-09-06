// The three convenience placements across the C ABI (add-convenience-transforms,
// c-abi and scene-model specs).
//
// The roadmap row reads like sugar — "snap-to-ground, centre-mass,
// zero-to-origin as single ABI calls" — and the load-bearing case here is the
// FIRST one, because it is the whole reason the calls exist. A host composing
// these out of clay_document_layer_transform / _set_layer_transform cannot get
// a squashed layer right: the single-factor reader REFUSES one and the
// single-factor setter CLEARS the squash. Both halves are asserted below, so
// the defect is pinned from the inside and from the outside.
//
// The rest are the refusal set (one case per row of the table in the change's
// design.md), the arithmetic against a hand-computed box on all three
// representations, one undo step each, and the two things a placement must not
// do: sever an instance, or move a layer it refused.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "clay.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

const float kAxisY[3] = {0.0f, 1.0f, 0.0f};

// A whole placement, read back per axis so a squash survives the trip.
struct Placement {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float axis[3] = {0.0f, 1.0f, 0.0f};
    float angle = 0.0f;
    float scale[3] = {1.0f, 1.0f, 1.0f};

    // BIT equality on everything but the position, which is the only component
    // these calls are allowed to write. Not a tolerance: the claim is that the
    // rotation and both scales are carried through untouched, and a tolerance
    // would pass a round trip through axis-angle that is not.
    bool same_but_position(const Placement& o) const {
        return std::memcmp(axis, o.axis, sizeof axis) == 0 &&
               std::memcmp(&angle, &o.angle, sizeof angle) == 0 &&
               std::memcmp(scale, o.scale, sizeof scale) == 0;
    }
    bool operator==(const Placement& o) const {
        return std::memcmp(position, o.position, sizeof position) == 0 && same_but_position(o);
    }
};

Placement placement_of(const clay_document* doc, clay_layer_id layer) {
    Placement p;
    REQUIRE(clay_document_layer_transform_nonuniform(doc, layer, p.position, p.axis, &p.angle,
                                                     p.scale) == CLAY_OK);
    return p;
}

struct Box {
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
    std::int32_t has = 0;
};

Box bounds_of(const clay_document* doc, clay_layer_id layer) {
    Box b;
    REQUIRE(clay_layer_bounds(doc, layer, b.min, b.max, &b.has) == CLAY_OK);
    return b;
}

clay_layer_id sphere_layer(clay_document* doc, const char* name, float r) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
    return layer;
}

// A tetrahedron, the smallest thing clay_document_add_mesh_layer accepts.
clay_layer_id mesh_layer(clay_document* doc, const char* name) {
    const float positions[12] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f};
    const std::uint32_t indices[12] = {0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3};
    clay_mesh* tetra = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions, 4, indices, 12, &tetra) == CLAY_OK);
    clay_mesh_layer_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.name = name;
    clay_layer_id layer = 0;
    REQUIRE(clay_document_add_mesh_layer(doc, tetra, &desc, &layer, nullptr) == CLAY_OK);
    clay_mesh_destroy(tetra);
    return layer;
}

clay_layer_id voxel_layer(clay_document* doc, const char* name) {
    clay_layer_id layer = 0;
    clay_voxel_grid* grid = nullptr;
    REQUIRE(clay_document_add_voxel_layer(doc, name, 0.25f, &layer, &grid) == CLAY_OK);
    REQUIRE(grid != nullptr);
    for (std::int32_t x = 0; x < 3; ++x)
        for (std::int32_t y = 0; y < 2; ++y) {
            const std::int32_t cell[3] = {x, y, 0};
            REQUIRE(clay_voxel_set(grid, cell, 1) == CLAY_OK);
        }
    return layer;
}

// The squash the whole change is about, plus a rotation, so nothing can pass by
// leaving a placement alone.
void squash_and_turn(clay_document* doc, clay_layer_id layer, const float position[3]) {
    const float axis[3] = {0.0f, 0.0f, 1.0f};
    const float scale[3] = {1.5f, 0.5f, 2.25f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(doc, layer, position, axis, 0.7f,
                                                         scale) == CLAY_OK);
}

clay_layer_info info_of(const clay_document* doc, clay_layer_id layer) {
    clay_layer_info info;
    std::memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(clay_document_layer_info(doc, layer, &info) == CLAY_OK);
    return info;
}

}  // namespace

TEST_CASE("a convenience placement keeps a squashed layer squashed") {
    // The defect the change exists for. A host writing this itself reaches for
    // clay_document_layer_transform first, and on this layer it cannot even
    // read — so it has nothing to write back.
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {3.0f, 5.0f, -2.0f};
    squash_and_turn(d.doc, layer, where);

    float uniform = 0.0f;
    CHECK(clay_document_layer_transform(d.doc, layer, nullptr, nullptr, nullptr, &uniform) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // And the setter a host would reach for next CLEARS the squash, which is
    // the silent half: it succeeds, and the model changes shape.
    {
        Doc other;
        const clay_layer_id l2 = sphere_layer(other.doc, "Bola", 1.0f);
        squash_and_turn(other.doc, l2, where);
        const Box before = bounds_of(other.doc, l2);
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        REQUIRE(clay_document_set_layer_transform(other.doc, l2, origin, kAxisY, 0.0f, 1.0f) ==
                CLAY_OK);
        const Box after = bounds_of(other.doc, l2);
        CHECK((after.max[0] - after.min[0]) != doctest::Approx(before.max[0] - before.min[0]));
    }

    // All three, on the same layer in turn: the placement's position moves and
    // nothing else does, bit for bit.
    const Placement start = placement_of(d.doc, layer);
    REQUIRE(clay_layer_snap_to_ground(d.doc, layer, -1.0f) == CLAY_OK);
    CHECK(placement_of(d.doc, layer).same_but_position(start));
    REQUIRE(clay_layer_centre_bounds(d.doc, layer) == CLAY_OK);
    CHECK(placement_of(d.doc, layer).same_but_position(start));
    REQUIRE(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
    const Placement end = placement_of(d.doc, layer);
    CHECK(end.same_but_position(start));
    CHECK(end.position[0] == 0.0f);
    CHECK(end.position[1] == 0.0f);
    CHECK(end.position[2] == 0.0f);

    // The box the layer had is the box it still has, moved: the squash is not
    // just reported back, it is still in the shape.
    const Box box = bounds_of(d.doc, layer);
    CHECK(box.has == 1);
    CHECK((box.max[0] - box.min[0]) == doctest::Approx(3.0f).epsilon(0.02));
    CHECK((box.max[2] - box.min[2]) == doctest::Approx(4.5f).epsilon(0.02));
}

TEST_CASE("each rule against a hand-computed box") {
    Doc d;
    // A unit sphere at (2, 7, -3): the box is that point +/- 1 exactly.
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {2.0f, 7.0f, -3.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, where, kAxisY, 0.0f, 1.0f) == CLAY_OK);
    {
        const Box b = bounds_of(d.doc, layer);
        CHECK(b.min[1] == doctest::Approx(6.0f));
    }

    SUBCASE("the low face lands on the plane and the other axes do not move") {
        REQUIRE(clay_layer_snap_to_ground(d.doc, layer, -4.0f) == CLAY_OK);
        const Box b = bounds_of(d.doc, layer);
        CHECK(b.min[1] == doctest::Approx(-4.0f));
        CHECK(b.min[0] == doctest::Approx(1.0f));
        CHECK(b.max[2] == doctest::Approx(-2.0f));
    }

    SUBCASE("the box centre lands on the origin in all three axes") {
        REQUIRE(clay_layer_centre_bounds(d.doc, layer) == CLAY_OK);
        const Box b = bounds_of(d.doc, layer);
        for (int i = 0; i < 3; ++i) CHECK((b.min[i] + b.max[i]) * 0.5f == doctest::Approx(0.0f));
    }

    SUBCASE("zeroing leaves the box where the layer's own origin puts it") {
        // The sphere is authored at the layer origin here, so the two rules
        // coincide — which is exactly the case the change's design.md says they
        // coincide in, and the next subcase is the case they do not.
        REQUIRE(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
        const Placement p = placement_of(d.doc, layer);
        CHECK(p.position[0] == 0.0f);
        CHECK(p.position[1] == 0.0f);
        CHECK(p.position[2] == 0.0f);
        const Box b = bounds_of(d.doc, layer);
        CHECK(b.min[1] == doctest::Approx(-1.0f));
    }

    SUBCASE("off-pivot content: zeroing and centring differ") {
        // A second sphere placed away from the layer origin, so the content's
        // box is no longer centred on the pivot.
        const float r = 1.0f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it != nullptr);
        const float offset[3] = {6.0f, 0.0f, 0.0f};
        REQUIRE(clay_item_set_position(it, offset) == CLAY_OK);
        REQUIRE(clay_layer_add_item(d.doc, layer, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);

        Doc other;
        const clay_layer_id twin = sphere_layer(other.doc, "Bola", 1.0f);
        REQUIRE(clay_document_set_layer_transform(other.doc, twin, where, kAxisY, 0.0f, 1.0f) ==
                CLAY_OK);
        clay_item* it2 = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        REQUIRE(it2 != nullptr);
        REQUIRE(clay_item_set_position(it2, offset) == CLAY_OK);
        REQUIRE(clay_layer_add_item(other.doc, twin, it2, nullptr) == CLAY_OK);
        clay_item_destroy(it2);

        REQUIRE(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
        REQUIRE(clay_layer_centre_bounds(other.doc, twin) == CLAY_OK);
        CHECK(placement_of(d.doc, layer).position[0] == 0.0f);
        CHECK(placement_of(other.doc, twin).position[0] == doctest::Approx(-3.0f));
    }
}

TEST_CASE("all three representations take one rule") {
    // `position` outermost, held for the arm that goes through
    // pick::layer_bounds AND for the two that compose scene::layer_matrix on
    // the side tables. Each carries a rotation and a per-axis scale, so a
    // composition that put the delta anywhere but outermost would land the box
    // somewhere else.
    Doc d;
    const clay_layer_id sdf = sphere_layer(d.doc, "Sdf", 1.0f);
    const clay_layer_id vox = voxel_layer(d.doc, "Voxel");
    const clay_layer_id msh = mesh_layer(d.doc, "Malha");
    const float where[3] = {4.0f, 9.0f, 1.0f};
    for (clay_layer_id layer : {sdf, vox, msh}) {
        squash_and_turn(d.doc, layer, where);
        const Box before = bounds_of(d.doc, layer);
        REQUIRE(before.has == 1);
        REQUIRE(clay_layer_snap_to_ground(d.doc, layer, 2.0f) == CLAY_OK);
        const Box after = bounds_of(d.doc, layer);
        CHECK(after.min[1] == doctest::Approx(2.0f));
        // Moved by the delta and by nothing else: the extents are the extents.
        for (int i = 0; i < 3; ++i)
            CHECK((after.max[i] - after.min[i]) ==
                  doctest::Approx(before.max[i] - before.min[i]).epsilon(1e-4));
        CHECK(after.min[0] == doctest::Approx(before.min[0]));
        CHECK(after.min[2] == doctest::Approx(before.min[2]));
    }
}

TEST_CASE("one undo step each, and redo restores the computed placement") {
    Doc d;
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {3.0f, 5.0f, -2.0f};
    squash_and_turn(d.doc, layer, where);
    const Placement before = placement_of(d.doc, layer);

    for (int rule = 0; rule < 3; ++rule) {
        if (rule == 0) REQUIRE(clay_layer_snap_to_ground(d.doc, layer, -3.0f) == CLAY_OK);
        if (rule == 1) REQUIRE(clay_layer_centre_bounds(d.doc, layer) == CLAY_OK);
        if (rule == 2) REQUIRE(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
        const Placement moved = placement_of(d.doc, layer);

        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
        CHECK(undone == 1);
        // ONE step, and the inverse is the whole previous placement — rotation
        // and both scales included, which is why this is one command and not
        // two.
        CHECK(placement_of(d.doc, layer) == before);

        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(d.doc, &redone) == CLAY_OK);
        CHECK(redone == 1);
        CHECK(placement_of(d.doc, layer) == moved);

        REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
        CHECK(placement_of(d.doc, layer) == before);
    }
}

TEST_CASE("a press that moves nothing is still one step") {
    // The alternative — skip the command when the placement did not change —
    // was rejected because the geometric no-op is not exactly detectable in
    // float. An undo depth that depended on the geometry would leave a host
    // unable to predict what its own button cost.
    Doc d;
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, origin, kAxisY, 0.0f, 1.0f) == CLAY_OK);

    REQUIRE(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
    REQUIRE(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
    REQUIRE(clay_document_undo(d.doc, &undone) == CLAY_OK);
    CHECK(undone == 1);
}

TEST_CASE("applying a snap twice is applying it once, to within a rounding") {
    // Stated to an ULP and not to the bit: the second press recomputes the box
    // from an already-moved layer, and f + (p + (g - (f + p))) is not g in
    // float. Asserting bit equality here would be flaky at large coordinates,
    // which is the point of the tolerance rather than an excuse for it.
    //
    // WHICH MAGNITUDE, and this is the half building it corrected. The natural
    // reading -- one ulp at the coordinate the layer ENDS UP at -- is wrong and
    // fails here by sixty-fold: a snap from far away lands the layer near the
    // plane, so the result's magnitude is about 1 while the arithmetic that
    // produced it ran at 2048. The error is set by the box the FIRST press
    // read, and this asserts against that.
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {1024.0f, 2048.0f, -512.0f};
    squash_and_turn(d.doc, layer, where);

    const Box read_by_the_first_press = bounds_of(d.doc, layer);
    float magnitude = 1.0f;
    for (int i = 0; i < 3; ++i) {
        magnitude = std::fmax(magnitude, std::fabs(read_by_the_first_press.min[i]));
        magnitude = std::fmax(magnitude, std::fabs(read_by_the_first_press.max[i]));
    }
    const float ulp = std::nextafter(magnitude, std::numeric_limits<float>::infinity()) - magnitude;

    REQUIRE(clay_layer_snap_to_ground(d.doc, layer, 0.25f) == CLAY_OK);
    const Placement once = placement_of(d.doc, layer);
    REQUIRE(clay_layer_snap_to_ground(d.doc, layer, 0.25f) == CLAY_OK);
    const Placement twice = placement_of(d.doc, layer);

    CHECK(std::fabs(twice.position[1] - once.position[1]) <= ulp);
    // And it does not accumulate: a third press moves no further than the
    // second did, which is what makes the tolerance a bound rather than a step.
    REQUIRE(clay_layer_snap_to_ground(d.doc, layer, 0.25f) == CLAY_OK);
    const Placement thrice = placement_of(d.doc, layer);
    CHECK(std::fabs(thrice.position[1] - twice.position[1]) <= ulp);
    // X and Z are not touched by this rule at all, so they are bit-stable.
    CHECK(twice.position[0] == once.position[0]);
    CHECK(twice.position[2] == once.position[2]);
}

TEST_CASE("the refusals, one per row, each leaving the document unchanged") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {3.0f, 5.0f, -2.0f};
    squash_and_turn(d.doc, layer, where);
    const Placement before = placement_of(d.doc, layer);

    SUBCASE("no layer carries the id: NOT_FOUND, and only that") {
        // NOT_FOUND has to keep meaning "no layer carries this id" alone, or a
        // host cannot tell a stale id from a layer that cannot take the
        // operation.
        const clay_layer_id ghost_id = layer + 4242;
        CHECK(clay_layer_snap_to_ground(d.doc, ghost_id, 0.0f) == CLAY_ERROR_NOT_FOUND);
        CHECK(clay_layer_centre_bounds(d.doc, ghost_id) == CLAY_ERROR_NOT_FOUND);
        CHECK(clay_layer_zero_to_origin(d.doc, ghost_id) == CLAY_ERROR_NOT_FOUND);
        CHECK(placement_of(d.doc, layer) == before);
    }

    SUBCASE("a null document is a malformed call, not a missing layer") {
        CHECK(clay_layer_snap_to_ground(nullptr, layer, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(nullptr, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_zero_to_origin(nullptr, layer) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("a locked layer refuses before it costs a bounds walk") {
        REQUIRE(clay_document_set_layer_protection(d.doc, layer, 0, 1) == CLAY_OK);
        CHECK(clay_layer_snap_to_ground(d.doc, layer, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_zero_to_origin(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, layer) == before);
    }

    SUBCASE("a ghosted layer refuses the same way") {
        REQUIRE(clay_document_set_layer_protection(d.doc, layer, 1, 0) == CLAY_OK);
        CHECK(clay_layer_snap_to_ground(d.doc, layer, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_zero_to_origin(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, layer) == before);
    }

    SUBCASE("a ground height that is not finite") {
        CHECK(clay_layer_snap_to_ground(d.doc, layer,
                                        std::numeric_limits<float>::quiet_NaN()) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_snap_to_ground(d.doc, layer, std::numeric_limits<float>::infinity()) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, layer) == before);
    }

    SUBCASE("a radial layer: the two that read a box refuse, the one that does not accepts") {
        // clay_layer_bounds carries the MIRROR copies and not the radial ones,
        // so a placement computed from that box drops the ORIGINAL onto the
        // plane with its copies already through it.
        REQUIRE(clay_set_layer_radial(d.doc, layer, 1, 6, 0.0f) == CLAY_OK);
        CHECK(clay_layer_snap_to_ground(d.doc, layer, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, layer) == before);
        CHECK(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);

        // ... and a count of 1 is the mode turned off, so it is not refused.
        REQUIRE(clay_set_layer_radial(d.doc, layer, 1, 1, 0.0f) == CLAY_OK);
        CHECK(clay_layer_centre_bounds(d.doc, layer) == CLAY_OK);
    }

    SUBCASE("an empty layer: refused by the two that read a box, accepted by the third") {
        clay_layer_id vazia = 0;
        REQUIRE(clay_add_sdf_layer(d.doc, "Vazia", &vazia) == CLAY_OK);
        REQUIRE(clay_document_set_layer_transform(d.doc, vazia, where, kAxisY, 0.0f, 1.0f) ==
                CLAY_OK);
        const Placement empty_before = placement_of(d.doc, vazia);
        CHECK(clay_layer_snap_to_ground(d.doc, vazia, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(d.doc, vazia) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, vazia) == empty_before);
        CHECK(clay_layer_zero_to_origin(d.doc, vazia) == CLAY_OK);
        CHECK(placement_of(d.doc, vazia).position[1] == 0.0f);
    }

    SUBCASE("an UNBOUNDED layer, which the design's table did not have a row for") {
        // A plane answers Aabb::infinite(), whose faces are +/-FLT_MAX rather
        // than an infinity — so nothing here would have thrown, and the layer
        // would have been placed at a coordinate that compiles a tape of NaNs.
        clay_layer_id chao = 0;
        REQUIRE(clay_add_sdf_layer(d.doc, "Chao", &chao) == CLAY_OK);
        const float plane[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        clay_item* it = clay_item_create(CLAY_PRIM_PLANE, plane, 4);
        REQUIRE(it != nullptr);
        REQUIRE(clay_layer_add_item(d.doc, chao, it, nullptr) == CLAY_OK);
        clay_item_destroy(it);
        const Placement plane_before = placement_of(d.doc, chao);
        CHECK(clay_layer_snap_to_ground(d.doc, chao, -1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(d.doc, chao) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, chao) == plane_before);
        CHECK(clay_layer_zero_to_origin(d.doc, chao) == CLAY_OK);
    }

    SUBCASE("a placement gesture refuses these as it refuses every other edit") {
        clay_placement_tx* tx = clay_layer_placement_begin(d.doc, layer);
        REQUIRE(tx != nullptr);
        CHECK(clay_layer_snap_to_ground(d.doc, layer, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_centre_bounds(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_zero_to_origin(d.doc, layer) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(placement_of(d.doc, layer) == before);
        clay_layer_placement_destroy(tx);
        CHECK(clay_layer_zero_to_origin(d.doc, layer) == CLAY_OK);
    }
}

TEST_CASE("a flat layer is placed, not refused") {
    // Nothing here divides by an extent, and a planar layer is exactly the
    // layer a "drop it on the floor" button is most often pressed on.
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "Chapa", &layer) == CLAY_OK);
    const float box[3] = {1.0f, 0.0f, 1.0f};
    clay_item* it = clay_item_create(CLAY_PRIM_BOX, box, 3);
    REQUIRE(it != nullptr);
    REQUIRE(clay_layer_add_item(d.doc, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);

    const Box b0 = bounds_of(d.doc, layer);
    REQUIRE(b0.has == 1);
    REQUIRE(b0.min[1] == doctest::Approx(b0.max[1]));

    REQUIRE(clay_layer_snap_to_ground(d.doc, layer, 3.0f) == CLAY_OK);
    const Box b1 = bounds_of(d.doc, layer);
    CHECK(b1.min[1] == doctest::Approx(3.0f));
    CHECK(b1.max[1] == doctest::Approx(3.0f));
    REQUIRE(clay_layer_centre_bounds(d.doc, layer) == CLAY_OK);
    const Box b2 = bounds_of(d.doc, layer);
    CHECK(b2.min[1] == doctest::Approx(0.0f));
}

TEST_CASE("a hidden layer is placed") {
    // The box answers from CONTENT rather than from the visibility flag, and a
    // caller naming a layer says more than the flag does.
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {2.0f, 7.0f, -3.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, where, kAxisY, 0.0f, 1.0f) == CLAY_OK);
    REQUIRE(clay_document_set_layer_visible(d.doc, layer, 0) == CLAY_OK);

    REQUIRE(clay_layer_centre_bounds(d.doc, layer) == CLAY_OK);
    const Box b = bounds_of(d.doc, layer);
    for (int i = 0; i < 3; ++i) CHECK((b.min[i] + b.max[i]) * 0.5f == doctest::Approx(0.0f));
    CHECK(info_of(d.doc, layer).visible == 0);
}

TEST_CASE("an instance is placed and never severed") {
    Doc d;
    const clay_layer_id source = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {2.0f, 7.0f, -3.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, source, where, kAxisY, 0.0f, 1.0f) == CLAY_OK);
    clay_layer_id copy = 0;
    REQUIRE(clay_document_instance_layer(d.doc, source, "Bola 2", &copy) == CLAY_OK);
    const float elsewhere[3] = {-8.0f, 1.0f, 4.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, copy, elsewhere, kAxisY, 0.0f, 1.0f) ==
            CLAY_OK);
    REQUIRE(info_of(d.doc, source).share_count == 2);

    const Placement other_before = placement_of(d.doc, copy);
    const Box other_box_before = bounds_of(d.doc, copy);
    REQUIRE(clay_layer_snap_to_ground(d.doc, source, 0.0f) == CLAY_OK);

    // The other instance did not move, did not change shape, and the sharing is
    // reported as it was. Placing one instance is the gesture instancing exists
    // for; severing here would be the worst possible way to learn the link was
    // fragile.
    CHECK(placement_of(d.doc, copy) == other_before);
    CHECK(bounds_of(d.doc, copy).min[1] == doctest::Approx(other_box_before.min[1]));
    CHECK(info_of(d.doc, source).share_count == 2);
    CHECK(info_of(d.doc, copy).share_count == 2);
    CHECK(info_of(d.doc, copy).content_source == source);

    // And both can be placed independently: the bound each reads is the shared
    // content under ITS OWN placement.
    REQUIRE(clay_layer_snap_to_ground(d.doc, copy, 0.0f) == CLAY_OK);
    CHECK(bounds_of(d.doc, source).min[1] == doctest::Approx(0.0f));
    CHECK(bounds_of(d.doc, copy).min[1] == doctest::Approx(0.0f));
    CHECK(info_of(d.doc, source).share_count == 2);
}

TEST_CASE("a computed placement classifies as RIGID") {
    // The 0.82.0 guarantee applies verbatim, which is what lets a host
    // transform its drawn mesh instead of refilling — and it is also how a host
    // recovers the delta these calls deliberately do not return.
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "Bola", 1.0f);
    const float where[3] = {3.0f, 5.0f, -2.0f};
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, where, kAxisY, 0.0f, 1.0f) == CLAY_OK);

    const Placement before = placement_of(d.doc, layer);
    REQUIRE(clay_layer_snap_to_ground(d.doc, layer, -1.0f) == CLAY_OK);
    const Placement after = placement_of(d.doc, layer);

    clay_placement_report report;
    std::memset(&report, 0, sizeof report);
    report.struct_size = sizeof report;
    REQUIRE(clay_layer_placement_report(d.doc, layer, before.position, before.axis, before.angle,
                                        1.0f, nullptr, &report) == CLAY_OK);
    CHECK(report.kind == CLAY_PLACEMENT_RIGID);
    // The subtraction the header tells a host to make.
    CHECK(after.position[1] - before.position[1] == doctest::Approx(-5.0f));
}
