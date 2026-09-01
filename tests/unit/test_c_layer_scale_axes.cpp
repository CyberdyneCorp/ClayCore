// A LAYER's per-axis scale across the C ABI (#373, c-abi and scene-model
// specs).
//
// The node half shipped in 0.54.0; a layer's placement still took one float, so
// a ZBrush-style gizmo on a whole subtool had to hide its axis boxes. These
// cases pin the half that was missing: what the composition means, what a
// squashed layer costs the field and what it does not, that everything a host
// reads about a shape agrees with the shape, and the one verb that refuses
// rather than approximating.
//
// The load-bearing case is the LAST one — three equal factors compile to
// byte-identical tape — because that is what makes every document written
// before this field unaffected by it.

#include <doctest/doctest.h>

#include <cmath>
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

const float kIdentityAxis[3] = {0.0f, 1.0f, 0.0f};
const float kOrigin[3] = {0.0f, 0.0f, 0.0f};

clay_layer_id sphere_layer(clay_document* doc, const char* name, float r) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
    return layer;
}

float eval_one(const clay_document* doc, float x, float y, float z) {
    const float p[3] = {x, y, z};
    float d = 0.0f;
    REQUIRE(clay_eval_points(doc, nullptr, p, 1, &d, nullptr) == CLAY_OK);
    return d;
}

// The compiled tape as bytes, for the byte-identity case. Instructions and
// params both, since a scale that leaked would land in the params and a
// composition that changed shape would land in the instructions.
struct TapeBytes {
    std::vector<clay_tape_instr> instrs;
    std::vector<float> params;
    std::vector<float> blob;
    bool operator==(const TapeBytes& o) const {
        return params == o.params && blob == o.blob &&
               instrs.size() == o.instrs.size() &&
               std::memcmp(instrs.data(), o.instrs.data(),
                           instrs.size() * sizeof(clay_tape_instr)) == 0;
    }
};

TapeBytes tape_bytes(const clay_document* doc) {
    clay_tape* tape = nullptr;
    REQUIRE(clay_tape_export(doc, nullptr, nullptr, &tape) == CLAY_OK);
    REQUIRE(tape != nullptr);
    TapeBytes out;
    size_t n = 0;
    const clay_tape_instr* ins = clay_tape_instrs(tape, &n);
    out.instrs.assign(ins, ins + n);
    const float* ps = clay_tape_params(tape, &n);
    out.params.assign(ps, ps + n);
    const float* bl = clay_tape_blob(tape, &n);
    if (bl) out.blob.assign(bl, bl + n);
    clay_tape_release(tape);
    return out;
}

int tape_is_exact(const clay_document* doc) {
    clay_tape* tape = nullptr;
    REQUIRE(clay_tape_export(doc, nullptr, nullptr, &tape) == CLAY_OK);
    int32_t exact = 0;
    REQUIRE(clay_tape_info(tape, &exact, nullptr, nullptr, nullptr, nullptr, nullptr) == CLAY_OK);
    clay_tape_release(tape);
    return exact;
}

float safe_step(const clay_document* doc) {
    clay_tape* tape = nullptr;
    REQUIRE(clay_tape_export(doc, nullptr, nullptr, &tape) == CLAY_OK);
    float step = 0.0f;
    REQUIRE(clay_tape_info(tape, nullptr, nullptr, &step, nullptr, nullptr, nullptr) == CLAY_OK);
    clay_tape_release(tape);
    return step;
}

}  // namespace

