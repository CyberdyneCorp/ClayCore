// C ABI implementation (c-abi spec): opaque handles over the C++ modules,
// thread-local error details, no exceptions cross this boundary (the core
// builds with -fno-exceptions on GCC/Clang).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "clay.h"
#include "clay_internal.h"
#include "clay/brush/gate_bake.h"
#include "clay/brush/lattice_gizmo.h"
#include "clay/brush/mask_extrude.h"
#include "clay/brush/magnify.h"
#include "clay/brush/move.h"
#include "clay/brush/procedural_mask.h"
#include "clay/brush/preset.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/brush/stroke.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/project.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/brush/stroke.h"
#include "clay/brush/surface_measure.h"
#include "clay/brush/tube.h"
#include "clay/cut/cut.h"
#include "clay/eval/backend.h"
#include "clay/eval/bake_points.h"
#include "clay/eval/bake_volume.h"
#include "clay/field/flatten.h"
#include "clay/field/move_topological.h"
#include "clay/field/relax.h"
#include "clay/io/clayspace.h"
#include "clay/io/handoff.h"
#include "clay/io/memory.h"
#include "clay/io/mesh_io.h"
#include "clay/io/parity_fixture.h"
#include "clay/kernel/field.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/deform.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/lattice.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/transfer.h"
#include "clay/mesh/voxel_remesh.h"
#include "clay/mesh/weld.h"
#include "clay/mesh/validate.h"
#include "clay/parallel/cancel.h"
#include "clay/parallel/thread_pool.h"
#include "clay/pick/pick.h"
#include "clay/scene/armature.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/curve.h"
#include "clay/scene/tape.h"
#include "clay/session/history.h"
#include "clay/session/sdf_sculpt.h"
#include "clay/version.h"
#include "clay/voxel/grab.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/groups.h"
#include "clay/voxel/hide.h"
#include "clay/voxel/mask.h"
#include "desc_version.h"

using namespace clay;

// -- enum pinning (c-abi spec: enumerations stay in step with the engine) ----
// One assertion per enumerator: the C values ARE the engine's values, so no
// translation table exists to drift. Adding a primitive, op or blend to the
// scene model without a C enumerator is caught by the exhaustive switches in
// prim_is_known/op_is_known/blend_is_known below, which have no default.

static_assert(CLAY_PRIM_SPHERE == static_cast<int>(scene::PrimType::Sphere));
static_assert(CLAY_PRIM_BOX == static_cast<int>(scene::PrimType::Box));
static_assert(CLAY_PRIM_ROUND_BOX == static_cast<int>(scene::PrimType::RoundBox));
static_assert(CLAY_PRIM_BOX_FRAME == static_cast<int>(scene::PrimType::BoxFrame));
static_assert(CLAY_PRIM_TORUS == static_cast<int>(scene::PrimType::Torus));
static_assert(CLAY_PRIM_CAPSULE == static_cast<int>(scene::PrimType::Capsule));
static_assert(CLAY_PRIM_CAPPED_CYLINDER == static_cast<int>(scene::PrimType::CappedCylinder));
static_assert(CLAY_PRIM_ROUNDED_CYLINDER == static_cast<int>(scene::PrimType::RoundedCylinder));
static_assert(CLAY_PRIM_CAPPED_CONE == static_cast<int>(scene::PrimType::CappedCone));
static_assert(CLAY_PRIM_ROUND_CONE == static_cast<int>(scene::PrimType::RoundCone));
static_assert(CLAY_PRIM_ELLIPSOID == static_cast<int>(scene::PrimType::Ellipsoid));
static_assert(CLAY_PRIM_OCTAHEDRON == static_cast<int>(scene::PrimType::Octahedron));
static_assert(CLAY_PRIM_HEX_PRISM == static_cast<int>(scene::PrimType::HexPrism));
static_assert(CLAY_PRIM_PYRAMID == static_cast<int>(scene::PrimType::Pyramid));
static_assert(CLAY_PRIM_STROKE == static_cast<int>(scene::PrimType::Stroke));
static_assert(CLAY_PRIM_EXTRUDE == static_cast<int>(scene::PrimType::Extrude));
static_assert(CLAY_PRIM_REVOLVE == static_cast<int>(scene::PrimType::Revolve));
static_assert(CLAY_PRIM_CAPPED_TORUS == static_cast<int>(scene::PrimType::CappedTorus));
static_assert(CLAY_PRIM_LINK == static_cast<int>(scene::PrimType::Link));
static_assert(CLAY_PRIM_CYLINDER_INFINITE ==
              static_cast<int>(scene::PrimType::CylinderInfinite));
static_assert(CLAY_PRIM_CONE == static_cast<int>(scene::PrimType::Cone));
static_assert(CLAY_PRIM_PLANE == static_cast<int>(scene::PrimType::Plane));
static_assert(CLAY_PRIM_CUT_SPHERE == static_cast<int>(scene::PrimType::CutSphere));
static_assert(CLAY_PRIM_CUT_HOLLOW_SPHERE ==
              static_cast<int>(scene::PrimType::CutHollowSphere));
static_assert(CLAY_PRIM_SOLID_ANGLE == static_cast<int>(scene::PrimType::SolidAngle));
static_assert(CLAY_PRIM_TETRAHEDRON == static_cast<int>(scene::PrimType::Tetrahedron));
static_assert(CLAY_PRIM_DODECAHEDRON == static_cast<int>(scene::PrimType::Dodecahedron));
static_assert(CLAY_PRIM_ICOSAHEDRON == static_cast<int>(scene::PrimType::Icosahedron));
static_assert(CLAY_PRIM_TRI_PRISM == static_cast<int>(scene::PrimType::TriPrism));
static_assert(CLAY_PRIM_OCTAHEDRON_CHEAP ==
              static_cast<int>(scene::PrimType::OctahedronCheap));
static_assert(CLAY_PRIM_LNORM_SPHERE == static_cast<int>(scene::PrimType::LNormSphere));
static_assert(CLAY_PRIM_LOFT == static_cast<int>(scene::PrimType::Loft));
static_assert(CLAY_PRIM_SWEPT == static_cast<int>(scene::PrimType::Swept));
static_assert(CLAY_PRIM_VOLUME == static_cast<int>(scene::PrimType::Volume));
static_assert(CLAY_PRIM_ARMATURE == static_cast<int>(scene::PrimType::Armature));
// The tape's own count: a new opcode without a clay_prim entry fails here.
static_assert(CLAY_PRIM_ARMATURE + 1 == kernel::ctape_prim_count);

static_assert(CLAY_OP_ADD == static_cast<int>(scene::Op::Add));
static_assert(CLAY_OP_SUBTRACT == static_cast<int>(scene::Op::Subtract));
static_assert(CLAY_OP_INTERSECT == static_cast<int>(scene::Op::Intersect));
static_assert(CLAY_OP_PAINT == static_cast<int>(scene::Op::Paint));
static_assert(CLAY_OP_GROOVE == static_cast<int>(scene::Op::Groove));
static_assert(CLAY_OP_TONGUE == static_cast<int>(scene::Op::Tongue));
static_assert(CLAY_OP_PIPE == static_cast<int>(scene::Op::Pipe));
static_assert(CLAY_OP_ENGRAVE == static_cast<int>(scene::Op::Engrave));
static_assert(CLAY_OP_EMBOSS == static_cast<int>(scene::Op::Emboss));
static_assert(CLAY_OP_INSET == static_cast<int>(scene::Op::Inset));
static_assert(CLAY_OP_SHELL == static_cast<int>(scene::Op::Shell));
static_assert(CLAY_OP_REPLACE == static_cast<int>(scene::Op::Replace));
static_assert(CLAY_OP_TRANSITION_LINEAR == static_cast<int>(scene::Op::TransitionLinear));
static_assert(CLAY_OP_TRANSITION_RADIAL == static_cast<int>(scene::Op::TransitionRadial));

static_assert(CLAY_BLEND_HARD == static_cast<int>(scene::BlendProfile::Hard));
static_assert(CLAY_BLEND_QUADRATIC == static_cast<int>(scene::BlendProfile::Quadratic));
static_assert(CLAY_BLEND_CUBIC == static_cast<int>(scene::BlendProfile::Cubic));
static_assert(CLAY_BLEND_CIRCULAR == static_cast<int>(scene::BlendProfile::Circular));
static_assert(CLAY_BLEND_CHAMFER == static_cast<int>(scene::BlendProfile::Chamfer));

static_assert(CLAY_DEFORM_TWIST == static_cast<int>(kernel::cdeform_twist));
static_assert(CLAY_DEFORM_BEND == static_cast<int>(kernel::cdeform_bend));
static_assert(CLAY_DEFORM_TAPER == static_cast<int>(kernel::cdeform_taper));
static_assert(CLAY_DEFORM_DISPLACE == static_cast<int>(kernel::cdeform_displace));
static_assert(CLAY_DEFORM_WRAP_AROUND == static_cast<int>(kernel::cdeform_wrap));
static_assert(CLAY_DEFORM_ELONGATE == static_cast<int>(kernel::cdeform_elongate));
static_assert(CLAY_DEFORM_BEND_LINEAR == static_cast<int>(kernel::cdeform_bend_linear));
static_assert(CLAY_DEFORM_BEND_RADIAL == static_cast<int>(kernel::cdeform_bend_radial));
static_assert(CLAY_DEFORM_ELONGATE_AXIS == static_cast<int>(kernel::cdeform_elongate_axis));
static_assert(CLAY_DEFORM_GRAB == static_cast<int>(kernel::cdeform_grab));
static_assert(CLAY_DEFORM_MAGNIFY == static_cast<int>(kernel::cdeform_magnify));
static_assert(CLAY_DEFORM_NOISE == static_cast<int>(kernel::cdeform_noise));
static_assert(CLAY_DEFORM_POSE == static_cast<int>(kernel::cdeform_pose));
static_assert(CLAY_DEFORM_POSE_LINE == static_cast<int>(kernel::cdeform_pose_line));
static_assert(CLAY_DEFORM_TWIST_RANGE == static_cast<int>(kernel::cdeform_twist_range));
static_assert(CLAY_DEFORM_BEND_RANGE == static_cast<int>(kernel::cdeform_bend_range));

static_assert(CLAY_PROFILE_CIRCLE == static_cast<int>(kernel::cprofile_circle));
static_assert(CLAY_PROFILE_BOX == static_cast<int>(kernel::cprofile_box));
static_assert(CLAY_PROFILE_HEXAGON == static_cast<int>(kernel::cprofile_hexagon));
static_assert(CLAY_PROFILE_TRIANGLE == static_cast<int>(kernel::cprofile_triangle));
static_assert(CLAY_PROFILE_TRAPEZOID == static_cast<int>(kernel::cprofile_trapezoid));
static_assert(CLAY_PROFILE_VESICA == static_cast<int>(kernel::cprofile_vesica));
static_assert(CLAY_PROFILE_POLYGON == static_cast<int>(kernel::cprofile_polygon));

static_assert(CLAY_EASE_LINEAR == kernel::ease_linear);
static_assert(CLAY_EASE_COUNT == kernel::ease_count);

static_assert(CLAY_MIRROR_X == voxel::kVoxMirrorX);
static_assert(CLAY_MIRROR_Y == voxel::kVoxMirrorY);
static_assert(CLAY_MIRROR_Z == voxel::kVoxMirrorZ);

static_assert(CLAY_BRUSH_SHAPE_CUBE == static_cast<int>(voxel::BrushShape::Cube));
static_assert(CLAY_BRUSH_SHAPE_SPHERE == static_cast<int>(voxel::BrushShape::Sphere));

static_assert(CLAY_BRUSH_FALLOFF_CONSTANT == static_cast<int>(voxel::BrushFalloff::Constant));
static_assert(CLAY_BRUSH_FALLOFF_LINEAR == static_cast<int>(voxel::BrushFalloff::Linear));
static_assert(CLAY_BRUSH_FALLOFF_SMOOTH == static_cast<int>(voxel::BrushFalloff::Smooth));
static_assert(CLAY_BRUSH_FALLOFF_GAUSSIAN == static_cast<int>(voxel::BrushFalloff::Gaussian));

// A voxel coordinate is three int32 values in x, y, z order and nothing else,
// which is what lets a flood selection reach the caller's buffer as one copy
// instead of a loop that could disagree with the engine's field order.
static_assert(sizeof(voxel::VoxelCoord) == 3 * sizeof(std::int32_t));
static_assert(alignof(voxel::VoxelCoord) == alignof(std::int32_t));
static_assert(offsetof(voxel::VoxelCoord, x) == 0);
static_assert(offsetof(voxel::VoxelCoord, y) == sizeof(std::int32_t));
static_assert(offsetof(voxel::VoxelCoord, z) == 2 * sizeof(std::int32_t));

// -- the brick cache ---------------------------------------------------------
// One assertion per enumerator, as above: the C values ARE the engine's, and
// the default-less switches in to_c_brick_state / to_c_submit below make a new
// engine enumerator a -Werror compile error rather than a silent mapping.
static_assert(CLAY_BRICK_INSIDE == static_cast<int>(brick::BrickState::Inside));
static_assert(CLAY_BRICK_OUTSIDE == static_cast<int>(brick::BrickState::Outside));
static_assert(CLAY_BRICK_SURFACE == static_cast<int>(brick::BrickState::Surface));
// CLAY_BRICK_MISSING has no engine counterpart to pin — the C++ surface says
// "nothing here" with a null Brick pointer, and this boundary hands back a
// value instead, the way clay_mesher names three entry points that are not an
// enumeration. It must stay outside the engine's range, which this checks.
static_assert(CLAY_BRICK_MISSING > CLAY_BRICK_SURFACE);

static_assert(CLAY_BRICK_SUBMIT_ACCEPTED == static_cast<int>(brick::SubmitResult::Accepted));
static_assert(CLAY_BRICK_SUBMIT_STALE == static_cast<int>(brick::SubmitResult::Stale));
static_assert(CLAY_BRICK_SUBMIT_BUDGET_EXCEEDED ==
              static_cast<int>(brick::SubmitResult::BudgetExceeded));

static_assert(CLAY_NORMAL_NONE == static_cast<int>(mesh::NormalMode::None));
static_assert(CLAY_NORMAL_FACE == static_cast<int>(mesh::NormalMode::Face));
static_assert(CLAY_NORMAL_GRADIENT == static_cast<int>(mesh::NormalMode::Gradient));

// clay_brick_request IS brick::BrickRequest, field for field, so a drain is one
// memcpy out of the engine's own vector and nothing is transcribed. The
// voxel::VoxelCoord precedent above, applied to a struct that is three engine
// types glued together: BrickKey, a generation, and eval::GridQuery, whose
// cfloat3 is three bare floats.
static_assert(sizeof(brick::BrickKey) == 3 * sizeof(std::int32_t));
static_assert(offsetof(brick::BrickKey, x) == 0);
static_assert(offsetof(brick::BrickKey, y) == sizeof(std::int32_t));
static_assert(offsetof(brick::BrickKey, z) == 2 * sizeof(std::int32_t));
static_assert(sizeof(int) == sizeof(std::int32_t));  // GridQuery's nx, ny, nz
static_assert(offsetof(eval::GridQuery, ny) == offsetof(eval::GridQuery, nx) + sizeof(int));
static_assert(offsetof(eval::GridQuery, nz) == offsetof(eval::GridQuery, nx) + 2 * sizeof(int));
static_assert(sizeof(clay_brick_request) == sizeof(brick::BrickRequest));
static_assert(alignof(clay_brick_request) == alignof(brick::BrickRequest));
static_assert(offsetof(clay_brick_request, key) == offsetof(brick::BrickRequest, key));
static_assert(offsetof(clay_brick_request, generation) ==
              offsetof(brick::BrickRequest, generation));
static_assert(offsetof(clay_brick_request, origin) ==
              offsetof(brick::BrickRequest, grid) + offsetof(eval::GridQuery, origin));
static_assert(offsetof(clay_brick_request, spacing) ==
              offsetof(brick::BrickRequest, grid) + offsetof(eval::GridQuery, spacing));
static_assert(offsetof(clay_brick_request, band) == offsetof(brick::BrickRequest, band));
static_assert(offsetof(clay_brick_request, dims) ==
              offsetof(brick::BrickRequest, grid) + offsetof(eval::GridQuery, nx));
static_assert(std::is_trivially_copyable_v<brick::BrickRequest>);

// clay_voxel_face has no engine enumeration behind it either: the face ids are
// a documented convention of pick::VoxelHit, so nothing here can assert them.
// What pins them is the smoke test, which shoots a ray down each axis and
// requires the face and the adjacent cell clay.h names.

namespace {

thread_local std::string g_last_error;

clay_result fail(clay_result code, std::string detail) {
    g_last_error = std::move(detail);
    return code;
}

// Enum validation, and at the same time the drift guard: the switches list
// every engine enumerator and have no default, so adding one to the scene
// model without a clay.h entry is a -Werror compile error here.
bool prim_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<scene::PrimType>(v)) {
        case scene::PrimType::Sphere:
        case scene::PrimType::Box:
        case scene::PrimType::RoundBox:
        case scene::PrimType::BoxFrame:
        case scene::PrimType::Torus:
        case scene::PrimType::Capsule:
        case scene::PrimType::CappedCylinder:
        case scene::PrimType::RoundedCylinder:
        case scene::PrimType::CappedCone:
        case scene::PrimType::RoundCone:
        case scene::PrimType::Ellipsoid:
        case scene::PrimType::Octahedron:
        case scene::PrimType::HexPrism:
        case scene::PrimType::Pyramid:
        case scene::PrimType::Stroke:
        case scene::PrimType::Extrude:
        case scene::PrimType::Revolve:
        case scene::PrimType::CappedTorus:
        case scene::PrimType::Link:
        case scene::PrimType::CylinderInfinite:
        case scene::PrimType::Cone:
        case scene::PrimType::Plane:
        case scene::PrimType::CutSphere:
        case scene::PrimType::CutHollowSphere:
        case scene::PrimType::SolidAngle:
        case scene::PrimType::Tetrahedron:
        case scene::PrimType::Dodecahedron:
        case scene::PrimType::Icosahedron:
        case scene::PrimType::TriPrism:
        case scene::PrimType::OctahedronCheap:
        case scene::PrimType::Loft:
        case scene::PrimType::Swept:
        case scene::PrimType::Volume:
        case scene::PrimType::Armature:
        case scene::PrimType::LNormSphere: return true;
    }
    return false;
}

// Op::None is groups only: children apply inline, so it is not a combine an
// item may carry.
bool op_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<scene::Op>(v)) {
        case scene::Op::Add:
        case scene::Op::Subtract:
        case scene::Op::Intersect:
        case scene::Op::Paint:
        case scene::Op::Groove:
        case scene::Op::Tongue:
        case scene::Op::Pipe:
        case scene::Op::Engrave:
        case scene::Op::Emboss:
        case scene::Op::Inset:
        case scene::Op::Shell:
        case scene::Op::Replace:
        case scene::Op::Relief:
        case scene::Op::Incise:
        case scene::Op::TransitionLinear:
        case scene::Op::TransitionRadial: return true;
        case scene::Op::None: return false;
    }
    return false;
}

bool blend_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<scene::BlendProfile>(v)) {
        case scene::BlendProfile::Hard:
        case scene::BlendProfile::Quadratic:
        case scene::BlendProfile::Cubic:
        case scene::BlendProfile::Circular:
        case scene::BlendProfile::Chamfer: return true;
    }
    return false;
}

// The blend half of an op/blend edit, which an item and a group state alike;
// only the set of ops they accept differs.
clay_result validate_blend(std::int32_t blend, float blend_k, float rounding) {
    if (!blend_is_known(blend)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown blend");
    if (!(blend_k >= 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "blend k must be >= 0");
    if (!(rounding >= 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "rounding must be >= 0");
    return CLAY_OK;
}

clay_result validate_item_op_blend(std::int32_t op, std::int32_t blend, float blend_k,
                                   float rounding) {
    if (!op_is_known(op)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown combine op");
    return validate_blend(blend, blend_k, rounding);
}

// What a GROUP may carry, which is not what an item may carry. Op::None is the
// inline op and belongs to groups alone; the transitions belong to items alone,
// because compile_group emits no transition parameters and a group carrying one
// would morph on the compiler's defaults instead of the node's. Inline reads no
// blend, rounding or colour off the group at all, so those are refused rather
// than silently ignored — an accepted blend would still dilate the group's
// influence bound and dirty more than the edit touches.
clay_result validate_group_op_blend(std::int32_t op, std::int32_t blend, float blend_k,
                                    float rounding) {
    if (op != static_cast<std::int32_t>(scene::Op::None) && !op_is_known(op))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown combine op");
    if (scene::op_is_transition(static_cast<scene::Op>(op)))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a group cannot carry a transition op");
    clay_result r = validate_blend(blend, blend_k, rounding);
    if (r != CLAY_OK) return r;
    if (op == static_cast<std::int32_t>(scene::Op::None) &&
        (blend != CLAY_BLEND_HARD || blend_k != 0.0f || rounding != 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "an inline group reads no blend or rounding: its children combine into "
                    "the outer chain with their own");
    return CLAY_OK;
}

bool brush_shape_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<voxel::BrushShape>(v)) {
        case voxel::BrushShape::Cube:
        case voxel::BrushShape::Sphere: return true;
    }
    return false;
}

bool brush_falloff_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<voxel::BrushFalloff>(v)) {
        case voxel::BrushFalloff::Constant:
        case voxel::BrushFalloff::Linear:
        case voxel::BrushFalloff::Smooth:
        case voxel::BrushFalloff::Gaussian: return true;
    }
    return false;
}

// The one enumeration with nothing behind it to pin: the meshers are three
// separate entry points, not an engine enum, so this list IS the definition.
// The switch still has no default, which keeps adding a clay_mesher value
// without teaching mesh_with about it a -Werror compile error.
bool mesher_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<clay_mesher>(v)) {
        case CLAY_MESHER_MARCHING:
        case CLAY_MESHER_NETS:
        case CLAY_MESHER_DUAL_CONTOURING: return true;
    }
    return false;
}

// Same rule for the quad mode, and for the same reason: nothing downstream
// enumerates it, so an unknown value must be refused here rather than
// defaulted into the dual.
bool quad_mode_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<clay_quad_mode>(v)) {
        case CLAY_QUAD_DUAL:
        case CLAY_QUAD_FACES: return true;
    }
    return false;
}

// Versioned descriptor structs (c-abi spec): the prefix rule lives in
// desc_version.h; this maps its verdict onto the boundary's error code and
// detail message. struct_size is required — there is no "0 means the original
// layout" sentinel, because a descriptor from ABI 0.1.0 (which had no
// struct_size) puts its prim or its voxel_size in that word, and a zero there
// is indistinguishable from a 0.1.0 sphere. Requiring it turns every such
// call into a rejection instead of a silently shifted read.
template <typename Desc>
clay_result read_desc(const Desc* src, std::size_t original, Desc* out) {
    std::string declared = std::to_string(src->struct_size);
    switch (clay_abi::read_desc(src, original, out)) {
        case clay_abi::DescStatus::Ok: return CLAY_OK;
        case clay_abi::DescStatus::TooShort:
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "struct_size below the original layout: " + declared +
                            " (set it to the sizeof of the struct you compiled against)");
        default:
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "struct_size is not a descriptor size: " + declared);
    }
}

// Fill a caller's OUTPUT descriptor, bounded by the size THEY declared.
//
// `read_desc` validates an incoming struct_size; it does not bound what we
// write back afterwards, and the difference is a buffer overrun. The natural
// spelling — `*out = clay_thing{}` then assign the fields — writes
// sizeof(clay_thing) bytes as THIS build defines it, so the moment a descriptor
// grows a field, every host compiled against the older header has its buffer
// overwritten past the end.
//
// That is not hypothetical: growing clay_brick_stats by two fields segfaulted
// the ctypes ABI check, which declares the ORIGINAL layout on purpose. The
// checker is the reason this is a fixed bug rather than a shipped one.
template <typename Desc>
void write_desc(Desc* out, std::uint32_t declared, const Desc& value) {
    const std::size_t n = std::min<std::size_t>(declared, sizeof(Desc));
    std::memcpy(out, &value, n);
    // The caller keeps the size they declared: it describes THEIR buffer, and
    // handing back ours would tell them fields exist that were never written.
    out->struct_size = declared;
}

// Original layouts (ABI 0.2.0), named by their last field so appending one
// does not silently move the baseline.
constexpr std::size_t kItemDescOriginal = offsetof(clay_item_desc, mirror) + sizeof(std::int32_t);
constexpr std::size_t kMeshParamsOriginal =
    offsetof(clay_mesh_params, decimate_ratio) + sizeof(float);
constexpr std::size_t kBrushParamsOriginal =
    offsetof(clay_brush_params, seed) + sizeof(std::uint32_t);
constexpr std::size_t kVolumeParamsOriginal = offsetof(clay_volume_params, beta) + sizeof(float);
constexpr std::size_t kRelaxParamsOriginal = offsetof(clay_relax_params, falloff) + sizeof(float);
constexpr std::size_t kTopologicalMoveParamsOriginal =
    offsetof(clay_topological_move_params, ease) + sizeof(std::int32_t);
constexpr std::size_t kFlattenParamsOriginal =
    offsetof(clay_flatten_params, falloff) + sizeof(float);
constexpr std::size_t kMaskExtrudeParamsOriginal =
    offsetof(clay_mask_extrude_params, band) + sizeof(float);
constexpr std::size_t kGizmoCageOriginal =
    offsetof(clay_gizmo_cage, nz) + sizeof(std::int32_t);
constexpr std::size_t kMoveParamsOriginal =
    offsetof(clay_move_params, front_only) + sizeof(std::int32_t);
constexpr std::size_t kMagnifyParamsOriginal =
    offsetof(clay_magnify_params, ease) + sizeof(std::int32_t);
constexpr std::size_t kImportBudgetOriginal =
    offsetof(clay_import_budget, max_triangles) + sizeof(std::uint64_t);
constexpr std::size_t kMeshLayerDescOriginal =
    offsetof(clay_mesh_layer_desc, import_scale) + sizeof(float);
constexpr std::size_t kGridQueryOriginal =
    offsetof(clay_grid_query, dims) + 3 * sizeof(std::int32_t);
constexpr std::size_t kBrickConfigOriginal =
    offsetof(clay_brick_config, memory_budget) + sizeof(std::uint64_t);
constexpr std::size_t kBrickStatsOriginal =
    offsetof(clay_brick_stats, memory_budget) + sizeof(std::uint64_t);
constexpr std::size_t kResumeStatsOriginal =
    offsetof(clay_resume_stats, refilled_bricks) + sizeof(std::uint64_t);
constexpr std::size_t kBrickMeshParamsOriginal =
    offsetof(clay_brick_mesh_params, gradient_eps) + sizeof(float);
constexpr std::size_t kVertexLayoutOriginal =
    offsetof(clay_vertex_layout, uv_offset) + sizeof(std::int32_t);
// The meshing fields alone. The count controls sit past this, so a caller who
// declares only the lattice gets the mesher with no search — which is the
// layout this descriptor would have had if the target had arrived later.
constexpr std::size_t kQuadParamsOriginal =
    offsetof(clay_quad_params, level) + sizeof(std::uint32_t);
constexpr std::size_t kQuadReportOriginal =
    offsetof(clay_quad_report, clamped) + sizeof(std::int32_t);
constexpr std::size_t kHistoryBytesOriginal =
    offsetof(clay_history_bytes, dropped_steps) + sizeof(std::uint64_t);
constexpr std::size_t kMemoryReportOriginal =
    offsetof(clay_memory_report, mask_count) + sizeof(std::uint64_t);

// One conversion, so the document-wide and per-layer paths cannot fill the
// struct differently — which is the kind of divergence a total-only test would
// never see. FILE SCOPE, above the first extern "C": a helper defined inside
// that block is what broke the macOS and Windows builds in #235, and GCC does
// not warn about it, so a green local build proves nothing.
inline clay_memory_report to_c_report(const io::MemoryReport& r) {
    clay_memory_report out{};
    out.edit_list = r.edit_list;
    out.voxel_content = r.voxel_content;
    out.mesh_layers = r.mesh_layers;
    out.masks = r.masks;
    out.voxel_sculpt_layers = r.voxel_sculpt_layers;
    out.history = r.history;
    out.passthrough = r.passthrough;
    out.transient = r.transient;
    out.total = r.total;
    out.voxel_layers = r.voxel_layers;
    out.mesh_layer_count = r.mesh_layer_count;
    out.mask_count = r.mask_count;
    return out;
}
constexpr std::size_t kMeasureParamsOriginal =
    offsetof(clay_measure_params, seed) + sizeof(std::uint32_t);
constexpr std::size_t kProjectionOriginal =
    offsetof(clay_projection, normal) + sizeof(float) * 3;
constexpr std::size_t kProgressOriginal = offsetof(clay_progress, total) + sizeof(std::uint64_t);
constexpr std::size_t kValidationReportOriginal =
    offsetof(clay_validation_report, euler_characteristic) + sizeof(std::int64_t);
constexpr std::size_t kDeviceDescOriginal =
    offsetof(clay_device_desc, queue_family) + sizeof(std::uint32_t);
constexpr std::size_t kDeviceBufferOriginal =
    offsetof(clay_device_buffer, size) + sizeof(std::uint64_t);

// Parameters each primitive takes, indexed by clay_prim (= the tape opcode).
// This is what the clay_prim comments document and what clay_item_create
// requires: a stroke's points and a lift's profile are out-of-line, so those
// entries count only the lift's own parameter.
// Loft takes 2: the half-depth and the ease. Its profiles are added
// separately, since a fixed block cannot carry a variable number of them.
constexpr int kPrimParams[] = {1, 3, 4, 4, 2, 7, 2, 3, 3, 3, 3, 1, 2, 1, 0, 1,
                               1, 4, 3, 3, 3, 4, 2, 3, 3, 1, 1, 1, 2, 1, 2, 2, 1, 0, 0};
static_assert(sizeof kPrimParams / sizeof kPrimParams[0] == kernel::ctape_prim_count);

constexpr int kProfileParams[] = {1, 2, 1, 1, 3, 2, 0};  // polygon: vertices instead
static_assert(sizeof kProfileParams / sizeof kProfileParams[0] == kernel::cprofile_polygon + 1);

// -1 marks a kind whose payload is not a flat float array, so the generic
// clay_item_add_deformer cannot take it and says so by name.
constexpr int kDeformParams[] = {1,  1, 4, 2, 2, 3,  9,  3,  3, 8, 8,
                                 10, 5, 5, 3, 3, -1, -1, -1, 9, -1};
static_assert(sizeof kDeformParams / sizeof kDeformParams[0] == kernel::cdeform_alpha + 1);

// A ceiling on an alpha's sample count, so a caller's bad multiply is refused
// rather than allocating whatever the product happened to be. 8192x8192 is far
// past any stamp an artist uses and still an unambiguous mistake below.
constexpr std::size_t kMaxAlphaSamples = 8192u * 8192u;

clay_result check_params(const char* what, const float* params, std::size_t count, int expected) {
    if (count != static_cast<std::size_t>(expected))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string(what) + " takes " + std::to_string(expected) +
                        " parameters, got " + std::to_string(count));
    if (expected > 0 && !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null parameters");
    return CLAY_OK;
}

clay_result check_ease(std::int32_t ease) {
    if (ease < 0 || ease >= CLAY_EASE_COUNT)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown easing curve: " + std::to_string(ease));
    return CLAY_OK;
}

// The one argument this boundary cannot check against the caller's memory is
// a count, and several entry points size a working buffer from one. A count
// that is not a count — a byte length where an element count belongs, a
// negative Swift Int widened to size_t — would allocate until the allocator
// gives up, and the throw could not be caught here: the core builds
// -fno-exceptions, so std::bad_alloc would reach std::terminate and take the
// host process down instead of returning an error. CLAY_MAX_BATCH is the
// documented ceiling; every count crossing the ABI passes through here.
static_assert(CLAY_MAX_BATCH <= 0xffffffffu);  // out-of-line counts serialize as u32

clay_result check_batch(const char* what, std::size_t count) {
    if (count > CLAY_MAX_BATCH)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("too many ") + what + ": " + std::to_string(count) +
                        " is above the batch limit of " + std::to_string(CLAY_MAX_BATCH));
    return CLAY_OK;
}

// Out-of-line payloads: the same ceiling, plus the pointer the count implies.
clay_result check_payload(const char* what, const float* data, std::size_t count) {
    if (count > 0 && !data) return fail(CLAY_ERROR_INVALID_ARGUMENT, std::string("null ") + what);
    return check_batch(what, count);
}

// -- voxel argument checks ---------------------------------------------------

// The engine stores a palette index in a byte, so the boundary widens it to
// int32_t and rejects what does not fit rather than truncating it silently.
clay_result check_palette_index(std::int32_t index, std::uint8_t* out) {
    if (index < 0 || index > 255)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "palette index must be in [0, 255], got " + std::to_string(index));
    *out = static_cast<std::uint8_t>(index);
    return CLAY_OK;
}

clay_result check_mirror_axes(std::int32_t axes, std::uint8_t* out) {
    constexpr std::int32_t kAll = CLAY_MIRROR_X | CLAY_MIRROR_Y | CLAY_MIRROR_Z;
    if (axes < 0 || (axes & ~kAll) != 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "mirror axes outside CLAY_MIRROR_X|Y|Z: " + std::to_string(axes));
    *out = static_cast<std::uint8_t>(axes);
    return CLAY_OK;
}

// The brush descriptor, checked in the order the Python bindings check it —
// size, then shape, then falloff — so the same mistake reports the same way
// through both bindings. strength comes last because they do not check it at
// all: it is passed through untouched, so a stamp lands on exactly the cells
// pyclay's does, and the one thing this boundary adds is refusing a strength
// that is not > 0. That value covers nothing, which no caller means to ask
// for and which is what a zero-initialized descriptor would otherwise say.
// Defined with the mask entry points below; a brush descriptor may name a
// mask, so the two resolve together.
clay_result resolve_mask(const clay_mask* mask, voxel::MaskField** out);

constexpr std::size_t kRepairReportOriginal =
    offsetof(clay_repair_report, airtight) + sizeof(std::int32_t);

constexpr std::size_t kTubeParamsOriginal =
    offsetof(clay_tube_params, blend_k) + sizeof(float);
constexpr std::size_t kCutDescOriginal =
    offsetof(clay_cut_desc, far_extent) + sizeof(float);

constexpr std::size_t kStrokePresetOriginal =
    offsetof(clay_stroke_preset, accumulation) + sizeof(std::int32_t);

// A preset descriptor as the engine's type. The two values the engine cannot
// make sense of — a radius or spacing that is not > 0 — are refused here
// rather than clamped, on the same footing as a brush strength that is not
// > 0: they describe no stroke, and a zero-initialized descriptor is what
// they usually mean.
clay_result read_preset(const clay_stroke_preset* src, brush::StrokePreset* out) {
    if (!src) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null stroke preset");
    clay_stroke_preset d;
    clay_result r = read_desc(src, kStrokePresetOriginal, &d);
    if (r != CLAY_OK) return r;
    if (!(d.radius > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "preset radius must be > 0");
    if (!(d.spacing > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "preset spacing must be > 0");
    if (d.accumulation < 0 ||
        d.accumulation > static_cast<std::int32_t>(brush::Accumulation::Clamped))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown accumulation: " + std::to_string(d.accumulation));
    out->radius = d.radius;
    out->spacing = d.spacing;
    out->strength = d.strength;
    out->pressure.size = d.pressure_size;
    out->pressure.strength = d.pressure_strength;
    out->pressure.curve = d.pressure_curve;
    out->jitter_position = d.jitter_position;
    out->jitter_size = d.jitter_size;
    out->jitter_rotation = d.jitter_rotation;
    out->seed = d.seed;
    out->rotate_along_stroke = d.rotate_along_stroke != 0;
    out->taper_start = d.taper_start;
    out->taper_end = d.taper_end;
    out->steady = d.steady;
    out->accumulation = static_cast<brush::Accumulation>(d.accumulation);
    return CLAY_OK;
}

// Samples arrive packed as count*5 floats, matching clay_stroke_sample.
// The samples alone, without a preset. Split out because a BRUSH preset already
// carries its stroke half, so that path has no separate preset to read.
clay_result read_samples(const float* samples_xyzpt, std::size_t sample_count,
                         std::vector<brush::StrokeSample>* out_samples) {
    if (sample_count > 0 && !samples_xyzpt)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null stroke samples");
    clay_result r = check_batch("stroke samples", sample_count);
    if (r != CLAY_OK) return r;
    out_samples->resize(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const float* row = samples_xyzpt + i * 5;
        (*out_samples)[i].position = kernel::cf3(row[0], row[1], row[2]);
        (*out_samples)[i].pressure = row[3];
        (*out_samples)[i].tilt = row[4];
    }
    return CLAY_OK;
}

clay_result read_stroke(const float* samples_xyzpt, std::size_t sample_count,
                        const clay_stroke_preset* preset,
                        std::vector<brush::StrokeSample>* out_samples,
                        brush::StrokePreset* out_preset) {
    clay_result r = read_preset(preset, out_preset);
    if (r != CLAY_OK) return r;
    return read_samples(samples_xyzpt, sample_count, out_samples);
}

// The same, from the struct array that carries the channels a tablet reports.
clay_result read_stroke_full(const clay_stroke_sample_full* samples, std::size_t sample_count,
                             const clay_stroke_preset* preset,
                             std::vector<brush::StrokeSample>* out_samples,
                             brush::StrokePreset* out_preset) {
    if (sample_count > 0 && !samples)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null stroke samples");
    clay_result r = check_batch("stroke samples", sample_count);
    if (r != CLAY_OK) return r;
    r = read_preset(preset, out_preset);
    if (r != CLAY_OK) return r;
    out_samples->resize(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const clay_stroke_sample_full& in = samples[i];
        brush::StrokeSample& out = (*out_samples)[i];
        out.position = kernel::cf3(in.position[0], in.position[1], in.position[2]);
        out.pressure = in.pressure;
        out.tilt = in.tilt;
        out.azimuth = in.azimuth;
        out.velocity = in.velocity;
        out.timestamp = in.timestamp;
    }
    return CLAY_OK;
}

clay_stamp to_c_stamp(const brush::Stamp& s) {
    clay_stamp out{};
    out.position[0] = s.position.x;
    out.position[1] = s.position.y;
    out.position[2] = s.position.z;
    out.radius = s.radius;
    out.strength = s.strength;
    out.rotation[0] = s.rotation.x;
    out.rotation[1] = s.rotation.y;
    out.rotation[2] = s.rotation.z;
    out.rotation[3] = s.rotation.w;
    out.along = s.along;
    return out;
}

// The size-query pattern for a byte payload.
clay_result write_sized(const std::uint8_t* data, std::size_t size, std::uint8_t* out_data,
                        std::size_t* count, const char* what) {
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    if (out_data && *count < size) {
        *count = size;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    std::string("the ") + what + " needs " + std::to_string(size) + " bytes");
    }
    if (out_data && size > 0) std::memcpy(out_data, data, size);
    *count = size;
    return CLAY_OK;
}

clay_result read_brush(const clay_brush_params* src, voxel::BrushParams* out) {
    clay_brush_params b;
    clay_result r = read_desc(src, kBrushParamsOriginal, &b);
    if (r != CLAY_OK) return r;
    if (b.size <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "brush size must be > 0");
    if (!brush_shape_is_known(b.shape))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown brush shape: " + std::to_string(b.shape));
    if (!brush_falloff_is_known(b.falloff))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown brush falloff: " + std::to_string(b.falloff));
    if (!(b.strength > 0.0f))  // also rejects NaN
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "brush strength must be > 0");
    out->size = b.size;
    out->shape = static_cast<voxel::BrushShape>(b.shape);
    out->falloff = static_cast<voxel::BrushFalloff>(b.falloff);
    out->strength = b.strength;
    out->seed = b.seed;
    out->mask = nullptr;
    if (b.mask) {
        voxel::MaskField* m = nullptr;
        clay_result mr = resolve_mask(b.mask, &m);
        if (mr != CLAY_OK) return mr;
        out->mask = m;
    }
    return CLAY_OK;
}

// The parameter block is the engine's own, but three of the engine's Prim
// constructors do work on the way in, and a C author must land on the same
// values pyclay does. Prim::plane normalizes its normal — an unnormalized one
// scales the whole field, and since a plane is never culled it would corrupt
// every other item — so this normalizes too. The angle primitives store an
// angle as (sin, cos); the pair is checked rather than computed, because the
// parameter block is what the header documents.
clay_result canonical_prim_params(std::int32_t prim, float* p) {
    if (prim == CLAY_PRIM_PLANE) {
        kernel::cfloat3 n = kernel::cf3(p[0], p[1], p[2]);
        if (kernel::cdot2(n) <= 0.0f)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "a plane needs a non-zero normal");
        n = kernel::cnormalize(n);
        p[0] = n.x;
        p[1] = n.y;
        p[2] = n.z;
    } else if (prim == CLAY_PRIM_CAPPED_TORUS || prim == CLAY_PRIM_CONE ||
               prim == CLAY_PRIM_SOLID_ANGLE) {
        if (std::fabs(p[0] * p[0] + p[1] * p[1] - 1.0f) > 1e-3f)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "the first two parameters are an angle's sine and cosine");
    }
    return CLAY_OK;
}

clay_result make_deformer(std::int32_t kind, const float* p, scene::Deformer* out) {
    if (kind == CLAY_DEFORM_TWIST) {
        *out = scene::Deformer::twist(p[0]);
    } else if (kind == CLAY_DEFORM_BEND) {
        *out = scene::Deformer::bend(p[0]);
    } else if (kind == CLAY_DEFORM_DISPLACE) {
        *out = scene::Deformer::displace(p[0], p[1]);
    } else if (kind == CLAY_DEFORM_BEND_LINEAR) {
        kernel::cfloat3 a = kernel::cf3(p[0], p[1], p[2]);
        kernel::cfloat3 b = kernel::cf3(p[3], p[4], p[5]);
        if (!(kernel::cdot2(b - a) > 0.0f))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "bend_linear needs a != b");
        *out = scene::Deformer::bend_linear(a, b, kernel::cf3(p[6], p[7], p[8]));
    } else if (kind == CLAY_DEFORM_BEND_RADIAL) {
        if (p[0] == p[1]) return fail(CLAY_ERROR_INVALID_ARGUMENT, "bend_radial needs r0 != r1");
        *out = scene::Deformer::bend_radial(p[0], p[1], p[2]);
    } else if (kind == CLAY_DEFORM_TWIST_RANGE) {
        // A zero-width range is a division by zero in the ramp, not a twist
        // that happens instantly — refused for the reason bend_radial's band
        // is refused.
        if (p[1] == p[2])
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "twist_range needs y0 != y1");
        *out = scene::Deformer::twist_range(p[0], p[1], p[2]);
    } else if (kind == CLAY_DEFORM_BEND_RANGE) {
        if (p[1] == p[2])
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "bend_range needs x0 != x1");
        *out = scene::Deformer::bend_range(p[0], p[1], p[2]);
    } else if (kind == CLAY_DEFORM_GRAB) {
        if (!(p[3] > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "grab radius must be > 0");
        *out = scene::Deformer::grab(kernel::cf3(p[0], p[1], p[2]), p[3],
                                     kernel::cf3(p[4], p[5], p[6]), 0, p[7] != 0.0f);
    } else if (kind == CLAY_DEFORM_POSE_LINE) {
        kernel::cfloat3 a = kernel::cf3(p[0], p[1], p[2]);
        kernel::cfloat3 b = kernel::cf3(p[3], p[4], p[5]);
        if (!(kernel::cdot2(b - a) > 0.0f))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "pose_line needs a != b");
        *out = scene::Deformer::pose_line(a, b, kernel::cf3(p[6], p[7], p[8]), p[9]);
    } else if (kind == CLAY_DEFORM_BLOB) {
        if (!(p[3] > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "blob needs a radius > 0");
        *out = scene::Deformer::blob(kernel::cf3(p[0], p[1], p[2]), p[3], p[4], p[5],
                                     static_cast<int>(p[6]), p[7],
                                     static_cast<std::uint32_t>(p[8]));
    } else if (kind == CLAY_DEFORM_NOISE) {
        if (!(p[2] >= 1.0f))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "noise needs at least one octave");
        *out = scene::Deformer::noise(p[0], p[1], static_cast<int>(p[2]), p[3],
                                      static_cast<std::uint32_t>(p[4]));
    } else if (kind == CLAY_DEFORM_MAGNIFY) {
        if (!(p[3] > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "magnify radius must be > 0");
        *out = scene::Deformer::magnify(kernel::cf3(p[0], p[1], p[2]), p[3], p[4]);
    } else if (kind == CLAY_DEFORM_POSE) {
        if (!(p[3] > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "pose radius must be > 0");
        *out = scene::Deformer::pose(kernel::cf3(p[0], p[1], p[2]), p[3],
                                     kernel::cf3(p[4], p[5], p[6]), p[7]);
    } else if (kind == CLAY_DEFORM_ELONGATE_AXIS) {
        if (p[0] < 0.0f || p[1] < 0.0f || p[2] < 0.0f)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "elongate_axis half-extents must be >= 0");
        *out = scene::Deformer::elongate_axis(kernel::cf3(p[0], p[1], p[2]));
    } else if (kind == CLAY_DEFORM_ELONGATE) {
        if (p[0] < 0.0f || p[1] < 0.0f || p[2] < 0.0f)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "elongate half-extents must be >= 0");
        *out = scene::Deformer::elongate(kernel::cf3(p[0], p[1], p[2]));
    } else if (kind == CLAY_DEFORM_WRAP_AROUND) {
        // The interval fixes the cylinder radius, so a degenerate one has no
        // meaning rather than a degenerate result.
        if (p[0] == p[1])
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "wrap_around needs x0 != x1");
        *out = scene::Deformer::wrap_around(p[0], p[1]);
    } else {
        if (p[0] == p[1]) return fail(CLAY_ERROR_INVALID_ARGUMENT, "taper needs y1 != y0");
        if (p[2] <= 0.0f || p[3] <= 0.0f)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "taper scales must be > 0");
        *out = scene::Deformer::taper(p[0], p[1], p[2], p[3]);
    }
    return CLAY_OK;
}

// What the flat descriptor can express: every primitive whose parameters fit
// the params block, and every op that needs no parameters of its own.
clay_result validate_item_desc(const clay_item_desc& d) {
    if (!prim_is_known(d.prim)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown primitive type");
    scene::PrimType prim = static_cast<scene::PrimType>(d.prim);
    if (prim == scene::PrimType::Stroke || scene::prim_is_lift(prim) ||
        scene::prim_carries_profiles(prim) || scene::prim_is_volume(prim))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "primitive needs out-of-line data");
    if (!op_is_known(d.op)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown combine op");
    if (scene::op_is_transition(static_cast<scene::Op>(d.op)))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "transition ops need transition parameters");
    if (!blend_is_known(d.blend)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown blend");
    return CLAY_OK;
}

clay_result from_io(const io::IoStatus& s) {
    g_last_error = s.detail;
    switch (s.error) {
        case io::IoError::Ok: return CLAY_OK;
        case io::IoError::FileNotFound: return CLAY_ERROR_NOT_FOUND;
        case io::IoError::ForwardVersion: return CLAY_ERROR_FORWARD_VERSION;
        case io::IoError::BudgetExceeded: return CLAY_ERROR_BUDGET_EXCEEDED;
        case io::IoError::Unsupported: return CLAY_ERROR_UNSUPPORTED;
        default: return CLAY_ERROR_IO;
    }
}

}  // namespace

struct clay_document;

// A voxel grid handle is one of two things and says which: the owner of a
// standalone grid, or a borrow of the grid a document keeps for one layer.
// A borrow names the layer rather than pointing at it, so nothing caches a
// pointer into the document's layer map: the lookup is redone on every call.
// clay_document_remove_layer removes the LAYER and leaves the grid beside the
// document, so a borrow held across a removal still resolves. The miss below
// guards a grid that stops being held while a handle still names it — which
// nothing in this ABI does today, and which would be a use after free without
// it.
struct clay_voxel_grid {
    voxel::VoxelGrid* owned = nullptr;  // non-null: the caller destroys it
    clay_document* doc = nullptr;       // non-null: borrowed from a layer
    clay_layer_id layer = 0;

    // The dirty-chunk drain is capacity-in/count-out while the engine's drain
    // is all-or-nothing, so the keys the caller could not take live here until
    // it comes back for them. Held on the HANDLE rather than the grid because
    // it is a property of this conversation, not of the sculpt: a borrowed
    // handle is per layer and stable, so a host draining one layer in chunks
    // does not disturb another.
    std::vector<voxel::VoxelCoord> staged_dirty;
    std::size_t staged_head = 0;
    std::size_t staged_remaining() const { return staged_dirty.size() - staged_head; }
};

struct clay_mask {
    voxel::MaskField* owned = nullptr;  // non-null: the caller destroys it
    clay_document* doc = nullptr;       // non-null: borrowed from a layer
    clay_layer_id layer = 0;
    // One memoised gate bake per handle, so gating N items by one painted mask
    // pays for one measurement rather than N. Lives HERE rather than in a
    // table keyed by mask address: an address is reused after a free, and a
    // handle's memo dies with the handle.
    // `mutable` because gating takes the mask as const: a memo does not change
    // what the mask MEANS, only what it has already cost.
    mutable brush::GateBake gate_bake;
};

// A group handle is always a BORROW of the document's one lattice — there is no
// standalone form, unlike a mask or a grid. Deliberate: groups are per document
// by design, so a standalone one would name a region of nothing. The handle
// carries no id because the document has exactly one, and the lookup is redone
// on every call so nothing caches a pointer into the document.
struct clay_groups {
    clay_document* doc = nullptr;
};

// The same discriminator, one shape different: a standalone mesh is held by
// value because every producer in this file builds one in place, and `data` is
// Bytes the library owns, handed out and released. One type for every
// serialized payload, so a host learns the lifetime once rather than per
// format. Nothing here interprets the bytes.
struct clay_blob {
    std::vector<std::uint8_t> bytes;
};

// The token a host cancels an operation with. A thin owner around the engine
// type so the C handle has a stable identity of its own.
struct clay_cancel_token {
    parallel::CancelToken token;
};

// simply unused by a borrow. Declared above clay_document because the document
// keeps a map of these by value.
struct clay_mesh {
    mesh::Mesh data;               // the owned mesh; empty on a borrow
    clay_document* doc = nullptr;  // non-null: borrowed from a mesh layer
    clay_layer_id layer = 0;

    // How this mesh was quad-meshed, when it was. Held on the HANDLE rather
    // than on mesh::Mesh because it describes a CALL and not a surface: a mesh
    // that travelled through a file, a document layer or a concatenation was
    // not produced by one, and clay_mesh_quad_report refuses it rather than
    // answering with zeroes a host cannot tell from a search that found
    // nothing.
    struct QuadProvenance {
        mesh::QuadFit fit;
        std::uint64_t target = 0;
    };
    std::optional<QuadProvenance> quad_provenance;
};

struct clay_document {
    io::ClaySpaceDoc doc;
    // Opt-in undo. Null means off, and a document that never enables it
    // behaves exactly as it did before the feature existed.
    //
    // A session::History rather than a scene::UndoStack since
    // unify-the-undo-history: the same opt-in, the same entry points, and now
    // spanning voxel grids and mesh layers as well as the edit list. It WRAPS
    // an UndoStack, so every command path below is unchanged.
    std::unique_ptr<session::History> undo;

    // The resolvers the history takes, because session sits BELOW io and
    // cannot name the document that owns the three representations.
    session::History::GridFor grid_for() {
        return [this](scene::LayerId id) -> voxel::VoxelGrid* {
            auto it = doc.voxel_layers.find(id);
            return it == doc.voxel_layers.end() ? nullptr : &it->second;
        };
    }
    session::History::MaskFor mask_for() {
        return [this](scene::LayerId id) -> voxel::MaskField* {
            auto it = doc.masks.find(id);
            return it == doc.masks.end() ? nullptr : &it->second;
        };
    }
    session::History::MeshFor mesh_for() {
        return [this](scene::LayerId id) -> mesh::Mesh* {
            auto it = doc.mesh_layers.find(id);
            return it == doc.mesh_layers.end() ? nullptr : &it->second;
        };
    }
    // Borrowed handles are the document's, one per layer, handed back by
    // address: repeated lookups return the same handle, nothing leaks, and
    // std::map keeps the addresses stable as more layers arrive.
    std::map<clay_layer_id, clay_voxel_grid> voxel_handles;
    std::map<clay_layer_id, clay_mask> mask_handles;
    std::map<clay_layer_id, clay_mesh> mesh_handles;

    // -- compiled tape cache -------------------------------------------------
    //
    // Every read of the field used to recompile the whole document: eval,
    // gradients, safe step scale, meshing, and picking each paid O(document)
    // before doing any work. On the interactive path that dominates — a sculpt
    // grows a node per brush stamp, so the cost of LOOKING grew with everything
    // the artist had already drawn.
    //
    // Keyed on a revision the mutating entry points bump through touch(). The
    // dangerous direction is under-bumping (a stale field, silently), so the
    // rule is to bump on anything that could possibly matter: an unnecessary
    // bump costs one recompile, which is exactly the old behaviour.
    //
    // Handed out as a shared_ptr rather than a reference because two threads
    // calling clay_eval_points on one document worked before this cache existed
    // — compile_document takes a const Document& and returned a fresh tape — and
    // that must keep working. A caller holding a snapshot is unaffected by
    // another thread invalidating and rebuilding.
    std::atomic<std::uint64_t> revision{1};

    // Per mesh layer, bumped only when its triangles are REPLACED wholesale.
    // See mesh_layer_revision_of for why a sculpt deliberately does not bump it
    // and why the pointer and count checks that came before it are not enough.
    std::map<clay_layer_id, std::uint64_t> mesh_geometry_revision;

    // The general invalidation: everything the cache knows becomes stale.
    // This stays the DEFAULT, and the append form below is the exception you
    // have to ask for by name — a tape rebuilt from a prefix that has in fact
    // moved is silent, so any entry point that does not positively know it
    // appended must land here.
    void touch() {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        forget_appends();
        // A seed is only usable across APPENDS, so one kept past any other
        // edit is memory nothing can ever read again.
        forget_resume();
        // The general funnel does not know what moved, so it must assume the
        // evaluation order did: an ordinal held against the old order is a
        // wrong boundary against the new one, silently.
        ++structure_revision_;
        revision.fetch_add(1, std::memory_order_relaxed);
    }

    // The narrow one: `node` was appended at the tail of `layer`'s root list
    // and nothing else changed, so a cache built at any revision the log
    // covers can be brought forward instead of rebuilt.
    //
    // ONE LOG, READ BY SEVERAL CACHES, and that is a change from how it began.
    // It used to be reset when a reader absorbed it, which made it
    // single-consumer: whichever cache the host asked for first spent it, and
    // the others rebuilt. #309 worked around that by giving the cull index a
    // second copy, and a third reader would have wanted a third.
    //
    // Instead nothing resets it on a read. An append bumps the revision by
    // exactly one, so entry i IS the append that took the document from
    // `append_base_ + i` to `append_base_ + i + 1` -- which means a reader
    // sitting at revision R wants entries from `R - append_base_` onward, and
    // several readers at different revisions can each take their own tail of
    // the same log. `appends_since` is that lookup.
    void touch_appended(scene::LayerId layer, scene::NodeId node) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const std::uint64_t now = revision.load(std::memory_order_relaxed);
        // The log is only usable while it is CONTIGUOUS: it describes the
        // appends made on top of append_base_, and every revision between
        // that and now has to be one of them. A general touch() in between
        // cleared it, and a different layer starts a new one.
        if (!append_valid_ || append_layer_ != layer || append_at_ != now) {
            append_valid_ = true;
            append_layer_ = layer;
            append_base_ = now;
            append_log_.clear();
        }
        append_log_.push_back(node);
        // An append IS an order change: it grows the root list, so every
        // ordinal taken before it counts a different tail. This does not gate
        // the append fast path -- plan_resume never reads structure_revision_
        // -- it only retires prefix seeds, which a stamp landing mid-drag must
        // do: their boundaries were counted against the shorter list.
        ++structure_revision_;
        revision.fetch_add(1, std::memory_order_relaxed);
        append_at_ = revision.load(std::memory_order_relaxed);
    }

    // -- resumable brick refill (#306) --------------------------------------
    //
    // A dab re-evaluates every surviving item of the edit list over a dirty
    // brick's samples, so its cost follows what the artist has already
    // sculpted: 18 ms at 50,000 items against a 4.17 ms budget. #308 built the
    // way out -- compile only the appended items and run them onto the value
    // the rest produced -- and this is the value. What a refill hands back IS
    // that accumulator, exact and in float32, so keeping it is all that is
    // needed to make the next dab cost what the dab adds.
    //
    // NOT the brick cache's own samples: those are fp16 clamped to the band,
    // and its Inside/Outside bricks hold nothing at all, which is exactly
    // where a growing dab lands.
    //
    // WHAT A SEED DESCRIBES, not merely where it sits (#349). A brick
    // coordinate alone is not an identity: the same (x, y, z) names a
    // different set of samples under a different lattice, and a different
    // FIELD under a different band, because a brick's tape is culled against
    // `request_brick_box(req).dilated(req.band)` -- a smaller band drops items
    // a larger one keeps. Both are properties of the CACHE that asked, fixed
    // for its lifetime, so they belong in the key rather than in a gate:
    // keying on them lets two caches over one document each hold their own
    // seeds instead of evicting each other's on every call.
    //
    // The cull PAD is the other term of that dilation and is deliberately NOT
    // here. It is a property of the document -- a global maximum that an
    // append can raise -- so it moves under a single cache, and a key would
    // strand the old entry rather than replace it. It is gated in `seed_for`
    // instead.
    struct ResumeKey {
        std::int32_t x = 0, y = 0, z = 0;
        std::int32_t dims[3] = {0, 0, 0};
        float spacing = 0.0f;
        float band = 0.0f;
        // BY BITS, not by float ==. An unordered_map requires equal keys to
        // hash equal, and -0.0 == 0.0 while their bits differ. Comparing bits
        // both ways keeps that invariant; the cost is that a caller passing
        // -0.0 gets its own entry, which no valid spacing or band is.
        static std::uint32_t bits(float f) {
            std::uint32_t u = 0;
            std::memcpy(&u, &f, sizeof u);
            return u;
        }
        bool operator==(const ResumeKey& o) const {
            return x == o.x && y == o.y && z == o.z && dims[0] == o.dims[0] &&
                   dims[1] == o.dims[1] && dims[2] == o.dims[2] &&
                   bits(spacing) == bits(o.spacing) && bits(band) == bits(o.band);
        }
    };
    static ResumeKey resume_key(const clay_brick_request& request) {
        ResumeKey k;
        k.x = request.key[0];
        k.y = request.key[1];
        k.z = request.key[2];
        k.dims[0] = request.dims[0];
        k.dims[1] = request.dims[1];
        k.dims[2] = request.dims[2];
        k.spacing = request.spacing;
        k.band = request.band;
        return k;
    }
    struct ResumeKeyHash {
        std::size_t operator()(const ResumeKey& k) const {
            std::size_t h = static_cast<std::size_t>(static_cast<std::uint32_t>(k.x));
            auto mix = [&h](std::uint32_t v) { h = h * 0x9e3779b97f4a7c15ull + v; };
            mix(static_cast<std::uint32_t>(k.y));
            mix(static_cast<std::uint32_t>(k.z));
            mix(static_cast<std::uint32_t>(k.dims[0]));
            mix(static_cast<std::uint32_t>(k.dims[1]));
            mix(static_cast<std::uint32_t>(k.dims[2]));
            mix(ResumeKey::bits(k.spacing));
            mix(ResumeKey::bits(k.band));
            return h;
        }
    };
    // Frontier sentinels (#360). `dirty_from` counts ROOT-LIST ordinals of the
    // active layer -- boundary N means N semantic items already folded, so an
    // edit inside root i dirties from i. kFrontierClean means "no semantic
    // dirtying pending" and is the identity of the min-merge below.
    // kFrontierDrop is the touch_region_locked argument meaning "no frontier --
    // drop what the bound reaches", i.e. the legacy behaviour, and no real
    // ordinal can collide with either: both sit above any root list a document
    // can hold.
    static constexpr std::uint32_t kFrontierClean = 0xFFFFFFFFu;
    static constexpr std::uint32_t kFrontierDrop = 0xFFFFFFFFu - 1u;

    struct ResumeEntry {
        std::uint64_t revision = 0;
        // Whether that brick's culled prefix produced a value at all. A brick
        // whose whole chain the cull dropped has NO accumulator, and a suffix
        // compiled as though it had one would combine against far-outside
        // instead of seeding the chain. Read off the values: an empty tape
        // evaluates to CLAY_TAPE_FAR everywhere, and a non-empty one that
        // happens to as well only makes this refuse.
        bool had_acc = false;
        float pad = 0.0f;  // the cull pad the values were computed under
        // The ACTIVE layer's chain at this brick's lattice -- what a suffix
        // continues. For a document with one visible SDF layer that IS the
        // whole field, which is why the single-layer case needs nothing else.
        std::vector<float> values;
        std::vector<float> colors;  // empty when that refill carried none
        // -- the group half: a resume that picks up INSIDE a group ----------
        //
        // `values` above is THE FIELD and keeps that meaning, because readers
        // depend on it: a brick still at the current revision is answered
        // straight out of it, with no walk at all. A stack is NOT the field --
        // the field is what the frames' combines produce from it -- so putting
        // one in `values` hands that reader the bottom of a stack as the
        // answer. Measured, before this was separated: 0.28 against a cold
        // document, where the root-list control is exact.
        //
        // So the stack lives beside the field rather than in place of it.
        // Empty is "this brick can only resume at a root list", which is every
        // brick until a group append gives it one.
        std::vector<float> stack;
        std::vector<float> stack_colors;
        std::uint32_t stack_levels = 0;
        // The checkpoint's own, and per brick for the same reason `frames` is:
        // false where the chain the checkpoint sits in had produced nothing,
        // which a tail group that was EMPTY when the seed was taken leaves
        // behind. Part of the stack's shape, so the level check has to see it.
        bool layer_have_acc = true;
        // The frames the stack was taken with. Per BRICK, because a frame's
        // `emits` depends on whether anything before the group survived THIS
        // brick's cull -- the same reason `had_acc` is per entry.
        std::vector<scene::TapeCheckpointFrame> frames;
        // The visible SDF layers BEFORE the active one, hard-unioned. Empty
        // when there are none. Static across a stroke -- only the active layer
        // moves -- so it is stored once and carried forward untouched.
        std::vector<float> below;
        std::vector<float> below_colors;
        // This entry's node in resume_order_. Held on the entry so that
        // dropping it -- eviction, or a region invalidation that reaches it --
        // is a list erase and not a scan of the order (#346).
        std::list<ResumeKey>::iterator lru;
        // -- the frontier half (#360): ONE extra checkpoint, not a ladder ----
        //
        // A continuing Move drag replaces the tail deformer(s) every frame, so
        // nothing an append can describe changed -- and the append log dies on
        // the first non-append edit anyway. What DOES survive such a frame is
        // everything before the dragged nodes, so the entry can carry a second
        // value: the active chain folded through the first `prefix_boundary`
        // roots, at this brick's lattice, under `prefix_pad`. A dirty entry
        // (`dirty_from` != kFrontierClean) whose prefix sits at or before the
        // dirty frontier is refilled by folding only roots[boundary..end) onto
        // it -- the seeded walk the append path already runs.
        //
        // One prefix, deliberately: a ladder of checkpoints per entry would
        // scale per-brick bytes with history, and the drag workload only ever
        // wants the boundary in front of its own deformers (out of scope by
        // the issue). `dirty_from` is MIN-merged, never overwritten: two edits
        // at two ordinals dirty from the earlier one, and an overwrite would
        // resurrect a prefix the first edit already invalidated.
        //
        // `prefix_structure` tags the structure_revision_ the ordinals were
        // taken under; 0 means no prefix recorded. Ordinals are positions and
        // positions do not survive a reorder, so a mismatch is a full refusal,
        // never a remap.
        std::uint32_t dirty_from = kFrontierClean;  // min-merged; kFrontierClean = none
        std::uint32_t prefix_boundary = 0;          // roots folded into prefix_values
        bool prefix_had_acc = false;
        float prefix_pad = 0.0f;
        std::uint64_t prefix_structure = 0;  // 0 = no prefix recorded
        std::vector<float> prefix_values;    // roots[0..prefix_boundary) at this lattice
        std::vector<float> prefix_colors;    // empty when the entry is distance-only
    };

    // A BYTE budget, not a brick count. With colour a brick carries four times
    // the floats -- one value plus three channels a sample -- so a count would
    // mean two very different ceilings depending on what the host asked for.
    // Measured through clay_document_resume_stats over one visible SDF layer, a
    // dim-8 entry is 2,048 B distance-only and 8,192 B coloured, so 64 MB is
    // 32,768 of the first or 8,192 of the second -- a stroke's working set
    // either way. A layer beneath the active one doubles both: the entry then
    // carries that half's lattice as well.
    static constexpr std::size_t kResumeBytes = 64u << 20;

    // CAPACITY, not size, and that is the deliberate answer to #346's third
    // question. `store_active` clears `colors` for a request that carries none,
    // and a cleared vector keeps its buffer -- so a size-based sum reports
    // memory the entry still holds as free, and the 64 MB ceiling becomes
    // optimistic by an amount the host cannot see. Counting capacity makes it a
    // true bound on what the store has allocated. The alternative,
    // `shrink_to_fit` on every clear, would free and reallocate one lattice per
    // brick per dab on the path this whole cache exists to keep cheap.
    //
    // Exact because every mutation is bracketed: entry_bytes is subtracted
    // before the vectors are touched and added back after, so the number taken
    // out is always the one that was put in.
    static std::size_t entry_bytes(const ResumeEntry& e) {
        return (e.values.capacity() + e.colors.capacity() + e.below.capacity() +
                e.below_colors.capacity() + e.prefix_values.capacity() +
                e.prefix_colors.capacity() + e.stack.capacity() + e.stack_colors.capacity()) *
                   sizeof(float) +
               e.frames.capacity() * sizeof(scene::TapeCheckpointFrame);
    }

    // Move an entry to the most-recently-used end. O(1), and it neither
    // invalidates `e.lru` nor the pointers a caller may be holding into the
    // entry's vectors -- splice relinks a node, it does not move one.
    void touch_lru(ResumeEntry& e) const {
        resume_order_.splice(resume_order_.end(), resume_order_, e.lru);
    }

    // Down to the budget, oldest USE first. Never the most recently used entry:
    // a budget smaller than one brick would otherwise evict what the caller
    // just stored, which is the brick the next dab is about to read.
    void evict_locked() const {
        while (resume_bytes_ > resume_budget_ && resume_order_.size() > 1) {
            auto old = resume_.find(resume_order_.front());
            resume_order_.pop_front();
            if (old == resume_.end()) continue;  // the invariant says unreachable
            resume_bytes_ -= entry_bytes(old->second);
            resume_.erase(old);
        }
    }

    void forget_resume() const {
        resume_.clear();
        resume_order_.clear();
        resume_bytes_ = 0;
    }

    // What a resumable refill needs, decided under one lock so the answer
    // cannot be overtaken between the parts of it.
    struct ResumePlan {
        bool usable = false;
        bool has_below = false;
        scene::LayerId active = 0;
        std::uint64_t now = 0;
        std::vector<scene::NodeId> appended;  // what to compile as the suffix
        scene::TapeCheckpoint checkpoint;
    };

    // The suffix that takes every stored seed forward, or `usable = false`.
    //
    // The checkpoint is BUILT here rather than borrowed from the cached tape.
    // A brick refill never reads the tape, so `tape_checkpoint_` is cold in
    // exactly the case this exists for -- which made the fast path unreachable
    // until a mutation test showed it never fired.
    //
    // What `compile_layer_suffix` reads from a checkpoint is the layer, and
    // whether an accumulator is on the stack: the byte lengths are for
    // `compile_document_append`, which copies a prefix, and this does not.
    // Both are stated rather than derived, and gated so the statement is true:
    // ONE visible SDF layer, so nothing is underneath (`doc_have_acc` false),
    // and every seed recorded a prefix that produced a value
    // (`layer_have_acc` true, checked per brick in `seed_for`).
    ResumePlan plan_resume(std::uint64_t seed_revision) const {
        ResumePlan p;
        p.now = revision.load(std::memory_order_relaxed);
        if (seed_revision == 0 || seed_revision == p.now) return p;
        // The LAST visible SDF layer, which is the one an append extends. The
        // layers beneath it are held as their own value and folded in
        // afterwards, so more than one is no longer a reason to refuse.
        const scene::Layer* active = nullptr;
        int visible = 0;
        for (const scene::Layer& l : doc.document.layers) {
            if (!l.visible || l.kind != scene::LayerKind::Sdf || !l.sdf) continue;
            active = &l;
            ++visible;
        }
        if (!active) return p;
        // Both are properties of the DOCUMENT rather than of any append, and
        // callers probe this plan for `has_below` alone, so they are set before
        // anything below can decline.
        p.active = active->id;
        p.has_below = visible > 1;
        // THE APPENDS HAVE TO HAVE GONE TO THE LAYER THE SUFFIX WOULD EXTEND.
        //
        // Without this the log is trusted whatever layer it describes, and the
        // only thing left standing between that and a wrong field is
        // `compile_layer_suffix`'s check that `appended` is the tail of the
        // active layer's roots -- which compares NodeIds, and a NodeId is only
        // meaningful inside one SdfContent. Every layer's ids start at 1, so an
        // append to a lower layer whose new id happens to equal the active
        // layer's last root PASSES that check, and the suffix then folds the
        // active layer's own last node onto the seed a second time while the
        // dab that was actually made is never evaluated at all. The brick is
        // marked answered, so the full path that would have caught it never
        // runs, and `store_active` keeps the wrong value as the next seed.
        //
        // Measured on a two-layer document sculpted on the lower layer: up to
        // 0.49 in distance, ~10 cells at a 0.05 voxel, silently.
        // `touch_appended` already records the layer; this is only reading it.
        if (!append_valid_ || append_layer_ != active->id) return p;
        p.appended = appends_since(seed_revision, p.now);
        if (p.appended.empty()) return p;
        p.checkpoint = scene::TapeCheckpoint{};
        p.checkpoint.valid = true;
        p.checkpoint.layer = active->id;
        p.checkpoint.layer_have_acc = true;
        // FALSE even when layers sit beneath, so the suffix emits no union: the
        // refill holds that value separately and folds it in itself, with the
        // same hard Add the whole-document compile emits between layers.
        p.checkpoint.doc_have_acc = false;
        p.usable = true;
        return p;
    }

    // The frontier analogue of plan_resume (#360): the suffix that carries a
    // PREFIX seed at `boundary` forward. No append log involved -- the proof
    // that roots[0..boundary) still means what it meant when the prefix was
    // evaluated is structure_revision_ equality plus boundary <= dirty_from,
    // both checked per brick in frontier_seed_for. This is the same shape of
    // guarantee the layer gate above gives the append path: the checkpoint's
    // statements are made true elsewhere and only STATED here. Caller holds
    // cache_mutex_.
    ResumePlan plan_frontier(std::uint32_t boundary) const {
        ResumePlan p;
        p.now = revision.load(std::memory_order_relaxed);
        const scene::Layer* active = nullptr;
        int visible = 0;
        for (const scene::Layer& l : doc.document.layers) {
            if (!l.visible || l.kind != scene::LayerKind::Sdf || !l.sdf) continue;
            active = &l;
            ++visible;
        }
        if (!active) return p;
        p.active = active->id;
        p.has_below = visible > 1;
        const std::vector<scene::NodeId>& roots = active->sdf->roots;
        // Boundary 0 would be an empty prefix -- no accumulator, which the
        // layer_have_acc statement below could not honestly make -- and a
        // boundary at or past the end leaves no suffix to compile.
        if (boundary == 0 || static_cast<std::size_t>(boundary) >= roots.size()) return p;
        p.appended.assign(roots.begin() + static_cast<std::ptrdiff_t>(boundary), roots.end());
        p.checkpoint = scene::TapeCheckpoint{};
        p.checkpoint.valid = true;
        p.checkpoint.layer = active->id;
        p.checkpoint.layer_have_acc = true;
        // FALSE even when layers sit beneath, so the suffix emits no union: the
        // refill holds that value separately and folds it in itself, with the
        // same hard Add the whole-document compile emits between layers.
        p.checkpoint.doc_have_acc = false;
        p.usable = true;
        return p;
    }

    // The entry that can serve this request's LATTICE, or null. Shape only --
    // what the stored floats mean is the same question for every caller, and
    // getting it wrong hands back a buffer of the wrong length.
    //
    // Deliberately NOT a revision test. A brick's seed is usable at whatever
    // revision it was taken; how far it has to be carried forward is that
    // brick's own business, decided in `seed_for`.
    const ResumeEntry* shaped_entry(const clay_brick_request& request, std::size_t per,
                                    bool want_colour, bool want_below) const {
        auto it = resume_.find(resume_key(request));
        if (it == resume_.end()) return nullptr;
        const ResumeEntry& e = it->second;
        // The key already fixes the lattice, so this holds by construction --
        // kept because it is what makes the `per`-length reads of `values` in
        // `seed_for` safe by inspection rather than by an argument about a
        // caller two frames up.
        if (e.values.size() != per) return nullptr;
        // A colour asked for is a colour that has to have been kept: continuing
        // a coloured fold from a distance alone folds every combine against
        // black. The same for the layers beneath: a document that has them and
        // a seed that does not are describing different fields.
        if (want_colour && e.colors.size() != per * 3) return nullptr;
        if (want_below != !e.below.empty()) return nullptr;
        if (want_below && want_colour && e.below_colors.size() != per * 3) return nullptr;
        return &e;
    }

    // The revision a brick's seed sits at, or 0 when it has none this request
    // can use. PER BRICK, and that is the point of it.
    //
    // It used to be one revision for the whole batch, and a batch that did not
    // agree on one took the full path entire. That reads as a rare case and is
    // in fact the common one: `touch_appended` does not re-stamp seeds, and a
    // refill only re-stamps the bricks it filled, so a dirty window that MOVES
    // -- any stroke that is not standing still -- mixes the revision it stamped
    // last dab with whatever the newly entered ground was stamped at. One brick
    // the stroke had not reached before, or one evicted by the byte budget, did
    // the same. So the fast path #306 exists for fired only while the window
    // stood still, and every other dab paid the 18 ms it was built to remove.
    //
    // Nothing about the arithmetic wanted that. The suffix is compiled and
    // evaluated per brick against that brick's own cull region either way;
    // only the PLAN was shared, and a plan is cheap enough to keep one per
    // distinct revision. Bricks that cannot be served fall into the miss
    // gather below, which already existed and already scatters back to fixed
    // slots.
    std::uint64_t seed_revision_for(const clay_brick_request& request, std::size_t per,
                                    bool want_colour, bool want_below) const {
        const ResumeEntry* e = shaped_entry(request, per, want_colour, want_below);
        return e ? e->revision : 0;
    }

    // What a brick's stored seed holds, or `values == nullptr` when it cannot
    // serve this request.
    struct Seed {
        const float* values = nullptr;
        const float* colors = nullptr;
        const float* below = nullptr;  // null when no layer sits beneath
        const float* below_colors = nullptr;
        // Null unless this brick has a group stack; `values` is the field
        // either way.
        const float* stack = nullptr;
        const float* stack_colors = nullptr;
        std::uint32_t stack_levels = 0;
        bool layer_have_acc = true;
        const std::vector<scene::TapeCheckpointFrame>* frames = nullptr;
    };

    Seed seed_for(const clay_brick_request& request, std::size_t per, float pad, bool want_colour,
                  bool want_below) const {
        Seed s;
        const ResumeEntry* e = shaped_entry(request, per, want_colour, want_below);
        if (!e) return s;
        // The cull pad decides which items a brick's compile keeps, so a seed
        // taken under a different one was continued from a different field.
        // The pad only grows on an append, so this is a real gate rather than
        // a formality.
        if (e->pad != pad) return s;
        if (!e->had_acc) return s;
        // The USE in least-recently-used. A brick answered straight from its
        // seed -- a refill with no edit in between, or one a region
        // invalidation could not reach -- is never re-stored, so without this
        // the only bricks kept young would be the ones that were rebuilt.
        touch_lru(const_cast<ResumeEntry&>(*e));
        s.values = e->values.data();
        if (want_colour) s.colors = e->colors.data();
        // Handed over only when it is COMPLETE: the planes, the depth and the
        // frames are written together and read together, so a half-formed one
        // simply does not appear.
        if (e->stack_levels > 0 && e->stack.size() == per * e->stack_levels &&
            scene::checkpoint_stack_levels(e->frames, e->layer_have_acc) == e->stack_levels &&
            (!want_colour || e->stack_colors.size() == per * e->stack_levels * 3)) {
            s.stack = e->stack.data();
            s.stack_levels = e->stack_levels;
            s.layer_have_acc = e->layer_have_acc;
            s.frames = &e->frames;
            if (want_colour) s.stack_colors = e->stack_colors.data();
        }
        if (want_below) {
            s.below = e->below.data();
            if (want_colour) s.below_colors = e->below_colors.data();
        }
        return s;
    }

    struct FrontierSeed {
        Seed seed;  // seed.values == nullptr when the prefix cannot serve
        std::uint32_t boundary = 0;
    };

    // The prefix half of a dirty entry, or seed.values == nullptr. Caller
    // holds cache_mutex_. ELIGIBILITY IS NOT PROOF, so every claim the keep in
    // touch_region_locked relied on is re-checked here per brick: shape
    // (shaped_entry -- the same five checks, colour and below symmetry
    // included), structure (ordinals are only comparable inside one
    // structure_revision_), boundary <= dirty_from (equality valid: boundary B
    // means B items folded, dirty_from = i means item i changed, and item i is
    // the FIRST suffix item), pad (the document cull pad the prefix was
    // evaluated under; the pad only grows on an append and an append bumps
    // structure_revision_, so this is belt over braces), and prefix_had_acc
    // (the layer_have_acc statement plan_frontier makes, made true per brick
    // exactly as seed_for makes it for the append path). Any failure means the
    // full walk -- slower, never wrong.
    FrontierSeed frontier_seed_for(const clay_brick_request& request, std::size_t per, float pad,
                                   bool want_colour, bool want_below) const {
        FrontierSeed fs;
        const ResumeEntry* e = shaped_entry(request, per, want_colour, want_below);
        if (!e) return fs;
        if (e->dirty_from == kFrontierClean) return fs;
        if (e->prefix_structure == 0 || e->prefix_structure != structure_revision_) return fs;
        if (e->prefix_boundary > e->dirty_from) return fs;
        if (e->prefix_values.size() != per) return fs;
        if (want_colour && e->prefix_colors.size() != per * 3) return fs;
        if (e->prefix_pad != pad) return fs;
        if (!e->prefix_had_acc) return fs;
        // The USE in least-recently-used, for seed_for's reason: a brick the
        // drag rewrites every frame is the working set.
        touch_lru(const_cast<ResumeEntry&>(*e));
        fs.seed.values = e->prefix_values.data();
        if (want_colour) fs.seed.colors = e->prefix_colors.data();
        if (want_below) {
            // The below half is the append path's own: static across a drag
            // for the same reason it is static across a stroke -- only the
            // active layer moves, and command_frontier refuses edits that
            // would move this half.
            fs.seed.below = e->below.data();
            if (want_colour) fs.seed.below_colors = e->below_colors.data();
        }
        fs.boundary = e->prefix_boundary;
        return fs;
    }

    // What a full refill can hand back about where its walk passed the
    // checkpoint. `levels` 0 means it could not, which is the ordinary case
    // for a document with no tail group and for a backend whose grid batch
    // does not produce one.
    struct SeedStack {
        const float* values = nullptr;  // per * levels, plane 0 the bottom
        const float* colors = nullptr;  // per * levels * 3, or null
        std::uint32_t levels = 0;
        bool layer_have_acc = true;
        const std::vector<scene::TapeCheckpointFrame>* frames = nullptr;
    };

    void store_seed(const clay_brick_request& request, std::uint64_t at, float pad,
                    const float* values, const float* colors, const float* below,
                    const float* below_colors, std::size_t per,
                    const SeedStack* stack = nullptr) const {
        const ResumeKey key = resume_key(request);
        auto [it, fresh] = resume_.try_emplace(key);
        ResumeEntry& e = it->second;
        resume_bytes_ -= entry_bytes(e);
        if (fresh)
            e.lru = resume_order_.insert(resume_order_.end(), key);
        else
            touch_lru(e);
        e.had_acc = false;
        for (std::size_t s = 0; s < per && !e.had_acc; ++s) e.had_acc = values[s] != CLAY_TAPE_FAR;
        e.revision = at;
        e.pad = pad;
        e.values.assign(values, values + per);
        if (colors)
            e.colors.assign(colors, colors + per * 3);
        else
            e.colors.clear();
        // A full walk produces no stack UNLESS the caller took one on the way
        // past, and a stack that does not describe this walk is worse than
        // none. Leaving a stale one behind pairs it with a fresh field and pad
        // and the next resume reads it as though it described this document --
        // measured as a BAND of document sizes where the cull pad steps
        // mid-stroke, sending bricks down this path and back: clean at 20, 50
        // and 80 stamps, wrong at 100 and 120, clean again from 200. So this
        // clears first and stores only what arrived with THIS field.
        e.stack.clear();
        e.stack_colors.clear();
        e.stack_levels = 0;
        e.frames.clear();
        if (stack && stack->levels > 0 && stack->values && stack->frames &&
            scene::checkpoint_stack_levels(*stack->frames, stack->layer_have_acc) ==
                stack->levels) {
            e.stack.assign(stack->values, stack->values + per * stack->levels);
            if (stack->colors)
                e.stack_colors.assign(stack->colors, stack->colors + per * stack->levels * 3);
            e.frames = *stack->frames;
            e.layer_have_acc = stack->layer_have_acc;
            e.stack_levels = stack->levels;
        }
        if (below)
            e.below.assign(below, below + per);
        else
            e.below.clear();
        if (below_colors)
            e.below_colors.assign(below_colors, below_colors + per * 3);
        else
            e.below_colors.clear();
        // A full-path answer is a fresh whole world for this brick; a prefix
        // kept beside it describes an older one -- dead bytes inside a budget
        // that would then evict live seeds, and a structure-stale prefix
        // waiting for a counter wrap that never comes. The prepare pass
        // re-records cheaply when a drag wants one.
        e.dirty_from = kFrontierClean;
        e.prefix_structure = 0;
        e.prefix_boundary = 0;
        e.prefix_had_acc = false;
        e.prefix_values.clear();
        e.prefix_colors.clear();
        resume_bytes_ += entry_bytes(e);
        evict_locked();
    }

    // The batch's results, kept as the next dab's seeds. `at` 0 means "the
    // current revision", which is what the full path passes -- it has just
    // produced the document as it is now.
    void store_seeds(const clay_brick_request* requests, std::size_t count, const float* values,
                     const float* colors, const float* below, const float* below_colors,
                     std::size_t per, std::uint64_t at, float pad,
                     const SeedStack* stacks = nullptr) const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const std::uint64_t now = revision.load(std::memory_order_relaxed);
        if (at == 0) {
            at = now;
            pad = cull_index_locked()->cull_pad();
        } else if (at != now) {
            return;  // the document moved under the batch; keeping it would lie
        }
        for (std::size_t i = 0; i < count; ++i)
            store_seed(requests[i], at, pad, values + i * per,
                       colors ? colors + i * per * 3 : nullptr, below ? below + i * per : nullptr,
                       below_colors ? below_colors + i * per * 3 : nullptr, per,
                       stacks ? &stacks[i] : nullptr);
    }

    // The resumed path's store: the ACTIVE layer's value moved, the half
    // beneath it did not. Caller holds cache_mutex_.
    void store_active(const clay_brick_request& request, std::uint64_t at, float pad,
                      const float* values, const float* colors, std::size_t per,
                      const float* stack = nullptr, const float* stack_colors = nullptr,
                      std::uint32_t stack_levels = 0,
                      const std::vector<scene::TapeCheckpointFrame>* frames = nullptr,
                      bool layer_have_acc = true) const {
        auto it = resume_.find(resume_key(request));
        if (it == resume_.end()) return;
        ResumeEntry& e = it->second;
        // The rewrite a dab makes is the strongest statement there is that this
        // brick is in the working set: it was read this dab and will be read
        // the next. Leaving the order alone here is what made the policy
        // anti-LRU -- the bricks rewritten every dab are the ones stored first,
        // so a first-insertion order evicted precisely them (#346).
        touch_lru(e);
        resume_bytes_ -= entry_bytes(e);
        e.had_acc = false;
        for (std::size_t s = 0; s < per && !e.had_acc; ++s) e.had_acc = values[s] != CLAY_TAPE_FAR;
        e.revision = at;
        // This is the ACCEPTED CURRENT-GENERATION submit -- the call site only
        // reaches here when the plan's `now` is still the current revision, so
        // a stale submit never clears a frontier a newer edit set. The prefix
        // vectors are deliberately NOT touched: they are the next frame's
        // seed, and overwriting them with the post-suffix value is what would
        // turn every drag frame after the first back into a full walk.
        e.dirty_from = kFrontierClean;
        e.pad = pad;
        e.values.assign(values, values + per);
        if (colors)
            e.colors.assign(colors, colors + per * 3);
        else
            e.colors.clear();
        // The stack, the depth and the frames go together or not at all -- a
        // reader takes all three or none, so a partial write cannot be read.
        if (stack && stack_levels > 0 && frames &&
            scene::checkpoint_stack_levels(*frames, layer_have_acc) == stack_levels) {
            e.stack.assign(stack, stack + per * stack_levels);
            if (stack_colors)
                e.stack_colors.assign(stack_colors, stack_colors + per * stack_levels * 3);
            else
                e.stack_colors.clear();
            e.stack_levels = stack_levels;
            e.frames = *frames;
        } else {
            e.stack.clear();
            e.stack_colors.clear();
            e.stack_levels = 0;
            e.frames.clear();
        }
        resume_bytes_ += entry_bytes(e);
    }

    // The invalidation for an edit that is NOT an append but whose reach is
    // known: everything the caches hold by revision goes, but a brick's seed
    // survives when the change cannot reach it.
    //
    // A seed is the value of that brick's CULLED tape. An item whose influence
    // misses the brick's cull region is dropped from that tape, so editing it
    // cannot change what the brick evaluates to -- the seed is still the answer,
    // now at the new revision. Seeds the change does reach are dropped, and the
    // next refill computes them.
    //
    // `changed` MUST cover both sides of the edit. `command_influence_bound` on
    // one side is not an answer -- an add's node is not there before, a removal's
    // is not there after, a move has two ends -- so a caller unions the two, as
    // the undo stack does.
    //
    // An EMPTY box means the edit cannot change what the document evaluates to
    // (a rename, a protection flag), so every seed survives. An INFINITE one
    // reaches everywhere and takes them all.
    void touch_region(const math::Aabb& changed) { touch_regions({&changed, 1}); }

    // The same drop for a change that lands in SEVERAL places at once -- a
    // drag under a layer mirror moves material under the ball and under its
    // reflection (#363) -- taken as one invalidation, one revision, one lock.
    // Stated as the boxes rather than their union because the union of two
    // balls a diameter apart is the slab between them, and under a mirror
    // that slab is the whole document: every warm brick would resume every
    // frame for ground the drag never touched (measured 0.35x the cold refill
    // against 0.16x for the same drag unmirrored).
    void touch_regions(std::span<const math::Aabb> changed) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        forget_appends();  // not an append; no prefix may be reused
        const std::uint64_t next = revision.fetch_add(1, std::memory_order_relaxed) + 1;
        touch_region_locked(changed, kFrontierDrop, next);
    }

    // The region invalidation for an edit that MOVES ordinals -- node
    // add/insert/delete/move, layer add/remove/visibility, and a replayed undo
    // or redo, which can contain any of them. The same drop as touch_region,
    // plus the structure bump that retires every prefix seed: their boundaries
    // were counted against a root list this edit just rewrote, and
    // over-invalidating costs one full walk (the rule at touch(), restated).
    void touch_region_structural(const math::Aabb& changed) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        forget_appends();
        ++structure_revision_;
        const std::uint64_t next = revision.fetch_add(1, std::memory_order_relaxed) + 1;
        touch_region_locked({&changed, 1}, kFrontierDrop, next);
    }

    // The frontier invalidation (#360): a parameter edit inside root ordinal
    // `frontier` of the active layer. Seeds inside the bound whose prefix can
    // still serve that frontier are KEPT and marked dirty rather than dropped;
    // everything else behaves exactly as touch_region.
    void touch_region_from(const math::Aabb& changed, std::uint32_t frontier) {
        touch_regions_from({&changed, 1}, frontier);
    }

    // touch_region_from over several boxes, as touch_regions is to touch_region.
    void touch_regions_from(std::span<const math::Aabb> changed, std::uint32_t frontier) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        forget_appends();  // not an append either; the log's contiguity is broken
        const std::uint64_t next = revision.fetch_add(1, std::memory_order_relaxed) + 1;
        touch_region_locked(changed, frontier, next);
    }

    std::uint64_t current_revision() const { return revision.load(std::memory_order_relaxed); }

    std::mutex& cache_lock() const { return cache_mutex_; }

    // What the last refills did, and what the seed store costs. The counts are
    // what makes the resumable path OBSERVABLE: it is bit-identical to the full
    // path by contract, so nothing about a refill's output can tell whether it
    // fired, and a fast path that quietly stopped firing reads as correct.
    void note_refill(std::uint64_t resumed, std::uint64_t refilled) const {
        resumed_bricks_.fetch_add(resumed, std::memory_order_relaxed);
        refilled_bricks_.fetch_add(refilled, std::memory_order_relaxed);
    }

    void resume_stats(std::uint64_t* entries, std::uint64_t* bytes, std::uint64_t* budget,
                      std::uint64_t* resumed, std::uint64_t* refilled) const {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            *entries = resume_.size();
            *bytes = resume_bytes_;
            *budget = resume_budget_;
        }
        *resumed = resumed_bricks_.load(std::memory_order_relaxed);
        *refilled = refilled_bricks_.load(std::memory_order_relaxed);
    }

    // -- what only a test asks for (bindings/c/clay_internal.h) -------------
    //
    // The eviction order holds one node per entry and no others, which is the
    // invariant #346 broke. Not in clay_resume_stats because a host has nothing
    // to do with the number: a store reporting an order size that differs from
    // its entry count would be describing its own bug, not a state to react to.
    std::size_t resume_order_size() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return resume_order_.size();
    }

    // A byte ceiling reachable in a test. 64 MB is 32,768 dim-8 distance-only
    // bricks (2,048 B each, measured), so proving anything about eviction at the
    // shipped budget means filling and refilling that many -- which measures the
    // machine rather than the policy. Lowering it evicts down immediately, so
    // the store is never left above a budget it has been told to keep -- other
    // than the one entry evict_locked always keeps.
    void set_resume_budget(std::size_t bytes) const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        resume_budget_ = bytes;
        evict_locked();
    }

    // The interleaving the ABI cannot produce single-threaded: an edit landing
    // between a resume batch's seeded walks and the retaken store lock, which
    // is the only window where the plan->now == now gate in front of
    // store_active has a stale submit to refuse. resume_bricks fires this once,
    // right before that lock, and it disarms itself BEFORE the callback runs so
    // the edit the callback makes cannot re-trigger it on the refill that
    // follows. Exists only so that gate is testable (clay_internal.h); never
    // armed outside a test, so the fast path pays one null check.
    void set_resume_store_interleave(void (*fn)(void*), void* user) const {
        resume_interleave_ = fn;
        resume_interleave_user_ = user;
    }
    void run_resume_store_interleave() const {
        if (!resume_interleave_) return;
        void (*fn)(void*) = resume_interleave_;
        void* user = resume_interleave_user_;
        resume_interleave_ = nullptr;
        resume_interleave_user_ = nullptr;
        fn(user);
    }

    // -- the pre-drag seed pass (#360) --------------------------------------
    //
    // One prefix evaluation the prepare pass owes a brick: which entry (by
    // key), at what lattice, at which boundary. Phase B fills `values` /
    // `colors` / `had_acc` off the lock; a job whose compile or evaluation
    // refused leaves `values` shorter than `per`, and phase C skips it.
    struct FrontierJob {
        ResumeKey key;
        std::size_t per = 0;
        bool want_colour = false;
        std::uint32_t boundary = 0;
        bool had_acc = false;
        std::vector<float> values;
        std::vector<float> colors;
    };

    // Everything phase A decided under one lock, carried across the unlocked
    // evaluation so phase C can tell whether the document moved underneath it.
    struct FrontierPrepare {
        std::uint64_t now = 0;
        std::uint64_t structure = 0;
        float pad = 0.0f;
        std::shared_ptr<const scene::CullIndex> index;
        std::vector<FrontierJob> jobs;
    };

    // PHASE A: which entries the coming applies will dirty and which of them
    // lack a usable prefix at this frame's boundary. `warps` carries one
    // (root ordinal, pre-apply influence bound) pair per node the drag will
    // rewrite. Only a CLEAN, CURRENT entry is a candidate -- a dirty or stale
    // one describes a field a prefix cannot be sliced out of -- and an entry
    // already holding a prefix this frame can use is skipped, which is what
    // makes calling this every frame cost the region once: frame one pays for
    // it, later frames pay only for bricks the growing drag has just reached.
    FrontierPrepare frontier_prepare(
        const std::vector<std::pair<std::uint32_t, math::Aabb>>& warps) const {
        FrontierPrepare out;
        std::lock_guard<std::mutex> lock(cache_mutex_);
        out.now = revision.load(std::memory_order_relaxed);
        out.structure = structure_revision_;
        for (const auto& [k, e] : resume_) {
            if (e.dirty_from != kFrontierClean || e.revision != out.now) continue;
            const float width = static_cast<float>(k.dims[0]) * k.spacing;
            const kernel::cfloat3 lo =
                kernel::cf3(static_cast<float>(k.x), static_cast<float>(k.y),
                            static_cast<float>(k.z)) *
                width;
            const math::Aabb cull =
                math::Aabb{lo, lo + kernel::cf3(width, width, width)}.dilated(k.band + e.pad);
            // The boundary this brick needs: the earliest ordinal among the
            // warps whose influence reaches it. MIN for touch_region_locked's
            // reason -- the seed has to sit before everything that will move.
            std::uint32_t boundary = kFrontierClean;
            for (const auto& [ordinal, bound] : warps)
                if (!bound.empty() && bound.intersects(cull))
                    boundary = std::min(boundary, ordinal);
            // Ordinal 0 would be an empty prefix, which plan_frontier refuses
            // anyway; kFrontierClean means no warp reaches this brick.
            if (boundary == kFrontierClean || boundary == 0) continue;
            // The cull index is only wanted once a brick turns out to be a
            // candidate, for resume_bricks's reason: a document with no seeds
            // near the drag should not pay an O(document) index rebuild per
            // frame on its way to doing nothing.
            if (!out.index) {
                out.index = cull_index_locked();
                out.pad = out.index->cull_pad();
            }
            const bool want_colour = !e.colors.empty();
            const bool usable_already =
                e.prefix_structure == out.structure && e.prefix_boundary <= boundary &&
                e.prefix_values.size() == e.values.size() &&
                (!want_colour || e.prefix_colors.size() == e.values.size() * 3) &&
                e.prefix_pad == out.pad;
            if (usable_already) continue;
            FrontierJob job;
            job.key = k;
            job.per = e.values.size();
            job.want_colour = want_colour;
            job.boundary = boundary;
            out.jobs.push_back(std::move(job));
        }
        return out;
    }

    // PHASE C: keep what phase B evaluated, INSIDE the existing budget and
    // LRU. If the document or its structure moved while the lock was down the
    // whole batch is abandoned -- a prefix stored against a document that has
    // moved would lie, the same discipline store_seeds keeps. Per job the
    // entry is re-verified (evicted meanwhile => skip; dirtied or re-stamped
    // meanwhile => skip) and every mutation sits inside the entry_bytes
    // bracket, so the budget stays exact. One evict after the loop: the
    // prefixes live under the same ceiling as everything else.
    void frontier_store(FrontierPrepare& prep) const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (revision.load(std::memory_order_relaxed) != prep.now) return;
        if (structure_revision_ != prep.structure) return;
        bool stored = false;
        for (FrontierJob& job : prep.jobs) {
            if (job.per == 0 || job.values.size() != job.per) continue;
            if (job.want_colour && job.colors.size() != job.per * 3) continue;
            auto it = resume_.find(job.key);
            if (it == resume_.end()) continue;  // evicted while the lock was down
            ResumeEntry& e = it->second;
            if (e.dirty_from != kFrontierClean || e.revision != prep.now) continue;
            if (e.values.size() != job.per) continue;
            resume_bytes_ -= entry_bytes(e);
            e.prefix_boundary = job.boundary;
            e.prefix_structure = prep.structure;
            e.prefix_pad = prep.pad;
            e.prefix_had_acc = job.had_acc;
            e.prefix_values = std::move(job.values);
            e.prefix_colors = std::move(job.colors);
            resume_bytes_ += entry_bytes(e);
            stored = true;
        }
        if (stored) evict_locked();
    }

    // dirty_from / prefix_boundary / prefix_structure of one brick's entry,
    // for bindings/c/clay_internal.h. Tests pin the min-merge, the clear-on-
    // accepted-submit and the structure tagging on it; a host has no business
    // with any of the three numbers.
    bool frontier_probe(const clay_brick_request& request, std::uint32_t* dirty_from,
                        std::uint32_t* boundary, std::uint64_t* structure) const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = resume_.find(resume_key(request));
        if (it == resume_.end()) return false;
        *dirty_from = it->second.dirty_from;
        *boundary = it->second.prefix_boundary;
        *structure = it->second.prefix_structure;
        return true;
    }

    // The appends that take revision `from` to the current one, or nothing
    // when the log does not cover that span -- no valid log, a reader older
    // than its base, or a reader ahead of its end. Caller holds cache_mutex_.
    std::vector<scene::NodeId> appends_since(std::uint64_t from, std::uint64_t now) const {
        if (!append_valid_ || append_at_ != now || from < append_base_ || from > append_at_)
            return {};
        const std::size_t skip = static_cast<std::size_t>(from - append_base_);
        if (skip > append_log_.size()) return {};
        return std::vector<scene::NodeId>(append_log_.begin() + static_cast<std::ptrdiff_t>(skip),
                                          append_log_.end());
    }

    std::shared_ptr<const scene::Tape> tape() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const std::uint64_t now = revision.load(std::memory_order_relaxed);
        if (tape_cache_ && tape_revision_ == now) return tape_cache_;
        // The append fast path, taken only when the cached tape is exactly
        // the document the log sits on top of. compile_document_append does
        // its own checking and refuses rather than trusting any of this, so a
        // wrong answer here costs a full compile, not a wrong field.
        const std::vector<scene::NodeId> since =
            tape_cache_ ? appends_since(tape_revision_, now) : std::vector<scene::NodeId>{};
        if (!since.empty() && tape_checkpoint_.valid) {
            scene::Tape grown;
            scene::TapeCheckpoint next;
            if (scene::compile_document_append(*tape_cache_, tape_checkpoint_, doc.document, since,
                                               &grown, &next)) {
                tape_cache_ = std::make_shared<const scene::Tape>(std::move(grown));
                tape_checkpoint_ = next;
                tape_revision_ = now;
                // The log is NOT cleared here: another cache may still be
                // behind it, and the next append extends it rather than
                // starting over. See touch_appended.
                return tape_cache_;
            }
        }
        scene::TapeCheckpoint fresh;
        tape_cache_ =
            std::make_shared<const scene::Tape>(scene::compile_document_resumable(doc.document,
                                                                                  &fresh));
        tape_checkpoint_ = fresh;
        tape_revision_ = now;
        return tape_cache_;
    }

    // Picking excludes ghosted layers, so it is a different tape and gets its
    // own slot rather than sharing one that would thrash between the two.
    std::shared_ptr<const scene::Tape> pickable_tape() const {
        return cached(pick_cache_, pick_revision_,
                      [this] { return pick::pickable_tape(doc.document); });
    }

    // The per-revision cull index (scene/cull_index.h): cached bounds and
    // coarse pruning for per-brick culled compiles, keyed on the same
    // revision as the tape — an edit invalidates both the same way.
    std::shared_ptr<const scene::CullIndex> cull_index() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cull_index_locked();
    }

    // The same, for a caller that already holds cache_mutex_.
    std::shared_ptr<const scene::CullIndex> cull_index_locked() const {
        const std::uint64_t now = revision.load(std::memory_order_relaxed);
        if (index_cache_ && index_revision_ == now) return index_cache_;
        // The append fast path, taken only when the cached index is exactly
        // the document the log sits on top of. Rebuilding it to add one item
        // walks every node recomputing bounds that did not move -- 2.42 ms at
        // 50,000 items against 0.13 ms to extend. CullIndex::append does its
        // own checking and refuses rather than trusting any of this, so being
        // wrong here costs a rebuild, not a wrong cull.
        const std::vector<scene::NodeId> since =
            index_cache_ ? appends_since(index_revision_, now) : std::vector<scene::NodeId>{};
        // scene::append_cached extends index_cache_ in place when this handle
        // is the only one alive and copies it first when it is not, which saves
        // a dab an O(document) copy to add one item's bounds -- 0.043 ms at
        // 20,000 items, which with the 0.054 ms pad walk beside it was 79% of
        // the 0.123 ms a whole resumed dab cost there (#347).
        //
        // WHAT MAKES THE COUNT MEANINGFUL, and the reason it is read here and
        // not anywhere else: every reader takes its handle from this function
        // under cache_mutex_ and holds it for as long as it reads, so a count
        // of one observed under that lock means no snapshot exists and none can
        // appear before the lock is released.
        //
        // An append that refuses leaves the index untouched (its contract), so
        // the rebuild below is still correct after one.
        if (!since.empty() && scene::append_cached(index_cache_, since)) {
            index_revision_ = now;
            return index_cache_;
        }
        index_cache_ = std::make_shared<scene::CullIndex>(doc.document);
        index_revision_ = now;
        return index_cache_;
    }

  private:
    // Caller holds cache_mutex_.
    void forget_appends() const {
        append_valid_ = false;
        append_log_.clear();
    }

    // The one loop behind the three touch_region fronts; caller holds
    // cache_mutex_ and has already bumped `revision` to `next`.
    //
    // `frontier` == kFrontierDrop is the legacy behaviour: everything the
    // bound reaches is dropped. Any other frontier F KEEPS an in-bound entry
    // whose prefix can still serve F, marking dirty_from = min(dirty_from, F)
    // -- MIN because two edits at two ordinals dirty from the EARLIER one, and
    // an overwrite would resurrect a prefix the first edit already
    // invalidated.
    //
    // `changed` is one box per place the edit lands; an entry is reached when
    // ANY of them intersects its cull region, and one infinite box takes
    // everything, exactly as it does alone.
    void touch_region_locked(std::span<const math::Aabb> changed, std::uint32_t frontier,
                             std::uint64_t next) const {
        if (std::any_of(changed.begin(), changed.end(),
                        [](const math::Aabb& b) { return b.is_infinite(); })) {
            forget_resume();
            return;
        }
        for (auto it = resume_.begin(); it != resume_.end();) {
            const ResumeKey& k = it->first;
            ResumeEntry& e = it->second;
            const float width = static_cast<float>(k.dims[0]) * k.spacing;
            const kernel::cfloat3 lo =
                kernel::cf3(static_cast<float>(k.x), static_cast<float>(k.y),
                            static_cast<float>(k.z)) *
                width;
            const math::Aabb cull =
                math::Aabb{lo, lo + kernel::cf3(width, width, width)}.dilated(k.band + e.pad);
            const bool reached =
                std::any_of(changed.begin(), changed.end(), [&](const math::Aabb& b) {
                    return !b.empty() && b.intersects(cull);
                });
            if (reached) {
                // ELIGIBILITY, not proof: kept only when the prefix's claims
                // still stand -- same structure, right shape, boundary at or
                // before both the existing frontier and this one. The refill
                // re-proves all of it per brick in frontier_seed_for, so a
                // keep that turns out wrong there costs a full walk, never a
                // wrong field.
                const bool keepable =
                    frontier != kFrontierDrop && e.prefix_structure != 0 &&
                    e.prefix_structure == structure_revision_ &&
                    e.prefix_values.size() == e.values.size() &&
                    e.prefix_boundary <= std::min(e.dirty_from, frontier);
                if (keepable) {
                    e.dirty_from = std::min(e.dirty_from, frontier);
                    ++it;
                    continue;
                }
                resume_bytes_ -= entry_bytes(e);
                // The order node goes with the entry. It used to be left
                // behind: the order then grew without bound (its nodes are not
                // counted by entry_bytes, so they sat outside the budget
                // entirely) and a key erased here and stored again later was
                // inserted a SECOND time, so one brick held several slots and
                // the order stopped describing the store (#346).
                resume_order_.erase(e.lru);
                it = resume_.erase(it);
                continue;
            }
            // Untouched: the same value, now current -- but ONLY while the
            // entry is clean. Advancing a DIRTY entry to the current revision
            // would let the rev == now shortcut in resume_bricks hand out its
            // stale composite as the whole answer: a later edit sweeping past
            // ELSEWHERE would launder the entry current without anything ever
            // folding the suffix in. A dirty entry stays at its old revision
            // until an accepted submit clears it in store_active. For a store
            // that holds no dirty entries -- every path before #360 -- this is
            // byte-for-byte the old behaviour.
            if (e.dirty_from == kFrontierClean) e.revision = next;
            ++it;
        }
    }


    template <typename T, typename Build>
    std::shared_ptr<const T> cached(std::shared_ptr<const T>& slot,
                                    std::uint64_t& slot_revision, Build build) const {
        const std::uint64_t now = revision.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!slot || slot_revision != now) {
            slot = std::make_shared<const T>(build());
            slot_revision = now;
        }
        return slot;
    }

    mutable std::mutex cache_mutex_;
    mutable std::shared_ptr<const scene::Tape> tape_cache_;
    mutable std::uint64_t tape_revision_ = 0;
    // Where a resumed compile picks tape_cache_ up, and the appends recorded
    // since it was built: the log applies on top of revision append_base_ and
    // brings the document to append_at_.
    mutable scene::TapeCheckpoint tape_checkpoint_;
    mutable std::vector<scene::NodeId> append_log_;
    mutable scene::LayerId append_layer_ = 0;
    mutable std::uint64_t append_base_ = 0;
    mutable std::uint64_t append_at_ = 0;
    mutable bool append_valid_ = false;
    // Bumps on any edit that can change the active layer's EVALUATION ORDER --
    // append/insert/delete/move of nodes, layer add/remove/visibility, and the
    // unknown-mutation funnels (touch) -- and on nothing parameter-shaped. A
    // prefix seed is only meaningful inside one value of this counter: its
    // boundary is a POSITION in the root list, and positions do not survive a
    // reorder, so a mismatch is a full refusal, never a remap. Guarded by
    // cache_mutex_, like the append log beside it.
    mutable std::uint64_t structure_revision_ = 1;
    mutable std::shared_ptr<const scene::Tape> pick_cache_;
    mutable std::uint64_t pick_revision_ = 0;
    // NOT const, unlike the tape beside it: an append extends it in place when
    // the handle here is the only one alive (see cull_index_locked). Handed out
    // as shared_ptr<const>, so a caller still cannot write through it.
    mutable std::shared_ptr<scene::CullIndex> index_cache_;
    mutable std::uint64_t index_revision_ = 0;
    mutable std::unordered_map<ResumeKey, ResumeEntry, ResumeKeyHash> resume_;
    // The eviction order, least recently USED at the front, one node per live
    // entry. A list rather than a deque because both of the operations #346
    // needed are O(1) on it and neither is on a deque: moving a re-used brick
    // to the back, and removing an invalidated one from the middle. Every entry
    // holds its own node (ResumeEntry::lru), so neither is a search.
    mutable std::list<ResumeKey> resume_order_;
    mutable std::size_t resume_bytes_ = 0;
    mutable std::size_t resume_budget_ = kResumeBytes;
    // Cumulative over the document's life, so a host can see whether its refill
    // loop is actually reaching the fast path. Atomic rather than under
    // cache_mutex_: they are read without it, and a torn count would be worse
    // than a slightly stale one.
    mutable std::atomic<std::uint64_t> resumed_bricks_{0};
    mutable std::atomic<std::uint64_t> refilled_bricks_{0};
    // The one-shot test seam of set_resume_store_interleave. Not under
    // cache_mutex_: it is armed and fired on the one thread a test owns, and
    // the point where it fires holds no lock by design.
    mutable void (*resume_interleave_)(void*) = nullptr;
    mutable void* resume_interleave_user_ = nullptr;
};

// The item builder is a scene::Node under construction. Whether a transition
// was given (and which kind) is not a Node field, so it rides alongside: the
// op and the parameters have to agree, as they do in the Python bindings.
struct clay_item {
    scene::Node node;
    bool has_transition = false;
    bool transition_is_linear = false;
};

// A brick cache the caller owns, plus the one thing the C++ class does not
// provide: a staging FIFO. BrickCache::take_dirty() is all-or-nothing — it
// drains the whole dirty set and bumps every generation — so the ABI's
// capacity-in/count-out drain calls it ONCE into this buffer and serves chunks
// from it. Serving from a buffer rather than partially draining the engine is
// what keeps a short call from stranding requests whose generations were
// already bumped and whose bricks are no longer marked: those would never
// refill again. A staged request that is re-dirtied before it is submitted
// simply lands as STALE, which is exactly right.
//
// No mutex: the cache is the host's to serialize (see clay.h). This handle
// adds no state that changes that.
struct clay_brick_cache {
    brick::BrickCache cache;
    std::vector<brick::BrickRequest> staged;
    std::size_t staged_head = 0;

    explicit clay_brick_cache(brick::BrickConfig config) : cache(config) {}

    std::size_t staged_remaining() const { return staged.size() - staged_head; }
};

namespace {

// -- builder helpers, below the handles they touch ---------------------------

// Defined with the other edit plumbing below; declared here because the
// insertion path routes through it too.

// Defined at the end of this namespace: every edit routes through the
// command vocabulary. Declared here because it is used above.
clay_result apply_edit(clay_document* doc, const scene::Command& cmd, const char* what);

// The same edit, WITHOUT the invalidation — for a gesture that knows the region
// it can reach and invalidates once for all of its commands. See GestureRegion.
clay_result apply_edit_in_gesture(clay_document* doc, const scene::Command& cmd,
                                  const char* what);

// The one insertion path: everything authored through this ABI, flat
// descriptor included, ends here. It routes through the command vocabulary —
// an AddNodeCmd with a reserved id, since command replay preserves ids — so
// an enabled undo stack records the add like every other edit. (Regression:
// inserting directly into the layer let adds escape undo.)
clay_result insert_node(clay_document* doc, clay_layer_id layer_id, scene::Node node,
                        clay_node_id* out_node, scene::NodeId parent = scene::kNoNode,
                        int index = -1) {
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    // A parent that is not a group is the caller's mistake, not a missing
    // node: reinsert would refuse it and apply_edit could only report
    // NOT_FOUND, which says nothing about which of the two ids was wrong.
    if (parent != scene::kNoNode) {
        const scene::Node* g = layer->sdf->find(parent);
        if (!g) return fail(CLAY_ERROR_NOT_FOUND, "group not found");
        if (!g->is_group) return fail(CLAY_ERROR_INVALID_ARGUMENT, "node is not a group");
    }
    node.id = layer->sdf->reserve_id();
    scene::NodeId id = node.id;
    std::vector<scene::Node> subtree;
    subtree.push_back(std::move(node));
    clay_result r = apply_edit(
        doc, scene::Command{scene::AddNodeCmd{layer_id, parent, index, std::move(subtree)}},
        "layer not found");
    if (r != CLAY_OK) return r;
    if (out_node) *out_node = id;
    return CLAY_OK;
}

// The transition cross-check the Python bindings do in Layer.add: a morph op
// needs parameters, of its own kind, and no other op accepts them.
clay_result validate_item(const clay_item& item) {
    // A loft with fewer than two profiles has nothing to interpolate between.
    // Refused here rather than compiled into a shape the caller did not ask
    // for — the tape would read a record that was never written.
    if (scene::prim_carries_profiles(item.node.prim.type) && item.node.profiles.size() < 2)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a loft or sweep needs two or more profiles: add them with "
                    "clay_item_add_loft_profile");
    if (scene::prim_is_swept(item.node.prim.type) && item.node.stroke.size() < 2)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a sweep needs a guide of two or more points: set it with "
                    "clay_item_set_curve_points");
    bool morph = scene::op_is_transition(item.node.op);
    if (morph && !item.has_transition)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "transition ops need transition parameters");
    if (!morph && item.has_transition)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "transition parameters apply to transition ops only");
    if (morph && (item.node.op == scene::Op::TransitionLinear) != item.transition_is_linear)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the linear op needs linear parameters and the radial op radial ones");
    return CLAY_OK;
}

// -- voxel handle resolution, below the handles it touches -------------------

voxel::VoxelCoord to_coord(const std::int32_t c[3]) { return {c[0], c[1], c[2]}; }

// Bracket a voxel edit so it becomes ONE undo step (unify-the-undo-history).
//
// A standalone grid has no document and therefore no history: undo is a
// document concept, and a grid made with clay_voxel_grid_create is not in one.
// A borrowed handle names its document and layer, which is exactly what the
// history needs, so the bracket costs a null check on the common path.
//
// RAII because the verbs below have many early returns, and a step left open
// would install a sink on a grid nobody is recording — which would attribute
// the NEXT edit to this one.
struct VoxelStep {
    clay_document* doc = nullptr;
    voxel::VoxelGrid* grid = nullptr;

    VoxelStep(const clay_voxel_grid* handle, voxel::VoxelGrid* g);
    ~VoxelStep();
    VoxelStep(const VoxelStep&) = delete;
    VoxelStep& operator=(const VoxelStep&) = delete;
};

clay_result resolve(const clay_voxel_grid* grid, voxel::VoxelGrid** out) {
    if (!grid) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null voxel grid");
    if (grid->owned) {
        *out = grid->owned;
        return CLAY_OK;
    }
    auto it = grid->doc->doc.voxel_layers.find(grid->layer);
    // A removal keeps the grid (see the handle's comment), so no call reaches
    // this today; it is the guard that keeps a dropped payload a refusal.
    if (it == grid->doc->doc.voxel_layers.end())
        return fail(CLAY_ERROR_NOT_FOUND, "voxel layer is no longer in its document");
    *out = &it->second;
    return CLAY_OK;
}

// The mesh a handle stands for, or null when a borrow names a layer the
// document no longer carries. The read accessors return no status, so they
// answer 0/null through this rather than pretending the handle is empty.
const mesh::Mesh* mesh_data(const clay_mesh* mesh) {
    if (!mesh) return nullptr;
    if (!mesh->doc) return &mesh->data;
    auto it = mesh->doc->doc.mesh_layers.find(mesh->layer);
    return it == mesh->doc->doc.mesh_layers.end() ? nullptr : &it->second;
}

clay_result resolve_mesh(const clay_mesh* mesh, const mesh::Mesh** out) {
    if (!mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh");
    *out = mesh_data(mesh);
    if (!*out) return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer is no longer in its document");
    return CLAY_OK;
}

clay_result resolve_mask(const clay_mask* mask, voxel::MaskField** out) {
    if (!mask) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mask");
    if (mask->owned) {
        *out = mask->owned;
        return CLAY_OK;
    }
    auto it = mask->doc->doc.masks.find(mask->layer);
    if (it == mask->doc->doc.masks.end())
        return fail(CLAY_ERROR_NOT_FOUND, "mask is no longer in its document");
    *out = &it->second;
    return CLAY_OK;
}

// One backend raycast, re-issued past any hit that lands on hidden surface.
//
// The BACKEND path, which is a different one from pick::raycast_scene: eval
// cannot see clay/voxel by the layering table, so a backend can never consult a
// group field and the skip has to happen here, at the boundary that can see
// both. clay_raycast and clay_raycast_many go through here;
// clay_raycast_attributed takes RaycastOptions::groups instead and does the
// same thing one layer down.
//
// SKIPPED, NOT MISSED: hiding the front of a head is how an artist reaches the
// inside of it, so a ray that stopped at hidden surface would defeat the
// feature it implements.
eval::Status raycast_visible(eval::Backend* b, const scene::Tape& tape, const float ray[6],
                             const voxel::GroupField* groups, eval::RayHit* out) {
    eval::RayQuery q{ray, 1, 0.0f, 1e6f, 1e-4f, 256};
    eval::Status st = b->raycast(tape, q, out);
    if (st != eval::Status::Ok || !out->hit || !groups) return st;
    const kernel::cfloat3 hit_p = kernel::cf3(out->pos[0], out->pos[1], out->pos[2]);
    if (!groups->point_hidden(hit_p)) return st;

    // Hidden. Hand off to the same scan pick::raycast_scene uses rather than
    // re-issuing the march: the ray is now INSIDE the shape it just hit, where
    // the field is negative and a sphere-march cannot step, and walking out of
    // that solid overshoots the far wall — which is the surface being looked
    // for. See pick::next_visible_crossing.
    const math::Ray r{kernel::cf3(ray[0], ray[1], ray[2]), kernel::cf3(ray[3], ray[4], ray[5])};
    auto field = [&](kernel::cfloat3 p) { return tape.eval(p).d; };
    const float step = kernel::cmax(groups->cell_size(), q.eps * 4.0f);
    const float t = pick::next_visible_crossing(field, r, out->t + step * 0.5f, q.tmax, step,
                                                *groups);
    if (t < 0.0f) {
        out->hit = 0;
        return eval::Status::Ok;
    }
    const kernel::cfloat3 p = r.at(t);
    out->t = t;
    out->pos[0] = p.x;
    out->pos[1] = p.y;
    out->pos[2] = p.z;
    const kernel::cfloat3 n = kernel::cnormal(field, p, 1e-4f);
    out->normal[0] = n.x;
    out->normal[1] = n.y;
    out->normal[2] = n.z;
    return eval::Status::Ok;
}

// Surface measures. FILE SCOPE, above the first extern "C" — a helper defined
// inside that block is what broke the macOS and Windows builds in #235, and GCC
// does not warn about it.
clay_result to_measure(clay_surface_measure in, brush::SurfaceMeasure* out) {
    switch (in) {
        case CLAY_MEASURE_CURVATURE: *out = brush::SurfaceMeasure::Curvature; return CLAY_OK;
        case CLAY_MEASURE_CAVITY: *out = brush::SurfaceMeasure::Cavity; return CLAY_OK;
        case CLAY_MEASURE_CONVEXITY: *out = brush::SurfaceMeasure::Convexity; return CLAY_OK;
        case CLAY_MEASURE_NORMAL_DIR: *out = brush::SurfaceMeasure::NormalDirection; return CLAY_OK;
        case CLAY_MEASURE_OCCLUSION: *out = brush::SurfaceMeasure::AmbientOcclusion; return CLAY_OK;
        case CLAY_MEASURE_THICKNESS: *out = brush::SurfaceMeasure::Thickness; return CLAY_OK;
    }
    return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown surface measure");
}

// Null params means "the defaults", which is what a caller measuring curvature
// with no opinion about the stencil wants — and is why every field has a
// meaningful zero-derived value rather than requiring the struct.
clay_result read_measure_params(const clay_measure_params* params,
                                brush::MeasureSettings* out) {
    if (!params) return CLAY_OK;  // *out is already default-constructed
    clay_measure_params p;
    clay_result r = read_desc(params, kMeasureParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    out->h = p.h;
    out->scale = p.scale;
    out->direction = kernel::cf3(p.direction[0], p.direction[1], p.direction[2]);
    out->threshold = p.threshold;
    out->ray_length = p.ray_length;
    out->ray_count = p.ray_count;
    out->falloff = p.falloff;
    out->seed = p.seed;
    return CLAY_OK;
}

// Surface groups. FILE SCOPE, above the first extern "C" — a helper defined
// inside that block is what broke the macOS and Windows builds in #235, and GCC
// does not warn about it, so a green local build proves nothing.
const voxel::GroupField* group_field(const clay_groups* groups) {
    if (!groups || !groups->doc || !groups->doc->doc.groups) return nullptr;
    return &*groups->doc->doc.groups;
}

voxel::GroupField* group_field_mut(clay_groups* groups) {
    if (!groups || !groups->doc || !groups->doc->doc.groups) return nullptr;
    return &*groups->doc->doc.groups;
}

const voxel::MaskField* mask_of(const clay_mask* mask) {
    voxel::MaskField* m = nullptr;
    return resolve_mask(mask, &m) == CLAY_OK ? m : nullptr;
}

// Brackets a group edit so it becomes one undo step, on the same RAII shape
// MaskStep uses. Every mutating group entry point takes one, which is what
// keeps eleven call sites from each having to remember — the omission that made
// `record_barrier` a documented lie until unify-the-undo-history wired it.
//
// A standalone lattice cannot occur (a group handle is always a borrow), so
// unlike MaskStep there is no not-in-a-document case to fall through.
struct GroupStep {
    clay_document* doc = nullptr;
    voxel::GroupField* field = nullptr;

    explicit GroupStep(clay_groups* groups) {
        if (!groups || !groups->doc || !groups->doc->undo) return;
        voxel::GroupField* g = group_field_mut(groups);
        if (!g || !groups->doc->undo->begin_group_step(*g)) return;
        doc = groups->doc;
        field = g;
    }
    ~GroupStep() {
        if (doc) doc->undo->end_group_step(*field);
    }
    GroupStep(const GroupStep&) = delete;
    GroupStep& operator=(const GroupStep&) = delete;
};

// The shapes of argument list the voxel entry points start with, checked once
// here so each entry point below is its own engine call and nothing else.
clay_result resolve_at(const clay_voxel_grid* grid, const std::int32_t cell[3],
                       voxel::VoxelGrid** out) {
    clay_result r = resolve(grid, out);
    if (r != CLAY_OK) return r;
    if (!cell) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    return CLAY_OK;
}

clay_result resolve_batch(const clay_voxel_grid* grid, const std::int32_t* cells_xyz,
                          std::size_t count, voxel::VoxelGrid** out) {
    clay_result r = resolve(grid, out);
    if (r != CLAY_OK) return r;
    if (count > 0 && !cells_xyz) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cells");
    return check_batch("cells", count);
}

clay_result resolve_at_index(const clay_voxel_grid* grid, const std::int32_t cell[3],
                             std::int32_t index, voxel::VoxelGrid** out_grid,
                             std::uint8_t* out_index) {
    clay_result r = resolve_at(grid, cell, out_grid);
    if (r != CLAY_OK) return r;
    return check_palette_index(index, out_index);
}

clay_result resolve_brush(const clay_voxel_grid* grid, const std::int32_t cell[3],
                          const clay_brush_params* brush, voxel::VoxelGrid** out_grid,
                          voxel::BrushParams* out_brush) {
    clay_result r = resolve_at(grid, cell, out_grid);
    if (r != CLAY_OK) return r;
    if (!brush) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brush parameters");
    return read_brush(brush, out_brush);
}

// -- evaluation, picking and meshing helpers ---------------------------------

void write_f3(float* out, kernel::cfloat3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

// Origin and direction as the engine's ray. The direction is normalized here
// rather than required to be: the tracer walks along the vector it is given,
// so an unnormalized one silently rescales t, and the Python bindings
// normalize for the same reason. A zero-length one has no direction at all and
// is rejected, which is the one place this boundary is stricter than they are.
clay_result make_ray(const float origin[3], const float dir[3], math::Ray* out) {
    if (!origin || !dir) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null origin or direction");
    kernel::cfloat3 d = kernel::cf3(dir[0], dir[1], dir[2]);
    if (!(kernel::clength(d) >= 1e-12f))  // also rejects NaN
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "ray direction must be non-zero");
    out->origin = kernel::cf3(origin[0], origin[1], origin[2]);
    out->dir = kernel::cnormalize(d);
    return CLAY_OK;
}

// The same, for a batch: the rays are copied so the caller's buffer is never
// written, and one bad direction rejects the call rather than the ray.
clay_result normalize_rays(const float* src, std::size_t count, std::vector<float>* out) {
    clay_result r = check_batch("rays", count);
    if (r != CLAY_OK) return r;
    if (count > 0) out->assign(src, src + count * 6);
    for (std::size_t i = 0; i < count; ++i) {
        float* d = out->data() + i * 6 + 3;
        kernel::cfloat3 v = kernel::cf3(d[0], d[1], d[2]);
        if (!(kernel::clength(v) >= 1e-12f))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "ray direction must be non-zero");
        write_f3(d, kernel::cnormalize(v));
    }
    return CLAY_OK;
}

// Each of a batch raycast's four results is a buffer the caller may not have
// asked for, so the scatter is its own pass rather than four tests inside the
// query.
void write_ray_hits(const std::vector<eval::RayHit>& hits, std::size_t count,
                    std::int32_t* out_hits, float* out_t, float* out_positions_xyz,
                    float* out_normals_xyz) {
    for (std::size_t i = 0; i < count; ++i) {
        if (out_hits) out_hits[i] = hits[i].hit;
        if (out_t) out_t[i] = hits[i].t;
        if (out_positions_xyz)
            std::memcpy(out_positions_xyz + i * 3, hits[i].pos, sizeof hits[i].pos);
        if (out_normals_xyz)
            std::memcpy(out_normals_xyz + i * 3, hits[i].normal, sizeof hits[i].normal);
    }
}

// Neither of the box predicates catches a box a caller derived from a camera
// frustum or a degenerate selection: empty() and is_infinite() are both
// comparisons and every comparison against NaN is false, and is_infinite()
// tests for +/-FLT_MAX, which an actual infinity is not. Both then reach the
// engine's float-to-int cell conversion — undefined for a NaN, and an
// unbounded loop for an infinity. Finiteness is therefore checked first, on
// the same footing as clay_voxel_grid_create's `!(voxel_size > 0.0f)`.
bool box_is_finite(const math::Aabb& box) {
    return std::isfinite(box.min.x) && std::isfinite(box.min.y) && std::isfinite(box.min.z) &&
           std::isfinite(box.max.x) && std::isfinite(box.max.y) && std::isfinite(box.max.z);
}

// A box reaches the caller as two triples plus a flag, because "no bounds" is
// a real answer — an empty layer, a selection of ids the layer does not hold —
// and not a failure; the Python bindings answer it with None.
clay_result write_bounds(const math::Aabb& box, float out_min[3], float out_max[3],
                         std::int32_t* out_has_bounds) {
    bool has = !box.empty();
    if (out_has_bounds) *out_has_bounds = has ? 1 : 0;
    if (!has) return CLAY_OK;
    if (out_min) write_f3(out_min, box.min);
    if (out_max) write_f3(out_max, box.max);
    return CLAY_OK;
}

// Where a layer IS, answered from whichever representation the layer actually
// holds (issue #318).
//
// pick::layer_bounds walks scene::Layer::sdf and can do nothing else: a
// scene::Layer HOLDS only SDF content, and the voxel grid and the mesh live in
// side tables keyed by layer id because check_layering.py withholds clay/voxel
// and clay/mesh from clay::scene. That is the invariant working, not a gap to
// route around, so the composition happens HERE, where the document that owns
// all three is in scope.
//
// Both non-SDF kinds reported nothing however much material they held. A mesh
// cannot be unbounded -- its vertices ARE a box -- and a grid says where it is
// itself, so "no bounds" was never the truth for either; it was the answer a
// walk of the wrong container gives.
//
// WORLD SPACE, like the SDF arm, which composes layer.xform with the node's
// own. A caller comparing two layers, framing a camera or placing a manipulator
// is asking one question, and it would be answered in two different spaces
// otherwise.
math::Aabb layer_world_bounds(const clay_document* doc, const scene::Layer& layer) {
    if (layer.kind == scene::LayerKind::Sdf) return pick::layer_bounds(layer);

    math::Aabb local;  // default-constructed is the empty box
    if (layer.kind == scene::LayerKind::Voxel) {
        auto it = doc->doc.voxel_layers.find(layer.id);
        if (it == doc->doc.voxel_layers.end()) return local;
        const voxel::VoxelGrid& grid = it->second;
        const std::optional<voxel::VoxelCoord> lo = grid.bounds_min();
        const std::optional<voxel::VoxelCoord> hi = grid.bounds_max();
        if (!lo || !hi) return local;  // no material: genuinely nowhere
        const float vs = grid.voxel_size();
        if (!(vs > 0.0f)) return local;
        // A cell is a BOX, not a point: cell i spans [i*vs, (i+1)*vs], which is
        // the convention build_plane_pick reads back with floor(p / vs). So the
        // far corner takes the +1, and a single occupied cell has the extent of
        // one cell rather than zero.
        local.expand(kernel::cf3(static_cast<float>(lo->x) * vs, static_cast<float>(lo->y) * vs,
                                 static_cast<float>(lo->z) * vs));
        local.expand(kernel::cf3(static_cast<float>(hi->x + 1) * vs,
                                 static_cast<float>(hi->y + 1) * vs,
                                 static_cast<float>(hi->z + 1) * vs));
    } else if (layer.kind == scene::LayerKind::Mesh) {
        auto it = doc->doc.mesh_layers.find(layer.id);
        if (it == doc->doc.mesh_layers.end()) return local;
        for (const kernel::cfloat3& p : it->second.positions) local.expand(p);
    }
    if (local.empty()) return local;  // transforming an empty box invents one
    return local.transformed(scene::layer_matrix(layer));
}

// A cell reaches the caller as three int32 values, which is exactly the
// engine's layout (asserted at the top of this file).
void write_cell(std::int32_t out[3], voxel::VoxelCoord c) {
    std::memcpy(out, &c, sizeof c);
}

// Distances, colors and gradients come off one backend call and differ only in
// which buffer they fill, so the backend lookup lives here once. The tape is
// the caller's: a document compiles the whole stack, a layer only itself.
clay_result eval_into(const scene::Tape& tape, const char* backend, const float* points_xyz,
                      std::size_t count, const eval::PointResults& out) {
    clay_result r = check_batch("points", count);
    if (r != CLAY_OK) return r;
    const char* name = backend ? backend : "cpu";
    eval::Backend* b = eval::Registry::instance().find(name);
    if (!b)
        return fail(CLAY_ERROR_NOT_FOUND, std::string("backend not registered: ") + name);
    eval::PointQuery q{points_xyz, count, 1e-4f};
    if (b->eval_points(tape, q, out) != eval::Status::Ok)
        return fail(CLAY_ERROR_BACKEND, "eval_points failed");
    return CLAY_OK;
}

// The backends fill distances alongside whatever else was asked for, so the
// ones a gradients-only caller does not want still need somewhere to land.
clay_result gradients_into(const scene::Tape& tape, const char* backend,
                           const float* points_xyz, std::size_t count, float* out_gradients_xyz) {
    clay_result r = check_batch("points", count);  // before the scratch buffer, not after
    if (r != CLAY_OK) return r;
    std::vector<float> distances(count ? count : 1);
    return eval_into(tape, backend, points_xyz, count,
                     eval::PointResults{distances.data(), out_gradients_xyz, nullptr});
}

// One layer's own field, which is what the Python bindings' Layer.eval answers:
// an edit in a layer above cannot change it, so a layer can be probed while the
// stack it sits in is being authored.
clay_result compile_one_layer(const clay_document* doc, clay_layer_id layer_id,
                              scene::Tape* out) {
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    *out = scene::compile_layer(*layer);
    return CLAY_OK;
}

// Mesher dispatch and the one gate the meshing spec puts on it: plain dual
// contouring is not guaranteed manifold, so it is reachable only when the
// caller opts in. The refusal happens here rather than in the engine, which
// answers an unflagged call with an empty mesh — indistinguishable, one line
// later, from a document that meshed to nothing.
clay_result mesh_with(std::int32_t mesher, bool experimental, const scene::Tape& tape,
                      const math::Aabb& region, float voxel, mesh::Mesh* out) {
    if (!mesher_is_known(mesher))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown mesher: " + std::to_string(mesher));
    if (mesher == CLAY_MESHER_DUAL_CONTOURING && !experimental)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the dual_contouring mesher is experimental: set "
                    "clay_mesh_params.experimental to opt in");
    if (mesher == CLAY_MESHER_NETS) {
        *out = mesh::mesh_tape_nets(tape, region, voxel);
    } else if (mesher == CLAY_MESHER_DUAL_CONTOURING) {
        mesh::DualContouringOptions dc;
        dc.enable_experimental = true;
        *out = mesh::mesh_tape_dc(tape, region, voxel, dc);
    } else {
        *out = mesh::mesh_tape(tape, region, voxel);
    }
    return CLAY_OK;
}

// The quad descriptor, read and checked once for both sources. The count
// controls land in a mesh::QuadTarget, which is the same struct the engine's
// own search takes — there is no second set of defaults here to drift from the
// ones the header documents.
clay_result read_quad_params(const clay_quad_params* params, clay_quad_params* out,
                             mesh::QuadTarget* target) {
    clay_result r = read_desc(params, kQuadParamsOriginal, out);
    if (r != CLAY_OK) return r;
    if (!quad_mode_is_known(out->mode))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown quad mode: " + std::to_string(out->mode));
    if (out->cell_size != 0.0f && !std::isfinite(out->cell_size))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "cell size must be finite");
    if (!std::isfinite(out->tolerance))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "tolerance must be finite; <= 0 means the default");
    // A tolerance of 1 or more is 100% of the target, which makes
    // within_tolerance true for almost any count and so reports nothing. Bound
    // here rather than in the engine so pyclay and this binding refuse the
    // same value with the same words.
    if (out->tolerance >= 1.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "tolerance is a fraction of the target, below 1; <= 0 means the default");
    // Bounded here rather than in the engine: every iteration is a whole mesh,
    // so a caller who passed a byte count or a negative widened by mistake
    // would otherwise buy that many dense field evaluations.
    if (out->max_iterations < 0 || out->max_iterations > 64)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "max_iterations must be 0..64; 0 means the default");
    if (out->target_quads > CLAY_MAX_BATCH)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "target_quads above CLAY_MAX_BATCH: " + std::to_string(out->target_quads));
    target->target = static_cast<std::size_t>(out->target_quads);
    target->tolerance = out->tolerance;
    target->max_iterations = out->max_iterations;
    return CLAY_OK;
}

clay_voxel_grid* borrow_layer(clay_document* doc, clay_layer_id layer) {
    clay_voxel_grid& handle = doc->voxel_handles[layer];
    handle.doc = doc;
    handle.layer = layer;
    return &handle;
}

// A mesh layer's geometry revision, and why it lives on the handle rather than
// on the mesh.
//
// `resolve_sculptor` compares `mesh_data_mut(s->mesh)` against the pointer the
// sculptor was built over, which catches a layer that was REMOVED — a
// std::map node's address is stable, so it does not catch the layer's contents
// being replaced underneath. `MeshSculptor::valid()` catches a changed vertex
// or index COUNT, which catches most replacements and misses the one that
// matters most: a rebuild that happens to land on the same counts leaves a live
// sculptor with a silently wrong adjacency and BVH over entirely different
// triangles.
//
// A counter closes that. Bumped only by a wholesale replacement, never by a
// sculpt, because a sculpt is exactly the change those caches are built to
// survive.
std::uint64_t mesh_layer_revision_of(const clay_document* doc, clay_layer_id layer) {
    auto it = doc->mesh_geometry_revision.find(layer);
    return it == doc->mesh_geometry_revision.end() ? 1u : it->second;
}

clay_mesh* borrow_mesh_layer(clay_document* doc, clay_layer_id layer) {
    clay_mesh& handle = doc->mesh_handles[layer];
    handle.doc = doc;
    handle.layer = layer;
    return &handle;
}

bool point_type_is_known(std::int32_t t) {
    return t >= 0 && t <= static_cast<std::int32_t>(scene::StrokePointType::Bezier);
}

// The one place a caller's point arrays become StrokePoints, so the plain and
// the typed setters cannot drift apart in what they accept.
clay_result read_curve_points(const float* xyzr, std::size_t count, const std::int32_t* types,
                              const float* in_handles_xyz, const float* out_handles_xyz,
                              std::vector<scene::StrokePoint>* out) {
    clay_result r = check_payload("stroke points", xyzr, count);
    if (r != CLAY_OK) return r;
    out->clear();
    out->reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float* p = xyzr + i * 4;
        if (p[3] < 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke radius must be >= 0");
        scene::StrokePoint sp;
        sp.pos = kernel::cf3(p[0], p[1], p[2]);
        sp.radius = p[3];
        if (types) {
            if (!point_type_is_known(types[i]))
                return fail(CLAY_ERROR_INVALID_ARGUMENT,
                            "unknown point type: " + std::to_string(types[i]));
            sp.type = static_cast<scene::StrokePointType>(types[i]);
        }
        if (in_handles_xyz)
            sp.in_handle = kernel::cf3(in_handles_xyz[i * 3 + 0], in_handles_xyz[i * 3 + 1],
                                       in_handles_xyz[i * 3 + 2]);
        if (out_handles_xyz)
            sp.out_handle = kernel::cf3(out_handles_xyz[i * 3 + 0], out_handles_xyz[i * 3 + 1],
                                        out_handles_xyz[i * 3 + 2]);
        out->push_back(sp);
    }
    return CLAY_OK;
}

// The one place StrokePoints become a caller's arrays, so the readback and the
// setters above cannot drift apart in what they mean. xyzr is required, as it
// is there; the other three are the same optional parallel arrays.
void write_curve_points(const std::vector<scene::StrokePoint>& points, float* xyzr,
                        std::int32_t* types, float* in_handles_xyz, float* out_handles_xyz) {
    for (std::size_t i = 0; i < points.size(); ++i) {
        const scene::StrokePoint& p = points[i];
        write_f3(xyzr + i * 4, p.pos);
        xyzr[i * 4 + 3] = p.radius;
        if (types) types[i] = static_cast<std::int32_t>(p.type);
        if (in_handles_xyz) write_f3(in_handles_xyz + i * 3, p.in_handle);
        if (out_handles_xyz) write_f3(out_handles_xyz + i * 3, p.out_handle);
    }
}

// Resolving a placed curve for READING. Protection guards edits, so a ghosted
// or locked layer resolves here like any other; the typed errors are the ones
// the setters already report for the same mistakes.
clay_result find_curve_node(const clay_document* doc, clay_layer_id layer, clay_node_id node,
                            const scene::Node** out) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const scene::Node* n = l->sdf ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no stroke or curve with that id in that layer");
    // An armature's nodes are the same x, y, z, radius list, so the READ side
    // serves them too — the same set of prims the item-side setter accepts.
    // The placed-node SETTER stays narrower on purpose: replacing an
    // armature's points alone would desynchronise them from its parents, and
    // clay_layer_armature_edit already owns that half.
    if (n->is_group || !(scene::prim_carries_curve(n->prim.type) ||
                         scene::prim_is_armature(n->prim.type)))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "curve points need CLAY_PRIM_STROKE, CLAY_PRIM_SWEPT or "
                    "CLAY_PRIM_ARMATURE");
    *out = n;
    return CLAY_OK;
}

// The node an edit names, or null — for the entry points whose rules differ
// between a group and an item. A miss is deliberately NOT reported here: the
// edit still routes through apply_edit, which is the one place that tells a
// missing layer apart from a protected one.
const scene::Node* peek_node(const clay_document* doc, clay_layer_id layer, clay_node_id node) {
    if (!doc) return nullptr;
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    return (l && l->sdf) ? l->sdf->find(node) : nullptr;
}

bool node_is_swept(const clay_document* doc, clay_layer_id layer, clay_node_id node) {
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    const scene::Node* n = (l && l->sdf) ? l->sdf->find(node) : nullptr;
    return n && scene::prim_is_swept(n->prim.type);
}

clay_mask* borrow_mask_handle(clay_document* doc, clay_layer_id layer) {
    clay_mask& handle = doc->mask_handles[layer];
    handle.doc = doc;
    handle.layer = layer;
    return &handle;
}

// The flat descriptor as a builder: same fields, same insertion path. The
// params block is copied whole, so a descriptor keeps meaning exactly what
// it meant before the builder existed.
clay_item item_from_desc(const clay_item_desc& d) {
    clay_item item;
    scene::Node& node = item.node;
    node.prim.type = static_cast<scene::PrimType>(d.prim);
    std::memcpy(node.prim.params, d.params, sizeof node.prim.params);
    node.xform.position = kernel::cf3(d.position[0], d.position[1], d.position[2]);
    node.xform.rotation =
        math::Quat{d.rotation[0], d.rotation[1], d.rotation[2],
                   d.rotation[3] == 0 && d.rotation[0] == 0 && d.rotation[1] == 0 &&
                           d.rotation[2] == 0
                       ? 1.0f
                       : d.rotation[3]};
    node.xform.scale = d.scale <= 0 ? 1.0f : d.scale;
    node.op = static_cast<scene::Op>(d.op);
    node.blend = scene::Blend{static_cast<scene::BlendProfile>(d.blend), d.blend_k};
    node.rounding = d.rounding;
    node.color = kernel::cf3(d.color[0], d.color[1], d.color[2]);
    // Negative excludes the item from the layer's mirror; 0 (a zeroed
    // descriptor) and 1 both follow it. 0 used to mean excluded, which made
    // clay_set_layer_mirror a silent no-op for every host that did not also
    // flag every item (#60).
    node.mirror = d.mirror >= 0;
    return item;
}

// The one command shape whose result a later compile can resume from: a
// subtree appended at the TAIL of a layer's root list, which is what a brush
// stamp is. `layer` is 0 for every other edit — including an insert one place
// short of the end, which reads almost the same here and moves the compiled
// prefix, so it must fall through to the general invalidation.
struct TailAppend {
    scene::LayerId layer = 0;
    scene::NodeId node = 0;
};

// Is `node` the LAST member of its chain, and is every group above it the last
// of its own, all the way to the root list? That is what makes an append into
// it a tail append: the compiled prefix ends where the new node goes, and the
// only thing between there and the end of the tape is the ancestors' combines.
//
// A group that is NOT last has the same reuse point, but everything after it
// would have to be recompiled — unbounded, where a tail-in-tail append is the
// ancestors' combines and nothing else.
bool is_tail_in_tail(const scene::SdfContent& content, scene::NodeId node) {
    scene::NodeId cur = node;
    for (std::size_t step = 0; step <= content.nodes().size(); ++step) {
        scene::NodeId parent = scene::kNoNode;
        int index = -1;
        if (!content.locate(cur, &parent, &index)) return false;
        const std::vector<scene::NodeId>& siblings =
            parent == scene::kNoNode ? content.roots : content.find(parent)->children;
        if (siblings.empty() || siblings.back() != cur) return false;
        if (parent == scene::kNoNode) return true;
        const scene::Node* g = content.find(parent);
        if (!g || !g->is_group) return false;
        // An inline group has no chain of its own — its children continue the
        // outer one — so the compiler never takes a checkpoint in front of a
        // combine for it, and there is no frame to unwind. Refused rather than
        // reasoned about.
        if (g->op == scene::Op::None) return false;
        // A non-local combine above the append makes the whole subtree's reach
        // infinite, which is the one shape whose prefix cannot be trusted to
        // still be a prefix.
        if (!scene::op_is_local(g->op)) return false;
        cur = parent;
    }
    return false;
}

TailAppend tail_append(const scene::Document& doc, const scene::Command& cmd) {
    const auto* add = std::get_if<scene::AddNodeCmd>(&cmd);
    if (!add || add->index != -1) return {};
    const scene::Layer* l = doc.find_layer(add->layer);
    if (!l || l->kind != scene::LayerKind::Sdf || !l->sdf || l->sdf->roots.empty()) return {};
    // An append into a GROUP qualifies when that group is in tail position all
    // the way up. `tail_append` used to require the root list outright, which
    // is what made a dab placed inside a group cost 90x a dab placed beside it
    // on the reference iPad — the append fast path switched off by a modelling
    // decision with no performance meaning.
    if (add->parent != scene::kNoNode && !is_tail_in_tail(*l->sdf, add->parent)) return {};
    // And no other layer may share this one's content. An append to shared
    // content grows the edit list of every layer holding it, while the append
    // fast path is per LAYER: it would extend the cached tape for the layer
    // named and leave every instance of it stale, so an edit through one
    // subtool would stop appearing in its duplicates — visibly, and only in
    // the cached path. Refused the way command_frontier refuses the same
    // situation below: the legacy region drop, and a full recompile.
    for (const scene::Layer& other : doc.layers)
        if (&other != l && other.sdf == l->sdf) return {};
    // The tail of the chain the node was added to, read back from the TREE
    // rather than taken from the command's own subtree: this cannot then
    // disagree with what apply() actually did to the list.
    const std::vector<scene::NodeId>& chain =
        add->parent == scene::kNoNode ? l->sdf->roots : l->sdf->find(add->parent)->children;
    if (chain.empty()) return {};
    return TailAppend{add->layer, chain.back()};
}

// The commands that can move a root ordinal or change WHICH layer is the last
// visible SDF layer. Either breaks what a frontier ordinal means, so these
// take the structural drop: same spatial behaviour, plus the structure bump
// that retires every prefix seed. (A tail AddNodeCmd never reaches this --
// tail_append routes it to touch_appended, which bumps structure itself.)
bool command_is_structural(const scene::Command& cmd) {
    return std::holds_alternative<scene::AddNodeCmd>(cmd) ||
           std::holds_alternative<scene::RemoveNodeCmd>(cmd) ||
           std::holds_alternative<scene::MoveNodeCmd>(cmd) ||
           std::holds_alternative<scene::AddLayerCmd>(cmd) ||
           std::holds_alternative<scene::RemoveLayerCmd>(cmd) ||
           std::holds_alternative<scene::SetLayerVisibleCmd>(cmd);
}

// The root-list ordinal of the root subtree holding `node`: chase locate()
// parents to kNoNode, whose sibling index IS the root ordinal. O(depth), no
// cache, no invalidation triggers to get wrong. Bounded like root_ancestor in
// scene/commands.cpp: `roots` is a public vector and the walk must terminate
// whatever a caller wrote there.
bool root_ordinal_of(const scene::SdfContent& content, scene::NodeId node, std::uint32_t* out) {
    scene::NodeId cur = node;
    for (std::size_t step = 0; step <= content.nodes().size(); ++step) {
        scene::NodeId parent = scene::kNoNode;
        int index = -1;
        if (!content.locate(cur, &parent, &index)) return false;
        if (parent == scene::kNoNode) {
            *out = static_cast<std::uint32_t>(index);
            return true;
        }
        cur = parent;
    }
    return false;
}

// Whether `cmd` is a (layer, node) PARAMETER edit the frontier path can carry
// -- and if so, the root ordinal it dirties from (#360).
struct CommandFrontier {
    bool usable = false;
    std::uint32_t ordinal = 0;
};

CommandFrontier command_frontier(const scene::Document& doc, const scene::Command& cmd) {
    // The (layer, node) parameter commands and nothing else. Structural
    // commands are listed in command_is_structural; layer-parameter commands
    // (mirror/radial/transform) reach the whole layer, and the legacy region
    // drop already says everything true about them.
    scene::LayerId layer = 0;
    scene::NodeId node = scene::kNoNode;
    const bool parameter = std::visit(
        [&](const auto& c) -> bool {
            using C = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<C, scene::SetTransformCmd> ||
                          std::is_same_v<C, scene::SetPrimCmd> ||
                          std::is_same_v<C, scene::SetColorCmd> ||
                          std::is_same_v<C, scene::SetOpBlendCmd> ||
                          std::is_same_v<C, scene::SetDeformersCmd> ||
                          std::is_same_v<C, scene::SetStrokePointsCmd> ||
                          std::is_same_v<C, scene::AppendStrokeCmd> ||
                          std::is_same_v<C, scene::TrimStrokeCmd> ||
                          std::is_same_v<C, scene::SetArmatureCmd>) {
                layer = c.layer;
                node = c.node;
                return true;
            } else {
                return false;
            }
        },
        cmd);
    if (!parameter) return {};
    // The edit must land on the ACTIVE layer -- the last visible SDF layer,
    // the only one whose chain the stored seeds hold as their own value. A
    // non-active-layer edit changes the BELOW half of every seed it reaches,
    // which the frontier path carries forward untouched, so it must take the
    // legacy drop.
    const scene::Layer* active = nullptr;
    for (const scene::Layer& l : doc.layers) {
        if (!l.visible || l.kind != scene::LayerKind::Sdf || !l.sdf) continue;
        active = &l;
    }
    if (!active || active->id != layer) return {};
    // And no other visible SDF layer may share the active layer's content.
    // Instancing compiles the same node again under the lower layer's
    // transform, so an edit through the active layer also moves the below
    // half -- the same family of silent corruption as the layer gate in
    // plan_resume (#354), refused the same way: legacy drop, full walk.
    for (const scene::Layer& l : doc.layers) {
        if (&l == active) continue;
        if (l.visible && l.kind == scene::LayerKind::Sdf && l.sdf == active->sdf) return {};
    }
    CommandFrontier f;
    if (!root_ordinal_of(*active->sdf, node, &f.ordinal)) return {};
    f.usable = true;
    return f;
}

// Every edit below routes through the command vocabulary rather than touching
// the document, so a C edit means what a saved document means — and becomes
// undoable for free once the undo stack is exposed. apply() reports failure by
// returning nullopt and leaves the document untouched.
// May this edit be applied at all? Shared, so apply_edit and the gesture path
// cannot drift about what an edit is allowed to do or how a refusal reads.
//
// Separate from the apply because the ORDER matters on the cost side: the
// caller checks this BEFORE computing any influence bound, so a refused edit
// pays nothing for a region it will not touch.
clay_result edit_guard(const clay_document* doc, const scene::Command& cmd) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    // apply() says no for two different reasons, and a caller needs to tell
    // them apart: a missing layer is a bug in the caller's bookkeeping, while
    // a protected one is a state the artist chose and a UI can explain.
    const scene::LayerId target = scene::edited_layer(cmd);
    if (target != 0) {
        const scene::Layer* l = doc->doc.document.find_layer(target);
        if (l && l->protected_from_edits())
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        std::string("layer ") + std::to_string(target) + " is " +
                            (l->ghost ? "ghosted" : "locked") + " and takes no edits");
    }
    return CLAY_OK;
}

// The apply itself. With a stack attached the edit is applied AND its inverse
// recorded, so no reachable edit can escape undo.
clay_result perform_edit(clay_document* doc, const scene::Command& cmd, const char* what) {
    const bool ok = doc->undo ? doc->undo->perform(doc->doc.document, cmd)
                              : static_cast<bool>(scene::apply(doc->doc.document, cmd));
    if (!ok) return fail(CLAY_ERROR_NOT_FOUND, what);
    return CLAY_OK;
}

// One command of a gesture whose reach its CALLER knows. No bound is computed
// and no cache is invalidated; the enclosing GestureRegion does that once.
clay_result apply_edit_in_gesture(clay_document* doc, const scene::Command& cmd,
                                  const char* what) {
    const clay_result r = edit_guard(doc, cmd);
    return r != CLAY_OK ? r : perform_edit(doc, cmd, what);
}

// ONE invalidation for a whole gesture, from a region the caller knows.
//
// A gesture that issues one command per node it reaches pays, in apply_edit,
// two `command_influence_bound` calls and one seed-store walk PER COMMAND. A
// Move drag over a 1000-item document issues 257 of them, and that is where
// `layer_move_surface` lost 1.34x when region invalidation landed (#358):
// measured on an M2 Max, 0.094 ms before, 0.126 after, of which the bounds are
// about three quarters.
//
// The region is taken UP FRONT rather than accumulated, because accumulating it
// is what costs. It MUST cover everything the bracket does — a region that does
// not is stale bricks, which is a silent wrong picture rather than a slow one.
// Only a caller that can state its reach analytically should use this; a
// gesture that cannot must keep apply_edit, whose per-command bound is derived
// and therefore always right.
//
// RAII because the invalidation must happen on the failure path too: a gesture
// that applied three of its commands and then refused has still changed the
// document.
struct GestureRegion {
    clay_document* doc = nullptr;
    // One box per place the gesture lands. Several rather than their union
    // because a gesture under a layer mirror lands under the ball AND under
    // its reflection, and the union of those is the slab between them --
    // which under a mirror is the whole document (#363).
    std::vector<math::Aabb> reach;
    // A gesture that states its reach may state its FRONTIER for the same
    // reason (#360): it knows every command it issues is a parameter edit on
    // nodes whose root ordinals it resolved, so the seeds it dirties can keep
    // their prefix half instead of being dropped. A gesture that cannot say
    // that must leave this unset and take the legacy drop. The RAII-on-refusal
    // property is unchanged either way: touch_region_from is still a real
    // invalidation -- every entry in reach is dirtied, just not dropped when
    // its prefix can still serve the stated frontier.
    std::uint32_t frontier = clay_document::kFrontierDrop;

    GestureRegion(clay_document* d, std::vector<math::Aabb> r,
                  std::uint32_t f = clay_document::kFrontierDrop)
        : doc(d), reach(std::move(r)), frontier(f) {}
    ~GestureRegion() {
        if (!doc) return;
        if (frontier == clay_document::kFrontierDrop)
            doc->touch_regions(reach);
        else
            doc->touch_regions_from(reach, frontier);
    }
    GestureRegion(const GestureRegion&) = delete;
    GestureRegion& operator=(const GestureRegion&) = delete;
};

clay_result apply_edit(clay_document* doc, const scene::Command& cmd, const char* what) {
    // What this edit can reach, taken on BOTH sides of the apply and unioned.
    // One side is not an answer: an add's node is not there before, a removal's
    // is not there after, and a move has two ends -- the contract
    // command_influence_bound states and the undo stack already follows.
    // Gathered before the apply because after it the old shape is gone.
    clay_result r = edit_guard(doc, cmd);
    if (r != CLAY_OK) return r;
    const math::Aabb reach_before = scene::command_influence_bound(doc->doc.document, cmd);
    r = perform_edit(doc, cmd, what);
    if (r != CLAY_OK) return r;
    math::Aabb reach = reach_before;
    reach.expand(scene::command_influence_bound(doc->doc.document, cmd));
    // The funnel every command-based edit passes through, so the tape cache is
    // invalidated in one place for all of them — and the one place that can
    // tell the cache an edit was an APPEND, which is what a brush stamp is
    // and what lets the next compile reuse its prefix. Everything else, here
    // and at every other call site, keeps the general invalidation.
    if (const TailAppend appended = tail_append(doc->doc.document, cmd); appended.layer != 0) {
        doc->touch_appended(appended.layer, appended.node);
        return CLAY_OK;
    }
    // Not an append, but its reach is known: a brick the edit cannot touch keeps
    // the seed it already has, so adjusting one item no longer costs every
    // brick the whole edit list. Three fronts (#360): an edit that can move
    // root ordinals retires every prefix seed with it; a parameter edit inside
    // one root of the active layer dirties the seeds it reaches from that
    // ordinal instead of dropping them; everything else keeps the legacy drop.
    // `reach` is already the union of command_influence_bound on both sides of
    // the apply -- root-subtree influence, deformed and blend-dilated, on
    // every instancing layer -- never a primitive's raw box.
    if (command_is_structural(cmd)) {
        doc->touch_region_structural(reach);
        return CLAY_OK;
    }
    if (const CommandFrontier f = command_frontier(doc->doc.document, cmd); f.usable) {
        doc->touch_region_from(reach, f.ordinal);
        return CLAY_OK;
    }
    doc->touch_region(reach);
    return CLAY_OK;
}

// A per-axis scale, checked once for every entry point that takes one. Zero has
// no inverse — it collapses the item onto a plane — and a negative component
// mirrors it, which the layer mirror already expresses and which would flip the
// winding of a boolean without saying so.
clay_result read_scale_axes(const float scale[3], kernel::cfloat3* out) {
    if (!scale) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null scale");
    for (int i = 0; i < 3; ++i)
        if (!(scale[i] > 0.0f))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "every scale component must be > 0");
    *out = kernel::cf3(scale[0], scale[1], scale[2]);
    return CLAY_OK;
}

// A whole transform, checked once for every entry point that takes one. Both
// arrays are REQUIRED: this ABI takes the whole value rather than a partial
// update, so a null one is a missing argument and not a "leave this alone".
// Named separately (#327), because a caller who reads a null axis as "no
// rotation" and gets "null transform" back learns that something was null but
// not which of the two, and the refusal is the only thing standing between
// them and an edit they believe landed.
clay_result read_transform(const float position[3], const float rotation_axis[3],
                           float rotation_angle, float scale, math::Transform* out) {
    if (!position) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null position");
    if (!rotation_axis)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "null rotation axis: name an axis and pass angle 0 for no rotation");
    if (!(scale > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "scale must be > 0");
    out->position = kernel::cf3(position[0], position[1], position[2]);
    kernel::cfloat3 axis = kernel::cf3(rotation_axis[0], rotation_axis[1], rotation_axis[2]);
    if (!(kernel::cdot2(axis) > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "rotation axis must be non-zero");
    out->rotation = math::Quat::from_axis_angle(axis, rotation_angle);
    out->scale = scale;
    return CLAY_OK;
}

// -- influence bounds, dense grids and the brick cache -----------------------

// An influence bound reaches the caller as three states through two flags,
// matching clay_layer_bounds: nothing to dirty, a finite box, or unbounded.
// The unbounded case does NOT write out_min/out_max — handing back +/-FLT_MAX
// would look like a region a host could pass to mark_dirty, and that is
// precisely the region mark_dirty refuses.
clay_result write_influence(const math::Aabb& box, float out_min[3], float out_max[3],
                            std::int32_t* out_has_bounds, std::int32_t* out_infinite) {
    const bool infinite = box.is_infinite();
    if (out_infinite) *out_infinite = infinite ? 1 : 0;
    if (infinite) {
        if (out_has_bounds) *out_has_bounds = 1;
        return CLAY_OK;
    }
    return write_bounds(box, out_min, out_max, out_has_bounds);
}

// An optional (min, max) pair as clay_voxel_rasterize takes one: both null is
// "no region", one without the other is a mistake rather than a default, and a
// region that is empty or carries a non-finite bound is refused rather than
// converted (see box_is_finite above for why empty() cannot catch it).
clay_result read_region(const float region_min[3], const float region_max[3], const char* what,
                        bool* out_present, math::Aabb* out) {
    *out_present = false;
    if ((region_min == nullptr) != (region_max == nullptr))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string(what) + " needs both bounds or neither");
    if (!region_min) return CLAY_OK;
    math::Aabb box{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                   kernel::cf3(region_max[0], region_max[1], region_max[2])};
    if (!box_is_finite(box))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string(what) + " must be finite on every axis");
    if (box.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, std::string(what) + " is empty");
    *out_present = true;
    *out = box;
    return CLAY_OK;
}

// count * per, computed where it cannot wrap, and required to be exactly the
// caller's capacity. A count is the one argument this boundary cannot check
// against the caller's memory, so it is not inferred.
clay_result exact_capacity(const char* what, std::size_t count, std::size_t per,
                           std::size_t capacity) {
    const std::uint64_t total = static_cast<std::uint64_t>(count) * per;
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (total > static_cast<std::uint64_t>(static_cast<std::size_t>(-1)))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, std::string("too many ") + what);
    }
    if (capacity != static_cast<std::size_t>(total))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("the ") + what + " buffer must hold exactly " +
                        std::to_string(total) + " values, got " + std::to_string(capacity));
    return CLAY_OK;
}

// The same rule for a buffer that may be absent: exactly count * per when it is
// there, and exactly nothing when it is not. A capacity declared for a buffer
// that was not passed is a caller who forgot the pointer, not a caller being
// generous, so it is refused rather than ignored.
clay_result optional_capacity(const char* what, const void* buffer, std::size_t count,
                              std::size_t per, std::size_t capacity) {
    if (buffer) return exact_capacity(what, count, per, capacity);
    if (capacity != 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("no ") + what + " buffer, but a non-zero capacity");
    return CLAY_OK;
}

clay_result eval_grid_into(const scene::Tape& tape, const char* backend,
                           const eval::GridQuery& q, float* out_values,
                           float* out_colors_rgb) {
    const char* name = backend ? backend : "cpu";
    eval::Backend* b = eval::Registry::instance().find(name);
    if (!b) return fail(CLAY_ERROR_NOT_FOUND, std::string("backend not registered: ") + name);
    if (b->eval_grid(tape, q, out_values, out_colors_rgb) != eval::Status::Ok)
        return fail(CLAY_ERROR_BACKEND, "eval_grid failed");
    return CLAY_OK;
}

// A lattice as the engine's query, with every value the backend would trust
// checked first: a non-finite origin or spacing produces a grid of NaNs rather
// than an error, and a dims product is a count, so it passes check_batch.
clay_result read_grid(const float origin[3], float spacing, const std::int32_t dims[3],
                      eval::GridQuery* out, std::size_t* out_samples) {
    if (!(spacing > 0.0f) || !std::isfinite(spacing))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "grid spacing must be finite and > 0");
    if (!std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(origin[2]))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "grid origin must be finite");
    for (int a = 0; a < 3; ++a)
        if (dims[a] <= 0)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "every grid dimension must be > 0");
    const std::uint64_t samples = static_cast<std::uint64_t>(dims[0]) *
                                  static_cast<std::uint64_t>(dims[1]) *
                                  static_cast<std::uint64_t>(dims[2]);
    if (samples > CLAY_MAX_BATCH)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the grid holds " + std::to_string(samples) +
                        " samples, above the batch limit of " + std::to_string(CLAY_MAX_BATCH));
    out->origin = kernel::cf3(origin[0], origin[1], origin[2]);
    out->spacing = spacing;
    out->nx = dims[0];
    out->ny = dims[1];
    out->nz = dims[2];
    *out_samples = static_cast<std::size_t>(samples);
    return CLAY_OK;
}

std::int32_t to_c_brick_state(brick::BrickState s) {
    switch (s) {
        case brick::BrickState::Inside: return CLAY_BRICK_INSIDE;
        case brick::BrickState::Outside: return CLAY_BRICK_OUTSIDE;
        case brick::BrickState::Surface: return CLAY_BRICK_SURFACE;
    }
    return CLAY_BRICK_MISSING;
}

std::int32_t to_c_submit(brick::SubmitResult r) {
    switch (r) {
        case brick::SubmitResult::Accepted: return CLAY_BRICK_SUBMIT_ACCEPTED;
        case brick::SubmitResult::Stale: return CLAY_BRICK_SUBMIT_STALE;
        case brick::SubmitResult::BudgetExceeded: return CLAY_BRICK_SUBMIT_BUDGET_EXCEEDED;
    }
    return CLAY_BRICK_SUBMIT_STALE;
}

bool normal_mode_is_known(std::int32_t v) {
    if (v < 0 || v > 0xff) return false;
    switch (static_cast<mesh::NormalMode>(v)) {
        case mesh::NormalMode::None:
        case mesh::NormalMode::Face:
        case mesh::NormalMode::Gradient: return true;
    }
    return false;
}

clay_result read_brick_config(const clay_brick_config* src, brick::BrickConfig* out) {
    if (!src) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick config");
    clay_brick_config c;
    clay_result r = read_desc(src, kBrickConfigOriginal, &c);
    if (r != CLAY_OK) return r;
    if (c.dim != 8 && c.dim != 16)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "brick dim must be 8 or 16, got " + std::to_string(c.dim));
    if (!(c.voxel_size > 0.0f) || !std::isfinite(c.voxel_size))  // also rejects NaN
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "voxel_size must be finite and > 0");
    if (c.band_voxels <= 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "band_voxels must be > 0");
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (c.memory_budget > static_cast<std::uint64_t>(static_cast<std::size_t>(-1)))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "memory_budget does not fit this platform's size_t");
    }
    out->dim = c.dim;
    out->voxel_size = c.voxel_size;
    out->band_voxels = c.band_voxels;
    out->memory_budget = static_cast<std::size_t>(c.memory_budget);
    out->colors = c.colors != 0;
    // The band and the brick size are derived, and both are used as divisors
    // and dilations below; an overflow here would make every span check
    // meaningless.
    if (!std::isfinite(out->band()) ||
        !std::isfinite(static_cast<float>(out->dim) * out->voxel_size))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "dim and band_voxels overflow the voxel size");
    return CLAY_OK;
}

std::size_t brick_samples(const brick::BrickConfig& c) {
    return static_cast<std::size_t>(c.dim) * static_cast<std::size_t>(c.dim) *
           static_cast<std::size_t>(c.dim);
}

brick::BrickKey to_brick_key(const std::int32_t key[3]) { return {key[0], key[1], key[2]}; }

// BrickCache::mark_dirty converts the region to brick coordinates with a bare
// (int) cast and then inserts a tracked entry per brick in the span. Three
// caller floats name that span: a region of 1e6 world units at a brick size of
// 0.4 is 1e19 bricks, which is undefined behaviour on the cast and an
// unbounded allocation on the insert — and under -fno-exceptions the
// std::bad_alloc would reach std::terminate and take the host down rather than
// return an error. So the span is computed in 64-bit and checked BEFORE the
// engine sees the region, and the arithmetic mirrors the engine's float
// division exactly so the two cannot disagree about the edge brick.
// How many bricks one cache may TRACK. A tracked brick is a permanent map
// entry, roughly a hundred bytes, so this is a memory ceiling wearing a count:
// 2^21 is about 200 MB of bookkeeping, which is a lot to have already spent on
// a mobile device and far past anything a real sculpt marks.
constexpr std::int64_t kMaxTrackedBricks = 1 << 21;

clay_result check_dirty_span(const brick::BrickCache& cache, const math::Aabb& world_bound) {
    if (world_bound.empty() || world_bound.is_infinite()) return CLAY_OK;  // no span to walk
    const brick::BrickConfig& config = cache.config();
    const math::Aabb region = world_bound.dilated(config.band());
    const float bs = static_cast<float>(config.dim) * config.voxel_size;
    const float lo[3] = {region.min.x, region.min.y, region.min.z};
    const float hi[3] = {region.max.x, region.max.y, region.max.z};
    // The widest float interval the engine's (int) cast is defined over.
    constexpr float kIntMin = -2147483648.0f;
    constexpr float kIntMax = 2147483520.0f;  // the largest float <= INT32_MAX
    std::int64_t span = 1;
    for (int a = 0; a < 3; ++a) {
        const float first = std::floor(lo[a] / bs);
        const float last = std::floor(hi[a] / bs);
        if (!(first >= kIntMin && first <= kIntMax) || !(last >= kIntMin && last <= kIntMax))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "the region reaches a brick coordinate outside int32");
        span *= static_cast<std::int64_t>(last) - static_cast<std::int64_t>(first) + 1;
        if (span > kMaxTrackedBricks) break;  // before the next multiply can overflow
    }
    // Counted against a ceiling of its own, not against CLAY_MAX_BATCH. That
    // limit counts values in a TRANSIENT batch, freed when the call returns;
    // each brick here is a PERMANENT map entry of about a hundred bytes, so
    // borrowing the number authorised well over a gigabyte of bookkeeping from
    // six floats — and memory_budget covers stored payloads, not this.
    //
    // The cache's current size counts too: the span guard is per call, and a
    // session marks many regions.
    const std::int64_t already = static_cast<std::int64_t>(cache.tracked_count());
    if (span > kMaxTrackedBricks || already + span > kMaxTrackedBricks)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the region spans " + std::to_string(span) + " bricks and the cache holds " +
                        std::to_string(already) + ", above the tracked-brick limit of " +
                        std::to_string(kMaxTrackedBricks));
    return CLAY_OK;
}

// The fixed stride ONE brick occupies in a read_bricks output: its own lattice
// plus the halo on both sides of every axis. apron is validated against dim
// before this runs, so (dim + 2*apron) is at most 3*dim and the cube cannot
// wrap a size_t for any dim this cache accepts.
std::size_t padded_samples(const brick::BrickConfig& c, std::int32_t apron) {
    const std::size_t w = static_cast<std::size_t>(c.dim) + 2 * static_cast<std::size_t>(apron);
    return w * w * w;
}

// The colour payload crosses as bytes and is written through a BrickColor*, so
// the two layouts have to be the same four bytes in the same order.
static_assert(sizeof(brick::BrickColor) == 4, "BrickColor must be four packed bytes");
static_assert(offsetof(brick::BrickColor, r) == 0, "BrickColor.r must be first");
static_assert(offsetof(brick::BrickColor, g) == 1, "BrickColor.g must follow r");
static_assert(offsetof(brick::BrickColor, b) == 2, "BrickColor.b must follow g");
static_assert(offsetof(brick::BrickColor, a) == 3, "BrickColor.a must follow b");

// A batch shares one stride, so it shares one lattice size. Requests from one
// cache always do; a batch that does not is a caller mixing two caches, and
// there is no offset that would be right for both.
clay_result check_uniform_dims(const clay_brick_request* requests, std::size_t count) {
    for (std::size_t i = 1; i < count; ++i)
        if (requests[i].dims[0] != requests[0].dims[0] ||
            requests[i].dims[1] != requests[0].dims[1] ||
            requests[i].dims[2] != requests[0].dims[2])
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "every request in a batch must have the same dims");
    return CLAY_OK;
}

// The grid fields are CHECKED against the configuration rather than re-derived
// from the key: a mismatch means the request was modified or came from a
// different cache, which is a malformed call and not a stale one.
clay_result check_requests_match(const clay_brick_request* requests, std::size_t count,
                                 const brick::BrickConfig& config) {
    for (std::size_t i = 0; i < count; ++i)
        if (requests[i].spacing != config.voxel_size || requests[i].dims[0] != config.dim ||
            requests[i].dims[1] != config.dim || requests[i].dims[2] != config.dim)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "request " + std::to_string(i) +
                            " does not match this cache's configuration");
    return CLAY_OK;
}

// The world box a request's lattice covers — the brick itself, NOT the region
// its tape is culled against. Culling dilates this by the band first: a sample
// stores its true distance whenever that distance is within the band, so an
// item a band outside this box still decides samples inside it.
math::Aabb request_brick_box(const clay_brick_request& req) {
    const kernel::cfloat3 lo = kernel::cf3(req.origin[0], req.origin[1], req.origin[2]);
    const kernel::cfloat3 size = kernel::cf3(req.spacing * static_cast<float>(req.dims[0]),
                                             req.spacing * static_cast<float>(req.dims[1]),
                                             req.spacing * static_cast<float>(req.dims[2]));
    return math::Aabb{lo, lo + size};
}

// The batch pipeline shared by the host- and device-destination request
// evaluators, so the two cannot drift (issue #64 applied to both): validate
// every request BEFORE any is evaluated — a malformed one refuses the call
// with nothing written rather than after its neighbours have already landed —
// then build one per-revision cull index and one coarse plan over the union
// of every request's cull region, so each per-brick compile walks only the
// items near the batch instead of the whole document. The plan's region
// contains every brick's, which is what makes the per-brick tapes
// byte-identical to compiling without it.
//
// The per-brick culled tapes then reach `run` as GridBatchQuery CHUNKS of up
// to 4096, so the compiled tapes held at once stay bounded — a tape carrying
// a sampled volume is megabytes. Dims are checked uniform by both callers,
// but each request keeps its own spacing, so a chunk splits where the
// spacing changes and a batch that mixes caches keeps each request's own
// lattice. Each tape is culled against its own brick DILATED by its own
// band: an item a band outside the brick still decides samples inside it,
// because a sample keeps its true distance whenever that distance is within
// the band.
//
// run(bq, base) evaluates one chunk, whose requests start at `base`, into
// the caller's destination — host memory or the caller's device buffer, the
// only thing the two entry points do differently.
// Which of a document a batch compiles: the whole thing, or one side of the
// split a resumable multi-layer refill holds as two values.
enum class ChunkHalf { Whole, Below, Active };

// `post`, when given, is called once per chunk AFTER `run` has filled it, with
// the chunk's per-brick tapes and the checkpoint each passed through. That is
// where a refill takes the stack a group resume needs: the tapes are compiled
// here and nowhere else, and compiling them a second time to ask where their
// checkpoint sits would cost more than the walk it saves.
struct NoPost {
    void operator()(std::size_t, std::size_t, const std::vector<scene::Tape>&,
                    const std::vector<scene::TapeCheckpoint>&) const {}
};

template <typename Run, typename Post = NoPost>
clay_result eval_requests_in_chunks(const clay_document* doc, const clay_brick_request* requests,
                                    std::size_t count, Run&& run, ChunkHalf half = ChunkHalf::Whole,
                                    scene::LayerId active = 0, Post&& post = Post{}) {
    std::vector<kernel::cfloat3> origins(count);
    for (std::size_t i = 0; i < count; ++i) {
        eval::GridQuery q;
        std::size_t samples = 0;
        clay_result r =
            read_grid(requests[i].origin, requests[i].spacing, requests[i].dims, &q, &samples);
        if (r != CLAY_OK) return r;
        const float band = requests[i].band;
        if (!(band >= 0.0f) || !std::isfinite(band))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "a request carries a band that is not finite and >= 0");
        origins[i] = q.origin;
    }
    std::shared_ptr<const scene::CullIndex> index = doc->cull_index();
    math::Aabb batch_region;
    for (std::size_t i = 0; i < count; ++i)
        batch_region.expand(request_brick_box(requests[i]).dilated(requests[i].band));
    const scene::CullPlan plan = index->plan(batch_region);
    constexpr std::size_t kChunk = 4096;
    constexpr bool kWantCheckpoints = !std::is_same_v<std::decay_t<Post>, NoPost>;
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    std::vector<scene::TapeCheckpoint> cps;
    for (std::size_t base = 0; base < count;) {
        std::size_t n = 1;
        while (n < kChunk && base + n < count &&
               requests[base + n].spacing == requests[base].spacing)
            ++n;
        tapes.clear();
        tape_ptrs.clear();
        cps.clear();
        tapes.reserve(n);
        tape_ptrs.reserve(n);
        if (kWantCheckpoints) cps.resize(n);
        for (std::size_t i = base; i < base + n; ++i) {
            scene::CullRegion cull{request_brick_box(requests[i]).dilated(requests[i].band)};
            scene::TapeCheckpoint* cp = kWantCheckpoints ? &cps[i - base] : nullptr;
            if (half == ChunkHalf::Whole)
                tapes.push_back(cp ? scene::compile_document_resumable(doc->doc.document, cp, &cull,
                                                                       index.get(), &plan)
                                   : scene::compile_document(doc->doc.document, &cull, index.get(),
                                                             &plan));
            else if (cp && half == ChunkHalf::Active)
                tapes.push_back(scene::compile_document_part_resumable(
                    doc->doc.document, active, /*below=*/false, &cull, index.get(), cp));
            else
                tapes.push_back(scene::compile_document_part(doc->doc.document, active,
                                                             half == ChunkHalf::Below, &cull,
                                                             index.get()));
            tape_ptrs.push_back(&tapes.back());
        }
        eval::GridBatchQuery bq;
        bq.tapes = tape_ptrs.data();
        bq.origins = origins.data() + base;
        bq.spacing = requests[base].spacing;
        bq.nx = requests[0].dims[0];
        bq.ny = requests[0].dims[1];
        bq.nz = requests[0].dims[2];
        bq.count = n;
        clay_result r = run(bq, base);
        if (r != CLAY_OK) return r;
        post(base, n, tapes, cps);
        base += n;
    }
    return CLAY_OK;
}

// Bracket a mask edit so it becomes ONE undo step (masks-in-the-history).
//
// This used to record a BARRIER, because voxel::MaskField was the fourth
// representation with no history mechanism at all. It has one now, so a mask
// edit is an ordinary step and the two features that documented around the
// barrier — undo, and journal replay — no longer have to.
//
// The mechanism differs from VoxelStep and the difference is not cosmetic:
// VoxelGrid::set is the one choke point every voxel verb funnels through, so a
// sink there sees everything. A mask's invert, clear, expand, contract and
// smooth write chunk data directly, so the mask snapshots on its first touch()
// and diffs when the step closes.
//
// A standalone mask is not in a document and records nothing, exactly as a
// standalone voxel grid does not.
struct MaskStep {
    clay_document* doc = nullptr;
    voxel::MaskField* mask = nullptr;

    MaskStep(const clay_mask* handle, voxel::MaskField* m) {
        if (!handle || !handle->doc || !handle->doc->undo || !m) return;
        if (!handle->doc->undo->begin_mask_step(handle->layer, *m)) return;
        doc = handle->doc;
        mask = m;
    }
    ~MaskStep() {
        if (doc) doc->undo->end_mask_step(*mask);
    }
    MaskStep(const MaskStep&) = delete;
    MaskStep& operator=(const MaskStep&) = delete;
};

VoxelStep::VoxelStep(const clay_voxel_grid* handle, voxel::VoxelGrid* g) {
    if (!handle || !handle->doc || !handle->doc->undo || !g) return;
    if (!handle->doc->undo->begin_voxel_step(handle->layer, *g)) return;
    doc = handle->doc;
    grid = g;
}

VoxelStep::~VoxelStep() {
    if (doc) doc->undo->end_voxel_step(*grid);
}

// One normaliser for every format name that crosses, so the reader and the
// writer cannot disagree about what "OBJ" means. They did: the loader
// lowercased and the writer did not, so a host could load MODEL.OBJ and then
// be refused when it saved back to the path it had just read.
std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string extension_of(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string{} : lower_ascii(path.substr(dot + 1));
}

// A caller's budget, or the library's defaults. Zero means "the default"
// rather than "allow nothing", which is what a zeroed struct would say.
clay_result import_limits(const clay_import_budget* budget, io::ImportBudget* out) {
    if (!budget) return CLAY_OK;
    clay_import_budget b;
    clay_result r = read_desc(budget, kImportBudgetOriginal, &b);
    if (r != CLAY_OK) return r;
    if (b.max_vertices) out->max_vertices = static_cast<std::size_t>(b.max_vertices);
    if (b.max_triangles) out->max_triangles = static_cast<std::size_t>(b.max_triangles);
    return CLAY_OK;
}

// Calls `run(first, n)` once per maximal run of consecutive bricks whose mark
// is `want`. Both refill destinations address a brick by a FIXED slot, so a run
// of bricks is a run of bytes and a run is the unit that can be handed to a
// batched evaluation or a single buffer write.
template <typename Run>
clay_result for_each_run(const std::vector<std::uint8_t>& mark, bool want, Run&& run) {
    const std::size_t count = mark.size();
    for (std::size_t i = 0; i < count;) {
        if ((mark[i] != 0) != want) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < count && (mark[end] != 0) == want) ++end;
        const clay_result r = run(i, end - i);
        if (r != CLAY_OK) return r;
        i = end;
    }
    return CLAY_OK;
}

// The slice of a caller's allocation holding `n` bricks from brick `at`, at the
// fixed per-brick stride the device refill documents. FILE SCOPE, above the
// first extern "C" — a helper returning a C++ type from inside that block is
// what broke the macOS builds in #235, and GCC does not warn about it, so a
// green local build proves nothing.
eval::DeviceBuffer brick_slot(const eval::DeviceBuffer& whole, std::size_t at, std::size_t n,
                              std::size_t stride) {
    eval::DeviceBuffer s = whole;
    s.offset = whole.offset + static_cast<std::uint64_t>(at) * stride * sizeof(float);
    s.size = static_cast<std::uint64_t>(n) * stride * sizeof(float);
    return s;
}

}  // namespace

extern "C" {

void clay_version(int32_t* major, int32_t* minor, int32_t* patch) {
    if (major) *major = CLAY_ABI_MAJOR;
    if (minor) *minor = CLAY_ABI_MINOR;
    if (patch) *patch = CLAY_ABI_PATCH;
}

const char* clay_last_error(void) { return g_last_error.c_str(); }

clay_document* clay_document_create(void) { return new clay_document(); }

void clay_document_destroy(clay_document* doc) { delete doc; }

clay_cancel_token* clay_cancel_token_create(void) { return new clay_cancel_token(); }

void clay_cancel_token_destroy(clay_cancel_token* token) { delete token; }

void clay_cancel_token_cancel(clay_cancel_token* token) {
    if (token) token->token.cancel();
}

int32_t clay_cancel_token_cancelled(const clay_cancel_token* token) {
    return token && token->token.cancelled() ? 1 : 0;
}

void clay_cancel_token_reset(clay_cancel_token* token) {
    if (token) token->token.reset();
}

clay_result clay_cancel_token_progress(const clay_cancel_token* token,
                                       clay_progress* out_progress) {
    if (!token || !out_progress) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_progress probe;
    clay_result r = read_desc(out_progress, kProgressOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_progress->struct_size;
    const parallel::Progress p = token->token.progress();
    clay_progress filled{};
    filled.phase = p.phase;
    filled.phase_count = p.phase_count;
    filled.running = p.running ? 1 : 0;
    filled.fraction = p.fraction;
    filled.done = p.done;
    filled.total = p.total;
    write_desc(out_progress, declared, filled);
    return CLAY_OK;
}

const uint8_t* clay_blob_data(const clay_blob* blob) {
    if (!blob || blob->bytes.empty()) return nullptr;
    return blob->bytes.data();
}

size_t clay_blob_size(const clay_blob* blob) { return blob ? blob->bytes.size() : 0; }

void clay_blob_destroy(clay_blob* blob) { delete blob; }

clay_result clay_document_save(const clay_document* doc, const char* path) {
    if (!doc || !path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or path");
    return from_io(io::save_clayspace_file(doc->doc, path));
}

clay_result clay_document_save_memory(const clay_document* doc, clay_blob** out_blob) {
    if (!doc || !out_blob) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_blob");
    *out_blob = nullptr;
    *out_blob = new clay_blob{io::save_clayspace(doc->doc)};
    return CLAY_OK;
}

clay_result clay_document_history_bytes(const clay_document* doc,
                                        clay_history_bytes* out_bytes) {
    if (!doc || !out_bytes) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_history_bytes probe;
    clay_result r = read_desc(out_bytes, kHistoryBytesOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_bytes->struct_size;
    clay_history_bytes filled{};
    if (doc->undo) {
        const session::History::Bytes b = doc->undo->bytes();
        filled.undo = b.undo;
        filled.redo = b.redo;
        filled.journal = b.journal;
        filled.total = b.total;
        filled.undo_steps = b.undo_steps;
        filled.redo_steps = b.redo_steps;
        filled.journal_events = b.journal_events;
        filled.dropped_steps = b.dropped_steps;
    }
    // Undo off is not an error: it costs nothing, which is the honest answer.
    write_desc(out_bytes, declared, filled);
    return CLAY_OK;
}

clay_result clay_document_memory(const clay_document* doc, clay_memory_report* out_report) {
    if (!doc || !out_report) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_memory_report probe;
    clay_result r = read_desc(out_report, kMemoryReportOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_report->struct_size;
    write_desc(out_report, declared, to_c_report(io::document_memory(doc->doc, doc->undo.get())));
    return CLAY_OK;
}

clay_result clay_layer_memory(const clay_document* doc, clay_layer_id layer,
                              clay_memory_report* out_report) {
    if (!doc || !out_report) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_memory_report probe;
    clay_result r = read_desc(out_report, kMemoryReportOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_report->struct_size;
    io::MemoryReport rep;
    if (!io::layer_memory(doc->doc, layer, &rep))
        return fail(CLAY_ERROR_NOT_FOUND, "no such layer");
    write_desc(out_report, declared, to_c_report(rep));
    return CLAY_OK;
}

clay_result clay_document_set_history_budget(clay_document* doc, uint64_t bytes) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    doc->undo->set_budget(static_cast<std::size_t>(bytes));
    return CLAY_OK;
}

clay_result clay_document_trim_history(clay_document* doc, uint64_t bytes) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    doc->undo->trim_to(static_cast<std::size_t>(bytes));
    return CLAY_OK;
}

clay_result clay_document_journal_since(const clay_document* doc, size_t from,
                                        clay_blob** out_blob, size_t* out_now_at) {
    if (!doc || !out_blob) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_blob");
    *out_blob = nullptr;
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    std::size_t now_at = 0;
    std::vector<std::uint8_t> bytes = doc->undo->journal_since(from, &now_at);
    if (out_now_at) *out_now_at = now_at;
    *out_blob = new clay_blob{std::move(bytes)};
    return CLAY_OK;
}

clay_result clay_document_journal_range(const clay_document* doc, size_t* out_first,
                                        size_t* out_next) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    if (out_first) *out_first = doc->undo->journal_first();
    if (out_next) *out_next = doc->undo->journal_next();
    return CLAY_OK;
}

clay_result clay_document_journal_trim(clay_document* doc, size_t upto) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    doc->undo->trim_journal(upto);
    return CLAY_OK;
}

clay_result clay_document_replay_journal(clay_document* doc, const uint8_t* data, size_t size,
                                         size_t* out_applied,
                                         int32_t* out_stopped_at_barrier) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!data || size == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or empty journal");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    if (out_applied) *out_applied = 0;
    if (out_stopped_at_barrier) *out_stopped_at_barrier = 0;

    session::History::ReplayResult result;
    const bool ok = doc->undo->replay(data, size, doc->doc.document, doc->grid_for(),
                                      doc->mesh_for(), &result, doc->mask_for());
    if (out_applied) *out_applied = result.applied;
    if (out_stopped_at_barrier) *out_stopped_at_barrier = result.stopped_at_barrier ? 1 : 0;
    // Replay writes straight onto the document rather than through apply_edit,
    // so it invalidates here — and it does so even on a refusal, because the
    // events before the bad one were applied.
    doc->touch();
    if (!ok)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the journal could not be replayed: a version this build does not "
                    "understand, a truncated buffer, or a step naming a layer that is gone");
    return CLAY_OK;
}

clay_result clay_document_load_memory(const uint8_t* data, size_t size,
                                      clay_document** out_doc) {
    if (!out_doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out pointer");
    *out_doc = nullptr;
    if (!data || size == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or empty buffer");
    auto doc = std::make_unique<clay_document>();
    io::IoStatus s = io::load_clayspace(data, size, &doc->doc);
    if (!s.ok()) return from_io(s);
    *out_doc = doc.release();
    return CLAY_OK;
}

clay_result clay_document_load(const char* path, clay_document** out_doc) {
    if (!path || !out_doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path or out pointer");
    auto* doc = new clay_document();
    io::IoStatus s = io::load_clayspace_file(path, &doc->doc);
    if (!s.ok()) {
        delete doc;
        *out_doc = nullptr;
        return from_io(s);
    }
    *out_doc = doc;
    return CLAY_OK;
}

clay_result clay_add_sdf_layer(clay_document* doc, const char* name,
                               clay_layer_id* out_layer) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    // Through the command vocabulary (AddLayerCmd with a reserved id) so an
    // enabled undo stack records the add; see insert_node.
    scene::Layer layer;
    layer.id = doc->doc.document.reserve_layer_id();
    layer.name = name;
    layer.sdf = std::make_shared<scene::SdfContent>();
    clay_layer_id id = layer.id;
    clay_result r = apply_edit(doc, scene::Command{scene::AddLayerCmd{std::move(layer), -1}},
                               "layer could not be added");
    if (r != CLAY_OK) return r;
    if (out_layer) *out_layer = id;
    return CLAY_OK;
}

namespace {

// The flat-descriptor path, shared by the root-level add and the in-group one:
// a descriptor means the same thing wherever the edit lands.
clay_result add_item_desc(clay_document* doc, clay_layer_id layer_id, const clay_item_desc* item,
                          clay_node_id* out_node, scene::NodeId parent, int index) {
    if (!doc || !item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or item");
    clay_item_desc d;
    clay_result r = read_desc(item, kItemDescOriginal, &d);
    if (r != CLAY_OK) return r;
    r = validate_item_desc(d);
    if (r != CLAY_OK) return r;
    r = canonical_prim_params(d.prim, d.params);
    if (r != CLAY_OK) return r;
    return insert_node(doc, layer_id, item_from_desc(d).node, out_node, parent, index);
}

}  // namespace

clay_result clay_add_item(clay_document* doc, clay_layer_id layer_id,
                          const clay_item_desc* item, clay_node_id* out_node) {
    return add_item_desc(doc, layer_id, item, out_node, scene::kNoNode, -1);
}

clay_result clay_remove_node(clay_document* doc, clay_layer_id layer_id, clay_node_id node) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    // Through the command vocabulary so the removal is undoable; the inverse
    // AddNodeCmd carries the removed subtree, ids preserved.
    return apply_edit(doc, scene::Command{scene::RemoveNodeCmd{layer_id, node}},
                      "node not found");
}


clay_result clay_document_enable_undo(clay_document* doc) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) {
        doc->undo = std::make_unique<session::History>();
        doc->undo->set_enabled(true);
        // Set once rather than passed to undo/redo/replay like the three
        // per-layer resolvers, because the group lattice is per DOCUMENT and
        // there is no map lookup that can miss. Still a callable, so it is
        // consulted at the moment of use and cannot outlive what it names — the
        // lattice is created lazily and this must see it whenever that happens.
        doc->undo->set_groups_resolver([doc]() -> voxel::GroupField* {
            return doc->doc.groups ? &*doc->doc.groups : nullptr;
        });
    }
    return CLAY_OK;
}

clay_result clay_document_undo(clay_document* doc, int32_t* out_undone) {
    return clay_document_undo_bound(doc, out_undone, nullptr, nullptr, nullptr, nullptr);
}

clay_result clay_document_redo(clay_document* doc, int32_t* out_redone) {
    return clay_document_redo_bound(doc, out_redone, nullptr, nullptr, nullptr, nullptr);
}

clay_result clay_document_undo_bound(clay_document* doc, int32_t* out_undone, float out_min[3],
                                     float out_max[3], int32_t* out_has_bounds,
                                     int32_t* out_infinite) {
    if (!doc || !out_undone) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    // An empty stack is reported, not failed: a UI drives this without having
    // to track whether anything is left. It leaves the bound empty, which
    // write_influence spells as no bounds — nothing to dirty.
    math::Aabb bound;
    *out_undone = doc->undo->undo(doc->doc.document, doc->grid_for(), doc->mesh_for(),
                                  &bound, doc->mask_for())
                      ? 1
                      : 0;
    // Undo and redo replay commands straight onto the document rather than
    // through apply_edit, so they invalidate here -- with the bound the stack
    // already unioned over the commands it replayed. STRUCTURAL, because a
    // replayed inverse can be one (removing what an edit added), and
    // over-invalidating a prefix seed costs one full walk -- the rule at
    // touch(), restated.
    if (*out_undone) doc->touch_region_structural(bound);
    return write_influence(bound, out_min, out_max, out_has_bounds, out_infinite);
}

clay_result clay_document_redo_bound(clay_document* doc, int32_t* out_redone, float out_min[3],
                                     float out_max[3], int32_t* out_has_bounds,
                                     int32_t* out_infinite) {
    if (!doc || !out_redone) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    math::Aabb bound;
    *out_redone = doc->undo->redo(doc->doc.document, doc->grid_for(), doc->mesh_for(),
                                  &bound, doc->mask_for())
                      ? 1
                      : 0;
    if (*out_redone) doc->touch_region_structural(bound);
    return write_influence(bound, out_min, out_max, out_has_bounds, out_infinite);
}

clay_result clay_document_undo_state(const clay_document* doc, int32_t* out_enabled,
                                     size_t* out_undo_depth, size_t* out_redo_depth) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (out_enabled) *out_enabled = doc->undo ? 1 : 0;
    if (out_undo_depth) *out_undo_depth = doc->undo ? doc->undo->undo_depth() : 0;
    if (out_redo_depth) *out_redo_depth = doc->undo ? doc->undo->redo_depth() : 0;
    return CLAY_OK;
}

clay_result clay_document_begin_undo_group(clay_document* doc) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    doc->undo->begin_group();
    return CLAY_OK;
}

clay_result clay_document_end_undo_group(clay_document* doc) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    doc->undo->end_group();
    return CLAY_OK;
}

clay_result clay_layer_set_transform(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float position[3], const float rotation_axis[3],
                                     float rotation_angle, float scale) {
    // A group's transform never reaches its children — the compiler composes
    // layer * item and nothing else — so this would be an undoable, saved edit
    // that changes nothing at all. Refused rather than recorded.
    const scene::Node* target = peek_node(doc, layer, node);
    if (target && target->is_group)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a group has no transform of its own: transform its children");
    math::Transform xform;
    clay_result r = read_transform(position, rotation_axis, rotation_angle, scale, &xform);
    if (r != CLAY_OK) return r;
    // The command carries the WHOLE transform, so a uniform edit collapses any
    // per-axis scale the node had. Stated at the declaration: this call means
    // "this node's scale is uniform s".
    return apply_edit(doc, scene::Command{scene::SetTransformCmd{layer, node, xform}},
                      "node not found");
}

clay_result clay_layer_set_transform_nonuniform(clay_document* doc, clay_layer_id layer,
                                                clay_node_id node, const float position[3],
                                                const float rotation_axis[3], float rotation_angle,
                                                const float scale[3]) {
    const scene::Node* target = peek_node(doc, layer, node);
    if (target && target->is_group)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a group has no transform of its own: transform its children");
    kernel::cfloat3 axes;
    clay_result r = read_scale_axes(scale, &axes);
    if (r != CLAY_OK) return r;
    // The uniform factor stays 1 here and the three components carry the whole
    // scale, so what this call writes is exactly what its reader returns.
    math::Transform xform;
    r = read_transform(position, rotation_axis, rotation_angle, 1.0f, &xform);
    if (r != CLAY_OK) return r;
    return apply_edit(doc, scene::Command{scene::SetTransformCmd{layer, node, xform, axes}},
                      "node not found");
}

clay_result clay_layer_set_prim(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                int32_t prim, const float* params, size_t param_count) {
    if (!prim_is_known(prim)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown primitive type");
    // The same refusal validate_item_desc makes: this entry point replaces only
    // Node::prim, so it has no way to supply a stroke, profiles or a volume.
    // Letting it through turned a node into a loft with zero profiles, which
    // the tape then read as a record that was never written.
    scene::PrimType replaced = static_cast<scene::PrimType>(prim);
    if (replaced == scene::PrimType::Stroke || scene::prim_is_lift(replaced) ||
        scene::prim_carries_profiles(replaced) || scene::prim_is_volume(replaced))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "primitive needs out-of-line data");
    clay_result r = check_params("primitive", params, param_count, kPrimParams[prim]);
    if (r != CLAY_OK) return r;
    float p[scene::kMaxPrimParams] = {};
    for (size_t i = 0; i < param_count; ++i) p[i] = params[i];
    r = canonical_prim_params(prim, p);
    if (r != CLAY_OK) return r;

    scene::Prim replacement;
    replacement.type = replaced;
    std::memcpy(replacement.params, p, sizeof p);
    return apply_edit(doc, scene::Command{scene::SetPrimCmd{layer, node, replacement}},
                      "node not found");
}

clay_result clay_layer_set_color(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                 const float rgb[3]) {
    if (!rgb) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null colour");
    return apply_edit(
        doc,
        scene::Command{scene::SetColorCmd{layer, node, kernel::cf3(rgb[0], rgb[1], rgb[2])}},
        "node not found");
}

clay_result clay_layer_set_op_blend(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t op, int32_t blend, float blend_k, float rounding) {
    // A group takes the inline op and refuses the transitions; an item is the
    // other way round. Which rules apply is a property of the node, so the
    // node decides — a miss falls through to apply_edit's NOT_FOUND as before.
    const scene::Node* target = peek_node(doc, layer, node);
    clay_result r = (target && target->is_group)
                        ? validate_group_op_blend(op, blend, blend_k, rounding)
                        : validate_item_op_blend(op, blend, blend_k, rounding);
    if (r != CLAY_OK) return r;

    scene::Blend b;
    b.profile = static_cast<scene::BlendProfile>(blend);
    b.k = blend_k;
    return apply_edit(doc,
                      scene::Command{scene::SetOpBlendCmd{layer, node,
                                                          static_cast<scene::Op>(op), b,
                                                          rounding}},
                      "node not found");
}

clay_result clay_layer_move(clay_document* doc, clay_layer_id layer, clay_node_id node,
                            clay_node_id new_parent, int32_t index) {
    // SdfContent::move refuses this too — it is the engine's invariant, not the
    // binding's — but apply_edit could only report it as a missing id, which is
    // not what went wrong.
    const scene::Layer* l = doc ? doc->doc.document.find_layer(layer) : nullptr;
    if (l && l->sdf && new_parent != scene::kNoNode && l->sdf->contains(node, new_parent))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a node cannot move into its own subtree");
    return apply_edit(
        doc, scene::Command{scene::MoveNodeCmd{layer, node, new_parent, index}},
        "node or new parent not found");
}

clay_result clay_layer_append_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float* points_xyzr, size_t count) {
    if (!points_xyzr || count == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "append_stroke needs at least one point");
    clay_result r = check_batch("stroke points", count);
    if (r != CLAY_OK) return r;

    scene::AppendStrokeCmd cmd{layer, node, {}};
    cmd.points.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const float* q = points_xyzr + i * 4;
        if (!(q[3] > 0.0f))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke radii must be > 0");
        cmd.points.push_back(scene::StrokePoint{kernel::cf3(q[0], q[1], q[2]), q[3]});
    }
    return apply_edit(doc, scene::Command{std::move(cmd)}, "node not found");
}

clay_result clay_layer_trim_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   uint32_t count) {
    return apply_edit(doc, scene::Command{scene::TrimStrokeCmd{layer, node, count}},
                      "node not found");
}

clay_result clay_layer_add_group(clay_document* doc, clay_layer_id layer, clay_node_id parent,
                                 int32_t index, int32_t op, int32_t blend, float blend_k,
                                 float rounding, clay_node_id* out_node) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    clay_result r = validate_group_op_blend(op, blend, blend_k, rounding);
    if (r != CLAY_OK) return r;
    scene::Node group;
    group.is_group = true;
    group.op = static_cast<scene::Op>(op);
    group.blend.profile = static_cast<scene::BlendProfile>(blend);
    group.blend.k = blend_k;
    group.rounding = rounding;
    // The same AddNodeCmd an item goes through, so the group is one undo step
    // and its inverse is the RemoveNodeCmd that carries the whole subtree back.
    return insert_node(doc, layer, std::move(group), out_node, parent, index);
}

clay_result clay_add_item_in_group(clay_document* doc, clay_layer_id layer, clay_node_id group,
                                   int32_t index, const clay_item_desc* item,
                                   clay_node_id* out_node) {
    if (group == scene::kNoNode) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null group");
    return add_item_desc(doc, layer, item, out_node, group, index);
}

clay_result clay_layer_add_item_in_group(clay_document* doc, clay_layer_id layer,
                                         clay_node_id group, int32_t index,
                                         const clay_item* item, clay_node_id* out_node) {
    if (!doc || !item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or item");
    if (group == scene::kNoNode) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null group");
    clay_result r = validate_item(*item);
    if (r != CLAY_OK) return r;
    return insert_node(doc, layer, item->node, out_node, group, index);
}

clay_result clay_layer_children(const clay_document* doc, clay_layer_id layer, clay_node_id node,
                                clay_node_id* out_children, size_t* count) {
    if (!doc || !count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or count");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const scene::Node* n = l->sdf ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in that layer");
    // Not a group is a caller asking the wrong question, not a missing node —
    // and the answer a reloading host reads to tell the two apart.
    if (!n->is_group) return fail(CLAY_ERROR_INVALID_ARGUMENT, "node is not a group");

    const std::size_t needed = n->children.size();
    if (out_children && *count < needed) {
        *count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the group has " + std::to_string(needed) + " children");
    }
    if (out_children)
        for (std::size_t i = 0; i < needed; ++i) out_children[i] = n->children[i];
    *count = needed;
    return CLAY_OK;
}

clay_result clay_layer_node_prim(const clay_document* doc, clay_layer_id layer,
                                 clay_node_id node, int32_t* out_prim) {
    if (!doc || !out_prim)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_prim");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const scene::Node* n = l->sdf ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in that layer");
    // A group carries no primitive: the dual of clay_layer_children refusing
    // an item, so every node answers exactly one of the two questions.
    if (n->is_group) return fail(CLAY_ERROR_INVALID_ARGUMENT, "node is a group");
    // scene::PrimType's values ARE the clay_prim values; the static_asserts at
    // the top of this file pin the correspondence.
    *out_prim = static_cast<std::int32_t>(n->prim.type);
    return CLAY_OK;
}

namespace {

// Every reader below asks the same two questions before it answers anything,
// and each has its own typed refusal: an id that is not a layer's, then an id
// that layer does not hold. Protection is deliberately NOT consulted — reading
// is not editing, so a ghosted, locked or hidden layer answers normally.
clay_result find_node(const clay_document* doc, clay_layer_id layer, clay_node_id node,
                      const scene::Node** out) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const scene::Node* n = l->sdf ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in that layer");
    *out = n;
    return CLAY_OK;
}

// The quaternion a node stores, as the axis and angle its setter takes.
// Canonical in two ways, both so the pair feeds straight back into
// clay_layer_set_transform:
//   - w is made non-negative, which picks one of the two names every rotation
//     has (q and -q are the same rotation) and puts the angle in [0, pi], so
//     a turn of 4 radians about +Z comes back as 2*pi - 4 about -Z;
//   - a rotation with no axis to speak of reads back as angle 0 about +Y
//     rather than about the zero vector, which read_transform refuses. A
//     reader whose output its own setter rejects would not be a round trip.
// atan2 rather than acos: near identity acos(w) loses most of its precision to
// the flat top of the cosine, and this is exactly where an unrotated item sits.
void axis_angle_of(const math::Transform& xform, float out_axis[3], float* out_angle) {
    math::Quat q = xform.rotation;
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    // A stored rotation is unit length; a zero one could only come from a
    // corrupt file, and identity is the reading that keeps this call total
    // rather than handing a NaN to the caller's manipulator.
    q = (n > 0.0f) ? math::Quat{q.x / n, q.y / n, q.z / n, q.w / n} : math::Quat::identity();
    if (q.w < 0.0f) q = math::Quat{-q.x, -q.y, -q.z, -q.w};

    const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    const bool has_axis = s > 1e-7f;
    if (out_angle) *out_angle = has_axis ? 2.0f * std::atan2(s, q.w) : 0.0f;
    if (!out_axis) return;
    const kernel::cfloat3 axis =
        has_axis ? kernel::cf3(q.x / s, q.y / s, q.z / s) : kernel::cf3(0.0f, 1.0f, 0.0f);
    out_axis[0] = axis.x;
    out_axis[1] = axis.y;
    out_axis[2] = axis.z;
}

}  // namespace

clay_result clay_layer_node_transform(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id node, float out_position[3],
                                      float out_rotation_axis[3], float* out_rotation_angle,
                                      float* out_scale) {
    const scene::Node* n = nullptr;
    clay_result r = find_node(doc, layer, node, &n);
    if (r != CLAY_OK) return r;
    // The refusal clay_layer_set_transform makes, for its reason: the compiler
    // composes layer * item and nothing else, so a group holds no transform
    // this call could answer with.
    if (n->is_group)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a group has no transform of its own: read its children");
    // One float cannot express three, and every way of pretending otherwise is
    // a lie a host would act on: the uniform factor alone describes a
    // differently-shaped item, and a read-change-write through the uniform
    // setter would round the artist's squash away. #317's own lesson — a reader
    // that cannot express what is there must not answer.
    if (!scene::scale_axes_uniform(n->scale_axes))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this node carries a per-axis scale: use "
                    "clay_layer_node_transform_nonuniform");
    if (out_position) {
        out_position[0] = n->xform.position.x;
        out_position[1] = n->xform.position.y;
        out_position[2] = n->xform.position.z;
    }
    axis_angle_of(n->xform, out_rotation_axis, out_rotation_angle);
    if (out_scale) *out_scale = n->xform.scale * n->scale_axes.x;
    return CLAY_OK;
}

clay_result clay_layer_node_transform_nonuniform(const clay_document* doc, clay_layer_id layer,
                                                 clay_node_id node, float out_position[3],
                                                 float out_rotation_axis[3],
                                                 float* out_rotation_angle, float out_scale[3]) {
    const scene::Node* n = nullptr;
    clay_result r = find_node(doc, layer, node, &n);
    if (r != CLAY_OK) return r;
    if (n->is_group)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a group has no transform of its own: read its children");
    if (out_position) {
        out_position[0] = n->xform.position.x;
        out_position[1] = n->xform.position.y;
        out_position[2] = n->xform.position.z;
    }
    axis_angle_of(n->xform, out_rotation_axis, out_rotation_angle);
    // The two scales multiply, and this call reports the product — so a node
    // placed through the UNIFORM setter answers (s, s, s) here rather than
    // (1, 1, 1) with the factor hidden somewhere the caller cannot see.
    if (out_scale) {
        out_scale[0] = n->xform.scale * n->scale_axes.x;
        out_scale[1] = n->xform.scale * n->scale_axes.y;
        out_scale[2] = n->xform.scale * n->scale_axes.z;
    }
    return CLAY_OK;
}

clay_result clay_layer_node_params(const clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   float* out_params, size_t* count) {
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    const scene::Node* n = nullptr;
    clay_result r = find_node(doc, layer, node, &n);
    if (r != CLAY_OK) return r;
    // A group carries no primitive, so it has no parameter block either: the
    // refusal clay_layer_node_prim already makes, worded the same way.
    if (n->is_group) return fail(CLAY_ERROR_INVALID_ARGUMENT, "node is a group");

    // The arity is the primitive's, which is what makes this answerable from
    // clay_layer_node_prim alone: a caller that has just learned the prim does
    // not also need kPrimParams. The out-of-line kinds count 0 here and are
    // read by the typed reader that applies.
    const std::size_t needed =
        static_cast<std::size_t>(kPrimParams[static_cast<int>(n->prim.type)]);
    if (out_params && *count < needed) {
        *count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "that primitive takes " + std::to_string(needed) + " parameters");
    }
    if (out_params)
        for (std::size_t i = 0; i < needed; ++i) out_params[i] = n->prim.params[i];
    *count = needed;
    return CLAY_OK;
}

clay_result clay_layer_node_op_blend(const clay_document* doc, clay_layer_id layer,
                                     clay_node_id node, int32_t* out_op, int32_t* out_blend,
                                     float* out_blend_k, float* out_rounding) {
    const scene::Node* n = nullptr;
    clay_result r = find_node(doc, layer, node, &n);
    if (r != CLAY_OK) return r;
    // No group refusal: a group carries an op and a blend like any other node,
    // and clay_layer_set_op_blend writes them, so this is the one of the three
    // that answers for both kinds.
    if (out_op) *out_op = static_cast<std::int32_t>(n->op);
    if (out_blend) *out_blend = static_cast<std::int32_t>(n->blend.profile);
    if (out_blend_k) *out_blend_k = n->blend.k;
    if (out_rounding) *out_rounding = n->rounding;
    return CLAY_OK;
}

clay_result clay_layer_node_count(const clay_document* doc, clay_layer_id layer,
                                  size_t* out_count) {
    if (!doc || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or count");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    // A voxel or mesh layer carries no SDF content, so it holds no nodes:
    // empty, not an error, the same reading clay_layer_eval_points makes.
    *out_count = l->sdf ? l->sdf->roots.size() : 0;
    return CLAY_OK;
}

clay_result clay_layer_node_at(const clay_document* doc, clay_layer_id layer, size_t index,
                               clay_node_id* out_node) {
    if (!doc || !out_node)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out pointer");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    // The root list IS evaluation order, and it is the list a group's children
    // are the recursion of — top level only, as clay_layer_children descends.
    const std::size_t count = l->sdf ? l->sdf->roots.size() : 0;
    if (index >= count)
        return fail(CLAY_ERROR_NOT_FOUND, "no node at index " + std::to_string(index) +
                                              ": the layer holds " + std::to_string(count) +
                                              " top-level nodes");
    *out_node = l->sdf->roots[index];
    return CLAY_OK;
}

clay_result clay_document_instance_layer(clay_document* doc, clay_layer_id source,
                                         const char* name, clay_layer_id* out_layer) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    // clay_document_set_layer_name's rule, for its reason: an empty name is
    // what a cleared text field submits.
    if (!*name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "a layer name may not be empty");
    const scene::Layer* src = doc->doc.document.find_layer(source);
    if (!src) return fail(CLAY_ERROR_NOT_FOUND, "source layer not found");
    // A voxel grid and a mesh live beside the document keyed by layer id, not
    // behind the shared pointer an instance shares, so instancing one would be
    // a second kind of sharing with its own memory contract. Refused, and the
    // message says which kind it found.
    if (src->kind != scene::LayerKind::Sdf || !src->sdf)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("only an SDF layer can be instanced: layer ") +
                        std::to_string(source) + " is a " +
                        (src->kind == scene::LayerKind::Voxel ? "voxel" : "mesh") + " layer");

    // The Layer is COPIED, so the instance starts with the source's transform,
    // visibility, protection, mirror and radial and diverges from there; the
    // CONTENT is the source's shared_ptr, so nothing proportional to the edit
    // list is paid here. Through the command vocabulary, exactly as
    // clay_add_sdf_layer and clay_document_add_voxel_layer do, so an enabled
    // undo stack records the creation as one step.
    scene::Layer layer = *src;
    layer.id = doc->doc.document.reserve_layer_id();
    layer.name = name;
    const clay_layer_id id = layer.id;
    // `content_source` is what makes the share survive the command being
    // SERIALIZED — into the journal, and through the same layer record the
    // document format writes. In memory the shared_ptr above is already the
    // whole story.
    scene::AddLayerCmd add{std::move(layer), -1, source};
    clay_result r = apply_edit(doc, scene::Command{std::move(add)},
                               "the instance layer could not be added");
    if (r != CLAY_OK) return r;
    if (out_layer) *out_layer = id;
    return CLAY_OK;
}

clay_result clay_document_remove_layer(clay_document* doc, clay_layer_id layer) {
    return apply_edit(doc, scene::Command{scene::RemoveLayerCmd{layer}}, "layer not found");
}

clay_result clay_document_move_layer(clay_document* doc, clay_layer_id layer, int32_t index) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* found = doc->doc.document.find_layer(layer);
    if (!found) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    scene::Layer copy = *found;
    // Which layer the reinsertion NAMES as its content source, taken before
    // the remove while the sharer is still findable. 0 for a layer that shares
    // with nobody, which is every layer that was never instanced. In memory
    // this changes nothing — `copy.sdf` is non-null, so apply_one ignores the
    // field — but a journal replay of a reorder without it deserializes the
    // content inline and silently unlinks the instances (see
    // scene::content_sharer_of).
    const scene::LayerId sharer = scene::content_sharer_of(doc->doc.document, layer);
    // One group, so one undo puts the layer back where it was. Ungrouped, the
    // undo stack held a remove and an insert separately and a single undo
    // applied only the remove — the layer vanished.
    if (doc->undo) doc->undo->begin_group();
    clay_result r = apply_edit(doc, scene::Command{scene::RemoveLayerCmd{layer}},
                               "layer not found");
    if (r == CLAY_OK)
        r = apply_edit(doc, scene::Command{scene::AddLayerCmd{std::move(copy), index, sharer}},
                       "layer could not be reinserted");
    if (doc->undo) doc->undo->end_group();
    return r;
}

clay_result clay_document_set_layer_visible(clay_document* doc, clay_layer_id layer,
                                            int32_t visible) {
    return apply_edit(doc, scene::Command{scene::SetLayerVisibleCmd{layer, visible != 0}},
                      "layer not found");
}

clay_result clay_document_set_layer_name(clay_document* doc, clay_layer_id layer,
                                         const char* name) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    // An empty name is what a cleared text field submits, and the name in the
    // document is the only one left to lose — refused rather than saved.
    if (!*name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "a layer name may not be empty");
    // Through the command vocabulary like every other layer edit, so the
    // rename is one undo step and a protected layer refuses it.
    return apply_edit(doc, scene::Command{scene::SetLayerNameCmd{layer, name}},
                      "layer not found");
}

clay_result clay_document_set_layer_protection(clay_document* doc, clay_layer_id layer,
                                               int32_t ghost, int32_t locked) {
    return apply_edit(
        doc, scene::Command{scene::SetLayerProtectionCmd{layer, ghost != 0, locked != 0}},
        "layer not found");
}

clay_result clay_document_layer_protection(const clay_document* doc, clay_layer_id layer,
                                           int32_t* out_ghost, int32_t* out_locked) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (out_ghost) *out_ghost = l->ghost ? 1 : 0;
    if (out_locked) *out_locked = l->locked ? 1 : 0;
    return CLAY_OK;
}

// -- discovering layers ------------------------------------------------------

clay_result clay_document_layer_count(const clay_document* doc, size_t* out_count) {
    if (!doc || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or count");
    *out_count = doc->doc.document.layers.size();
    return CLAY_OK;
}

clay_result clay_document_layer_at(const clay_document* doc, size_t index,
                                   clay_layer_id* out_layer) {
    if (!doc || !out_layer)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out pointer");
    const std::vector<scene::Layer>& layers = doc->doc.document.layers;
    if (index >= layers.size())
        return fail(CLAY_ERROR_NOT_FOUND, "no layer at index " + std::to_string(index) +
                                              ": the document has " +
                                              std::to_string(layers.size()) + " layers");
    *out_layer = layers[index].id;
    return CLAY_OK;
}

namespace {
constexpr std::size_t kLayerInfoOriginal =
    offsetof(clay_layer_info, locked) + sizeof(std::int32_t);

// Who owns a shared edit list, by the FIRST-IN-STACK-ORDER rule the document
// writer uses to decide whose record carries the bytes (scene::kSceneMinor,
// minor 15). Deriving it the same way on both sides is what makes the answer
// survive a save and reload, and what makes removing the layer that happened
// to be instanced re-home the link instead of dangling it: the first survivor
// becomes the owner and reports 0.
void fill_content_share(const std::vector<scene::Layer>& layers, const scene::Layer& l,
                        clay_layer_info* out) {
    if (!l.sdf) return;  // a voxel or mesh layer shares nothing
    std::uint32_t count = 0;
    clay_layer_id owner = 0;
    for (const scene::Layer& other : layers) {
        if (other.sdf != l.sdf) continue;
        ++count;
        if (owner == 0) owner = other.id;
    }
    out->share_count = count;
    out->content_source = owner == l.id ? 0 : owner;
}
}  // namespace

clay_result clay_document_layer_info(const clay_document* doc, clay_layer_id layer,
                                     clay_layer_info* out_info) {
    if (!doc || !out_info) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or info");
    const std::vector<scene::Layer>& layers = doc->doc.document.layers;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const scene::Layer& l = layers[i];
        if (l.id != layer) continue;
        // An OUTPUT descriptor: struct_size is the caller saying how much of
        // it exists, so it is probed and then written back (see
        // clay_layer_field_report).
        clay_layer_info probe;
        clay_result r = read_desc(out_info, kLayerInfoOriginal, &probe);
        if (r != CLAY_OK) return r;
        const std::uint32_t declared = out_info->struct_size;
        clay_layer_info filled{};
        filled.id = l.id;
        filled.representation = static_cast<std::int32_t>(l.kind);
        filled.stack_index = static_cast<std::int32_t>(i);
        filled.visible = l.visible ? 1 : 0;
        filled.ghost = l.ghost ? 1 : 0;
        filled.locked = l.locked ? 1 : 0;
        fill_content_share(layers, l, &filled);
        write_desc(out_info, declared, filled);
        return CLAY_OK;
    }
    return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
}

clay_result clay_layer_name(const clay_document* doc, clay_layer_id layer, char* buffer,
                            size_t* size) {
    if (!doc || !size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or size");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const std::size_t needed = l->name.size() + 1;
    if (!buffer) {
        *size = needed;
        return CLAY_OK;
    }
    if (*size < needed) {
        *size = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the name needs " + std::to_string(needed) + " bytes");
    }
    std::memcpy(buffer, l->name.c_str(), needed);
    *size = needed;
    return CLAY_OK;
}

clay_result clay_document_set_layer_transform(clay_document* doc, clay_layer_id layer,
                                              const float position[3],
                                              const float rotation_axis[3],
                                              float rotation_angle, float scale) {
    math::Transform xform;
    clay_result r = read_transform(position, rotation_axis, rotation_angle, scale, &xform);
    if (r != CLAY_OK) return r;
    // The identity triple, so this call REPLACES a squash rather than leaving
    // one behind: the placement is one value in this ABI, and a caller setting
    // it means the whole of it.
    return apply_edit(
        doc,
        scene::Command{scene::SetLayerTransformCmd{layer, xform, kernel::cf3(1.0f, 1.0f, 1.0f)}},
        "layer not found");
}

clay_result clay_document_set_layer_transform_nonuniform(clay_document* doc, clay_layer_id layer,
                                                         const float position[3],
                                                         const float rotation_axis[3],
                                                         float rotation_angle,
                                                         const float scale[3]) {
    kernel::cfloat3 axes;
    clay_result r = read_scale_axes(scale, &axes);
    if (r != CLAY_OK) return r;
    // The rotation and position come through the same reader every other
    // transform in this ABI uses; the scale is the part that is not a
    // similarity, so it rides beside the Transform rather than inside it —
    // exactly as clay_layer_set_transform_nonuniform does one level down.
    math::Transform xform;
    r = read_transform(position, rotation_axis, rotation_angle, 1.0f, &xform);
    if (r != CLAY_OK) return r;
    return apply_edit(doc, scene::Command{scene::SetLayerTransformCmd{layer, xform, axes}},
                      "layer not found");
}

clay_result clay_document_layer_transform(const clay_document* doc, clay_layer_id layer,
                                          float out_position[3], float out_rotation_axis[3],
                                          float* out_rotation_angle, float* out_scale) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    // One float cannot express three, and every way of pretending otherwise is
    // a lie a host would act on — the uniform factor alone describes a
    // differently-shaped subtool, and a read-change-write through the uniform
    // setter would round the artist's squash away. The same refusal
    // clay_layer_node_transform makes one level down, for the same reason.
    if (!scene::scale_axes_uniform(l->scale_axes))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this layer carries a per-axis scale: use "
                    "clay_document_layer_transform_nonuniform");
    if (out_position) {
        out_position[0] = l->xform.position.x;
        out_position[1] = l->xform.position.y;
        out_position[2] = l->xform.position.z;
    }
    axis_angle_of(l->xform, out_rotation_axis, out_rotation_angle);
    if (out_scale) *out_scale = l->xform.scale * l->scale_axes.x;
    return CLAY_OK;
}

clay_result clay_document_layer_transform_nonuniform(const clay_document* doc, clay_layer_id layer,
                                                     float out_position[3],
                                                     float out_rotation_axis[3],
                                                     float* out_rotation_angle,
                                                     float out_scale[3]) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (out_position) {
        out_position[0] = l->xform.position.x;
        out_position[1] = l->xform.position.y;
        out_position[2] = l->xform.position.z;
    }
    axis_angle_of(l->xform, out_rotation_axis, out_rotation_angle);
    // The two scales multiply and this call reports the product, so a layer
    // placed through the UNIFORM setter answers (s, s, s) here rather than
    // (1, 1, 1) with the factor hidden somewhere the caller cannot see. That is
    // what lets ONE manipulator read this call and never branch.
    if (out_scale) {
        out_scale[0] = l->xform.scale * l->scale_axes.x;
        out_scale[1] = l->xform.scale * l->scale_axes.y;
        out_scale[2] = l->xform.scale * l->scale_axes.z;
    }
    return CLAY_OK;
}

clay_result clay_set_layer_mirror(clay_document* doc, clay_layer_id layer_id, int32_t axis_x,
                                  int32_t axis_y, int32_t axis_z, float mirror_k) {
    // Through the command vocabulary like every other layer edit: writing the
    // fields directly skipped the lock check that apply_edit makes and left
    // nothing on the undo stack, so a mirror could neither be refused on a
    // locked layer nor undone.
    const std::uint8_t axes = static_cast<std::uint8_t>((axis_x ? scene::kMirrorX : 0) |
                                                        (axis_y ? scene::kMirrorY : 0) |
                                                        (axis_z ? scene::kMirrorZ : 0));
    return apply_edit(doc, scene::Command{scene::SetLayerMirrorCmd{layer_id, axes, mirror_k}},
                      "layer not found");
}

clay_result clay_set_layer_radial(clay_document* doc, clay_layer_id layer_id, int32_t axis,
                                  int32_t count, float radial_k) {
    // Rejected rather than clamped: a caller who passed axis 3 meant something,
    // and silently arraying about Y would be a wrong picture rather than an
    // error they could see.
    if (axis < 0 || axis > 2) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radial axis must be 0..2");
    if (!(radial_k >= 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radial blend must be >= 0");
    if (count < 0 || count > 65535)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "radial count out of range");
    // 0 and 1 both mean off, and both are stored as the caller sent them: a
    // host that steps a slider down to 1 gets the same field as 0 without the
    // ABI deciding which of the two it "really" meant.
    return apply_edit(doc,
                      scene::Command{scene::SetLayerRadialCmd{
                          layer_id, static_cast<std::uint16_t>(count),
                          static_cast<std::uint8_t>(axis), radial_k}},
                      "layer not found");
}

// -- item builder (c-abi spec: item builder for composed edits) --------------

clay_item* clay_item_create(int32_t prim, const float* params, size_t param_count) {
    if (!prim_is_known(prim)) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown primitive type");
        return nullptr;
    }
    // A volume needs samples, and this entry point has no way to supply them:
    // one built here could only ever be a silently empty item, which looks
    // like it worked. clay_item_volume_from_mesh is the producer.
    if (prim == CLAY_PRIM_VOLUME) {
        fail(CLAY_ERROR_INVALID_ARGUMENT,
             "use clay_item_volume_from_mesh to build a volume; clay_item_create has no "
             "samples to give it");
        return nullptr;
    }
    if (check_params("primitive", params, param_count, kPrimParams[prim]) != CLAY_OK)
        return nullptr;
    if (prim == CLAY_PRIM_EXTRUDE && params[0] <= 0.0f) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "half_depth must be > 0");
        return nullptr;
    }
    float p[scene::kMaxPrimParams] = {};
    for (size_t i = 0; i < param_count; ++i) p[i] = params[i];
    if (canonical_prim_params(prim, p) != CLAY_OK) return nullptr;
    auto* item = new clay_item();
    item->node.prim.type = static_cast<scene::PrimType>(prim);
    std::memcpy(item->node.prim.params, p, sizeof p);
    return item;
}

void clay_item_destroy(clay_item* item) { delete item; }

clay_result clay_item_set_position(clay_item* item, const float position[3]) {
    if (!item || !position) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or position");
    item->node.xform.position = kernel::cf3(position[0], position[1], position[2]);
    return CLAY_OK;
}

clay_result clay_item_set_rotation(clay_item* item, const float axis[3], float radians) {
    if (!item || !axis) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or axis");
    kernel::cfloat3 a = kernel::cf3(axis[0], axis[1], axis[2]);
    if (kernel::cdot2(a) <= 0.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "rotation axis must be non-zero");
    item->node.xform.rotation = math::Quat::from_axis_angle(a, radians);
    return CLAY_OK;
}

clay_result clay_item_set_scale(clay_item* item, float scale) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (scale <= 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "scale must be > 0");
    item->node.xform.scale = scale;
    return CLAY_OK;
}

clay_result clay_item_set_scale_nonuniform(clay_item* item, const float scale[3]) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    // The two scales MULTIPLY rather than replace: the uniform one stays the
    // similarity factor and this modulates it per axis, so setting both in
    // either order means the same thing.
    return read_scale_axes(scale, &item->node.scale_axes);
}

clay_result clay_item_set_op(clay_item* item, int32_t op) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!op_is_known(op)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown combine op");
    item->node.op = static_cast<scene::Op>(op);
    return CLAY_OK;
}

clay_result clay_item_set_blend(clay_item* item, int32_t blend, float k) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!blend_is_known(blend)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown blend");
    if (k < 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "blend k must be >= 0");
    item->node.blend = scene::Blend{static_cast<scene::BlendProfile>(blend), k};
    return CLAY_OK;
}

clay_result clay_item_set_rounding(clay_item* item, float rounding) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (rounding < 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "rounding must be >= 0");
    item->node.rounding = rounding;
    return CLAY_OK;
}

clay_result clay_item_set_color(clay_item* item, const float rgb[3]) {
    if (!item || !rgb) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or color");
    item->node.color = kernel::cf3(rgb[0], rgb[1], rgb[2]);
    return CLAY_OK;
}

clay_result clay_item_set_mirror(clay_item* item, int32_t mirror) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    // One rule with clay_item_desc.mirror: negative excludes, 0 and 1 both
    // follow the layer's mirror — which is also what a builder starts as.
    item->node.mirror = mirror >= 0;
    return CLAY_OK;
}

clay_result clay_item_add_deformer(clay_item* item, int32_t deform, const float* params,
                                   size_t param_count, int32_t ease) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    // Bounded by the TABLE's own size rather than by a named enumerator. Twice
    // now a deformer was declared, documented, given a parameter count and
    // handled by make_deformer, and still refused here because this bound was
    // left naming the previous last kind — magnify and noise the first time,
    // the ranged pair the second. The binding parity gate cannot see it: it
    // checks that the ENUMERATOR exists, not that a call accepts it.
    constexpr int kDeformKinds = sizeof kDeformParams / sizeof kDeformParams[0];
    if (deform < 0 || deform >= kDeformKinds)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown deformer kind");
    if (kDeformParams[deform] < 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this deformer carries a payload that is not a parameter list; "
                    "use its own entry point (bend_curve: clay_item_add_bend_curve, "
                    "lattice: clay_item_add_lattice)");
    clay_result r = check_params("deformer", params, param_count, kDeformParams[deform]);
    if (r != CLAY_OK) return r;
    if ((r = check_ease(ease)) != CLAY_OK) return r;
    scene::Deformer d;
    if ((r = make_deformer(deform, params, &d)) != CLAY_OK) return r;
    d.ease = static_cast<std::uint8_t>(ease);
    item->node.deformers.push_back(d);  // chain order is call order
    return CLAY_OK;
}

clay_result clay_item_add_bend_curve(clay_item* item, const float* guide_xyz, size_t point_count,
                                     int32_t point_type, float t0, float t1) {
    if (!item || !guide_xyz) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or guide");
    clay_result r = check_batch("guide points", point_count);
    if (r != CLAY_OK) return r;
    if (point_count < 2)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "bend_curve needs at least two guide points");
    if (!point_type_is_known(point_type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown point type: " + std::to_string(point_type));
    if (t0 == t1) return fail(CLAY_ERROR_INVALID_ARGUMENT, "bend_curve needs t0 != t1");

    std::vector<scene::StrokePoint> guide;
    guide.reserve(point_count);
    for (std::size_t i = 0; i < point_count; ++i) {
        scene::StrokePoint sp;
        sp.pos = kernel::cf3(guide_xyz[i * 3], guide_xyz[i * 3 + 1], guide_xyz[i * 3 + 2]);
        sp.type = static_cast<scene::StrokePointType>(point_type);
        guide.push_back(sp);
    }
    // A guide of zero length has no arc to lay the span onto, and the division
    // that maps one to the other would be by zero. Measured on the CONTROL
    // points: tessellating a curve whose points all coincide cannot give it
    // length, so this catches the degenerate case before it costs anything.
    if (!(scene::guide_arc_length(guide) > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "bend_curve guide has zero length");

    item->node.deformers.push_back(scene::Deformer::bend_curve(std::move(guide), t0, t1));
    return CLAY_OK;
}

clay_result clay_item_add_alpha(clay_item* item, const float* samples, int32_t width,
                                int32_t height, const float centre[3], const float direction[3],
                                const float tangent[3], float extent, float radius, float amplitude,
                                int32_t ease) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!samples) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null samples");
    if (!centre || !direction || !tangent)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null centre, direction or tangent");
    // Below 2 on an axis there are no adjacent samples to interpolate between,
    // so the stamp would be inert; refusing says so rather than silently
    // appending a deformer that does nothing.
    if (width < 2 || height < 2)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "an alpha needs at least 2x2 samples; there is nothing to interpolate below "
                    "that");
    // The multiply is what a caller most easily gets wrong, and getting it
    // wrong means reading past their buffer.
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (count > kMaxAlphaSamples)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "an alpha of " + std::to_string(width) + "x" + std::to_string(height) +
                        " exceeds the " + std::to_string(kMaxAlphaSamples) + "-sample ceiling");
    if (!(extent > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "an alpha's extent must be positive");
    // A zero direction is not a plane, and a non-positive radius is a region
    // that reaches nothing. Both were accepted and both were silently inert —
    // the kernel substitutes local +Z for a zero direction and floors the
    // radius at 1e-6 — which is exactly the case this file already refuses
    // width < 2 for: a deformer that is appended, returns CLAY_OK and does
    // nothing is harder to notice than an error.
    //
    // NOT the mesh brush descriptor's rule, and the asymmetry is deliberate:
    // there, all-zeroes means "the surface normal under the centre", which it
    // resolves by querying the mesh. An SDF item has no surface at authoring
    // time to read one from, so the same spelling cannot mean the same thing.
    if (!(kernel::clength(kernel::cf3(direction[0], direction[1], direction[2])) > 1e-9f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "an alpha's direction must have length; it is the normal of the stamp's "
                    "plane, and all-zeroes is the mesh brush's convention, not this one");
    if (!(radius > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "an alpha's radius must be positive");
    if (ease < 0 || ease >= CLAY_EASE_COUNT)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "ease index out of range");

    item->node.deformers.push_back(
        scene::Deformer::alpha(kernel::cf3(centre[0], centre[1], centre[2]),
                               kernel::cf3(direction[0], direction[1], direction[2]),
                               kernel::cf3(tangent[0], tangent[1], tangent[2]), samples, width,
                               height, extent, radius, amplitude, static_cast<std::uint8_t>(ease)));
    return CLAY_OK;
}

clay_result clay_item_set_gate(clay_item* item, const clay_mask* mask, float threshold,
                               float width) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (!(width > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a gate's width must be positive; a step in the field has no finite "
                    "Lipschitz bound and nothing could march it");

    // Memoised against the mask's own change token: repainting rebakes, and
    // gating another item by the same unchanged mask does not. The band rule
    // lives in GateBake so the two bindings cannot drift on it.
    std::shared_ptr<const field::FieldVolume> measured =
        mask->gate_bake.gate_for(*m, threshold > 0.0f ? threshold : 0.5f, width);
    if (!measured)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the mask is empty or nothing reaches the threshold, so the gate would "
                    "protect nothing");

    item->node.gate = std::move(measured);
    item->node.gate_width = width;
    return CLAY_OK;
}

clay_result clay_item_add_lattice(clay_item* item, const float min[3], const float max[3],
                                  int32_t nx, int32_t ny, int32_t nz, const float* offsets_xyz) {
    if (!item || !min || !max) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or box");
    const int cap = scene::Deformer::kMaxLatticeDivisions;
    if (nx < 2 || ny < 2 || nz < 2 || nx > cap || ny > cap || nz > cap)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "lattice divisions must be in [2, " + std::to_string(cap) +
                        "] per axis; the cage is evaluated per sample, at nx*ny*nz "
                        "multiply-adds each time");
    const math::Aabb box{kernel::cf3(min[0], min[1], min[2]),
                         kernel::cf3(max[0], max[1], max[2])};
    if (box.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the cage's box is empty; there is nothing to span");

    scene::Deformer d = scene::Deformer::lattice(box.min, box.max, nx, ny, nz);
    if (offsets_xyz) {
        for (std::size_t n = 0; n < d.cage.size(); ++n)
            d.cage[n] = kernel::cf3(offsets_xyz[n * 3], offsets_xyz[n * 3 + 1],
                                    offsets_xyz[n * 3 + 2]);
    }
    item->node.deformers.push_back(std::move(d));
    return CLAY_OK;
}

clay_result clay_item_set_repeat_grid(clay_item* item, const float spacing[3],
                                      const float counts[3]) {
    if (!item || !spacing) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or spacing");
    kernel::cfloat3 s = kernel::cf3(spacing[0], spacing[1], spacing[2]);
    if (s.x <= 0.0f || s.y <= 0.0f || s.z <= 0.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "spacing must be > 0 on every axis");
    if (!counts) {
        item->node.repeat = scene::Repeat::grid_infinite(s);
        return CLAY_OK;
    }
    kernel::cfloat3 c = kernel::cf3(counts[0], counts[1], counts[2]);
    if (c.x < 0.0f || c.y < 0.0f || c.z < 0.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "counts must be >= 0 (max cell index)");
    if (s.x != s.y || s.y != s.z)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "finite grids use one spacing for all axes");
    item->node.repeat = scene::Repeat::grid_finite(s.x, c);
    return CLAY_OK;
}

clay_result clay_item_set_repeat_radial(clay_item* item, int32_t count, float offset) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (count < 2) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radial count must be >= 2");
    item->node.repeat = scene::Repeat::radial(count, offset);
    return CLAY_OK;
}

clay_result clay_item_set_profile(clay_item* item, int32_t profile, const float* params,
                                  size_t param_count) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!scene::prim_is_lift(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a profile needs a lift primitive");
    if (profile < 0 || profile > CLAY_PROFILE_POLYGON)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown profile kind");
    if (profile == CLAY_PROFILE_POLYGON)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a polygon profile takes its vertices, not parameters");
    clay_result r = check_params("profile", params, param_count, kProfileParams[profile]);
    if (r != CLAY_OK) return r;
    scene::Profile p{static_cast<std::uint8_t>(profile), {}};
    for (size_t i = 0; i < param_count; ++i) p.params[i] = params[i];
    item->node.profile = p;
    item->node.profile_points.clear();
    return CLAY_OK;
}

clay_result clay_item_add_loft_profile(clay_item* item, int32_t profile, const float* params,
                                       size_t param_count, const float* polygon_xy,
                                       size_t polygon_count) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!scene::prim_carries_profiles(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "loft profiles need CLAY_PRIM_LOFT or CLAY_PRIM_SWEPT");
    if (profile < 0 || profile > CLAY_PROFILE_POLYGON)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown profile kind");

    scene::Profile p{static_cast<std::uint8_t>(profile), {}};
    std::vector<kernel::cfloat2> vertices;
    if (profile == CLAY_PROFILE_POLYGON) {
        // Three vertices is the least that bounds an area; fewer describes no
        // cross-section, which is a caller mistake rather than an empty one.
        if (polygon_count < 3 || !polygon_xy)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "a polygon profile needs 3+ vertices");
        clay_result r = check_batch("polygon vertices", polygon_count);
        if (r != CLAY_OK) return r;
        vertices.reserve(polygon_count);
        for (size_t i = 0; i < polygon_count; ++i)
            vertices.push_back(kernel::cf2(polygon_xy[i * 2], polygon_xy[i * 2 + 1]));
    } else {
        clay_result r = check_params("profile", params, param_count, kProfileParams[profile]);
        if (r != CLAY_OK) return r;
        for (size_t i = 0; i < param_count; ++i) p.params[i] = params[i];
    }
    item->node.profiles.push_back(p);
    item->node.profile_polygons.push_back(std::move(vertices));
    return CLAY_OK;
}

clay_result clay_item_set_profile_polygon(clay_item* item, const float* xy, size_t count) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!scene::prim_is_lift(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a profile needs a lift primitive");
    if (count < 3) return fail(CLAY_ERROR_INVALID_ARGUMENT, "a polygon needs at least 3 vertices");
    clay_result r = check_payload("polygon vertices", xy, count);
    if (r != CLAY_OK) return r;
    std::vector<kernel::cfloat2> vertices;
    vertices.reserve(count);
    for (size_t i = 0; i < count; ++i)
        vertices.push_back(kernel::cf2(xy[i * 2], xy[i * 2 + 1]));
    item->node.profile = scene::Profile::polygon();
    item->node.profile_points = std::move(vertices);
    return CLAY_OK;
}

clay_result clay_item_set_stroke_points(clay_item* item, const float* xyzr, size_t count) {
    return clay_item_set_curve_points(item, xyzr, count, nullptr, nullptr, nullptr);
}

clay_result clay_item_add_child(clay_item* item, const float position[3], float radius,
                                std::int32_t parent) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!scene::prim_is_armature(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "children need CLAY_PRIM_ARMATURE");
    if (!position) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null position");
    if (!(radius >= 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radius must be >= 0");
    const std::int32_t n = static_cast<std::int32_t>(item->node.stroke.size());
    if (parent >= n) return fail(CLAY_ERROR_INVALID_ARGUMENT, "parent index out of range");
    scene::StrokePoint node;
    node.pos = kernel::cf3(position[0], position[1], position[2]);
    node.radius = radius;
    item->node.stroke.push_back(node);
    // Negative means "the node before this one", so a caller walking a limb
    // outward never has to track indices it just created.
    const std::int32_t chosen = parent >= 0 ? parent : (n > 0 ? n - 1 : 0);
    item->node.armature_parents.push_back(static_cast<std::uint32_t>(chosen));
    return CLAY_OK;
}

clay_result clay_item_set_armature_parents(clay_item* item, const uint32_t* parents,
                                           size_t count) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!scene::prim_is_armature(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "parents need CLAY_PRIM_ARMATURE");
    if (count && !parents) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null parents");
    std::vector<std::uint32_t> tree(parents, parents + count);
    for (std::size_t i = 0; i < tree.size(); ++i)
        if (tree[i] >= tree.size())
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "armature parent index out of range");
    // A cycle would make the field depend on the order the links are walked
    // rather than on the tree, so it is refused where the reason can be said.
    for (std::size_t i = 0; i < tree.size(); ++i) {
        std::size_t walk = i, steps = 0;
        while (tree[walk] != walk && steps++ <= tree.size()) walk = tree[walk];
        if (steps > tree.size())
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "armature parents form a cycle");
    }
    item->node.armature_parents = std::move(tree);
    return CLAY_OK;
}

clay_result clay_item_set_armature_signs(clay_item* item, const int8_t* signs, size_t count) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (!scene::prim_is_armature(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "signs need CLAY_PRIM_ARMATURE");
    if (count && !signs) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null signs");
    // Only +1 and -1 mean anything. A zero, or a magnitude, is refused rather
    // than coerced — the negative-radius convention this feature deliberately
    // did not take would have re-read invalid input, and so would this.
    for (std::size_t i = 0; i < count; ++i)
        if (signs[i] != 1 && signs[i] != -1)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "an armature sign must be +1 or -1");
    item->node.armature_signs.assign(signs, signs + count);
    return CLAY_OK;
}

clay_result clay_item_set_curve_points(clay_item* item, const float* xyzr, size_t count,
                                       const int32_t* types, const float* in_handles_xyz,
                                       const float* out_handles_xyz) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (item->node.prim.type != scene::PrimType::Stroke &&
        !scene::prim_is_swept(item->node.prim.type) &&
        !scene::prim_is_armature(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "curve points need CLAY_PRIM_STROKE, CLAY_PRIM_SWEPT or "
                    "CLAY_PRIM_ARMATURE");
    std::vector<scene::StrokePoint> points;
    clay_result r = read_curve_points(xyzr, count, types, in_handles_xyz, out_handles_xyz,
                                      &points);
    if (r != CLAY_OK) return r;
    item->node.stroke = std::move(points);
    return CLAY_OK;
}

clay_result clay_item_set_curve(clay_item* item, int32_t closed, float tolerance) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (item->node.prim.type != scene::PrimType::Stroke &&
        !scene::prim_is_swept(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "curve settings need CLAY_PRIM_STROKE or CLAY_PRIM_SWEPT");
    // A closed GUIDE is deliberately out of scope: parallel transport around a
    // loop does not generally return to its starting frame, and the leftover
    // twist is real geometry rather than a bug. Refused rather than ignored —
    // silently dropping the flag would tell the caller it had a closed sweep.
    if (closed != 0 && scene::prim_is_swept(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a swept guide cannot be closed: transporting a frame around a loop does "
                    "not close the seam");
    if (!(tolerance > 0.0f))  // also rejects NaN
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "curve tolerance must be > 0");
    item->node.stroke_closed = closed != 0;
    item->node.curve_tolerance = tolerance;
    return CLAY_OK;
}

clay_result clay_layer_armature_edit(clay_document* doc, clay_layer_id layer,
                                     clay_node_id node, std::int32_t op, std::uint32_t target,
                                     const float value[3], float radius,
                                     std::int32_t mirrored) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    const scene::Node* n = (l && l->sdf) ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no such node in that layer");
    if (!scene::prim_is_armature(n->prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "that node is not an armature");

    std::vector<scene::StrokePoint> nodes = n->stroke;
    std::vector<std::uint32_t> parents = n->armature_parents;
    std::vector<std::int8_t> signs = n->armature_signs;
    kernel::cfloat3 v = kernel::cf3(0, 0, 0);
    if (value) v = kernel::cf3(value[0], value[1], value[2]);
    bool ok = false;
    switch (op) {
        case CLAY_ARMATURE_ADD_CHILD:
            if (!value) return fail(CLAY_ERROR_INVALID_ARGUMENT, "add-child needs a position");
            ok = mirrored ? scene::armature_add_child_mirrored(nodes, parents, target, v,
                                                               radius) > 0
                          : scene::armature_add_child(nodes, parents, target, v, radius);
            break;
        case CLAY_ARMATURE_MOVE:
            if (!value) return fail(CLAY_ERROR_INVALID_ARGUMENT, "move needs a delta");
            ok = scene::armature_move(nodes, parents, target, v);
            break;
        case CLAY_ARMATURE_SET_RADIUS:
            ok = scene::armature_set_radius(nodes, target, radius);
            break;
        case CLAY_ARMATURE_DELETE:
            ok = scene::armature_delete_subtree(nodes, parents, signs, target);
            break;
        case CLAY_ARMATURE_SET_SIGN: {
            // The sign rides the radius argument so no signature changes
            // shape; only exactly +1 and -1 are readable as intent.
            if (radius != 1.0f && radius != -1.0f)
                return fail(CLAY_ERROR_INVALID_ARGUMENT, "an armature sign must be +1 or -1");
            ok = scene::armature_set_sign(signs, nodes.size(), target,
                                          radius < 0.0f ? std::int8_t{-1} : std::int8_t{1});
            break;
        }
        default: return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown armature edit");
    }
    if (!ok) return fail(CLAY_ERROR_INVALID_ARGUMENT, "that armature node does not exist");
    return apply_edit(doc,
                      scene::Command{scene::SetArmatureCmd{layer, node, std::move(nodes),
                                                           std::move(parents), std::move(signs),
                                                           n->stroke_blend_k}},
                      "no armature with that id in that layer");
}

clay_result clay_layer_set_stroke_points(clay_document* doc, clay_layer_id layer,
                                         clay_node_id node, const float* xyzr, size_t count,
                                         const int32_t* types, const float* in_handles_xyz,
                                         const float* out_handles_xyz, int32_t closed,
                                         float tolerance) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!(tolerance > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "curve tolerance must be > 0");
    // Both of a swept guide's invariants became reachable here once the command
    // accepted swept nodes, and neither breaks loudly on its own: a closed
    // guide tessellates and bakes the twist seam, and a guide of under two
    // points emits no tape record at all, so the sweep silently disappears.
    // validate_item refuses both when the item is placed; the placed-node path
    // must not be a back door around either.
    if (node_is_swept(doc, layer, node)) {
        if (closed != 0)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "a swept guide cannot be closed: transporting a frame around a loop does "
                        "not close the seam");
        if (count < 2)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "a sweep needs a guide of two or more points");
    }
    std::vector<scene::StrokePoint> points;
    clay_result r = read_curve_points(xyzr, count, types, in_handles_xyz, out_handles_xyz,
                                      &points);
    if (r != CLAY_OK) return r;
    return apply_edit(doc,
                      scene::Command{scene::SetStrokePointsCmd{layer, node, std::move(points),
                                                               closed != 0, tolerance}},
                      "no stroke or curve with that id in that layer");
}

clay_result clay_layer_stroke_points(const clay_document* doc, clay_layer_id layer,
                                     clay_node_id node, float* out_xyzr, size_t* count,
                                     int32_t* out_types, float* out_in_handles_xyz,
                                     float* out_out_handles_xyz, int32_t* out_closed,
                                     float* out_tolerance) {
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    // Nothing sizes the parallel arrays on a size query, so passing one is a
    // caller that meant to read and got the buffer wrong: refused, not ignored.
    if (!out_xyzr && (out_types || out_in_handles_xyz || out_out_handles_xyz))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a size query takes no point buffers");
    const scene::Node* n = nullptr;
    clay_result r = find_curve_node(doc, layer, node, &n);
    if (r != CLAY_OK) return r;

    const std::size_t needed = n->stroke.size();
    if (out_xyzr && *count < needed) {
        *count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the curve has " + std::to_string(needed) + " points");
    }
    // Written on the size query too, so one call answers "how many, and closed?".
    if (out_closed) *out_closed = n->stroke_closed ? 1 : 0;
    if (out_tolerance) *out_tolerance = n->curve_tolerance;
    if (out_xyzr)
        write_curve_points(n->stroke, out_xyzr, out_types, out_in_handles_xyz,
                           out_out_handles_xyz);
    *count = needed;
    return CLAY_OK;
}

clay_result clay_layer_armature_parents(const clay_document* doc, clay_layer_id layer,
                                        clay_node_id node, uint32_t* out_parents,
                                        size_t* count) {
    if (!doc || !count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or count");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const scene::Node* n = l->sdf ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no such node in that layer");
    // Parents are the other half of a different primitive, not an attribute a
    // curve happens to lack — the same typed refusal the tree edits give.
    if (n->is_group || !scene::prim_is_armature(n->prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "that node is not an armature");

    // Counted in NODES, not in stored parents: the xyzr readback and this one
    // are parallel arrays, so they must agree on the count. A tree authored
    // shorter than its points reads back padded with roots — the reading
    // tape_build makes when it compiles the same node, so what comes back is
    // the tree the document evaluates.
    const std::size_t needed = n->stroke.size();
    if (out_parents && *count < needed) {
        *count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the armature has " + std::to_string(needed) + " nodes");
    }
    if (out_parents)
        for (std::size_t i = 0; i < needed; ++i) {
            std::uint32_t parent = i < n->armature_parents.size()
                                       ? n->armature_parents[i]
                                       : static_cast<std::uint32_t>(i);
            if (parent >= needed) parent = static_cast<std::uint32_t>(i);
            out_parents[i] = parent;
        }
    *count = needed;
    return CLAY_OK;
}

clay_result clay_layer_armature_signs(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id node, int8_t* out_signs, size_t* count) {
    if (!doc || !count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or count");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    const scene::Node* n = l->sdf ? l->sdf->find(node) : nullptr;
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no such node in that layer");
    if (n->is_group || !scene::prim_is_armature(n->prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "that node is not an armature");

    // Counted in NODES like the parents readback, so the three armature
    // readbacks are parallel arrays. Signs stored shorter than the nodes read
    // back padded positive — the reading tape_build makes when it compiles the
    // same node, so what comes back is the rig the document evaluates.
    const std::size_t needed = n->stroke.size();
    if (out_signs && *count < needed) {
        *count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the armature has " + std::to_string(needed) + " nodes");
    }
    if (out_signs)
        for (std::size_t i = 0; i < needed; ++i)
            out_signs[i] = i < n->armature_signs.size() ? n->armature_signs[i] : std::int8_t{1};
    *count = needed;
    return CLAY_OK;
}

clay_result clay_item_add_stroke_point(clay_item* item, const float position[3], float radius) {
    if (!item || !position) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or position");
    if (item->node.prim.type != scene::PrimType::Stroke)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke points need CLAY_PRIM_STROKE");
    if (radius < 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke radius must be >= 0");
    item->node.stroke.push_back(
        scene::StrokePoint{kernel::cf3(position[0], position[1], position[2]), radius});
    return CLAY_OK;
}

clay_result clay_item_set_stroke_blend_k(clay_item* item, float k) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (item->node.prim.type != scene::PrimType::Stroke)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke points need CLAY_PRIM_STROKE");
    if (k < 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "blend_k must be >= 0");
    item->node.stroke_blend_k = k;
    return CLAY_OK;
}

clay_result clay_item_set_transition_linear(clay_item* item, const float a[3], const float b[3],
                                            int32_t ease) {
    if (!item || !a || !b) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item or endpoint");
    clay_result r = check_ease(ease);
    if (r != CLAY_OK) return r;
    kernel::cfloat3 p0 = kernel::cf3(a[0], a[1], a[2]);
    kernel::cfloat3 p1 = kernel::cf3(b[0], b[1], b[2]);
    // the weight divides by |b - a|; a degenerate segment would be a NaN field
    if (kernel::cdot2(p1 - p0) <= 0.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "transition needs a != b");
    item->node.transition = scene::Transition{p0, p1, 0.0f, 1.0f, static_cast<std::uint8_t>(ease)};
    item->has_transition = true;
    item->transition_is_linear = true;
    return CLAY_OK;
}

clay_result clay_item_set_transition_radial(clay_item* item, float r0, float r1, int32_t ease) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    clay_result r = check_ease(ease);
    if (r != CLAY_OK) return r;
    if (r0 == r1) return fail(CLAY_ERROR_INVALID_ARGUMENT, "transition needs r0 != r1");
    scene::Transition t;
    t.r0 = r0;
    t.r1 = r1;
    t.ease = static_cast<std::uint8_t>(ease);
    item->node.transition = t;
    item->has_transition = true;
    item->transition_is_linear = false;
    return CLAY_OK;
}

clay_result clay_layer_add_item(clay_document* doc, clay_layer_id layer_id, const clay_item* item,
                                clay_node_id* out_node) {
    if (!doc || !item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or item");
    clay_result r = validate_item(*item);
    if (r != CLAY_OK) return r;
    return insert_node(doc, layer_id, item->node, out_node);
}

namespace {

// The half both entry points share: validate, find the layer, resolve. Kept in
// one place so a preview can never disagree with the move it is previewing.
// `out_radius` is the radius AS READ through the versioned descriptor, which is
// the only value a caller may use: reading params->radius directly would take a
// field an older struct version does not carry.
// Yields the PREPARED items rather than resolved warps: the callers either want
// node ids (the preview) or resolve one at a time (the apply). See
// apply_surface_gesture.
clay_result resolve_move(const clay_document* doc, clay_layer_id layer, const float centre[3],
                         const float displacement[3], const clay_move_params* params,
                         const scene::Layer** out_layer,
                         std::vector<brush::PreparedMove>* out_prepared,
                         float* out_radius = nullptr) {
    if (!doc || !centre || !displacement)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document, centre or displacement");
    if (!params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null move parameters");
    clay_move_params p;
    clay_result r = read_desc(params, kMoveParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (!(p.radius > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radius must be > 0");
    if ((r = check_ease(p.ease)) != CLAY_OK) return r;

    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "no layer with id " + std::to_string(layer));
    if (l->kind != scene::LayerKind::Sdf || !l->sdf)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this layer holds no SDF content to move");

    brush::MoveSettings settings;
    settings.radius = p.radius;
    settings.ease = static_cast<std::uint8_t>(p.ease);
    settings.front_only = p.front_only != 0;

    *out_layer = l;
    if (out_radius) *out_radius = p.radius;
    // A drag of zero displacement moves nothing, and `move_brush` said so by
    // returning no warps. Said here instead, because preparing is what happens
    // now and preparing does not look at the displacement.
    const kernel::cfloat3 pull =
        kernel::cf3(displacement[0], displacement[1], displacement[2]);
    if (kernel::clength(pull) <= 0.0f) {
        out_prepared->clear();
        return CLAY_OK;
    }
    *out_prepared =
        brush::prepare_move(*l, kernel::cf3(centre[0], centre[1], centre[2]), settings);
    return CLAY_OK;
}

// What one drag can state about the FRONTIER it dirties from (#360), decided
// once per gesture and shared by the prepare pass below and the gesture's own
// invalidation: the ordinal walk is one walk, not one per consumer.
struct DragFrontier {
    // The two gates command_frontier makes, decided here for the whole drag
    // because every command it issues lands on the same layer. A drag on a
    // non-active layer moves the below half the frontier path carries forward
    // untouched; an active layer whose content a visible lower layer instances
    // moves BOTH halves on one edit. Either refusal means the legacy drop, and
    // nothing the prepare pass records could ever be used.
    bool usable = false;
    // Whether EVERY warp node resolved to a root ordinal. A gesture may state
    // a frontier only when it can vouch for all of its commands; one
    // unresolved node and the whole drag takes the legacy drop.
    bool all_resolved = false;
    // The earliest root ordinal any warp dirties from -- MIN for
    // touch_region_locked's reason: the kept prefix has to sit before
    // everything that will move.
    std::uint32_t min_ordinal = clay_document::kFrontierClean;
    // (ordinal, pre-apply influence bound) per resolved warp -- what
    // frontier_prepare consumes to find the bricks the applies will dirty.
    std::vector<std::pair<std::uint32_t, math::Aabb>> spans;
};

// Filled through an out-parameter, as resolve_move above is: this namespace
// sits inside the extern "C" block, where a function RETURNING a class type
// has no C-compatible calling convention (MSVC gates it as C4190). Parameters
// of C++ type are fine — it is the return slot that has to be C-shaped.
// Takes the PREPARED items rather than resolved warps, because the only thing
// it ever read from a warp was its node id — and resolving a warp allocates.
// See apply_surface_gesture for why that matters.
void drag_frontier(const clay_document* doc, const scene::Layer& layer,
                   const std::vector<brush::PreparedMove>& prepared, DragFrontier* out) {
    DragFrontier& df = *out;
    const scene::Document& d = doc->doc.document;
    const scene::Layer* active = nullptr;
    for (const scene::Layer& l : d.layers) {
        if (!l.visible || l.kind != scene::LayerKind::Sdf || !l.sdf) continue;
        active = &l;
    }
    if (!active || active->id != layer.id) return;
    for (const scene::Layer& l : d.layers) {
        if (&l == active) continue;
        if (l.visible && l.kind == scene::LayerKind::Sdf && l.sdf == active->sdf) return;
    }
    df.usable = true;
    df.all_resolved = !prepared.empty();
    df.spans.reserve(prepared.size());
    // Per warp: the root ordinal it will dirty from, and its PRE-apply
    // influence. The post-apply territory a growing displacement reaches next
    // frame is dirtied by the apply itself and refilled once by the full path,
    // after which the prepare pass seeds it -- so a brick the drag grows into
    // is slow for one frame and resumed after.
    // ONE command, its node rewritten per warp, rather than one built per warp.
    // `scene::Command` is a variant over twenty alternatives and the widest of
    // them carries a whole Layer by value, so constructing and destroying it
    // inside this loop costs more than the bound it is asking for -- 138 times
    // on a thousand-item drag, 1,392 on a ten-thousand-item one. What the bound
    // reads is the (layer, node) pair; the deformer list is empty here and stays
    // empty, because `command_influence_bound` routes every (layer, node)
    // parameter edit to the same node_command_bound and never looks at it.
    scene::Command probe_cmd{scene::SetDeformersCmd{layer.id, scene::kNoNode, {}}};
    auto& probe = std::get<scene::SetDeformersCmd>(probe_cmd);
    for (const brush::PreparedMove& p : prepared) {
        std::uint32_t ordinal = 0;
        if (!root_ordinal_of(*layer.sdf, p.node, &ordinal)) {
            df.all_resolved = false;
            continue;
        }
        df.min_ordinal = std::min(df.min_ordinal, ordinal);
        probe.node = p.node;
        const math::Aabb bound = scene::command_influence_bound(d, probe_cmd);
        if (bound.empty()) continue;
        df.spans.emplace_back(ordinal, bound);
    }
}

// Record, for every resume entry the coming applies will dirty, the value of
// the active chain BEFORE the dragged nodes -- the seed the frontier path
// folds each frame's deformer suffix onto (#360). Runs BEFORE the applies of
// one clay_layer_move_surface call, EVERY frame: no drag-start detection is
// needed, because entries that already hold a usable prefix at this frame's
// boundary are skipped in frontier_prepare, so frame one pays for the region
// and later frames pay only for bricks the growing drag has just reached.
// Missing entries are not created -- a brick the store never held falls to
// the full path, which is the fallback contract.
//
// Takes cache_lock() itself (twice, around an unlocked evaluation) and must
// NOT be called with it held.
void prepare_frontier_seeds(clay_document* doc, const DragFrontier& frontier) {
    const scene::Document& d = doc->doc.document;
    if (frontier.spans.empty()) return;
    clay_document::FrontierPrepare prep = doc->frontier_prepare(frontier.spans);  // phase A, locked
    if (prep.jobs.empty()) return;
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    if (!cpu) return;
    // Phase B, off the lock: evaluate each job's prefix at its brick's
    // lattice, exactly as the refill would have -- same brick geometry from
    // the KEY (the arithmetic touch_region trusts), same cull region, same
    // document pad through compile_layer_prefix. Serial, deliberately: a
    // region is prepared once per gesture, not per frame, and a pool here
    // would burn cores to save microseconds nothing is waiting on.
    std::vector<float> points;
    for (clay_document::FrontierJob& job : prep.jobs) {
        const float width = static_cast<float>(job.key.dims[0]) * job.key.spacing;
        const kernel::cfloat3 lo =
            kernel::cf3(static_cast<float>(job.key.x), static_cast<float>(job.key.y),
                        static_cast<float>(job.key.z)) *
            width;
        const kernel::cfloat3 size =
            kernel::cf3(job.key.spacing * static_cast<float>(job.key.dims[0]),
                        job.key.spacing * static_cast<float>(job.key.dims[1]),
                        job.key.spacing * static_cast<float>(job.key.dims[2]));
        const math::Aabb box = math::Aabb{lo, lo + size}.dilated(job.key.band);
        scene::CullRegion cull{box};
        scene::Tape prefix;
        if (!scene::compile_layer_prefix(d, job.boundary, &prefix, &cull, prep.index.get()))
            continue;  // values stay empty; phase C skips the job
        points.resize(job.per * 3);
        std::size_t at = 0;
        for (int k = 0; k < job.key.dims[2]; ++k)
            for (int j = 0; j < job.key.dims[1]; ++j)
                for (int x = 0; x < job.key.dims[0]; ++x) {
                    const kernel::cfloat3 pt =
                        lo + kernel::cf3(static_cast<float>(x) * job.key.spacing,
                                         static_cast<float>(j) * job.key.spacing,
                                         static_cast<float>(k) * job.key.spacing);
                    points[at * 3] = pt.x;
                    points[at * 3 + 1] = pt.y;
                    points[at * 3 + 2] = pt.z;
                    ++at;
                }
        job.values.resize(job.per);
        if (job.want_colour) job.colors.resize(job.per * 3);
        eval::PointQuery q;
        q.points_xyz = points.data();
        q.count = job.per;
        eval::PointResults res;
        res.distances = job.values.data();
        res.colors_rgb = job.want_colour ? job.colors.data() : nullptr;
        if (cpu->eval_points(prefix, q, res) != eval::Status::Ok) {
            job.values.clear();
            continue;
        }
        job.had_acc = false;
        for (std::size_t s = 0; s < job.per && !job.had_acc; ++s)
            job.had_acc = job.values[s] != CLAY_TAPE_FAR;
    }
    doc->frontier_store(prep);  // phase C, locked; abandons all if the document moved
}

}  // namespace

clay_result clay_layer_move_surface_preview(const clay_document* doc, clay_layer_id layer,
                                            const float centre[3],
                                            const float displacement[3],
                                            const clay_move_params* params,
                                            clay_node_id* out_nodes, size_t capacity,
                                            size_t* out_count) {
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    const scene::Layer* l = nullptr;
    std::vector<brush::PreparedMove> prepared;
    clay_result r = resolve_move(doc, layer, centre, displacement, params, &l, &prepared);
    if (r != CLAY_OK) return r;
    *out_count = prepared.size();
    if (!out_nodes) return CLAY_OK;  // size query
    for (std::size_t i = 0; i < prepared.size() && i < capacity; ++i)
        out_nodes[i] = prepared[i].node;
    return CLAY_OK;
}

// -- transient SDF sculpt transactions (sdf-sculpt-transaction spec) ----------
//
// See clay.h for the shape and why the two verbs need it. The handles below own
// a session transaction and BORROW the document, exactly as the header says.

struct clay_sdf_smooth_tx {
    clay_document* doc = nullptr;
    std::optional<session::SdfSmoothTransaction> tx;
};

struct clay_sdf_move_tx {
    clay_document* doc = nullptr;
    std::optional<session::SdfMoveTransaction> tx;
    // The drag as ordinary scene content (issue #388), built on first request.
    //
    // It holds the real document's LAYERS with the dragged one replaced by the
    // transaction's preview. A scene::Layer carries its edit list by
    // shared_ptr, so this copy is a vector of handles: every untouched layer
    // shares its content with the real document, and the dragged one shares the
    // transaction's own preview content — which is what makes an update visible
    // through this handle with nothing to keep in step.
    //
    // Only the layers. A voxel grid, a mask and a mesh layer are not part of
    // the field tape (compile_document takes LayerKind::Sdf and nothing else),
    // so copying them here would charge a drag for content it cannot change,
    // and by value at that.
    std::unique_ptr<clay_document> preview;
};

// Defined further down, beside the other relax entry points; declared here so
// the live Smooth reads its dab exactly as clay_item_volume_relax reads one.
static clay_result read_relax_settings(const clay_relax_params* params,
                                       field::RelaxSettings* out);

namespace {

constexpr std::size_t kSculptPolicyOriginal =
    offsetof(clay_sculpt_policy, allow_consolidation) + sizeof(std::int32_t);
constexpr std::size_t kSculptDirtyOriginal =
    offsetof(clay_sculpt_dirty, has_bounds) + sizeof(std::int32_t);
constexpr std::size_t kSculptBudgetOriginal =
    offsetof(clay_sculpt_budget, item_count) + sizeof(std::int32_t);

clay_result read_sculpt_policy(const clay_sculpt_policy* policy, session::SdfSculptPolicy* out) {
    // A null policy is a gesture with no budget and no sampling of its own,
    // which is meaningful for Move and refused for Smooth by the cell check.
    if (!policy) {
        *out = session::SdfSculptPolicy{};
        return CLAY_OK;
    }
    clay_sculpt_policy p;
    clay_result r = read_desc(policy, kSculptPolicyOriginal, &p);
    if (r != CLAY_OK) return r;
    session::SdfSculptPolicy sp;
    sp.cell_size = p.cell_size;
    sp.band = p.band;
    sp.padding = p.padding;
    sp.complexity.min_safe_step_scale = p.min_safe_step_scale;
    sp.complexity.max_deformer_chain = p.max_deformer_chain;
    sp.complexity.max_item_count = p.max_item_count;
    sp.complexity.allow_consolidation = p.allow_consolidation != 0;
    // The consolidation, if it is authorised, resamples at the gesture's own
    // resolution: the ABI has no nested descriptors, and inventing a second
    // cell size for a host to get out of step with would be worse than reusing
    // the one it already chose for this sculpt mode.
    *out = sp;
    return CLAY_OK;
}

// The out-descriptors, through write_desc so an older caller's buffer is not
// overrun -- the natural `*out = clay_thing{}` spelling does exactly that.
clay_result write_dirty(const session::SdfSculptDirty& dirty, clay_sculpt_dirty* out) {
    clay_sculpt_dirty probe;
    clay_result r = read_desc(out, kSculptDirtyOriginal, &probe);
    if (r != CLAY_OK) return r;
    clay_sculpt_dirty value{};
    value.struct_size = sizeof(clay_sculpt_dirty);
    value.touched_bricks = static_cast<std::uint64_t>(dirty.touched_bricks);
    value.changed = dirty.changed ? 1 : 0;
    value.has_bounds = dirty.bounds.empty() ? 0 : 1;
    if (value.has_bounds) {
        value.bounds_min[0] = dirty.bounds.min.x;
        value.bounds_min[1] = dirty.bounds.min.y;
        value.bounds_min[2] = dirty.bounds.min.z;
        value.bounds_max[0] = dirty.bounds.max.x;
        value.bounds_max[1] = dirty.bounds.max.y;
        value.bounds_max[2] = dirty.bounds.max.z;
    }
    write_desc(out, out->struct_size, value);
    return CLAY_OK;
}

clay_result write_budget(const session::SdfSculptBudget& budget, clay_sculpt_budget* out) {
    clay_sculpt_budget probe;
    clay_result r = read_desc(out, kSculptBudgetOriginal, &probe);
    if (r != CLAY_OK) return r;
    clay_sculpt_budget value{};
    value.struct_size = sizeof(clay_sculpt_budget);
    value.over_budget = budget.over_budget ? 1 : 0;
    value.consolidated = budget.consolidated ? 1 : 0;
    value.lipschitz = budget.report.lipschitz;
    value.safe_step_scale = budget.report.safe_step_scale;
    value.steepest_volume = budget.report.steepest_volume;
    value.longest_deformer_chain = budget.report.longest_deformer_chain;
    value.item_count = budget.report.item_count;
    write_desc(out, out->struct_size, value);
    return CLAY_OK;
}

// What a committed transaction owes the document handle: the history learns how
// many entries appeared on the wrapped stack, and every cache keyed on the
// revision is dropped. Consolidation has done exactly this since it landed --
// both perform commands through the stack directly rather than through the
// per-command funnel.
void settle_commit(clay_document* doc) {
    if (doc->undo) doc->undo->sync_scene_steps();
    doc->touch();
}

}  // namespace

clay_sdf_smooth_tx* clay_sdf_smooth_begin(clay_document* doc, clay_layer_id layer,
                                          const clay_sculpt_policy* policy,
                                          clay_cancel_token* token) {
    if (!doc || !policy) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or policy");
        return nullptr;
    }
    session::SdfSculptPolicy sp;
    if (read_sculpt_policy(policy, &sp) != CLAY_OK) return nullptr;
    if (!(sp.cell_size > 0.0f)) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "cell_size must be > 0");
        return nullptr;
    }
    std::optional<session::SdfSmoothTransaction> tx = session::SdfSmoothTransaction::begin(
        doc->doc.document, layer, sp, eval::pooled_bake_eval(),
        token ? &token->token : nullptr);
    if (!tx) {
        fail(CLAY_ERROR_INVALID_ARGUMENT,
             "cannot smooth this layer: it is missing, not an SDF layer, protected, or its "
             "field is empty or unbounded");
        return nullptr;
    }
    auto* handle = new clay_sdf_smooth_tx();
    handle->doc = doc;
    handle->tx = std::move(tx);
    return handle;
}

clay_result clay_sdf_smooth_update(clay_sdf_smooth_tx* tx, const clay_relax_params* params,
                                   clay_cancel_token* token, clay_sculpt_dirty* out_dirty) {
    if (!tx || !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or params");
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    field::RelaxSettings settings;
    clay_result r = read_relax_settings(params, &settings);
    if (r != CLAY_OK) return r;
    const session::SdfSculptDirty dirty =
        tx->tx->update(settings, token ? &token->token : nullptr);
    if (out_dirty) return write_dirty(dirty, out_dirty);
    return CLAY_OK;
}

clay_result clay_sdf_smooth_preview_item(const clay_sdf_smooth_tx* tx, clay_item** out_item) {
    if (!tx || !out_item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or out");
    *out_item = nullptr;
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(tx->tx->preview_volume());
    *out_item = item;
    return CLAY_OK;
}

clay_result clay_sdf_smooth_commit(clay_sdf_smooth_tx* tx, clay_sculpt_budget* out_budget) {
    if (!tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction");
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    scene::UndoStack* stack = tx->doc->undo ? tx->doc->undo->commands() : nullptr;
    if (!tx->tx->commit(stack))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the layer changed since this stroke began, so the preview was discarded "
                    "rather than written over the newer edit");
    settle_commit(tx->doc);
    if (out_budget) return write_budget(tx->tx->budget(), out_budget);
    return CLAY_OK;
}

namespace {
constexpr std::size_t kPreviewDeltaInfoOriginal =
    offsetof(clay_sdf_preview_delta_info, bounds_max) + sizeof(float) * 3;
}  // namespace

clay_result clay_sdf_smooth_preview_delta_info(const clay_sdf_smooth_tx* tx,
                                               clay_sdf_preview_delta_info* out_info) {
    if (!tx || !out_info) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or out");
    // LIVE, not merely present: after a commit or a cancel the working field is
    // released, so a delta read would describe something the host can no longer
    // draw. Refused on the same terms as every other call here.
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    clay_sdf_preview_delta_info probe;
    clay_result r = read_desc(out_info, kPreviewDeltaInfoOriginal, &probe);
    if (r != CLAY_OK) return r;

    const std::vector<field::FieldVolume::BrickCoord>& bricks = tx->tx->preview_delta();
    const field::FieldVolume& volume = tx->tx->preview_volume();
    clay_sdf_preview_delta_info value{};
    value.struct_size = sizeof(clay_sdf_preview_delta_info);
    value.generation = tx->tx->preview_generation();
    value.brick_count = static_cast<std::uint64_t>(bricks.size());
    value.sample_floats =
        static_cast<std::uint64_t>(bricks.size()) * static_cast<std::uint64_t>(field::kBrickSamples);
    math::Aabb bounds;
    const float span = static_cast<float>(field::kBrickDim) * volume.cell_size();
    for (const field::FieldVolume::BrickCoord& c : bricks) {
        const kernel::cfloat3 lo = volume.brick_origin(c);
        bounds.expand(math::Aabb{lo, lo + kernel::cf3(span, span, span)});
    }
    value.has_bounds = bounds.empty() ? 0 : 1;
    if (value.has_bounds) {
        for (int a = 0; a < 3; ++a) {
            value.bounds_min[a] = (&bounds.min.x)[a];
            value.bounds_max[a] = (&bounds.max.x)[a];
        }
    }
    write_desc(out_info, out_info->struct_size, value);
    return CLAY_OK;
}

clay_result clay_sdf_smooth_preview_delta_take(clay_sdf_smooth_tx* tx,
                                               clay_sdf_preview_brick* bricks,
                                               uint64_t brick_capacity, float* samples,
                                               uint64_t sample_capacity, uint64_t* out_bricks,
                                               uint64_t* out_samples) {
    if (!tx || !out_bricks || !out_samples)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or out counts");
    // LIVE, not merely present: after a commit or a cancel the working field is
    // released, so a delta read would describe something the host can no longer
    // draw. Refused on the same terms as every other call here.
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");

    const std::vector<field::FieldVolume::BrickCoord>& waiting = tx->tx->preview_delta();
    const std::uint64_t need_bricks = static_cast<std::uint64_t>(waiting.size());
    const std::uint64_t need_samples =
        need_bricks * static_cast<std::uint64_t>(field::kBrickSamples);
    *out_bricks = need_bricks;
    *out_samples = need_samples;
    if (need_bricks == 0) return CLAY_OK;
    if (!bricks || !samples)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick or sample buffer");
    // NOTHING is taken when either buffer is short. A partial drain would
    // strand bricks that no later call reports, because taking is what clears
    // them -- so the caller grows its buffers and asks again, and the counts
    // above say how far.
    if (brick_capacity < need_bricks || sample_capacity < need_samples)
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "delta needs " + std::to_string(need_bricks) + " bricks and " +
                        std::to_string(need_samples) + " sample floats");

    const field::FieldVolume& volume = tx->tx->preview_volume();
    std::uint64_t written = 0;
    for (std::uint64_t i = 0; i < need_bricks; ++i) {
        const field::FieldVolume::BrickCoord c = waiting[static_cast<std::size_t>(i)];
        float* into = samples + written;
        // A brick the working field no longer stores is not reported: the delta
        // records what changed, and a consumer cannot patch with samples that
        // are not there. It cannot happen through the transaction's own paths,
        // which only ever add storage, and skipping is the honest answer if it
        // ever does.
        if (!volume.read_brick(c, into)) continue;
        clay_sdf_preview_brick& out = bricks[i];
        out.key[0] = c.x;
        out.key[1] = c.y;
        out.key[2] = c.z;
        const kernel::cfloat3 lo = volume.brick_origin(c);
        out.origin[0] = lo.x;
        out.origin[1] = lo.y;
        out.origin[2] = lo.z;
        out.spacing = volume.cell_size();
        out.sample_dim = static_cast<std::uint32_t>(field::kBrickDim + 1);
        out.sample_offset = written;
        written += static_cast<std::uint64_t>(field::kBrickSamples);
    }
    *out_bricks = written / static_cast<std::uint64_t>(field::kBrickSamples);
    *out_samples = written;
    tx->tx->take_preview_delta(nullptr);
    return CLAY_OK;
}

void clay_sdf_smooth_cancel(clay_sdf_smooth_tx* tx) {
    if (tx && tx->tx) tx->tx->cancel();
}

void clay_sdf_smooth_destroy(clay_sdf_smooth_tx* tx) {
    // Destroying without committing IS a cancel, which is what makes an error
    // path that simply drops the handle safe. Nothing persistent was written,
    // so there is nothing to unwind.
    delete tx;
}

clay_sdf_move_tx* clay_sdf_move_begin(clay_document* doc, clay_layer_id layer,
                                      const float centre[3], const clay_move_params* params,
                                      const clay_sculpt_policy* policy) {
    if (!doc || !centre || !params) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "null document, centre or move parameters");
        return nullptr;
    }
    clay_move_params p;
    if (read_desc(params, kMoveParamsOriginal, &p) != CLAY_OK) return nullptr;
    if (!(p.radius > 0.0f)) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "radius must be > 0");
        return nullptr;
    }
    if (check_ease(p.ease) != CLAY_OK) return nullptr;
    session::SdfSculptPolicy sp;
    if (read_sculpt_policy(policy, &sp) != CLAY_OK) return nullptr;

    brush::MoveSettings settings;
    settings.radius = p.radius;
    settings.ease = static_cast<std::uint8_t>(p.ease);
    settings.front_only = p.front_only != 0;

    std::optional<session::SdfMoveTransaction> tx = session::SdfMoveTransaction::begin(
        doc->doc.document, layer, kernel::cf3(centre[0], centre[1], centre[2]), settings, sp,
        eval::pooled_bake_eval());
    if (!tx) {
        fail(CLAY_ERROR_INVALID_ARGUMENT,
             "cannot drag this layer: it is missing, not an SDF layer, or protected");
        return nullptr;
    }
    auto* handle = new clay_sdf_move_tx();
    handle->doc = doc;
    handle->tx = std::move(tx);
    return handle;
}

clay_result clay_sdf_move_update(clay_sdf_move_tx* tx, const float total_displacement[3],
                                 clay_sculpt_dirty* out_dirty) {
    if (!tx || !total_displacement)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or displacement");
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    const session::SdfSculptDirty dirty =
        tx->tx->update(kernel::cf3(total_displacement[0], total_displacement[1],
                                   total_displacement[2]));
    if (out_dirty) return write_dirty(dirty, out_dirty);
    return CLAY_OK;
}

clay_result clay_sdf_move_preview_nodes(const clay_sdf_move_tx* tx, clay_node_id* out_nodes,
                                        size_t capacity, size_t* out_count) {
    if (!tx || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or count");
    if (!tx->tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    const std::vector<scene::NodeId>& ids = tx->tx->affected_nodes();
    *out_count = ids.size();
    if (!out_nodes) return CLAY_OK;  // the size query
    if (capacity < ids.size())
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "buffer holds " + std::to_string(capacity) + " of " +
                        std::to_string(ids.size()) + " affected nodes");
    for (std::size_t i = 0; i < ids.size(); ++i)
        out_nodes[i] = static_cast<clay_node_id>(ids[i]);
    return CLAY_OK;
}

clay_result clay_sdf_move_preview_grab_count(const clay_sdf_move_tx* tx, clay_node_id node,
                                             size_t* out_count) {
    if (!tx || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction or count");
    if (!tx->tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    std::vector<scene::Deformer> grabs;
    if (!tx->tx->preview_grabs(static_cast<scene::NodeId>(node), &grabs))
        return fail(CLAY_ERROR_NOT_FOUND,
                    "node " + std::to_string(node) + " is not one this drag reaches");
    *out_count = grabs.size();
    return CLAY_OK;
}

clay_result clay_sdf_move_preview_grab(const clay_sdf_move_tx* tx, clay_node_id node,
                                       size_t index, float out_centre[3], float* out_radius,
                                       float out_displacement[3], int32_t* out_ease,
                                       int32_t* out_front_only) {
    if (!tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction");
    if (!tx->tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    std::vector<scene::Deformer> grabs;
    if (!tx->tx->preview_grabs(static_cast<scene::NodeId>(node), &grabs))
        return fail(CLAY_ERROR_NOT_FOUND,
                    "node " + std::to_string(node) + " is not one this drag reaches");
    if (index >= grabs.size())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "grab " + std::to_string(index) + " of " + std::to_string(grabs.size()) +
                        " on node " + std::to_string(node));
    const scene::Deformer& grab = grabs[index];
    if (out_centre) {
        out_centre[0] = grab.k;
        out_centre[1] = grab.a;
        out_centre[2] = grab.b;
    }
    if (out_radius) *out_radius = grab.c;
    if (out_displacement) {
        out_displacement[0] = grab.ext[0];
        out_displacement[1] = grab.ext[1];
        out_displacement[2] = grab.ext[2];
    }
    if (out_ease) *out_ease = static_cast<std::int32_t>(grab.ease);
    if (out_front_only) *out_front_only = grab.ext[3] != 0.0f ? 1 : 0;
    return CLAY_OK;
}

const clay_document* clay_sdf_move_preview_document(const clay_sdf_move_tx* tx) {
    if (!tx || !tx->tx || !tx->tx->live()) return nullptr;
    // Const in the API and mutable here: the handle is BUILT on first request
    // rather than at begin, so a host that never previews pays nothing for the
    // ability to. What the caller gets back is read-only either way.
    auto* self = const_cast<clay_sdf_move_tx*>(tx);
    if (!self->preview) {
        self->preview = std::make_unique<clay_document>();
        self->preview->doc.document = tx->doc->doc.document;
    }
    // Re-point the dragged layer every call rather than once: `preview_layer()`
    // is a reference into the transaction, and the layer STRUCT (its transform,
    // its visibility) can differ from the one copied above. The edit list
    // itself is shared, so this is a handful of words, not a walk.
    const scene::Layer& live = tx->tx->preview_layer();
    for (scene::Layer& l : self->preview->doc.document.layers) {
        if (l.id == tx->tx->layer()) {
            l = live;
            break;
        }
    }
    // ...and TOUCH it, which is the part that is easy to miss and silent when
    // missed. A document caches its compiled tape by a revision the mutating
    // entry points bump, and none of them ran here: the drag changed the
    // shared edit list behind the cache's back, so without this the second
    // frame of a gesture would be served the first frame's tape and the
    // preview would freeze after one update.
    self->preview->touch();
    return self->preview.get();
}

clay_result clay_sdf_move_commit(clay_sdf_move_tx* tx, clay_sculpt_budget* out_budget) {
    if (!tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transaction");
    if (!tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this transaction is spent");
    scene::UndoStack* stack = tx->doc->undo ? tx->doc->undo->commands() : nullptr;
    if (!tx->tx->commit(stack))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the layer changed since this drag began, so the preview was discarded "
                    "rather than written over the newer edit");
    settle_commit(tx->doc);
    // Same as cancel: the preview borrowed the transaction's content, and the
    // gesture is over. Held past here it would describe a drag the document
    // now carries for real.
    tx->preview.reset();
    if (out_budget) return write_budget(tx->tx->budget(), out_budget);
    return CLAY_OK;
}

void clay_sdf_move_cancel(clay_sdf_move_tx* tx) {
    if (tx && tx->tx) tx->tx->cancel();
    // The preview borrows the transaction's content, so it dies with the
    // gesture rather than outliving it holding stale handles. The header says
    // it is valid until commit, cancel or destroy; this is that.
    if (tx) tx->preview.reset();
}

void clay_sdf_move_destroy(clay_sdf_move_tx* tx) { delete tx; }

namespace {

// The half of a SURFACE GESTURE that is the same whether it drags, swells or
// gathers: what it invalidates beyond its own reach, what it can state about
// the frontier, and the one undo group and one invalidation that make it a
// gesture rather than a run of edits.
//
// `reach` arrives holding the gesture's own boxes -- one per image the layer's
// symmetry makes of it -- because only the caller knows how far its own
// deformation carries. A drag dilates by its displacement; a magnify does not,
// its weight being zero outside the radius for either sign of the strength.
// Everything after that is common, and it is a hundred lines of it, so a second
// gesture that copied it would be a second place to fix #360 and #363.
// ONE WARP BUFFER FOR THE WHOLE GESTURE, resolved per item as the item is
// applied, rather than a vector of warps resolved up front.
//
// A MoveWarp owns two `std::vector<scene::Deformer>`, so materialising one per
// reached item costs an allocation per item that is freed moments later — 138
// of them on a thousand-item drag, and #372 measured itself at 1.09x on this
// exact path when it introduced them (#375). Nothing needed them all at once:
// `drag_frontier` reads only node ids, which the PREPARED items carry, and a
// PreparedMove is a snapshot taken before any apply, so resolving late reads
// nothing an earlier apply has moved. `resolve_prepared_move` clears the
// vectors rather than reassigning them, so the buffer keeps its capacity and
// the second item onward allocates nothing.
//
// Each node is applied exactly once, so `moved_chain` still reads that node's
// pre-drag chain.
// Which gesture is being resolved, and the one number it carries. A plain
// struct rather than a template or a std::function: this file is compiled
// inside `extern "C"`, where a template cannot be declared, and the set of
// surface gestures is closed at two.
struct GestureResolver {
    enum class Kind { Move, Magnify };
    Kind kind = Kind::Move;
    kernel::cfloat3 displacement{};  // Move: the world pull
    float strength = 0.0f;           // Magnify: the dimensionless strength

    void operator()(const brush::PreparedMove& p, brush::MoveWarp* out) const {
        if (kind == Kind::Move)
            brush::resolve_prepared_move(p, displacement, out);
        else
            brush::resolve_prepared_magnify(p, strength, out);
    }
};

clay_result apply_surface_gesture(clay_document* doc, clay_layer_id layer,
                                  const scene::Layer& l,
                                  const std::vector<brush::PreparedMove>& prepared,
                                  const GestureResolver& resolve_into,
                                  std::vector<math::Aabb> reach, size_t* out_applied) {
    const scene::Layer* lp = &l;
    // ... IN ONE PLACEMENT. The ball above is stated in the dragged layer's
    // frame, and an instanced edit list is placed by every layer that shares
    // it: the same nodes move under every one of those transforms, so the
    // field changes in regions the ball does not contain and nothing else
    // here would dirty them. Left out, the seeds there were advanced to the
    // new revision while still clean and handed back as the whole answer --
    // measured 0.4 world units stale, the whole displacement, in a second
    // placement four units away.
    //
    // Widened by each sharer's WHOLE influence bound rather than by the ball
    // mapped through its transform: mirror and radial place one ball in
    // several spots and layer_influence_bound already accounts for all of
    // them. Conservative, and only a shared edit list pays it -- the common
    // layer shares with nobody and the loop finds nothing. This is the same
    // union node_command_bound takes for the per-command path, which is why
    // every other edit route was already right.
    for (const scene::Layer& other : doc->doc.document.layers) {
        if (&other == lp || other.sdf != lp->sdf) continue;
        reach.push_back(scene::layer_influence_bound(other));
    }

    // What the drag can state about HISTORY, beside what the ball states about
    // space (#360): every command it issues is a SetDeformersCmd -- a parameter
    // edit -- so when the gates hold and every warp node resolves to a root
    // ordinal, the gesture may state the earliest of those ordinals as its
    // frontier and the seeds it dirties keep their prefix half. Stated here
    // rather than derived per command because the gesture-grained invalidation
    // below sees no commands; left unstated, the legacy drop would destroy the
    // prefix seeds every frame and the frontier path would never resume.
    DragFrontier frontier;
    drag_frontier(doc, *lp, prepared, &frontier);
    const bool states_frontier = frontier.usable && frontier.all_resolved;

    // The pre-drag seeds (#360), recorded before the applies dirty anything:
    // once a seed is dirty its stored value no longer describes the document,
    // and a prefix can only be sliced out of a value that does. Skipped when
    // the gesture cannot state its frontier -- the drop below would only
    // destroy what this pass recorded.
    if (states_frontier) prepare_frontier_seeds(doc, frontier);

    // One group for the whole drag: it is one gesture, and undoing it item by
    // item would be the implementation showing through. One invalidation too,
    // for the same reason and at the same grain (#358).
    GestureRegion region{doc, std::move(reach),
                         states_frontier ? frontier.min_ordinal
                                         : clay_document::kFrontierDrop};
    if (doc->undo) doc->undo->begin_group();
    std::size_t applied = 0;
    brush::MoveWarp warp;  // reused across items; see the note above
    for (const brush::PreparedMove& p : prepared) {
        const scene::Node* n = lp->sdf->find(p.node);
        if (!n) continue;
        resolve_into(p, &warp);
        scene::SetDeformersCmd cmd{layer, warp.node, brush::moved_chain(*n, warp)};
        const clay_result r = apply_edit_in_gesture(doc, scene::Command{cmd}, "node not found");
        if (r != CLAY_OK) {
            if (doc->undo) doc->undo->end_group();
            return r;
        }
        ++applied;
    }
    if (doc->undo) doc->undo->end_group();
    if (out_applied) *out_applied = applied;
    return CLAY_OK;
}

}  // namespace

clay_result clay_layer_move_surface(clay_document* doc, clay_layer_id layer,
                                    const float centre[3], const float displacement[3],
                                    const clay_move_params* params, size_t* out_applied) {
    const scene::Layer* l = nullptr;
    std::vector<brush::PreparedMove> prepared;
    float radius = 0.0f;
    clay_result r = resolve_move(doc, layer, centre, displacement, params, &l, &prepared, &radius);
    if (r != CLAY_OK) return r;

    // WHAT A DRAG CAN REACH, in one box, without asking the geometry.
    //
    // The warp's region weight is zero outside `radius` of the drag centre, and
    // a point whose weight is zero is not moved -- so a sample there evaluates
    // the same nodes at the same place and the field cannot have changed. The
    // reachable region is therefore the drag's own ball, whatever the drag
    // touched, dilated by the displacement as margin. `front_only` only gates
    // the pull further, and a uniform item scale keeps the falloff spherical in
    // the item's frame (see brush/move.h), so neither widens this.
    //
    // Cheaper AND TIGHTER than what apply_edit would derive. Per command it
    // unions the whole influence bound of each moved node, so a drag that
    // catches the edge of 257 items spread through the volume invalidates the
    // union of 257 whole items -- far more than the ball the drag actually
    // reached.
    //
    // ... ONCE PER IMAGE THE LAYER'S SYMMETRY MAKES OF IT (#363). The copies a
    // mirror or radial layer emits move where the REFLECTED ball is, and the
    // drag's warps are aimed there too (brush::drag_images), so the reach is
    // the union of every image's ball. Left at the one ball, a seed warm on
    // the reflected side was advanced to the new revision still describing
    // the undragged copies and handed back whole: measured 997 of 8,192
    // samples stale, up to 0.023 -- the whole pull -- on the ridge fixture.
    // The prepare pass records prefixes from the mirror-expanded spans, so a
    // prepared seed on that side is exactly one this would otherwise serve
    // stale. One BOX PER IMAGE and still one invalidation per gesture: the
    // union of two balls a diameter apart is the slab between them, and
    // under a mirror that slab is the whole document -- stated as the union,
    // every warm brick resumed every frame and the warm mirrored refill
    // measured 0.35x the cold one against 0.16x unmirrored.
    const float pull = std::sqrt(displacement[0] * displacement[0] +
                                 displacement[1] * displacement[1] +
                                 displacement[2] * displacement[2]);
    std::vector<math::Aabb> reach;
    for (const brush::DragImage& image :
         brush::drag_images(*l, kernel::cf3(centre[0], centre[1], centre[2]),
                            kernel::cf3(displacement[0], displacement[1], displacement[2])))
        reach.push_back(math::Aabb{image.centre, image.centre}.dilated(radius + pull));

    const kernel::cfloat3 world_pull =
        kernel::cf3(displacement[0], displacement[1], displacement[2]);
    GestureResolver resolver;
    resolver.kind = GestureResolver::Kind::Move;
    resolver.displacement = world_pull;
    return apply_surface_gesture(doc, layer, *l, prepared, resolver, std::move(reach),
                                 out_applied);
}

namespace {

clay_result resolve_magnify(const clay_document* doc, clay_layer_id layer, const float centre[3],
                            float strength, const clay_magnify_params* params,
                            const scene::Layer** out_layer,
                            std::vector<brush::PreparedMove>* out_prepared,
                            float* out_radius = nullptr) {
    if (!doc || !centre) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or centre");
    if (!params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null magnify parameters");
    clay_magnify_params p;
    clay_result r = read_desc(params, kMagnifyParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (!(p.radius > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radius must be > 0");
    // Refused rather than accepted as a no-op, for the reason a drag of zero
    // is: a strength of zero scales by one, so it is not a gesture, and a host
    // that reached here by accident should hear about it at the boundary.
    if (!(strength != 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "strength must be non-zero; positive swells, negative gathers");
    if ((r = check_ease(p.ease)) != CLAY_OK) return r;

    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "no layer with id " + std::to_string(layer));
    if (l->kind != scene::LayerKind::Sdf || !l->sdf)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this layer holds no SDF content to magnify");

    brush::MagnifySettings settings;
    settings.radius = p.radius;
    settings.ease = static_cast<std::uint8_t>(p.ease);

    *out_layer = l;
    if (out_radius) *out_radius = p.radius;
    *out_prepared =
        brush::prepare_magnify(*l, kernel::cf3(centre[0], centre[1], centre[2]), settings);
    return CLAY_OK;
}

}  // namespace

clay_result clay_layer_magnify_surface_preview(const clay_document* doc, clay_layer_id layer,
                                               const float centre[3], float strength,
                                               const clay_magnify_params* params,
                                               clay_node_id* out_nodes, size_t capacity,
                                               size_t* out_count) {
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    const scene::Layer* l = nullptr;
    std::vector<brush::PreparedMove> prepared;
    clay_result r = resolve_magnify(doc, layer, centre, strength, params, &l, &prepared);
    if (r != CLAY_OK) return r;
    *out_count = prepared.size();
    if (!out_nodes) return CLAY_OK;  // size query
    for (std::size_t i = 0; i < prepared.size() && i < capacity; ++i)
        out_nodes[i] = prepared[i].node;
    return CLAY_OK;
}

clay_result clay_layer_magnify_surface(clay_document* doc, clay_layer_id layer,
                                       const float centre[3], float strength,
                                       const clay_magnify_params* params, size_t* out_applied) {
    const scene::Layer* l = nullptr;
    std::vector<brush::PreparedMove> prepared;
    float radius = 0.0f;
    clay_result r = resolve_magnify(doc, layer, centre, strength, params, &l, &prepared, &radius);
    if (r != CLAY_OK) return r;

    // WHAT THIS REACHES, and it is the ball with NO dilation — where a drag has
    // to add its displacement as margin.
    //
    // Outside `radius` the region weight is zero and cmagnify_point returns the
    // point unchanged, so a sample there evaluates the same nodes at the same
    // place and the field cannot have moved. A PINCH samples from outside the
    // ball, its scale factor exceeding one, but only ever at points inside it,
    // and it is where the deformation is EVALUATED that bounds what changed.
    //
    // ... once per image the layer's symmetry makes of it (#363), for the
    // reason clay_layer_move_surface states at length: the copies a mirror or
    // radial layer emits gather where the reflected ball is, this gesture's
    // warps are aimed there too, and a host invalidating one ball serves the
    // reflected side stale. The displacement handed to drag_images is zero
    // because this gesture has none — only the image CENTRES are read here.
    std::vector<math::Aabb> reach;
    for (const brush::DragImage& image :
         brush::drag_images(*l, kernel::cf3(centre[0], centre[1], centre[2]),
                            kernel::cf3(0.0f, 0.0f, 0.0f)))
        reach.push_back(math::Aabb{image.centre, image.centre}.dilated(radius));

    GestureResolver resolver;
    resolver.kind = GestureResolver::Kind::Magnify;
    resolver.strength = strength;
    return apply_surface_gesture(doc, layer, *l, prepared, resolver, std::move(reach),
                                 out_applied);
}

namespace {

clay_result resolve_gizmo(const clay_document* doc, clay_layer_id layer,
                          const clay_gizmo_cage* desc, const float* offsets_xyz,
                          const scene::Layer** out_layer,
                          std::vector<brush::LatticeWarp>* out_warps) {
    if (!doc || !desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or cage");
    clay_gizmo_cage d;
    clay_result r = read_desc(desc, kGizmoCageOriginal, &d);
    if (r != CLAY_OK) return r;
    desc = &d;
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "no layer with id " + std::to_string(layer));
    if (l->kind != scene::LayerKind::Sdf || !l->sdf)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this layer holds no SDF content to cage");

    const int cap = scene::Deformer::kMaxLatticeDivisions;
    if (desc->nx < 2 || desc->ny < 2 || desc->nz < 2 || desc->nx > cap || desc->ny > cap ||
        desc->nz > cap)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "lattice divisions must be in [2, " + std::to_string(cap) +
                        "] per axis; the cage is evaluated per sample");
    if (!(desc->scale > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the cage's scale must be > 0");

    brush::GizmoCage cage;
    cage.placement.position = kernel::cf3(desc->position[0], desc->position[1], desc->position[2]);
    const kernel::cfloat3 axis = kernel::cf3(desc->axis[0], desc->axis[1], desc->axis[2]);
    if (kernel::clength(axis) > 1e-9f)
        cage.placement.rotation = math::Quat::from_axis_angle(axis, desc->angle);
    cage.placement.scale = desc->scale;
    cage.box_min = kernel::cf3(desc->box_min[0], desc->box_min[1], desc->box_min[2]);
    cage.box_max = kernel::cf3(desc->box_max[0], desc->box_max[1], desc->box_max[2]);
    if (!(cage.box_max.x > cage.box_min.x || cage.box_max.y > cage.box_min.y ||
          cage.box_max.z > cage.box_min.z))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the cage's box is empty");
    cage.nx = desc->nx;
    cage.ny = desc->ny;
    cage.nz = desc->nz;
    cage.offsets.assign(cage.point_count(), kernel::cf3(0, 0, 0));
    if (offsets_xyz)
        for (std::size_t i = 0; i < cage.offsets.size(); ++i)
            cage.offsets[i] = kernel::cf3(offsets_xyz[i * 3], offsets_xyz[i * 3 + 1],
                                          offsets_xyz[i * 3 + 2]);

    *out_layer = l;
    *out_warps = brush::lattice_gizmo(*l, cage);
    return CLAY_OK;
}

}  // namespace

clay_result clay_layer_lattice_gizmo_preview(const clay_document* doc, clay_layer_id layer,
                                             const clay_gizmo_cage* cage,
                                             const float* offsets_xyz, clay_node_id* out_nodes,
                                             size_t capacity, size_t* out_count) {
    const scene::Layer* l = nullptr;
    std::vector<brush::LatticeWarp> warps;
    clay_result r = resolve_gizmo(doc, layer, cage, offsets_xyz, &l, &warps);
    if (r != CLAY_OK) return r;
    if (out_count) *out_count = warps.size();
    if (!out_nodes) return CLAY_OK;
    if (capacity < warps.size())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "buffer too small for the warped nodes");
    for (std::size_t i = 0; i < warps.size(); ++i) out_nodes[i] = warps[i].node;
    return CLAY_OK;
}

clay_result clay_layer_lattice_gizmo(clay_document* doc, clay_layer_id layer,
                                     const clay_gizmo_cage* cage, const float* offsets_xyz,
                                     size_t* out_applied) {
    const scene::Layer* l = nullptr;
    std::vector<brush::LatticeWarp> warps;
    clay_result r = resolve_gizmo(doc, layer, cage, offsets_xyz, &l, &warps);
    if (r != CLAY_OK) return r;

    // One group for the whole cage: it is one gesture.
    if (doc->undo) doc->undo->begin_group();
    std::size_t applied = 0;
    for (const brush::LatticeWarp& w : warps) {
        const scene::Node* n = l->sdf->find(w.node);
        if (!n) continue;
        scene::SetDeformersCmd cmd{layer, w.node, brush::caged_chain(*n, w)};
        r = apply_edit(doc, scene::Command{cmd}, "node not found");
        if (r != CLAY_OK) {
            if (doc->undo) doc->undo->end_group();
            return r;
        }
        ++applied;
    }
    if (doc->undo) doc->undo->end_group();
    if (out_applied) *out_applied = applied;
    return CLAY_OK;
}

clay_result clay_layer_add_deformer(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t deform, const float* params, size_t param_count,
                                    int32_t ease, int32_t at_front) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    // Bounded by the TABLE, like clay_item_add_deformer. This is the SECOND
    // door into a deformer chain and it was left naming an enumerator when the
    // first one was fixed — so every kind added since noise was declared,
    // documented, handled by make_deformer, reachable on a builder, and
    // refused on a PLACED node. The test that walks the enum now walks both.
    constexpr int kKinds = sizeof kDeformParams / sizeof kDeformParams[0];
    if (deform < 0 || deform >= kKinds)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown deformer kind");
    if (kDeformParams[deform] < 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this deformer carries a payload that is not a parameter list; "
                    "use its own entry point (bend_curve: clay_layer_add_bend_curve, "
                    "lattice: clay_layer_add_lattice)");
    clay_result r = check_params("deformer", params, param_count, kDeformParams[deform]);
    if (r != CLAY_OK) return r;
    if ((r = check_ease(ease)) != CLAY_OK) return r;
    scene::Deformer d;
    if ((r = make_deformer(deform, params, &d)) != CLAY_OK) return r;
    d.ease = static_cast<std::uint8_t>(ease);

    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || !l->sdf) return fail(CLAY_ERROR_NOT_FOUND, "no SDF layer with that id");
    const scene::Node* n = l->sdf->find(node);
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in this layer");

    std::vector<scene::Deformer> chain = n->deformers;
    if (at_front != 0) {
        chain.insert(chain.begin(), d);
    } else {
        chain.push_back(d);
    }
    return apply_edit(doc, scene::Command{scene::SetDeformersCmd{layer, node, std::move(chain)}},
                      "node not found");
}

namespace {

// The three inverses share one lookup and one command; only what they do to the
// chain differs (issue #388).
clay_result edit_placed_chain(clay_document* doc, clay_layer_id layer, clay_node_id node,
                              const std::function<clay_result(std::vector<scene::Deformer>*)>& fn) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || !l->sdf) return fail(CLAY_ERROR_NOT_FOUND, "no SDF layer with that id");
    const scene::Node* n = l->sdf->find(node);
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in this layer");
    std::vector<scene::Deformer> chain = n->deformers;
    const clay_result r = fn(&chain);
    if (r != CLAY_OK) return r;
    return apply_edit(doc, scene::Command{scene::SetDeformersCmd{layer, node, std::move(chain)}},
                      "node not found");
}

}  // namespace

clay_result clay_layer_deformer_count(const clay_document* doc, clay_layer_id layer,
                                      clay_node_id node, size_t* out_count) {
    if (!doc || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or count");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || !l->sdf) return fail(CLAY_ERROR_NOT_FOUND, "no SDF layer with that id");
    const scene::Node* n = l->sdf->find(node);
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in this layer");
    *out_count = n->deformers.size();
    return CLAY_OK;
}

clay_result clay_layer_remove_deformer(clay_document* doc, clay_layer_id layer,
                                       clay_node_id node, size_t index) {
    return edit_placed_chain(doc, layer, node, [index](std::vector<scene::Deformer>* chain) {
        // Refused rather than ignored: a host that miscounted has a bug, and a
        // silent success would let it ship.
        if (index >= chain->size())
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "no deformer at index " + std::to_string(index) + "; the chain has " +
                            std::to_string(chain->size()));
        chain->erase(chain->begin() + static_cast<std::ptrdiff_t>(index));
        return CLAY_OK;
    });
}

clay_result clay_layer_clear_deformers(clay_document* doc, clay_layer_id layer,
                                       clay_node_id node) {
    // An empty chain cleared is a success that changes nothing — there is
    // nothing wrong with clearing what is already clear.
    return edit_placed_chain(doc, layer, node, [](std::vector<scene::Deformer>* chain) {
        chain->clear();
        return CLAY_OK;
    });
}

namespace {

// The shared half of "add this deformer to a node already in a document": find
// the node, place the warp per `at_front`, and commit it as one edit. The two
// payload-carrying kinds differ only in how their deformer is built.
clay_result add_placed_deformer(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                scene::Deformer d, std::int32_t at_front) {
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || !l->sdf) return fail(CLAY_ERROR_NOT_FOUND, "no SDF layer with that id");
    const scene::Node* n = l->sdf->find(node);
    if (!n) return fail(CLAY_ERROR_NOT_FOUND, "no node with that id in this layer");
    std::vector<scene::Deformer> chain = n->deformers;
    if (at_front != 0)
        chain.insert(chain.begin(), std::move(d));
    else
        chain.push_back(std::move(d));
    return apply_edit(doc, scene::Command{scene::SetDeformersCmd{layer, node, std::move(chain)}},
                      "node not found");
}

}  // namespace

clay_result clay_layer_add_bend_curve(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                      const float* guide_xyz, size_t point_count,
                                      int32_t point_type, float t0, float t1, int32_t at_front) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    // Built through the builder entry point so the two cannot disagree about
    // what is legal — one place for the refusals, one for the construction.
    clay_item scratch;
    clay_result r = clay_item_add_bend_curve(&scratch, guide_xyz, point_count, point_type, t0, t1);
    if (r != CLAY_OK) return r;
    return add_placed_deformer(doc, layer, node, std::move(scratch.node.deformers.front()),
                               at_front);
}

clay_result clay_layer_add_lattice(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   const float min[3], const float max[3], int32_t nx, int32_t ny,
                                   int32_t nz, const float* offsets_xyz, int32_t at_front) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    clay_item scratch;
    clay_result r = clay_item_add_lattice(&scratch, min, max, nx, ny, nz, offsets_xyz);
    if (r != CLAY_OK) return r;
    return add_placed_deformer(doc, layer, node, std::move(scratch.node.deformers.front()),
                               at_front);
}

clay_result clay_layer_apply_stroke(clay_document* doc, clay_layer_id layer_id,
                                    const float* samples_xyzpt, size_t sample_count,
                                    const clay_stroke_preset* preset, const clay_item* item,
                                    const clay_mask* mask, clay_node_id* out_nodes,
                                    size_t* count) {
    if (!doc || !item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or item");
    clay_result r = validate_item(*item);
    if (r != CLAY_OK) return r;

    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset p;
    r = read_stroke(samples_xyzpt, sample_count, preset, &samples, &p);
    if (r != CLAY_OK) return r;

    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");

    voxel::MaskField* m = nullptr;
    if (mask) {
        r = resolve_mask(mask, &m);
        if (r != CLAY_OK) return r;
    }

    std::vector<scene::Node> nodes = brush::stamps_to_nodes(
        *layer->sdf, brush::resolve_stroke(samples, p), item->node, m);

    // One AddNodeCmd per stamp inside a single undo group, so a whole stroke
    // is one step to undo. Grouping is the stack's, not this module's.
    if (doc->undo) doc->undo->begin_group();
    std::size_t capacity = count ? *count : 0;
    std::size_t written = 0;
    for (scene::Node& node : nodes) {
        clay_node_id id = node.id;
        std::vector<scene::Node> subtree;
        subtree.push_back(std::move(node));
        r = apply_edit(
            doc,
            scene::Command{scene::AddNodeCmd{layer_id, scene::kNoNode, -1, std::move(subtree)}},
            "layer not found");
        if (r != CLAY_OK) break;
        if (out_nodes && written < capacity) out_nodes[written] = id;
        ++written;
    }
    if (doc->undo) doc->undo->end_group();
    if (count) *count = written;
    return r;
}

clay_result clay_list_backends(char* buffer, size_t* size) {
    if (!size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null size");
    std::string names;
    for (eval::Backend* b : eval::Registry::instance().all()) {
        if (!names.empty()) names += ",";
        names += b->name();
    }
    size_t needed = names.size() + 1;
    if (!buffer) {
        *size = needed;
        return CLAY_OK;
    }
    if (*size < needed) {
        *size = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL, "backend list needs " + std::to_string(needed));
    }
    std::memcpy(buffer, names.c_str(), needed);
    *size = needed;
    return CLAY_OK;
}

clay_result clay_backend_supports(const char* backend, clay_backend_op op,
                                  int32_t* out_supported) {
    if (!backend || !out_supported) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    eval::Backend* b = eval::Registry::instance().find(backend);
    // Not registered is NOT_FOUND rather than "supports nothing": a host falls
    // back from an operation this backend cannot do, and asks a different
    // question entirely about a backend that is not there.
    if (!b)
        return fail(CLAY_ERROR_NOT_FOUND, std::string("backend not registered: ") + backend);
    const eval::BackendCaps caps = b->caps();
    switch (op) {
        case CLAY_BACKEND_OP_EVAL_POINTS: *out_supported = caps.eval_points ? 1 : 0; return CLAY_OK;
        case CLAY_BACKEND_OP_EVAL_GRID: *out_supported = caps.eval_grid ? 1 : 0; return CLAY_OK;
        case CLAY_BACKEND_OP_RAYCAST: *out_supported = caps.raycast ? 1 : 0; return CLAY_OK;
    }
    return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown backend operation");
}

clay_result clay_backend_diagnostic(const char* backend, char* buffer, size_t* size) {
    if (!backend || !size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    std::string text = eval::backend_diagnostic(backend);
    // "Never compiled in" reads differently from "compiled in and failed",
    // because a host acts on them differently: one is a build to change, the
    // other is a machine or a bug to report. Neither is an error — the call
    // answered.
    if (text.empty() && !eval::backend_compiled_in(backend))
        text = std::string("backend '") + backend +
               "' is not compiled into this build of claycore";
    size_t needed = text.size() + 1;
    if (!buffer) {
        *size = needed;
        return CLAY_OK;
    }
    if (*size < needed) {
        *size = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "backend diagnostic needs " + std::to_string(needed));
    }
    std::memcpy(buffer, text.c_str(), needed);
    *size = needed;
    return CLAY_OK;
}

clay_result clay_eval_points(const clay_document* doc, const char* backend,
                             const float* points_xyz, size_t count, float* out_distances,
                             float* out_colors_rgb) {
    if (!doc || !points_xyz || !out_distances)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    return eval_into(*doc->tape(), backend, points_xyz, count,
                     eval::PointResults{out_distances, nullptr, out_colors_rgb});
}

clay_result clay_eval_gradients(const clay_document* doc, const char* backend,
                                const float* points_xyz, size_t count,
                                float* out_gradients_xyz) {
    if (!doc || !points_xyz || !out_gradients_xyz)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    return gradients_into(*doc->tape(), backend, points_xyz,
                          count, out_gradients_xyz);
}

clay_result clay_layer_eval_points(const clay_document* doc, clay_layer_id layer,
                                   const char* backend, const float* points_xyz, size_t count,
                                   float* out_distances, float* out_colors_rgb) {
    if (!doc || !points_xyz || !out_distances)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    scene::Tape tape;
    clay_result r = compile_one_layer(doc, layer, &tape);
    if (r != CLAY_OK) return r;
    return eval_into(tape, backend, points_xyz, count,
                     eval::PointResults{out_distances, nullptr, out_colors_rgb});
}

clay_result clay_layer_eval_gradients(const clay_document* doc, clay_layer_id layer,
                                      const char* backend, const float* points_xyz, size_t count,
                                      float* out_gradients_xyz) {
    if (!doc || !points_xyz || !out_gradients_xyz)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    scene::Tape tape;
    clay_result r = compile_one_layer(doc, layer, &tape);
    if (r != CLAY_OK) return r;
    return gradients_into(tape, backend, points_xyz, count, out_gradients_xyz);
}

clay_result clay_safe_step_scale(const clay_document* doc, float* out_scale) {
    if (!doc || !out_scale)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out pointer");
    *out_scale = doc->tape()->safe_step_scale();
    return CLAY_OK;
}

clay_result clay_layer_safe_step_scale(const clay_document* doc, clay_layer_id layer,
                                       float* out_scale) {
    if (!doc || !out_scale)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out pointer");
    scene::Tape tape;
    clay_result r = compile_one_layer(doc, layer, &tape);
    if (r != CLAY_OK) return r;
    *out_scale = tape.safe_step_scale();
    return CLAY_OK;
}

// -- consolidating a degraded chain ------------------------------------------

namespace {

constexpr std::size_t kFieldReportOriginal =
    offsetof(clay_field_report, advises_consolidation) + sizeof(std::int32_t);
constexpr std::size_t kConsolidationParamsOriginal =
    offsetof(clay_consolidation_params, skip_redistance) + sizeof(std::int32_t);
constexpr std::size_t kConsolidationCostOriginal =
    offsetof(clay_consolidation_cost, bounds_max) + sizeof(float) * 3;

// An OUTPUT descriptor: struct_size is the caller saying how much of it
// exists, not what it filled in, so it is probed and then written back.
clay_result begin_out_cost(clay_consolidation_cost* out) {
    clay_consolidation_cost probe;
    clay_result r = read_desc(out, kConsolidationCostOriginal, &probe);
    if (r != CLAY_OK) return r;
    write_desc(out, out->struct_size, clay_consolidation_cost{});
    return CLAY_OK;
}

// Callable only after begin_out_cost, which is what leaves struct_size holding
// the size the caller declared rather than whatever was in their buffer.
void write_cost(const scene::ConsolidationCost& src, clay_consolidation_cost* out) {
    clay_consolidation_cost filled{};
    filled.cell_size = src.cell_size;
    filled.band = src.band;
    filled.brick_count = static_cast<std::uint64_t>(src.brick_count);
    filled.sample_count = static_cast<std::uint64_t>(src.sample_count);
    filled.bytes = static_cast<std::uint64_t>(src.bytes);
    filled.sample_lipschitz = src.sample_lipschitz;
    filled.lipschitz = src.lipschitz;
    filled.safe_step_scale = src.safe_step_scale;
    const math::Aabb b = src.bounds.empty() ? math::Aabb{kernel::cf3(0, 0, 0), kernel::cf3(0, 0, 0)}
                                            : src.bounds;
    for (int a = 0; a < 3; ++a) {
        filled.bounds_min[a] = (&b.min.x)[a];
        filled.bounds_max[a] = (&b.max.x)[a];
    }
    write_desc(out, out->struct_size, filled);
}

clay_result read_consolidation(const clay_consolidation_params* params, const float region_min[3],
                               const float region_max[3], scene::ConsolidationParams* out) {
    clay_consolidation_params p;
    clay_result r = read_desc(params, kConsolidationParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (!(p.cell_size > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "cell_size must be > 0: a layer has no intrinsic scale to derive one from "
                    "the way a mesh's bounds give one");
    out->cell_size = p.cell_size;
    out->band = p.band;
    out->padding = p.padding;
    out->skip_redistance = p.skip_redistance != 0;
    if (region_min && region_max) {
        out->region = math::Aabb{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                                 kernel::cf3(region_max[0], region_max[1], region_max[2])};
        if (out->region.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty region");
    }
    return CLAY_OK;
}

}  // namespace

clay_result clay_layer_field_report(const clay_document* doc, clay_layer_id layer_id,
                                    float advise_below_step_scale,
                                    clay_field_report* out_report) {
    if (!doc || !out_report) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or report");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    clay_field_report probe;
    clay_result r = read_desc(out_report, kFieldReportOriginal, &probe);
    if (r != CLAY_OK) return r;

    const scene::FieldReport report = scene::report_layer(*layer, advise_below_step_scale);
    const std::uint32_t declared = out_report->struct_size;
    clay_field_report filled{};
    filled.lipschitz = report.lipschitz;
    filled.safe_step_scale = report.safe_step_scale;
    filled.steepest_volume = report.steepest_volume;
    filled.longest_deformer_chain = report.longest_deformer_chain;
    filled.item_count = report.item_count;
    filled.advises_consolidation = report.advises_consolidation ? 1 : 0;
    filled.steepest_deformer_chain = report.steepest_deformer_chain;
    filled.drawable_count = report.drawable_count;
    filled.degradation = static_cast<std::int32_t>(report.degradation);
    // write_desc copies only as far as the caller's own struct_size, so a
    // caller built against kFieldReportOriginal never sees the three fields
    // appended in 0.70.0 and never has them written past the end of its
    // struct.
    write_desc(out_report, declared, filled);
    return CLAY_OK;
}

clay_result clay_layer_consolidation_cost(const clay_document* doc, clay_layer_id layer_id,
                                          const clay_consolidation_params* params,
                                          const float region_min[3], const float region_max[3],
                                          clay_consolidation_cost* out_cost) {
    if (!doc || !params || !out_cost)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document, params or cost");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    scene::ConsolidationParams p;
    clay_result r = read_consolidation(params, region_min, region_max, &p);
    if (r != CLAY_OK) return r;
    r = begin_out_cost(out_cost);
    if (r != CLAY_OK) return r;

    scene::ConsolidationCost cost;
    if (!scene::bake_layer(*layer, p, &cost, eval::pooled_bake_eval()))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "nothing to consolidate: the layer is empty, unbounded, or the region "
                    "contains no surface");
    write_cost(cost, out_cost);
    return CLAY_OK;
}

clay_result clay_layer_consolidate(clay_document* doc, clay_layer_id layer_id,
                                   const clay_consolidation_params* params,
                                   const float region_min[3], const float region_max[3],
                                   clay_consolidation_cost* out_cost) {
    // Sugar over the cancellable form with no token, so there is one
    // implementation rather than two that could drift.
    return clay_layer_consolidate_cancellable(doc, layer_id, params, region_min, region_max,
                                              out_cost, nullptr);
}

clay_result clay_layer_consolidate_cancellable(clay_document* doc, clay_layer_id layer_id,
                                   const clay_consolidation_params* params,
                                   const float region_min[3], const float region_max[3],
                                   clay_consolidation_cost* out_cost,
                                               clay_cancel_token* token) {
    if (!doc || !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or params");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (layer->protected_from_edits())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "layer is protected (ghosted or locked)");
    scene::ConsolidationParams p;
    clay_result r = read_consolidation(params, region_min, region_max, &p);
    if (r != CLAY_OK) return r;
    if (out_cost) {
        r = begin_out_cost(out_cost);
        if (r != CLAY_OK) return r;
    }

    scene::ConsolidationCost cost;
    // Consolidate performs commands through the stack directly, so the session
    // is told afterwards how many entries appeared. It IS undoable — worth
    // saying, because it is the operation most often assumed not to be.
    scene::UndoStack* stack = doc->undo ? doc->undo->commands() : nullptr;
    bool cancelled = false;
    if (!scene::consolidate_layer(doc->doc.document, layer_id, p, stack, &cost,
                                  eval::pooled_bake_eval(), token ? &token->token : nullptr,
                                  &cancelled)) {
        // A cancel and "nothing to consolidate" both fail, and a host must not
        // show the second when the user did the first.
        if (cancelled)
            return fail(CLAY_ERROR_CANCELLED, "the consolidate was cancelled");
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "nothing to consolidate: the layer is empty, unbounded, or the region "
                    "contains no surface");
    }
    if (doc->undo) doc->undo->sync_scene_steps();
    doc->touch();
    if (out_cost) write_cost(cost, out_cost);
    return CLAY_OK;
}

namespace {

constexpr std::size_t kRegionMergeOriginal =
    offsetof(clay_region_merge, whole_layer) + sizeof(std::int32_t);

// The caller's region, refused rather than guessed at: a region merge without a
// region is a whole-layer consolidate, and a host should ask for that by name.
clay_result read_region(const float region_min[3], const float region_max[3],
                        math::Aabb* out) {
    if (!region_min || !region_max)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a region merge needs both region_min and region_max; for the whole layer "
                    "use clay_layer_consolidate");
    *out = math::Aabb{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                      kernel::cf3(region_max[0], region_max[1], region_max[2])};
    if (out->empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "the region is empty");
    return CLAY_OK;
}

void write_merge(const scene::RegionMerge& plan, clay_region_merge* out) {
    const std::uint32_t declared = out->struct_size;
    clay_region_merge filled{};
    if (!plan.box.empty()) {
        filled.box_min[0] = plan.box.min.x;
        filled.box_min[1] = plan.box.min.y;
        filled.box_min[2] = plan.box.min.z;
        filled.box_max[0] = plan.box.max.x;
        filled.box_max[1] = plan.box.max.y;
        filled.box_max[2] = plan.box.max.z;
    }
    filled.absorbed = plan.absorb.size();
    filled.whole_layer = plan.whole_layer ? 1 : 0;
    write_desc(out, declared, filled);
}

}  // namespace

clay_result clay_layer_plan_region_merge(const clay_document* doc, clay_layer_id layer_id,
                                         const float region_min[3], const float region_max[3],
                                         clay_region_merge* out_merge) {
    if (!doc || !out_merge) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or report");
    clay_region_merge probe;
    clay_result r = read_desc(out_merge, kRegionMergeOriginal, &probe);
    if (r != CLAY_OK) return r;
    math::Aabb region;
    if ((r = read_region(region_min, region_max, &region)) != CLAY_OK) return r;
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    write_merge(scene::plan_region_merge(*layer, region), out_merge);
    return CLAY_OK;
}

clay_result clay_layer_consolidate_region(clay_document* doc, clay_layer_id layer_id,
                                          const float region_min[3], const float region_max[3],
                                          const clay_consolidation_params* params,
                                          clay_consolidation_cost* out_cost,
                                          clay_region_merge* out_merge) {
    if (!doc || !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or params");
    math::Aabb region;
    clay_result r = read_region(region_min, region_max, &region);
    if (r != CLAY_OK) return r;
    if (out_merge) {
        clay_region_merge probe;
        if ((r = read_desc(out_merge, kRegionMergeOriginal, &probe)) != CLAY_OK) return r;
    }
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (layer->protected_from_edits())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "layer is protected (ghosted or locked)");

    scene::ConsolidationParams p;
    // The region goes to the merge, not through the params: the closure
    // replaces params.region, so passing it twice would invite a caller to set
    // one and mean the other.
    if ((r = read_consolidation(params, nullptr, nullptr, &p)) != CLAY_OK) return r;
    if (out_cost) {
        if ((r = begin_out_cost(out_cost)) != CLAY_OK) return r;
    }

    scene::ConsolidationCost cost;
    scene::RegionMerge plan;
    scene::UndoStack* stack = doc->undo ? doc->undo->commands() : nullptr;
    if (!scene::consolidate_region(doc->doc.document, layer_id, region, p, stack, &cost,
                                   eval::pooled_bake_eval(), nullptr, nullptr, &plan)) {
        if (out_merge) write_merge(plan, out_merge);
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "nothing to merge: the layer is empty or protected, or the region reaches "
                    "no item");
    }
    if (doc->undo) doc->undo->sync_scene_steps();
    doc->touch();
    if (out_cost) write_cost(cost, out_cost);
    if (out_merge) write_merge(plan, out_merge);
    return CLAY_OK;
}

clay_result clay_layer_consolidation_state(const clay_document* doc, clay_layer_id layer_id,
                                           int32_t* out_consolidated,
                                           clay_consolidation_cost* out_cost) {
    if (!doc || !out_consolidated)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out pointer");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (out_cost) {
        clay_result r = begin_out_cost(out_cost);
        if (r != CLAY_OK) return r;
    }
    scene::ConsolidationCost cost;
    const bool baked = scene::consolidation_state(*layer, &cost);
    *out_consolidated = baked ? 1 : 0;
    if (baked && out_cost) write_cost(cost, out_cost);
    return CLAY_OK;
}

clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]) {
    if (!doc || !origin || !dir || !out_hit)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    eval::Backend* b = eval::Registry::instance().find("cpu");
    std::shared_ptr<const scene::Tape> tape_ref = doc->pickable_tape();
    const scene::Tape& tape = *tape_ref;
    float ray[6] = {origin[0], origin[1], origin[2], dir[0], dir[1], dir[2]};
    eval::RayHit hit;
    if (raycast_visible(b, tape, ray, doc->doc.groups ? &*doc->doc.groups : nullptr, &hit) !=
        eval::Status::Ok)
        return fail(CLAY_ERROR_BACKEND, "raycast failed");
    *out_hit = hit.hit;
    if (out_t) *out_t = hit.t;
    if (out_position) std::memcpy(out_position, hit.pos, sizeof hit.pos);
    if (out_normal) std::memcpy(out_normal, hit.normal, sizeof hit.normal);
    return CLAY_OK;
}

clay_result clay_raycast_many(const clay_document* doc, const float* rays_origin_dir,
                              size_t count, int32_t* out_hits, float* out_t,
                              float* out_positions_xyz, float* out_normals_xyz) {
    if (!doc || (count > 0 && !rays_origin_dir))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or rays");
    if (count == 0) return CLAY_OK;  // no rays is no work, not a rejected query
    std::vector<float> rays;
    clay_result r = normalize_rays(rays_origin_dir, count, &rays);
    if (r != CLAY_OK) return r;
    eval::Backend* b = eval::Registry::instance().find("cpu");
    std::shared_ptr<const scene::Tape> tape_ref = doc->pickable_tape();
    const scene::Tape& tape = *tape_ref;
    std::vector<eval::RayHit> hits(count ? count : 1);
    eval::RayQuery q{rays.data(), count, 0.0f, 1e6f, 1e-4f, 256};
    if (b->raycast(tape, q, hits.data()) != eval::Status::Ok)
        return fail(CLAY_ERROR_BACKEND, "raycast failed");
    // The batch stays a batch. Only the rays that actually landed on hidden
    // surface are re-issued singly, so a document with nothing hidden — every
    // document that never named a region — pays exactly what it always did, and
    // one hidden group does not turn a thousand-ray batch into a thousand
    // calls.
    const voxel::GroupField* groups = doc->doc.groups ? &*doc->doc.groups : nullptr;
    if (groups && groups->any_hidden()) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!hits[i].hit) continue;
            const kernel::cfloat3 p =
                kernel::cf3(hits[i].pos[0], hits[i].pos[1], hits[i].pos[2]);
            if (!groups->point_hidden(p)) continue;
            if (raycast_visible(b, tape, &rays[i * 6], groups, &hits[i]) != eval::Status::Ok)
                return fail(CLAY_ERROR_BACKEND, "raycast failed");
        }
    }
    write_ray_hits(hits, count, out_hits, out_t, out_positions_xyz, out_normals_xyz);
    return CLAY_OK;
}

// -- picking (picking spec, through the C boundary) --------------------------

clay_result clay_raycast_attributed(const clay_document* doc, const float origin[3],
                                    const float dir[3], int32_t* out_hit, float* out_t,
                                    float out_position[3], float out_normal[3],
                                    clay_layer_id* out_layer, clay_node_id* out_node) {
    if (!doc || !out_hit) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_hit");
    math::Ray ray;
    clay_result r = make_ray(origin, dir, &ray);
    if (r != CLAY_OK) return r;
    pick::RaycastOptions ropts;
    // Hidden surface is stepped over, not turned into a miss: hiding the front
    // of a head is how an artist reaches the inside of it.
    if (doc->doc.groups) ropts.groups = &*doc->doc.groups;
    pick::SceneHit hit = pick::raycast_scene(doc->doc.document, ray, ropts);
    *out_hit = hit.hit ? 1 : 0;
    if (out_t) *out_t = hit.t;
    if (out_position) write_f3(out_position, hit.position);
    if (out_normal) write_f3(out_normal, hit.normal);
    if (out_layer) *out_layer = hit.layer;
    if (out_node) *out_node = hit.item;
    return CLAY_OK;
}

clay_result clay_snap_to_surface(const clay_document* doc, const float* points_xyz, size_t count,
                                 float* out_positions_xyz, float* out_normals_xyz,
                                 int32_t* out_ok) {
    if (!doc || (count > 0 && !points_xyz))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or points");
    clay_result r = check_batch("points", count);
    if (r != CLAY_OK) return r;
    std::shared_ptr<const scene::Tape> tape_ref = doc->pickable_tape();
    const scene::Tape& tape = *tape_ref;
    for (size_t i = 0; i < count; ++i) {
        kernel::cfloat3 p =
            kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]);
        pick::SnapResult s = pick::snap_to_surface(tape, p);
        if (out_positions_xyz) write_f3(out_positions_xyz + i * 3, s.position);
        if (out_normals_xyz) write_f3(out_normals_xyz + i * 3, s.normal);
        if (out_ok) out_ok[i] = s.ok ? 1 : 0;
    }
    return CLAY_OK;
}

clay_result clay_layer_bounds(const clay_document* doc, clay_layer_id layer_id, float out_min[3],
                              float out_max[3], int32_t* out_has_bounds) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    return write_bounds(layer_world_bounds(doc, *layer), out_min, out_max, out_has_bounds);
}

clay_result clay_layer_selection_bounds(const clay_document* doc, clay_layer_id layer_id,
                                        const clay_node_id* nodes, size_t count,
                                        float out_min[3], float out_max[3],
                                        int32_t* out_has_bounds) {
    if (!doc || (count > 0 && !nodes))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or nodes");
    clay_result r = check_batch("selected nodes", count);
    if (r != CLAY_OK) return r;
    if (!doc->doc.document.find_layer(layer_id))
        return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    std::vector<scene::NodeId> ids;
    if (count > 0) ids.assign(nodes, nodes + count);
    return write_bounds(pick::selection_bounds(doc->doc.document, layer_id, ids), out_min,
                        out_max, out_has_bounds);
}

clay_result clay_document_mesh(const clay_document* doc, const clay_mesh_params* params,
                               clay_mesh** out_mesh) {
    if (!doc || !params || !out_mesh)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_mesh_params p;
    clay_result r = read_desc(params, kMeshParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    if (tape.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty document");
    math::Aabb region = tape.bounds;
    if (region.empty() || region.is_infinite())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unbounded scene");
    float voxel = p.voxel_size;
    if (voxel <= 0) {
        int res = p.resolution > 0 ? p.resolution : 128;
        kernel::cfloat3 ext = region.extent();
        voxel = kernel::cmax(ext.x, kernel::cmax(ext.y, ext.z)) / static_cast<float>(res);
    }
    // The mesher samples a DENSE grid over the region, so the caller's voxel
    // size decides an allocation. Priced in cells and refused up front: an
    // over-fine size used to reach the allocator and terminate the host.
    if (!(voxel > 0.0f) || !std::isfinite(voxel))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "voxel size must be finite and > 0");
    {
        // The mesher's own ceiling, not the batch limit: CLAY_MAX_BATCH bounds
        // how many items cross this boundary in one call, which is a different
        // quantity, and it is far below the resolution 512 this API documents.
        kernel::cfloat3 ext = region.extent();
        const double cells = (static_cast<double>(ext.x) / voxel + 2.0) *
                             (static_cast<double>(ext.y) / voxel + 2.0) *
                             (static_cast<double>(ext.z) / voxel + 2.0);
        if (!(cells <= static_cast<double>(mesh::kMaxGridSamples)))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "the requested resolution needs more than " +
                            std::to_string(mesh::kMaxGridSamples) + " grid samples");
    }
    mesh::Mesh m;
    r = mesh_with(p.mesher, p.experimental != 0, tape, region, voxel, &m);
    if (r != CLAY_OK) return r;
    // What the artist put away does not come back in the export. A no-op when
    // nothing is hidden, so a document that never named a region meshes to the
    // bytes it always did.
    if (doc->doc.groups) voxel::drop_hidden(m, *doc->doc.groups);
    if (m.empty()) return fail(CLAY_ERROR_BACKEND, "meshing produced no triangles");
    if (p.decimate) {
        mesh::DecimateOptions opts;
        opts.target_ratio = p.decimate_ratio > 0 ? p.decimate_ratio : 0.5f;
        m = mesh::decimate(m, opts);
    }
    auto* handle = new clay_mesh();
    handle->data = std::move(m);
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_document_mesh_quads(const clay_document* doc, const clay_quad_params* params,
                                     clay_mesh** out_mesh) {
    if (!doc || !params || !out_mesh)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mesh = nullptr;
    clay_quad_params p;
    mesh::QuadTarget target;
    clay_result r = read_quad_params(params, &p, &target);
    if (r != CLAY_OK) return r;
    // Refused rather than quietly given the dual: substituting a smooth mesh
    // for a boxy one is a change the caller sees in the render and cannot see
    // in the return code.
    if (p.mode == CLAY_QUAD_FACES)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the faces mode is a voxel mode — it meshes exposed voxel faces, and a "
                    "document has none; use clay_voxel_mesh_quads or CLAY_QUAD_DUAL");

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    if (tape.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty document");
    const math::Aabb region = tape.bounds;
    if (region.empty() || region.is_infinite())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unbounded scene");
    if (p.cell_size <= 0.0f && p.target_quads == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "give a cell_size, a target_quads, or both — neither names a lattice");

    mesh::QuadFit fit;
    mesh::Mesh m = mesh::mesh_tape_quads_fit(tape, region, p.cell_size, target, {}, &fit);
    // The search stops at the mesher's own sample ceiling, so an empty mesh
    // here is a shape with no surface in the region rather than a lattice
    // nobody could afford.
    // Filtered BY QUAD, so the export keeps its quads — see voxel::drop_hidden.
    if (doc->doc.groups) voxel::drop_hidden(m, *doc->doc.groups);
    if (m.empty()) return fail(CLAY_ERROR_BACKEND, "quad meshing produced no faces");

    auto* handle = new clay_mesh();
    handle->data = std::move(m);
    handle->quad_provenance = clay_mesh::QuadProvenance{fit, p.target_quads};
    *out_mesh = handle;
    return CLAY_OK;
}

void clay_mesh_destroy(clay_mesh* mesh) {
    // A borrowed handle belongs to its document, which keeps it by address.
    if (mesh && !mesh->doc) delete mesh;
}

size_t clay_mesh_vertex_count(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m ? m->positions.size() : 0;
}
size_t clay_mesh_index_count(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m ? m->indices.size() : 0;
}
const float* clay_mesh_positions(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m && !m->positions.empty() ? &m->positions[0].x : nullptr;
}
// !empty() before the size comparison, not instead of it: clay_voxel_mesh is
// the one call that hands back a mesh with nothing in it, and indexing an
// empty vector to take the address of its first field is undefined even when
// the result is never dereferenced.
const float* clay_mesh_normals(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m && !m->normals.empty() && m->normals.size() == m->positions.size()
               ? &m->normals[0].x
               : nullptr;
}
const float* clay_mesh_colors(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m && !m->colors.empty() && m->colors.size() == m->positions.size()
               ? &m->colors[0].x
               : nullptr;
}
const float* clay_mesh_uvs(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m && !m->uvs.empty() && m->uvs.size() == m->positions.size() ? &m->uvs[0].x : nullptr;
}
const uint32_t* clay_mesh_indices(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m && !m->indices.empty() ? m->indices.data() : nullptr;
}

namespace {

// One attribute's place in an interleaved vertex: where the caller wants it,
// how wide it is, and where it comes from. An attribute the caller did not name
// has no entry, so the checks below never special-case "absent".
struct VertexAttr {
    std::uint32_t offset;
    std::uint32_t width;  // bytes
    const float* src;
    std::uint32_t src_stride;  // bytes between consecutive vertices in src
};

// Gathers the named attributes, refusing any the mesh does not carry. Refused
// rather than zero-filled: a silently black model is harder to find than an
// error at the call that asked for it.
clay_result collect_attrs(const clay_mesh* mesh, const clay_vertex_layout& l,
                          std::vector<VertexAttr>* out) {
    const struct {
        std::int32_t offset;
        std::uint32_t width;
        const float* src;
        const char* name;
    } wanted[] = {
        {l.position_offset, 12, clay_mesh_positions(mesh), "positions"},
        {l.normal_offset, 12, clay_mesh_normals(mesh), "normals"},
        {l.color_offset, 12, clay_mesh_colors(mesh), "colours"},
        {l.uv_offset, 8, clay_mesh_uvs(mesh), "uvs"},
    };
    for (const auto& w : wanted) {
        if (w.offset < 0) continue;
        if (!w.src)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        std::string("the layout names ") + w.name + ", which this mesh does not "
                        "carry");
        out->push_back({static_cast<std::uint32_t>(w.offset), w.width, w.src, w.width});
    }
    if (out->empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the layout names no attribute to copy");
    return CLAY_OK;
}

// The two mistakes that produce a buffer which is wrong without looking wrong:
// attributes that overlap, and a stride that does not clear them.
clay_result check_layout_fits(const std::vector<VertexAttr>& attrs, std::uint32_t* stride) {
    std::uint32_t packed_end = 0;
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(attrs[i].offset) + attrs[i].width;
        if (end > 0xFFFFFFFFull)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "an attribute offset overflows a vertex");
        packed_end = std::max<std::uint32_t>(packed_end, static_cast<std::uint32_t>(end));
        for (std::size_t j = i + 1; j < attrs.size(); ++j) {
            const bool disjoint = attrs[i].offset + attrs[i].width <= attrs[j].offset ||
                                  attrs[j].offset + attrs[j].width <= attrs[i].offset;
            if (!disjoint)
                return fail(CLAY_ERROR_INVALID_ARGUMENT,
                            "two attributes overlap in the vertex layout");
        }
    }
    if (*stride == 0) {
        *stride = packed_end;  // tightly packed IS the layout the caller described
    } else if (*stride < packed_end) {
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "stride " + std::to_string(*stride) + " does not clear the attributes, which "
                    "need " + std::to_string(packed_end) + " bytes");
    }
    return CLAY_OK;
}

}  // namespace

clay_result clay_mesh_copy_vertices(const clay_mesh* mesh, const clay_vertex_layout* layout,
                                    void* dst, size_t dst_bytes) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!layout || !dst) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null layout or destination");
    clay_vertex_layout l;
    r = read_desc(layout, kVertexLayoutOriginal, &l);
    if (r != CLAY_OK) return r;
    std::vector<VertexAttr> attrs;
    r = collect_attrs(mesh, l, &attrs);
    if (r != CLAY_OK) return r;
    std::uint32_t stride = l.stride;
    r = check_layout_fits(attrs, &stride);
    if (r != CLAY_OK) return r;
    const std::size_t vertices = m->positions.size();
    r = exact_capacity("interleaved vertex", vertices, stride, dst_bytes);
    if (r != CLAY_OK) return r;
    auto* out = static_cast<std::uint8_t*>(dst);
    for (std::size_t v = 0; v < vertices; ++v)
        for (const VertexAttr& a : attrs)
            std::memcpy(out + v * stride + a.offset,
                        reinterpret_cast<const std::uint8_t*>(a.src) + v * a.src_stride, a.width);
    return CLAY_OK;
}

clay_result clay_mesh_copy_indices(const clay_mesh* mesh, uint32_t* dst, size_t dst_count) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!dst) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null destination");
    r = exact_capacity("index", m->indices.size(), 1, dst_count);
    if (r != CLAY_OK) return r;
    if (!m->indices.empty())
        std::memcpy(dst, m->indices.data(), m->indices.size() * sizeof(std::uint32_t));
    return CLAY_OK;
}

size_t clay_mesh_quad_count(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m ? m->quad_count() : 0;
}
const uint32_t* clay_mesh_quads(const clay_mesh* mesh) {
    const mesh::Mesh* m = mesh_data(mesh);
    return m && !m->quads.empty() ? m->quads.data() : nullptr;
}

clay_result clay_mesh_copy_quads(const clay_mesh* mesh, uint32_t* dst, size_t dst_count) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!dst) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null destination");
    r = exact_capacity("quad index", m->quads.size(), 1, dst_count);
    if (r != CLAY_OK) return r;
    if (!m->quads.empty())
        std::memcpy(dst, m->quads.data(), m->quads.size() * sizeof(std::uint32_t));
    return CLAY_OK;
}

clay_result clay_mesh_quad_report(const clay_mesh* mesh, clay_quad_report* out_report) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!out_report) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null report");
    if (!mesh->quad_provenance)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this mesh was not produced by a quad mesher, so there is no search to "
                    "report — a mesh loaded from a file, concatenated, or meshed by any other "
                    "call has no cell size or target behind it");
    clay_quad_report probe;
    r = read_desc(out_report, kQuadReportOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_report->struct_size;

    const clay_mesh::QuadProvenance& p = *mesh->quad_provenance;
    clay_quad_report filled{};
    filled.cell_size = p.fit.cell_size;
    filled.target_quads = p.target;
    filled.quad_count = static_cast<std::uint64_t>(p.fit.quad_count);
    filled.iterations = p.fit.iterations;
    filled.within_tolerance = p.fit.within_tolerance ? 1 : 0;
    filled.clamped = p.fit.clamped ? 1 : 0;
    write_desc(out_report, declared, filled);
    return CLAY_OK;
}

clay_result clay_mesh_bounds(const clay_mesh* mesh, float out_min[3], float out_max[3]) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!out_min || !out_max) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_min or out_max");
    if (m->positions.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "an empty mesh has no bounds");
    math::Aabb box;  // default-constructed is the empty box
    for (const kernel::cfloat3& p : m->positions) box.expand(p);
    write_f3(out_min, box.min);
    write_f3(out_max, box.max);
    return CLAY_OK;
}

clay_result clay_mesh_validation_report(const clay_mesh* mesh, size_t max_intersection_pairs,
                                       clay_validation_report* out_report) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!out_report) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null report");
    // The descriptor is an OUTPUT, so struct_size is the caller telling us how
    // much of it exists rather than what it filled in.
    clay_validation_report probe;
    r = read_desc(out_report, kValidationReportOriginal, &probe);
    if (r != CLAY_OK) return r;
    if (max_intersection_pairs > CLAY_MAX_BATCH)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "max_intersection_pairs above CLAY_MAX_BATCH");

    const mesh::ValidationReport report = mesh::validate(*m, max_intersection_pairs);
    const std::uint32_t declared = out_report->struct_size;
    clay_validation_report filled{};
    filled.vertices = report.vertices;
    filled.triangles = report.triangles;
    filled.watertight = report.watertight ? 1 : 0;
    filled.manifold = report.manifold ? 1 : 0;
    filled.oriented = report.oriented ? 1 : 0;
    filled.clean = report.clean() ? 1 : 0;
    filled.boundary_edges = report.boundary_edges;
    filled.non_manifold_edges = report.non_manifold_edges;
    filled.degenerate_triangles = report.degenerate_triangles;
    filled.sliver_triangles = report.sliver_triangles;
    filled.intersecting_pairs = report.intersecting_pairs;
    // Echoed rather than recomputed: it is what tells a caller whether the
    // zero above means "none found" or "none looked for".
    filled.intersection_budget = max_intersection_pairs;
    filled.euler_characteristic = report.euler_characteristic;
    write_desc(out_report, declared, filled);
    return CLAY_OK;
}

clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                               int32_t* out_manifold) {
    // Sugar over the report, so there is one validation path rather than two
    // that could drift. No self-intersection pass, which is what this entry
    // point has always done.
    clay_validation_report report{};
    report.struct_size = static_cast<std::uint32_t>(sizeof(report));
    clay_result r = clay_mesh_validation_report(mesh, 0, &report);
    if (r != CLAY_OK) return r;
    if (out_watertight) *out_watertight = report.watertight;
    if (out_manifold) *out_manifold = report.manifold;
    return CLAY_OK;
}

clay_result clay_mesh_measure(const clay_mesh* mesh, double* out_signed_volume,
                              double* out_surface_area) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!out_signed_volume && !out_surface_area)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "both outputs are null");
    if (out_signed_volume) *out_signed_volume = mesh::signed_volume(*m);
    if (out_surface_area) *out_surface_area = mesh::surface_area(*m);
    return CLAY_OK;
}

// -- the sculpt handoff (add-sculpt-handoff-export) ---------------------------

clay_result clay_mesh_save_handoff(const clay_mesh* mesh, const char* path,
                                   const char* producer, const clay_mask* material_mask,
                                   int32_t binary) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path");
    io::HandoffOptions o;
    if (producer) o.producer = producer;
    o.material_mask = mask_of(material_mask);
    o.binary = binary != 0;
    return from_io(io::save_handoff_ply_file(*m, path, o));
}

clay_result clay_mesh_save_handoff_memory(const clay_mesh* mesh, const char* producer,
                                          const clay_mask* material_mask, int32_t binary,
                                          clay_blob** out_blob) {
    if (!out_blob) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_blob");
    *out_blob = nullptr;
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    io::HandoffOptions o;
    if (producer) o.producer = producer;
    o.material_mask = mask_of(material_mask);
    o.binary = binary != 0;
    *out_blob = new clay_blob{io::save_handoff_ply(*m, o)};
    return CLAY_OK;
}

clay_result clay_mesh_handoff_material_mix(const clay_mesh* mesh, const clay_mask* material_mask,
                                           float* out_values, size_t capacity,
                                           size_t* out_count) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    const std::vector<float> mix = io::handoff_material_mix(*m, mask_of(material_mask));
    *out_count = mix.size();
    if (!out_values) return CLAY_OK;  // a count query
    const std::size_t n = mix.size() < capacity ? mix.size() : capacity;
    for (std::size_t i = 0; i < n; ++i) out_values[i] = mix[i];
    *out_count = n;
    return CLAY_OK;
}

clay_result clay_mesh_save(const clay_mesh* mesh, const char* path) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path");
    std::string p(path);
    const std::string ext = extension_of(p);
    if (ext == "obj") return from_io(io::save_obj_file(*m, p));
    if (ext == "ply") return from_io(io::save_ply_file(*m, p));
    if (ext == "fbx") return from_io(io::save_fbx_file(*m, p));
    if (ext == "glb") return from_io(io::save_glb_file(*m, p));
    return fail(CLAY_ERROR_UNSUPPORTED, "unknown extension: " + ext);
}

clay_result clay_mesh_save_memory(const clay_mesh* mesh, const char* format,
                                  clay_blob** out_blob) {
    if (!out_blob) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_blob");
    *out_blob = nullptr;
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!format) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null format");
    const std::string name = lower_ascii(format);

    std::vector<std::uint8_t> bytes;
    if (name == "obj") {
        // No mtl NAME, so no mtllib line: a buffer has no companion file, and
        // naming one that was never written is worse than naming none.
        const std::string text = io::save_obj(*m, "claycore", {});
        bytes.assign(text.begin(), text.end());
    } else if (name == "ply") {
        bytes = io::save_ply(*m);
    } else if (name == "fbx") {
        bytes = io::save_fbx(*m);
    } else if (name == "glb") {
        bytes = io::save_glb(*m);
    } else {
        return fail(CLAY_ERROR_UNSUPPORTED, "unknown format: " + name);
    }
    *out_blob = new clay_blob{std::move(bytes)};
    return CLAY_OK;
}

clay_result clay_mesh_load_memory(const uint8_t* data, size_t size, const char* format,
                                  const clay_import_budget* budget, clay_mesh** out_mesh) {
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_mesh");
    *out_mesh = nullptr;
    if (!data || size == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or empty buffer");
    if (!format) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null format");
    const std::string name = lower_ascii(format);

    io::ImportBudget limits;
    clay_result r = import_limits(budget, &limits);
    if (r != CLAY_OK) return r;

    auto loaded = std::make_unique<clay_mesh>();
    io::IoStatus status;
    if (name == "obj") {
        status = io::load_obj(std::string(reinterpret_cast<const char*>(data), size),
                              &loaded->data, limits);
    } else if (name == "ply") {
        status = io::load_ply(data, size, &loaded->data, limits);
    } else if (name == "fbx") {
        status = io::load_fbx(data, size, &loaded->data, limits);
    } else if (name == "glb") {
        status = io::load_glb(data, size, &loaded->data, limits);
    } else {
        return fail(CLAY_ERROR_UNSUPPORTED, "unknown format: " + name);
    }
    if (!status.ok()) return from_io(status);
    *out_mesh = loaded.release();
    return CLAY_OK;
}

clay_result clay_mesh_load(const char* path, const clay_import_budget* budget,
                           clay_mesh** out_mesh) {
    if (!path || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path or out_mesh");
    *out_mesh = nullptr;

    io::ImportBudget limits;
    clay_result r = import_limits(budget, &limits);
    if (r != CLAY_OK) return r;

    std::string p(path);
    // Case-insensitive: a file called MODEL.OBJ is an OBJ file, and the Python
    // loader has always accepted one. The C ABI refusing it was a plain bug.
    const std::string ext = extension_of(p);

    auto loaded = std::make_unique<clay_mesh>();
    io::IoStatus status;
    if (ext == "obj") {
        status = io::load_obj_file(p, &loaded->data, limits);
    } else if (ext == "ply") {
        status = io::load_ply_file(p, &loaded->data, limits);
    } else if (ext == "fbx") {
        status = io::load_fbx_file(p, &loaded->data, limits);
    } else if (ext == "glb") {
        // .glb only, not .gltf: the JSON-only variant keeps its buffers in
        // separate files beside it, and a loader that took a path and silently
        // read whatever the JSON named would be reading files the caller never
        // handed it.
        status = io::load_glb_file(p, &loaded->data, limits);
    } else {
        return fail(CLAY_ERROR_UNSUPPORTED, "unknown extension: " + ext);
    }
    if (!status.ok()) return from_io(status);
    *out_mesh = loaded.release();
    return CLAY_OK;
}

clay_result clay_mesh_from_triangles(const float* positions, size_t vertex_count,
                                     const uint32_t* indices, size_t index_count,
                                     clay_mesh** out_mesh) {
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_mesh");
    *out_mesh = nullptr;
    if (!positions || !indices)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null positions or indices");
    if (vertex_count == 0 || index_count == 0 || index_count % 3 != 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "need a whole number of triangles");
    auto built = std::make_unique<clay_mesh>();
    built->data.positions.reserve(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i)
        built->data.positions.push_back(
            kernel::cf3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]));
    built->data.indices.assign(indices, indices + index_count);
    for (std::uint32_t index : built->data.indices)
        if (index >= vertex_count)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "an index points past the vertices");
    *out_mesh = built.release();
    return CLAY_OK;
}

clay_result clay_item_volume_from_mesh(const clay_mesh* mesh, const clay_volume_params* params,
                                       clay_item** out_item) {
    if (!params || !out_item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_item = nullptr;
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(mesh, &src);
    if (r != CLAY_OK) return r;
    clay_volume_params p;
    r = read_desc(params, kVolumeParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (src->triangle_count() == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the mesh has no triangles to sample");

    mesh::ImportSettings settings;
    settings.cell_size = p.cell_size;
    settings.band = p.band;
    settings.padding = p.padding;
    // Zero here means the default rather than "sum every triangle": summing
    // exactly is a testing control, and an app that reached it by leaving a
    // field zeroed would get an import that took minutes.
    settings.beta = p.beta > 0.0f ? p.beta : 2.0f;

    std::optional<field::FieldVolume> volume = mesh::to_field(*src, settings);
    if (!volume) return fail(CLAY_ERROR_INVALID_ARGUMENT, "the mesh could not be sampled");
    volume->set_feather(p.feather);

    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(std::move(*volume));
    *out_item = item;
    return CLAY_OK;
}

// The sampling half of baking from a document: descriptor, cell, band, and
// the region convention. Shared with clay_item_volume_flatten_from so this
// ABI has ONE meaning for "both NULL means the document's bounds padded by
// the band", rather than a second copy free to drift from it.
static clay_result read_volume_sampling(const clay_document* doc,
                                        const clay_volume_params* params,
                                        const float region_min[3], const float region_max[3],
                                        math::Aabb* out_region, float* out_cell,
                                        float* out_band, float* out_feather) {
    clay_volume_params p;
    clay_result r = read_desc(params, kVolumeParamsOriginal, &p);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    if (tape.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty document");

    const float cell = p.cell_size > 0.0f ? p.cell_size : 0.0f;
    if (!(cell > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "cell_size must be > 0: a document has no intrinsic scale to derive one "
                    "from the way a mesh's bounds give one");
    const float band = p.band > 0.0f ? p.band : cell * 3.0f;
    const float padding = p.padding > 0.0f ? p.padding : band;

    // One without the other is a caller that meant to pass a region and got it
    // half right; refused rather than silently falling back to the bounds.
    if ((region_min == nullptr) != (region_max == nullptr))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "pass both region_min and region_max, or neither");

    math::Aabb region;
    if (region_min && region_max) {
        region = math::Aabb{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                            kernel::cf3(region_max[0], region_max[1], region_max[2])};
    } else {
        region = tape.bounds;
        if (region.empty() || region.is_infinite())
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "unbounded scene; pass a region");
        // Padded by the band: sampling exactly to the bounds would clip the
        // band at the surface, which is where it is needed most.
        kernel::cfloat3 pad = kernel::cf3(padding, padding, padding);
        region = math::Aabb{region.min - pad, region.max + pad};
    }
    if (region.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty region");

    *out_region = region;
    *out_cell = cell;
    *out_band = band;
    // Absent from a shorter struct_size, in which case read_desc has already
    // zeroed it — the hard replace, which is what an older caller means.
    *out_feather = p.feather > 0.0f ? p.feather : 0.0f;
    return CLAY_OK;
}

clay_result clay_item_volume_from_document(const clay_document* doc,
                                           const clay_volume_params* params,
                                           const float region_min[3], const float region_max[3],
                                           clay_item** out_item) {
    if (!doc || !params || !out_item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_item = nullptr;

    math::Aabb region;
    float cell = 0.0f, band = 0.0f, feather = 0.0f;
    clay_result r = read_volume_sampling(doc, params, region_min, region_max, &region, &cell,
                                         &band, &feather);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;

    // Through the pool, on the same terms scene::bake_layer has always used.
    // The serial form this replaced asked the tape for one point at a time,
    // which is where a bake's time went: 7.5x on the benchmark pair that gates
    // consolidation, byte-identical output either way.
    field::FieldVolume volume = field::FieldVolume::sample_blocks(
        eval::document_block_fill(doc->doc.document, tape), region, cell, band);
    // brick_count, not empty(): a volume covering only empty space still has a
    // full brick index, it just stores no samples. It evaluates perfectly well
    // as "at least this far from anything" — and returning one from a BAKE
    // means the caller picked a region with no surface in it and would get an
    // item that silently contributes nothing.
    if (volume.brick_count() == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the region contains no surface to sample");
    volume.set_feather(feather);

    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(std::move(volume));
    *out_item = item;
    return CLAY_OK;
}

// Reads and validates a relax descriptor into settings. Shared by the
// in-place relax and the document-sourced one, exactly as the two flattens
// share read_flatten_settings and for the same reason: a second copy of the
// defaulting rules would drift.
static clay_result read_relax_settings(const clay_relax_params* params,
                                       field::RelaxSettings* out) {
    clay_relax_params p;
    clay_result r = read_desc(params, kRelaxParamsOriginal, &p);
    if (r != CLAY_OK) return r;

    field::RelaxSettings settings;
    settings.strength = std::clamp(p.strength, 0.0f, 1.0f);
    settings.radius_cells = p.radius_cells > 0 ? p.radius_cells : 1;
    settings.iterations = p.iterations > 0 ? p.iterations : 1;
    settings.centre = kernel::cf3(p.centre[0], p.centre[1], p.centre[2]);
    settings.region_radius = p.region_radius;
    settings.falloff = p.falloff;
    // Absent from a shorter struct_size, in which case read_desc has already
    // zeroed it and there is no mask — which is what an older caller means.
    if (p.mask) {
        voxel::MaskField* m = nullptr;
        r = resolve_mask(p.mask, &m);
        if (r != CLAY_OK) return r;
        // The engine takes a callable rather than a mask type, so a sampled
        // field stays a leaf module. Borrowed: the handle outlives the call.
        settings.mask = [m](kernel::cfloat3 q) { return m->sample(q); };
    }

    *out = std::move(settings);
    return CLAY_OK;
}

clay_result clay_item_volume_relax(clay_item* item, const clay_relax_params* params) {
    if (!item || !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    // Refused rather than ignored: an item that is not a volume has no samples
    // to smooth, and quietly returning OK would look like it worked.
    if (item->node.prim.type != scene::PrimType::Volume || !item->node.volume)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this item carries no volume to relax");

    field::RelaxSettings settings;
    clay_result r = read_relax_settings(params, &settings);
    if (r != CLAY_OK) return r;

    item->node.volume =
        std::make_shared<field::FieldVolume>(field::relax(*item->node.volume, settings));
    return CLAY_OK;
}

clay_result clay_item_volume_relax_from(const clay_document* doc,
                                        const clay_relax_params* relax,
                                        const clay_volume_params* volume,
                                        const float region_min[3],
                                        const float region_max[3],
                                        clay_item** out_item) {
    if (!doc || !relax || !volume || !out_item)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_item = nullptr;

    field::RelaxSettings settings;
    clay_result r = read_relax_settings(relax, &settings);
    if (r != CLAY_OK) return r;

    // The sampling half, on the same terms as clay_item_volume_from_document:
    // one region convention in this ABI, not two.
    math::Aabb region;
    float cell = 0.0f, band = 0.0f, feather = 0.0f;
    r = read_volume_sampling(doc, volume, region_min, region_max, &region, &cell, &band,
                             &feather);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;

    // Samples the document exactly as clay_item_volume_from_document would,
    // then relaxes those samples: identical to bake-then-relax inside the
    // band, by construction — and held by a test rather than assumed.
    field::FieldVolume relaxed = field::relax(eval::document_block_fill(doc->doc.document, tape),
                                              region, cell, band, settings);
    if (relaxed.brick_count() == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the region contains no surface to sample");
    relaxed.set_feather(feather);

    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(std::move(relaxed));
    *out_item = item;
    return CLAY_OK;
}

clay_result clay_item_volume_move_topological(clay_item* item,
                                             const clay_topological_move_params* params) {
    if (!item || !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_topological_move_params p;
    clay_result r = read_desc(params, kTopologicalMoveParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (!(p.radius > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "radius must be > 0");
    if ((r = check_ease(p.ease)) != CLAY_OK) return r;
    if (item->node.prim.type != scene::PrimType::Volume || !item->node.volume)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this item carries no volume to move");

    field::TopologicalMoveSettings settings;
    settings.anchor = kernel::cf3(p.anchor[0], p.anchor[1], p.anchor[2]);
    settings.radius = p.radius;
    settings.displacement =
        kernel::cf3(p.displacement[0], p.displacement[1], p.displacement[2]);
    settings.ease = static_cast<std::uint8_t>(p.ease);

    item->node.volume = std::make_shared<field::FieldVolume>(
        field::move_topological(*item->node.volume, settings));
    return CLAY_OK;
}

// Reads and validates a flatten descriptor into settings. Shared by the
// in-place flatten and the document-sourced one: two copies of "region_radius
// must be > 0" would drift, and the refusals ARE the contract here — every one
// of them exists because the alternative looks like it worked.
static clay_result read_flatten_settings(const clay_flatten_params* params,
                                         field::FlattenSettings* out) {
    clay_flatten_params p;
    clay_result r = read_desc(params, kFlattenParamsOriginal, &p);
    if (r != CLAY_OK) return r;

    field::FlattenSettings settings;
    settings.plane_point = kernel::cf3(p.plane_point[0], p.plane_point[1], p.plane_point[2]);
    settings.plane_normal = kernel::cf3(p.plane_normal[0], p.plane_normal[1], p.plane_normal[2]);
    // A zero normal describes no plane. Refused here rather than shaping the
    // field by an arbitrary direction, which would look like it worked.
    if (!(kernel::clength(settings.plane_normal) > 1e-6f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "plane_normal must not be zero length");
    settings.strength = std::clamp(p.strength, 0.0f, 1.0f);
    settings.centre = kernel::cf3(p.centre[0], p.centre[1], p.centre[2]);
    // Flatten is local: where its weight is one the result IS the plane, so
    // with no region it replaces the shape with a half-space rather than
    // flattening it.
    if (!(p.region_radius > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "region_radius must be > 0");
    settings.region_radius = p.region_radius;
    settings.falloff = p.falloff;
    if (p.mode < 0 || p.mode > CLAY_FLATTEN_FILL_ONLY)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown flatten mode: " + std::to_string(p.mode));
    settings.mode = static_cast<field::FlattenMode>(p.mode);
    if (p.mask) {
        voxel::MaskField* m = nullptr;
        r = resolve_mask(p.mask, &m);
        if (r != CLAY_OK) return r;
        // The engine takes a callable rather than a mask type, so a sampled
        // field stays a leaf module. Borrowed: the handle outlives the call.
        settings.mask = [m](kernel::cfloat3 q) { return m->sample(q); };
    }

    *out = std::move(settings);
    return CLAY_OK;
}

clay_result clay_item_volume_flatten(clay_item* item, const clay_flatten_params* params) {
    if (!item || !params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (item->node.prim.type != scene::PrimType::Volume || !item->node.volume)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this item carries no volume to flatten");

    field::FlattenSettings settings;
    clay_result r = read_flatten_settings(params, &settings);
    if (r != CLAY_OK) return r;

    item->node.volume =
        std::make_shared<field::FieldVolume>(field::flatten(*item->node.volume, settings));
    return CLAY_OK;
}

clay_result clay_item_volume_flatten_from(const clay_document* doc,
                                          const clay_flatten_params* flatten,
                                          const clay_volume_params* volume,
                                          const float region_min[3],
                                          const float region_max[3],
                                          clay_item** out_item) {
    if (!doc || !flatten || !volume || !out_item)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_item = nullptr;

    field::FlattenSettings settings;
    clay_result r = read_flatten_settings(flatten, &settings);
    if (r != CLAY_OK) return r;

    // The sampling half, on the same terms as clay_item_volume_from_document:
    // one region convention in this ABI, not two.
    math::Aabb region;
    float cell = 0.0f, band = 0.0f, feather = 0.0f;
    r = read_volume_sampling(doc, volume, region_min, region_max, &region, &cell, &band,
                             &feather);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;

    // The point of this entry point: the source is the DOCUMENT, which is
    // exact everywhere, rather than a volume that reports a bound outside its
    // band and places the facet against it.
    // tape_block_fill, NOT document_block_fill: flatten transforms the block
    // the fill produced, so a per-brick cull's near-surface classification no
    // longer matches what ends up stored. See the note on document_block_fill.
    field::FieldVolume flattened =
        field::flatten(eval::tape_block_fill(tape), region, cell, band, settings);
    if (flattened.brick_count() == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the region contains no surface to sample");
    flattened.set_feather(feather);

    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(std::move(flattened));
    *out_item = item;
    return CLAY_OK;
}

// -- voxel grids (c-abi spec: voxel grids across the ABI) --------------------

clay_voxel_grid* clay_voxel_grid_create(float voxel_size) {
    if (!(voxel_size > 0.0f)) {  // also rejects NaN
        fail(CLAY_ERROR_INVALID_ARGUMENT, "voxel_size must be > 0");
        return nullptr;
    }
    auto* handle = new clay_voxel_grid();
    handle->owned = new voxel::VoxelGrid(voxel_size);
    return handle;
}

clay_result clay_voxel_grid_destroy(clay_voxel_grid* grid) {
    if (!grid) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null voxel grid");
    if (!grid->owned)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this grid is a document layer: the document owns it");
    delete grid->owned;
    delete grid;
    return CLAY_OK;
}

clay_result clay_document_add_voxel_layer(clay_document* doc, const char* name, float voxel_size,
                                          clay_layer_id* out_layer, clay_voxel_grid** out_grid) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    if (!(voxel_size > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "voxel_size must be > 0");
    // Through the command vocabulary (AddLayerCmd with a reserved id), exactly
    // as clay_add_sdf_layer and clay_document_add_mesh_layer do, so an enabled
    // undo stack records the creation.
    //
    // This was the ONE reachable layer creation the history never saw (#341).
    // It mattered most at a crossing — make a voxel layer, rasterize a starting
    // form into it — because the rasterize DOES record: the fill was one step
    // and the layer was none, so a single undo emptied the new layer and left
    // it standing, which is not the half of a conversion anyone asked to take
    // back. A voxel layer carries no SDF content.
    scene::Layer layer;
    layer.id = doc->doc.document.reserve_layer_id();
    layer.name = name;
    layer.kind = scene::LayerKind::Voxel;
    const clay_layer_id id = layer.id;
    clay_result r = apply_edit(doc, scene::Command{scene::AddLayerCmd{std::move(layer), -1}},
                               "the voxel layer could not be added");
    if (r != CLAY_OK) return r;
    // Beside the document, keyed by layer id, as the mesh path does: undoing
    // the creation removes the LAYER and leaves this entry, which is what lets
    // a redo pick the cells back up. AddLayerCmd carries a scene::Layer by
    // value and could not carry a grid. save_clayspace is what keeps an
    // orphan from reaching a file — see is_voxel_layer there.
    //
    // insert_or_assign rather than emplace: a loaded document can carry a grid
    // for a layer it no longer has, and emplace would keep that dead sculpt
    // and hand it to the next layer that takes the id.
    doc->doc.voxel_layers.insert_or_assign(id, voxel::VoxelGrid(voxel_size));
    if (out_layer) *out_layer = id;
    if (out_grid) *out_grid = borrow_layer(doc, id);
    return CLAY_OK;
}

clay_result clay_document_voxel_layer(clay_document* doc, const char* name,
                                      clay_layer_id* out_layer, clay_voxel_grid** out_grid) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    for (const scene::Layer& layer : doc->doc.document.layers) {
        if (layer.name != name || layer.kind != scene::LayerKind::Voxel) continue;
        if (!doc->doc.voxel_layers.count(layer.id)) continue;
        if (out_layer) *out_layer = layer.id;
        if (out_grid) *out_grid = borrow_layer(doc, layer.id);
        return CLAY_OK;
    }
    return fail(CLAY_ERROR_NOT_FOUND, std::string("no voxel layer named ") + name);
}

clay_result clay_document_voxel_layer_by_id(clay_document* doc, clay_layer_id layer,
                                            clay_voxel_grid** out_grid) {
    // out_grid is required here and optional in the by-name form: there a NULL
    // still leaves the call something to answer (the resolved id), here the
    // caller already supplied the id and the handle is the whole answer.
    if (!doc || !out_grid) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_grid");
    // Resolved in the DOCUMENT first, not in voxel_layers. Undoing the creation
    // removes the layer and KEEPS the grid beside the document (see
    // clay_document_add_voxel_layer), so the side table alone would hand back
    // the grid of a layer that is not currently there — which is not what the
    // by-name lookup does and would be a new way to edit an undone layer.
    //
    // The kind check is not redundant with the payload check below by
    // CONTRACT, only by coincidence: voxel_layers is keyed by layer id and is
    // written by nothing but the voxel create call, so today a layer of
    // another kind is never in it. Reverting this line alone therefore fails
    // no test. It stays because the by-name form checks the kind in the same
    // position and the two are required to agree, and because the coincidence
    // is a property of the create calls rather than of the type.
    const scene::Layer* found = doc->doc.document.find_layer(layer);
    if (!found || found->kind != scene::LayerKind::Voxel)
        return fail(CLAY_ERROR_NOT_FOUND, "no voxel layer with id " + std::to_string(layer));
    // A voxel layer whose grid did not come with the file it was loaded from:
    // the by-name form guards the same way, and the two must agree.
    if (!doc->doc.voxel_layers.count(layer))
        return fail(CLAY_ERROR_NOT_FOUND,
                    "voxel layer " + std::to_string(layer) + " holds no grid");
    *out_grid = borrow_layer(doc, layer);
    return CLAY_OK;
}

// -- mesh layers (c-abi spec: mesh layers across the ABI) --------------------

namespace {

// What a document may CARRY, not what a file may decode into: the loader's
// 50M vertices is 600 MB of positions alone, which is not a default a document
// should inherit. Zero on a field means this default, as everywhere else.
constexpr std::size_t kMeshLayerMaxVertices = 8u * 1000 * 1000;
constexpr std::size_t kMeshLayerMaxTriangles = 16u * 1000 * 1000;

clay_result check_attach_budget(const mesh::Mesh& m, const clay_mesh_layer_desc& d) {
    std::size_t max_vertices =
        d.max_vertices ? static_cast<std::size_t>(d.max_vertices) : kMeshLayerMaxVertices;
    std::size_t max_triangles =
        d.max_triangles ? static_cast<std::size_t>(d.max_triangles) : kMeshLayerMaxTriangles;
    if (m.positions.size() > max_vertices)
        return fail(CLAY_ERROR_BUDGET_EXCEEDED,
                    "the mesh has " + std::to_string(m.positions.size()) +
                        " vertices, over the attach budget of " + std::to_string(max_vertices));
    if (m.triangle_count() > max_triangles)
        return fail(CLAY_ERROR_BUDGET_EXCEEDED,
                    "the mesh has " + std::to_string(m.triangle_count()) +
                        " triangles, over the attach budget of " + std::to_string(max_triangles));
    return CLAY_OK;
}

}  // namespace

clay_result clay_document_add_mesh_layer(clay_document* doc, const clay_mesh* mesh,
                                         const clay_mesh_layer_desc* desc,
                                         clay_layer_id* out_layer, clay_mesh** out_mesh) {
    if (!doc || !desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or descriptor");
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(mesh, &src);
    if (r != CLAY_OK) return r;
    clay_mesh_layer_desc d;
    r = read_desc(desc, kMeshLayerDescOriginal, &d);
    if (r != CLAY_OK) return r;
    if (!d.name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "a mesh layer needs a name");
    if (src->triangle_count() == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the mesh has no triangles to carry");
    r = check_attach_budget(*src, d);
    if (r != CLAY_OK) return r;

    // Copied, and the import scale baked in, before the layer exists: a
    // failure past this point would leave a layer with no geometry.
    mesh::Mesh stored = *src;
    if (d.import_scale > 0.0f && d.import_scale != 1.0f)
        for (kernel::cfloat3& p : stored.positions) p = p * d.import_scale;

    // Through the command vocabulary, like clay_add_sdf_layer, so an enabled
    // undo stack records the attach. A mesh layer carries no SDF content.
    scene::Layer layer;
    layer.id = doc->doc.document.reserve_layer_id();
    layer.name = d.name;
    layer.kind = scene::LayerKind::Mesh;
    clay_layer_id id = layer.id;
    r = apply_edit(doc, scene::Command{scene::AddLayerCmd{std::move(layer), -1}},
                   "the mesh layer could not be added");
    if (r != CLAY_OK) return r;
    // Beside the document, keyed by layer id: undoing the attach removes the
    // layer and leaves this entry, which save_clayspace then does not write.
    doc->doc.mesh_layers.insert_or_assign(id, std::move(stored));
    if (out_layer) *out_layer = id;
    if (out_mesh) *out_mesh = borrow_mesh_layer(doc, id);
    return CLAY_OK;
}

clay_result clay_document_mesh_layer(clay_document* doc, const char* name,
                                     clay_layer_id* out_layer, clay_mesh** out_mesh) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    for (const scene::Layer& layer : doc->doc.document.layers) {
        if (layer.name != name || layer.kind != scene::LayerKind::Mesh) continue;
        if (!doc->doc.mesh_layers.count(layer.id)) continue;
        if (out_layer) *out_layer = layer.id;
        if (out_mesh) *out_mesh = borrow_mesh_layer(doc, layer.id);
        return CLAY_OK;
    }
    return fail(CLAY_ERROR_NOT_FOUND, std::string("no mesh layer named ") + name);
}

// The mesh half of clay_document_voxel_layer_by_id, on the same terms and for
// the same reasons — see there for why the layer is resolved in the document
// rather than in the payloads held beside it.
clay_result clay_document_mesh_layer_by_id(clay_document* doc, clay_layer_id layer,
                                           clay_mesh** out_mesh) {
    if (!doc || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_mesh");
    const scene::Layer* found = doc->doc.document.find_layer(layer);
    if (!found || found->kind != scene::LayerKind::Mesh)
        return fail(CLAY_ERROR_NOT_FOUND, "no mesh layer with id " + std::to_string(layer));
    if (!doc->doc.mesh_layers.count(layer))
        return fail(CLAY_ERROR_NOT_FOUND,
                    "mesh layer " + std::to_string(layer) + " holds no geometry");
    *out_mesh = borrow_mesh_layer(doc, layer);
    return CLAY_OK;
}

clay_result clay_mesh_layer(const clay_mesh* mesh, clay_layer_id* out_layer) {
    if (!mesh || !out_layer) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh or out_layer");
    if (!mesh->doc) return fail(CLAY_ERROR_NOT_FOUND, "this mesh is not a document layer");
    *out_layer = mesh->layer;
    return CLAY_OK;
}

// -- combining meshes for export --------------------------------------------

}  // extern "C" — the helpers below return C++ types and cannot have C linkage

namespace {

// The mesh a handle refers to: its own, or the layer's if it is a borrow.
const mesh::Mesh* mesh_of(const clay_mesh* handle) {
    if (!handle) return nullptr;
    if (handle->doc) {
        auto it = handle->doc->doc.mesh_layers.find(handle->layer);
        return it == handle->doc->doc.mesh_layers.end() ? nullptr : &it->second;
    }
    return &handle->data;
}

// Concatenate, rebasing indices.
//
// An attribute present on some inputs and absent on others is DROPPED. Padding
// it would invent data and truncating it would return a mesh whose normals are
// non-empty and a different length than its positions — which is malformed,
// and which no call here may hand back. So the rule is: an attribute survives
// only if EVERY input carries it.
mesh::Mesh concat_meshes(const std::vector<const mesh::Mesh*>& parts) {
    mesh::Mesh out;
    bool keep_normals = !parts.empty(), keep_colors = !parts.empty(), keep_uvs = !parts.empty();
    // The quads follow the same all-or-nothing rule, and they must: a result
    // that is quads over part of itself has an `indices` array that is no
    // longer its quad list's triangulation, which is the invariant every quad
    // consumer reads.
    bool keep_quads = !parts.empty();
    std::size_t vertices = 0, indices = 0, quads = 0;
    for (const mesh::Mesh* m : parts) {
        keep_normals = keep_normals && m->normals.size() == m->positions.size();
        keep_colors = keep_colors && m->colors.size() == m->positions.size();
        keep_uvs = keep_uvs && m->uvs.size() == m->positions.size();
        keep_quads = keep_quads && m->has_quads();
        vertices += m->positions.size();
        indices += m->indices.size();
        quads += m->quads.size();
    }
    out.positions.reserve(vertices);
    out.indices.reserve(indices);
    if (keep_normals) out.normals.reserve(vertices);
    if (keep_colors) out.colors.reserve(vertices);
    if (keep_uvs) out.uvs.reserve(vertices);
    if (keep_quads) out.quads.reserve(quads);

    std::uint32_t base = 0;
    for (const mesh::Mesh* m : parts) {
        out.positions.insert(out.positions.end(), m->positions.begin(), m->positions.end());
        if (keep_normals) out.normals.insert(out.normals.end(), m->normals.begin(), m->normals.end());
        if (keep_colors) out.colors.insert(out.colors.end(), m->colors.begin(), m->colors.end());
        if (keep_uvs) out.uvs.insert(out.uvs.end(), m->uvs.begin(), m->uvs.end());
        for (std::uint32_t i : m->indices) out.indices.push_back(i + base);
        if (keep_quads)
            for (std::uint32_t i : m->quads) out.quads.push_back(i + base);
        base += static_cast<std::uint32_t>(m->positions.size());
    }
    return out;
}

}  // namespace

extern "C" {

clay_result clay_mesh_transform(const clay_mesh* mesh, const float position[3],
                                const float rotation_axis[3], float rotation_angle,
                                float scale, clay_mesh** out_mesh) {
    if (!mesh || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mesh = nullptr;
    const mesh::Mesh* src = mesh_of(mesh);
    if (!src) return fail(CLAY_ERROR_NOT_FOUND, "this handle refers to no mesh");

    // The same reader every other transform in this ABI goes through, so a
    // mesh transform accepts exactly what a layer or item transform accepts.
    math::Transform xform;
    clay_result r = read_transform(position, rotation_axis, rotation_angle, scale, &xform);
    if (r != CLAY_OK) return r;

    auto* handle = new clay_mesh();
    handle->data = *src;
    // The quads come with the copy and stay valid: nothing here rewrites an
    // index. So does the report — this is the same mesh, moved.
    handle->quad_provenance = mesh->quad_provenance;
    for (kernel::cfloat3& v : handle->data.positions) v = xform.apply(v);
    // Normals rotate, but do not translate and do not scale: a uniform scale
    // leaves a direction unchanged, and adding the position would turn a
    // direction into a point.
    for (kernel::cfloat3& n : handle->data.normals) n = xform.rotation.rotate(n);
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_mesh_transform_nonuniform(const clay_mesh* mesh, const float position[3],
                                           const float rotation_axis[3], float rotation_angle,
                                           const float scale[3], clay_mesh** out_mesh) {
    if (!mesh || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mesh = nullptr;
    const mesh::Mesh* src = mesh_of(mesh);
    if (!src) return fail(CLAY_ERROR_NOT_FOUND, "this handle refers to no mesh");

    kernel::cfloat3 axes;
    clay_result r = read_scale_axes(scale, &axes);
    if (r != CLAY_OK) return r;
    // The rotation and position come through the same reader every other
    // transform in this ABI uses; the scale is the part that is not a
    // similarity, so it is applied here rather than carried by the Transform.
    math::Transform xform;
    r = read_transform(position, rotation_axis, rotation_angle, 1.0f, &xform);
    if (r != CLAY_OK) return r;

    auto* handle = new clay_mesh();
    handle->data = *src;
    handle->quad_provenance = mesh->quad_provenance;
    for (kernel::cfloat3& v : handle->data.positions)
        v = xform.apply(kernel::cf3(v.x * axes.x, v.y * axes.y, v.z * axes.z));
    // Normals go through the INVERSE TRANSPOSE of the linear part, which for
    // rotation-times-diagonal is the rotation times the reciprocal scale. The
    // uniform call can rotate a normal and stop, because a similarity leaves a
    // direction alone; a squash does not — transforming a normal as a direction
    // tilts every one of them off the surface, and the shading and every
    // consumer that trusts them go with it. Renormalized, since the reciprocal
    // scale does not preserve length.
    for (kernel::cfloat3& n : handle->data.normals) {
        kernel::cfloat3 t =
            xform.rotation.rotate(kernel::cf3(n.x / axes.x, n.y / axes.y, n.z / axes.z));
        // A normal that was already degenerate stays as it was rather than
        // becoming a NaN: this call moves a mesh, it does not repair one.
        const float len2 = kernel::cdot2(t);
        n = len2 > 0.0f ? t / kernel::csqrt(len2) : n;
    }
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_mesh_concat(const clay_mesh* const* meshes, size_t count,
                             clay_mesh** out_mesh) {
    if (!meshes || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mesh = nullptr;
    if (count == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "no meshes to concatenate");

    std::vector<const mesh::Mesh*> parts;
    parts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const mesh::Mesh* m = mesh_of(meshes[i]);
        if (!m) return fail(CLAY_ERROR_INVALID_ARGUMENT,
                            "mesh " + std::to_string(i) + " is null or refers to no mesh");
        parts.push_back(m);
    }
    auto* handle = new clay_mesh();
    handle->data = concat_meshes(parts);
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_document_mesh_combined(const clay_document* doc,
                                        const clay_mesh_params* params,
                                        clay_mesh** out_mesh) {
    if (!doc || !params || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mesh = nullptr;

    // The field first, through the untouched call, so a document with no
    // visible mesh layer gets exactly what clay_document_mesh gives.
    clay_mesh* field = nullptr;
    clay_result r = clay_document_mesh(doc, params, &field);
    if (r != CLAY_OK) return r;

    std::vector<mesh::Mesh> placed;
    for (const scene::Layer& layer : doc->doc.document.layers) {
        if (layer.kind != scene::LayerKind::Mesh) continue;
        // Hidden means contributes nothing. Ghost and lock deliberately do
        // NOT filter here: neither changes what a document evaluates to, so
        // neither may change what it exports.
        if (!layer.visible) continue;
        auto it = doc->doc.mesh_layers.find(layer.id);
        if (it == doc->doc.mesh_layers.end()) continue;

        mesh::Mesh m = it->second;
        const kernel::cfloat3 axes = layer.scale_axes;
        for (kernel::cfloat3& v : m.positions)
            v = layer.xform.apply(kernel::cf3(v.x * axes.x, v.y * axes.y, v.z * axes.z));
        // Normals through the INVERSE TRANSPOSE of the linear part, which for
        // rotation-times-diagonal is the rotation times the reciprocal scale —
        // the same rule and the same code shape clay_mesh_transform_nonuniform
        // states. Rotating a normal is right for a similarity and wrong for a
        // squash: it tilts every normal off the surface and takes the shading
        // with it.
        const bool squashed = scene::layer_is_squashed(layer);
        for (kernel::cfloat3& n : m.normals) {
            if (!squashed) {
                n = layer.xform.rotation.rotate(n);
                continue;
            }
            kernel::cfloat3 t = layer.xform.rotation.rotate(
                kernel::cf3(n.x / axes.x, n.y / axes.y, n.z / axes.z));
            const float len2 = kernel::cdot2(t);
            n = len2 > 0.0f ? t / kernel::csqrt(len2) : n;
        }
        placed.push_back(std::move(m));
    }

    if (placed.empty()) {
        *out_mesh = field;
        return CLAY_OK;
    }

    std::vector<const mesh::Mesh*> parts;
    parts.reserve(placed.size() + 1);
    parts.push_back(&field->data);
    for (const mesh::Mesh& m : placed) parts.push_back(&m);

    auto* handle = new clay_mesh();
    handle->data = concat_meshes(parts);
    clay_mesh_destroy(field);
    *out_mesh = handle;
    return CLAY_OK;
}

// -- the cut tool (c-abi spec: the cut tool) ---------------------------------

clay_item* clay_cut_create(const clay_cut_desc* desc, const float* polygon_xy,
                           size_t polygon_count) {
    if (!desc) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "null cut descriptor");
        return nullptr;
    }
    clay_cut_desc d;
    if (read_desc(desc, kCutDescOriginal, &d) != CLAY_OK) return nullptr;

    cut::CutFrame frame;
    frame.origin = kernel::cf3(d.origin[0], d.origin[1], d.origin[2]);
    frame.right = kernel::cf3(d.right[0], d.right[1], d.right[2]);
    frame.up = kernel::cf3(d.up[0], d.up[1], d.up[2]);
    frame.forward = kernel::cf3(d.forward[0], d.forward[1], d.forward[2]);
    if (!frame.is_orthonormal()) {
        fail(CLAY_ERROR_INVALID_ARGUMENT,
             "right, up and forward must be orthonormal: the shape was drawn in a frame, and "
             "squaring it up here would cut somewhere the user did not draw");
        return nullptr;
    }
    if (d.rounding < 0.0f) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "cut rounding must be >= 0");
        return nullptr;
    }

    cut::CutShape shape;
    switch (d.shape) {
        case CLAY_CUT_CIRCLE:
            shape = cut::CutShape::circle(d.radius);
            break;
        case CLAY_CUT_POLYGON: {
            if (polygon_count > 0 && !polygon_xy) {
                fail(CLAY_ERROR_INVALID_ARGUMENT, "null polygon outline");
                return nullptr;
            }
            if (check_batch("polygon vertices", polygon_count) != CLAY_OK) return nullptr;
            std::vector<kernel::cfloat2> verts;
            verts.reserve(polygon_count);
            for (std::size_t i = 0; i < polygon_count; ++i)
                verts.push_back(kernel::cf2(polygon_xy[i * 2], polygon_xy[i * 2 + 1]));
            shape = cut::CutShape::from_polygon(std::move(verts));
            break;
        }
        case CLAY_CUT_RECT:
            shape = cut::CutShape::rect(d.half_width, d.half_height);
            break;
        default:
            fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown cut shape: " + std::to_string(d.shape));
            return nullptr;
    }

    cut::CutOptions options;
    options.rounding = d.rounding;
    // Both zero is the "derive it" sentinel the header documents: a sweep of
    // no depth is not a cut anyone means to ask for.
    if (d.near_extent != 0.0f || d.far_extent != 0.0f) {
        options.near_extent = d.near_extent;
        options.far_extent = d.far_extent;
    }

    math::Aabb region{kernel::cf3(d.region_min[0], d.region_min[1], d.region_min[2]),
                      kernel::cf3(d.region_max[0], d.region_max[1], d.region_max[2])};
    std::optional<scene::Node> node = cut::cut_item(frame, shape, region, options);
    if (!node) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "the cut is degenerate: a shape with no area");
        return nullptr;
    }
    auto* item = new clay_item();
    item->node = std::move(*node);
    return item;
}

clay_item* clay_tube_create(const float* points_xyz, size_t count,
                            const clay_tube_params* params, int32_t profile,
                            const float* profile_params, size_t profile_param_count) {
    if (!points_xyz || !params || count < 2) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "a tube needs at least two points");
        return nullptr;
    }
    // The upper bound every other out-of-line payload is held to. Without it a
    // bogus count reserved for itself and std::bad_alloc reached the host as a
    // terminate, since the library builds -fno-exceptions.
    if (check_payload("tube points", points_xyz, count) != CLAY_OK) return nullptr;
    clay_tube_params p;
    if (read_desc(params, kTubeParamsOriginal, &p) != CLAY_OK) return nullptr;
    if (p.point_type < 0 || p.point_type > CLAY_POINT_BEZIER) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown point type");
        return nullptr;
    }

    brush::TubeSettings settings;
    settings.point_type = static_cast<scene::StrokePointType>(p.point_type);
    settings.radius_start = p.radius_start;
    settings.radius_mid = p.radius_mid;
    settings.radius_end = p.radius_end;
    settings.closed = p.closed != 0;
    settings.tolerance = p.tolerance > 0.0f ? p.tolerance : 0.01f;
    settings.blend_k = p.blend_k;

    std::vector<kernel::cfloat3> path;
    path.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        path.push_back(kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1],
                                   points_xyz[i * 3 + 2]));

    std::optional<scene::Node> node;
    if (profile >= 0) {
        if (profile > CLAY_PROFILE_POLYGON || profile == CLAY_PROFILE_POLYGON) {
            fail(CLAY_ERROR_INVALID_ARGUMENT,
                 "a tube profile must be a parametric kind, not a polygon");
            return nullptr;
        }
        if (check_params("profile", profile_params, profile_param_count,
                         kProfileParams[profile]) != CLAY_OK)
            return nullptr;
        scene::Profile prof{static_cast<std::uint8_t>(profile), {}};
        for (std::size_t i = 0; i < profile_param_count; ++i)
            prof.params[i] = profile_params[i];
        node = brush::tube_with_profile(path, {prof}, settings);
    } else {
        node = brush::tube(path, settings);
    }
    if (!node) {
        fail(CLAY_ERROR_INVALID_ARGUMENT,
             "a tube needs at least two points and a radius > 0 somewhere");
        return nullptr;
    }
    auto* item = new clay_item();
    item->node = std::move(*node);
    return item;
}

clay_result clay_cut_polygon_from_open_curve(const float* points_xyzr, size_t count,
                                             const int32_t* types, int32_t side,
                                             const float extent_xy[2], float tolerance,
                                             float* out_xy, size_t* out_count) {
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    if (!extent_xy) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null extent");
    if (!(tolerance > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "tolerance must be > 0");
    if (side < 0 || side > CLAY_TRIM_RIGHT)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown trim side: " + std::to_string(side));
    std::vector<scene::StrokePoint> control;
    clay_result r = read_curve_points(points_xyzr, count, types, nullptr, nullptr, &control);
    if (r != CLAY_OK) return r;

    cut::CutShape shape = cut::CutShape::from_open_curve(
        control, static_cast<cut::CutShape::Side>(side),
        kernel::cf2(extent_xy[0], extent_xy[1]), tolerance);
    if (shape.polygon.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a trim needs at least two control points");
    const std::size_t needed = shape.polygon.size();
    if (!out_xy) {
        *out_count = needed;
        return CLAY_OK;
    }
    if (*out_count < needed) {
        *out_count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the outline needs " + std::to_string(needed) + " vertices");
    }
    for (std::size_t i = 0; i < needed; ++i) {
        out_xy[i * 2 + 0] = shape.polygon[i].x;
        out_xy[i * 2 + 1] = shape.polygon[i].y;
    }
    *out_count = needed;
    return CLAY_OK;
}

clay_result clay_cut_polygon_from_curve(const float* points_xyzr, size_t count,
                                        const int32_t* types, float tolerance, float* out_xy,
                                        size_t* out_count) {
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    if (!(tolerance > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "tolerance must be > 0");
    std::vector<scene::StrokePoint> control;
    clay_result r = read_curve_points(points_xyzr, count, types, nullptr, nullptr, &control);
    if (r != CLAY_OK) return r;

    cut::CutShape shape = cut::CutShape::from_curve(control, tolerance);
    const std::size_t needed = shape.polygon.size();
    if (!out_xy) {
        *out_count = needed;
        return CLAY_OK;
    }
    if (*out_count < needed) {
        *out_count = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the outline needs " + std::to_string(needed) + " vertices");
    }
    for (std::size_t i = 0; i < needed; ++i) {
        out_xy[i * 2 + 0] = shape.polygon[i].x;
        out_xy[i * 2 + 1] = shape.polygon[i].y;
    }
    *out_count = needed;
    return CLAY_OK;
}

// -- brush strokes (c-abi spec: the stroke engine) ---------------------------

namespace {

clay_stroke_preset preset_fields(const brush::StrokePreset& d) {
    clay_stroke_preset filled{};
    filled.radius = d.radius;
    filled.spacing = d.spacing;
    filled.strength = d.strength;
    filled.pressure_size = d.pressure.size;
    filled.pressure_strength = d.pressure.strength;
    filled.pressure_curve = d.pressure.curve;
    filled.jitter_position = d.jitter_position;
    filled.jitter_size = d.jitter_size;
    filled.jitter_rotation = d.jitter_rotation;
    filled.seed = d.seed;
    filled.rotate_along_stroke = d.rotate_along_stroke ? 1 : 0;
    filled.taper_start = d.taper_start;
    filled.taper_end = d.taper_end;
    filled.steady = d.steady;
    filled.accumulation = static_cast<std::int32_t>(d.accumulation);
    return filled;
}

// Both entry points that hand a caller a whole preset: probe the size the
// caller declared, fill a local, write back bounded.
clay_result write_preset(clay_stroke_preset* out, const brush::StrokePreset& src) {
    if (!out) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset");
    clay_stroke_preset probe;
    clay_result r = read_desc(out, kStrokePresetOriginal, &probe);
    if (r != CLAY_OK) return r;
    write_desc(out, out->struct_size, preset_fields(src));
    return CLAY_OK;
}

}  // namespace

// A defaults call is an OUTPUT descriptor like any other, so the caller
// declares its size going IN. It used to set struct_size itself, which reads
// as a convenience and is really the one case the prefix rule cannot cover:
// with nothing declared there is no size to bound against, so the fill was
// always sizeof as THIS build defines it. See clay_brick_config_defaults.
clay_result clay_stroke_preset_defaults(clay_stroke_preset* out_preset) {
    return write_preset(out_preset, brush::StrokePreset{});
}

uint32_t clay_stroke_preset_version(void) { return brush::kPresetVersion; }

clay_result clay_stroke_preset_serialize(const clay_stroke_preset* preset, uint8_t* out_data,
                                         size_t* count) {
    brush::StrokePreset p;
    clay_result r = read_preset(preset, &p);
    if (r != CLAY_OK) return r;
    std::vector<std::uint8_t> bytes = p.serialize();
    return write_sized(bytes.data(), bytes.size(), out_data, count, "preset");
}

clay_result clay_stroke_preset_deserialize(const uint8_t* data, size_t size,
                                           clay_stroke_preset* out_preset) {
    if (!data || size == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or empty preset data");
    auto p = brush::StrokePreset::deserialize(data, size);
    if (!p)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "not a preset this build can read: malformed, or written by a schema "
                    "version newer than " +
                        std::to_string(brush::kPresetVersion));
    // Also an output descriptor, and one the earlier sweep missed because it
    // delegated its fill to the defaults call rather than spelling out
    // `*out = clay_stroke_preset{}` the way the other sites did.
    return write_preset(out_preset, *p);
}

clay_result clay_stroke_resolve(const float* samples_xyzpt, size_t sample_count,
                                const clay_stroke_preset* preset, clay_stamp* out_stamps,
                                size_t* count) {
    // Sugar over the wider form: the flat count*5 packing widened in place
    // would change the stride under every host already compiled against it.
    // The three new channels default to zero, which is exactly the stroke this
    // resolved before — a preset asking for neither the barrel nor a speed
    // response ignores all three.
    if (sample_count > 0 && !samples_xyzpt)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null stroke samples");
    clay_result r = check_batch("stroke samples", sample_count);
    if (r != CLAY_OK) return r;
    std::vector<clay_stroke_sample_full> wide(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const float* row = samples_xyzpt + i * 5;
        wide[i] = clay_stroke_sample_full{};
        wide[i].position[0] = row[0];
        wide[i].position[1] = row[1];
        wide[i].position[2] = row[2];
        wide[i].pressure = row[3];
        wide[i].tilt = row[4];
    }
    return clay_stroke_resolve_full(wide.empty() ? nullptr : wide.data(), sample_count, preset,
                                    out_stamps, count);
}

clay_result clay_stroke_resolve_full(const clay_stroke_sample_full* samples_in,
                                     size_t sample_count, const clay_stroke_preset* preset,
                                     clay_stamp* out_stamps, size_t* count) {
    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset p;
    clay_result r = read_stroke_full(samples_in, sample_count, preset, &samples, &p);
    if (r != CLAY_OK) return r;
    std::vector<brush::Stamp> stamps = brush::resolve_stroke(samples, p);

    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    if (!out_stamps) {
        *count = stamps.size();
        return CLAY_OK;
    }
    if (*count < stamps.size()) {
        *count = stamps.size();
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the stroke needs " + std::to_string(stamps.size()) + " stamps");
    }
    for (std::size_t i = 0; i < stamps.size(); ++i) out_stamps[i] = to_c_stamp(stamps[i]);
    *count = stamps.size();
    return CLAY_OK;
}

clay_result clay_voxel_apply_stroke(clay_voxel_grid* grid, const float* samples_xyzpt,
                                    size_t sample_count, const clay_stroke_preset* preset,
                                    int32_t index, int32_t shape, int32_t falloff,
                                    const clay_mask* mask, size_t* out_applied) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    std::uint8_t slot = 0;
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    if (!brush_shape_is_known(shape))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown brush shape: " + std::to_string(shape));
    if (!brush_falloff_is_known(falloff))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown brush falloff: " + std::to_string(falloff));

    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset p;
    r = read_stroke(samples_xyzpt, sample_count, preset, &samples, &p);
    if (r != CLAY_OK) return r;

    voxel::MaskField* m = nullptr;
    if (mask) {
        r = resolve_mask(mask, &m);
        if (r != CLAY_OK) return r;
    }
    std::size_t applied =
        brush::apply_to_grid(*g, brush::resolve_stroke(samples, p), slot,
                             static_cast<voxel::BrushShape>(shape),
                             static_cast<voxel::BrushFalloff>(falloff), m);
    if (out_applied) *out_applied = applied;
    return CLAY_OK;
}

// -- masks (c-abi spec: the mask field) --------------------------------------

clay_mask* clay_mask_create(float cell_size) {
    if (!(cell_size > 0.0f)) {  // also rejects NaN
        fail(CLAY_ERROR_INVALID_ARGUMENT, "cell_size must be > 0");
        return nullptr;
    }
    auto* handle = new clay_mask();
    handle->owned = new voxel::MaskField(cell_size);
    return handle;
}

clay_result clay_mask_destroy(clay_mask* mask) {
    if (!mask) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mask");
    if (!mask->owned)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this mask belongs to a document layer: the document owns it");
    delete mask->owned;
    delete mask;
    return CLAY_OK;
}

clay_result clay_document_add_mask(clay_document* doc, clay_layer_id layer, float cell_size,
                                   clay_mask** out_mask) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!(cell_size > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "cell_size must be > 0");
    if (!doc->doc.document.find_layer(layer))
        return fail(CLAY_ERROR_NOT_FOUND, "no layer with id " + std::to_string(layer));
    doc->doc.masks.insert_or_assign(layer, voxel::MaskField(cell_size));
    if (out_mask) *out_mask = borrow_mask_handle(doc, layer);
    return CLAY_OK;
}

clay_result clay_document_mask(clay_document* doc, clay_layer_id layer, clay_mask** out_mask) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->doc.masks.count(layer))
        return fail(CLAY_ERROR_NOT_FOUND, "layer " + std::to_string(layer) + " has no mask");
    if (out_mask) *out_mask = borrow_mask_handle(doc, layer);
    return CLAY_OK;
}

clay_result clay_document_remove_mask(clay_document* doc, clay_layer_id layer) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (doc->doc.masks.erase(layer) == 0)
        return fail(CLAY_ERROR_NOT_FOUND, "layer " + std::to_string(layer) + " has no mask");
    return CLAY_OK;
}

clay_result clay_mask_cell_size(const clay_mask* mask, float* out_cell_size) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (out_cell_size) *out_cell_size = m->cell_size();
    return CLAY_OK;
}

clay_result clay_mask_painted_count(const clay_mask* mask, size_t* out_count) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (out_count) *out_count = m->painted_count();
    return CLAY_OK;
}

clay_result clay_mask_empty(const clay_mask* mask, int32_t* out_empty) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (out_empty) *out_empty = m->empty() ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mask_get(const clay_mask* mask, const int32_t cell[3], float* out_value) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (!cell) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    if (out_value) *out_value = m->get({cell[0], cell[1], cell[2]});
    return CLAY_OK;
}

clay_result clay_mask_set(clay_mask* mask, const int32_t cell[3], float value) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (!cell) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    m->set({cell[0], cell[1], cell[2]}, value);
    return CLAY_OK;
}

clay_result clay_mask_sample(const clay_mask* mask, const float point[3], float* out_value) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (!point) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null point");
    if (out_value) *out_value = m->sample(kernel::cf3(point[0], point[1], point[2]));
    return CLAY_OK;
}

clay_result clay_mask_sample_many(const clay_mask* mask, const float* points_xyz, size_t count,
                                  float* out_values) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (count > 0 && (!points_xyz || !out_values))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null points or output");
    clay_result batch = check_batch("points", count);
    if (batch != CLAY_OK) return batch;
    for (std::size_t i = 0; i < count; ++i)
        out_values[i] = m->sample(
            kernel::cf3(points_xyz[i * 3 + 0], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]));
    return CLAY_OK;
}

clay_result clay_mask_paint(clay_mask* mask, const float point[3],
                            const clay_brush_params* brush, float target) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (!point) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null point");
    if (!brush) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brush parameters");
    voxel::BrushParams p;
    r = read_brush(brush, &p);
    if (r != CLAY_OK) return r;
    p.mask = nullptr;  // a mask does not gate itself
    m->paint(kernel::cf3(point[0], point[1], point[2]), p, target);
    return CLAY_OK;
}

clay_result clay_mask_paint_cell(clay_mask* mask, const int32_t cell[3],
                                 const clay_brush_params* brush, float target) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (!cell) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    if (!brush) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brush parameters");
    voxel::BrushParams p;
    r = read_brush(brush, &p);
    if (r != CLAY_OK) return r;
    p.mask = nullptr;
    m->paint(voxel::VoxelCoord{cell[0], cell[1], cell[2]}, p, target);
    return CLAY_OK;
}

clay_result clay_mask_invert(clay_mask* mask) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    m->invert();
    return CLAY_OK;
}

clay_result clay_mask_clear(clay_mask* mask) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    m->clear();
    return CLAY_OK;
}

// steps and iterations must be > 0: a zero or negative count is a
// zero-initialized argument rather than a request, on the same footing as a
// brush strength that is not > 0.
clay_result clay_mask_expand(clay_mask* mask, int32_t steps) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (steps <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "steps must be > 0");
    m->expand(steps);
    return CLAY_OK;
}

clay_result clay_mask_contract(clay_mask* mask, int32_t steps) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (steps <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "steps must be > 0");
    m->contract(steps);
    return CLAY_OK;
}

clay_result clay_mask_smooth(clay_mask* mask, int32_t iterations) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (iterations <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "iterations must be > 0");
    m->smooth(iterations);
    return CLAY_OK;
}

clay_result clay_mask_bounds(const clay_mask* mask, int32_t out_min[3], int32_t out_max[3],
                             int32_t* out_has_bounds) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    std::optional<voxel::VoxelCoord> lo = m->bounds_min();
    std::optional<voxel::VoxelCoord> hi = m->bounds_max();
    if (out_has_bounds) *out_has_bounds = (lo && hi) ? 1 : 0;
    if (!lo || !hi) return CLAY_OK;
    if (out_min) {
        out_min[0] = lo->x;
        out_min[1] = lo->y;
        out_min[2] = lo->z;
    }
    if (out_max) {
        out_max[0] = hi->x;
        out_max[1] = hi->y;
        out_max[2] = hi->z;
    }
    return CLAY_OK;
}

namespace {

// The bounded region both forms take. Refused rather than clamped when it is
// empty or unbounded: the cost of the operation is the box's volume in cells,
// so an infinite one is not a request that can be honoured.
clay_result read_box(const voxel::MaskField& mask, const float box_min[3],
                     const float box_max[3], math::Aabb* out) {
    if (!box_min || !box_max) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null box");
    *out = math::Aabb{kernel::cf3(box_min[0], box_min[1], box_min[2]),
                      kernel::cf3(box_max[0], box_max[1], box_max[2])};
    if (out->empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "box_min must not exceed box_max on any axis");
    // Typed here rather than silently doing nothing, which is what the engine
    // does with a region it cannot walk: a C caller has no other way to tell
    // "the box was empty" from "the box was too big to mean anything".
    if (!mask.region_is_walkable(*out))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this box spans more mask cells than can be walked; it is unbounded or a "
                    "coordinate is wrong");
    return CLAY_OK;
}

}  // namespace

clay_result clay_mask_fill(clay_mask* mask, const float box_min[3], const float box_max[3],
                           float value) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    math::Aabb box;
    r = read_box(*m, box_min, box_max, &box);
    if (r != CLAY_OK) return r;
    m->fill(box, value);
    return CLAY_OK;
}

clay_result clay_mask_invert_within(clay_mask* mask, const float box_min[3],
                                    const float box_max[3]) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    math::Aabb box;
    r = read_box(*m, box_min, box_max, &box);
    if (r != CLAY_OK) return r;
    m->invert_within(box);
    return CLAY_OK;
}

clay_result clay_mask_apply_stroke(clay_mask* mask, const float* samples_xyzpt,
                                   size_t sample_count, const clay_stroke_preset* preset,
                                   float target, int32_t shape, int32_t falloff,
                                   size_t* out_applied) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    MaskStep mask_step(mask, m);
    if (!brush_shape_is_known(shape))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown brush shape: " + std::to_string(shape));
    if (!brush_falloff_is_known(falloff))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown brush falloff: " + std::to_string(falloff));

    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset p;
    r = read_stroke(samples_xyzpt, sample_count, preset, &samples, &p);
    if (r != CLAY_OK) return r;

    std::size_t applied = brush::apply_to_mask(*m, brush::resolve_stroke(samples, p), target,
                                               static_cast<voxel::BrushShape>(shape),
                                               static_cast<voxel::BrushFalloff>(falloff));
    if (out_applied) *out_applied = applied;
    return CLAY_OK;
}

// -- mask extrude (c-abi spec: mask extrude) ---------------------------------

namespace {

clay_result read_extrude(const clay_mask_extrude_params* params, brush::MaskExtrudeSettings* out) {
    if (!params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null extrude parameters");
    clay_mask_extrude_params p;
    clay_result r = read_desc(params, kMaskExtrudeParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (!(p.thickness > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "thickness must be > 0");
    if (p.side < CLAY_EXTRUDE_OUTWARD || p.side > CLAY_EXTRUDE_CENTRED)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown extrude side: " + std::to_string(p.side));
    out->thickness = p.thickness;
    out->side = static_cast<brush::ExtrudeSide>(p.side);
    out->threshold = p.threshold > 0.0f ? p.threshold : 0.5f;
    out->border_round = p.border_round > 0.0f ? p.border_round : 0.0f;
    out->border_smooth = p.border_smooth > 0 ? p.border_smooth : 0;
    out->cell_size = p.cell_size > 0.0f ? p.cell_size : 0.0f;
    out->band = p.band > 0.0f ? p.band : 0.0f;
    return CLAY_OK;
}

// One message for every way an extrude can come back with nothing, because from
// here they are one thing: the caller asked for a solid that does not exist.
clay_result no_extract() {
    return fail(CLAY_ERROR_INVALID_ARGUMENT,
                "nothing to extrude: the mask is empty, does not reach the surface, or the wall "
                "is thinner than a cell");
}

}  // namespace

clay_result clay_mask_to_field(const clay_mask* mask, float threshold, float band, float pad,
                               float cell_size, clay_item** out_item) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (!out_item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_item");
    std::optional<field::FieldVolume> volume = brush::mask_to_field(
        *m, threshold > 0.0f ? threshold : 0.5f, band > 0.0f ? band : 0.0f,
        pad > 0.0f ? pad : 0.0f, cell_size > 0.0f ? cell_size : 0.0f);
    if (!volume)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "nothing painted at or above the threshold: there is no region to measure");
    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(std::move(*volume));
    *out_item = item;
    return CLAY_OK;
}

// -- measuring the surface, bounded rays, cage projection (add-claycore-bridge)

clay_result clay_measure_defaults(clay_measure_params* out_params) {
    if (!out_params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_params");
    clay_measure_params probe;
    clay_result r = read_desc(out_params, kMeasureParamsOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_params->struct_size;
    const brush::MeasureSettings d;
    clay_measure_params filled{};
    filled.h = d.h;
    filled.scale = d.scale;
    filled.direction[0] = d.direction.x;
    filled.direction[1] = d.direction.y;
    filled.direction[2] = d.direction.z;
    filled.threshold = d.threshold;
    filled.ray_length = d.ray_length;
    filled.ray_count = d.ray_count;
    filled.falloff = d.falloff;
    filled.seed = d.seed;
    write_desc(out_params, declared, filled);
    return CLAY_OK;
}

clay_result clay_measure_points(const clay_document* doc, clay_surface_measure measure,
                                const float* points_xyz, size_t count,
                                const clay_measure_params* params, float* out_values,
                                clay_cancel_token* token) {
    if (!doc || (count > 0 && (!points_xyz || !out_values)))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (count == 0) return CLAY_OK;  // no points is no work, not a rejected query
    brush::MeasureSettings settings;
    clay_result r = read_measure_params(params, &settings);
    if (r != CLAY_OK) return r;
    brush::SurfaceMeasure m;
    r = to_measure(measure, &m);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    if (tape.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty document");
    auto field = [&tape](kernel::cfloat3 p) { return tape.eval(p).d; };

    std::vector<kernel::cfloat3> pts(count);
    for (std::size_t i = 0; i < count; ++i)
        pts[i] = kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]);

    bool cancelled = false;
    brush::measure_points(field, m, pts.data(), count, settings, out_values,
                          token ? &token->token : nullptr, &cancelled);
    if (cancelled) return fail(CLAY_ERROR_CANCELLED, "measurement cancelled");
    return CLAY_OK;
}

clay_result clay_mask_from_surface(const clay_document* doc, clay_surface_measure measure,
                                   const float region_min[3], const float region_max[3],
                                   float cell_size, float band,
                                   const clay_measure_params* params, clay_mask** out_mask,
                                   clay_cancel_token* token) {
    if (!doc || !region_min || !region_max || !out_mask)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mask = nullptr;
    brush::MeasureSettings settings;
    clay_result r = read_measure_params(params, &settings);
    if (r != CLAY_OK) return r;
    brush::SurfaceMeasure m;
    r = to_measure(measure, &m);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    if (tape.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty document");
    auto field = [&tape](kernel::cfloat3 p) { return tape.eval(p).d; };

    brush::ProceduralMaskSettings ps;
    ps.cell_size = cell_size;
    ps.band = band;
    ps.region = math::Aabb{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                           kernel::cf3(region_max[0], region_max[1], region_max[2])};
    ps.measure = settings;
    if (ps.region.empty() || ps.region.is_infinite())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "region must be bounded and non-empty");

    bool cancelled = false;
    voxel::MaskField built =
        brush::mask_from_surface(field, m, ps, token ? &token->token : nullptr, &cancelled);
    if (cancelled) return fail(CLAY_ERROR_CANCELLED, "measurement cancelled");
    // An EMPTY mask is returned rather than refused: "the region contained no
    // surface" is an answer, and a caller can ask painted_count. A cancel is
    // the error, which is how the two are told apart — both come back empty.
    auto* handle = new clay_mask();
    handle->owned = new voxel::MaskField(std::move(built));
    *out_mask = handle;
    return CLAY_OK;
}

clay_result clay_raycast_bounded(const clay_document* doc, const float origin[3],
                                 const float dir[3], float tmin, float tmax, int32_t* out_hit,
                                 float* out_t, float out_position[3], float out_normal[3]) {
    if (!doc || !origin || !dir || !out_hit)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (!(tmax > tmin)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "tmax must exceed tmin");
    *out_hit = 0;
    std::shared_ptr<const scene::Tape> tape_ref = doc->pickable_tape();
    const scene::Tape& tape = *tape_ref;
    if (tape.empty()) return CLAY_OK;  // nothing to hit is not an error

    pick::RaycastOptions opts;
    opts.tmin = tmin;
    opts.tmax = tmax;
    if (doc->doc.groups) opts.groups = &*doc->doc.groups;
    const math::Ray ray{kernel::cf3(origin[0], origin[1], origin[2]),
                        kernel::cnormalize(kernel::cf3(dir[0], dir[1], dir[2]))};
    const pick::SceneHit hit = pick::raycast_scene(doc->doc.document, ray, opts);
    *out_hit = hit.hit ? 1 : 0;
    if (out_t) *out_t = hit.t;
    if (out_position) {
        out_position[0] = hit.position.x;
        out_position[1] = hit.position.y;
        out_position[2] = hit.position.z;
    }
    if (out_normal) {
        out_normal[0] = hit.normal.x;
        out_normal[1] = hit.normal.y;
        out_normal[2] = hit.normal.z;
    }
    return CLAY_OK;
}

clay_result clay_project_to_surface(const clay_document* doc, const float point[3],
                                    const float direction[3], float max_distance,
                                    clay_projection* out_projection) {
    if (!doc || !point || !direction || !out_projection)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    clay_projection probe;
    clay_result r = read_desc(out_projection, kProjectionOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_projection->struct_size;
    if (!(max_distance > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "max_distance must be positive");

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const pick::Projection p = pick::project_to_surface(
        *tape_ref, kernel::cf3(point[0], point[1], point[2]),
        kernel::cf3(direction[0], direction[1], direction[2]), max_distance);

    clay_projection filled{};
    filled.hit = p.hit ? 1 : 0;
    filled.distance = p.distance;
    filled.position[0] = p.position.x;
    filled.position[1] = p.position.y;
    filled.position[2] = p.position.z;
    filled.normal[0] = p.normal.x;
    filled.normal[1] = p.normal.y;
    filled.normal[2] = p.normal.z;
    write_desc(out_projection, declared, filled);
    return CLAY_OK;
}

clay_result clay_project_to_surface_many(const clay_document* doc, const float* points_xyz,
                                         const float* directions_xyz, size_t count,
                                         float max_distance, int32_t* out_hits,
                                         float* out_distances, float* out_positions_xyz,
                                         float* out_normals_xyz, clay_cancel_token* token) {
    if (!doc || (count > 0 && (!points_xyz || !directions_xyz || !out_hits)))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (count == 0) return CLAY_OK;
    if (!(max_distance > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "max_distance must be positive");

    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    parallel::CancelToken* tok = token ? &token->token : nullptr;
    // ONE PHASE, not one per point. The second argument is a PHASE COUNT —
    // passing the work count both narrows size_t to uint32_t (which MSVC /WX
    // rejects and GCC accepts silently) and means the wrong thing: the
    // per-item figure is what advance() carries.
    parallel::ProgressScope progress(tok, 1);
    std::atomic<bool> stop{false};

    // A cancelled chunk RETURNS NORMALLY and never throws: thread_pool.h's join
    // waits for `done >= num_tasks` and increments only after fn returns, so a
    // throw here would hang the join forever.
    parallel::for_range(count, 64, [&](std::size_t first, std::size_t last) {
        if (stop.load(std::memory_order_relaxed)) return;
        if (parallel::cancelled(tok)) {
            stop.store(true, std::memory_order_relaxed);
            return;
        }
        for (std::size_t i = first; i < last; ++i) {
            const pick::Projection p = pick::project_to_surface(
                tape, kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]),
                kernel::cf3(directions_xyz[i * 3], directions_xyz[i * 3 + 1],
                            directions_xyz[i * 3 + 2]),
                max_distance);
            out_hits[i] = p.hit ? 1 : 0;
            if (out_distances) out_distances[i] = p.distance;
            if (out_positions_xyz) {
                out_positions_xyz[i * 3] = p.position.x;
                out_positions_xyz[i * 3 + 1] = p.position.y;
                out_positions_xyz[i * 3 + 2] = p.position.z;
            }
            if (out_normals_xyz) {
                out_normals_xyz[i * 3] = p.normal.x;
                out_normals_xyz[i * 3 + 1] = p.normal.y;
                out_normals_xyz[i * 3 + 2] = p.normal.z;
            }
        }
        progress.advance(last, static_cast<float>(last) / static_cast<float>(count));
    });
    if (stop.load(std::memory_order_relaxed))
        return fail(CLAY_ERROR_CANCELLED, "projection cancelled");
    return CLAY_OK;
}

// -- surface groups (add-surface-groups) --------------------------------------

clay_result clay_document_groups(clay_document* doc, float cell_size, clay_groups** out_groups) {
    if (!doc || !out_groups) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_groups = nullptr;
    if (!doc->doc.groups) {
        const float size = cell_size > 0.0f ? cell_size : 0.05f;
        if (!std::isfinite(size)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "cell size must be finite");
        doc->doc.groups.emplace(size);
    }
    // An EXISTING lattice is not re-scaled, whatever cell_size says: re-scaling
    // would move every boundary the artist placed, and silently.
    auto* handle = new clay_groups();
    handle->doc = doc;
    *out_groups = handle;
    return CLAY_OK;
}

clay_result clay_document_has_groups(const clay_document* doc, int32_t* out_has) {
    if (!doc || !out_has) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_has = doc->doc.groups && !doc->doc.groups->empty() ? 1 : 0;
    return CLAY_OK;
}

void clay_groups_destroy(clay_groups* groups) { delete groups; }

clay_result clay_groups_at(const clay_groups* groups, const float world_p[3], uint16_t* out_group) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !world_p || !out_group) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_group = g->at(kernel::cf3(world_p[0], world_p[1], world_p[2]));
    return CLAY_OK;
}

clay_result clay_groups_fill(clay_groups* groups, const float box_min[3], const float box_max[3],
                             uint16_t group) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g || !box_min || !box_max) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    GroupStep step(groups);
    g->fill(math::Aabb{kernel::cf3(box_min[0], box_min[1], box_min[2]),
                       kernel::cf3(box_max[0], box_max[1], box_max[2])},
            group);
    return CLAY_OK;
}

clay_result clay_groups_fill_from_mask(clay_groups* groups, const clay_mask* mask, uint16_t group,
                                       float threshold, uint64_t* out_cells) {
    voxel::GroupField* g = group_field_mut(groups);
    const voxel::MaskField* m = mask_of(mask);
    if (!g || !m) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    GroupStep step(groups);
    const std::size_t n = g->fill_from_mask(*m, group, threshold);
    if (out_cells) *out_cells = n;
    return CLAY_OK;
}

clay_result clay_groups_reassign(clay_groups* groups, uint16_t from, uint16_t to,
                                 uint64_t* out_moved) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    const std::size_t n = g->reassign(from, to);
    if (out_moved) *out_moved = n;
    return CLAY_OK;
}

clay_result clay_groups_grow(clay_groups* groups, uint16_t group, int32_t steps,
                             uint64_t* out_claimed) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    const std::size_t n = g->grow(group, steps);
    if (out_claimed) *out_claimed = n;
    return CLAY_OK;
}

clay_result clay_groups_shrink(clay_groups* groups, uint16_t group, int32_t steps,
                               uint64_t* out_released) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    const std::size_t n = g->shrink(group, steps);
    if (out_released) *out_released = n;
    return CLAY_OK;
}

clay_result clay_groups_border(const clay_groups* groups, uint16_t group, int32_t* out_cells_xyz,
                               size_t capacity, size_t* out_count) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    const std::vector<voxel::VoxelCoord> rim = g->border(group);
    *out_count = rim.size();
    if (!out_cells_xyz) return CLAY_OK;  // a count query
    const std::size_t n = rim.size() < capacity ? rim.size() : capacity;
    for (std::size_t i = 0; i < n; ++i) {
        out_cells_xyz[i * 3] = rim[i].x;
        out_cells_xyz[i * 3 + 1] = rim[i].y;
        out_cells_xyz[i * 3 + 2] = rim[i].z;
    }
    *out_count = n;
    return CLAY_OK;
}

clay_result clay_groups_set_visible(clay_groups* groups, uint16_t group, int32_t visible) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    g->set_visible(group, visible != 0);
    return CLAY_OK;
}

clay_result clay_groups_visible(const clay_groups* groups, uint16_t group, int32_t* out_visible) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_visible) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_visible = g->visible(group) ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_groups_isolate(clay_groups* groups, uint16_t group) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    g->isolate(group);
    return CLAY_OK;
}

clay_result clay_groups_show_all(clay_groups* groups) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    g->show_all();
    return CLAY_OK;
}

clay_result clay_groups_invert_visibility(clay_groups* groups) {
    voxel::GroupField* g = group_field_mut(groups);
    if (!g) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null groups");
    GroupStep step(groups);
    g->invert_visibility();
    return CLAY_OK;
}

clay_result clay_groups_any_hidden(const clay_groups* groups, int32_t* out_any) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_any) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_any = g->any_hidden() ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_groups_point_hidden(const clay_groups* groups, const float world_p[3],
                                     int32_t* out_hidden) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !world_p || !out_hidden) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_hidden = g->point_hidden(kernel::cf3(world_p[0], world_p[1], world_p[2])) ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_groups_ids(const clay_groups* groups, uint16_t* out_ids, size_t capacity,
                            size_t* out_count) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    const std::vector<voxel::GroupId> ids = g->ids();
    *out_count = ids.size();
    if (!out_ids) return CLAY_OK;
    const std::size_t n = ids.size() < capacity ? ids.size() : capacity;
    for (std::size_t i = 0; i < n; ++i) out_ids[i] = ids[i];
    *out_count = n;
    return CLAY_OK;
}

clay_result clay_groups_cell_size(const clay_groups* groups, float* out_cell_size) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_cell_size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_cell_size = g->cell_size();
    return CLAY_OK;
}

clay_result clay_groups_empty(const clay_groups* groups, int32_t* out_empty) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_empty) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_empty = g->empty() ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_groups_cell_count(const clay_groups* groups, uint16_t group, uint64_t* out_cells) {
    const voxel::GroupField* g = group_field(groups);
    if (!g || !out_cells) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_cells = group == CLAY_NO_GROUP ? g->cell_count() : g->cell_count(group);
    return CLAY_OK;
}

clay_result clay_document_mask_extrude(clay_document* doc, clay_layer_id layer,
                                       const clay_mask* mask,
                                       const clay_mask_extrude_params* params,
                                       clay_item** out_item) {
    // Sugar over the cancellable form with no token, so there is one
    // implementation rather than two that could drift.
    return clay_document_mask_extrude_cancellable(doc, layer, mask, params, out_item, nullptr);
}

clay_result clay_document_mask_extrude_cancellable(clay_document* doc, clay_layer_id layer,
                                       const clay_mask* mask,
                                       const clay_mask_extrude_params* params,
                                       clay_item** out_item,
                                                   clay_cancel_token* token) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!out_item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_item");
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    brush::MaskExtrudeSettings settings;
    r = read_extrude(params, &settings);
    if (r != CLAY_OK) return r;

    scene::Tape tape;
    r = compile_one_layer(doc, layer, &tape);
    if (r != CLAY_OK) return r;
    // The source is the layer's own field rather than a volume, so it stays
    // EXACT: a volume reports a bound outside its band, and sampling one would
    // record the boundary between bound and distance as part of the shape.
    if (tape.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this layer has no field to extrude from");

    std::optional<field::FieldVolume> volume = brush::mask_extrude(
        [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, *m, settings,
        token ? &token->token : nullptr);
    // A cancel and "the mask never reached the surface" both come back as
    // nullopt, and a host must not be shown the second when the user did the
    // first.
    if (!volume) {
        if (token && token->token.cancelled())
            return fail(CLAY_ERROR_CANCELLED, "the mask extrude was cancelled");
        return no_extract();
    }

    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<field::FieldVolume>(std::move(*volume));
    *out_item = item;
    return CLAY_OK;
}

clay_result clay_voxel_mask_extrude(const clay_voxel_grid* grid, const clay_mask* mask,
                                    const clay_mask_extrude_params* params,
                                    clay_voxel_grid** out_grid) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    voxel::MaskField* m = nullptr;
    r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (!out_grid) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_grid");
    brush::MaskExtrudeSettings settings;
    r = read_extrude(params, &settings);
    if (r != CLAY_OK) return r;

    std::optional<voxel::VoxelGrid> extract = brush::mask_extrude(*g, *m, settings);
    if (!extract) return no_extract();

    // Owned by the caller, on the same rule clay_voxel_grid_create follows: a
    // handle a document does not hold is one the caller destroys.
    auto* handle = new clay_voxel_grid();
    handle->owned = new voxel::VoxelGrid(std::move(*extract));
    *out_grid = handle;
    return CLAY_OK;
}

clay_result clay_voxel_size(const clay_voxel_grid* grid, float* out_voxel_size) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_voxel_size) *out_voxel_size = g->voxel_size();
    return CLAY_OK;
}

clay_result clay_voxel_level_count(const clay_voxel_grid* grid, size_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_count) *out_count = g->level_count();
    return CLAY_OK;
}

clay_result clay_voxel_active_level(const clay_voxel_grid* grid, size_t* out_level) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_level) *out_level = g->active_level();
    return CLAY_OK;
}

clay_result clay_voxel_set_active_level(clay_voxel_grid* grid, size_t level) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!g->set_active_level(level))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "no such resolution level: " + std::to_string(level));
    return CLAY_OK;
}

clay_result clay_voxel_add_level(clay_voxel_grid* grid, size_t* out_level) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    std::size_t before = g->level_count();
    std::size_t level = g->add_level();
    if (g->level_count() == before)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the level stack is capped at " + std::to_string(voxel::VoxelGrid::kMaxLevels));
    if (out_level) *out_level = level;
    return CLAY_OK;
}

clay_result clay_voxel_add_level_region(clay_voxel_grid* grid, const float min[3],
                                        const float max[3], size_t* out_level) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!min || !max) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null region");
    math::Aabb region{kernel::cf3(min[0], min[1], min[2]), kernel::cf3(max[0], max[1], max[2])};
    if (region.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the region is empty; there is nothing to refine");
    std::size_t before = g->level_count();
    std::size_t level = g->add_level(region);
    if (g->level_count() == before)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the level stack is capped at " + std::to_string(voxel::VoxelGrid::kMaxLevels));
    if (out_level) *out_level = level;
    return CLAY_OK;
}

clay_result clay_voxel_level_chunk_count(const clay_voxel_grid* grid, size_t level,
                                         size_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    if (level >= g->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    *out_count = g->level_refined_chunk_count(level);
    return CLAY_OK;
}

clay_result clay_voxel_level_is_whole(const clay_voxel_grid* grid, size_t level,
                                      int32_t* out_whole) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!out_whole) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_whole");
    if (level >= g->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    *out_whole = g->level_is_whole(level) ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_voxel_drop_level(clay_voxel_grid* grid) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!g->drop_level())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a grid always has at least one level");
    return CLAY_OK;
}

// A level this grid does not have is an error rather than a zero, so a caller
// cannot read a stack's cost off by one and see a plausible-looking answer.
clay_result clay_voxel_level_voxel_size(const clay_voxel_grid* grid, size_t level,
                                        float* out_voxel_size) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (level >= g->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "no such resolution level: " + std::to_string(level));
    if (out_voxel_size) *out_voxel_size = g->level_voxel_size(level);
    return CLAY_OK;
}

clay_result clay_voxel_level_occupied_count(const clay_voxel_grid* grid, size_t level,
                                            size_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (level >= g->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "no such resolution level: " + std::to_string(level));
    if (out_count) *out_count = g->level_occupied_count(level);
    return CLAY_OK;
}

clay_result clay_voxel_palette_add(clay_voxel_grid* grid, const float rgb[3],
                                   int32_t* out_index) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!rgb) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null color");
    std::uint8_t index = g->palette_add(kernel::cf3(rgb[0], rgb[1], rgb[2]));
    if (out_index) *out_index = index;
    return CLAY_OK;
}

clay_result clay_voxel_palette_color(const clay_voxel_grid* grid, int32_t index,
                                     float out_rgb[3]) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    kernel::cfloat3 c = g->palette_color(slot);
    if (!out_rgb) return CLAY_OK;
    out_rgb[0] = c.x;
    out_rgb[1] = c.y;
    out_rgb[2] = c.z;
    return CLAY_OK;
}

clay_result clay_voxel_palette_set(clay_voxel_grid* grid, int32_t index, const float rgb[3]) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    if (!rgb) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null color");
    g->palette_set(slot, kernel::cf3(rgb[0], rgb[1], rgb[2]));
    return CLAY_OK;
}

clay_result clay_voxel_palette_size(const clay_voxel_grid* grid, size_t* out_size) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_size) *out_size = g->palette_size();
    return CLAY_OK;
}

clay_result clay_voxel_get(const clay_voxel_grid* grid, const int32_t cell[3],
                           int32_t* out_index) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_at(grid, cell, &g);
    if (r != CLAY_OK) return r;
    if (out_index) *out_index = g->get(to_coord(cell));
    return CLAY_OK;
}

clay_result clay_voxel_set(clay_voxel_grid* grid, const int32_t cell[3], int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, cell, index, &g, &slot);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->set(to_coord(cell), slot);
    return CLAY_OK;
}

clay_result clay_voxel_erase(clay_voxel_grid* grid, const int32_t cell[3]) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_at(grid, cell, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->erase(to_coord(cell));
    return CLAY_OK;
}

clay_result clay_voxel_paint(clay_voxel_grid* grid, const int32_t cell[3], int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, cell, index, &g, &slot);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->paint(to_coord(cell), slot);
    return CLAY_OK;
}

clay_result clay_voxel_set_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count,
                                int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_batch(grid, cells_xyz, count, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    for (size_t i = 0; i < count; ++i) g->set(to_coord(cells_xyz + i * 3), slot);
    return CLAY_OK;
}

clay_result clay_voxel_erase_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_batch(grid, cells_xyz, count, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    for (size_t i = 0; i < count; ++i) g->erase(to_coord(cells_xyz + i * 3));
    return CLAY_OK;
}

clay_result clay_voxel_fill_box(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, a, index, &g, &slot);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!b) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    g->fill_box(to_coord(a), to_coord(b), slot);
    return CLAY_OK;
}

clay_result clay_voxel_fill_line(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                 int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, a, index, &g, &slot);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!b) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    g->fill_line(to_coord(a), to_coord(b), slot);
    return CLAY_OK;
}

clay_result clay_voxel_set_mirrored(clay_voxel_grid* grid, const int32_t cell[3], int32_t index,
                                    int32_t axes) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    std::uint8_t mask = 0;
    clay_result r = resolve_at_index(grid, cell, index, &g, &slot);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    r = check_mirror_axes(axes, &mask);
    if (r != CLAY_OK) return r;
    g->set_mirrored(to_coord(cell), slot, mask);
    return CLAY_OK;
}

clay_result clay_voxel_paint_mirrored(clay_voxel_grid* grid, const int32_t cell[3], int32_t index,
                                      int32_t axes) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    std::uint8_t mask = 0;
    clay_result r = resolve_at_index(grid, cell, index, &g, &slot);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    r = check_mirror_axes(axes, &mask);
    if (r != CLAY_OK) return r;
    g->paint_mirrored(to_coord(cell), slot, mask);
    return CLAY_OK;
}

clay_result clay_voxel_set_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                 const clay_brush_params* brush, int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    std::uint8_t slot = 0;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    g->set_brush(to_coord(cell), p, slot);
    return CLAY_OK;
}

clay_result clay_voxel_erase_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->erase_brush(to_coord(cell), p);
    return CLAY_OK;
}

clay_result clay_voxel_paint_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush, int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    std::uint8_t slot = 0;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    g->paint_brush(to_coord(cell), p, slot);
    return CLAY_OK;
}

/* -- sculpt layers --------------------------------------------------------- */

namespace {

// Every accessor here needs the same two things, and the index check is the
// one a caller is most likely to get wrong.
clay_result resolve_sculpt_layer(const clay_voxel_grid* grid, std::size_t layer,
                                 voxel::VoxelGrid** out) {
    clay_result r = resolve(const_cast<clay_voxel_grid*>(grid), out);
    if (r != CLAY_OK) return r;
    if (layer >= (*out)->sculpt_layer_count())
        return fail(CLAY_ERROR_NOT_FOUND, "no sculpt layer " + std::to_string(layer));
    return CLAY_OK;
}

}  // namespace

clay_result clay_voxel_begin_sculpt_layer(clay_voxel_grid* grid, const char* name,
                                          size_t* out_layer) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (g->recording_sculpt_layer())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a sculpt layer is already recording");
    const std::size_t layer = g->begin_sculpt_layer(name ? std::string(name) : std::string());
    if (out_layer) *out_layer = layer;
    return CLAY_OK;
}

clay_result clay_voxel_end_sculpt_layer(clay_voxel_grid* grid) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    g->end_sculpt_layer();
    return CLAY_OK;
}

clay_result clay_voxel_recording_sculpt_layer(const clay_voxel_grid* grid, int32_t* out_recording) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(const_cast<clay_voxel_grid*>(grid), &g);
    if (r != CLAY_OK) return r;
    if (!out_recording) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_recording");
    *out_recording = g->recording_sculpt_layer() ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layer_count(const clay_voxel_grid* grid, size_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(const_cast<clay_voxel_grid*>(grid), &g);
    if (r != CLAY_OK) return r;
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    *out_count = g->sculpt_layer_count();
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layer_name(const clay_voxel_grid* grid, size_t layer, char* buffer,
                                         size_t* size) {
    if (!size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null size");
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    const std::string& name = g->sculpt_layer_name(layer);
    const std::size_t needed = name.size() + 1;
    if (!buffer) {
        *size = needed;
        return CLAY_OK;
    }
    if (*size < needed) {
        *size = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the name needs " + std::to_string(needed) + " bytes");
    }
    std::memcpy(buffer, name.c_str(), needed);
    *size = needed;
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layer_cell_count(const clay_voxel_grid* grid, size_t layer,
                                               size_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    *out_count = g->sculpt_layer_cell_count(layer);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layer_strength(const clay_voxel_grid* grid, size_t layer,
                                             float* out_strength) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    if (!out_strength) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_strength");
    *out_strength = g->sculpt_layer_strength(layer);
    return CLAY_OK;
}

clay_result clay_voxel_set_sculpt_layer_strength(clay_voxel_grid* grid, size_t layer,
                                                 float strength) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    g->set_sculpt_layer_strength(layer, strength);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layer_visible(const clay_voxel_grid* grid, size_t layer,
                                            int32_t* out_visible) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    if (!out_visible) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_visible");
    *out_visible = g->sculpt_layer_visible(layer) ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_voxel_set_sculpt_layer_visible(clay_voxel_grid* grid, size_t layer,
                                                int32_t visible) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    g->set_sculpt_layer_visible(layer, visible != 0);
    return CLAY_OK;
}

clay_result clay_voxel_remove_sculpt_layer(clay_voxel_grid* grid, size_t layer) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    g->remove_sculpt_layer(layer);
    return CLAY_OK;
}

clay_result clay_voxel_move_sculpt_layer(clay_voxel_grid* grid, size_t from, size_t to) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, from, &g);
    if (r != CLAY_OK) return r;
    if (to >= g->sculpt_layer_count())
        return fail(CLAY_ERROR_NOT_FOUND, "no sculpt layer " + std::to_string(to));
    g->move_sculpt_layer(from, to);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layer_bytes(const clay_voxel_grid* grid, size_t layer,
                                          size_t* out_bytes) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    if (!out_bytes) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_bytes");
    *out_bytes = g->sculpt_layer_bytes(layer);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_layers_bytes(const clay_voxel_grid* grid, size_t* out_bytes) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(const_cast<clay_voxel_grid*>(grid), &g);
    if (r != CLAY_OK) return r;
    if (!out_bytes) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_bytes");
    *out_bytes = g->sculpt_layer_total_bytes();
    return CLAY_OK;
}

clay_result clay_voxel_merge_sculpt_layer_down(clay_voxel_grid* grid, size_t layer) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_sculpt_layer(grid, layer, &g);
    if (r != CLAY_OK) return r;
    if (!g->merge_sculpt_layer_down(layer))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the bottom sculpt layer has nothing below it");
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_smooth(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->sculpt_smooth(to_coord(cell), p);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_inflate(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, int32_t amount) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->sculpt_inflate(to_coord(cell), p, amount);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_flatten(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, const float normal[3],
                                      float offset_cells) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!normal) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null normal");
    kernel::cfloat3 n = kernel::cf3(normal[0], normal[1], normal[2]);
    // The engine normalizes without checking, as the Python bindings do; a
    // zero-length normal would flatten every cell against a NaN plane, so it
    // is rejected here the way a plane primitive's is.
    if (kernel::cdot2(n) <= 0.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "flatten needs a non-zero normal");
    g->sculpt_flatten(to_coord(cell), p, n, offset_cells);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_pinch(clay_voxel_grid* grid, const int32_t cell[3],
                                    const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->sculpt_pinch(to_coord(cell), p);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_magnify(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->sculpt_magnify(to_coord(cell), p);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_grab(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush, const float displacement[3],
                                   int32_t front_only) {
    if (!displacement) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null displacement");
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    g->sculpt_grab(to_coord(cell), p,
                   kernel::cf3(displacement[0], displacement[1], displacement[2]),
                   front_only != 0);
    return CLAY_OK;
}

/* -- a grab as a gesture (issue #393) --------------------------------------- */

// The handle BORROWS the grid, as a voxel sculpt layer's does. It also holds
// the grid's C handle, because every write has to raise the same undo step and
// dirty-region bookkeeping a stateless verb does — a drag that skipped that
// would leave the host's caches serving the pre-drag material.
struct clay_voxel_grab_tx {
    clay_voxel_grid* handle = nullptr;
    voxel::VoxelGrid* grid = nullptr;
    std::optional<voxel::GrabTransaction> tx;
};

clay_voxel_grab_tx* clay_voxel_grab_begin(clay_voxel_grid* grid, const int32_t cell[3],
                                          const clay_brush_params* brush, int32_t front_only) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    if (resolve_brush(grid, cell, brush, &g, &p) != CLAY_OK) return nullptr;
    std::optional<voxel::GrabTransaction> tx =
        voxel::GrabTransaction::begin(*g, to_coord(cell), p, front_only != 0);
    if (!tx) return nullptr;
    auto* handle = new clay_voxel_grab_tx();
    handle->handle = grid;
    handle->grid = g;
    handle->tx = std::move(tx);
    return handle;
}

clay_result clay_voxel_grab_update(clay_voxel_grab_tx* tx, const float total_displacement[3]) {
    if (!tx || !tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or spent grab transaction");
    if (!total_displacement) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null displacement");
    VoxelStep step(tx->handle, tx->grid);
    tx->tx->update(kernel::cf3(total_displacement[0], total_displacement[1],
                               total_displacement[2]));
    return CLAY_OK;
}

clay_result clay_voxel_grab_written_box(const clay_voxel_grab_tx* tx, int32_t out_lo[3],
                                        int32_t out_hi[3]) {
    if (!tx || !tx->tx) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null grab transaction");
    const voxel::VoxelCoord lo = tx->tx->written_lo();
    const voxel::VoxelCoord hi = tx->tx->written_hi();
    if (out_lo) {
        out_lo[0] = lo.x;
        out_lo[1] = lo.y;
        out_lo[2] = lo.z;
    }
    if (out_hi) {
        out_hi[0] = hi.x;
        out_hi[1] = hi.y;
        out_hi[2] = hi.z;
    }
    return CLAY_OK;
}

clay_result clay_voxel_grab_live(const clay_voxel_grab_tx* tx, int32_t* out_live) {
    if (!out_live) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_live");
    *out_live = (tx && tx->tx && tx->tx->live()) ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_voxel_grab_commit(clay_voxel_grab_tx* tx) {
    if (!tx || !tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or spent grab transaction");
    tx->tx->commit();
    return CLAY_OK;
}

clay_result clay_voxel_grab_cancel(clay_voxel_grab_tx* tx) {
    if (!tx || !tx->tx || !tx->tx->live())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or spent grab transaction");
    // Restoring is a write like any other, so it raises a step too: undoing a
    // cancelled drag must not be a no-op that leaves the previous edit exposed.
    VoxelStep step(tx->handle, tx->grid);
    tx->tx->cancel();
    return CLAY_OK;
}

void clay_voxel_grab_destroy(clay_voxel_grab_tx* tx) {
    if (!tx) return;
    // An uncommitted drag is cancelled rather than left half-applied: a host
    // that drops the handle on an error path should not find a partial gesture
    // baked into the grid.
    if (tx->tx && tx->tx->live()) {
        VoxelStep step(tx->handle, tx->grid);
        tx->tx->cancel();
    }
    delete tx;
}

clay_result clay_voxel_sculpt_fill_cavities(clay_voxel_grid* grid, const int32_t cell[3],
                                            const clay_brush_params* brush, int32_t passes) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (passes <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "passes must be > 0");
    g->sculpt_fill_cavities(to_coord(cell), p, passes);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_scrape(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush, const float normal[3],
                                     float offset_cells) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!normal) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null normal");
    kernel::cfloat3 n = kernel::cf3(normal[0], normal[1], normal[2]);
    if (!(kernel::clength(n) >= 1e-12f))  // also rejects NaN
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "scrape normal must not be zero length");
    g->sculpt_scrape(to_coord(cell), p, n, offset_cells);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_smudge(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush,
                                     const float displacement[3]) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!displacement) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null displacement");
    g->sculpt_smudge(to_coord(cell), p,
                     kernel::cf3(displacement[0], displacement[1], displacement[2]));
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_carve_alpha(clay_voxel_grid* grid, const int32_t cell[3],
                                          const clay_brush_params* brush, const float* alpha,
                                          int32_t alpha_width, int32_t alpha_height,
                                          const float direction[3], int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!direction) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null direction");
    std::uint8_t slot = 0;
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    if (!g->sculpt_carve_alpha(to_coord(cell), p, alpha, alpha_width, alpha_height,
                               kernel::cf3(direction[0], direction[1], direction[2]), slot))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the alpha stamp is malformed: a null or empty grid, or a zero-length "
                    "direction");
    return CLAY_OK;
}

clay_result clay_voxel_repair_report(const clay_voxel_grid* grid,
                                     clay_repair_report* out_report) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!out_report) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null report");
    // The descriptor is an OUTPUT, so struct_size is the caller telling us how
    // much of it exists rather than what it filled in.
    clay_repair_report probe;
    r = read_desc(out_report, kRepairReportOriginal, &probe);
    if (r != CLAY_OK) return r;

    voxel::VoxelGrid::RepairReport report = g->repair_report();
    const std::uint32_t declared = out_report->struct_size;
    clay_repair_report filled{};
    filled.enclosed_voids = report.enclosed_voids;
    filled.void_cells = report.void_cells;
    filled.largest_void = report.largest_void;
    filled.airtight = report.airtight ? 1 : 0;
    write_desc(out_report, declared, filled);
    return CLAY_OK;
}

clay_result clay_voxel_repair_close_holes(clay_voxel_grid* grid, int32_t passes,
                                          const clay_mask* mask) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (passes <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "passes must be > 0");
    voxel::MaskField* m = nullptr;
    if (mask) {
        r = resolve_mask(mask, &m);
        if (r != CLAY_OK) return r;
    }
    g->repair_close_holes(passes, m);
    return CLAY_OK;
}

clay_result clay_voxel_repair_fill_voids(clay_voxel_grid* grid, const clay_mask* mask) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    voxel::MaskField* m = nullptr;
    if (mask) {
        r = resolve_mask(mask, &m);
        if (r != CLAY_OK) return r;
    }
    g->repair_fill_voids(m);
    return CLAY_OK;
}

clay_result clay_voxel_occupied_count(const clay_voxel_grid* grid, size_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_count) *out_count = g->occupied_count();
    return CLAY_OK;
}

clay_result clay_voxel_change_count(const clay_voxel_grid* grid, uint64_t* out_count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_count) *out_count = g->change_count();
    return CLAY_OK;
}

clay_result clay_voxel_bounds(const clay_voxel_grid* grid, int32_t out_min[3],
                              int32_t out_max[3], int32_t* out_has_bounds) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    std::optional<voxel::VoxelCoord> lo = g->bounds_min();
    std::optional<voxel::VoxelCoord> hi = g->bounds_max();
    if (out_has_bounds) *out_has_bounds = lo && hi ? 1 : 0;
    if (!lo || !hi) return CLAY_OK;
    if (out_min) std::memcpy(out_min, &*lo, sizeof(voxel::VoxelCoord));
    if (out_max) std::memcpy(out_max, &*hi, sizeof(voxel::VoxelCoord));
    return CLAY_OK;
}

clay_result clay_voxel_flood_select(const clay_voxel_grid* grid, const int32_t seed[3],
                                    int32_t same_color, int32_t* out_cells, size_t* count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_at(grid, seed, &g);
    if (r != CLAY_OK) return r;
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    std::vector<voxel::VoxelCoord> sel = g->flood_select(to_coord(seed), same_color != 0);
    std::size_t found = sel.size();
    if (out_cells && *count < found) {
        *count = found;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the selection needs " + std::to_string(found) + " cells");
    }
    if (out_cells && found > 0)
        std::memcpy(out_cells, sel.data(), found * sizeof(voxel::VoxelCoord));
    *count = found;
    return CLAY_OK;
}

clay_result clay_voxel_sample_step_field(const clay_voxel_grid* grid, const float* points_xyz,
                                         size_t count, float* out_values) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (count > 0 && (!points_xyz || !out_values))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    r = check_batch("points", count);
    if (r != CLAY_OK) return r;
    for (size_t i = 0; i < count; ++i)
        out_values[i] = g->sample_step_field(
            kernel::cf3(points_xyz[i * 3], points_xyz[i * 3 + 1], points_xyz[i * 3 + 2]));
    return CLAY_OK;
}

clay_result clay_voxel_mesh(const clay_voxel_grid* grid, clay_mesh** out_mesh) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out pointer");
    auto* handle = new clay_mesh();
    handle->data = g->mesh_greedy();
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_item_volume_from_voxels(const clay_voxel_grid* grid, int32_t blur, int32_t index,
                                         clay_item** out_item) {
    if (!out_item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out pointer");
    *out_item = nullptr;
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (blur < 0 || blur > 8) return fail(CLAY_ERROR_INVALID_ARGUMENT, "blur must be 0..8 passes");
    if (index < 0 || index > 255) return fail(CLAY_ERROR_INVALID_ARGUMENT, "index must be 0..255");

    std::optional<field::FieldVolume> volume = g->to_field(
        voxel::VoxelGrid::FieldOptions{blur, 0.0f, static_cast<std::uint8_t>(index)});
    if (!volume)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the grid holds nothing to convert at that palette index");

    auto* item = new clay_item();
    item->node.prim = scene::Prim::volume();
    item->node.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
    if (index > 0) item->node.color = g->palette_color(static_cast<std::uint8_t>(index));
    *out_item = item;
    return CLAY_OK;
}

clay_result clay_voxel_to_layer(clay_document* doc, const clay_voxel_grid* grid, const char* name,
                                int32_t blur, clay_layer_id* out_layer) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (blur < 0 || blur > 8)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "blur must be 0..8 passes");

    // ONE volume, carrying the palette per sample. This used to be one volume
    // per palette entry — the only way to keep colour when a field had nowhere
    // to store one — so a forty-entry sculpt became forty items and forty
    // volumes. A host that counted one node per entry now counts one.
    //
    // Converted BEFORE the layer exists, so a conversion that fails leaves the
    // document exactly as it was rather than an empty layer to clean up.
    std::optional<field::FieldVolume> volume =
        g->to_field(voxel::VoxelGrid::FieldOptions{blur, 0.0f, 0});
    if (!volume)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the grid could not be converted to a field");

    clay_layer_id layer = 0;
    r = clay_add_sdf_layer(doc, name, &layer);
    if (r != CLAY_OK) return r;

    scene::Node node;
    node.prim = scene::Prim::volume();
    node.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
    // The node colour is left at its default deliberately. It is what a sample
    // OUTSIDE the stored bricks reports — empty space, where the sculpt has no
    // colour to give — and picking a palette entry for it would mean running a
    // conversion per entry just to find one, which is the cost this change
    // exists to remove.
    clay_node_id placed = 0;
    r = insert_node(doc, layer, std::move(node), &placed);
    if (r != CLAY_OK) return r;
    if (out_layer) *out_layer = layer;
    return CLAY_OK;
}

clay_result clay_voxel_mesh_smooth(const clay_voxel_grid* grid, int32_t blur,
                                   clay_mesh** out_mesh) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out pointer");
    // Negative is refused rather than clamped: a caller passing -1 has a bug,
    // and silently meshing at 0 would hide it behind a picture that looks
    // plausible. The upper end is bounded because each pass is a full sweep
    // over the occupied box and nothing sensible asks for more.
    if (blur < 0 || blur > 8)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "blur must be 0..8 passes");
    auto* handle = new clay_mesh();
    handle->data = g->mesh_smooth(voxel::VoxelGrid::SmoothOptions{blur});
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_voxel_mesh_quads(const clay_voxel_grid* grid, const clay_quad_params* params,
                                  clay_mesh** out_mesh) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!params || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    *out_mesh = nullptr;
    clay_quad_params p;
    mesh::QuadTarget target;
    r = read_quad_params(params, &p, &target);
    if (r != CLAY_OK) return r;
    // The same bounds clay_voxel_mesh_smooth applies, for the same reason: a
    // negative is a caller bug, and each pass is a full sweep over the
    // occupied box.
    if (p.blur < 0 || p.blur > 8)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "blur must be 0..8 passes");
    if (p.level >= g->level_count())
        return fail(CLAY_ERROR_NOT_FOUND,
                    "no such resolution level: " + std::to_string(p.level));

    voxel::VoxelGrid::QuadOptions options;
    options.mode = p.mode == CLAY_QUAD_FACES ? voxel::VoxelGrid::QuadOptions::Mode::Faces
                                             : voxel::VoxelGrid::QuadOptions::Mode::Dual;
    options.cell_size = p.cell_size;
    options.blur = p.blur;
    options.level = p.level;

    mesh::QuadFit fit;
    auto* handle = new clay_mesh();
    handle->data = g->mesh_quads_fit(options, target, &fit);
    // An empty grid yields an empty mesh rather than an error, as
    // clay_voxel_mesh does: a frame in which nothing is occupied is not a
    // failure.
    handle->quad_provenance = clay_mesh::QuadProvenance{fit, p.target_quads};
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_voxel_take_dirty_chunks(clay_voxel_grid* grid, int32_t* out_keys_xyz,
                                         size_t* count, size_t* out_remaining) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    if (!out_keys_xyz)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "clay_voxel_take_dirty_chunks is capacity-in/count-out, not a size query: "
                    "pass a buffer and call again while *out_remaining is non-zero");
    const std::size_t capacity = *count;
    r = check_batch("chunk keys", capacity);
    if (r != CLAY_OK) return r;
    if (grid->staged_head == grid->staged_dirty.size()) {
        grid->staged_dirty = g->take_dirty_chunks();  // all-or-nothing, once
        grid->staged_head = 0;
    }
    const std::size_t available = grid->staged_remaining();
    const std::size_t written = capacity < available ? capacity : available;
    for (std::size_t i = 0; i < written; ++i) {
        const voxel::VoxelCoord& key = grid->staged_dirty[grid->staged_head + i];
        out_keys_xyz[i * 3] = key.x;
        out_keys_xyz[i * 3 + 1] = key.y;
        out_keys_xyz[i * 3 + 2] = key.z;
    }
    grid->staged_head += written;
    if (grid->staged_head == grid->staged_dirty.size()) {
        grid->staged_dirty.clear();
        grid->staged_dirty.shrink_to_fit();
        grid->staged_head = 0;
    }
    *count = written;
    if (out_remaining) *out_remaining = grid->staged_remaining() + g->dirty_chunk_count();
    return CLAY_OK;
}

clay_result clay_voxel_mesh_chunks(const clay_voxel_grid* grid, const int32_t* keys_xyz,
                                   size_t key_count, clay_voxel_chunk_mesh_range* out_ranges,
                                   clay_mesh** out_mesh) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out pointer");
    if (key_count > 0 && !keys_xyz) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null chunk keys");
    r = check_batch("chunk keys", key_count);
    if (r != CLAY_OK) return r;
    // Ranges without a key list would be a length inferred from the grid's
    // current chunk set — the one kind of length this ABI never infers.
    if (out_ranges && key_count == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "ranges were requested with no keys: there is no count to have sized them "
                    "from");
    std::vector<voxel::VoxelCoord> keys;
    keys.reserve(key_count);
    for (std::size_t i = 0; i < key_count; ++i)
        keys.push_back({keys_xyz[i * 3], keys_xyz[i * 3 + 1], keys_xyz[i * 3 + 2]});
    std::vector<voxel::VoxelChunkMeshRange> ranges;
    auto* handle = new clay_mesh();
    handle->data = g->mesh_greedy_chunks(keys, out_ranges ? &ranges : nullptr);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        out_ranges[i].key[0] = ranges[i].key.x;
        out_ranges[i].key[1] = ranges[i].key.y;
        out_ranges[i].key[2] = ranges[i].key.z;
        out_ranges[i].vertex_first = ranges[i].vertex_first;
        out_ranges[i].vertex_count = ranges[i].vertex_count;
        out_ranges[i].index_first = ranges[i].index_first;
        out_ranges[i].index_count = ranges[i].index_count;
    }
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_voxel_rasterize_mesh(clay_voxel_grid* grid, const clay_mesh* mesh,
                                      const float region_min[3], const float region_max[3]) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    const mesh::Mesh* m = nullptr;
    r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (m->empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "the mesh has no triangles");
    if ((region_min == nullptr) != (region_max == nullptr))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a region needs both a min and a max");

    // No region means the mesh's own bounds, which a mesh always has — that is
    // the difference from clay_voxel_rasterize, where a document may not.
    if (!region_min) {
        g->rasterize_mesh(*m);
        return CLAY_OK;
    }
    const math::Aabb box{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                         kernel::cf3(region_max[0], region_max[1], region_max[2])};
    // Checked before the grid is touched, so a rejected call leaves it as it
    // was rather than half-rasterized.
    if (!box_is_finite(box) || box.empty() || box.is_infinite())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the region must be finite, non-empty and bounded");
    g->rasterize_mesh(*m, box);
    return CLAY_OK;
}

clay_result clay_voxel_rasterize(clay_voxel_grid* grid, const clay_document* doc,
                                 const float region_min[3], const float region_max[3]) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    VoxelStep step(grid, g);
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if ((region_min == nullptr) != (region_max == nullptr))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a region needs both a min and a max");
    std::shared_ptr<const scene::Tape> tape_ref = doc->tape();
    const scene::Tape& tape = *tape_ref;
    math::Aabb box = tape.bounds;
    if (region_min)
        box = math::Aabb{kernel::cf3(region_min[0], region_min[1], region_min[2]),
                         kernel::cf3(region_max[0], region_max[1], region_max[2])};
    // Checked before the grid is touched, so a rejected call leaves it as it
    // was rather than half-rasterized.
    if (!box_is_finite(box) || box.empty() || box.is_infinite())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    region_min ? "the region must be finite, non-empty and bounded"
                               : "document has no bounded content to rasterize");
    g->rasterize_tape(tape, box);
    return CLAY_OK;
}

clay_result clay_voxel_raycast(const clay_voxel_grid* grid, const float origin[3],
                               const float dir[3], int32_t* out_hit, int32_t out_cell[3],
                               int32_t* out_face, int32_t out_adjacent[3], float* out_t) {
    voxel::VoxelGrid* g = nullptr;
    math::Ray ray;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    r = make_ray(origin, dir, &ray);
    if (r != CLAY_OK) return r;
    if (!out_hit) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_hit");
    pick::VoxelHit hit = pick::raycast_voxels(*g, ray);
    *out_hit = hit.hit ? 1 : 0;
    if (!hit.hit) return CLAY_OK;  // nothing else means anything on a miss
    if (out_cell) write_cell(out_cell, hit.cell);
    if (out_face) *out_face = hit.face;
    if (out_adjacent) write_cell(out_adjacent, pick::adjacent_cell(hit));
    if (out_t) *out_t = hit.t;
    return CLAY_OK;
}

clay_result clay_voxel_build_plane_pick(const clay_voxel_grid* grid, const float origin[3],
                                        const float dir[3], int32_t plane_cell,
                                        int32_t* out_hit, int32_t out_cell[3]) {
    voxel::VoxelGrid* g = nullptr;
    math::Ray ray;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    r = make_ray(origin, dir, &ray);
    if (r != CLAY_OK) return r;
    if (!out_hit) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_hit");
    std::optional<voxel::VoxelCoord> cell = pick::pick_build_plane(*g, ray, plane_cell);
    *out_hit = cell ? 1 : 0;
    if (cell && out_cell) write_cell(out_cell, *cell);
    return CLAY_OK;
}

// -- influence bounds (scene-model spec: what culling and dirtying consult) ---

clay_result clay_layer_node_influence_bound(const clay_document* doc, clay_layer_id layer_id,
                                            clay_node_id node, float out_min[3],
                                            float out_max[3], int32_t* out_has_bounds,
                                            int32_t* out_infinite) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    // A voxel layer carries no SDF content, so it has no influence bound and
    // no node to name one — reported as "nothing", not as a failure.
    if (!layer->sdf)
        return write_influence(math::Aabb{}, out_min, out_max, out_has_bounds, out_infinite);
    // Over every layer sharing this content, not just the one named: an
    // instanced layer compiles the same node under its own transform, and a
    // host handed one copy's box leaves the others stale (issue #325).
    return write_influence(
        scene::node_influence_bound_in_document(doc->doc.document, *layer->sdf, node), out_min,
        out_max, out_has_bounds, out_infinite);
}

clay_result clay_layer_influence_bound(const clay_document* doc, clay_layer_id layer_id,
                                       float out_min[3], float out_max[3],
                                       int32_t* out_has_bounds, int32_t* out_infinite) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    return write_influence(scene::layer_influence_bound(*layer), out_min, out_max,
                           out_has_bounds, out_infinite);
}

// -- dense grid evaluation ---------------------------------------------------

clay_result clay_eval_grid(const clay_document* doc, const char* backend,
                           const clay_grid_query* grid, const float region_min[3],
                           const float region_max[3], float* out_values,
                           float* out_colors_rgb, size_t value_count) {
    if (!doc || !grid || !out_values)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document, grid or values");
    clay_grid_query q;
    clay_result r = read_desc(grid, kGridQueryOriginal, &q);
    if (r != CLAY_OK) return r;
    eval::GridQuery query;
    std::size_t samples = 0;
    r = read_grid(q.origin, q.spacing, q.dims, &query, &samples);
    if (r != CLAY_OK) return r;
    if (value_count != samples)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "value_count must be the dims product (" + std::to_string(samples) +
                        "), got " + std::to_string(value_count));
    bool has_region = false;
    math::Aabb region;
    r = read_region(region_min, region_max, "the cull region", &has_region, &region);
    if (r != CLAY_OK) return r;
    if (!has_region)  // the whole document, from the cache every other read uses
        return eval_grid_into(*doc->tape(), backend, query, out_values, out_colors_rgb);
    // A culled tape is compiled per call and deliberately not cached:
    // consecutive bricks want different regions, so a slot keyed on the
    // document alone would thrash. The compile itself runs through the
    // revision-cached cull index, so it walks the region's neighbourhood
    // rather than the whole document.
    std::shared_ptr<const scene::CullIndex> index = doc->cull_index();
    const scene::CullPlan plan = index->plan(region);
    scene::CullRegion cull{region};
    scene::Tape tape = scene::compile_document(doc->doc.document, &cull, index.get(), &plan);
    return eval_grid_into(tape, backend, query, out_values, out_colors_rgb);
}

// -- the host parity fixture (build-packaging spec: host parity fixture) -----

clay_result clay_parity_fixture_json(char* buffer, size_t* size) {
    if (!size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null size");
    // Built on each call rather than cached: this is a test-time entry point,
    // and a static cache would hold a few hundred kilobytes for the whole life
    // of every process that never calls it.
    const std::string json = io::kernel_parity_fixture_json(io::kernel_parity_cases());
    const std::size_t needed = json.size() + 1;
    if (!buffer) {
        *size = needed;
        return CLAY_OK;
    }
    if (*size < needed) {
        *size = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the parity fixture needs " + std::to_string(needed) + " bytes");
    }
    std::memcpy(buffer, json.c_str(), needed);
    *size = needed;
    return CLAY_OK;
}

// -- device interop (evaluation-backends spec: a caller-supplied device) -----

struct clay_device {
    std::unique_ptr<eval::Backend> backend;
};

namespace {
// The batch resume the host-memory refill runs (#348), declared here because
// the device refill below wants the same one rather than a second copy of the
// arithmetic. It copies each seed out under `cache_lock()` and then compiles
// and evaluates OFF it, over the pool -- so it must not be called with that
// lock held.
std::size_t resume_bricks(const clay_document* doc, const clay_brick_request* requests,
                          std::size_t count, std::size_t per, bool want_colour, float* out_values,
                          float* out_colors_rgb, std::uint8_t* resumed);

// A batched device evaluation's outcome in the ABI's words. Its own function
// because a batch split into runs reports it from more than one place, and a
// switch written out at each of them is a switch that can disagree with itself.
clay_result device_batch_status(eval::Status s) {
    switch (s) {
        case eval::Status::Ok: return CLAY_OK;
        case eval::Status::Unsupported:
            return fail(CLAY_ERROR_UNSUPPORTED,
                        "this backend does not evaluate into a caller's device buffer");
        case eval::Status::InvalidInput:
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "a brick's device slot is invalid");
        default: return fail(CLAY_ERROR_BACKEND, "device evaluation failed");
    }
}

// Answers whichever of `requests` carry a usable seed into `host_values` /
// `host_colors` and marks them in `resumed`, returning how many. `keep_seeds`
// comes back false when a seed may not be KEPT for the bricks this did NOT
// answer -- a stronger question than whether one may be used, and the only
// reason this needs to say anything beyond the count.
//
// With more than one visible SDF layer a seed is two values -- the active
// layer's and the hard union of everything beneath it -- and the device refill
// evaluates the document whole, so what it could store is neither half. It
// stores nothing rather than something mislabelled: the shape gate in
// `shaped_entry` would refuse such an entry to a multi-layer reader, but a
// document that later loses a layer would find it acceptable and wrong. The
// host-memory refill keeps the two halves apart and resumes multi-layer
// documents fine; the device one leaves them to the full walk.
std::size_t resume_batch_into_host(const clay_document* doc,
                                   const clay_brick_request* requests, std::size_t count,
                                   std::size_t per, bool want_colour,
                                   std::vector<std::uint8_t>* resumed,
                                   std::vector<float>* host_values,
                                   std::vector<float>* host_colors, bool* keep_seeds) {
    {
        // The probe alone under the lock: `resume_bricks` takes it itself, and
        // holding it across that call would put back the serialisation #348
        // took out.
        std::lock_guard<std::mutex> lock(doc->cache_lock());
        *keep_seeds = !doc->plan_resume(1).has_below;  // probed for has_below only
    }
    if (!*keep_seeds) return 0;
    host_values->resize(count * per);
    if (want_colour) host_colors->resize(count * per * 3);
    return resume_bricks(doc, requests, count, per, want_colour, host_values->data(),
                         want_colour ? host_colors->data() : nullptr, resumed->data());
}

clay_result read_device_buffer(const clay_device_buffer* src, const char* what,
                               eval::DeviceBuffer* out) {
    if (!src) {
        *out = eval::DeviceBuffer{};
        return CLAY_OK;  // an absent optional destination, not an error
    }
    clay_device_buffer d;
    clay_result r = read_desc(src, kDeviceBufferOriginal, &d);
    if (r != CLAY_OK) return r;
    if (!d.handle)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, std::string("null ") + what + " handle");
    if (d.size == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("the ") + what + " buffer declares no available size; it is "
                    "required rather than inferred, as every other length here is");
    out->handle = d.handle;
    out->offset = d.offset;
    out->size = d.size;
    return CLAY_OK;
}

}  // namespace

clay_device* clay_device_adopt(const clay_device_desc* desc) {
    clay_device_desc d;
    if (read_desc(desc, kDeviceDescOriginal, &d) != CLAY_OK) return nullptr;
    eval::DeviceApi api;
    const char* backend_name = nullptr;
    switch (d.api) {
        case CLAY_DEVICE_API_METAL:
            api = eval::DeviceApi::Metal;
            backend_name = "metal";
            break;
        case CLAY_DEVICE_API_VULKAN:
            api = eval::DeviceApi::Vulkan;
            backend_name = "vulkan";
            break;
        case CLAY_DEVICE_API_CUDA:
            api = eval::DeviceApi::Cuda;
            backend_name = "cuda";
            break;
        default:
            fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown device API: " + std::to_string(d.api));
            return nullptr;
    }
    eval::DeviceHandles handles;
    handles.api = api;
    for (int i = 0; i < 6; ++i) handles.handles[i] = d.handles[i];
    handles.queue_family = d.queue_family;
    std::unique_ptr<eval::Backend> backend = eval::make_backend(backend_name, handles);
    if (!backend) {
        // Refused at ADOPT rather than at first use, so a caller learns at the
        // point it can still choose a fallback. It is a capability report: the
        // registered backend stays usable and produces the same values.
        fail(CLAY_ERROR_UNSUPPORTED,
             std::string("the ") + backend_name +
                 " backend cannot adopt this device: it is not compiled in, has no adoption "
                 "path, or the handles are incomplete for that API");
        return nullptr;
    }
    auto* handle = new clay_device();
    handle->backend = std::move(backend);
    return handle;
}

void clay_device_release(clay_device* device) { delete device; }

const char* clay_device_backend_name(const clay_device* device) {
    return device && device->backend ? device->backend->name() : nullptr;
}

clay_result clay_eval_grid_device(const clay_document* doc, clay_device* device,
                                  const clay_grid_query* grid, const float region_min[3],
                                  const float region_max[3],
                                  const clay_device_buffer* out_values,
                                  const clay_device_buffer* out_colors_rgb) {
    if (!doc || !device || !device->backend || !grid || !out_values)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document, device, grid or values");
    clay_grid_query q;
    clay_result r = read_desc(grid, kGridQueryOriginal, &q);
    if (r != CLAY_OK) return r;
    eval::GridQuery query;
    std::size_t samples = 0;
    r = read_grid(q.origin, q.spacing, q.dims, &query, &samples);
    if (r != CLAY_OK) return r;
    eval::DeviceBuffer values, colors;
    r = read_device_buffer(out_values, "values", &values);
    if (r != CLAY_OK) return r;
    r = read_device_buffer(out_colors_rgb, "colours", &colors);
    if (r != CLAY_OK) return r;
    bool has_region = false;
    math::Aabb region;
    r = read_region(region_min, region_max, "the cull region", &has_region, &region);
    if (r != CLAY_OK) return r;

    std::shared_ptr<const scene::Tape> whole;
    scene::Tape culled;
    const scene::Tape* tape = nullptr;
    if (has_region) {
        std::shared_ptr<const scene::CullIndex> index = doc->cull_index();
        const scene::CullPlan plan = index->plan(region);
        scene::CullRegion cull{region};
        culled = scene::compile_document(doc->doc.document, &cull, index.get(), &plan);
        tape = &culled;
    } else {
        whole = doc->tape();
        tape = whole.get();
    }
    switch (device->backend->eval_grid_device(*tape, query, values, colors)) {
        case eval::Status::Ok: return CLAY_OK;
        case eval::Status::Unsupported:
            return fail(CLAY_ERROR_UNSUPPORTED,
                        "this backend does not evaluate into a caller's device buffer");
        case eval::Status::InvalidInput:
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "the device buffer is too small for the lattice, or the lattice is "
                        "empty");
        default: return fail(CLAY_ERROR_BACKEND, "device evaluation failed");
    }
}

clay_result clay_brick_cache_eval_requests_device(const clay_document* doc, clay_device* device,
                                                  const clay_brick_request* requests,
                                                  size_t count,
                                                  const clay_device_buffer* out_values,
                                                  const clay_device_buffer* out_colors_rgb) {
    if (!doc || !device || !device->backend)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or device");
    if (count > 0 && (!requests || !out_values))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null requests or values");
    clay_result r = check_batch("brick requests", count);
    if (r != CLAY_OK) return r;
    if (count == 0) return CLAY_OK;
    eval::GridQuery first;
    std::size_t per = 0;
    r = read_grid(requests[0].origin, requests[0].spacing, requests[0].dims, &first, &per);
    if (r != CLAY_OK) return r;
    r = check_uniform_dims(requests, count);
    if (r != CLAY_OK) return r;
    eval::DeviceBuffer values, colors;
    r = read_device_buffer(out_values, "values", &values);
    if (r != CLAY_OK) return r;
    r = read_device_buffer(out_colors_rgb, "colours", &colors);
    if (r != CLAY_OK) return r;
    // The whole batch's stride is checked up front, so a buffer that cannot
    // hold every brick is refused before the first dispatch rather than after
    // the ones that fit have already landed.
    const std::uint64_t values_bytes = static_cast<std::uint64_t>(count) * per * sizeof(float);
    if (values.size < values_bytes)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the device values buffer holds " + std::to_string(values.size) +
                        " bytes; " + std::to_string(count) + " bricks need " +
                        std::to_string(values_bytes));
    if (!colors.empty() && colors.size < values_bytes * 3)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the device colour buffer is too small for " + std::to_string(count) +
                        " bricks");

    // -- the resumable path on a device destination (#345) ------------------
    //
    // The host-memory refill has kept each brick's float32 result as a seed
    // since #306, so a dab costs what the dab adds rather than what the sculpt
    // holds. This entry point is the one a RENDERER uses — clay_device_adopt
    // exists so evaluation lands in the caller's own GPU buffer — and it had
    // none of that: every dab walked the whole surviving edit list over every
    // sample, which is what the host path did before #306.
    //
    // The seed is host-resident float32 and the answer here is not, so the two
    // halves are joined by the backend's buffer write and read:
    //
    //   * a brick that CAN be resumed is answered on the host, exactly as the
    //     host-memory form answers it — same suffix, same arithmetic, the same
    //     `resume_one_brick` — and the finished lattice is written into its
    //     fixed slot in the caller's allocation;
    //   * a brick that cannot is evaluated on the DEVICE into that same slot,
    //     as the whole batch was before, and read back so it becomes the next
    //     dab's seed.
    //
    // So a resumed brick here is a CPU suffix continued from a GPU prefix, and
    // it is held to the parity suite's backend standard rather than to bit
    // identity -- which is the standard the device path was already held to,
    // since the prefix is the same GPU walk either way. The host-memory refill
    // named with a GPU backend has done exactly this since #306.
    //
    // MEASURED before it was chosen (openspec/changes/resume-the-device-refill).
    // The other shape the issue names — seeds resident on the device, so the
    // suffix evaluates there and nothing crosses — cannot win on the windows a
    // sculpt actually submits: a device seeded kernel still has to DISPATCH,
    // and on an RTX 5060 one dispatch of the emptiest possible tape costs 23 us
    // a brick, where the whole host-side resumed refill of 12 bricks — the
    // per-brick suffix compile a device path would also have to pay included —
    // is 18 us at 200 items and 155 us at 20,000. The copy it would save is
    // 24 KiB, under a microsecond. Residency would only start to pay on a
    // window of hundreds of resumable bricks, and a window that large is one a
    // stroke has not covered before, so its bricks have no seed to be resident.
    //
    // A backend with no buffer write and read says so through
    // `caps().device_copy`, and the whole batch takes the full walk it took
    // before — correct, silent, and exactly as fast as it always was.
    const bool want_colour = !colors.empty();
    std::vector<std::uint8_t> resumed(count, 0);
    std::size_t resumed_count = 0;
    std::vector<float> host_values, host_colors;
    // Whether a seed may be KEPT for the bricks the resume does not answer,
    // which `resume_batch_into_host` decides and explains.
    bool keep_seeds = false;
    if (device->backend->caps().device_copy)
        resumed_count = resume_batch_into_host(doc, requests, count, per, want_colour, &resumed,
                                               &host_values, &host_colors, &keep_seeds);
    doc->note_refill(resumed_count, count - resumed_count);

    // The resumed answers into their slots, one write per CONTIGUOUS run rather
    // than one per brick: the slots are a fixed stride, so consecutive bricks
    // are consecutive bytes and a moving window is one or two writes.
    const clay_result written = for_each_run(resumed, true, [&](std::size_t at, std::size_t n) {
        if (device->backend->write_device_buffer(brick_slot(values, at, n, per),
                                                 host_values.data() + at * per,
                                                 static_cast<std::uint64_t>(n) * per *
                                                     sizeof(float)) != eval::Status::Ok)
            return fail(CLAY_ERROR_BACKEND, "writing a resumed brick to the device failed");
        if (!want_colour) return CLAY_OK;
        if (device->backend->write_device_buffer(brick_slot(colors, at, n, per * 3),
                                                 host_colors.data() + at * per * 3,
                                                 static_cast<std::uint64_t>(n) * per * 3 *
                                                     sizeof(float)) != eval::Status::Ok)
            return fail(CLAY_ERROR_BACKEND, "writing a resumed brick's colours failed");
        return CLAY_OK;
    });
    if (written != CLAY_OK) return written;
    if (resumed_count == count) return CLAY_OK;

    // The rest take the batch pipeline the host-memory form runs — validation
    // up front, one cull index and coarse plan, per-brick culled tapes in
    // chunks — with the whole chunk reaching the adopted backend as ONE
    // batched device evaluation (issue #64 applied to the zero-copy path:
    // the per-brick loop paid a command buffer and a wait per 8^3 lattice,
    // which left this path 25-165x behind the host-memory one). Each chunk
    // lands at its requests' fixed slots in the caller's single allocation,
    // so brick i still occupies out_values[i * dim^3 ...] exactly as
    // documented, and the values are identical to the host-memory form's.
    //
    // Per RUN of un-resumed bricks rather than over the whole batch, because a
    // batched device evaluation writes its grids consecutively: a run of bricks
    // is the largest thing that can land at the slots it belongs in. A batch
    // with nothing resumed is one run and is exactly the call this made before.
    return for_each_run(resumed, false, [&](std::size_t at, std::size_t n) {
        const clay_result walked = eval_requests_in_chunks(
            doc, requests + at, n,
            [&](const eval::GridBatchQuery& bq, std::size_t base) -> clay_result {
                return device_batch_status(device->backend->eval_grid_batch_device(
                    bq, brick_slot(values, at + base, bq.count, per),
                    colors.empty() ? eval::DeviceBuffer{}
                                   : brick_slot(colors, at + base, bq.count, per * 3)));
            });
        if (walked != CLAY_OK || !keep_seeds) return walked;
        // Read back what the device just produced, so the NEXT dab over this
        // ground resumes instead of walking the edit list again. A few
        // kilobytes a brick, attached to the walk that is the expensive half.
        if (device->backend->read_device_buffer(
                host_values.data() + at * per, brick_slot(values, at, n, per),
                static_cast<std::uint64_t>(n) * per * sizeof(float)) != eval::Status::Ok)
            return fail(CLAY_ERROR_BACKEND, "reading a refilled brick back failed");
        if (want_colour &&
            device->backend->read_device_buffer(
                host_colors.data() + at * per * 3, brick_slot(colors, at, n, per * 3),
                static_cast<std::uint64_t>(n) * per * 3 * sizeof(float)) != eval::Status::Ok)
            return fail(CLAY_ERROR_BACKEND, "reading a refilled brick's colours back failed");
        doc->store_seeds(requests + at, n, host_values.data() + at * per,
                         want_colour ? host_colors.data() + at * per * 3 : nullptr, nullptr,
                         nullptr, per, 0, 0.0f);
        return CLAY_OK;
    });
}

// -- the compiled tape (c-abi spec: the compiled tape is exportable) ---------

// An immutable snapshot. The document hands out its compiled tape as a
// shared_ptr<const Tape> keyed on a revision and installs a NEW one on an edit
// rather than mutating the old, so holding a copy of the pointer is the whole
// implementation of "editing cannot invalidate an export": exporting costs a
// refcount, and there is no window in which a borrowed buffer goes bad.
//
// A culled tape has no such cache and is compiled per call, so it is owned here
// the same way — the handle is the only difference the caller sees.
struct clay_tape {
    std::shared_ptr<const scene::Tape> tape;
    std::uint64_t revision = 0;
};

namespace {

// clay_tape_instr IS kernel::CTapeInstr: the caller's evaluator is ctape_eval
// compiled from the header that declares it, so the two layouts agreeing is
// not a convenience, it is the contract.
static_assert(sizeof(clay_tape_instr) == sizeof(kernel::CTapeInstr),
              "clay_tape_instr must be kernel::CTapeInstr");
static_assert(offsetof(clay_tape_instr, op) == offsetof(kernel::CTapeInstr, op),
              "clay_tape_instr.op must match");
static_assert(offsetof(clay_tape_instr, param_offset) ==
                  offsetof(kernel::CTapeInstr, param_offset),
              "clay_tape_instr.param_offset must match");

clay_result resolve_tape(const clay_tape* tape, const scene::Tape** out) {
    if (!tape || !tape->tape) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null tape");
    *out = tape->tape.get();
    return CLAY_OK;
}

}  // namespace

clay_result clay_tape_export(const clay_document* doc, const float region_min[3],
                             const float region_max[3], clay_tape** out_tape) {
    if (!doc || !out_tape)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out_tape");
    bool has_region = false;
    math::Aabb region;
    clay_result r = read_region(region_min, region_max, "the cull region", &has_region, &region);
    if (r != CLAY_OK) return r;
    auto* handle = new clay_tape();
    handle->revision = doc->revision.load(std::memory_order_relaxed);
    if (has_region) {
        // Compiled per call and deliberately not cached, exactly as
        // clay_eval_grid does it: consecutive regions differ, so a slot keyed
        // on the document alone would thrash. The cull index is the cached
        // part, keyed on the revision this handle already records.
        std::shared_ptr<const scene::CullIndex> index = doc->cull_index();
        const scene::CullPlan plan = index->plan(region);
        scene::CullRegion cull{region};
        handle->tape = std::make_shared<const scene::Tape>(
            scene::compile_document(doc->doc.document, &cull, index.get(), &plan));
    } else {
        handle->tape = doc->tape();  // a refcount, not a compile
    }
    *out_tape = handle;
    return CLAY_OK;
}

void clay_tape_release(clay_tape* tape) { delete tape; }

uint32_t clay_tape_encoding_version(void) {
    // clay::version(), which is the CMake project version — the same number
    // tools/package_kernels.py stamps into the package's VERSION file, read
    // from the same line of CMakeLists.txt. One number for both because a host
    // evaluates an exported tape with ctape_eval from that package's headers:
    // an opcode present on one side and absent on the other is a wrong ANSWER,
    // not a link error, so the two cannot be versioned independently. This is
    // deliberately NOT clay_version()'s CLAY_ABI_*, which tracks the shape of
    // this header rather than the shape of the tape.
    const Version v = clay::version();
    return static_cast<std::uint32_t>(v.major) * 1000000u +
           static_cast<std::uint32_t>(v.minor) * 1000u + static_cast<std::uint32_t>(v.patch);
}

const clay_tape_instr* clay_tape_instrs(const clay_tape* tape, size_t* out_count) {
    const scene::Tape* t = nullptr;
    if (!out_count || resolve_tape(tape, &t) != CLAY_OK) return nullptr;
    *out_count = t->instrs.size();
    return t->instrs.empty() ? nullptr
                             : reinterpret_cast<const clay_tape_instr*>(t->instrs.data());
}

const float* clay_tape_params(const clay_tape* tape, size_t* out_count) {
    const scene::Tape* t = nullptr;
    if (!out_count || resolve_tape(tape, &t) != CLAY_OK) return nullptr;
    *out_count = t->params.size();
    return t->params.empty() ? nullptr : t->params.data();
}

const float* clay_tape_blob(const clay_tape* tape, size_t* out_count) {
    const scene::Tape* t = nullptr;
    if (!out_count || resolve_tape(tape, &t) != CLAY_OK) return nullptr;
    *out_count = t->blob.size();
    return t->blob.empty() ? nullptr : t->blob.data();
}

clay_result clay_tape_info(const clay_tape* tape, int32_t* out_is_exact, float* out_lipschitz,
                           float* out_safe_step_scale, float out_bounds_min[3],
                           float out_bounds_max[3], uint64_t* out_revision) {
    const scene::Tape* t = nullptr;
    clay_result r = resolve_tape(tape, &t);
    if (r != CLAY_OK) return r;
    if (out_is_exact) *out_is_exact = t->info.is_exact ? 1 : 0;
    if (out_lipschitz) *out_lipschitz = t->info.lipschitz;
    if (out_safe_step_scale) *out_safe_step_scale = t->safe_step_scale();
    if (out_bounds_min) write_f3(out_bounds_min, t->bounds.min);
    if (out_bounds_max) write_f3(out_bounds_max, t->bounds.max);
    if (out_revision) *out_revision = tape->revision;
    return CLAY_OK;
}

// -- the brick cache (brick-cache spec, through the C boundary) --------------

// The caller declares struct_size going IN, as it does for every other
// descriptor. This one was the live case: clay_brick_config grew a `colors`
// field, so a host built against the 24-byte layout had 8 bytes of its stack
// written past the end — silently, and only on the hosts the prefix rule
// exists to serve, since anything rebuilt from this header is the same size we
// are. There is no version of this that helps an ALREADY-compiled old host:
// it declares nothing, so the best available answer is to refuse it loudly
// rather than corrupt it quietly.
clay_result clay_brick_config_defaults(clay_brick_config* out_config) {
    if (!out_config) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null config");
    clay_brick_config probe;
    clay_result r = read_desc(out_config, kBrickConfigOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_config->struct_size;

    const brick::BrickConfig d;
    clay_brick_config filled{};
    filled.dim = d.dim;
    filled.voxel_size = d.voxel_size;
    filled.band_voxels = d.band_voxels;
    filled.memory_budget = d.memory_budget;
    filled.colors = d.colors ? 1 : 0;
    write_desc(out_config, declared, filled);
    return CLAY_OK;
}

clay_brick_cache* clay_brick_cache_create(const clay_brick_config* config) {
    brick::BrickConfig c;
    if (read_brick_config(config, &c) != CLAY_OK) return nullptr;
    return new clay_brick_cache(c);
}

void clay_brick_cache_destroy(clay_brick_cache* cache) { delete cache; }

clay_result clay_brick_cache_config(const clay_brick_cache* cache,
                                    clay_brick_config* out_config) {
    if (!cache || !out_config)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or config");
    // The descriptor is an OUTPUT, so struct_size is the caller telling us how
    // much of it exists rather than what it filled in.
    clay_brick_config probe;
    clay_result r = read_desc(out_config, kBrickConfigOriginal, &probe);
    if (r != CLAY_OK) return r;
    const brick::BrickConfig& c = cache->cache.config();
    const std::uint32_t declared = out_config->struct_size;
    clay_brick_config filled{};
    filled.dim = c.dim;
    filled.voxel_size = c.voxel_size;
    filled.band_voxels = c.band_voxels;
    filled.memory_budget = c.memory_budget;
    filled.colors = c.colors ? 1 : 0;
    write_desc(out_config, declared, filled);
    return CLAY_OK;
}

clay_result clay_brick_cache_stats(const clay_brick_cache* cache, clay_brick_stats* out_stats) {
    if (!cache || !out_stats)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or stats");
    clay_brick_stats probe;
    clay_result r = read_desc(out_stats, kBrickStatsOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_stats->struct_size;
    clay_brick_stats filled{};
    filled.tracked_bricks = cache->cache.tracked_count();
    filled.surface_bricks = cache->cache.surface_bricks().size();
    // What is still queued INSIDE the engine plus what this binding drained
    // into staging and has not handed out yet: both are bricks the host still
    // owes an evaluation.
    filled.dirty_bricks = cache->cache.dirty_count() + cache->staged_remaining();
    filled.memory_usage = cache->cache.memory_usage();
    filled.memory_budget = cache->cache.config().memory_budget;
    filled.bookkeeping_bytes = cache->cache.bookkeeping_bytes();
    filled.brick_bytes = cache->cache.config().brick_bytes();
    // Bounded by what the caller declared: a host compiled against the layout
    // before these two fields existed gets exactly the struct it allocated.
    write_desc(out_stats, declared, filled);
    return CLAY_OK;
}

clay_result clay_document_resume_stats(const clay_document* doc, clay_resume_stats* out_stats) {
    if (!doc || !out_stats) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or stats");
    clay_resume_stats probe;
    clay_result r = read_desc(out_stats, kResumeStatsOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_stats->struct_size;
    clay_resume_stats filled{};
    doc->resume_stats(&filled.entries, &filled.bytes, &filled.budget, &filled.resumed_bricks,
                      &filled.refilled_bricks);
    write_desc(out_stats, declared, filled);
    return CLAY_OK;
}

// -- bindings/c/clay_internal.h: not the ABI, and not versioned --------------

clay_result clay_internal_resume_order_size(const clay_document* doc, uint64_t* out_entries) {
    if (!doc || !out_entries) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or out");
    *out_entries = static_cast<std::uint64_t>(doc->resume_order_size());
    return CLAY_OK;
}

clay_result clay_internal_set_resume_budget(clay_document* doc, uint64_t bytes) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    doc->set_resume_budget(static_cast<std::size_t>(bytes));
    return CLAY_OK;
}

clay_result clay_internal_resume_frontier(const clay_document* doc,
                                          const clay_brick_request* request,
                                          uint32_t* out_dirty_from, uint32_t* out_boundary,
                                          uint64_t* out_structure) {
    if (!doc || !request || !out_dirty_from || !out_boundary || !out_structure)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document, request or out");
    std::uint32_t dirty = 0, boundary = 0;
    std::uint64_t structure = 0;
    if (!doc->frontier_probe(*request, &dirty, &boundary, &structure))
        return fail(CLAY_ERROR_NOT_FOUND, "no resume entry for that brick");
    *out_dirty_from = dirty;
    *out_boundary = boundary;
    *out_structure = structure;
    return CLAY_OK;
}

clay_result clay_internal_set_resume_store_interleave(clay_document* doc, void (*fn)(void* user),
                                                      void* user) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    doc->set_resume_store_interleave(fn, user);
    return CLAY_OK;
}

clay_result clay_brick_cache_trim(clay_brick_cache* cache, uint64_t target_bytes,
                                  const float focus[3], uint64_t* out_dropped) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    const std::size_t target = static_cast<std::size_t>(target_bytes);
    // NULL focus is a statement rather than an omission: a host with no
    // meaningful focus takes the deterministic-order trim.
    const std::size_t dropped =
        focus ? cache->cache.trim_to(target, kernel::cf3(focus[0], focus[1], focus[2]))
              : cache->cache.trim_to(target);
    if (out_dropped) *out_dropped = static_cast<std::uint64_t>(dropped);
    return CLAY_OK;
}

clay_result clay_brick_cache_evict(clay_brick_cache* cache, const int32_t key[3],
                                   uint64_t* out_dropped) {
    if (!cache || !key) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or key");
    const bool dropped = cache->cache.evict(brick::BrickKey{key[0], key[1], key[2]});
    if (out_dropped) *out_dropped = dropped ? 1u : 0u;
    return CLAY_OK;
}

clay_result clay_brick_cache_forget_empty(clay_brick_cache* cache, uint64_t* out_forgotten) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    const std::size_t forgotten = cache->cache.forget_empty();
    if (out_forgotten) *out_forgotten = static_cast<std::uint64_t>(forgotten);
    return CLAY_OK;
}

clay_result clay_brick_cache_mark_dirty(clay_brick_cache* cache, const float region_min[3],
                                        const float region_max[3]) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    if (!region_min && !region_max) {
        // The only way to say "dirty everything tracked". It allocates
        // nothing: the engine walks the bricks it already holds.
        cache->cache.mark_dirty(math::Aabb::infinite());
        return CLAY_OK;
    }
    bool has_region = false;
    math::Aabb region;
    clay_result r = read_region(region_min, region_max, "the dirty region", &has_region, &region);
    if (r != CLAY_OK) return r;
    r = check_dirty_span(cache->cache, region);
    if (r != CLAY_OK) return r;
    cache->cache.mark_dirty(region);
    return CLAY_OK;
}

clay_result clay_brick_cache_mark_dirty_nodes(clay_brick_cache* cache, const clay_document* doc,
                                              clay_layer_id layer_id, const clay_node_id* nodes,
                                              size_t count, size_t* out_marked) {
    if (!cache || !doc)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or document");
    if (count > 0 && !nodes) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null nodes");
    clay_result r = check_batch("selected nodes", count);
    if (r != CLAY_OK) return r;
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    // Every bound is computed and span-checked BEFORE any of them is marked,
    // so a refusal leaves the cache exactly as it was rather than half-dirtied.
    std::vector<math::Aabb> bounds;
    if (layer->sdf) {
        for (std::size_t i = 0; i < count; ++i) {
            // Over every layer sharing this content (issue #325): the same
            // union clay_layer_node_influence_bound reports, so what a host
            // dirties and what the query told it cannot disagree.
            math::Aabb b = scene::node_influence_bound_in_document(doc->doc.document,
                                                                  *layer->sdf, nodes[i]);
            if (b.empty()) continue;  // absent, hidden, or contributing nothing
            r = check_dirty_span(cache->cache, b);
            if (r != CLAY_OK) return r;
            bounds.push_back(b);
        }
    }
    for (const math::Aabb& b : bounds) cache->cache.mark_dirty(b);
    if (out_marked) *out_marked = bounds.size();
    return CLAY_OK;
}

clay_result clay_brick_cache_mark_dirty_layer(clay_brick_cache* cache, const clay_document* doc,
                                              clay_layer_id layer_id) {
    if (!cache || !doc)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or document");
    const scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    math::Aabb bound = scene::layer_influence_bound(*layer);
    if (bound.empty()) return CLAY_OK;  // a layer that shows nothing marks nothing
    clay_result r = check_dirty_span(cache->cache, bound);
    if (r != CLAY_OK) return r;
    cache->cache.mark_dirty(bound);
    return CLAY_OK;
}

clay_result clay_brick_cache_take_dirty(clay_brick_cache* cache,
                                        clay_brick_request* out_requests, size_t* count,
                                        size_t* out_remaining) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    if (!out_requests)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "clay_brick_cache_take_dirty is capacity-in/count-out, not a size query: "
                    "pass a buffer, and read the pending count from clay_brick_cache_stats");
    const std::size_t capacity = *count;
    clay_result r = check_batch("brick requests", capacity);
    if (r != CLAY_OK) return r;
    if (cache->staged_head == cache->staged.size()) {
        cache->staged = cache->cache.take_dirty();  // all-or-nothing, once
        cache->staged_head = 0;
    }
    const std::size_t available = cache->staged_remaining();
    const std::size_t written = capacity < available ? capacity : available;
    // One memcpy: clay_brick_request IS brick::BrickRequest (asserted above).
    if (written > 0)
        std::memcpy(static_cast<void*>(out_requests),
                    static_cast<const void*>(cache->staged.data() + cache->staged_head),
                    written * sizeof(clay_brick_request));
    cache->staged_head += written;
    if (cache->staged_head == cache->staged.size()) {
        cache->staged.clear();
        cache->staged.shrink_to_fit();
        cache->staged_head = 0;
    }
    *count = written;
    if (out_remaining) *out_remaining = cache->staged_remaining() + cache->cache.dirty_count();
    return CLAY_OK;
}

namespace {
// The hard union a whole-document compile emits between visible SDF layers,
// applied sample by sample to the two halves a resumable refill holds apart.
// Through the kernel's own combine rather than a min written out here: the two
// have to agree bit for bit, and one of them is the definition.
void fold_layers_below(const float* below_d, const float* below_rgb, const float* active_d,
                       const float* active_rgb, std::size_t per, float* out_d, float* out_rgb) {
    for (std::size_t s = 0; s < per; ++s) {
        kernel::CTapeValue a;
        a.d = below_d[s];
        a.color = below_rgb
                      ? kernel::cf3(below_rgb[s * 3], below_rgb[s * 3 + 1], below_rgb[s * 3 + 2])
                      : kernel::cf3(0.0f, 0.0f, 0.0f);
        kernel::CTapeValue c;
        c.d = active_d[s];
        c.color = active_rgb
                      ? kernel::cf3(active_rgb[s * 3], active_rgb[s * 3 + 1], active_rgb[s * 3 + 2])
                      : kernel::cf3(0.0f, 0.0f, 0.0f);
        const kernel::CTapeValue r = kernel::ctape_combine_values(
            a, c, static_cast<CLAY_UINT_T>(scene::Op::Add), 0, 0.0f, 0.0f);
        out_d[s] = r.d;
        if (out_rgb) {
            out_rgb[s * 3] = r.color.x;
            out_rgb[s * 3 + 1] = r.color.y;
            out_rgb[s * 3 + 2] = r.color.z;
        }
    }
}

// -- the resumable path (#306, #348) --------------------------------------
//
// How many of `count` bricks this answered from their seeds, with `resumed[i]`
// marking each one it did. The rest are the caller's to walk in full.
//
// When a brick carries a seed and the document has only been APPENDED to
// since that seed was taken, what this call has to evaluate for it is the
// appended items -- not the whole surviving edit list over every sample.
// The suffix is compiled per brick and culled exactly as a whole-document
// compile would cull it, which is what makes continuing from the seed the
// same arithmetic rather than an approximation of it.
//
// PER BRICK, not per batch. Whether a brick can be resumed, and how far it
// has to be carried, is that brick's own question: the bricks of one batch
// routinely sit at different revisions, because a refill re-stamps only the
// bricks it filled and an append re-stamps none. A stroke whose dirty window
// moves -- which is every stroke -- therefore mixes the ground it covered
// last dab with the ground it has just reached, and a brick it has never
// reached has no seed at all. Bricks that cannot be served drop into the
// miss gather below and take the full path alone.
//
// WITH MORE THAN ONE VISIBLE SDF LAYER the seed is two values, not one. The
// layers hard-union left to right, so the tape holds the layers BENEATH the
// active one as its own accumulator, and a single stored number cannot be
// taken apart into the two again. They are kept apart instead: the suffix
// folds into the active layer's value and the union is applied here, with
// the same hard Add the whole-document compile emits between layers. The
// layers beneath are static across a stroke, so their half is stored once
// and carried forward untouched.
std::size_t resume_bricks(const clay_document* doc, const clay_brick_request* requests,
                          std::size_t count, std::size_t per, bool want_colour, float* out_values,
                          float* out_colors_rgb, std::uint8_t* resumed) {
    std::size_t resumed_count = 0;

    // OFF THE LOCK, AND OVER THE POOL (#348).
    //
    // This used to compile and evaluate every brick inside the one
    // `cache_lock()` it takes to read the seeds, which made a refill two wrong
    // things at once: serial, where the full path it replaces hands the batch
    // to a row-level parallel_for across every brick; and a writer holding a
    // mutex `clay_eval_points` on another thread also takes, so a refill
    // blocked unrelated readers for as long as it evaluated.
    //
    // The lock was held because `seed_for` hands out a raw pointer into the
    // seed store and a concurrent refill may evict the entry under it. So the
    // seed is COPIED OUT under the lock -- into the buffer the evaluation will
    // write its answer to, which costs nothing extra in the common
    // single-layer case because that buffer is the caller's own output and had
    // to be written anyway. `eval_points_seeded` reads a block's seed into the
    // stack before it writes that block's result, so seeding a walk from its
    // own destination is safe: that is #306's open question 7, answered here.
    //
    // Three phases: resolve the plans and copy the seeds under the lock;
    // compile and evaluate with it released; retake it to keep what the bricks
    // reached as the next dab's seeds.
    //
    // WHAT THE POOL IS WORTH, measured on a 24-thread desktop at a dim-8
    // lattice. Dispatching an empty `parallel_for` over 48 units costs 16-19 us
    // there, and that is the whole of the question:
    //
    //   48 bricks, 16-dab suffix   393,216 units    66 us serial -> 30 us pooled
    //   12 bricks, 16-dab suffix    98,304 units    21 us serial -> 25 us pooled
    //   48 bricks,  1-dab suffix    24,576 units    19 us serial
    //
    // The middle row is why this is gated rather than always taken: the pool
    // costs more than it saves there, and burns two dozen cores to do it.
    // `kResumeParallelUnits` sits above it, at about three times the dispatch.
    //
    // A UNIT is one sample times one appended item -- the shape of the work the
    // walk actually does -- so the gate does not have to be restated for a
    // different lattice size or a longer suffix. Below it the loop still runs
    // OFF the lock; only the pool is skipped.
    //
    // CALIBRATED CONCURRENTLY TOO, because letting several host threads refill
    // at once is the whole point of coming off the lock and the pool holds ONE
    // job slot. A dispatch from a second thread REPLACES the advertised job, so
    // workers that have not yet woken for the first never will
    // (thread_pool.h); `in_job()` guards nesting on ONE thread and does not
    // guard this at all. It degrades toward serial rather than deadlocking, and
    // the question a gate has to answer is whether it degrades past the serial
    // branch it is choosing between.
    //
    // It does not, at any concurrency this machine can produce. T threads each
    // refilling the same 48-brick window at a sixteen-dab suffix over a
    // 5,000-item document, minimum over 25 interleaved repetitions of 60
    // rounds, box at load 3-9, microseconds:
    //
    //   T     main   pooled   serial    pooled/serial
    //   1     93.1     74.9     94.8         0.79
    //   2    110.1     84.7    110.6         0.77
    //   4    127.1    115.2    130.8         0.88
    //   6    142.6    142.7    155.2         0.92
    //  12    199.9    197.7    233.9         0.85
    //
    // Main is the column that stops improving: it holds the mutex across its
    // evaluation, so T concurrent refills are T serial refills, which is why
    // 12 threads cost it 2.1x one thread and cost this 2.6x -- the same work
    // actually overlapping.
    //
    // The OTHER shape is the one clay.h recommends -- fan out over REQUESTS,
    // one batch split across threads -- and there the gate settles it without
    // any of the above: 48 bricks over two threads is 196,608 units a slice,
    // under the gate, so no thread dispatches and the collision cannot happen.
    // That shape runs 0.55-0.77x main at one to six threads.
    //
    // Read the ratios and not the times. Two builds running IDENTICAL code --
    // the split shape at T >= 2, where both are below the gate -- measure
    // 1.01-1.07x apart here, which is this harness's noise floor and wider than
    // several of the differences the table would otherwise invite reading.
    struct ResumeTask {
        std::size_t slot = 0;  // which brick of the batch
        const clay_document::ResumePlan* plan = nullptr;
        eval::GridQuery grid;
        // Where the seed IS, valid only while the lock is held. Cleared by the
        // copy pass below, so nothing can still be holding one when the lock
        // drops.
        clay_document::Seed seed;
        // The seed on entry and the ACTIVE layer's answer on exit: the walk
        // runs in place. The caller's own output slot when nothing sits
        // beneath, staging when something does and a union is still to apply.
        float* active = nullptr;
        float* active_rgb = nullptr;
        const float* below = nullptr;
        const float* below_rgb = nullptr;
        // The group half, copied off the seed under the lock. Empty means this
        // brick resumes at a root list, which is the path that always existed.
        std::vector<float> stack;
        std::vector<float> stack_rgb;
        std::uint32_t stack_levels = 0;
        // The checkpoint's own: false where the chain it sits in had produced
        // nothing, which an empty tail group leaves behind. Part of the shape,
        // so the level check has to see it.
        bool layer_have_acc = true;
        std::vector<scene::TapeCheckpointFrame> frames;
        // What this task produces for the NEXT dab, when it produces one.
        std::vector<float> snap;
        std::vector<float> snap_rgb;
        std::size_t snap_levels = 0;
        bool snap_layer_have_acc = true;
        bool ok = false;
    };
    std::vector<ResumeTask> tasks;
    // Staging for the multi-layer case only, where the walk cannot write to the
    // caller's slot because a union still has to be applied to it. Sized once
    // the tasks are known rather than for the whole batch: a call that resumes
    // a tenth of its bricks should not allocate for all of them.
    std::vector<float> stage_active, stage_active_rgb, stage_below, stage_below_rgb;
    std::unordered_map<std::uint64_t, clay_document::ResumePlan> plans;
    // The enclosing groups of the nodes a plan appends, innermost first --
    // the same order compile_group pushes frames in, because it records them
    // as the recursion unwinds. Memoised per plan: it is a property of the
    // append, not of the brick.
    std::unordered_map<const clay_document::ResumePlan*, std::vector<scene::NodeId>> chains;
    auto chain_for = [&](const clay_document::ResumePlan* plan) -> const std::vector<scene::NodeId>& {
        auto it = chains.find(plan);
        if (it != chains.end()) return it->second;
        std::vector<scene::NodeId> chain;
        const scene::SdfContent* content = nullptr;
        for (const scene::Layer& l : doc->doc.document.layers)
            if (l.id == plan->active && l.sdf) content = l.sdf.get();
        if (content && !plan->appended.empty()) {
            scene::NodeId cur = plan->appended.front();
            // Bounded by the node count for the reason node_reach_bound is:
            // `roots` is a public member and this walk must terminate whatever
            // a caller wrote there.
            for (std::size_t step = 0; step <= content->nodes().size(); ++step) {
                scene::NodeId parent = scene::kNoNode;
                int index = -1;
                if (!content->locate(cur, &parent, &index)) break;
                if (parent == scene::kNoNode) break;
                chain.push_back(parent);
                cur = parent;
            }
        }
        return chains.emplace(plan, std::move(chain)).first->second;
    };
    // One plan per distinct prefix BOUNDARY (#360), memoized like `plans` and
    // held by address for the same reason: node-based, and nothing inserts
    // after the lock drops. A drag's bricks overwhelmingly share one boundary,
    // so this is one plan_frontier call per batch in the common case.
    std::unordered_map<std::uint32_t, clay_document::ResumePlan> fplans;
    std::shared_ptr<const scene::CullIndex> index;
    float resume_pad = 0.0f;
    bool has_below = false;
    std::size_t units = 0;  // sample-instructions of deferred work
    {
        std::lock_guard<std::mutex> lock(doc->cache_lock());
        const clay_document::ResumePlan probe = doc->plan_resume(1);  // for has_below only
        has_below = probe.has_below;
        const std::uint64_t now = doc->current_revision();

        // One plan per distinct stored revision. A moving window holds one or
        // two of them -- what the last dab stamped, and what it had not reached
        // -- so this is a couple of plan_resume calls for the batch rather than
        // one per brick. Each plan carries its own `appended` tail, which is
        // what lets a brick left behind two dabs ago catch up in one go.
        //
        // Tasks hold plans by ADDRESS, which this container allows and a vector
        // would not: an unordered_map's elements are nodes, so a rehash moves
        // iterators and not values. Nothing inserts into it after the lock
        // drops.
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t rev =
                doc->seed_revision_for(requests[i], per, want_colour, has_below);
            if (rev == 0) continue;
            // The cull index is only wanted once a brick turns out to have a
            // seed: obtaining it copies the cached one, and a batch with
            // nothing to resume should not pay for that on its way to the full
            // path.
            if (!index) {
                index = doc->cull_index_locked();
                resume_pad = index->cull_pad();
            }
            const clay_document::Seed seed =
                doc->seed_for(requests[i], per, resume_pad, want_colour, has_below);
            if (!seed.values) continue;
            float* vd = out_values + i * per;
            float* vc = want_colour ? out_colors_rgb + i * per * 3 : nullptr;

            // A seed already AT the current revision is the answer: its brick's
            // culled tape has not changed since it was computed, so there is
            // nothing to fold into it. That is what a region-limited
            // invalidation leaves behind -- an edit the brick cannot reach
            // advances the seed rather than dropping it -- and it is also the
            // plain case of a refill asked for twice without an edit in
            // between.
            //
            // The union still applies: what is stored is the ACTIVE layer's
            // value, and the layers beneath are their own half. Answered under
            // the lock rather than deferred because it is one copy either way,
            // and deferring it would be that same copy plus a task.
            if (rev == now) {
                if (has_below) {
                    fold_layers_below(seed.below, seed.below_colors, seed.values, seed.colors, per,
                                      vd, vc);
                } else {
                    std::memcpy(vd, seed.values, per * sizeof(float));
                    if (vc) std::memcpy(vc, seed.colors, per * 3 * sizeof(float));
                }
                resumed[i] = 1;
                ++resumed_count;
                continue;
            }

            auto pit = plans.find(rev);
            if (pit == plans.end()) pit = plans.emplace(rev, doc->plan_resume(rev)).first;
            const clay_document::ResumePlan* plan = &pit->second;
            clay_document::Seed chosen = seed;
            if (!plan->usable) {
                // The append log cannot carry this brick -- a non-append edit
                // broke its contiguity, which is every touch_region front. The
                // frontier path (#360) is the second way forward: same
                // checkpoint shape, same suffix compiler, same seeded walk --
                // only the seed is the entry's PREFIX half and the suffix is
                // roots[boundary..end) instead of the log's tail.
                const clay_document::FrontierSeed fs =
                    doc->frontier_seed_for(requests[i], per, resume_pad, want_colour, has_below);
                if (!fs.seed.values) continue;
                auto fit = fplans.find(fs.boundary);
                if (fit == fplans.end())
                    fit = fplans.emplace(fs.boundary, doc->plan_frontier(fs.boundary)).first;
                if (!fit->second.usable) continue;
                plan = &fit->second;
                chosen = fs.seed;
            }

            ResumeTask task;
            std::size_t samples = 0;
            if (read_grid(requests[i].origin, requests[i].spacing, requests[i].dims, &task.grid,
                          &samples) != CLAY_OK)
                continue;
            task.slot = i;
            task.plan = plan;
            task.seed = chosen;
            units += per * plan->appended.size();
            tasks.push_back(task);
        }

        // THE COPY, in its own pass so the staging can be sized to the tasks
        // that exist rather than to the batch that might have needed it. Still
        // under the lock: `Seed` names memory the seed store owns, and the
        // whole point is that no one holds such a pointer once it drops.
        if (has_below && !tasks.empty()) {
            stage_active.resize(tasks.size() * per);
            stage_below.resize(tasks.size() * per);
            if (want_colour) {
                stage_active_rgb.resize(tasks.size() * per * 3);
                stage_below_rgb.resize(tasks.size() * per * 3);
            }
        }
        for (std::size_t t = 0; t < tasks.size(); ++t) {
            ResumeTask& task = tasks[t];
            if (has_below) {
                task.active = stage_active.data() + t * per;
                task.below = stage_below.data() + t * per;
                std::memcpy(stage_below.data() + t * per, task.seed.below, per * sizeof(float));
                if (want_colour) {
                    task.active_rgb = stage_active_rgb.data() + t * per * 3;
                    task.below_rgb = stage_below_rgb.data() + t * per * 3;
                    std::memcpy(stage_below_rgb.data() + t * per * 3, task.seed.below_colors,
                                per * 3 * sizeof(float));
                }
            } else {
                task.active = out_values + task.slot * per;
                task.active_rgb = want_colour ? out_colors_rgb + task.slot * per * 3 : nullptr;
            }
            std::memcpy(task.active, task.seed.values, per * sizeof(float));
            if (task.active_rgb)
                std::memcpy(task.active_rgb, task.seed.colors, per * 3 * sizeof(float));
            // The seed's frames are THIS BRICK's -- `emits` turns on the
            // brick's own cull, which is why they travel with the seed rather
            // than with the batch-wide plan, and why the plan states no frames
            // at all. So the check that the seed belongs to THIS append cannot
            // come from the plan: it comes from the append target itself, whose
            // enclosing groups are what a resumable checkpoint's frames name.
            //
            // Without it a seed taken while the tail was a group is read for an
            // append to the ROOT list, the suffix compile refuses, and every
            // dab of a root-list stroke over a grouped document takes a full
            // walk -- measured at 0.045 -> 0.61 ms/dab at 1000 items.
            const std::vector<scene::NodeId>& want_chain = chain_for(task.plan);
            const bool groups_match = [&] {
                if (!task.seed.frames) return false;
                const auto& f = *task.seed.frames;
                if (f.size() != want_chain.size()) return false;
                for (std::size_t k = 0; k < f.size(); ++k)
                    if (f[k].group != want_chain[k]) return false;
                return true;
            }();
            if (task.seed.stack_levels > 0 && task.seed.frames && groups_match) {
                const std::size_t n = per * task.seed.stack_levels;
                task.stack.assign(task.seed.stack, task.seed.stack + n);
                if (task.active_rgb && task.seed.stack_colors)
                    task.stack_rgb.assign(task.seed.stack_colors, task.seed.stack_colors + n * 3);
                task.stack_levels = task.seed.stack_levels;
                task.layer_have_acc = task.seed.layer_have_acc;
                task.frames = *task.seed.frames;
            }
            task.seed = clay_document::Seed{};  // nothing may read it past here
        }
    }

    // -- released ---------------------------------------------------------
    //
    // Nothing here reads the seed store. What it does read -- the document and
    // the cull index snapshot -- is what the full path reads unlocked too: the
    // ABI's contract is that a mutating clay_document_* call is not concurrent
    // with a refill (clay.h, THREADING), while any number of refills and
    // readers may be. Each task writes its own brick's samples and nothing
    // else, so the tasks are disjoint by construction.
    auto run_task = [&](ResumeTask& t, std::vector<float>& points) {
        const math::Aabb box = request_brick_box(requests[t.slot]).dilated(requests[t.slot].band);
        scene::CullRegion cull{box};
        // The checkpoint is PER BRICK where it has frames, because a frame's
        // `emits` depends on this brick's own cull. The plan supplies what is
        // batch-wide -- the layer and the appended ids -- and the seed the rest.
        scene::TapeCheckpoint cp = t.plan->checkpoint;
        cp.frames = t.frames;
        // Per brick for the same reason `frames` is: whether the chain the
        // checkpoint sits in produced anything is a statement about THIS
        // brick's cull, and the seed is what was there when it was taken.
        if (t.stack_levels > 0) cp.layer_have_acc = t.layer_have_acc;
        scene::Tape suffix;
        scene::TapeCheckpoint next;
        const bool resumable =
            scene::compile_layer_suffix(cp, doc->doc.document, t.plan->appended, &suffix, &next,
                                        &cull, index.get());
        if (!resumable) {
            // REBUILD: one full walk of the active half -- what this brick
            // would have cost anyway -- taking the stack where its own
            // checkpoint sits so the dabs after it resume. Once per stroke
            // rather than once per dab, and it is how a brick whose seed has
            // no stack ever gets one.
            scene::TapeCheckpoint own;
            const scene::Tape whole = scene::compile_document_part_resumable(
                doc->doc.document, t.plan->active, /*below=*/false, &cull, index.get(), &own);
            const eval::GridQuery& gq = t.grid;
            points.resize(per * 3);
            std::size_t m = 0;
            for (int k = 0; k < gq.nz; ++k)
                for (int j = 0; j < gq.ny; ++j)
                    for (int x = 0; x < gq.nx; ++x) {
                        const kernel::cfloat3 pt =
                            gq.origin + kernel::cf3(static_cast<float>(x) * gq.spacing,
                                                    static_cast<float>(j) * gq.spacing,
                                                    static_cast<float>(k) * gq.spacing);
                        points[m * 3] = pt.x;
                        points[m * 3 + 1] = pt.y;
                        points[m * 3 + 2] = pt.z;
                        ++m;
                    }
            eval::PointQuery pqr;
            pqr.points_xyz = points.data();
            pqr.count = per;
            eval::PointResults prr;
            prr.distances = t.active;
            prr.colors_rgb = t.active_rgb;
            // ONE walk where a stack is wanted: the field is the top of the
            // stack, so asking for both costs what asking for either did. A
            // stack that came back the wrong shape is simply not stored, but
            // the field it produced alongside is still the brick's answer.
            if (own.valid && !own.frames.empty()) {
                // Sized for the deepest the tape can reach, not for what the
                // checkpoint claims: the walk writes what it finds, and the
                // two disagreeing is the bug this once had. The shape is
                // CHECKED below, after the write, where it is safe to be wrong.
                const std::size_t room = eval::tape_stack_depth(whole);
                const std::size_t want =
                    scene::checkpoint_stack_levels(own.frames, own.layer_have_acc);
                t.snap.assign(per * std::max(room, want), 0.0f);
                float* snap_rgb = nullptr;
                if (t.active_rgb) {
                    t.snap_rgb.assign(per * std::max(room, want) * 3, 0.0f);
                    snap_rgb = t.snap_rgb.data();
                }
                eval::eval_points_stack(whole, pqr, t.snap.data(), snap_rgb, &t.snap_levels,
                                        own.instrs, 0, &prr);
                if (t.snap_levels == want) {
                    t.frames = own.frames;
                    t.snap_layer_have_acc = own.layer_have_acc;
                    t.snap.resize(per * want);
                    if (snap_rgb) t.snap_rgb.resize(per * want * 3);
                } else {
                    t.snap_levels = 0;
                    t.snap_rgb.clear();
                }
            } else {
                eval::Backend* cpu_b = eval::Registry::instance().find("cpu");
                if (!cpu_b || cpu_b->eval_points(whole, pqr, prr) != eval::Status::Ok) return;
            }
            if (t.below)
                fold_layers_below(t.below, t.below_rgb, t.active, t.active_rgb, per,
                                  out_values + t.slot * per,
                                  want_colour ? out_colors_rgb + t.slot * per * 3 : nullptr);
            t.ok = true;
            return;
        }
        const eval::GridQuery& g = t.grid;
        points.resize(per * 3);
        std::size_t at = 0;
        for (int k = 0; k < g.nz; ++k)
            for (int j = 0; j < g.ny; ++j)
                for (int x = 0; x < g.nx; ++x) {
                    const kernel::cfloat3 pt =
                        g.origin + kernel::cf3(static_cast<float>(x) * g.spacing,
                                               static_cast<float>(j) * g.spacing,
                                               static_cast<float>(k) * g.spacing);
                    points[at * 3] = pt.x;
                    points[at * 3 + 1] = pt.y;
                    points[at * 3 + 2] = pt.z;
                    ++at;
                }
        eval::PointQuery pq;
        pq.points_xyz = points.data();
        pq.count = per;
        eval::PointResults pr;
        pr.distances = t.active;
        pr.colors_rgb = t.active_rgb;  // null unless the batch asked for colour
        // In place: the seed was copied here under the lock, and the answer
        // lands on top of it.
        if (t.stack_levels == 0) {
            // In place, as always: with nothing open above it the answer IS
            // the accumulator the next dab folds onto.
            eval::eval_points_seeded(suffix, pq, t.active, t.active_rgb, pr);
        } else {
            // Inside a group the answer is NOT the next seed -- the group's
            // combine has folded the chain into what sits above it -- so the
            // walk snapshots where the checkpoint sits on its way past, and
            // one walk produces the field and the next stack together.
            // The NEXT checkpoint's shape, which is not always this one's: a
            // dab into a group that was EMPTY gives the group's chain its
            // first value, so the stack goes from one plane to two. Sizing by
            // the incoming count would drop the stack on exactly the dab that
            // made the group worth resuming into.
            const std::size_t want_next =
                scene::checkpoint_stack_levels(next.frames, next.layer_have_acc);
            const std::size_t room_next =
                std::max(want_next, t.stack_levels + eval::tape_stack_depth(suffix));
            t.snap.assign(per * room_next, 0.0f);
            if (!t.stack_rgb.empty()) t.snap_rgb.assign(per * room_next * 3, 0.0f);
            eval::eval_points_seeded_stack(
                suffix, pq, t.stack.data(), t.stack_rgb.empty() ? nullptr : t.stack_rgb.data(),
                t.stack_levels, pr, t.snap.data(),
                t.snap_rgb.empty() ? nullptr : t.snap_rgb.data(), &t.snap_levels, next.instrs);
            if (t.snap_levels != want_next) {
                t.snap_levels = 0;
            } else {
                t.snap.resize(per * want_next);
                if (!t.snap_rgb.empty()) t.snap_rgb.resize(per * want_next * 3);
                t.frames = next.frames;
                t.snap_layer_have_acc = next.layer_have_acc;
            }
        }
        if (t.below)
            fold_layers_below(t.below, t.below_rgb, t.active, t.active_rgb, per,
                              out_values + t.slot * per,
                              want_colour ? out_colors_rgb + t.slot * per * 3 : nullptr);
        t.ok = true;
    };
    constexpr std::size_t kResumeParallelUnits = 256u * 1024u;
    if (tasks.size() > 1 && units >= kResumeParallelUnits) {
        parallel::ThreadPool::instance().parallel_for(
            tasks.size(), 1, [&](std::size_t task_b, std::size_t task_e) {
                std::vector<float> points;  // one per chunk, reused across its bricks
                for (std::size_t t = task_b; t < task_e; ++t) run_task(tasks[t], points);
            });
    } else {
        std::vector<float> points;
        for (ResumeTask& t : tasks) run_task(t, points);
    }
    for (const ResumeTask& t : tasks)
        if (t.ok) {
            resumed[t.slot] = 1;
            ++resumed_count;
        }

    // -- retaken ----------------------------------------------------------
    //
    // The revision check is the one `store_seeds` already makes for the full
    // path: a document that moved while the lock was down means these answer a
    // revision that is no longer current, and keeping them as seeds would lie.
    // The values still go back to the caller -- they are the batch it asked
    // for, at the revision it asked at.
    if (!tasks.empty()) {
        // The test seam (clay_internal.h): single-threaded nothing can edit
        // the document between the walks above and the lock below, which is
        // exactly why the stale case the gate refuses needs this to be
        // reachable at all. Unarmed -- every call outside one test -- it is a
        // null check.
        doc->run_resume_store_interleave();
        std::lock_guard<std::mutex> lock(doc->cache_lock());
        const std::uint64_t now = doc->current_revision();
        for (const ResumeTask& t : tasks) {
            if (!t.ok || t.plan->now != now) continue;
            // The FIELD always, and the stack beside it when this task made one.
            const bool have_stack =
                t.snap_levels > 0 && scene::checkpoint_stack_levels(t.frames,
                                                                    t.snap_layer_have_acc) ==
                                         t.snap_levels;
            doc->store_active(requests[t.slot], t.plan->now, resume_pad, t.active, t.active_rgb,
                              per, have_stack ? t.snap.data() : nullptr,
                              have_stack && !t.snap_rgb.empty() ? t.snap_rgb.data() : nullptr,
                              have_stack ? static_cast<std::uint32_t>(t.snap_levels) : 0,
                              have_stack ? &t.frames : nullptr, t.snap_layer_have_acc);
        }
    }
    return resumed_count;
}
}  // namespace

clay_result clay_brick_cache_eval_requests(const clay_document* doc, const char* backend,
                                           const clay_brick_request* requests, size_t count,
                                           float* out_values, size_t values_capacity,
                                           float* out_colors_rgb, size_t colors_capacity) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (count > 0 && (!requests || !out_values))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null requests or values");
    clay_result r = check_batch("brick requests", count);
    if (r != CLAY_OK) return r;
    if (count == 0) {
        if (values_capacity != 0 || colors_capacity != 0)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "no requests, but a non-empty buffer");
        return CLAY_OK;
    }
    // One stride for the whole batch, so brick i lands at i * per whatever
    // order the work is done in. Requests come from one cache and so share a
    // lattice size; a batch that does not is a caller mixing two caches.
    eval::GridQuery first;
    std::size_t per = 0;
    r = read_grid(requests[0].origin, requests[0].spacing, requests[0].dims, &first, &per);
    if (r != CLAY_OK) return r;
    r = exact_capacity("brick values", count, per, values_capacity);
    if (r != CLAY_OK) return r;
    r = optional_capacity("brick colours", out_colors_rgb, count, per * 3, colors_capacity);
    if (r != CLAY_OK) return r;
    r = check_uniform_dims(requests, count);
    if (r != CLAY_OK) return r;
    const char* name = backend ? backend : "cpu";
    eval::Backend* b = eval::Registry::instance().find(name);
    if (!b) return fail(CLAY_ERROR_NOT_FOUND, std::string("backend not registered: ") + name);
    // The resumable path (#306, #348), which answers what it can from the seeds
    // it kept last dab and leaves the rest to the full walk below.
    std::vector<std::uint8_t> resumed(count, 0);
    const bool want_colour = out_colors_rgb != nullptr;
    const std::size_t resumed_count = resume_bricks(doc, requests, count, per, want_colour,
                                                    out_values, out_colors_rgb, resumed.data());
    doc->note_refill(resumed_count, count - resumed_count);
    if (resumed_count == count) return CLAY_OK;

    // The bricks the resumable path did not answer, gathered so the batch stays
    // a batch, then scattered back to their fixed slots.
    std::vector<clay_brick_request> misses;
    std::vector<std::size_t> where;
    const bool partial = resumed_count > 0;
    if (partial) {
        misses.reserve(count - resumed_count);
        where.reserve(count - resumed_count);
        for (std::size_t i = 0; i < count; ++i)
            if (!resumed[i]) {
                misses.push_back(requests[i]);
                where.push_back(i);
            }
    }
    const clay_brick_request* todo = partial ? misses.data() : requests;
    const std::size_t todo_count = partial ? misses.size() : count;

    // Which layer an append would extend, and whether anything sits beneath it.
    scene::LayerId active_layer = 0;
    int visible_sdf = 0;
    for (const scene::Layer& l : doc->doc.document.layers)
        if (l.visible && l.kind == scene::LayerKind::Sdf && l.sdf) {
            active_layer = l.id;
            ++visible_sdf;
        }
    const bool has_below = visible_sdf > 1;

    std::vector<float> act(todo_count * per);
    std::vector<float> act_rgb(want_colour ? todo_count * per * 3 : 0);
    std::vector<float> bel(has_below ? todo_count * per : 0);
    std::vector<float> bel_rgb(has_below && want_colour ? todo_count * per * 3 : 0);

    // THE SEED A GROUP RESUME NEEDS, taken here or never.
    //
    // A brick's first seed comes from this path, and what it stores is what
    // the next dab continues from. For an append to the ROOT list that is the
    // field itself -- the chain's accumulator IS the answer -- which is why a
    // root-list stroke has never paid for a first touch. A dab inside a group
    // continues the group's OWN chain, which the group's combine has already
    // folded away by the time the field exists, so the field alone cannot
    // serve and the brick had to walk the whole active half again on its first
    // dab. Measured: 1040 such walks over a 24-dab stroke, 0.85 ms each, which
    // was the entire remaining gap to the root list.
    //
    // The walk that produces the field is the same walk that passes the
    // checkpoint, so taking the stack on the way costs nothing. It is the
    // rebuild's own fix, moved to where the brick is first filled.
    //
    // NOT free on every backend: a grid batch that ran on the GPU produces no
    // stack, and evaluating a second time on the CPU to get one would cost
    // more than the first touch it saves. So the fused walk is taken only when
    // the batch was going to run on the CPU anyway; elsewhere this stores what
    // is free (below) and the first touch pays as before.
    const bool fused = b == eval::Registry::instance().find("cpu");
    std::vector<clay_document::SeedStack> stacks(todo_count);
    std::vector<std::vector<float>> stack_of(todo_count);
    std::vector<std::vector<float>> stack_rgb_of(todo_count);
    std::vector<std::vector<scene::TapeCheckpointFrame>> frames_of(todo_count);

    auto take_stacks = [&](std::size_t base, std::size_t n, const std::vector<scene::Tape>& tapes,
                           const std::vector<scene::TapeCheckpoint>& cps) {
        if (cps.size() < n) return;
        parallel::ThreadPool::instance().parallel_for(
            n, 1, [&](std::size_t lo, std::size_t hi) {
                std::vector<float> points;
                for (std::size_t i = lo; i < hi; ++i) {
                    const std::size_t j = base + i;
                    const scene::TapeCheckpoint& cp = cps[i];
                    float* fd = act.data() + j * per;
                    float* fc = act_rgb.empty() ? nullptr : act_rgb.data() + j * per * 3;
                    // No frames is a root-list checkpoint: the field is the
                    // seed and always was. Nothing to take.
                    if (!cp.valid || cp.frames.empty()) continue;
                    const std::size_t levels =
                        scene::checkpoint_stack_levels(cp.frames, cp.layer_have_acc);
                    if (levels == 0) continue;
                    // FREE: the checkpoint sits at the very end of the tape and
                    // nothing is open above it, so the field IS the one plane.
                    // Half the bricks of a group stroke are this shape and cost
                    // nothing at all to seed.
                    if (levels == 1 && cp.instrs == tapes[i].instrs.size()) {
                        stack_of[j].assign(fd, fd + per);
                        if (fc) stack_rgb_of[j].assign(fc, fc + per * 3);
                        frames_of[j] = cp.frames;
                        stacks[j].values = stack_of[j].data();
                        stacks[j].colors = fc ? stack_rgb_of[j].data() : nullptr;
                        stacks[j].levels = static_cast<std::uint32_t>(levels);
                        stacks[j].layer_have_acc = cp.layer_have_acc;
                        stacks[j].frames = &frames_of[j];
                        continue;
                    }
                    if (!fused) continue;
                    // Otherwise the planes have to come off the walk, so the
                    // walk produces the field here instead of the grid batch.
                    eval::GridQuery g;
                    std::size_t samples = 0;
                    if (read_grid(todo[j].origin, todo[j].spacing, todo[j].dims, &g, &samples) !=
                        CLAY_OK)
                        continue;
                    points.resize(per * 3);
                    std::size_t m = 0;
                    for (int z = 0; z < g.nz; ++z)
                        for (int y = 0; y < g.ny; ++y)
                            for (int x = 0; x < g.nx; ++x) {
                                const kernel::cfloat3 pt =
                                    g.origin + kernel::cf3(static_cast<float>(x) * g.spacing,
                                                           static_cast<float>(y) * g.spacing,
                                                           static_cast<float>(z) * g.spacing);
                                points[m * 3] = pt.x;
                                points[m * 3 + 1] = pt.y;
                                points[m * 3 + 2] = pt.z;
                                ++m;
                            }
                    eval::PointQuery pq;
                    pq.points_xyz = points.data();
                    pq.count = per;
                    eval::PointResults pr;
                    pr.distances = fd;
                    pr.colors_rgb = fc;
                    // Room for what the tape can reach, checked after: a count
                    // handed to a buffer length is the bug this once had.
                    const std::size_t room =
                        std::max(eval::tape_stack_depth(tapes[i]), levels);
                    stack_of[j].assign(per * room, 0.0f);
                    if (fc) stack_rgb_of[j].assign(per * room * 3, 0.0f);
                    std::size_t got = 0;
                    eval::eval_points_stack(tapes[i], pq, stack_of[j].data(),
                                            fc ? stack_rgb_of[j].data() : nullptr, &got, cp.instrs,
                                            0, &pr);
                    if (got != levels) {
                        stack_of[j].clear();
                        stack_rgb_of[j].clear();
                        continue;
                    }
                    stack_of[j].resize(per * levels);
                    if (fc) stack_rgb_of[j].resize(per * levels * 3);
                    frames_of[j] = cp.frames;
                    stacks[j].values = stack_of[j].data();
                    stacks[j].colors = fc ? stack_rgb_of[j].data() : nullptr;
                    stacks[j].levels = static_cast<std::uint32_t>(levels);
                    stacks[j].layer_have_acc = cp.layer_have_acc;
                    stacks[j].frames = &frames_of[j];
                }
            });
    };

    // The ACTIVE half -- or the whole document when nothing is beneath it, in
    // which case the two are the same tape and only one batch is run.
    clay_result br = eval_requests_in_chunks(
        doc, todo, todo_count,
        [&](const eval::GridBatchQuery& bq, std::size_t base) -> clay_result {
            if (b->eval_grid_batch(bq, act.data() + base * per,
                                   act_rgb.empty() ? nullptr : act_rgb.data() + base * per * 3) !=
                eval::Status::Ok)
                return fail(CLAY_ERROR_BACKEND, "eval_grid_batch failed");
            return CLAY_OK;
        },
        has_below ? ChunkHalf::Active : ChunkHalf::Whole, active_layer, take_stacks);
    if (br != CLAY_OK) return br;
    if (has_below) {
        br = eval_requests_in_chunks(
            doc, todo, todo_count,
            [&](const eval::GridBatchQuery& bq, std::size_t base) -> clay_result {
                if (b->eval_grid_batch(
                        bq, bel.data() + base * per,
                        bel_rgb.empty() ? nullptr : bel_rgb.data() + base * per * 3) !=
                    eval::Status::Ok)
                    return fail(CLAY_ERROR_BACKEND, "eval_grid_batch failed");
                return CLAY_OK;
            },
            ChunkHalf::Below, active_layer);
        if (br != CLAY_OK) return br;
    }

    for (std::size_t j = 0; j < todo_count; ++j) {
        const std::size_t slot = partial ? where[j] : j;
        float* vd = out_values + slot * per;
        float* vc = want_colour ? out_colors_rgb + slot * per * 3 : nullptr;
        if (has_below)
            fold_layers_below(
                bel.data() + j * per, bel_rgb.empty() ? nullptr : bel_rgb.data() + j * per * 3,
                act.data() + j * per, act_rgb.empty() ? nullptr : act_rgb.data() + j * per * 3, per,
                vd, vc);
        else {
            std::memcpy(vd, act.data() + j * per, per * sizeof(float));
            if (vc) std::memcpy(vc, act_rgb.data() + j * per * 3, per * 3 * sizeof(float));
        }
    }
    // Kept so the NEXT dab can resume from it. The ACTIVE half is the seed a
    // suffix continues; the half beneath is what the union needs and does not
    // move while the active layer is being sculpted.
    doc->store_seeds(todo, todo_count, act.data(), act_rgb.empty() ? nullptr : act_rgb.data(),
                     has_below ? bel.data() : nullptr, bel_rgb.empty() ? nullptr : bel_rgb.data(),
                     per, 0, 0.0f, stacks.data());
    return CLAY_OK;
}

clay_result clay_brick_cache_submit(clay_brick_cache* cache,
                                    const clay_brick_request* requests, size_t count,
                                    const float* values, size_t values_capacity,
                                    const float* colors_rgb, size_t colors_capacity,
                                    int32_t* out_results, size_t* out_accepted) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    if (count > 0 && (!requests || !values))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null requests or values");
    if (!out_results && !out_accepted)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "submit needs out_results or out_accepted: with neither, a caller cannot "
                    "tell an accepted brick from a stale one");
    clay_result r = check_batch("brick requests", count);
    if (r != CLAY_OK) return r;
    const brick::BrickConfig& config = cache->cache.config();
    const std::size_t per = brick_samples(config);
    r = exact_capacity("brick values", count, per, values_capacity);
    if (r != CLAY_OK) return r;
    // Required with colour and refused without it, both ways round: a brick
    // with a colour lattice and a brick without one are not the same brick to
    // read back, so there is no cache that takes colour optionally.
    if (config.colors && count > 0 && !colors_rgb)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this cache was created with colours, so submit needs colors_rgb");
    if (!config.colors && colors_rgb)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this cache was created without colours and has nowhere to store them");
    r = optional_capacity("brick colours", colors_rgb, count, per * 3, colors_capacity);
    if (r != CLAY_OK) return r;
    r = check_requests_match(requests, count, config);
    if (r != CLAY_OK) return r;
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < count; ++i) {
        brick::BrickRequest req;
        // Through void*: BrickRequest is trivially copyable (asserted above)
        // but has default member initializers, which is enough for
        // -Wclass-memaccess to object to the typed form.
        std::memcpy(static_cast<void*>(&req), static_cast<const void*>(&requests[i]),
                    sizeof req);
        brick::SubmitResult result = cache->cache.submit(
            req, values + i * per, colors_rgb ? colors_rgb + i * per * 3 : nullptr);
        if (result == brick::SubmitResult::Accepted) ++accepted;
        if (out_results) out_results[i] = to_c_submit(result);
    }
    if (out_accepted) *out_accepted = accepted;
    return CLAY_OK;
}

clay_result clay_brick_cache_brick_bounds(const clay_brick_cache* cache, const int32_t key[3],
                                          float out_min[3], float out_max[3]) {
    if (!cache || !key) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or key");
    math::Aabb box = cache->cache.brick_bounds(to_brick_key(key));
    if (out_min) write_f3(out_min, box.min);
    if (out_max) write_f3(out_max, box.max);
    return CLAY_OK;
}

clay_result clay_brick_cache_cull_region(const clay_brick_cache* cache, const int32_t key[3],
                                         float out_min[3], float out_max[3]) {
    if (!cache || !key) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or key");
    math::Aabb box = cache->cache.cull_region(to_brick_key(key));
    if (out_min) write_f3(out_min, box.min);
    if (out_max) write_f3(out_max, box.max);
    return CLAY_OK;
}

clay_result clay_brick_cache_sample(const clay_brick_cache* cache, const int32_t key[3],
                                    int32_t i, int32_t j, int32_t k, float* out_value) {
    if (!cache || !key || !out_value)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache, key or out pointer");
    const std::int32_t dim = cache->cache.config().dim;
    if (i < 0 || j < 0 || k < 0 || i >= dim || j >= dim || k >= dim)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "lattice index outside [0, " + std::to_string(dim) + ")");
    *out_value = cache->cache.sample(to_brick_key(key), i, j, k);
    return CLAY_OK;
}

clay_result clay_brick_cache_read_bricks(const clay_brick_cache* cache, int32_t lod,
                                         const int32_t* keys_xyz, size_t count, int32_t apron,
                                         int32_t* out_states, uint16_t* out_halves,
                                         size_t values_capacity, uint8_t* out_colors_rgba,
                                         size_t colors_capacity) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    if (lod != 0 && lod != 1)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "lod must be 0 (full resolution) or 1 (mip), got " + std::to_string(lod));
    if (count > 0 && !keys_xyz) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null keys");
    if (!out_states && !out_halves && !out_colors_rgba)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "read_bricks needs out_states, out_halves or out_colors_rgba");
    clay_result r = check_batch("bricks", count);
    if (r != CLAY_OK) return r;
    const brick::BrickConfig& config = cache->cache.config();
    // Refused rather than clamped, for the reason lod > 1 is: past a brick's
    // own width the tile is mostly neighbour, and what the caller wants there
    // is a coarser lod, not a fatter apron.
    if (apron < 0 || apron > config.dim)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "apron must be in [0, dim] = [0, " + std::to_string(config.dim) +
                        "], got " + std::to_string(apron));
    if (out_colors_rgba && !config.colors)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "this cache was created without colours and stores none to read");
    // A mip subsamples DISTANCES; averaging colour over its 2x2x2 block would
    // be a filtering policy chosen on the caller's behalf, so it is reported
    // rather than invented.
    if (out_colors_rgba && lod != 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a mip carries no colour lattice");
    const std::size_t per = padded_samples(config, apron);
    r = optional_capacity("brick half", out_halves, count, per, values_capacity);
    if (r != CLAY_OK) return r;
    r = optional_capacity("brick colour", out_colors_rgba, count, per * 4, colors_capacity);
    if (r != CLAY_OK) return r;
    for (std::size_t i = 0; i < count; ++i) {
        const brick::BrickKey key = to_brick_key(keys_xyz + i * 3);
        const brick::Brick* b = cache->cache.find_lod(lod, key);
        // MISSING leaves the WHOLE padded slice untouched. The rule is about
        // the key, not its neighbourhood: a caller wanting a tile of band
        // values for a brick that does not exist can synthesize one.
        if (out_states) out_states[i] = b ? to_c_brick_state(b->state) : CLAY_BRICK_MISSING;
        if (!b) continue;
        if (out_halves) cache->cache.read_padded(lod, key, apron, out_halves + i * per);
        if (out_colors_rgba)
            cache->cache.read_colors_padded(
                key, apron, reinterpret_cast<brick::BrickColor*>(out_colors_rgba) + i * per);
    }
    return CLAY_OK;
}

clay_result clay_brick_cache_surface_bricks(const clay_brick_cache* cache,
                                            int32_t* out_keys_xyz, size_t* count) {
    if (!cache || !count)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or count");
    std::vector<brick::BrickKey> keys = cache->cache.surface_bricks();
    if (!out_keys_xyz) {
        *count = keys.size();
        return CLAY_OK;
    }
    if (*count < keys.size()) {
        *count = keys.size();
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL,
                    "the surface brick list needs " + std::to_string(keys.size()) + " keys");
    }
    if (!keys.empty())
        std::memcpy(out_keys_xyz, keys.data(), keys.size() * sizeof(brick::BrickKey));
    *count = keys.size();
    return CLAY_OK;
}

clay_result clay_brick_cache_build_mip(clay_brick_cache* cache, const int32_t coarse_key[3],
                                       int32_t* out_built) {
    if (!cache || !coarse_key || !out_built)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache, key or out pointer");
    *out_built = cache->cache.build_mip(to_brick_key(coarse_key)) ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_brick_cache_current_lod(const clay_brick_cache* cache,
                                         const int32_t coarse_key[3], int32_t* out_lod) {
    if (!cache || !coarse_key || !out_lod)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache, key or out pointer");
    *out_lod = cache->cache.current_lod(to_brick_key(coarse_key));
    return CLAY_OK;
}

namespace {

// "Not built" and "no surface" must stay distinguishable: an empty mesh already
// means the latter — an ordinary state of a session — so a level that was never
// built is reported instead. At lod 0 a key that stores no lattice is an
// ordinary uniform brick and is not an error; at lod 1 there is no uniform mip,
// so an absent one is a "not yet" the caller acts on with build_mip.
clay_result brick_cache_level_available(const clay_brick_cache* cache, std::int32_t lod,
                                        const std::vector<brick::BrickKey>& subset,
                                        bool whole_level) {
    if (lod != 1) return CLAY_OK;
    for (const brick::BrickKey& key : subset)
        if (!cache->cache.find_mip(key))
            return fail(CLAY_ERROR_NOT_FOUND,
                        "no level-1 mip for coarse key (" + std::to_string(key.x) + ", " +
                            std::to_string(key.y) + ", " + std::to_string(key.z) +
                            "): build it with clay_brick_cache_build_mip");
    // memory_usage() is the surface payload and is 0 exactly when the cache
    // stores no surface brick — an O(1) way to tell an unbuilt level from an
    // empty cache, which still meshes empty at every level.
    if (whole_level && cache->cache.mip_count() == 0 && cache->cache.memory_usage() > 0)
        return fail(CLAY_ERROR_NOT_FOUND,
                    "no level-1 mip has been built: clay_brick_cache_build_mip takes the "
                    "coarse keys");
    return CLAY_OK;
}

// The one body behind clay_brick_cache_mesh and clay_brick_cache_mesh_lod, so
// the older call is the newer one at lod 0 by construction rather than by
// two implementations agreeing.
clay_result brick_cache_mesh_at(const clay_brick_cache* cache, const clay_document* doc,
                                const clay_brick_mesh_params* params, std::int32_t lod,
                                const int32_t* keys_xyz, size_t key_count,
                                clay_brick_mesh_range* out_ranges, clay_mesh** out_mesh) {
    if (!cache || !params || !out_mesh)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache, params or out_mesh");
    if (lod != 0 && lod != 1)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "lod must be 0 (full resolution) or 1 (mip), got " + std::to_string(lod));
    if (key_count > 0 && !keys_xyz)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a key count without keys");
    // Ranges need a key list: with none there is no count the caller could have
    // sized out_ranges from, and this ABI infers no length anywhere else.
    if (out_ranges && !keys_xyz)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "out_ranges needs keys_xyz: without one there is no count to size it by");
    clay_result r = check_batch("brick keys", key_count);
    if (r != CLAY_OK) return r;
    clay_brick_mesh_params p;
    r = read_desc(params, kBrickMeshParamsOriginal, &p);
    if (r != CLAY_OK) return r;
    if (!normal_mode_is_known(p.normals))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown normal mode: " + std::to_string(p.normals));
    mesh::MeshingOptions options;
    options.normals = static_cast<mesh::NormalMode>(p.normals);
    options.colors = p.colors != 0;
    if (p.gradient_eps > 0.0f) options.gradient_eps = p.gradient_eps;
    // Gradient normals and colours are attributes of the FIELD, so without a
    // document there is nothing to read them from. Refused rather than quietly
    // downgraded: a mesh with the wrong shading is not obviously wrong.
    if (!doc && (options.normals == mesh::NormalMode::Gradient || options.colors))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "gradient normals and colours need a document to sample");
    // A coarse vertex sits on the MIP's surface rather than the field's, so the
    // per-brick culled tape that makes these attributes exact at lod 0 is only
    // both-out-of-band there, not equal. Refused rather than approximated, the
    // same answer read_bricks gives for a colour at lod 1.
    if (lod != 0 && (options.normals == mesh::NormalMode::Gradient || options.colors))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "gradient normals and colours are level 0 only: a mip's vertices are not on "
                    "the field's surface and it carries no colour lattice");
    // NULL keys means "every brick this level stores", which is what this call
    // did before the key list existed and what an export wants.
    std::vector<brick::BrickKey> subset;
    if (keys_xyz) {
        subset.reserve(key_count);
        for (std::size_t i = 0; i < key_count; ++i)
            subset.push_back(to_brick_key(keys_xyz + i * 3));
    }
    r = brick_cache_level_available(cache, lod, subset, keys_xyz == nullptr);
    if (r != CLAY_OK) return r;
    std::vector<mesh::BrickMeshRange> ranges;
    auto* handle = new clay_mesh();
    // The document rather than its compiled tape: gradient normals and colours
    // are evaluated through per-brick CULLED tapes, so their cost follows the
    // bricks named rather than the total document (issue #73). The document's
    // revision-cached cull index rides along so the attribute pass reuses the
    // bounds the refill path just computed.
    std::shared_ptr<const scene::CullIndex> index = doc ? doc->cull_index() : nullptr;
    handle->data = mesh::mesh_bricks(cache->cache, doc ? &doc->doc.document : nullptr, options,
                                     keys_xyz ? &subset : nullptr, out_ranges ? &ranges : nullptr,
                                     index.get(), lod);
    if (out_ranges)
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            out_ranges[i].key[0] = ranges[i].key.x;
            out_ranges[i].key[1] = ranges[i].key.y;
            out_ranges[i].key[2] = ranges[i].key.z;
            out_ranges[i].vertex_first = ranges[i].vertex_first;
            out_ranges[i].vertex_count = ranges[i].vertex_count;
            out_ranges[i].index_first = ranges[i].index_first;
            out_ranges[i].index_count = ranges[i].index_count;
        }
    *out_mesh = handle;
    return CLAY_OK;
}

}  // namespace

clay_result clay_brick_cache_mesh(const clay_brick_cache* cache, const clay_document* doc,
                                  const clay_brick_mesh_params* params, const int32_t* keys_xyz,
                                  size_t key_count, clay_brick_mesh_range* out_ranges,
                                  clay_mesh** out_mesh) {
    return brick_cache_mesh_at(cache, doc, params, 0, keys_xyz, key_count, out_ranges, out_mesh);
}

clay_result clay_brick_cache_mesh_lod(const clay_brick_cache* cache, const clay_document* doc,
                                      const clay_brick_mesh_params* params, int32_t lod,
                                      const int32_t* keys_xyz, size_t key_count,
                                      clay_brick_mesh_range* out_ranges, clay_mesh** out_mesh) {
    return brick_cache_mesh_at(cache, doc, params, lod, keys_xyz, key_count, out_ranges, out_mesh);
}

clay_result clay_brick_cache_raycast(const clay_brick_cache* cache, const float origin[3],
                                     const float dir[3], int32_t* out_hit, float* out_t,
                                     float out_position[3], float out_normal[3]) {
    if (!cache) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache");
    math::Ray ray;
    clay_result r = make_ray(origin, dir, &ray);
    if (r != CLAY_OK) return r;
    if (!out_hit) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_hit");
    pick::SceneHit hit = pick::raycast_bricks(cache->cache, ray);
    *out_hit = hit.hit ? 1 : 0;
    if (out_t) *out_t = hit.t;
    if (out_position) write_f3(out_position, hit.position);
    if (out_normal) write_f3(out_normal, hit.normal);
    return CLAY_OK;
}

clay_result clay_brick_cache_raycast_many(const clay_brick_cache* cache,
                                          const float* rays_origin_dir, size_t count,
                                          int32_t* out_hits, float* out_t,
                                          float* out_positions_xyz, float* out_normals_xyz) {
    if (!cache || (count > 0 && !rays_origin_dir))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or rays");
    if (count == 0) return CLAY_OK;  // no rays is no work, not a rejected query
    // Normalized and batch-checked by the same helper clay_raycast_many uses,
    // so a zero-length direction is refused identically on both surfaces.
    std::vector<float> rays;
    clay_result r = normalize_rays(rays_origin_dir, count, &rays);
    if (r != CLAY_OK) return r;
    std::vector<eval::RayHit> hits(count);
    // Rays are independent reads of a const cache, so the batch fans out over
    // the same pool the CPU backend evaluates points with. Each ray is marched
    // by exactly one chunk with the same arithmetic the serial loop used and
    // writes only its own slot, so the outputs match the serial loop bit for
    // bit, in order.
    parallel::ThreadPool::instance().parallel_for(
        count, 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) {
                math::Ray ray;
                ray.origin = kernel::cf3(rays[i * 6], rays[i * 6 + 1], rays[i * 6 + 2]);
                ray.dir = kernel::cf3(rays[i * 6 + 3], rays[i * 6 + 4], rays[i * 6 + 5]);
                const pick::SceneHit hit = pick::raycast_bricks(cache->cache, ray);
                hits[i].hit = hit.hit ? 1 : 0;
                hits[i].t = hit.t;
                write_f3(hits[i].pos, hit.position);
                write_f3(hits[i].normal, hit.normal);
            }
        });
    write_ray_hits(hits, count, out_hits, out_t, out_positions_xyz, out_normals_xyz);
    return CLAY_OK;
}

}  // extern "C" — the mesh-brush plumbing below takes C++ types

// -- fixed-topology mesh brushes ---------------------------------------------

// A sculpting session. The engine's MeshSculptor holds its mesh BY REFERENCE,
// and a mesh layer's triangles can be removed from under it, so the handle
// remembers what it was built over and every call checks that the answer has
// not changed. A layer removed mid-session becomes a refusal rather than a read
// of freed storage.
struct clay_mesh_sculptor {
    clay_mesh* mesh = nullptr;
    mesh::Mesh* bound = nullptr;
    // The layer's geometry revision when this sculptor was built, and the only
    // thing that catches the replacement neither of the two checks above can.
    // See mesh_layer_revision_of. Zero for a standalone mesh, which belongs to
    // no layer and cannot be replaced under anyone.
    std::uint64_t geometry_revision = 0;
    std::unique_ptr<mesh::MeshSculptor> sculptor;
};

struct clay_mesh_deltas {
    mesh::VertexDeltas deltas;
};

// The adaptive surface and its sculptor. OPAQUE, and owning: the surface must
// outlive the sculptor, which is why the sculptor keeps the owner rather than a
// bare reference — a report reads the owner's revisions after the stamp.
struct clay_dynamic_surface {
    mesh::DynamicSurface surface;
};

struct clay_dynamic_sculptor {
    clay_dynamic_surface* owner = nullptr;
    std::unique_ptr<mesh::DynamicSculptor> sculptor;
};

struct clay_mesh_lattice {
    mesh::Lattice cage;
};

namespace {

constexpr std::size_t kMeshBrushDescOriginal =
    offsetof(clay_mesh_brush_desc, smooth_iterations) + sizeof(std::int32_t);
constexpr std::size_t kMeshFrameOriginal = offsetof(clay_mesh_frame, scale) + sizeof(float);
constexpr std::size_t kMeshHitOriginal =
    offsetof(clay_mesh_hit, seed_class) + sizeof(std::uint32_t);

mesh::Mesh* mesh_data_mut(clay_mesh* mesh) {
    if (!mesh) return nullptr;
    if (!mesh->doc) return &mesh->data;
    auto it = mesh->doc->doc.mesh_layers.find(mesh->layer);
    return it == mesh->doc->doc.mesh_layers.end() ? nullptr : &it->second;
}

// Locked and ghosted both mean "never edited", and a vertex displacement is an
// edit. Checked per call rather than at create time: a layer can be locked
// while a session is open, and the lock has to win.
//
// A REMOVED layer is refused here too. Its triangles survive removal — the
// undo stack needs them — so reading a borrowed handle still answers, on the
// ABI's standing rule that reading is not editing. Sculpting content that
// belongs to no layer is a different thing, and it is a mistake.
clay_result mesh_layer_editable(const clay_mesh* mesh) {
    if (!mesh->doc) return CLAY_OK;  // a standalone mesh belongs to no layer
    const scene::Layer* l = mesh->doc->doc.document.find_layer(mesh->layer);
    if (!l) return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer is no longer in its document");
    if (l->protected_from_edits())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("layer ") + std::to_string(mesh->layer) + " is " +
                        (l->ghost ? "ghosted" : "locked") + " and takes no edits");
    return CLAY_OK;
}

clay_result resolve_sculptor(clay_mesh_sculptor* s, bool for_edit) {
    if (!s || !s->sculptor) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh sculptor");
    mesh::Mesh* now = mesh_data_mut(s->mesh);
    if (!now || now != s->bound)
        return fail(CLAY_ERROR_NOT_FOUND,
                    "the mesh this sculptor was built over is no longer in its document");
    if (!s->sculptor->valid())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the mesh changed its vertex or index count under this sculptor");
    // THE CHECK THE OTHER TWO CANNOT MAKE. The pointer comparison above catches
    // a layer that was REMOVED — a std::map node's address is stable, so it
    // does not see the contents replaced. `valid()` catches a changed vertex or
    // index COUNT, which catches most rebuilds and misses the one that matters:
    // a rebuild landing on the same counts would leave this sculptor's
    // adjacency and BVH describing triangles that no longer exist, and every
    // stamp after it would move the wrong vertices without a word.
    if (s->mesh && s->mesh->doc &&
        mesh_layer_revision_of(s->mesh->doc, s->mesh->layer) != s->geometry_revision)
        return fail(CLAY_ERROR_NOT_FOUND,
                    "the mesh layer was rebuilt under this sculptor; build a new one");
    return for_edit ? mesh_layer_editable(s->mesh) : CLAY_OK;
}

bool mesh_brush_is_known(std::int32_t verb) {
    return verb >= 0 && verb <= static_cast<std::int32_t>(mesh::MeshBrush::Smear);
}

bool mesh_falloff_is_known(std::int32_t curve) {
    return curve >= 0 && curve <= static_cast<std::int32_t>(mesh::MeshFalloff::Gaussian);
}

// Every refusal here is a refusal rather than a clamp, on the same footing as
// the mesher enums: a value outside the declared list is a mistake, and mapping
// it onto the default hides the mistake behind a plausible result.
clay_result read_mesh_brush(const clay_mesh_brush_desc* src, mesh::MeshBrush* out_verb,
                            mesh::MeshBrushSettings* out) {
    if (!src) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh brush descriptor");
    clay_mesh_brush_desc d;
    clay_result r = read_desc(src, kMeshBrushDescOriginal, &d);
    if (r != CLAY_OK) return r;
    if (!mesh_brush_is_known(d.verb))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown mesh brush: " + std::to_string(d.verb));
    if (!mesh_falloff_is_known(d.falloff))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown mesh falloff: " + std::to_string(d.falloff));
    if (d.flatten_mode < 0 ||
        d.flatten_mode > static_cast<std::int32_t>(field::FlattenMode::FillOnly))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown flatten mode: " + std::to_string(d.flatten_mode));
    if (!(d.radius > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "brush radius must be > 0");
    // Zero reads as "one pass", because a host that declared only the original
    // layout sends the appended fields as zero.
    if (d.smooth_iterations == 0) d.smooth_iterations = 1;
    if (d.smooth_iterations < 0 || d.smooth_iterations > CLAY_MESH_MAX_SMOOTH_ITERATIONS)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "smooth_iterations must be in 1.." +
                        std::to_string(CLAY_MESH_MAX_SMOOTH_ITERATIONS) + ", got " +
                        std::to_string(d.smooth_iterations));

    *out_verb = static_cast<mesh::MeshBrush>(d.verb);
    out->center = kernel::cf3(d.center[0], d.center[1], d.center[2]);
    out->radius = d.radius;
    out->strength = d.strength;
    out->falloff = static_cast<mesh::MeshFalloff>(d.falloff);
    out->direction = kernel::cf3(d.direction[0], d.direction[1], d.direction[2]);
    out->deposit_normal =
        kernel::cf3(d.deposit_normal[0], d.deposit_normal[1], d.deposit_normal[2]);
    out->geodesic = d.geodesic != 0;
    out->seed_class = d.seed_class;
    out->flatten_mode = static_cast<field::FlattenMode>(d.flatten_mode);
    out->use_given_plane = d.use_given_plane != 0;
    out->plane_point = kernel::cf3(d.plane_point[0], d.plane_point[1], d.plane_point[2]);
    out->plane_normal = kernel::cf3(d.plane_normal[0], d.plane_normal[1], d.plane_normal[2]);
    out->polish_angle = d.polish_angle;
    out->smooth_iterations = d.smooth_iterations;
    // Appended fields, all of which read ZERO as today's behaviour, because a
    // host that declared only the original layout sends them as zero.
    //
    // layer_height is the exception worth naming: zero would make LAYER a
    // silent no-op rather than an error, so it reads as the engine default the
    // way smooth_iterations does. Every other verb ignores it.
    if (d.layer_height != 0.0f) out->layer_height = d.layer_height;
    if (d.alpha) {
        if (d.alpha_width < 2 || d.alpha_height < 2)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "an alpha needs at least 2x2 samples; there is nothing to interpolate "
                        "below that");
        out->alpha = d.alpha;
        out->alpha_width = d.alpha_width;
        out->alpha_height = d.alpha_height;
        out->alpha_direction =
            kernel::cf3(d.alpha_direction[0], d.alpha_direction[1], d.alpha_direction[2]);
        out->alpha_tangent =
            kernel::cf3(d.alpha_tangent[0], d.alpha_tangent[1], d.alpha_tangent[2]);
        out->alpha_extent = d.alpha_extent;
    }
    // PAINT's target. Zero is BLACK rather than a default: black is a colour a
    // host may legitimately want, and there is no reading of the appended-field
    // rule that lets a caller mean it if we treat it as "unset". A caller that
    // declared only an older layout cannot reach this anyway — the colour verbs
    // did not exist then.
    out->color = kernel::cf3(d.color[0], d.color[1], d.color[2]);
    // The automask block, appended. Zero factors is no automask, which is what
    // a host declaring the older layout sends and exactly the behaviour it had.
    out->automask.factors = d.automask_factors;
    if (d.automask_normal_angle > 0.0f) out->automask.normal_angle = d.automask_normal_angle;
    if (d.automask_boundary_rings > 0) out->automask.boundary_rings = d.automask_boundary_rings;
    if (d.automask_cavity_strength > 0.0f)
        out->automask.cavity_strength = d.automask_cavity_strength;
    return CLAY_OK;
}

clay_result read_mesh_frame(const clay_mesh_frame* src, math::Transform* out) {
    *out = math::Transform::identity();
    if (!src) return CLAY_OK;
    clay_mesh_frame d;
    clay_result r = read_desc(src, kMeshFrameOriginal, &d);
    if (r != CLAY_OK) return r;
    out->position = kernel::cf3(d.position[0], d.position[1], d.position[2]);
    const float qlen = std::sqrt(d.rotation[0] * d.rotation[0] + d.rotation[1] * d.rotation[1] +
                                 d.rotation[2] * d.rotation[2] + d.rotation[3] * d.rotation[3]);
    out->rotation = qlen > 0.0f ? math::Quat{d.rotation[0] / qlen, d.rotation[1] / qlen,
                                             d.rotation[2] / qlen, d.rotation[3] / qlen}
                                : math::Quat::identity();
    out->scale = d.scale != 0.0f ? d.scale : 1.0f;
    if (!(out->scale > 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "frame scale must be > 0");
    return CLAY_OK;
}

}  // namespace

extern "C" {

clay_result clay_mesh_brush_defaults(clay_mesh_brush_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null descriptor");
    const mesh::MeshBrushSettings d;
    clay_mesh_brush_desc probe;
    clay_result r = read_desc(out_desc, kMeshBrushDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_desc->struct_size;
    clay_mesh_brush_desc out{};
    out.verb = CLAY_MESH_BRUSH_DRAW;
    write_f3(out.center, d.center);
    out.radius = d.radius;
    out.strength = d.strength;
    out.falloff = static_cast<std::int32_t>(d.falloff);
    write_f3(out.direction, d.direction);
    write_f3(out.deposit_normal, d.deposit_normal);
    out.geodesic = d.geodesic ? 1 : 0;
    out.seed_class = CLAY_MESH_NO_CLASS;
    out.flatten_mode = static_cast<std::int32_t>(d.flatten_mode);
    out.use_given_plane = 0;
    write_f3(out.plane_point, d.plane_point);
    write_f3(out.plane_normal, d.plane_normal);
    out.polish_angle = d.polish_angle;
    out.smooth_iterations = d.smooth_iterations;
    out.layer_height = d.layer_height;
    write_f3(out.color, d.color);
    out.automask_factors = d.automask.factors;
    out.automask_normal_angle = d.automask.normal_angle;
    out.automask_boundary_rings = d.automask.boundary_rings;
    out.automask_cavity_strength = d.automask.cavity_strength;
    // The alpha stays null in the defaults: a stamp without one is the common
    // case, and a default pointing at nothing a caller owns would be a trap.
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

}  // extern "C" — the helpers below return C++ types and cannot have C linkage

// A namespace does NOT reset language linkage: an anonymous namespace opened
// inside extern "C" leaves everything in it with C linkage, and a function
// returning a class type then fails -Wreturn-type-c-linkage on Clang (an
// error under -Werror) and C4190 on MSVC. GCC is silent, which is why this
// only broke the macOS and Windows jobs. Same shape as the close above
// clay_mesh_sculptor_create.
namespace {

constexpr std::size_t kBrushModelOriginal = offsetof(clay_brush_model, post) + sizeof(std::int32_t);
constexpr std::size_t kBrushPresetOriginal =
    offsetof(clay_brush_preset, brush) + sizeof(clay_mesh_brush_desc);

clay_brush_model to_c_model(const mesh::BrushModel& m) {
    clay_brush_model out{};
    out.verb = static_cast<std::int32_t>(m.verb);
    out.footprint = static_cast<std::int32_t>(m.footprint);
    out.falloff = static_cast<std::int32_t>(m.falloff);
    out.frame = static_cast<std::int32_t>(m.frame);
    out.kernel = static_cast<std::int32_t>(m.kernel);
    out.target = static_cast<std::int32_t>(m.target);
    out.post = static_cast<std::int32_t>(m.post);
    return out;
}

mesh::BrushModel from_c_model(const clay_brush_model& d) {
    mesh::BrushModel m;
    m.verb = static_cast<mesh::MeshBrush>(d.verb);
    m.footprint = static_cast<mesh::BrushFootprint>(d.footprint);
    m.falloff = static_cast<mesh::MeshFalloff>(d.falloff);
    m.frame = static_cast<mesh::BrushFrame>(d.frame);
    m.kernel = static_cast<mesh::BrushKernelId>(d.kernel);
    m.target = static_cast<mesh::BrushWriteTarget>(d.target);
    m.post = static_cast<mesh::BrushPostPolicy>(d.post);
    return m;
}

// The settings half of a preset, as a descriptor. PLACEMENT is deliberately not
// written — centre, direction, seed class and the alpha block belong to a stamp
// and to the caller, not to a brush.
clay_mesh_brush_desc to_c_brush(const mesh::MeshBrushSettings& s, mesh::MeshBrush verb) {
    clay_mesh_brush_desc out{};
    out.struct_size = sizeof(clay_mesh_brush_desc);
    out.verb = static_cast<std::int32_t>(verb);
    out.radius = s.radius;
    out.strength = s.strength;
    out.falloff = static_cast<std::int32_t>(s.falloff);
    write_f3(out.deposit_normal, s.deposit_normal);
    out.geodesic = s.geodesic ? 1 : 0;
    out.seed_class = CLAY_MESH_NO_CLASS;
    out.flatten_mode = static_cast<std::int32_t>(s.flatten_mode);
    out.use_given_plane = s.use_given_plane ? 1 : 0;
    write_f3(out.plane_point, s.plane_point);
    write_f3(out.plane_normal, s.plane_normal);
    out.polish_angle = s.polish_angle;
    out.smooth_iterations = s.smooth_iterations;
    out.layer_height = s.layer_height;
    write_f3(out.color, s.color);
    out.automask_factors = s.automask.factors;
    out.automask_normal_angle = s.automask.normal_angle;
    out.automask_boundary_rings = s.automask.boundary_rings;
    out.automask_cavity_strength = s.automask.cavity_strength;
    return out;
}

clay_brush_preset to_c_preset(const brush::BrushPreset& p) {
    clay_brush_preset out{};
    out.struct_size = sizeof(clay_brush_preset);
    // Truncated rather than refused: a name is a label, and losing its tail is
    // a worse outcome than nothing only if a host was using it as a key, which
    // is what the library's own short names exist to avoid.
    const std::size_t n = std::min<std::size_t>(p.name.size(), CLAY_BRUSH_PRESET_NAME_MAX - 1);
    std::memcpy(out.name, p.name.data(), n);
    out.name[n] = '\0';
    out.stroke = preset_fields(p.stroke);
    out.stroke.struct_size = sizeof(clay_stroke_preset);
    out.model = to_c_model(p.model);
    out.model.struct_size = sizeof(clay_brush_model);
    out.brush = to_c_brush(p.settings, p.model.verb);
    return out;
}

clay_result read_brush_preset(const clay_brush_preset* src, brush::BrushPreset* out) {
    if (!src) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brush preset");
    clay_brush_preset d;
    clay_result r = read_desc(src, kBrushPresetOriginal, &d);
    if (r != CLAY_OK) return r;
    d.name[CLAY_BRUSH_PRESET_NAME_MAX - 1] = '\0';
    out->name = d.name;
    r = read_preset(&d.stroke, &out->stroke);
    if (r != CLAY_OK) return r;
    if (!mesh_brush_is_known(d.model.verb))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown mesh brush in preset model: " + std::to_string(d.model.verb));
    out->model = from_c_model(d.model);
    mesh::MeshBrush verb = mesh::MeshBrush::Draw;
    // The settings descriptor validates itself through the same reader every
    // stamp uses, so a preset cannot smuggle a value a stamp would refuse.
    r = read_mesh_brush(&d.brush, &verb, &out->settings);
    if (r != CLAY_OK) return r;
    return CLAY_OK;
}

constexpr std::size_t kTransferDescOriginal =
    offsetof(clay_transfer_desc, max_distance) + sizeof(float);
constexpr std::size_t kTransferReportOriginal =
    offsetof(clay_transfer_report, max_distance) + sizeof(float);

// Original layouts (ABI 0.63.0), named by their last field so appending one
// does not silently move the baseline.
constexpr std::size_t kVoxelRemeshParamsOriginal =
    offsetof(clay_voxel_remesh_params, memory_budget_bytes) + sizeof(std::uint64_t);
constexpr std::size_t kVoxelRemeshEstimateOriginal =
    offsetof(clay_voxel_remesh_estimate, exceeds_memory_budget) + sizeof(std::int32_t);
constexpr std::size_t kWeldDescOriginal =
    offsetof(clay_weld_desc, attribute_epsilon) + sizeof(float);
constexpr std::size_t kWeldReportOriginal =
    offsetof(clay_weld_report, quads_dropped) + sizeof(std::int32_t);
constexpr std::size_t kVoxelRemeshReportOriginal =
    offsetof(clay_voxel_remesh_report, cancelled) + sizeof(std::int32_t);

// The status the engine returns, as the result code this ABI already has.
//
// Distinct rather than collapsed, because "lower the resolution", "your model
// has holes" and "you stopped it" are three different things for a host to
// say, and one generic failure would make a host guess between them.
clay_result voxel_remesh_result_code(mesh::VoxelRemeshStatus status) {
    switch (status) {
        case mesh::VoxelRemeshStatus::Ok:
            return CLAY_OK;
        case mesh::VoxelRemeshStatus::EmptySource:
        case mesh::VoxelRemeshStatus::InvalidResolution:
        case mesh::VoxelRemeshStatus::InvalidParameters:
            return CLAY_ERROR_INVALID_ARGUMENT;
        case mesh::VoxelRemeshStatus::ExceedsBudget:
            return CLAY_ERROR_BUDGET_EXCEEDED;
        case mesh::VoxelRemeshStatus::Unsupported:
        case mesh::VoxelRemeshStatus::OpenSurfaceRejected:
            return CLAY_ERROR_UNSUPPORTED;
        case mesh::VoxelRemeshStatus::ExtractionFailed:
        case mesh::VoxelRemeshStatus::ResultNotWatertight:
            return CLAY_ERROR_BACKEND;
        case mesh::VoxelRemeshStatus::Cancelled:
            return CLAY_ERROR_CANCELLED;
    }
    return CLAY_ERROR_BACKEND;
}

const char* voxel_remesh_message(mesh::VoxelRemeshStatus status) {
    switch (status) {
        case mesh::VoxelRemeshStatus::Ok:
            return "";
        case mesh::VoxelRemeshStatus::EmptySource:
            return "a mesh with no triangles has no surface to rebuild";
        case mesh::VoxelRemeshStatus::InvalidResolution:
            return "the resolution must be a finite positive voxel size, or a non-zero "
                   "longest-axis resolution";
        case mesh::VoxelRemeshStatus::InvalidParameters:
            return "a projection strength, projection distance or component volume is out of "
                   "range";
        case mesh::VoxelRemeshStatus::Unsupported:
            return "build_multires_levels is reserved and must be zero";
        case mesh::VoxelRemeshStatus::ExceedsBudget:
            return "the request exceeds the memory budget; choose a coarser resolution";
        case mesh::VoxelRemeshStatus::OpenSurfaceRejected:
            return "the source has open boundaries and the policy rejects them";
        case mesh::VoxelRemeshStatus::ExtractionFailed:
            return "no surface was found at this resolution";
        case mesh::VoxelRemeshStatus::ResultNotWatertight:
            return "the result failed the watertight validation this surface mode promises";
        case mesh::VoxelRemeshStatus::Cancelled:
            return "cancelled";
    }
    return "voxel remesh failed";
}

clay_result read_voxel_remesh_params(const clay_voxel_remesh_params* desc,
                                     mesh::VoxelRemeshParams* out) {
    *out = mesh::VoxelRemeshParams{};
    if (!desc) return CLAY_OK;  // the documented defaults
    clay_voxel_remesh_params d;
    const clay_result r = read_desc(desc, kVoxelRemeshParamsOriginal, &d);
    if (r != CLAY_OK) return r;

    switch (d.resolution_mode) {
        case CLAY_VOXEL_REMESH_VOXEL_SIZE:
            out->resolution_mode = mesh::VoxelRemeshResolutionMode::VoxelSize;
            break;
        case CLAY_VOXEL_REMESH_LONGEST_AXIS:
            out->resolution_mode = mesh::VoxelRemeshResolutionMode::LongestAxisResolution;
            break;
        default:
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown voxel remesh resolution mode");
    }
    switch (d.surface_mode) {
        case CLAY_VOXEL_REMESH_SMOOTH:
            out->surface_mode = mesh::VoxelRemeshSurfaceMode::Smooth;
            break;
        case CLAY_VOXEL_REMESH_SHARP:
            out->surface_mode = mesh::VoxelRemeshSurfaceMode::Sharp;
            break;
        default:
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown voxel remesh surface mode");
    }
    switch (d.open_surface_policy) {
        case CLAY_VOXEL_REMESH_OPEN_REJECT:
            out->open_surface_policy = mesh::VoxelRemeshOpenSurfacePolicy::Reject;
            break;
        case CLAY_VOXEL_REMESH_OPEN_CLOSE:
            out->open_surface_policy = mesh::VoxelRemeshOpenSurfacePolicy::Close;
            break;
        case CLAY_VOXEL_REMESH_OPEN_BEST_EFFORT:
            out->open_surface_policy = mesh::VoxelRemeshOpenSurfacePolicy::BestEffort;
            break;
        default:
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown voxel remesh open-surface policy");
    }
    switch (d.small_component_policy) {
        case CLAY_VOXEL_REMESH_KEEP_COMPONENTS:
            out->small_component_policy = mesh::VoxelRemeshSmallComponentPolicy::Preserve;
            break;
        case CLAY_VOXEL_REMESH_REMOVE_BELOW_VOLUME:
            out->small_component_policy =
                mesh::VoxelRemeshSmallComponentPolicy::RemoveBelowVolume;
            break;
        default:
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown voxel remesh component policy");
    }

    out->voxel_size = d.voxel_size;
    out->longest_axis_resolution = d.longest_axis_resolution;
    out->minimum_component_volume = d.minimum_component_volume;
    out->preserve_volume = d.preserve_volume != 0;
    out->project_to_source = d.project_to_source != 0;
    out->projection_strength = d.projection_strength;
    out->max_projection_distance_voxels = d.max_projection_distance_voxels;
    out->preserve_colors = d.preserve_colors != 0;
    out->build_multires_levels = d.build_multires_levels;
    out->memory_budget_bytes = d.memory_budget_bytes;
    return CLAY_OK;
}

clay_result write_voxel_remesh_estimate(clay_voxel_remesh_estimate* out,
                                        const mesh::VoxelRemeshEstimate& e) {
    if (!out) return CLAY_OK;
    clay_voxel_remesh_estimate probe;
    const clay_result r = read_desc(out, kVoxelRemeshEstimateOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out->struct_size;
    clay_voxel_remesh_estimate filled{};
    filled.resolved_voxel_size = e.resolved_voxel_size;
    for (int a = 0; a < 3; ++a) filled.grid_dimensions[a] = e.grid_dimensions[a];
    filled.estimated_active_samples = e.estimated_active_samples;
    filled.estimated_memory_bytes = e.estimated_memory_bytes;
    filled.estimated_triangle_min = e.estimated_triangle_min;
    filled.estimated_triangle_max = e.estimated_triangle_max;
    filled.boundary_edge_count = e.boundary_edge_count;
    filled.component_count = e.component_count;
    filled.has_open_boundaries = e.has_open_boundaries ? 1 : 0;
    filled.thin_feature_warning = e.thin_feature_warning ? 1 : 0;
    filled.exceeds_memory_budget = e.exceeds_memory_budget ? 1 : 0;
    write_desc(out, declared, filled);
    return CLAY_OK;
}

clay_result write_voxel_remesh_report(clay_voxel_remesh_report* out,
                                      const mesh::VoxelRemeshReport& rep) {
    if (!out) return CLAY_OK;
    clay_voxel_remesh_report probe;
    const clay_result r = read_desc(out, kVoxelRemeshReportOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out->struct_size;
    clay_voxel_remesh_report filled{};
    filled.voxel_size = rep.voxel_size;
    filled.source_vertices = rep.source_vertices;
    filled.source_triangles = rep.source_triangles;
    filled.result_vertices = rep.result_vertices;
    filled.result_triangles = rep.result_triangles;
    filled.source_volume = rep.source_volume;
    filled.result_volume = rep.result_volume;
    filled.relative_volume_error = rep.relative_volume_error;
    filled.source_boundary_edges = rep.source_boundary_edges;
    filled.result_boundary_edges = rep.result_boundary_edges;
    filled.source_components = rep.source_components;
    filled.result_components = rep.result_components;
    filled.removed_components = rep.removed_components;
    filled.active_samples = rep.active_samples;
    filled.source_was_open = rep.source_was_open ? 1 : 0;
    filled.result_watertight = rep.result_watertight ? 1 : 0;
    filled.result_manifold = rep.result_manifold ? 1 : 0;
    filled.result_oriented = rep.result_oriented ? 1 : 0;
    filled.projected_to_source = rep.projected_to_source ? 1 : 0;
    filled.projected_vertices = rep.projected_vertices;
    filled.volume_corrected = rep.volume_corrected ? 1 : 0;
    filled.colors_transferred = rep.colors_transferred ? 1 : 0;
    filled.uvs_dropped = rep.uvs_dropped ? 1 : 0;
    filled.cancelled = rep.cancelled ? 1 : 0;
    write_desc(out, declared, filled);
    return CLAY_OK;
}
}  // namespace

extern "C" {

clay_result clay_brush_model_of(int32_t verb, clay_brush_model* out_model) {
    if (!out_model) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null model");
    if (!mesh_brush_is_known(verb))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown mesh brush: " + std::to_string(verb));
    clay_brush_model probe;
    clay_result r = read_desc(out_model, kBrushModelOriginal, &probe);
    if (r != CLAY_OK) return r;
    write_desc(out_model, out_model->struct_size,
               to_c_model(mesh::model_of(static_cast<mesh::MeshBrush>(verb))));
    return CLAY_OK;
}

clay_result clay_brush_preset_defaults(clay_brush_preset* out_preset) {
    if (!out_preset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset");
    clay_brush_preset probe;
    clay_result r = read_desc(out_preset, kBrushPresetOriginal, &probe);
    if (r != CLAY_OK) return r;
    brush::BrushPreset d;
    d.name = "Standard";
    d.model = mesh::model_of(mesh::MeshBrush::Draw);
    write_desc(out_preset, out_preset->struct_size, to_c_preset(d));
    return CLAY_OK;
}

clay_result clay_brush_preset_serialize(const clay_brush_preset* preset, uint8_t* out_data,
                                        size_t* count) {
    brush::BrushPreset p;
    clay_result r = read_brush_preset(preset, &p);
    if (r != CLAY_OK) return r;
    const std::vector<std::uint8_t> bytes = p.serialize();
    return write_sized(bytes.data(), bytes.size(), out_data, count, "brush preset");
}

clay_result clay_brush_preset_deserialize(const uint8_t* data, size_t size,
                                          clay_brush_preset* out_preset) {
    if (!data || size == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or empty brush preset data");
    if (!out_preset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset");
    clay_brush_preset probe;
    clay_result r = read_desc(out_preset, kBrushPresetOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::optional<brush::BrushPreset> p = brush::BrushPreset::deserialize(data, size);
    if (!p)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "not a brush preset this build can read: malformed, or written by a schema "
                    "version newer than " +
                        std::to_string(brush::kBrushPresetVersion));
    write_desc(out_preset, out_preset->struct_size, to_c_preset(*p));
    return CLAY_OK;
}

uint32_t clay_brush_preset_version(void) {
    return static_cast<std::uint32_t>(brush::kBrushPresetVersion);
}

size_t clay_brush_preset_library_count(void) { return brush::reference_presets().size(); }

clay_result clay_brush_preset_library_at(size_t index, clay_brush_preset* out_preset) {
    if (!out_preset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset");
    clay_brush_preset probe;
    clay_result r = read_desc(out_preset, kBrushPresetOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::vector<brush::BrushPreset> lib = brush::reference_presets();
    if (index >= lib.size())
        return fail(CLAY_ERROR_NOT_FOUND,
                    "preset index " + std::to_string(index) + " of " + std::to_string(lib.size()));
    write_desc(out_preset, out_preset->struct_size, to_c_preset(lib[index]));
    return CLAY_OK;
}

clay_result clay_brush_preset_by_name(const char* name, clay_brush_preset* out_preset) {
    if (!name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset name");
    if (!out_preset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset");
    clay_brush_preset probe;
    clay_result r = read_desc(out_preset, kBrushPresetOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::optional<brush::BrushPreset> p = brush::reference_preset(name);
    if (!p) return fail(CLAY_ERROR_NOT_FOUND, std::string("no preset named ") + name);
    write_desc(out_preset, out_preset->struct_size, to_c_preset(*p));
    return CLAY_OK;
}

clay_result clay_mesh_transfer_defaults(clay_transfer_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null descriptor");
    clay_transfer_desc probe;
    clay_result r = read_desc(out_desc, kTransferDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_desc->struct_size;
    const mesh::TransferOptions d;
    clay_transfer_desc out{};
    out.colors = d.colors ? 1 : 0;
    out.uvs = d.uvs ? 1 : 0;
    out.normals = d.normals ? 1 : 0;
    out.max_distance = d.max_distance;
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

clay_result clay_mesh_transfer_attributes(const clay_mesh* source, clay_mesh* target,
                                          const clay_transfer_desc* desc,
                                          clay_transfer_report* out_report) {
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(source, &src);
    if (r != CLAY_OK) return r;
    if (!target) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null target mesh");
    mesh::Mesh* dst = mesh_data_mut(target);
    if (!dst) return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer is no longer in its document");

    mesh::TransferOptions options;
    if (desc) {
        clay_transfer_desc d;
        r = read_desc(desc, kTransferDescOriginal, &d);
        if (r != CLAY_OK) return r;
        if (d.max_distance < 0.0f)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "max_distance must be >= 0; zero derives it from the source's size");
        options.colors = d.colors != 0;
        options.uvs = d.uvs != 0;
        options.normals = d.normals != 0;
        options.max_distance = d.max_distance;
    }

    const mesh::TransferReport report = mesh::transfer_attributes(*src, dst, options);

    if (out_report) {
        clay_transfer_report probe;
        r = read_desc(out_report, kTransferReportOriginal, &probe);
        if (r != CLAY_OK) return r;
        const std::uint32_t declared = out_report->struct_size;
        clay_transfer_report filled{};
        filled.transferred = report.transferred;
        filled.fell_back = report.fell_back;
        filled.colors = report.colors ? 1 : 0;
        filled.uvs = report.uvs ? 1 : 0;
        filled.normals = report.normals ? 1 : 0;
        filled.max_distance = report.max_distance;
        write_desc(out_report, declared, filled);
    }
    return CLAY_OK;
}

clay_result clay_mesh_weld_defaults(clay_weld_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_desc");
    clay_weld_desc probe;
    clay_result r = read_desc(out_desc, kWeldDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_desc->struct_size;
    const mesh::WeldOptions d;
    clay_weld_desc out{};
    out.epsilon = d.epsilon;
    out.preserve_attribute_splits = d.preserve_attribute_splits ? 1 : 0;
    out.attribute_epsilon = d.attribute_epsilon;
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

clay_result clay_mesh_weld(clay_mesh* mesh, const clay_weld_desc* desc,
                           clay_weld_report* out_report) {
    if (!mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh");
    mesh::Mesh* data = mesh_data_mut(mesh);
    if (!data) return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer is no longer in its document");
    // A mesh LAYER's geometry is being rewritten, so the same protection every
    // other edit respects applies — and a ghosted or locked layer refuses this
    // exactly as it refuses a stamp.
    clay_result r = mesh_layer_editable(mesh);
    if (r != CLAY_OK) return r;

    mesh::WeldOptions options;
    if (desc) {
        clay_weld_desc d;
        r = read_desc(desc, kWeldDescOriginal, &d);
        if (r != CLAY_OK) return r;
        if (!(d.epsilon >= 0.0f) || !std::isfinite(d.epsilon))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "epsilon must be >= 0; zero welds only bit-identical positions");
        if (!(d.attribute_epsilon >= 0.0f) || !std::isfinite(d.attribute_epsilon))
            return fail(CLAY_ERROR_INVALID_ARGUMENT, "attribute_epsilon must be >= 0");
        options.epsilon = d.epsilon;
        options.preserve_attribute_splits = d.preserve_attribute_splits != 0;
        options.attribute_epsilon = d.attribute_epsilon;
    }

    const mesh::WeldReport report = mesh::weld(data, options);
    // A weld REPLACES the triangles, so anything cached over this layer is as
    // stale as it would be after a rebuild — which is exactly what the geometry
    // revision is for. Bumped only when something actually moved: a weld that
    // changed nothing must not invalidate a live sculptor.
    if (mesh->doc && report.vertices_merged + report.triangles_collapsed +
                             report.triangles_invalid >
                         0) {
        mesh->doc->mesh_geometry_revision[mesh->layer] =
            mesh_layer_revision_of(mesh->doc, mesh->layer) + 1;
        mesh->doc->touch();
    }

    if (out_report) {
        clay_weld_report probe;
        r = read_desc(out_report, kWeldReportOriginal, &probe);
        if (r != CLAY_OK) return r;
        const std::uint32_t declared = out_report->struct_size;
        clay_weld_report filled{};
        filled.vertices_before = report.vertices_before;
        filled.vertices_after = report.vertices_after;
        filled.triangles_before = report.triangles_before;
        filled.triangles_after = report.triangles_after;
        filled.vertices_merged = report.vertices_merged;
        filled.triangles_collapsed = report.triangles_collapsed;
        filled.triangles_invalid = report.triangles_invalid;
        filled.vertices_unreferenced = report.vertices_unreferenced;
        filled.epsilon = report.epsilon;
        filled.quads_dropped = report.quads_dropped ? 1 : 0;
        write_desc(out_report, declared, filled);
    }
    return CLAY_OK;
}

clay_result clay_mesh_transfer_vertex_scalar(const clay_mesh* source, const float* values,
                                             size_t value_count, const clay_mesh* target,
                                             float max_distance, float fallback,
                                             float* out_values, size_t out_count) {
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(source, &src);
    if (r != CLAY_OK) return r;
    const mesh::Mesh* dst = nullptr;
    r = resolve_mesh(target, &dst);
    if (r != CLAY_OK) return r;
    if (!values || !out_values) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null array");
    // Required rather than inferred, the same rule clay_mesh_copy_indices
    // follows: a length the caller states is a length the caller can be held
    // to, and one this side guessed is a read past somebody's buffer.
    if (value_count != src->positions.size())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "value_count must be the source's vertex count");
    if (out_count != dst->positions.size())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "out_count must be the target's vertex count");
    if (max_distance < 0.0f)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "max_distance must be >= 0; zero derives it from the source's size");

    const std::vector<float> in(values, values + value_count);
    const std::vector<float> out =
        mesh::transfer_vertex_scalar(*src, in, *dst, max_distance, fallback);
    if (out.size() != out_count) return fail(CLAY_ERROR_BACKEND, "transfer produced the wrong count");
    std::memcpy(out_values, out.data(), out_count * sizeof(float));
    return CLAY_OK;
}

clay_result clay_mesh_voxel_remesh_defaults(clay_voxel_remesh_params* out_params) {
    if (!out_params) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_params");
    clay_voxel_remesh_params probe;
    clay_result r = read_desc(out_params, kVoxelRemeshParamsOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_params->struct_size;

    const mesh::VoxelRemeshParams d;
    clay_voxel_remesh_params out{};
    out.resolution_mode = d.resolution_mode == mesh::VoxelRemeshResolutionMode::VoxelSize
                              ? CLAY_VOXEL_REMESH_VOXEL_SIZE
                              : CLAY_VOXEL_REMESH_LONGEST_AXIS;
    out.voxel_size = d.voxel_size;
    out.longest_axis_resolution = d.longest_axis_resolution;
    out.surface_mode = d.surface_mode == mesh::VoxelRemeshSurfaceMode::Sharp
                           ? CLAY_VOXEL_REMESH_SHARP
                           : CLAY_VOXEL_REMESH_SMOOTH;
    out.open_surface_policy =
        d.open_surface_policy == mesh::VoxelRemeshOpenSurfacePolicy::Reject
            ? CLAY_VOXEL_REMESH_OPEN_REJECT
            : (d.open_surface_policy == mesh::VoxelRemeshOpenSurfacePolicy::BestEffort
                   ? CLAY_VOXEL_REMESH_OPEN_BEST_EFFORT
                   : CLAY_VOXEL_REMESH_OPEN_CLOSE);
    out.small_component_policy =
        d.small_component_policy == mesh::VoxelRemeshSmallComponentPolicy::RemoveBelowVolume
            ? CLAY_VOXEL_REMESH_REMOVE_BELOW_VOLUME
            : CLAY_VOXEL_REMESH_KEEP_COMPONENTS;
    out.minimum_component_volume = d.minimum_component_volume;
    out.preserve_volume = d.preserve_volume ? 1 : 0;
    out.project_to_source = d.project_to_source ? 1 : 0;
    out.projection_strength = d.projection_strength;
    out.max_projection_distance_voxels = d.max_projection_distance_voxels;
    out.preserve_colors = d.preserve_colors ? 1 : 0;
    out.build_multires_levels = d.build_multires_levels;
    out.memory_budget_bytes = d.memory_budget_bytes;
    write_desc(out_params, declared, out);
    return CLAY_OK;
}

clay_result clay_mesh_voxel_remesh_estimate(const clay_mesh* source,
                                            const clay_voxel_remesh_params* params,
                                            clay_voxel_remesh_estimate* out_estimate) {
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(source, &src);
    if (r != CLAY_OK) return r;
    mesh::VoxelRemeshParams p;
    r = read_voxel_remesh_params(params, &p);
    if (r != CLAY_OK) return r;

    const mesh::VoxelRemeshEstimate e = mesh::voxel_remesh_estimate(*src, p);
    // Filled even for a refusal: the estimate is what JUSTIFIES the refusal,
    // and a host that got nothing back could not say why.
    r = write_voxel_remesh_estimate(out_estimate, e);
    if (r != CLAY_OK) return r;
    if (e.status != mesh::VoxelRemeshStatus::Ok)
        return fail(voxel_remesh_result_code(e.status), voxel_remesh_message(e.status));
    return CLAY_OK;
}

clay_result clay_mesh_voxel_remesh(const clay_mesh* source,
                                   const clay_voxel_remesh_params* params,
                                   clay_cancel_token* token, clay_mesh** out_mesh,
                                   clay_voxel_remesh_report* out_report) {
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_mesh");
    *out_mesh = nullptr;
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(source, &src);
    if (r != CLAY_OK) return r;
    mesh::VoxelRemeshParams p;
    r = read_voxel_remesh_params(params, &p);
    if (r != CLAY_OK) return r;

    mesh::VoxelRemeshResult result =
        mesh::voxel_remesh(*src, p, token ? &token->token : nullptr);
    // Written before the status is checked, for the same reason the estimate
    // is: an open-surface refusal carries the source's boundary-edge count,
    // which is exactly what a host puts in front of a user.
    r = write_voxel_remesh_report(out_report, result.report);
    if (r != CLAY_OK) return r;
    if (result.status != mesh::VoxelRemeshStatus::Ok)
        return fail(voxel_remesh_result_code(result.status),
                    voxel_remesh_message(result.status));

    auto built = std::make_unique<clay_mesh>();
    built->data = std::move(result.mesh);
    *out_mesh = built.release();
    return CLAY_OK;
}

}  // extern "C" — the helpers below return C++ types and cannot have C linkage

namespace {

// The one place a mesh layer's triangles are swapped, so every caller gets the
// same guards, the same undo record and the same invalidation.
clay_result replace_mesh_layer_geometry(clay_document* doc, clay_layer_id layer,
                                        mesh::Mesh replacement,
                                        std::uint64_t expected_revision) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || l->kind != scene::LayerKind::Mesh)
        return fail(CLAY_ERROR_NOT_FOUND, "no mesh layer with that id");
    if (l->protected_from_edits())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("layer ") + std::to_string(layer) + " is " +
                        (l->ghost ? "ghosted" : "locked") + " and takes no edits");
    auto it = doc->doc.mesh_layers.find(layer);
    if (it == doc->doc.mesh_layers.end())
        return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer holds no triangles");
    if (replacement.triangle_count() == 0)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "the replacement has no triangles");
    // A quad list describing triangles that no longer exist is a lie that
    // survives into a saved document — mesh_data.h states the rule and the save
    // path drops such a list silently, so this refuses rather than repairs.
    if (replacement.has_quads() && !mesh::quads_consistent(replacement))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the replacement's quads do not describe its triangles");
    // The staleness check, and it happens LAST among the guards so a caller
    // whose layer moved is told that rather than being told about the argument
    // it would also have been refused for.
    const std::uint64_t now = mesh_layer_revision_of(doc, layer);
    if (expected_revision != 0 && expected_revision != now)
        return fail(CLAY_ERROR_FORWARD_VERSION,
                    "the mesh layer was rebuilt while this result was being prepared");

    // ONE UNDO STEP, holding both meshes. A `VertexDeltas` cannot express this
    // and must not be asked to: deltas already on the stack for this layer were
    // recorded against the OLD vertex count, and History::apply_step would
    // apply them to whatever mesh the resolver now returns. Recording the
    // replacement as its own kind is what puts a boundary between them.
    // Guarded, because undo is OPT-IN: `doc->undo` exists only after
    // clay_document_enable_undo, and every other recording site in this file
    // tests it the same way. Unguarded, the first remesh on a document that
    // never enabled undo was a null dereference.
    if (doc->undo) doc->undo->record_mesh_replace(layer, it->second, replacement);
    it->second = std::move(replacement);
    doc->mesh_geometry_revision[layer] = now + 1;
    doc->touch();
    return CLAY_OK;
}

}  // namespace

extern "C" {

clay_result clay_document_mesh_layer_revision(const clay_document* doc, clay_layer_id layer,
                                              uint64_t* out_revision) {
    if (!out_revision) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_revision");
    *out_revision = 0;
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || l->kind != scene::LayerKind::Mesh)
        return fail(CLAY_ERROR_NOT_FOUND, "no mesh layer with that id");
    *out_revision = mesh_layer_revision_of(doc, layer);
    return CLAY_OK;
}

clay_result clay_document_replace_mesh_layer(clay_document* doc, clay_layer_id layer,
                                             const clay_mesh* replacement,
                                             uint64_t expected_revision) {
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(replacement, &src);
    if (r != CLAY_OK) return r;
    // A layer handed its own borrowed mesh would be assigned from itself
    // through a copy it is about to destroy. Refused rather than special-cased:
    // a caller doing that meant something else.
    if (doc) {
        auto it = doc->doc.mesh_layers.find(layer);
        if (it != doc->doc.mesh_layers.end() && &it->second == src)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "the replacement is the layer's own mesh");
    }
    return replace_mesh_layer_geometry(doc, layer, *src, expected_revision);
}

clay_result clay_document_voxel_remesh_layer(clay_document* doc, clay_layer_id layer,
                                             const clay_voxel_remesh_params* params,
                                             clay_cancel_token* token,
                                             clay_voxel_remesh_report* out_report) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* l = doc->doc.document.find_layer(layer);
    if (!l || l->kind != scene::LayerKind::Mesh)
        return fail(CLAY_ERROR_NOT_FOUND, "no mesh layer with that id");
    // Refused BEFORE the rebuild, not after: remeshing a locked layer for
    // several seconds and then declining to commit it is a worse answer than
    // declining immediately.
    if (l->protected_from_edits())
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    std::string("layer ") + std::to_string(layer) + " is " +
                        (l->ghost ? "ghosted" : "locked") + " and takes no edits");
    auto it = doc->doc.mesh_layers.find(layer);
    if (it == doc->doc.mesh_layers.end())
        return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer holds no triangles");

    mesh::VoxelRemeshParams p;
    clay_result r = read_voxel_remesh_params(params, &p);
    if (r != CLAY_OK) return r;

    // A COPY of the source, and the revision it was taken at. The copy is what
    // makes the whole operation transactional: `voxel_remesh` reads it while
    // the layer still holds the original, so a cancel or a refusal is a discard
    // and the document was never touched.
    const mesh::Mesh source = it->second;
    const std::uint64_t at = mesh_layer_revision_of(doc, layer);

    mesh::VoxelRemeshResult result =
        mesh::voxel_remesh(source, p, token ? &token->token : nullptr);
    r = write_voxel_remesh_report(out_report, result.report);
    if (r != CLAY_OK) return r;
    if (result.status != mesh::VoxelRemeshStatus::Ok)
        return fail(voxel_remesh_result_code(result.status),
                    voxel_remesh_message(result.status));

    return replace_mesh_layer_geometry(doc, layer, std::move(result.mesh), at);
}

clay_result clay_mesh_sculptor_create(clay_mesh* mesh, float weld_epsilon,
                                      clay_mesh_sculptor** out_sculptor) {
    if (!out_sculptor) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_sculptor");
    *out_sculptor = nullptr;
    if (!mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh");
    mesh::Mesh* data = mesh_data_mut(mesh);
    if (!data) return fail(CLAY_ERROR_NOT_FOUND, "the mesh layer is no longer in its document");
    if (data->positions.empty() || data->indices.empty())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a mesh with no triangles has no surface");
    auto handle = std::make_unique<clay_mesh_sculptor>();
    handle->mesh = mesh;
    handle->bound = data;
    handle->geometry_revision = mesh->doc ? mesh_layer_revision_of(mesh->doc, mesh->layer) : 0;
    handle->sculptor = std::make_unique<mesh::MeshSculptor>(
        *data, weld_epsilon < 0.0f ? mesh::kDefaultWeldEpsilon : weld_epsilon);
    *out_sculptor = handle.release();
    return CLAY_OK;
}

void clay_mesh_sculptor_destroy(clay_mesh_sculptor* sculptor) { delete sculptor; }

clay_result clay_mesh_sculptor_vertex_count(const clay_mesh_sculptor* sculptor, size_t* out_count) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh sculptor");
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    *out_count = sculptor->sculptor->adjacency().vertex_count();
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_class_count(const clay_mesh_sculptor* sculptor, size_t* out_count) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh sculptor");
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    *out_count = sculptor->sculptor->adjacency().class_count();
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_stamp(clay_mesh_sculptor* sculptor, const clay_mesh_brush_desc* desc,
                                     const clay_mask* mask, clay_mesh_deltas* deltas,
                                     size_t* out_moved) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    mesh::MeshBrush verb = mesh::MeshBrush::Draw;
    mesh::MeshBrushSettings settings;
    r = read_mesh_brush(desc, &verb, &settings);
    if (r != CLAY_OK) return r;

    field::MaskGate gate;
    if (mask) {
        voxel::MaskField* field_mask = nullptr;
        r = resolve_mask(mask, &field_mask);
        if (r != CLAY_OK) return r;
        gate = [field_mask](kernel::cfloat3 p) { return field_mask->sample(p); };
    }
    const std::size_t moved =
        sculptor->sculptor->stamp(verb, settings, gate, deltas ? &deltas->deltas : nullptr);
    if (out_moved) *out_moved = moved;
    return CLAY_OK;
}

clay_mesh_lattice* clay_mesh_lattice_create(const float min[3], const float max[3], int32_t nx,
                                            int32_t ny, int32_t nz) {
    if (!min || !max) return nullptr;
    const math::Aabb box{kernel::cf3(min[0], min[1], min[2]),
                         kernel::cf3(max[0], max[1], max[2])};
    return new clay_mesh_lattice{mesh::Lattice(box, nx, ny, nz)};
}

void clay_mesh_lattice_destroy(clay_mesh_lattice* lattice) { delete lattice; }

clay_result clay_mesh_lattice_divisions(const clay_mesh_lattice* lattice, int32_t* out_nx,
                                        int32_t* out_ny, int32_t* out_nz) {
    if (!lattice) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice");
    if (out_nx) *out_nx = lattice->cage.nx();
    if (out_ny) *out_ny = lattice->cage.ny();
    if (out_nz) *out_nz = lattice->cage.nz();
    return CLAY_OK;
}

clay_result clay_mesh_lattice_set_offset(clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                         int32_t k, const float offset[3]) {
    if (!lattice || !offset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice or offset");
    lattice->cage.set_offset(i, j, k, kernel::cf3(offset[0], offset[1], offset[2]));
    return CLAY_OK;
}

clay_result clay_mesh_lattice_offset(const clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                     int32_t k, float out_offset[3]) {
    if (!lattice || !out_offset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice or out");
    const kernel::cfloat3 o = lattice->cage.offset(i, j, k);
    out_offset[0] = o.x;
    out_offset[1] = o.y;
    out_offset[2] = o.z;
    return CLAY_OK;
}

clay_result clay_mesh_lattice_rest(const clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                   int32_t k, float out_rest[3]) {
    if (!lattice || !out_rest) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice or out");
    const kernel::cfloat3 o = lattice->cage.rest(i, j, k);
    out_rest[0] = o.x;
    out_rest[1] = o.y;
    out_rest[2] = o.z;
    return CLAY_OK;
}

clay_result clay_mesh_lattice_position(const clay_mesh_lattice* lattice, int32_t i, int32_t j,
                                       int32_t k, float out_position[3]) {
    if (!lattice || !out_position) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice or out");
    const kernel::cfloat3 o = lattice->cage.position(i, j, k);
    out_position[0] = o.x;
    out_position[1] = o.y;
    out_position[2] = o.z;
    return CLAY_OK;
}

clay_result clay_mesh_lattice_is_identity(const clay_mesh_lattice* lattice, int32_t* out_identity) {
    if (!lattice || !out_identity) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice or out");
    *out_identity = lattice->cage.is_identity() ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mesh_lattice_displacement(const clay_mesh_lattice* lattice, const float p[3],
                                           float out_displacement[3]) {
    if (!lattice || !p || !out_displacement)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice, point or out");
    const kernel::cfloat3 d = lattice->cage.displacement(kernel::cf3(p[0], p[1], p[2]));
    out_displacement[0] = d.x;
    out_displacement[1] = d.y;
    out_displacement[2] = d.z;
    return CLAY_OK;
}

namespace {
constexpr std::size_t kMeshDeformDescOriginal =
    offsetof(clay_mesh_deform_desc, ease) + sizeof(std::int32_t);

clay_result read_mesh_deform(const clay_mesh_deform_desc* src, mesh::MeshDeformSettings* out) {
    if (!src) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null deform descriptor");
    clay_mesh_deform_desc d;
    clay_result r = read_desc(src, kMeshDeformDescOriginal, &d);
    if (r != CLAY_OK) return r;
    if (d.verb != CLAY_MESH_DEFORM_TAPER && d.verb != CLAY_MESH_DEFORM_TWIST)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "unknown mesh deformer: " + std::to_string(d.verb));
    if (d.ease < 0 || d.ease >= CLAY_EASE_COUNT)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "ease index out of range");
    if (!(d.span > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "a deformer's span must be positive: there is nothing to ramp across");
    const kernel::cfloat3 axis = kernel::cf3(d.axis[0], d.axis[1], d.axis[2]);
    if (!(kernel::clength(axis) > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "a deformer's axis has no length");
    out->verb = static_cast<mesh::MeshDeform>(d.verb);
    out->origin = kernel::cf3(d.origin[0], d.origin[1], d.origin[2]);
    out->axis = axis;
    out->span = d.span;
    out->scale_start = d.scale_start;
    out->scale_end = d.scale_end;
    out->angle = d.angle;
    out->ease = d.ease;
    return CLAY_OK;
}
}  // namespace

clay_result clay_mesh_deform_defaults(clay_mesh_deform_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null descriptor");
    clay_mesh_deform_desc probe;
    clay_result r = read_desc(out_desc, kMeshDeformDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_desc->struct_size;
    const mesh::MeshDeformSettings d;
    clay_mesh_deform_desc out{};
    out.verb = CLAY_MESH_DEFORM_TAPER;
    write_f3(out.origin, d.origin);
    write_f3(out.axis, d.axis);
    out.span = d.span;
    out.scale_start = d.scale_start;
    out.scale_end = d.scale_end;
    out.angle = d.angle;
    out.ease = d.ease;
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

clay_result clay_mesh_deform_point(const clay_mesh_deform_desc* desc, const float p[3],
                                   float out_p[3]) {
    if (!p || !out_p) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null point");
    mesh::MeshDeformSettings settings;
    clay_result r = read_mesh_deform(desc, &settings);
    if (r != CLAY_OK) return r;
    write_f3(out_p, mesh::deform_point(settings, kernel::cf3(p[0], p[1], p[2])));
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_deform(clay_mesh_sculptor* sculptor,
                                      const clay_mesh_deform_desc* desc, const clay_mask* mask,
                                      clay_mesh_deltas* deltas, size_t* out_moved) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    mesh::MeshDeformSettings settings;
    r = read_mesh_deform(desc, &settings);
    if (r != CLAY_OK) return r;
    voxel::MaskField* m = nullptr;
    if (mask) {
        r = resolve_mask(mask, &m);
        if (r != CLAY_OK) return r;
    }
    field::MaskGate gate;
    if (m) gate = [m](kernel::cfloat3 p) { return m->sample(p); };
    const std::size_t moved = sculptor->sculptor->apply_deformer(
        settings, gate, deltas ? &deltas->deltas : nullptr);
    if (out_moved) *out_moved = moved;
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_lattice(clay_mesh_sculptor* sculptor,
                                       const clay_mesh_lattice* lattice, clay_mesh_deltas* deltas,
                                       size_t* out_moved) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    if (!lattice) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null lattice");
    const std::size_t moved =
        sculptor->sculptor->apply_lattice(lattice->cage, deltas ? &deltas->deltas : nullptr);
    if (out_moved) *out_moved = moved;
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_apply_stroke(clay_mesh_sculptor* sculptor,
                                            const float* samples_xyzpt, size_t sample_count,
                                            const clay_stroke_preset* preset,
                                            const clay_mesh_brush_desc* desc, const clay_mask* mask,
                                            const clay_mesh_frame* mesh_to_world,
                                            int32_t defer_normals, clay_mesh_deltas* deltas,
                                            size_t* out_applied) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    mesh::MeshBrush verb = mesh::MeshBrush::Draw;
    mesh::MeshBrushSettings settings;
    r = read_mesh_brush(desc, &verb, &settings);
    if (r != CLAY_OK) return r;
    brush::MeshStrokeOptions options;
    options.defer_normals = defer_normals != 0;
    r = read_mesh_frame(mesh_to_world, &options.mesh_to_world);
    if (r != CLAY_OK) return r;

    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset resolved;
    r = read_stroke(samples_xyzpt, sample_count, preset, &samples, &resolved);
    if (r != CLAY_OK) return r;

    voxel::MaskField* field_mask = nullptr;
    if (mask) {
        r = resolve_mask(mask, &field_mask);
        if (r != CLAY_OK) return r;
    }
    const std::size_t applied =
        brush::apply_to_mesh(*sculptor->sculptor, brush::resolve_stroke(samples, resolved), verb,
                             settings, field_mask, deltas ? &deltas->deltas : nullptr, options);
    if (out_applied) *out_applied = applied;
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_apply_preset(clay_mesh_sculptor* sculptor,
                                            const float* samples_xyzpt, size_t sample_count,
                                            const clay_brush_preset* preset, const float* alpha,
                                            int32_t alpha_width, int32_t alpha_height,
                                            int32_t orient_alpha_by_stamp, const clay_mask* mask,
                                            const clay_mesh_frame* mesh_to_world,
                                            int32_t defer_normals, clay_mesh_deltas* deltas,
                                            size_t* out_applied) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    brush::BrushPreset p;
    r = read_brush_preset(preset, &p);
    if (r != CLAY_OK) return r;

    // The alpha is BORROWED, never copied and never stored in a preset: image
    // content is the caller's, and a preset library has to cost kilobytes.
    if (alpha) {
        if (alpha_width < 2 || alpha_height < 2)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "an alpha needs at least 2x2 samples; there is nothing to interpolate "
                        "below that");
        p.settings.alpha = alpha;
        p.settings.alpha_width = alpha_width;
        p.settings.alpha_height = alpha_height;
    }

    brush::MeshStrokeOptions options;
    options.defer_normals = defer_normals != 0;
    options.orient_alpha_by_stamp = orient_alpha_by_stamp != 0;
    r = read_mesh_frame(mesh_to_world, &options.mesh_to_world);
    if (r != CLAY_OK) return r;

    std::vector<brush::StrokeSample> samples;
    r = read_samples(samples_xyzpt, sample_count, &samples);
    if (r != CLAY_OK) return r;

    voxel::MaskField* field_mask = nullptr;
    if (mask) {
        r = resolve_mask(mask, &field_mask);
        if (r != CLAY_OK) return r;
    }
    const std::size_t applied = brush::apply_to_mesh(
        *sculptor->sculptor, brush::resolve_stroke(samples, p.stroke), p.model.verb, p.settings,
        field_mask, deltas ? &deltas->deltas : nullptr, options);
    if (out_applied) *out_applied = applied;
    return CLAY_OK;
}

// -- adaptive topology --------------------------------------------------------

namespace {

constexpr std::size_t kDynSurfaceDescOriginal =
    offsetof(clay_dynamic_surface_desc, uv_seam_epsilon) + sizeof(float);
constexpr std::size_t kDynStatsOriginal =
    offsetof(clay_dynamic_surface_stats, bytes) + sizeof(std::uint64_t);
constexpr std::size_t kRevisionOriginal =
    offsetof(clay_surface_revision, attributes) + sizeof(std::uint64_t);
constexpr std::size_t kDynTopologyOriginal =
    offsetof(clay_dynamic_topology_desc, preserve_sharp_edges) + sizeof(std::int32_t);
constexpr std::size_t kDynReportOriginal =
    offsetof(clay_dynamic_stamp_report, revision) + sizeof(clay_surface_revision);
constexpr std::size_t kChunkInfoOriginal =
    offsetof(clay_dynamic_chunk_info, bounds_max) + sizeof(float) * 3;

clay_surface_revision to_c_revision(const mesh::DynamicSurface& s) {
    clay_surface_revision r{};
    r.struct_size = sizeof(clay_surface_revision);
    r.topology = s.topology_revision();
    r.geometry = s.geometry_revision();
    r.attributes = s.attribute_revision();
    return r;
}

}  // namespace

clay_result clay_dynamic_surface_defaults(clay_dynamic_surface_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null descriptor");
    clay_dynamic_surface_desc probe;
    clay_result r = read_desc(out_desc, kDynSurfaceDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const mesh::DynamicSurfaceBuildOptions d;
    clay_dynamic_surface_desc out{};
    out.weld_epsilon = d.weld_epsilon;
    out.uv_seam_epsilon = d.uv_seam_epsilon;
    write_desc(out_desc, out_desc->struct_size, out);
    return CLAY_OK;
}

clay_result clay_dynamic_surface_from_mesh(const clay_mesh* mesh_handle,
                                           const clay_dynamic_surface_desc* desc,
                                           clay_dynamic_surface** out_surface,
                                           int32_t* out_error) {
    if (out_error) *out_error = CLAY_DYNAMIC_OK;
    if (!out_surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_surface");
    *out_surface = nullptr;
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(mesh_handle, &src);
    if (r != CLAY_OK) return r;

    mesh::DynamicSurfaceBuildOptions options;
    if (desc) {
        clay_dynamic_surface_desc d;
        r = read_desc(desc, kDynSurfaceDescOriginal, &d);
        if (r != CLAY_OK) return r;
        if (d.weld_epsilon > 0.0f) options.weld_epsilon = d.weld_epsilon;
        if (d.uv_seam_epsilon > 0.0f) options.uv_seam_epsilon = d.uv_seam_epsilon;
    }

    mesh::DynamicBuildError err = mesh::DynamicBuildError::None;
    std::optional<mesh::DynamicSurface> built = mesh::DynamicSurface::from_mesh(*src, options, &err);
    if (!built) {
        if (out_error) *out_error = static_cast<std::int32_t>(err);
        // The reason is reported through `out_error` AND named in the message,
        // because a caller fixing a model needs to know which problem it hit
        // and a caller logging one needs it readable.
        const char* what = "a dynamic surface cannot be built from this mesh";
        switch (err) {
            case mesh::DynamicBuildError::EmptyMesh:
                what = "empty mesh, or an index count that is not a multiple of three";
                break;
            case mesh::DynamicBuildError::IndexOutOfRange:
                what = "a triangle index is past the end of the position array";
                break;
            case mesh::DynamicBuildError::DegenerateTriangle:
                what = "a triangle whose corners weld together has no area";
                break;
            case mesh::DynamicBuildError::NonManifoldEdge:
                what = "three or more faces on one edge; a half-edge surface cannot express it";
                break;
            default:
                break;
        }
        return fail(CLAY_ERROR_INVALID_ARGUMENT, what);
    }
    *out_surface = new clay_dynamic_surface{std::move(*built)};
    return CLAY_OK;
}

clay_result clay_dynamic_surface_to_mesh(const clay_dynamic_surface* surface,
                                         clay_mesh** out_mesh) {
    if (!surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic surface");
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_mesh");
    clay_mesh* handle = new clay_mesh{};
    handle->data = surface->surface.to_mesh();
    *out_mesh = handle;
    return CLAY_OK;
}

void clay_dynamic_surface_destroy(clay_dynamic_surface* surface) { delete surface; }

clay_result clay_dynamic_surface_stats_get(const clay_dynamic_surface* surface,
                                           clay_dynamic_surface_stats* out_stats) {
    if (!surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic surface");
    if (!out_stats) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_stats");
    clay_dynamic_surface_stats probe;
    clay_result r = read_desc(out_stats, kDynStatsOriginal, &probe);
    if (r != CLAY_OK) return r;
    const mesh::DynamicSurfaceStats st = surface->surface.stats();
    clay_dynamic_surface_stats out{};
    out.vertices = st.vertices;
    out.edges = st.edges;
    out.halfedges = st.halfedges;
    out.faces = st.faces;
    out.boundary_edges = st.boundary_edges;
    out.dead_slots = st.dead_slots;
    out.bytes = surface->surface.bytes();
    write_desc(out_stats, out_stats->struct_size, out);
    return CLAY_OK;
}

clay_result clay_dynamic_surface_revision(const clay_dynamic_surface* surface,
                                          clay_surface_revision* out_revision) {
    if (!surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic surface");
    if (!out_revision) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_revision");
    clay_surface_revision probe;
    clay_result r = read_desc(out_revision, kRevisionOriginal, &probe);
    if (r != CLAY_OK) return r;
    write_desc(out_revision, out_revision->struct_size, to_c_revision(surface->surface));
    return CLAY_OK;
}

clay_result clay_dynamic_surface_serialize(const clay_dynamic_surface* surface,
                                           uint8_t* out_data, size_t* count) {
    if (!surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic surface");
    const std::vector<std::uint8_t> bytes = surface->surface.encode();
    return write_sized(bytes.data(), bytes.size(), out_data, count, "dynamic surface");
}

clay_result clay_dynamic_surface_deserialize(const uint8_t* data, size_t size,
                                             clay_dynamic_surface** out_surface) {
    if (!out_surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_surface");
    *out_surface = nullptr;
    if (!data || size == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null or empty data");
    mesh::DynamicSurface built;
    if (!mesh::DynamicSurface::decode(data, size, &built))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "not a dynamic surface this build can read: malformed, truncated, or "
                    "written by a newer schema version");
    *out_surface = new clay_dynamic_surface{std::move(built)};
    return CLAY_OK;
}

clay_result clay_dynamic_surface_validate(const clay_dynamic_surface* surface, int32_t* out_ok,
                                          char* out_message, size_t* message_len) {
    if (!surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic surface");
    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(surface->surface);
    if (out_ok) *out_ok = report.ok ? 1 : 0;
    const std::string text = report.summary();
    return write_sized(reinterpret_cast<const std::uint8_t*>(text.c_str()), text.size() + 1,
                       reinterpret_cast<std::uint8_t*>(out_message), message_len,
                       "validation message");
}

clay_result clay_dynamic_topology_defaults(clay_dynamic_topology_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null descriptor");
    clay_dynamic_topology_desc probe;
    clay_result r = read_desc(out_desc, kDynTopologyOriginal, &probe);
    if (r != CLAY_OK) return r;
    const mesh::DynamicTopologySettings d;
    clay_dynamic_topology_desc out{};
    out.enabled = d.enabled ? 1 : 0;
    out.detail_mode = static_cast<std::int32_t>(d.detail_mode);
    out.target_edge_length = d.target_edge_length;
    out.detail_resolution = d.detail_resolution;
    out.split_factor = d.split_factor;
    out.collapse_factor = d.collapse_factor;
    out.max_passes = d.max_passes;
    out.max_ops_per_stamp = d.max_ops_per_stamp;
    out.allow_split = d.allow_split ? 1 : 0;
    out.allow_collapse = d.allow_collapse ? 1 : 0;
    out.allow_flip = d.allow_flip ? 1 : 0;
    out.relax_after_remesh = d.relax_after_remesh ? 1 : 0;
    out.relax_strength = d.relax_strength;
    out.preserve_boundaries = d.preserve_boundaries ? 1 : 0;
    out.preserve_uv_seams = d.preserve_uv_seams ? 1 : 0;
    out.preserve_sharp_edges = d.preserve_sharp_edges ? 1 : 0;
    write_desc(out_desc, out_desc->struct_size, out);
    return CLAY_OK;
}

clay_result clay_dynamic_sculptor_create(clay_dynamic_surface* surface,
                                         clay_dynamic_sculptor** out_sculptor) {
    if (!surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic surface");
    if (!out_sculptor) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_sculptor");
    clay_dynamic_sculptor* handle = new clay_dynamic_sculptor{};
    handle->owner = surface;
    handle->sculptor = std::make_unique<mesh::DynamicSculptor>(surface->surface);
    *out_sculptor = handle;
    return CLAY_OK;
}

void clay_dynamic_sculptor_destroy(clay_dynamic_sculptor* sculptor) { delete sculptor; }

clay_result clay_dynamic_sculptor_stamp(clay_dynamic_sculptor* sculptor,
                                        const clay_mesh_brush_desc* brush,
                                        const clay_dynamic_topology_desc* topology,
                                        const clay_mask* mask,
                                        clay_dynamic_stamp_report* out_report) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic sculptor");
    mesh::MeshBrush verb = mesh::MeshBrush::Draw;
    mesh::MeshBrushSettings settings;
    clay_result r = read_mesh_brush(brush, &verb, &settings);
    if (r != CLAY_OK) return r;
    if (!mesh::dynamic_offers(verb))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "an adaptive surface does not offer this verb; see dynamic_offers");

    mesh::DynamicTopologySettings topo;
    if (topology) {
        clay_dynamic_topology_desc d;
        r = read_desc(topology, kDynTopologyOriginal, &d);
        if (r != CLAY_OK) return r;
        if (d.detail_mode < 0 || d.detail_mode > 2)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "unknown detail mode: " + std::to_string(d.detail_mode));
        topo.enabled = d.enabled != 0;
        topo.detail_mode = static_cast<mesh::DynamicDetailMode>(d.detail_mode);
        if (d.target_edge_length > 0.0f) topo.target_edge_length = d.target_edge_length;
        if (d.detail_resolution > 0.0f) topo.detail_resolution = d.detail_resolution;
        if (d.split_factor > 0.0f) topo.split_factor = d.split_factor;
        if (d.collapse_factor > 0.0f) topo.collapse_factor = d.collapse_factor;
        if (d.max_passes > 0) topo.max_passes = d.max_passes;
        if (d.max_ops_per_stamp > 0) topo.max_ops_per_stamp = d.max_ops_per_stamp;
        topo.allow_split = d.allow_split != 0;
        topo.allow_collapse = d.allow_collapse != 0;
        topo.allow_flip = d.allow_flip != 0;
        topo.relax_after_remesh = d.relax_after_remesh != 0;
        if (d.relax_strength > 0.0f) topo.relax_strength = d.relax_strength;
        topo.preserve_boundaries = d.preserve_boundaries != 0;
        topo.preserve_uv_seams = d.preserve_uv_seams != 0;
        topo.preserve_sharp_edges = d.preserve_sharp_edges != 0;
    }

    field::MaskGate gate;
    if (mask) {
        voxel::MaskField* field_mask = nullptr;
        r = resolve_mask(mask, &field_mask);
        if (r != CLAY_OK) return r;
        gate = [field_mask](kernel::cfloat3 p) { return field_mask->sample(p); };
    }

    const mesh::DynamicStampResult res =
        sculptor->sculptor->stamp(verb, settings, topo, gate, nullptr);

    if (out_report) {
        clay_dynamic_stamp_report probe;
        r = read_desc(out_report, kDynReportOriginal, &probe);
        if (r != CLAY_OK) return r;
        clay_dynamic_stamp_report out{};
        out.moved_vertices = res.moved_vertices;
        out.split_edges = res.remesh.split;
        out.collapsed_edges = res.remesh.collapsed;
        out.flipped_edges = res.remesh.flipped;
        out.relaxed_vertices = res.remesh.relaxed;
        out.hit_budget = res.remesh.hit_budget ? 1 : 0;
        if (!res.dirty_bounds.empty()) {
            write_f3(out.dirty_min, res.dirty_bounds.min);
            write_f3(out.dirty_max, res.dirty_bounds.max);
        }
        out.revision = to_c_revision(sculptor->owner->surface);
        write_desc(out_report, out_report->struct_size, out);
    }
    return CLAY_OK;
}

clay_result clay_dynamic_sculptor_rebuild_index(clay_dynamic_sculptor* sculptor) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic sculptor");
    sculptor->sculptor->rebuild_index();
    return CLAY_OK;
}

size_t clay_dynamic_surface_chunk_count(const clay_dynamic_sculptor* sculptor) {
    if (!sculptor || !sculptor->sculptor) return 0;
    return sculptor->sculptor->bvh().leaf_count();
}

clay_result clay_dynamic_surface_chunk_info(const clay_dynamic_sculptor* sculptor, size_t index,
                                            clay_dynamic_chunk_info* out_info) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic sculptor");
    if (!out_info) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_info");
    clay_dynamic_chunk_info probe;
    clay_result r = read_desc(out_info, kChunkInfoOriginal, &probe);
    if (r != CLAY_OK) return r;
    const mesh::SurfaceLeaf* leaf = sculptor->sculptor->bvh().leaf(static_cast<std::uint32_t>(index));
    if (!leaf) return fail(CLAY_ERROR_NOT_FOUND, "chunk index " + std::to_string(index));

    clay_dynamic_chunk_info out{};
    out.index = static_cast<std::uint32_t>(index);
    out.revision = leaf->revision;
    // THREE VERTICES PER FACE, unwelded within the chunk. A chunk is a
    // standalone draw, so its indices are local and its vertices are its own;
    // welding across a chunk boundary would make one chunk's upload depend on
    // another's.
    out.vertex_count = static_cast<std::uint32_t>(leaf->faces.size() * 3);
    out.index_count = out.vertex_count;
    out.geometry_dirty = leaf->geometry_dirty ? 1 : 0;
    out.topology_dirty = leaf->topology_dirty ? 1 : 0;
    if (!leaf->bounds.empty()) {
        write_f3(out.bounds_min, leaf->bounds.min);
        write_f3(out.bounds_max, leaf->bounds.max);
    }
    write_desc(out_info, out_info->struct_size, out);
    return CLAY_OK;
}

clay_result clay_dynamic_surface_dirty_chunks(const clay_dynamic_sculptor* sculptor,
                                              uint32_t* out_indices, size_t* count) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic sculptor");
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    const std::vector<std::uint32_t>& dirty = sculptor->sculptor->bvh().dirty_leaves();
    // The size-query pattern by hand: `write_sized` copies BYTES and these are
    // indices, so a byte-wise fill would write a quarter of each one.
    if (!out_indices) {
        *count = dirty.size();
        return CLAY_OK;
    }
    if (*count < dirty.size()) {
        *count = dirty.size();
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "capacity below the " + std::to_string(dirty.size()) + " dirty chunks");
    }
    for (std::size_t i = 0; i < dirty.size(); ++i) out_indices[i] = dirty[i];
    *count = dirty.size();
    return CLAY_OK;
}

clay_result clay_dynamic_surface_clear_dirty(clay_dynamic_sculptor* sculptor) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic sculptor");
    sculptor->sculptor->bvh().clear_dirty();
    return CLAY_OK;
}

clay_result clay_dynamic_surface_copy_chunk(const clay_dynamic_sculptor* sculptor, size_t index,
                                            float* out_positions, size_t position_capacity,
                                            float* out_normals, size_t normal_capacity,
                                            uint32_t* out_indices, size_t index_capacity,
                                            clay_dynamic_chunk_info* out_written) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null dynamic sculptor");
    const mesh::DynamicSculptor& s = *sculptor->sculptor;
    const mesh::SurfaceLeaf* leaf = s.bvh().leaf(static_cast<std::uint32_t>(index));
    if (!leaf) return fail(CLAY_ERROR_NOT_FOUND, "chunk index " + std::to_string(index));

    const mesh::DynamicSurface& surface = s.surface();
    std::size_t live_faces = 0;
    for (mesh::FaceId f : leaf->faces)
        if (surface.live(f)) ++live_faces;
    const std::size_t needed_floats = live_faces * 9;
    const std::size_t needed_indices = live_faces * 3;

    if (out_written) {
        clay_dynamic_chunk_info probe;
        clay_result r = read_desc(out_written, kChunkInfoOriginal, &probe);
        if (r != CLAY_OK) return r;
    }
    // THE CAPACITY QUERY: a null buffer reports what it would need and writes
    // nothing, so a host sizes once and copies once.
    if (!out_positions && !out_indices) {
        if (out_written) {
            clay_dynamic_chunk_info out{};
            out.index = static_cast<std::uint32_t>(index);
            out.revision = leaf->revision;
            out.vertex_count = static_cast<std::uint32_t>(live_faces * 3);
            out.index_count = static_cast<std::uint32_t>(needed_indices);
            write_desc(out_written, out_written->struct_size, out);
        }
        return CLAY_OK;
    }
    if (out_positions && position_capacity < needed_floats)
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "position capacity " + std::to_string(position_capacity) + " below the " +
                        std::to_string(needed_floats) + " floats this chunk needs");
    if (out_normals && normal_capacity < needed_floats)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "normal capacity below what this chunk needs");
    if (out_indices && index_capacity < needed_indices)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "index capacity below what this chunk needs");

    std::size_t vi = 0;
    for (mesh::FaceId f : leaf->faces) {
        if (!surface.live(f)) continue;
        mesh::VertexId v[3];
        if (!surface.face_vertices(f, v)) continue;
        for (int k = 0; k < 3; ++k) {
            const mesh::DynamicVertex* rec = surface.vertex(v[k]);
            const kernel::cfloat3 p = rec ? rec->position : kernel::cf3(0, 0, 0);
            const kernel::cfloat3 n = rec ? rec->normal : kernel::cf3(0, 1, 0);
            if (out_positions) {
                out_positions[vi * 3 + 0] = p.x;
                out_positions[vi * 3 + 1] = p.y;
                out_positions[vi * 3 + 2] = p.z;
            }
            if (out_normals) {
                out_normals[vi * 3 + 0] = n.x;
                out_normals[vi * 3 + 1] = n.y;
                out_normals[vi * 3 + 2] = n.z;
            }
            if (out_indices) out_indices[vi] = static_cast<std::uint32_t>(vi);
            ++vi;
        }
    }
    if (out_written) {
        clay_dynamic_chunk_info out{};
        out.index = static_cast<std::uint32_t>(index);
        out.revision = leaf->revision;
        out.vertex_count = static_cast<std::uint32_t>(vi);
        out.index_count = static_cast<std::uint32_t>(vi);
        out.geometry_dirty = leaf->geometry_dirty ? 1 : 0;
        out.topology_dirty = leaf->topology_dirty ? 1 : 0;
        if (!leaf->bounds.empty()) {
            write_f3(out.bounds_min, leaf->bounds.min);
            write_f3(out.bounds_max, leaf->bounds.max);
        }
        write_desc(out_written, out_written->struct_size, out);
    }
    return CLAY_OK;
}

// -- multiresolution surfaces (mesh-multires spec, add-mesh-multires) ---------
//
// OPAQUE AND OWNING, the same shape the adaptive surface has: the hierarchy
// outlives the sculptor, and the sculptor keeps the owner rather than a bare
// reference so a report can read the owner's revisions after the stamp.

namespace {

constexpr std::size_t kMultiresDescOriginal =
    offsetof(clay_multires_desc, memory_budget) + sizeof(std::uint64_t);
constexpr std::size_t kMultiresPreflightOriginal =
    offsetof(clay_multires_preflight, error) + sizeof(std::int32_t);
constexpr std::size_t kMultiresMemoryOriginal =
    offsetof(clay_multires_memory, total) + sizeof(std::uint64_t);
constexpr std::size_t kMultiresProjectDescOriginal =
    offsetof(clay_multires_project_desc, strength) + sizeof(float);
constexpr std::size_t kMultiresProjectReportOriginal =
    offsetof(clay_multires_project_report, mean_offset) + sizeof(float);
constexpr std::size_t kMultiresStampReportOriginal =
    offsetof(clay_multires_stamp_report, evaluated_revision) + sizeof(std::uint64_t);
constexpr std::size_t kMultiresBlockInfoOriginal =
    offsetof(clay_multires_block_info, index_count) + sizeof(std::uint32_t);

// Two names rather than an overload: this region is `extern "C"`, where a
// second function of the same name is a redeclaration rather than an overload.
clay_result resolve_multires_ro(const clay_multires* handle, const mesh::MultiresSurface** out);
clay_result resolve_multires(clay_multires* handle, mesh::MultiresSurface** out);

}  // namespace

struct clay_multires {
    mesh::MultiresSurface surface;
    // Scratch the block copy fills, kept on the handle so a host draining a
    // hundred blocks a frame does not allocate a hundred times.
    mesh::MultiresSurface::Block block;
};

struct clay_multires_sculptor {
    clay_multires* owner = nullptr;
    std::unique_ptr<mesh::MultiresSculptor> sculptor;
};

namespace {

clay_result resolve_multires_ro(const clay_multires* handle, const mesh::MultiresSurface** out) {
    if (!handle) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null multires surface");
    if (!handle->surface.valid()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty hierarchy");
    *out = &handle->surface;
    return CLAY_OK;
}

clay_result resolve_multires(clay_multires* handle, mesh::MultiresSurface** out) {
    if (!handle) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null multires surface");
    if (!handle->surface.valid()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty hierarchy");
    *out = &handle->surface;
    return CLAY_OK;
}

}  // namespace

const char* clay_multires_error_text(int32_t error) {
    return mesh::multires_error_text(static_cast<mesh::MultiresError>(error));
}

clay_result clay_multires_defaults(clay_multires_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_desc");
    const std::uint32_t declared = out_desc->struct_size;
    clay_multires_desc probe;
    clay_result r = read_desc(out_desc, kMultiresDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const mesh::MultiresOptions defaults;
    clay_multires_desc out{};
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.rule = static_cast<std::int32_t>(defaults.rule);
    out.weld_epsilon = defaults.weld_epsilon;
    out.memory_budget = defaults.memory_budget;
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

clay_result clay_multires_from_mesh(const clay_mesh* mesh_handle, const clay_multires_desc* desc,
                                    clay_multires** out_surface, int32_t* out_error) {
    if (out_error) *out_error = CLAY_MULTIRES_OK;
    if (!out_surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_surface");
    *out_surface = nullptr;
    const mesh::Mesh* src = nullptr;
    clay_result r = resolve_mesh(mesh_handle, &src);
    if (r != CLAY_OK) return r;

    mesh::MultiresOptions options;
    if (desc) {
        clay_multires_desc d;
        r = read_desc(desc, kMultiresDescOriginal, &d);
        if (r != CLAY_OK) return r;
        if (d.rule != CLAY_SUBDIVISION_CATMULL_CLARK)
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "unknown subdivision rule: " + std::to_string(d.rule));
        options.rule = static_cast<mesh::SubdivisionRule>(d.rule);
        if (d.weld_epsilon > 0.0f) options.weld_epsilon = d.weld_epsilon;
        options.memory_budget = d.memory_budget;
    }

    mesh::MultiresError err = mesh::MultiresError::None;
    std::optional<mesh::MultiresSurface> built =
        mesh::MultiresSurface::from_mesh(*src, options, &err);
    if (!built) {
        if (out_error) *out_error = static_cast<std::int32_t>(err);
        // Reported through `out_error` AND named in the message, because a
        // caller fixing a model needs to know which problem it hit and a caller
        // logging one needs it readable.
        return fail(CLAY_ERROR_INVALID_ARGUMENT, mesh::multires_error_text(err));
    }
    auto* handle = new clay_multires{};
    handle->surface = std::move(*built);
    *out_surface = handle;
    return CLAY_OK;
}

void clay_multires_destroy(clay_multires* surface) { delete surface; }

uint32_t clay_multires_level_count(const clay_multires* surface) {
    return surface ? surface->surface.level_count() : 0u;
}

clay_result clay_multires_sculpt_level(const clay_multires* surface, uint32_t* out_level) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_level) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_level");
    *out_level = s->sculpt_level();
    return CLAY_OK;
}

clay_result clay_multires_display_level(const clay_multires* surface, uint32_t* out_level) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_level) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_level");
    *out_level = s->display_level();
    return CLAY_OK;
}

clay_result clay_multires_set_sculpt_level(clay_multires* surface, uint32_t level) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    if (!s->set_sculpt_level(level))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    return CLAY_OK;
}

clay_result clay_multires_set_display_level(clay_multires* surface, uint32_t level) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    if (!s->set_display_level(level))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    return CLAY_OK;
}

clay_result clay_multires_level_counts(const clay_multires* surface, uint32_t level,
                                       uint64_t* out_vertices, uint64_t* out_faces) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (level >= s->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    if (out_vertices) *out_vertices = s->topology_at(level).vertex_count;
    if (out_faces) *out_faces = s->topology_at(level).face_count;
    return CLAY_OK;
}

clay_result clay_multires_preflight_add_level(const clay_multires* surface,
                                              clay_multires_preflight* out_preflight) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_preflight) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_preflight");
    const std::uint32_t declared = out_preflight->struct_size;
    clay_multires_preflight probe;
    r = read_desc(out_preflight, kMultiresPreflightOriginal, &probe);
    if (r != CLAY_OK) return r;

    const mesh::MultiresPreflight p = s->preflight_add_level();
    clay_multires_preflight out{};
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.level = p.level;
    out.vertices = p.vertices;
    out.faces = p.faces;
    out.topology_bytes = p.topology_bytes;
    out.detail_bytes = p.detail_bytes;
    out.evaluated_bytes = p.evaluated_bytes;
    out.runtime_bytes = p.runtime_bytes;
    out.persistent_bytes = p.persistent_bytes;
    out.peak_bytes = p.peak_bytes;
    out.allowed = p.allowed ? 1 : 0;
    out.error = static_cast<std::int32_t>(p.error);
    write_desc(out_preflight, declared, out);
    return CLAY_OK;
}

clay_result clay_multires_add_level(clay_multires* surface, clay_cancel_token* token,
                                    int32_t* out_error) {
    if (out_error) *out_error = CLAY_MULTIRES_OK;
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    mesh::MultiresError err = mesh::MultiresError::None;
    const parallel::CancelToken* cancel = token ? &token->token : nullptr;
    if (!s->add_level(&err, cancel)) {
        if (out_error) *out_error = static_cast<std::int32_t>(err);
        if (err == mesh::MultiresError::Cancelled) return CLAY_ERROR_CANCELLED;
        // A BUDGET refusal is not an argument error: the caller asked a
        // reasonable question and the answer is no. It reports through
        // out_error and the message names the number.
        return fail(CLAY_ERROR_INVALID_ARGUMENT, mesh::multires_error_text(err));
    }
    return CLAY_OK;
}

clay_result clay_multires_remove_highest_level(clay_multires* surface, int32_t* out_error) {
    if (out_error) *out_error = CLAY_MULTIRES_OK;
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    mesh::MultiresError err = mesh::MultiresError::None;
    if (!s->remove_highest_level(&err)) {
        if (out_error) *out_error = static_cast<std::int32_t>(err);
        return fail(CLAY_ERROR_INVALID_ARGUMENT, mesh::multires_error_text(err));
    }
    return CLAY_OK;
}

clay_result clay_multires_copy_level_mesh(clay_multires* surface, uint32_t level,
                                          clay_mesh** out_mesh) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_mesh");
    *out_mesh = nullptr;
    if (level >= s->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    clay_mesh* handle = new clay_mesh{};
    handle->data = s->mesh_at_level(level);
    *out_mesh = handle;
    return CLAY_OK;
}

clay_result clay_multires_revision(const clay_multires* surface, uint64_t* out_base,
                                   uint64_t* out_detail, uint64_t* out_evaluated) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (out_base) *out_base = s->base_revision();
    if (out_detail) *out_detail = s->detail_revision();
    if (out_evaluated) *out_evaluated = s->evaluated_revision();
    return CLAY_OK;
}

clay_result clay_multires_detail_checksum(const clay_multires* surface, uint64_t* out_checksum) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_checksum) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_checksum");
    *out_checksum = s->detail_checksum();
    return CLAY_OK;
}

clay_result clay_multires_memory_get(const clay_multires* surface,
                                     clay_multires_memory* out_memory) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_memory) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_memory");
    const std::uint32_t declared = out_memory->struct_size;
    clay_multires_memory probe;
    r = read_desc(out_memory, kMultiresMemoryOriginal, &probe);
    if (r != CLAY_OK) return r;

    const mesh::MultiresMemory m = s->memory();
    clay_multires_memory out{};
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.resident_levels = m.resident_levels;
    out.base = m.base;
    out.topology = m.topology;
    out.detail = m.detail;
    out.authoritative = m.authoritative;
    out.evaluated = m.evaluated;
    out.runtime_index = m.runtime_index;
    out.rebuildable = m.rebuildable;
    out.total = m.total;
    write_desc(out_memory, declared, out);
    return CLAY_OK;
}

clay_result clay_multires_drop_inactive_caches(clay_multires* surface) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    s->drop_inactive_caches();
    return CLAY_OK;
}

clay_result clay_multires_serialize(const clay_multires* surface, uint8_t* out_data,
                                    size_t* size) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null size");
    const std::vector<std::uint8_t> bytes = s->encode();
    if (!out_data) {
        *size = bytes.size();
        return CLAY_OK;
    }
    if (*size < bytes.size()) {
        *size = bytes.size();
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "buffer too small for the hierarchy");
    }
    std::memcpy(out_data, bytes.data(), bytes.size());
    *size = bytes.size();
    return CLAY_OK;
}

clay_result clay_multires_deserialize(const uint8_t* data, size_t size,
                                      clay_multires** out_surface) {
    if (!out_surface) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_surface");
    *out_surface = nullptr;
    if (!data || size == 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty buffer");
    mesh::MultiresSurface decoded;
    if (!mesh::MultiresSurface::decode(data, size, &decoded))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    mesh::multires_error_text(mesh::MultiresError::Decode));
    auto* handle = new clay_multires{};
    handle->surface = std::move(decoded);
    *out_surface = handle;
    return CLAY_OK;
}

clay_result clay_multires_project_defaults(clay_multires_project_desc* out_desc) {
    if (!out_desc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_desc");
    const std::uint32_t declared = out_desc->struct_size;
    clay_multires_project_desc probe;
    clay_result r = read_desc(out_desc, kMultiresProjectDescOriginal, &probe);
    if (r != CLAY_OK) return r;
    const mesh::ProjectOptions defaults;
    clay_multires_project_desc out{};
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.max_distance = defaults.max_distance;
    out.normal_ray_first = defaults.normal_ray_first ? 1 : 0;
    out.strength = defaults.strength;
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

clay_result clay_multires_project(clay_multires* surface, const clay_mesh* reference,
                                  const clay_multires_project_desc* desc, clay_cancel_token* token,
                                  clay_multires_project_report* out_report) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    const mesh::Mesh* src = nullptr;
    r = resolve_mesh(reference, &src);
    if (r != CLAY_OK) return r;

    mesh::ProjectOptions options;
    if (desc) {
        clay_multires_project_desc d;
        r = read_desc(desc, kMultiresProjectDescOriginal, &d);
        if (r != CLAY_OK) return r;
        if (d.max_distance > 0.0f) options.max_distance = d.max_distance;
        options.normal_ray_first = d.normal_ray_first != 0;
        if (d.strength > 0.0f) options.strength = d.strength;
    }

    mesh::ProjectReport report;
    const parallel::CancelToken* cancel = token ? &token->token : nullptr;
    const bool ok = s->project_from(*src, options, &report, cancel);
    if (out_report) {
        const std::uint32_t declared = out_report->struct_size;
        clay_multires_project_report probe;
        r = read_desc(out_report, kMultiresProjectReportOriginal, &probe);
        if (r != CLAY_OK) return r;
        clay_multires_project_report out{};
        out.struct_size = static_cast<std::uint32_t>(sizeof(out));
        out.moved = report.moved;
        out.missed = report.missed;
        out.by_ray = report.by_ray;
        out.by_closest = report.by_closest;
        out.max_offset = report.max_offset;
        out.mean_offset = static_cast<float>(report.mean_offset);
        write_desc(out_report, declared, out);
    }
    if (!ok) {
        if (cancel && cancel->cancelled()) return CLAY_ERROR_CANCELLED;
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "the hierarchy has no level above its cage to project");
    }
    return CLAY_OK;
}

clay_result clay_multires_sculptor_create(clay_multires* surface,
                                          clay_multires_sculptor** out_sculptor) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    if (!out_sculptor) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_sculptor");
    auto* handle = new clay_multires_sculptor{};
    handle->owner = surface;
    handle->sculptor = std::make_unique<mesh::MultiresSculptor>(*s);
    *out_sculptor = handle;
    return CLAY_OK;
}

void clay_multires_sculptor_destroy(clay_multires_sculptor* sculptor) { delete sculptor; }

clay_result clay_multires_sculptor_begin_stroke(clay_multires_sculptor* sculptor) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null multires sculptor");
    sculptor->sculptor->begin_stroke();
    return CLAY_OK;
}

clay_result clay_multires_sculptor_stamp(clay_multires_sculptor* sculptor,
                                         const clay_mesh_brush_desc* brush, const clay_mask* mask,
                                         clay_multires_stamp_report* out_report) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null multires sculptor");
    mesh::MeshBrush verb = mesh::MeshBrush::Draw;
    mesh::MeshBrushSettings settings;
    clay_result r = read_mesh_brush(brush, &verb, &settings);
    if (r != CLAY_OK) return r;

    field::MaskGate gate;
    if (mask) {
        voxel::MaskField* field_mask = nullptr;
        r = resolve_mask(mask, &field_mask);
        if (r != CLAY_OK) return r;
        gate = [field_mask](kernel::cfloat3 p) { return field_mask->sample(p); };
    }

    mesh::MultiresSurface& s = sculptor->owner->surface;
    const std::uint32_t level = s.sculpt_level();
    const std::size_t moved = sculptor->sculptor->stamp(verb, settings, gate, nullptr);

    if (out_report) {
        const std::uint32_t declared = out_report->struct_size;
        clay_multires_stamp_report probe;
        r = read_desc(out_report, kMultiresStampReportOriginal, &probe);
        if (r != CLAY_OK) return r;
        clay_multires_stamp_report out{};
        out.struct_size = static_cast<std::uint32_t>(sizeof(out));
        out.level = level;
        out.moved_vertices = moved;
        out.base_revision = s.base_revision();
        out.detail_revision = s.detail_revision();
        out.evaluated_revision = s.evaluated_revision();
        write_desc(out_report, declared, out);
    }
    return CLAY_OK;
}

clay_result clay_multires_sculptor_apply_stroke(clay_multires_sculptor* sculptor,
                                                const float* samples_xyzpt, size_t sample_count,
                                                const clay_stroke_preset* preset,
                                                const clay_mesh_brush_desc* brush,
                                                const clay_mask* mask,
                                                const clay_mesh_frame* mesh_to_world,
                                                int32_t defer_normals, size_t* out_applied,
                                                clay_multires_stamp_report* out_report) {
    if (!sculptor || !sculptor->sculptor)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null multires sculptor");
    mesh::MeshBrush verb = mesh::MeshBrush::Draw;
    mesh::MeshBrushSettings settings;
    clay_result r = read_mesh_brush(brush, &verb, &settings);
    if (r != CLAY_OK) return r;

    brush::MeshStrokeOptions options;
    options.defer_normals = defer_normals != 0;
    r = read_mesh_frame(mesh_to_world, &options.mesh_to_world);
    if (r != CLAY_OK) return r;

    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset resolved;
    r = read_stroke(samples_xyzpt, sample_count, preset, &samples, &resolved);
    if (r != CLAY_OK) return r;

    voxel::MaskField* field_mask = nullptr;
    if (mask) {
        r = resolve_mask(mask, &field_mask);
        if (r != CLAY_OK) return r;
    }

    mesh::MultiresSurface& s = sculptor->owner->surface;
    const std::uint32_t level = s.sculpt_level();
    const std::size_t applied =
        brush::apply_to_multires(*sculptor->sculptor, brush::resolve_stroke(samples, resolved),
                                 verb, settings, field_mask, nullptr, options);
    if (out_applied) *out_applied = applied;
    if (out_report) {
        const std::uint32_t declared = out_report->struct_size;
        clay_multires_stamp_report probe;
        r = read_desc(out_report, kMultiresStampReportOriginal, &probe);
        if (r != CLAY_OK) return r;
        clay_multires_stamp_report out{};
        out.struct_size = static_cast<std::uint32_t>(sizeof(out));
        out.level = level;
        out.moved_vertices = applied;
        out.base_revision = s.base_revision();
        out.detail_revision = s.detail_revision();
        out.evaluated_revision = s.evaluated_revision();
        write_desc(out_report, declared, out);
    }
    return CLAY_OK;
}

size_t clay_multires_dirty_block_count(const clay_multires* surface) {
    if (!surface || !surface->surface.valid()) return 0;
    return surface->surface.dirty_patches().size();
}

clay_result clay_multires_dirty_blocks(const clay_multires* surface, uint32_t* out_patches,
                                       size_t* count) {
    const mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires_ro(surface, &s);
    if (r != CLAY_OK) return r;
    if (!count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null count");
    const std::vector<std::uint32_t>& dirty = s->dirty_patches();
    if (!out_patches) {
        *count = dirty.size();
        return CLAY_OK;
    }
    const std::size_t n = std::min(*count, dirty.size());
    for (std::size_t i = 0; i < n; ++i) out_patches[i] = dirty[i];
    *count = n;
    return CLAY_OK;
}

clay_result clay_multires_clear_dirty(clay_multires* surface) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    s->clear_dirty();
    return CLAY_OK;
}

namespace {

// The block, built into the handle's scratch. Shared by the info query and the
// copy so the two cannot disagree about what a block contains.
clay_result fill_block(clay_multires* surface, std::uint32_t patch, std::uint32_t level) {
    mesh::MultiresSurface* s = nullptr;
    clay_result r = resolve_multires(surface, &s);
    if (r != CLAY_OK) return r;
    if (level >= s->level_count())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such level: " + std::to_string(level));
    if (!s->build_block(level, patch, &surface->block))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "no such block: " + std::to_string(patch));
    return CLAY_OK;
}

void write_block_info(clay_multires_block_info* out, std::uint32_t declared,
                      const mesh::MultiresSurface::Block& block) {
    clay_multires_block_info info{};
    info.struct_size = static_cast<std::uint32_t>(sizeof(info));
    info.patch = block.patch;
    info.level = block.level;
    info.vertex_count = static_cast<std::uint32_t>(block.vertices.size());
    info.index_count = static_cast<std::uint32_t>(block.indices.size());
    write_desc(out, declared, info);
}

}  // namespace

clay_result clay_multires_block_info_get(clay_multires* surface, uint32_t patch, uint32_t level,
                                         clay_multires_block_info* out_info) {
    if (!out_info) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_info");
    const std::uint32_t declared = out_info->struct_size;
    clay_multires_block_info probe;
    clay_result r = read_desc(out_info, kMultiresBlockInfoOriginal, &probe);
    if (r != CLAY_OK) return r;
    r = fill_block(surface, patch, level);
    if (r != CLAY_OK) return r;
    write_block_info(out_info, declared, surface->block);
    return CLAY_OK;
}

clay_result clay_multires_copy_block(clay_multires* surface, uint32_t patch, uint32_t level,
                                     float* out_positions, size_t position_capacity,
                                     float* out_normals, size_t normal_capacity,
                                     uint32_t* out_indices, size_t index_capacity,
                                     clay_multires_block_info* out_written) {
    std::uint32_t declared = 0;
    if (out_written) {
        declared = out_written->struct_size;
        clay_multires_block_info probe;
        const clay_result d = read_desc(out_written, kMultiresBlockInfoOriginal, &probe);
        if (d != CLAY_OK) return d;
    }
    clay_result r = fill_block(surface, patch, level);
    if (r != CLAY_OK) return r;

    const mesh::MultiresSurface::Block& block = surface->block;
    // EVERY CAPACITY IS CHECKED FIRST and nothing is written past it. A partial
    // fill would leave a host drawing a block that is half this frame's and
    // half the last one's.
    if (out_positions && position_capacity < block.vertices.size() * 3u)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "position buffer too small for this block");
    if (out_normals && normal_capacity < block.vertices.size() * 3u)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "normal buffer too small for this block");
    if (out_indices && index_capacity < block.indices.size())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "index buffer too small for this block");

    const std::vector<kernel::cfloat3>& positions = surface->surface.positions_at(level);
    const std::vector<kernel::cfloat3>& normals = surface->surface.normals_at(level);
    for (std::size_t i = 0; i < block.vertices.size(); ++i) {
        const std::uint32_t v = block.vertices[i];
        if (out_positions) {
            out_positions[i * 3 + 0] = positions[v].x;
            out_positions[i * 3 + 1] = positions[v].y;
            out_positions[i * 3 + 2] = positions[v].z;
        }
        if (out_normals && v < normals.size()) {
            out_normals[i * 3 + 0] = normals[v].x;
            out_normals[i * 3 + 1] = normals[v].y;
            out_normals[i * 3 + 2] = normals[v].z;
        }
    }
    if (out_indices)
        for (std::size_t i = 0; i < block.indices.size(); ++i) out_indices[i] = block.indices[i];
    if (out_written) write_block_info(out_written, declared, block);
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_refit(clay_mesh_sculptor* sculptor) {
    // for_edit=false: refitting reads the mesh and writes only the tree, so it
    // is a READ of the layer in the sense the protection flags care about —
    // the same footing as raycast, and a ghosted layer stays queryable.
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/false);
    if (r != CLAY_OK) return r;
    sculptor->sculptor->refit_bvh();
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_quality(clay_mesh_sculptor* sculptor, float* out_quality) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/false);
    if (r != CLAY_OK) return r;
    if (!out_quality) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_quality");
    // Deliberately does NOT build a tree that does not exist. A quality reading
    // is a diagnostic, and a diagnostic that costs 1.3 s on a 2M-vertex mesh
    // because it silently built the thing it was asked to measure is a trap.
    // No tree means no queries to cost, which reads as zero.
    *out_quality = sculptor->sculptor->has_bvh() ? sculptor->sculptor->bvh().quality() : 0.0f;
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_refresh(clay_mesh_sculptor* sculptor) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/false);
    if (r != CLAY_OK) return r;
    sculptor->sculptor->refresh_bvh();
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_has_colors(const clay_mesh_sculptor* sculptor, int32_t* out_has) {
    if (!sculptor || !out_has)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null sculptor or out_has");
    *out_has = sculptor->sculptor->has_colors() ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_ensure_colors(clay_mesh_sculptor* sculptor, const float color[3],
                                             int32_t* out_created) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    if (!color) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null color");
    const bool created =
        sculptor->sculptor->ensure_colors(kernel::cf3(color[0], color[1], color[2]));
    if (out_created) *out_created = created ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mesh_sculptor_raycast(clay_mesh_sculptor* sculptor, const float origin[3],
                                       const float direction[3], const clay_mesh_frame* xform,
                                       clay_mesh_hit* out_hit) {
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/false);
    if (r != CLAY_OK) return r;
    if (!origin || !direction) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null ray");
    if (!out_hit) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_hit");
    clay_mesh_hit probe;
    r = read_desc(out_hit, kMeshHitOriginal, &probe);
    if (r != CLAY_OK) return r;
    math::Transform frame;
    r = read_mesh_frame(xform, &frame);
    if (r != CLAY_OK) return r;

    math::Ray ray;
    ray.origin = kernel::cf3(origin[0], origin[1], origin[2]);
    ray.dir = kernel::cf3(direction[0], direction[1], direction[2]);
    if (!(kernel::clength(ray.dir) > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "ray direction has no length");
    ray.dir = kernel::cnormalize(ray.dir);

    const mesh::Mesh& m = sculptor->sculptor->mesh();
    const pick::MeshHit hit = pick::raycast_mesh(m, sculptor->sculptor->bvh(), ray, frame);
    const std::uint32_t declared = out_hit->struct_size;
    clay_mesh_hit out{};
    out.hit = hit.hit ? 1 : 0;
    if (hit.hit) {
        out.t = hit.t;
        write_f3(out.position, hit.position);
        write_f3(out.normal, hit.normal);
        out.triangle = hit.triangle;
        out.u = hit.u;
        out.v = hit.v;
        out.seed_class = sculptor->sculptor->adjacency().class_of(m.indices[hit.triangle * 3]);
    } else {
        out.seed_class = CLAY_MESH_NO_CLASS;
    }
    write_desc(out_hit, declared, out);
    return CLAY_OK;
}

clay_mesh_deltas* clay_mesh_deltas_create(void) { return new clay_mesh_deltas(); }

void clay_mesh_deltas_destroy(clay_mesh_deltas* deltas) { delete deltas; }

clay_result clay_mesh_deltas_vertex_count(const clay_mesh_deltas* deltas, size_t* out_count) {
    if (!deltas) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null delta record");
    if (!out_count) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null out_count");
    *out_count = deltas->deltas.size();
    return CLAY_OK;
}

clay_result clay_mesh_deltas_revert(const clay_mesh_deltas* deltas, clay_mesh_sculptor* sculptor) {
    if (!deltas) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null delta record");
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    if (!deltas->deltas.revert(sculptor->sculptor->mesh()))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this record does not belong to this mesh");
    return CLAY_OK;
}

clay_result clay_mesh_deltas_apply(const clay_mesh_deltas* deltas, clay_mesh_sculptor* sculptor) {
    if (!deltas) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null delta record");
    clay_result r = resolve_sculptor(sculptor, /*for_edit=*/true);
    if (r != CLAY_OK) return r;
    if (!deltas->deltas.apply(sculptor->sculptor->mesh()))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "this record does not belong to this mesh");
    return CLAY_OK;
}

clay_result clay_mesh_deltas_clear(clay_mesh_deltas* deltas) {
    if (!deltas) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null delta record");
    deltas->deltas.clear();
    return CLAY_OK;
}

}  // extern "C"
