// C ABI implementation (c-abi spec): opaque handles over the C++ modules,
// thread-local error details, no exceptions cross this boundary (the core
// builds with -fno-exceptions on GCC/Clang).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "clay.h"
#include "clay/brush/gate_bake.h"
#include "clay/brush/lattice_gizmo.h"
#include "clay/brush/mask_extrude.h"
#include "clay/brush/move.h"
#include "clay/brush/procedural_mask.h"
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
#include "clay/version.h"
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
clay_result read_stroke(const float* samples_xyzpt, std::size_t sample_count,
                        const clay_stroke_preset* preset,
                        std::vector<brush::StrokeSample>* out_samples,
                        brush::StrokePreset* out_preset) {
    if (sample_count > 0 && !samples_xyzpt)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null stroke samples");
    clay_result r = check_batch("stroke samples", sample_count);
    if (r != CLAY_OK) return r;
    r = read_preset(preset, out_preset);
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
// No entry point removes a layer today, so the miss below cannot be reached
// through this ABI; it is what keeps that true if one is ever added.
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
    struct ResumeKey {
        std::int32_t x = 0, y = 0, z = 0;
        bool operator==(const ResumeKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct ResumeKeyHash {
        std::size_t operator()(const ResumeKey& k) const {
            std::size_t h = static_cast<std::size_t>(static_cast<std::uint32_t>(k.x));
            h = h * 0x9e3779b97f4a7c15ull + static_cast<std::uint32_t>(k.y);
            h = h * 0x9e3779b97f4a7c15ull + static_cast<std::uint32_t>(k.z);
            return h;
        }
    };
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
        float spacing = 0.0f;
        float band = 0.0f;
        std::int32_t dims[3] = {0, 0, 0};
        // The ACTIVE layer's chain at this brick's lattice -- what a suffix
        // continues. For a document with one visible SDF layer that IS the
        // whole field, which is why the single-layer case needs nothing else.
        std::vector<float> values;
        std::vector<float> colors;  // empty when that refill carried none
        // The visible SDF layers BEFORE the active one, hard-unioned. Empty
        // when there are none. Static across a stroke -- only the active layer
        // moves -- so it is stored once and carried forward untouched.
        std::vector<float> below;
        std::vector<float> below_colors;
    };

    // A BYTE budget, not a brick count. With colour a brick carries four times
    // the floats, so a count would mean two very different ceilings depending
    // on what the host asked for. 64 MB is 16,384 distance-only bricks of a
    // dim-8 cache, or 4,096 coloured ones -- a stroke's working set either way.
    static constexpr std::size_t kResumeBytes = 64u << 20;

    static std::size_t entry_bytes(const ResumeEntry& e) {
        return (e.values.size() + e.colors.size() + e.below.size() + e.below_colors.size()) *
               sizeof(float);
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
        p.active = active->id;
        p.has_below = visible > 1;
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

    // The one revision every stored seed shares, or 0 when they do not share
    // one. A batch is refilled together and stored together, so in the case
    // this exists for they always do; anything else takes the full path rather
    // than compiling a suffix per revision.
    std::uint64_t seed_revision_for(const clay_brick_request* requests, std::size_t count,
                                    std::size_t per, bool want_colour, bool want_below) const {
        std::uint64_t shared = 0;
        for (std::size_t i = 0; i < count; ++i) {
            auto it =
                resume_.find(ResumeKey{requests[i].key[0], requests[i].key[1], requests[i].key[2]});
            if (it == resume_.end()) return 0;
            const ResumeEntry& e = it->second;
            if (e.values.size() != per || e.spacing != requests[i].spacing) return 0;
            if (want_colour && e.colors.size() != per * 3) return 0;
            if (want_below != !e.below.empty()) return 0;
            if (want_below && want_colour && e.below_colors.size() != per * 3) return 0;
            if (e.dims[0] != requests[i].dims[0] || e.dims[1] != requests[i].dims[1] ||
                e.dims[2] != requests[i].dims[2])
                return 0;
            if (shared == 0)
                shared = e.revision;
            else if (shared != e.revision)
                return 0;
        }
        return shared;
    }

    // What a brick's stored seed holds, or `values == nullptr` when it cannot
    // serve this request.
    struct Seed {
        const float* values = nullptr;
        const float* colors = nullptr;
        const float* below = nullptr;  // null when no layer sits beneath
        const float* below_colors = nullptr;
    };

    Seed seed_for(const clay_brick_request& request, float pad, bool want_colour,
                  bool want_below) const {
        Seed s;
        auto it = resume_.find(ResumeKey{request.key[0], request.key[1], request.key[2]});
        if (it == resume_.end()) return s;
        // A colour asked for is a colour that has to have been kept: continuing
        // a coloured fold from a distance alone folds every combine against
        // black. The same for the layers beneath: a document that has them and
        // a seed that does not are describing different fields.
        if (want_colour && it->second.colors.empty()) return s;
        if (want_below != !it->second.below.empty()) return s;
        if (want_below && want_colour && it->second.below_colors.empty()) return s;
        // The cull pad decides which items a brick's compile keeps, so a seed
        // taken under a different one was continued from a different field.
        // The pad only grows on an append, so this is a real gate rather than
        // a formality.
        if (it->second.pad != pad) return s;
        if (!it->second.had_acc) return s;
        s.values = it->second.values.data();
        if (want_colour) s.colors = it->second.colors.data();
        if (want_below) {
            s.below = it->second.below.data();
            if (want_colour) s.below_colors = it->second.below_colors.data();
        }
        return s;
    }

    void store_seed(const clay_brick_request& request, std::uint64_t at, float pad,
                    const float* values, const float* colors, const float* below,
                    const float* below_colors, std::size_t per) const {
        const ResumeKey key{request.key[0], request.key[1], request.key[2]};
        auto [it, fresh] = resume_.try_emplace(key);
        ResumeEntry& e = it->second;
        resume_bytes_ -= entry_bytes(e);
        if (fresh) resume_order_.push_back(key);
        e.had_acc = false;
        for (std::size_t s = 0; s < per && !e.had_acc; ++s) e.had_acc = values[s] != CLAY_TAPE_FAR;
        e.revision = at;
        e.pad = pad;
        e.spacing = request.spacing;
        e.band = request.band;
        e.dims[0] = request.dims[0];
        e.dims[1] = request.dims[1];
        e.dims[2] = request.dims[2];
        e.values.assign(values, values + per);
        if (colors)
            e.colors.assign(colors, colors + per * 3);
        else
            e.colors.clear();
        if (below)
            e.below.assign(below, below + per);
        else
            e.below.clear();
        if (below_colors)
            e.below_colors.assign(below_colors, below_colors + per * 3);
        else
            e.below_colors.clear();
        resume_bytes_ += entry_bytes(e);
        // Oldest first, and never the brick just written -- a budget smaller
        // than one brick would otherwise evict what it just stored.
        while (resume_bytes_ > kResumeBytes && resume_order_.size() > 1) {
            const ResumeKey oldest = resume_order_.front();
            resume_order_.pop_front();
            if (oldest == key) {
                resume_order_.push_back(oldest);
                continue;
            }
            auto old = resume_.find(oldest);
            if (old == resume_.end()) continue;
            resume_bytes_ -= entry_bytes(old->second);
            resume_.erase(old);
        }
    }

    // The batch's results, kept as the next dab's seeds. `at` 0 means "the
    // current revision", which is what the full path passes -- it has just
    // produced the document as it is now.
    void store_seeds(const clay_brick_request* requests, std::size_t count, const float* values,
                     const float* colors, const float* below, const float* below_colors,
                     std::size_t per, std::uint64_t at, float pad) const {
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
                       below_colors ? below_colors + i * per * 3 : nullptr, per);
    }

    // The resumed path's store: the ACTIVE layer's value moved, the half
    // beneath it did not. Caller holds cache_mutex_.
    void store_active(const clay_brick_request& request, std::uint64_t at, float pad,
                      const float* values, const float* colors, std::size_t per) const {
        auto it = resume_.find(ResumeKey{request.key[0], request.key[1], request.key[2]});
        if (it == resume_.end()) return;
        ResumeEntry& e = it->second;
        resume_bytes_ -= entry_bytes(e);
        e.had_acc = false;
        for (std::size_t s = 0; s < per && !e.had_acc; ++s) e.had_acc = values[s] != CLAY_TAPE_FAR;
        e.revision = at;
        e.pad = pad;
        e.values.assign(values, values + per);
        if (colors)
            e.colors.assign(colors, colors + per * 3);
        else
            e.colors.clear();
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
    void touch_region(const math::Aabb& changed) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        forget_appends();  // not an append; no prefix may be reused
        const std::uint64_t next = revision.fetch_add(1, std::memory_order_relaxed) + 1;
        if (changed.is_infinite()) {
            forget_resume();
            return;
        }
        for (auto it = resume_.begin(); it != resume_.end();) {
            const ResumeEntry& e = it->second;
            const float width = static_cast<float>(e.dims[0]) * e.spacing;
            const kernel::cfloat3 lo =
                kernel::cf3(static_cast<float>(it->first.x), static_cast<float>(it->first.y),
                            static_cast<float>(it->first.z)) *
                width;
            const math::Aabb cull =
                math::Aabb{lo, lo + kernel::cf3(width, width, width)}.dilated(e.band + e.pad);
            if (!changed.empty() && changed.intersects(cull)) {
                resume_bytes_ -= entry_bytes(e);
                it = resume_.erase(it);
                continue;
            }
            // Untouched: the same value, now current.
            it->second.revision = next;
            ++it;
        }
    }

    std::uint64_t current_revision() const { return revision.load(std::memory_order_relaxed); }

    std::mutex& cache_lock() const { return cache_mutex_; }

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
        //
        // A COPY, because the cached index is shared and const: another thread
        // may be holding the old one against a plan it already made. Copying
        // the entries is the memcpy the rebuild would have done anyway, and it
        // is what the 0.13 ms above is mostly made of.
        const std::vector<scene::NodeId> since =
            index_cache_ ? appends_since(index_revision_, now) : std::vector<scene::NodeId>{};
        if (!since.empty()) {
            auto grown = std::make_shared<scene::CullIndex>(*index_cache_);
            if (grown->append(since)) {
                index_cache_ = std::move(grown);
                index_revision_ = now;
                return index_cache_;
            }
        }
        index_cache_ = std::make_shared<const scene::CullIndex>(doc.document);
        index_revision_ = now;
        return index_cache_;
    }

  private:
    // Caller holds cache_mutex_.
    void forget_appends() const {
        append_valid_ = false;
        append_log_.clear();
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
    mutable std::shared_ptr<const scene::Tape> pick_cache_;
    mutable std::uint64_t pick_revision_ = 0;
    mutable std::shared_ptr<const scene::CullIndex> index_cache_;
    mutable std::uint64_t index_revision_ = 0;
    mutable std::unordered_map<ResumeKey, ResumeEntry, ResumeKeyHash> resume_;
    mutable std::deque<ResumeKey> resume_order_;  // insertion order, for eviction
    mutable std::size_t resume_bytes_ = 0;
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
    if (it == grid->doc->doc.voxel_layers.end())  // no removal call exists yet
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

TailAppend tail_append(const scene::Document& doc, const scene::Command& cmd) {
    const auto* add = std::get_if<scene::AddNodeCmd>(&cmd);
    if (!add || add->parent != scene::kNoNode || add->index != -1) return {};
    const scene::Layer* l = doc.find_layer(add->layer);
    if (!l || l->kind != scene::LayerKind::Sdf || !l->sdf || l->sdf->roots.empty()) return {};
    // roots.back(), not the command's own subtree: this cannot then disagree
    // with what apply() actually did to the list.
    return TailAppend{add->layer, l->sdf->roots.back()};
}

// Every edit below routes through the command vocabulary rather than touching
// the document, so a C edit means what a saved document means — and becomes
// undoable for free once the undo stack is exposed. apply() reports failure by
// returning nullopt and leaves the document untouched.
clay_result apply_edit(clay_document* doc, const scene::Command& cmd, const char* what) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    // With a stack attached the edit is applied AND its inverse recorded, so
    // no reachable edit can escape undo.
    // apply() says no for two different reasons, and a caller needs to tell
    // them apart: a missing layer is a bug in the caller's bookkeeping, while
    // a protected one is a state the artist chose and a UI can explain.
    scene::LayerId target = scene::edited_layer(cmd);
    if (target != 0) {
        const scene::Layer* l = doc->doc.document.find_layer(target);
        if (l && l->protected_from_edits())
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        std::string("layer ") + std::to_string(target) + " is " +
                            (l->ghost ? "ghosted" : "locked") + " and takes no edits");
    }
    // What this edit can reach, taken on BOTH sides of the apply and unioned.
    // One side is not an answer: an add's node is not there before, a removal's
    // is not there after, and a move has two ends -- the contract
    // command_influence_bound states and the undo stack already follows.
    // Gathered before the apply because after it the old shape is gone.
    const math::Aabb reach_before = scene::command_influence_bound(doc->doc.document, cmd);
    bool ok = doc->undo ? doc->undo->perform(doc->doc.document, cmd)
                        : static_cast<bool>(scene::apply(doc->doc.document, cmd));
    if (!ok) return fail(CLAY_ERROR_NOT_FOUND, what);
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
    // brick the whole edit list.
    doc->touch_region(reach);
    return CLAY_OK;
}