TEST_CASE("c abi: a squashed layer squashes its items") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);

    // The subtool gizmo's own move: stretch the whole layer 3x on X. A unit
    // sphere becomes an ellipsoid three long and one wide.
    const float scale[3] = {3.0f, 1.0f, 1.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);
    CHECK(eval_one(d.doc, 3.0f, 0, 0) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_one(d.doc, 0, 1.0f, 0) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_one(d.doc, 0, 0, 1.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(eval_one(d.doc, 4.0f, 0, 0) > 0.0f);
    CHECK(eval_one(d.doc, 2.0f, 0, 0) < 0.0f);

    // The bounds a host zooms and culls with describe the shape it IS.
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    int32_t has = 0;
    REQUIRE(clay_layer_bounds(d.doc, layer, lo, hi, &has) == CLAY_OK);
    REQUIRE(has != 0);
    CHECK(hi[0] == doctest::Approx(3.0f).epsilon(1e-3));
    CHECK(hi[1] == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(lo[0] == doctest::Approx(-3.0f).epsilon(1e-3));

    // And so does the pick. A ray down -X from beyond the stretched end must
    // land near x = 3, where the surface now is and where an unsquashed layer
    // has nothing at all.
    const float origin[3] = {8.0f, 0.0f, 0.0f}, dir[3] = {-1.0f, 0.0f, 0.0f};
    int32_t got = 0;
    float t = 0.0f, hit_pos[3] = {0, 0, 0}, hit_n[3] = {0, 0, 0};
    REQUIRE(clay_raycast(d.doc, origin, dir, &got, &t, hit_pos, hit_n) == CLAY_OK);
    CHECK(got != 0);
    CHECK(hit_pos[0] == doctest::Approx(3.0f).epsilon(1e-2));
}

TEST_CASE("c abi: a squashed layer stays marchable and says it is not exact") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);
    const float before = safe_step(d.doc);
    CHECK(tape_is_exact(d.doc) != 0);

    const float scale[3] = {3.0f, 1.0f, 0.5f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);
    const float after = safe_step(d.doc);
    // The field stays 1-Lipschitz: dividing by s and multiplying back by min(s)
    // can only shorten, so no marcher slows down. That is the whole cost model
    // and it is worth pinning, because a step scale that moved would be a
    // performance regression on every squashed document.
    CHECK(after == doctest::Approx(before));
    CHECK(tape_is_exact(d.doc) == 0);  // and exactness is what actually goes

    // Conservative, not merely inexact: the value never overestimates the true
    // distance. Probed along the squashed axis, where the error is largest.
    for (float x = 0.1f; x < 4.0f; x += 0.13f) {
        const float d_reported = eval_one(d.doc, x, 0.0f, 0.0f);
        const float d_true = std::fabs(x - 3.0f);
        CHECK(std::fabs(d_reported) <= d_true + 1e-4f);
    }
}

TEST_CASE("c abi: the two layer readers agree, and the narrow one refuses") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);

    // Placed through the UNIFORM setter, the per-axis reader answers (s, s, s)
    // — which is what lets one manipulator read it and never branch.
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, kOrigin, kIdentityAxis, 0.0f, 2.0f) ==
            CLAY_OK);
    float pos[3] = {0, 0, 0}, axis[3] = {0, 0, 0}, angle = 0.0f, triple[3] = {0, 0, 0};
    REQUIRE(clay_document_layer_transform_nonuniform(d.doc, layer, pos, axis, &angle, triple) ==
            CLAY_OK);
    CHECK(triple[0] == doctest::Approx(2.0f));
    CHECK(triple[1] == doctest::Approx(2.0f));
    CHECK(triple[2] == doctest::Approx(2.0f));
    float one = 0.0f;
    REQUIRE(clay_document_layer_transform(d.doc, layer, pos, axis, &angle, &one) == CLAY_OK);
    CHECK(one == doctest::Approx(2.0f));

    // Squashed, the narrow reader refuses rather than picking one of three: a
    // read-change-write through the uniform setter would round the artist's
    // squash away without saying so.
    const float scale[3] = {3.0f, 1.0f, 0.5f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);
    CHECK(clay_document_layer_transform(d.doc, layer, pos, axis, &angle, &one) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_layer_transform_nonuniform(d.doc, layer, pos, axis, &angle, triple) ==
            CLAY_OK);
    CHECK(triple[0] == doctest::Approx(3.0f));
    CHECK(triple[2] == doctest::Approx(0.5f));

    // And the uniform SETTER clears it, because a placement is one value: a
    // caller setting it means the whole of it.
    REQUIRE(clay_document_set_layer_transform(d.doc, layer, kOrigin, kIdentityAxis, 0.0f, 1.0f) ==
            CLAY_OK);
    CHECK(clay_document_layer_transform(d.doc, layer, pos, axis, &angle, &one) == CLAY_OK);
    CHECK(one == doctest::Approx(1.0f));

    // Typed refusals, on the same terms the node-level call states.
    const float zeroed[3] = {1.0f, 0.0f, 1.0f};
    CHECK(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis, 0.0f,
                                                       zeroed) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis, 0.0f,
                                                       nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_document_set_layer_transform_nonuniform(d.doc, 9999, kOrigin, kIdentityAxis, 0.0f,
                                                       triple) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_document_layer_transform_nonuniform(d.doc, 9999, pos, axis, &angle, triple) ==
          CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("c abi: a squashed layer is one undo step, and the inverse restores it") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);
    REQUIRE(clay_document_enable_undo(d.doc) == CLAY_OK);
    const float before = eval_one(d.doc, 2.0f, 0, 0);

    const float scale[3] = {3.0f, 1.0f, 1.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);
    CHECK(eval_one(d.doc, 2.0f, 0, 0) != doctest::Approx(before));
    // The placement and the squash are ONE command, so one undo restores a
    // frame that actually existed rather than half of two.
    int32_t moved = 0;
    REQUIRE(clay_document_undo(d.doc, &moved) == CLAY_OK);
    CHECK(moved != 0);
    CHECK(eval_one(d.doc, 2.0f, 0, 0) == doctest::Approx(before));
    REQUIRE(clay_document_redo(d.doc, &moved) == CLAY_OK);
    CHECK(moved != 0);
    CHECK(eval_one(d.doc, 2.0f, 0, 0) != doctest::Approx(before));
}

