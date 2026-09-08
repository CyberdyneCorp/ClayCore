// Issue #506: the mask a brush consults was sampled at the UNPLACED point on
// every sculptable surface except a mesh layer.
//
// A layer's vertices are LAYER-LOCAL and its `xform` places them; a
// voxel::MaskField is WORLD-addressed. clay_mesh_sculptor has carried a frame
// between the two since define-carried-mesh-transform-semantics. The multires
// sculptor, the dynamic sculptor and the sculpt-layer stroke carried none, so
// on a placed layer they gated the wrong region -- and it presents as "the mask
// didn't take", which is why it went unfiled.
//
// WHY A PAINTED BOX AND NOT A CAVITY FIELD. The construction has to be binary,
// not threshold-dependent. voxel::MaskField::sample is get(cell_at(world_p)) --
// a nearest-cell lookup with no derivative -- so clay_mask_fill over an
// explicit box makes "was the gate asked here or there" a question with two
// answers and no middle. A cavity estimator would instead make the test's power
// depend on a crevice being deep enough that the placed and unplaced samples
// fall on opposite sides of a threshold, which is a sensitivity to bound rather
// than a question to settle.
//
// THE MARGIN. Non-overlap in SPACE is not the property needed; cells are
// half-open [x, x+1) * cell_size, so a point one hair outside a filled box can
// still quantise into the boundary cell and read 1. The translation therefore
// clears the box extent PLUS ONE MASK CELL, asserted as a relation below rather
// than baked into numbers that happen to satisfy it.
//
// THE VACUITY GUARD. Every case here asserts that the two candidate sample
// points really do read differently on the mask before it concludes anything
// from which way a stamp went. A run where both fall inside the box, or both
// outside, passes for a reason unrelated to the frame -- and `moved_vertices`
// is documented to mean "reached nothing, fully masked, or no displacement", so
// three ordinary outcomes hide behind the number this test could have used.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

// The layer sits this far from the origin. The mask box is 1.5 in half-extent
// and the mask cell is 0.05, so 10 clears extent + cell by a wide margin --
// checked rather than asserted by eye in `the_two_points_read_differently`.
constexpr float kPlacedX = 10.0f;
constexpr float kBoxHalf = 1.5f;
constexpr float kMaskCell = 0.05f;

void plane(int n, float half, std::vector<float>* positions, std::vector<std::uint32_t>* indices) {
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            positions->push_back(-half + step * static_cast<float>(x));
            positions->push_back(0.0f);
            positions->push_back(-half + step * static_cast<float>(z));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a = static_cast<std::uint32_t>(z) * stride +
                                    static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            indices->insert(indices->end(), {a, b, c, a, c, d});
        }
}

clay_mesh* plane_mesh(int n, float half) {
    std::vector<float> p;
    std::vector<std::uint32_t> idx;
    plane(n, half, &p, &idx);
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(p.data(), p.size() / 3, idx.data(), idx.size(), &m) ==
            CLAY_OK);
    return m;
}

struct Mask {
    clay_mask* m = nullptr;
    explicit Mask(float cell) : m(clay_mask_create(cell)) { REQUIRE(m != nullptr); }
    ~Mask() { clay_mask_destroy(m); }
    Mask(const Mask&) = delete;
    Mask& operator=(const Mask&) = delete;
    operator clay_mask*() const { return m; }
};

// A box of full mask centred on `cx`. A sample is scaled by (1 - mask), so this
// is the region a stamp cannot move at all.
void fill_box_at(clay_mask* m, float cx) {
    const float lo[3] = {cx - kBoxHalf, -kBoxHalf, -kBoxHalf};
    const float hi[3] = {cx + kBoxHalf, kBoxHalf, kBoxHalf};
    REQUIRE(clay_mask_fill(m, lo, hi, 1.0f) == CLAY_OK);
}