clay_result read_transform(const float position[3], const float rotation_axis[3],
                           float rotation_angle, float scale, math::Transform* out) {
    if (!position || !rotation_axis) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null transform");
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

template <typename Run>
clay_result eval_requests_in_chunks(const clay_document* doc, const clay_brick_request* requests,
                                    std::size_t count, Run&& run, ChunkHalf half = ChunkHalf::Whole,
                                    scene::LayerId active = 0) {
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
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    for (std::size_t base = 0; base < count;) {
        std::size_t n = 1;
        while (n < kChunk && base + n < count &&
               requests[base + n].spacing == requests[base].spacing)
            ++n;
        tapes.clear();
        tape_ptrs.clear();
        tapes.reserve(n);
        tape_ptrs.reserve(n);
        for (std::size_t i = base; i < base + n; ++i) {
            scene::CullRegion cull{request_brick_box(requests[i]).dilated(requests[i].band)};
            tapes.push_back(
                half == ChunkHalf::Whole
                    ? scene::compile_document(doc->doc.document, &cull, index.get(), &plan)
                    : scene::compile_document_part(doc->doc.document, active,
                                                   half == ChunkHalf::Below, &cull, index.get()));
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
    // already unioned over the commands it replayed.
    if (*out_undone) doc->touch_region(bound);
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
    if (*out_redone) doc->touch_region(bound);
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
    return apply_edit(doc, scene::Command{scene::SetTransformCmd{layer, node, xform}},
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

clay_result clay_document_remove_layer(clay_document* doc, clay_layer_id layer) {
    return apply_edit(doc, scene::Command{scene::RemoveLayerCmd{layer}}, "layer not found");
}

clay_result clay_document_move_layer(clay_document* doc, clay_layer_id layer, int32_t index) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* found = doc->doc.document.find_layer(layer);
    if (!found) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    scene::Layer copy = *found;
    // One group, so one undo puts the layer back where it was. Ungrouped, the
    // undo stack held a remove and an insert separately and a single undo
    // applied only the remove — the layer vanished.
    if (doc->undo) doc->undo->begin_group();
    clay_result r = apply_edit(doc, scene::Command{scene::RemoveLayerCmd{layer}},
                               "layer not found");
    if (r == CLAY_OK)
        r = apply_edit(doc, scene::Command{scene::AddLayerCmd{std::move(copy), index}},
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
    return apply_edit(doc, scene::Command{scene::SetLayerTransformCmd{layer, xform}},
                      "layer not found");
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
clay_result resolve_move(const clay_document* doc, clay_layer_id layer, const float centre[3],
                         const float displacement[3], const clay_move_params* params,
                         const scene::Layer** out_layer,
                         std::vector<brush::MoveWarp>* out_warps) {
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
    *out_warps = brush::move_brush(*l, kernel::cf3(centre[0], centre[1], centre[2]),
                                   kernel::cf3(displacement[0], displacement[1],
                                               displacement[2]),
                                   settings);
    return CLAY_OK;
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
    std::vector<brush::MoveWarp> warps;
    clay_result r = resolve_move(doc, layer, centre, displacement, params, &l, &warps);
    if (r != CLAY_OK) return r;
    *out_count = warps.size();
    if (!out_nodes) return CLAY_OK;  // size query
    for (std::size_t i = 0; i < warps.size() && i < capacity; ++i)
        out_nodes[i] = warps[i].node;
    return CLAY_OK;
}

clay_result clay_layer_move_surface(clay_document* doc, clay_layer_id layer,
                                    const float centre[3], const float displacement[3],
                                    const clay_move_params* params, size_t* out_applied) {
    const scene::Layer* l = nullptr;
    std::vector<brush::MoveWarp> warps;
    clay_result r = resolve_move(doc, layer, centre, displacement, params, &l, &warps);
    if (r != CLAY_OK) return r;

    // One group for the whole drag: it is one gesture, and undoing it item by
    // item would be the implementation showing through.
    if (doc->undo) doc->undo->begin_group();
    std::size_t applied = 0;
    for (const brush::MoveWarp& w : warps) {
        const scene::Node* n = l->sdf->find(w.node);
        if (!n) continue;
        scene::SetDeformersCmd cmd{layer, w.node, brush::moved_chain(*n, w)};
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
    return write_bounds(pick::layer_bounds(*layer), out_min, out_max, out_has_bounds);
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
    scene::Layer& layer = doc->doc.document.add_sdf_layer(name);
    doc->touch();
    layer.kind = scene::LayerKind::Voxel;
    layer.sdf.reset();  // a voxel layer carries no SDF content
    doc->doc.voxel_layers.emplace(layer.id, voxel::VoxelGrid(voxel_size));
    if (out_layer) *out_layer = layer.id;
    if (out_grid) *out_grid = borrow_layer(doc, layer.id);
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
        for (kernel::cfloat3& v : m.positions) v = layer.xform.apply(v);
        for (kernel::cfloat3& n : m.normals) n = layer.xform.rotation.rotate(n);
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
    return write_influence(scene::node_influence_bound(*layer->sdf, node, *layer), out_min,
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

    // The same batch pipeline the host-memory form runs — validation up
    // front, one cull index and coarse plan, per-brick culled tapes in
    // chunks — with the whole chunk reaching the adopted backend as ONE
    // batched device evaluation (issue #64 applied to the zero-copy path:
    // the per-brick loop paid a command buffer and a wait per 8^3 lattice,
    // which left this path 25-165x behind the host-memory one). Each chunk
    // lands at its requests' fixed slots in the caller's single allocation,
    // so brick i still occupies out_values[i * dim^3 ...] exactly as
    // documented, and the values are identical to the host-memory form's.
    return eval_requests_in_chunks(
        doc, requests, count,
        [&](const eval::GridBatchQuery& bq, std::size_t base) -> clay_result {
            eval::DeviceBuffer slot = values;
            slot.offset = values.offset + static_cast<std::uint64_t>(base) * per * sizeof(float);
            slot.size = static_cast<std::uint64_t>(bq.count) * per * sizeof(float);
            eval::DeviceBuffer color_slot;
            if (!colors.empty()) {
                color_slot = colors;
                color_slot.offset =
                    colors.offset + static_cast<std::uint64_t>(base) * per * 3 * sizeof(float);
                color_slot.size = static_cast<std::uint64_t>(bq.count) * per * 3 * sizeof(float);
            }
            switch (device->backend->eval_grid_batch_device(bq, slot, color_slot)) {
                case eval::Status::Ok: return CLAY_OK;
                case eval::Status::Unsupported:
                    return fail(CLAY_ERROR_UNSUPPORTED,
                                "this backend does not evaluate into a caller's device buffer");
                case eval::Status::InvalidInput:
                    return fail(CLAY_ERROR_INVALID_ARGUMENT,
                                "a brick's device slot is invalid");
                default: return fail(CLAY_ERROR_BACKEND, "device evaluation failed");
            }
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
            math::Aabb b = scene::node_influence_bound(*layer->sdf, nodes[i], *layer);
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
    // -- the resumable path (#306) -----------------------------------------
    //
    // When every brick asked for carries a seed from the same revision, and
    // the document has only been APPENDED to since, what this call has to
    // evaluate is the appended items -- not the whole surviving edit list over
    // every sample. The suffix is compiled per brick and culled exactly as a
    // whole-document compile would cull it, which is what makes continuing
    // from the seed the same arithmetic rather than an approximation of it.
    //
    // Distances only. A seed is one float a sample; a colour is not, so a
    // caller asking for colour takes the full path.
    // -- the resumable path (#306) -----------------------------------------
    //
    // When every brick asked for carries a seed from the same revision, and the
    // document has only been APPENDED to since, what this call has to evaluate
    // is the appended items -- not the whole surviving edit list over every
    // sample. The suffix is compiled per brick and culled exactly as a whole-
    // document compile would cull it, which is what makes continuing from the
    // seed the same arithmetic rather than an approximation of it.
    //
    // WITH MORE THAN ONE VISIBLE SDF LAYER the seed is two values, not one. The
    // layers hard-union left to right, so the tape holds the layers BENEATH the
    // active one as its own accumulator, and a single stored number cannot be
    // taken apart into the two again. They are kept apart instead: the suffix
    // folds into the active layer's value and the union is applied here, with
    // the same hard Add the whole-document compile emits between layers. The
    // layers beneath are static across a stroke, so their half is stored once
    // and carried forward untouched.
    const bool want_colour = out_colors_rgb != nullptr;
    std::vector<std::uint8_t> resumed(count, 0);
    std::size_t resumed_count = 0;
    float resume_pad = 0.0f;
    {
        std::lock_guard<std::mutex> lock(doc->cache_lock());
        const clay_document::ResumePlan probe = doc->plan_resume(1);  // for has_below only
        const std::uint64_t seed_rev =
            doc->seed_revision_for(requests, count, per, want_colour, probe.has_below);
        // A seed already AT the current revision is the answer: its brick's
        // culled tape has not changed since it was computed, so there is
        // nothing to fold into it. That is what a region-limited invalidation
        // leaves behind -- an edit the brick cannot reach advances the seed
        // rather than dropping it -- and it is also the plain case of a refill
        // asked for twice without an edit in between.
        //
        // The union still applies: what is stored is the ACTIVE layer's value,
        // and the layers beneath are their own half.
        if (seed_rev != 0 && seed_rev == doc->current_revision()) {
            const float pad = doc->cull_index_locked()->cull_pad();
            for (std::size_t i = 0; i < count; ++i) {
                const clay_document::Seed seed =
                    doc->seed_for(requests[i], pad, want_colour, probe.has_below);
                if (!seed.values) continue;
                float* vd = out_values + i * per;
                float* vc = want_colour ? out_colors_rgb + i * per * 3 : nullptr;
                if (probe.has_below) {
                    fold_layers_below(seed.below, seed.below_colors, seed.values, seed.colors, per,
                                      vd, vc);
                } else {
                    std::memcpy(vd, seed.values, per * sizeof(float));
                    if (vc) std::memcpy(vc, seed.colors, per * 3 * sizeof(float));
                }
                resumed[i] = 1;
                ++resumed_count;
            }
        }
        const clay_document::ResumePlan plan = doc->plan_resume(seed_rev);
        if (plan.usable) {
            std::shared_ptr<const scene::CullIndex> index = doc->cull_index_locked();
            resume_pad = index->cull_pad();
            std::vector<float> points;
            std::vector<float> active(per), active_rgb(want_colour ? per * 3 : 0);
            for (std::size_t i = 0; i < count; ++i) {
                const clay_document::Seed seed =
                    doc->seed_for(requests[i], resume_pad, want_colour, plan.has_below);
                if (!seed.values) continue;
                eval::GridQuery g;
                std::size_t samples = 0;
                if (read_grid(requests[i].origin, requests[i].spacing, requests[i].dims, &g,
                              &samples) != CLAY_OK)
                    continue;
                const math::Aabb box = request_brick_box(requests[i]).dilated(requests[i].band);
                scene::CullRegion cull{box};
                scene::Tape suffix;
                if (!scene::compile_layer_suffix(plan.checkpoint, doc->doc.document, plan.appended,
                                                 &suffix, nullptr, &cull, index.get()))
                    continue;
                points.resize(samples * 3);
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
                pq.count = samples;
                eval::PointResults pr;
                // Into scratch when there is a union still to apply, straight
                // out when there is not.
                pr.distances = plan.has_below ? active.data() : out_values + i * per;
                if (want_colour)
                    pr.colors_rgb =
                        plan.has_below ? active_rgb.data() : out_colors_rgb + i * per * 3;
                eval::eval_points_seeded(suffix, pq, seed.values, seed.colors, pr);
                if (plan.has_below) {
                    fold_layers_below(seed.below, seed.below_colors, active.data(),
                                      want_colour ? active_rgb.data() : nullptr, per,
                                      out_values + i * per,
                                      want_colour ? out_colors_rgb + i * per * 3 : nullptr);
                    // The seed for the NEXT dab is the ACTIVE layer's value,
                    // not what was just written out.
                    doc->store_active(requests[i], plan.now, resume_pad, active.data(),
                                      want_colour ? active_rgb.data() : nullptr, per);
                } else {
                    doc->store_active(requests[i], plan.now, resume_pad, out_values + i * per,
                                      want_colour ? out_colors_rgb + i * per * 3 : nullptr, per);
                }
                resumed[i] = 1;
                ++resumed_count;
            }
        }
    }
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
        has_below ? ChunkHalf::Active : ChunkHalf::Whole, active_layer);
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
                     per, 0, 0.0f);
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
    std::unique_ptr<mesh::MeshSculptor> sculptor;
};

struct clay_mesh_deltas {
    mesh::VertexDeltas deltas;
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
    // The alpha stays null in the defaults: a stamp without one is the common
    // case, and a default pointing at nothing a caller owns would be a trap.
    write_desc(out_desc, declared, out);
    return CLAY_OK;
}

namespace {
constexpr std::size_t kTransferDescOriginal =
    offsetof(clay_transfer_desc, max_distance) + sizeof(float);
constexpr std::size_t kTransferReportOriginal =
    offsetof(clay_transfer_report, max_distance) + sizeof(float);
}  // namespace

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