TEST_CASE("c abi: a squashed layer round-trips through the document format") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);
    const float scale[3] = {3.0f, 0.5f, 2.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);
    const float probe = eval_one(d.doc, 2.0f, 0.3f, 0.4f);

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(d.doc, &blob) == CLAY_OK);
    REQUIRE(blob != nullptr);

    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &back) ==
            CLAY_OK);
    clay_blob_destroy(blob);
    REQUIRE(back != nullptr);
    float pos[3] = {0, 0, 0}, axis[3] = {0, 0, 0}, angle = 0.0f, triple[3] = {0, 0, 0};
    REQUIRE(clay_document_layer_transform_nonuniform(back, layer, pos, axis, &angle, triple) ==
            CLAY_OK);
    CHECK(triple[0] == doctest::Approx(3.0f));
    CHECK(triple[1] == doctest::Approx(0.5f));
    CHECK(triple[2] == doctest::Approx(2.0f));
    CHECK(eval_one(back, 2.0f, 0.3f, 0.4f) == doctest::Approx(probe));
    clay_document_destroy(back);
}

TEST_CASE("c abi: a drag reaches a squashed layer's surface") {
    // The measurement drag-a-squashed-item made for the NODE scale, at the
    // layer level: the surface of a 3x-stretched layer is at world x = 3, and a
    // drag there has to move it. Before this change the composition dropped the
    // layer's squash and the warp landed where the layer is not.
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);
    const float scale[3] = {3.0f, 1.0f, 1.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(d.doc, layer, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);

    const float before = eval_one(d.doc, 3.0f, 0.0f, 0.0f);
    CHECK(before == doctest::Approx(0.0f).epsilon(1e-4));

    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = 0.8f;
    const float centre[3] = {3.0f, 0.0f, 0.0f}, push[3] = {0.35f, 0.0f, 0.0f};
    size_t applied = 0;
    REQUIRE(clay_layer_move_surface(d.doc, layer, centre, push, &mp, &applied) == CLAY_OK);
    CHECK(applied > 0);
    const float after = eval_one(d.doc, 3.0f, 0.0f, 0.0f);
    CHECK(after != doctest::Approx(before).epsilon(1e-4));

    // And the reach is bounded by what was circled: the radius is divided by
    // the LARGEST factor, so a point well outside the drag's world sphere is
    // untouched. Over-reach is not recoverable; under-reach is.
    Doc control;
    const clay_layer_id cl = sphere_layer(control.doc, "subtool", 1.0f);
    REQUIRE(clay_document_set_layer_transform_nonuniform(control.doc, cl, kOrigin, kIdentityAxis,
                                                         0.0f, scale) == CLAY_OK);
    const float far_before = eval_one(control.doc, 0.0f, 1.0f, 0.0f);
    REQUIRE(clay_layer_move_surface(control.doc, cl, centre, push, &mp, &applied) == CLAY_OK);
    CHECK(eval_one(control.doc, 0.0f, 1.0f, 0.0f) == doctest::Approx(far_before).epsilon(1e-4));
}