clay_mesh_frame translation(float x) {
    clay_mesh_frame f{};
    f.struct_size = sizeof(f);
    f.position[0] = x;
    f.rotation[3] = 1.0f;
    f.scale = 1.0f;
    return f;
}

clay_mesh_brush_desc draw_at(float cx, float radius, float strength) {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.center[0] = cx;
    d.radius = radius;
    d.strength = strength;
    return d;
}

// How far a level's vertices left the y = 0 plane, summed. A Draw on a flat
// cage displaces along +Y only, so this is the whole of what a stamp did and a
// mask can only reduce it. Positions, NOT `moved_vertices`: the count cannot
// distinguish "fully masked" from "reached nothing".
double lift_of_level(clay_multires* surface, std::uint32_t level) {
    clay_mesh* m = nullptr;
    REQUIRE(clay_multires_copy_level_mesh(surface, level, &m) == CLAY_OK);
    const float* p = clay_mesh_positions(m);
    const std::size_t n = clay_mesh_vertex_count(m);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += std::fabs(static_cast<double>(p[i * 3 + 1]));
    clay_mesh_destroy(m);
    return sum;
}

struct Multires {
    clay_mesh* mesh = nullptr;
    clay_multires* surface = nullptr;
    clay_multires_sculptor* sculptor = nullptr;

    Multires() {
        mesh = plane_mesh(4, 1.0f);
        clay_multires_desc desc{};
        desc.struct_size = sizeof(desc);
        REQUIRE(clay_multires_defaults(&desc) == CLAY_OK);
        std::int32_t err = -1;
        REQUIRE(clay_multires_from_mesh(mesh, &desc, &surface, &err) == CLAY_OK);
        REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
        REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
        REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);
        REQUIRE(clay_multires_set_sculpt_level(surface, 2) == CLAY_OK);
    }
    ~Multires() {
        clay_multires_sculptor_destroy(sculptor);
        clay_multires_destroy(surface);
        clay_mesh_destroy(mesh);
    }
    Multires(const Multires&) = delete;
    Multires& operator=(const Multires&) = delete;
};

// One stamp on a fresh hierarchy, with an optional declared frame and an
// optional mask. Returns the lift, so a caller compares displacements rather
// than counts.
double stamp_lift(const clay_mesh_frame* frame, clay_mask* mask, float brush_x) {
    Multires f;
    if (frame) REQUIRE(clay_multires_sculptor_set_world_frame(f.sculptor, frame) == CLAY_OK);
    clay_mesh_brush_desc d = draw_at(brush_x, 0.6f, 0.5f);
    REQUIRE(clay_multires_sculptor_stamp(f.sculptor, &d, mask, nullptr) == CLAY_OK);
    return lift_of_level(f.surface, 2);
}

}  // namespace

TEST_CASE("place every surface: the two candidate points read differently on the mask") {
    // THE PRECONDITION EVERY OTHER CASE RESTS ON. The layer-local point is the
    // origin and the placed point is kPlacedX; a box at one must not be a box
    // at the other, IN CELLS and not merely in space.
    //
    // Half-open cells mean the separation has to clear the box extent plus one
    // cell. Asserted as the relation so it survives someone changing the box.
    REQUIRE(kPlacedX > 2.0f * kBoxHalf + kMaskCell);

    Mask at_local(kMaskCell);
    fill_box_at(at_local, 0.0f);
    Mask at_placed(kMaskCell);
    fill_box_at(at_placed, kPlacedX);

    const float local_p[3] = {0.0f, 0.0f, 0.0f};
    const float placed_p[3] = {kPlacedX, 0.0f, 0.0f};
    float v = -1.0f;

    // Each mask reads 1 at its own point and 0 at the other's: the gate cannot
    // be asked "here or there" and give the same answer either way.
    REQUIRE(clay_mask_sample(at_local, local_p, &v) == CLAY_OK);
    CHECK(v == doctest::Approx(1.0f));
    REQUIRE(clay_mask_sample(at_local, placed_p, &v) == CLAY_OK);
    CHECK(v == doctest::Approx(0.0f));
    REQUIRE(clay_mask_sample(at_placed, placed_p, &v) == CLAY_OK);
    CHECK(v == doctest::Approx(1.0f));
    REQUIRE(clay_mask_sample(at_placed, local_p, &v) == CLAY_OK);
    CHECK(v == doctest::Approx(0.0f));
}