TEST_CASE("c abi: a lattice gizmo refuses a squashed layer rather than warping wrongly") {
    Doc d;
    const clay_layer_id layer = sphere_layer(d.doc, "subtool", 1.0f);

    clay_gizmo_cage cage;
    std::memset(&cage, 0, sizeof cage);
    cage.struct_size = sizeof cage;
    cage.axis[1] = 1.0f;
    cage.scale = 1.0f;
    cage.box_min[0] = cage.box_min[1] = cage.box_min[2] = -1.5f;
    cage.box_max[0] = cage.box_max[1] = cage.box_max[2] = 1.5f;
    cage.nx = cage.ny = cage.nz = 2;
    std::vector<float> offsets(static_cast<size_t>(2 * 2 * 2) * 3, 0.0f);
    offsets[0] = 0.4f;  // one control point moved, so the cage is not identity

    // Unsquashed, it warps.
    const float unsquashed = eval_one(d.doc, 1.0f, 0.0f, 0.0f);
    size_t applied = 0;
    REQUIRE(clay_layer_lattice_gizmo(d.doc, layer, &cage, offsets.data(), &applied) == CLAY_OK);
    CHECK(applied > 0);
    CHECK(eval_one(d.doc, 1.0f, 0.0f, 0.0f) != doctest::Approx(unsquashed).epsilon(1e-5));

    // Squashed, it changes nothing — and that is the point. A cage records its
    // placement as a rigid transform, and on a squashed layer the map it needs
    // is a general affine one; placing one through the narrower record would
    // warp every item in a space it does not occupy, silently.
    Doc s;
    const clay_layer_id sl = sphere_layer(s.doc, "subtool", 1.0f);
    const float scale[3] = {3.0f, 1.0f, 1.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(s.doc, sl, kOrigin, kIdentityAxis, 0.0f,
                                                         scale) == CLAY_OK);
    const float squashed_before = eval_one(s.doc, 3.0f, 0.0f, 0.0f);
    size_t squashed_applied = 0;
    REQUIRE(clay_layer_lattice_gizmo(s.doc, sl, &cage, offsets.data(), &squashed_applied) ==
            CLAY_OK);
    CHECK(squashed_applied == 0);  // nothing warped, and it said so
    CHECK(eval_one(s.doc, 3.0f, 0.0f, 0.0f) == doctest::Approx(squashed_before));
}

TEST_CASE("c abi: three equal factors are the uniform layer, byte for byte") {
    // THE LOAD-BEARING CASE. Everything above adds behaviour; this is what says
    // the addition costs nothing to a document that never asks for it. A tape
    // that differed by one float here would mean every existing document
    // silently recompiled.
    Doc uniform;
    const clay_layer_id ul = sphere_layer(uniform.doc, "subtool", 1.0f);
    REQUIRE(clay_document_set_layer_transform(uniform.doc, ul, kOrigin, kIdentityAxis, 0.0f,
                                              2.0f) == CLAY_OK);

    Doc triple;
    const clay_layer_id tl = sphere_layer(triple.doc, "subtool", 1.0f);
    const float equal[3] = {2.0f, 2.0f, 2.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(triple.doc, tl, kOrigin, kIdentityAxis,
                                                         0.0f, equal) == CLAY_OK);

    CHECK(tape_bytes(uniform.doc) == tape_bytes(triple.doc));

    // And the untouched default is the identity: a layer nobody scaled compiles
    // to what it always did.
    Doc plain;
    const clay_layer_id pl = sphere_layer(plain.doc, "subtool", 1.0f);
    Doc ones;
    const clay_layer_id ol = sphere_layer(ones.doc, "subtool", 1.0f);
    const float identity[3] = {1.0f, 1.0f, 1.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(ones.doc, ol, kOrigin, kIdentityAxis,
                                                         0.0f, identity) == CLAY_OK);
    CHECK(tape_bytes(plain.doc) == tape_bytes(ones.doc));
    (void)pl;
}