TEST_CASE("place every surface: a declared frame puts the multires mask on the placed point") {
    const clay_mesh_frame f = translation(kPlacedX);

    // The unmasked stamp, in the same world the frame declares. A session that
    // declares a frame speaks WORLD in every call, the brush centre included.
    const double plain = stamp_lift(&f, nullptr, kPlacedX);
    REQUIRE(plain > 0.0);

    Mask at_placed(kMaskCell);
    fill_box_at(at_placed, kPlacedX);
    Mask at_local(kMaskCell);
    fill_box_at(at_local, 0.0f);

    // THE BUG, INVERTED. Before the fix the gate was asked at the layer-local
    // point, so the box at the ORIGIN was the one that bit and the box around
    // the placed surface did nothing. Both halves are asserted, because either
    // alone is satisfied by a mask that is simply never consulted.
    CHECK(stamp_lift(&f, at_placed, kPlacedX) < plain * 0.01);
    CHECK(stamp_lift(&f, at_local, kPlacedX) == doctest::Approx(plain));
}

TEST_CASE("place every surface: no declared frame is the identity, exactly as before") {
    // THE NEGATIVE CONTROL, and the half that says the fix is a placement
    // rather than a change of behaviour: with no frame the surface is its own
    // world, so the box at the origin is the one that gates and the answer is
    // the one every existing host already gets.
    const double plain = stamp_lift(nullptr, nullptr, 0.0f);
    REQUIRE(plain > 0.0);

    Mask at_local(kMaskCell);
    fill_box_at(at_local, 0.0f);
    Mask at_placed(kMaskCell);
    fill_box_at(at_placed, kPlacedX);

    CHECK(stamp_lift(nullptr, at_local, 0.0f) < plain * 0.01);
    CHECK(stamp_lift(nullptr, at_placed, 0.0f) == doctest::Approx(plain));
}

TEST_CASE("place every surface: a frame declared on a handle reads back") {
    Multires f;
    std::int32_t declared = -1;
    // Before anything is declared, and this is the query that would otherwise
    // hide its own state: "did my _set_world_frame take?" has to be answerable
    // without stamping and inspecting the result.
    REQUIRE(clay_multires_sculptor_world_frame(f.sculptor, nullptr, &declared) == CLAY_OK);
    CHECK(declared == 0);

    const clay_mesh_frame in = translation(kPlacedX);
    REQUIRE(clay_multires_sculptor_set_world_frame(f.sculptor, &in) == CLAY_OK);

    clay_mesh_frame out{};
    out.struct_size = sizeof(out);
    REQUIRE(clay_multires_sculptor_world_frame(f.sculptor, &out, &declared) == CLAY_OK);
    CHECK(declared == 1);
    CHECK(out.position[0] == doctest::Approx(kPlacedX));
    CHECK(out.scale == doctest::Approx(1.0f));

    // NULL clears, which is how a session returns to the identity it had.
    REQUIRE(clay_multires_sculptor_set_world_frame(f.sculptor, nullptr) == CLAY_OK);
    REQUIRE(clay_multires_sculptor_world_frame(f.sculptor, nullptr, &declared) == CLAY_OK);
    CHECK(declared == 0);
}

TEST_CASE("place every surface: a per-call frame beside a declared one is refused") {
    Multires f;
    const clay_mesh_frame frame = translation(kPlacedX);
    REQUIRE(clay_multires_sculptor_set_world_frame(f.sculptor, &frame) == CLAY_OK);

    const float samples[10] = {kPlacedX, 0.0f, 0.0f, 1.0f, 0.0f,
                               kPlacedX, 0.0f, 0.5f, 1.0f, 0.0f};
    clay_stroke_preset preset{};
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_stroke_preset_defaults(&preset) == CLAY_OK);
    preset.radius = 0.6f;
    clay_mesh_brush_desc d = draw_at(kPlacedX, 0.6f, 0.5f);
    std::size_t applied = 0;

    // REFUSED, not resolved by precedence: a host passing both means one of the
    // two is what it believes, and picking silently would make the other a
    // wrong belief nothing corrects.
    CHECK(clay_multires_sculptor_apply_stroke(f.sculptor, samples, 2, &preset, &d, nullptr, &frame,
                                              0, &applied, nullptr) != CLAY_OK);
    // And the same call with NULL for the per-call frame is accepted.
    CHECK(clay_multires_sculptor_apply_stroke(f.sculptor, samples, 2, &preset, &d, nullptr, nullptr,
                                              0, &applied, nullptr) == CLAY_OK);
}

TEST_CASE("place every surface: a standalone hierarchy has no layer transform to adopt") {
    Multires f;
    // NOT_FOUND rather than a silent identity: this hierarchy was built by
    // clay_multires_from_mesh and belongs to no layer, so there is no transform
    // to read and saying so is the answer.
    CHECK(clay_multires_sculptor_use_layer_transform(f.sculptor) == CLAY_ERROR_NOT_FOUND);
    std::int32_t declared = -1;
    REQUIRE(clay_multires_sculptor_world_frame(f.sculptor, nullptr, &declared) == CLAY_OK);
    CHECK(declared == 0);
}

namespace {

// One sculpt-layer stroke stamp, mask placement isolated the same way. FIVE
// ENTRY POINTS SHARE ONE HELPER -- _stamp, _stamp_detail, _smooth, _erase and
// _restore all read their brush and mask through read_layer_stroke_stamp -- so
// this exercises the site rather than the verb, and the verb is the cheapest of
// the five to assert a displacement for.
double layer_stroke_lift(const clay_mesh_frame* frame, clay_mask* mask, float brush_x) {
    Multires f;
    std::uint64_t id = 0;
    std::int32_t err = -1;
    REQUIRE(clay_multires_add_sculpt_layer(f.surface, "pass", &id, &err) == CLAY_OK);
    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, id, &err) == CLAY_OK);

    clay_multires_sculpt_layer_stroke* stroke = nullptr;
    REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
    if (frame)
        REQUIRE(clay_multires_sculpt_layer_stroke_set_world_frame(stroke, frame) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, &err) == CLAY_OK);
    const clay_mesh_brush_desc d = draw_at(brush_x, 0.6f, 0.5f);
    REQUIRE(clay_multires_sculpt_layer_stroke_stamp(stroke, &d, mask, nullptr) == CLAY_OK);
    std::size_t entries = 0;
    REQUIRE(clay_multires_sculpt_layer_stroke_commit(stroke, &entries) == CLAY_OK);
    clay_multires_sculpt_layer_stroke_destroy(stroke);
    return lift_of_level(f.surface, 2);
}

// One dynamic-surface stamp. The adaptive surface is not a document layer, so
// it has no _use_layer_transform and _set_world_frame is the only spelling --
// which is a fact about the ABI and is asserted below rather than assumed.
double dynamic_lift(const clay_mesh_frame* frame, clay_mask* mask, float brush_x) {
    clay_mesh* m = plane_mesh(8, 1.0f);
    clay_dynamic_surface* surface = nullptr;
    std::int32_t err = -1;
    REQUIRE(clay_dynamic_surface_from_mesh(m, nullptr, &surface, &err) == CLAY_OK);
    clay_dynamic_sculptor* s = nullptr;
    REQUIRE(clay_dynamic_sculptor_create(surface, &s) == CLAY_OK);
    if (frame) REQUIRE(clay_dynamic_sculptor_set_world_frame(s, frame) == CLAY_OK);
    const clay_mesh_brush_desc d = draw_at(brush_x, 0.6f, 0.5f);
    REQUIRE(clay_dynamic_sculptor_stamp(s, &d, nullptr, mask, nullptr) == CLAY_OK);

    clay_mesh* out = nullptr;
    REQUIRE(clay_dynamic_surface_to_mesh(surface, &out) == CLAY_OK);
    const float* p = clay_mesh_positions(out);
    const std::size_t n = clay_mesh_vertex_count(out);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += std::fabs(static_cast<double>(p[i * 3 + 1]));
    clay_mesh_destroy(out);
    clay_dynamic_sculptor_destroy(s);
    clay_dynamic_surface_destroy(surface);
    clay_mesh_destroy(m);
    return sum;
}

}  // namespace

TEST_CASE("place every surface: the sculpt-layer stroke's five verbs share the placed gate") {
    const clay_mesh_frame f = translation(kPlacedX);
    const double plain = layer_stroke_lift(&f, nullptr, kPlacedX);
    REQUIRE(plain > 0.0);

    Mask at_placed(kMaskCell);
    fill_box_at(at_placed, kPlacedX);
    Mask at_local(kMaskCell);
    fill_box_at(at_local, 0.0f);

    CHECK(layer_stroke_lift(&f, at_placed, kPlacedX) < plain * 0.01);
    CHECK(layer_stroke_lift(&f, at_local, kPlacedX) == doctest::Approx(plain));
}

TEST_CASE("place every surface: the adaptive surface takes a declared frame too") {
    const clay_mesh_frame f = translation(kPlacedX);
    const double plain = dynamic_lift(&f, nullptr, kPlacedX);
    REQUIRE(plain > 0.0);

    Mask at_placed(kMaskCell);
    fill_box_at(at_placed, kPlacedX);
    Mask at_local(kMaskCell);
    fill_box_at(at_local, 0.0f);

    CHECK(dynamic_lift(&f, at_placed, kPlacedX) < plain * 0.01);
    CHECK(dynamic_lift(&f, at_local, kPlacedX) == doctest::Approx(plain));
}

TEST_CASE("place every surface: a host that carries its own gesture is not opted in") {
    // THE CONTRACT A SHIPPING HOST DEPENDS ON, named rather than left implied by
    // the identity case above, because this is the one that must not be tidied
    // away by a later reader who sees `!has_frame` as a redundant branch.
    //
    // ClaySpaceDesktop found the unplaced gate independently, measured it, and
    // adapted to it: it carries gesture samples into the layer's frame itself
    // and paints its freeze mask in LAYER-LOCAL coordinates, so its mask cells
    // and the gate's samples agree. It passes NULL for every `mesh_to_world`
    // and declares no session frame anywhere.
    //
    // Placing the gate must therefore change NOTHING for it. A host that has
    // not heard of the frame is not opted in -- the rule every appended knob in
    // this ABI follows -- and here that rule is what stands between a correct
    // fix and a silently broken freeze on every placed subtool.
    const double plain = stamp_lift(nullptr, nullptr, 0.0f);
    REQUIRE(plain > 0.0);

    Mask carried(kMaskCell);
    fill_box_at(carried, 0.0f);  // painted where the host carried the gesture to

    // The freeze holds, and holds COMPLETELY: a layer-local mask against a
    // layer-local gate is the agreement the host built, and it survives.
    CHECK(stamp_lift(nullptr, carried, 0.0f) < plain * 0.01);

    // And the same handle declaring the frame is the OPT-IN, which is where the
    // host's mask would have to move to world coordinates with it. Asserted so
    // the two behaviours are pinned as a pair rather than one at a time: this
    // is the whole of what changes at the pin move, and it changes only for a
    // caller that asks for it.
    const clay_mesh_frame f = translation(kPlacedX);
    CHECK(stamp_lift(&f, carried, kPlacedX) == doctest::Approx(stamp_lift(&f, nullptr, kPlacedX)));
}
