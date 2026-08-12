// C ABI implementation (c-abi spec): opaque handles over the C++ modules,
// thread-local error details, no exceptions cross this boundary (the core
// builds with -fno-exceptions on GCC/Clang).

#include "clay.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstddef>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "desc_version.h"

#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include <memory>

#include "clay/io/mesh_io.h"
#include "clay/field/flatten.h"
#include "clay/field/move_topological.h"
#include "clay/field/relax.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/validate.h"
#include "clay/pick/pick.h"
#include "clay/scene/armature.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/tape.h"
#include "clay/io/parity_fixture.h"
#include "clay/version.h"
#include "clay/voxel/grid.h"
#include "clay/brush/mask_extrude.h"
#include "clay/brush/move.h"
#include "clay/brush/stroke.h"
#include "clay/brush/tube.h"
#include "clay/cut/cut.h"
#include "clay/voxel/mask.h"

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

constexpr int kDeformParams[] = {1, 1, 4, 2, 2, 3, 9, 3, 3, 8, 8, 10, 5, 5};
static_assert(sizeof kDeformParams / sizeof kDeformParams[0] == kernel::cdeform_noise + 1);

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
};

struct clay_mask {
    voxel::MaskField* owned = nullptr;  // non-null: the caller destroys it
    clay_document* doc = nullptr;       // non-null: borrowed from a layer
    clay_layer_id layer = 0;
};

// The same discriminator, one shape different: a standalone mesh is held by
// value because every producer in this file builds one in place, and `data` is
// simply unused by a borrow. Declared above clay_document because the document
// keeps a map of these by value.
struct clay_mesh {
    mesh::Mesh data;               // the owned mesh; empty on a borrow
    clay_document* doc = nullptr;  // non-null: borrowed from a mesh layer
    clay_layer_id layer = 0;
};

struct clay_document {
    io::ClaySpaceDoc doc;
    // Opt-in undo. Null means off, and a document that never enables it
    // behaves exactly as it did before the feature existed.
    std::unique_ptr<scene::UndoStack> undo;
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

    void touch() { revision.fetch_add(1, std::memory_order_relaxed); }

    std::shared_ptr<const scene::Tape> tape() const {
        return cached(tape_cache_, tape_revision_,
                      [this] { return scene::compile_document(doc.document); });
    }

    // Picking excludes ghosted layers, so it is a different tape and gets its
    // own slot rather than sharing one that would thrash between the two.
    std::shared_ptr<const scene::Tape> pickable_tape() const {
        return cached(pick_cache_, pick_revision_,
                      [this] { return pick::pickable_tape(doc.document); });
    }

  private:
    template <typename Build>
    std::shared_ptr<const scene::Tape> cached(std::shared_ptr<const scene::Tape>& slot,
                                              std::uint64_t& slot_revision, Build build) const {
        const std::uint64_t now = revision.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!slot || slot_revision != now) {
            slot = std::make_shared<const scene::Tape>(build());
            slot_revision = now;
        }
        return slot;
    }

    mutable std::mutex cache_mutex_;
    mutable std::shared_ptr<const scene::Tape> tape_cache_;
    mutable std::uint64_t tape_revision_ = 0;
    mutable std::shared_ptr<const scene::Tape> pick_cache_;
    mutable std::uint64_t pick_revision_ = 0;
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
    bool ok = doc->undo ? doc->undo->perform(doc->doc.document, cmd)
                        : static_cast<bool>(scene::apply(doc->doc.document, cmd));
    if (!ok) return fail(CLAY_ERROR_NOT_FOUND, what);
    // The funnel every command-based edit passes through, so the tape cache is
    // invalidated in one place for all of them.
    doc->touch();
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

clay_result clay_document_save(const clay_document* doc, const char* path) {
    if (!doc || !path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or path");
    return from_io(io::save_clayspace_file(doc->doc, path));
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
    if (!doc->undo) doc->undo = std::make_unique<scene::UndoStack>();
    return CLAY_OK;
}

clay_result clay_document_undo(clay_document* doc, int32_t* out_undone) {
    if (!doc || !out_undone) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    // An empty stack is reported, not failed: a UI drives this without having
    // to track whether anything is left.
    *out_undone = doc->undo->undo(doc->doc.document) ? 1 : 0;
    // Undo and redo replay commands straight onto the document rather than
    // through apply_edit, so they invalidate here.
    if (*out_undone) doc->touch();
    return CLAY_OK;
}

clay_result clay_document_redo(clay_document* doc, int32_t* out_redone) {
    if (!doc || !out_redone) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    *out_redone = doc->undo->redo(doc->doc.document) ? 1 : 0;
    if (*out_redone) doc->touch();
    return CLAY_OK;
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
        *out_info = clay_layer_info{};
        out_info->struct_size = declared;
        out_info->id = l.id;
        out_info->representation = static_cast<std::int32_t>(l.kind);
        out_info->stack_index = static_cast<std::int32_t>(i);
        out_info->visible = l.visible ? 1 : 0;
        out_info->ghost = l.ghost ? 1 : 0;
        out_info->locked = l.locked ? 1 : 0;
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
    // NOISE, not POSE_LINE. The bound was not moved when magnify and noise were
    // added, so both were declared, documented, given parameter counts, handled
    // by make_deformer — and refused here. The binding parity gate cannot see
    // it: it checks that the ENUMERATOR exists, not that a call accepts it.
    if (deform < 0 || deform > CLAY_DEFORM_NOISE)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown deformer kind");
    clay_result r = check_params("deformer", params, param_count, kDeformParams[deform]);
    if (r != CLAY_OK) return r;
    if ((r = check_ease(ease)) != CLAY_OK) return r;
    scene::Deformer d;
    if ((r = make_deformer(deform, params, &d)) != CLAY_OK) return r;
    d.ease = static_cast<std::uint8_t>(ease);
    item->node.deformers.push_back(d);  // chain order is call order
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
        case CLAY_ARMATURE_DELETE: ok = scene::armature_delete_subtree(nodes, parents, target); break;
        default: return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown armature edit");
    }
    if (!ok) return fail(CLAY_ERROR_INVALID_ARGUMENT, "that armature node does not exist");
    return apply_edit(doc,
                      scene::Command{scene::SetArmatureCmd{layer, node, std::move(nodes),
                                                           std::move(parents),
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

clay_result clay_layer_add_deformer(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t deform, const float* params, size_t param_count,
                                    int32_t ease, int32_t at_front) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (deform < 0 || deform > CLAY_DEFORM_NOISE)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown deformer kind");
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
    const std::uint32_t declared = out->struct_size;
    *out = clay_consolidation_cost{};
    out->struct_size = declared;
    return CLAY_OK;
}

void write_cost(const scene::ConsolidationCost& src, clay_consolidation_cost* out) {
    out->cell_size = src.cell_size;
    out->band = src.band;
    out->brick_count = static_cast<std::uint64_t>(src.brick_count);
    out->sample_count = static_cast<std::uint64_t>(src.sample_count);
    out->bytes = static_cast<std::uint64_t>(src.bytes);
    out->sample_lipschitz = src.sample_lipschitz;
    out->lipschitz = src.lipschitz;
    out->safe_step_scale = src.safe_step_scale;
    const math::Aabb b = src.bounds.empty() ? math::Aabb{kernel::cf3(0, 0, 0), kernel::cf3(0, 0, 0)}
                                            : src.bounds;
    for (int a = 0; a < 3; ++a) {
        out->bounds_min[a] = (&b.min.x)[a];
        out->bounds_max[a] = (&b.max.x)[a];
    }
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
    *out_report = clay_field_report{};
    out_report->struct_size = declared;
    out_report->lipschitz = report.lipschitz;
    out_report->safe_step_scale = report.safe_step_scale;
    out_report->steepest_volume = report.steepest_volume;
    out_report->longest_deformer_chain = report.longest_deformer_chain;
    out_report->item_count = report.item_count;
    out_report->advises_consolidation = report.advises_consolidation ? 1 : 0;
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
    if (!scene::bake_layer(*layer, p, &cost))
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
    if (!scene::consolidate_layer(doc->doc.document, layer_id, p, doc->undo.get(), &cost))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "nothing to consolidate: the layer is empty, unbounded, or the region "
                    "contains no surface");
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
    eval::RayQuery q{ray, 1, 0.0f, 1e6f, 1e-4f, 256};
    eval::RayHit hit;
    if (b->raycast(tape, q, &hit) != eval::Status::Ok)
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
    pick::SceneHit hit = pick::raycast_scene(doc->doc.document, ray);
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

clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                               int32_t* out_manifold) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    mesh::ValidationReport report = mesh::validate(*m);
    if (out_watertight) *out_watertight = report.watertight ? 1 : 0;
    if (out_manifold) *out_manifold = report.manifold ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mesh_save(const clay_mesh* mesh, const char* path) {
    const mesh::Mesh* m = nullptr;
    clay_result r = resolve_mesh(mesh, &m);
    if (r != CLAY_OK) return r;
    if (!path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path");
    std::string p(path);
    std::size_t dot = p.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot + 1);
    if (ext == "obj") return from_io(io::save_obj_file(*m, p));
    if (ext == "ply") return from_io(io::save_ply_file(*m, p));
    if (ext == "fbx") return from_io(io::save_fbx_file(*m, p));
    if (ext == "glb") return from_io(io::save_glb_file(*m, p));
    return fail(CLAY_ERROR_UNSUPPORTED, "unknown extension: " + ext);
}

clay_result clay_mesh_load(const char* path, const clay_import_budget* budget,
                           clay_mesh** out_mesh) {
    if (!path || !out_mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path or out_mesh");
    *out_mesh = nullptr;

    io::ImportBudget limits;
    if (budget) {
        clay_import_budget b;
        clay_result r = read_desc(budget, kImportBudgetOriginal, &b);
        if (r != CLAY_OK) return r;
        // Zero means "the library's default" rather than "allow nothing",
        // which is what a zeroed struct would otherwise say.
        if (b.max_vertices) limits.max_vertices = static_cast<std::size_t>(b.max_vertices);
        if (b.max_triangles) limits.max_triangles = static_cast<std::size_t>(b.max_triangles);
    }

    std::string p(path);
    std::size_t dot = p.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot + 1);
    // Case-insensitive: a file called MODEL.OBJ is an OBJ file, and the Python
    // loader has always accepted one. The C ABI refusing it was a plain bug.
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto loaded = std::make_unique<clay_mesh>();
    io::IoStatus status;
    if (ext == "obj") {
        status = io::load_obj_file(p, &loaded->data, limits);
    } else if (ext == "ply") {
        status = io::load_ply_file(p, &loaded->data, limits);
    } else if (ext == "fbx") {
        status = io::load_fbx_file(p, &loaded->data, limits);
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

    field::FieldVolume volume = field::FieldVolume::sample(
        [&tape](kernel::cfloat3 q) { return tape.eval(q).d; }, region, cell, band);
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
    field::FieldVolume relaxed = field::relax(
        [&tape](kernel::cfloat3 q) { return tape.eval(q).d; }, region, cell, band, settings);
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
    field::FieldVolume flattened = field::flatten(
        [&tape](kernel::cfloat3 q) { return tape.eval(q).d; }, region, cell, band, settings);
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
    std::size_t vertices = 0, indices = 0;
    for (const mesh::Mesh* m : parts) {
        keep_normals = keep_normals && m->normals.size() == m->positions.size();
        keep_colors = keep_colors && m->colors.size() == m->positions.size();
        keep_uvs = keep_uvs && m->uvs.size() == m->positions.size();
        vertices += m->positions.size();
        indices += m->indices.size();
    }
    out.positions.reserve(vertices);
    out.indices.reserve(indices);
    if (keep_normals) out.normals.reserve(vertices);
    if (keep_colors) out.colors.reserve(vertices);
    if (keep_uvs) out.uvs.reserve(vertices);

    std::uint32_t base = 0;
    for (const mesh::Mesh* m : parts) {
        out.positions.insert(out.positions.end(), m->positions.begin(), m->positions.end());
        if (keep_normals) out.normals.insert(out.normals.end(), m->normals.begin(), m->normals.end());
        if (keep_colors) out.colors.insert(out.colors.end(), m->colors.begin(), m->colors.end());
        if (keep_uvs) out.uvs.insert(out.uvs.end(), m->uvs.begin(), m->uvs.end());
        for (std::uint32_t i : m->indices) out.indices.push_back(i + base);
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

clay_result clay_stroke_preset_defaults(clay_stroke_preset* out_preset) {
    if (!out_preset) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null preset");
    brush::StrokePreset d;
    *out_preset = clay_stroke_preset{};
    out_preset->struct_size = static_cast<std::uint32_t>(sizeof(clay_stroke_preset));
    out_preset->radius = d.radius;
    out_preset->spacing = d.spacing;
    out_preset->strength = d.strength;
    out_preset->pressure_size = d.pressure.size;
    out_preset->pressure_strength = d.pressure.strength;
    out_preset->pressure_curve = d.pressure.curve;
    out_preset->jitter_position = d.jitter_position;
    out_preset->jitter_size = d.jitter_size;
    out_preset->jitter_rotation = d.jitter_rotation;
    out_preset->seed = d.seed;
    out_preset->rotate_along_stroke = d.rotate_along_stroke ? 1 : 0;
    out_preset->taper_start = d.taper_start;
    out_preset->taper_end = d.taper_end;
    out_preset->steady = d.steady;
    out_preset->accumulation = static_cast<std::int32_t>(d.accumulation);
    return CLAY_OK;
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
    clay_result r = clay_stroke_preset_defaults(out_preset);
    if (r != CLAY_OK) return r;
    out_preset->radius = p->radius;
    out_preset->spacing = p->spacing;
    out_preset->strength = p->strength;
    out_preset->pressure_size = p->pressure.size;
    out_preset->pressure_strength = p->pressure.strength;
    out_preset->pressure_curve = p->pressure.curve;
    out_preset->jitter_position = p->jitter_position;
    out_preset->jitter_size = p->jitter_size;
    out_preset->jitter_rotation = p->jitter_rotation;
    out_preset->seed = p->seed;
    out_preset->rotate_along_stroke = p->rotate_along_stroke ? 1 : 0;
    out_preset->taper_start = p->taper_start;
    out_preset->taper_end = p->taper_end;
    out_preset->steady = p->steady;
    out_preset->accumulation = static_cast<std::int32_t>(p->accumulation);
    return CLAY_OK;
}

clay_result clay_stroke_resolve(const float* samples_xyzpt, size_t sample_count,
                                const clay_stroke_preset* preset, clay_stamp* out_stamps,
                                size_t* count) {
    std::vector<brush::StrokeSample> samples;
    brush::StrokePreset p;
    clay_result r = read_stroke(samples_xyzpt, sample_count, preset, &samples, &p);
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
    m->invert();
    return CLAY_OK;
}

clay_result clay_mask_clear(clay_mask* mask) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
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
    if (steps <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "steps must be > 0");
    m->expand(steps);
    return CLAY_OK;
}

clay_result clay_mask_contract(clay_mask* mask, int32_t steps) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
    if (steps <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "steps must be > 0");
    m->contract(steps);
    return CLAY_OK;
}

clay_result clay_mask_smooth(clay_mask* mask, int32_t iterations) {
    voxel::MaskField* m = nullptr;
    clay_result r = resolve_mask(mask, &m);
    if (r != CLAY_OK) return r;
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

clay_result clay_document_mask_extrude(clay_document* doc, clay_layer_id layer,
                                       const clay_mask* mask,
                                       const clay_mask_extrude_params* params,
                                       clay_item** out_item) {
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
        [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, *m, settings);
    if (!volume) return no_extract();

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
    g->set(to_coord(cell), slot);
    return CLAY_OK;
}

clay_result clay_voxel_erase(clay_voxel_grid* grid, const int32_t cell[3]) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_at(grid, cell, &g);
    if (r != CLAY_OK) return r;
    g->erase(to_coord(cell));
    return CLAY_OK;
}

clay_result clay_voxel_paint(clay_voxel_grid* grid, const int32_t cell[3], int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, cell, index, &g, &slot);
    if (r != CLAY_OK) return r;
    g->paint(to_coord(cell), slot);
    return CLAY_OK;
}

clay_result clay_voxel_set_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count,
                                int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_batch(grid, cells_xyz, count, &g);
    if (r != CLAY_OK) return r;
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    for (size_t i = 0; i < count; ++i) g->set(to_coord(cells_xyz + i * 3), slot);
    return CLAY_OK;
}

clay_result clay_voxel_erase_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_batch(grid, cells_xyz, count, &g);
    if (r != CLAY_OK) return r;
    for (size_t i = 0; i < count; ++i) g->erase(to_coord(cells_xyz + i * 3));
    return CLAY_OK;
}

clay_result clay_voxel_fill_box(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, a, index, &g, &slot);
    if (r != CLAY_OK) return r;
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
    r = check_palette_index(index, &slot);
    if (r != CLAY_OK) return r;
    g->paint_brush(to_coord(cell), p, slot);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_smooth(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    g->sculpt_smooth(to_coord(cell), p);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_inflate(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, int32_t amount) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
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
    g->sculpt_pinch(to_coord(cell), p);
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_magnify(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
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
    std::uint32_t declared = out_report->struct_size;
    *out_report = clay_repair_report{};
    out_report->struct_size = declared;
    out_report->enclosed_voids = report.enclosed_voids;
    out_report->void_cells = report.void_cells;
    out_report->largest_void = report.largest_void;
    out_report->airtight = report.airtight ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_voxel_repair_close_holes(clay_voxel_grid* grid, int32_t passes,
                                          const clay_mask* mask) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
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

clay_result clay_voxel_rasterize(clay_voxel_grid* grid, const clay_document* doc,
                                 const float region_min[3], const float region_max[3]) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
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
    // document alone would thrash.
    scene::CullRegion cull{region};
    scene::Tape tape = scene::compile_document(doc->doc.document, &cull);
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
        scene::CullRegion cull{region};
        culled = scene::compile_document(doc->doc.document, &cull);
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

    for (std::size_t i = 0; i < count; ++i) {
        eval::GridQuery q;
        std::size_t samples = 0;
        r = read_grid(requests[i].origin, requests[i].spacing, requests[i].dims, &q, &samples);
        if (r != CLAY_OK) return r;
        const float band = requests[i].band;
        if (!(band >= 0.0f) || !std::isfinite(band))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "a request carries a band that is not finite and >= 0");
        // Culled against its own brick dilated by its own band, exactly as the
        // host-memory form does: the two produce the same values and differ
        // only in where they land.
        scene::CullRegion cull{request_brick_box(requests[i]).dilated(band)};
        scene::Tape tape = scene::compile_document(doc->doc.document, &cull);
        // Brick i at its own slot in the caller's single allocation.
        eval::DeviceBuffer slot = values;
        slot.offset = values.offset + static_cast<std::uint64_t>(i) * per * sizeof(float);
        slot.size = static_cast<std::uint64_t>(per) * sizeof(float);
        eval::DeviceBuffer color_slot;
        if (!colors.empty()) {
            color_slot = colors;
            color_slot.offset =
                colors.offset + static_cast<std::uint64_t>(i) * per * 3 * sizeof(float);
            color_slot.size = static_cast<std::uint64_t>(per) * 3 * sizeof(float);
        }
        switch (device->backend->eval_grid_device(tape, q, slot, color_slot)) {
            case eval::Status::Ok: break;
            case eval::Status::Unsupported:
                return fail(CLAY_ERROR_UNSUPPORTED,
                            "this backend does not evaluate into a caller's device buffer");
            case eval::Status::InvalidInput:
                return fail(CLAY_ERROR_INVALID_ARGUMENT, "a brick's device slot is invalid");
            default: return fail(CLAY_ERROR_BACKEND, "device evaluation failed");
        }
    }
    return CLAY_OK;
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
        // on the document alone would thrash.
        scene::CullRegion cull{region};
        handle->tape =
            std::make_shared<const scene::Tape>(scene::compile_document(doc->doc.document, &cull));
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

clay_result clay_brick_config_defaults(clay_brick_config* out_config) {
    if (!out_config) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null config");
    const brick::BrickConfig d;
    *out_config = clay_brick_config{};
    out_config->struct_size = static_cast<std::uint32_t>(sizeof(clay_brick_config));
    out_config->dim = d.dim;
    out_config->voxel_size = d.voxel_size;
    out_config->band_voxels = d.band_voxels;
    out_config->memory_budget = d.memory_budget;
    out_config->colors = d.colors ? 1 : 0;
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
    *out_config = clay_brick_config{};
    out_config->struct_size = declared;
    out_config->dim = c.dim;
    out_config->voxel_size = c.voxel_size;
    out_config->band_voxels = c.band_voxels;
    out_config->memory_budget = c.memory_budget;
    out_config->colors = c.colors ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_brick_cache_stats(const clay_brick_cache* cache, clay_brick_stats* out_stats) {
    if (!cache || !out_stats)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache or stats");
    clay_brick_stats probe;
    clay_result r = read_desc(out_stats, kBrickStatsOriginal, &probe);
    if (r != CLAY_OK) return r;
    const std::uint32_t declared = out_stats->struct_size;
    *out_stats = clay_brick_stats{};
    out_stats->struct_size = declared;
    out_stats->tracked_bricks = cache->cache.tracked_count();
    out_stats->surface_bricks = cache->cache.surface_bricks().size();
    // What is still queued INSIDE the engine plus what this binding drained
    // into staging and has not handed out yet: both are bricks the host still
    // owes an evaluation.
    out_stats->dirty_bricks = cache->cache.dirty_count() + cache->staged_remaining();
    out_stats->memory_usage = cache->cache.memory_usage();
    out_stats->memory_budget = cache->cache.config().memory_budget;
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
    // Every request is validated BEFORE any is evaluated, so a malformed one
    // refuses the call with nothing written rather than after its neighbours
    // have already landed.
    std::vector<kernel::cfloat3> origins(count);
    for (std::size_t i = 0; i < count; ++i) {
        eval::GridQuery q;
        std::size_t samples = 0;
        r = read_grid(requests[i].origin, requests[i].spacing, requests[i].dims, &q, &samples);
        if (r != CLAY_OK) return r;
        const float band = requests[i].band;
        if (!(band >= 0.0f) || !std::isfinite(band))
            return fail(CLAY_ERROR_INVALID_ARGUMENT,
                        "a request carries a band that is not finite and >= 0");
        origins[i] = q.origin;
    }
    // The whole batch goes to the backend as BATCHES of per-brick culled
    // tapes, not one call per brick: a GPU backend turns a batch into a single
    // device submission, and a per-brick submission costs more than the 512
    // samples it carries (issue #64). Chunked so the compiled tapes held at
    // once stay bounded — a tape carrying a sampled volume is megabytes.
    constexpr std::size_t kChunk = 4096;
    std::vector<scene::Tape> tapes;
    std::vector<const scene::Tape*> tape_ptrs;
    for (std::size_t base = 0; base < count;) {
        // A chunk shares one spacing as well as one lattice size: dims are
        // checked uniform above, but spacing is not, and each request keeps
        // its own — so a batch that mixes spacings splits where they change.
        std::size_t n = 1;
        while (n < kChunk && base + n < count &&
               requests[base + n].spacing == requests[base].spacing)
            ++n;
        tapes.clear();
        tape_ptrs.clear();
        tapes.reserve(n);
        tape_ptrs.reserve(n);
        for (std::size_t i = base; i < base + n; ++i) {
            // Dilated by the request's OWN band: an item a band outside the
            // brick still decides samples inside it, because a sample keeps
            // its true distance whenever that distance is within the band.
            scene::CullRegion cull{request_brick_box(requests[i]).dilated(requests[i].band)};
            tapes.push_back(scene::compile_document(doc->doc.document, &cull));
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
        if (b->eval_grid_batch(bq, out_values + base * per,
                               out_colors_rgb ? out_colors_rgb + base * per * 3 : nullptr) !=
            eval::Status::Ok)
            return fail(CLAY_ERROR_BACKEND, "eval_grid_batch failed");
        base += n;
    }
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

clay_result clay_brick_cache_mesh(const clay_brick_cache* cache, const clay_document* doc,
                                  const clay_brick_mesh_params* params, const int32_t* keys_xyz,
                                  size_t key_count, clay_brick_mesh_range* out_ranges,
                                  clay_mesh** out_mesh) {
    if (!cache || !params || !out_mesh)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null brick cache, params or out_mesh");
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
    // NULL keys means "every surface brick", which is what this call did before
    // the key list existed and what an export wants.
    std::vector<brick::BrickKey> subset;
    if (keys_xyz) {
        subset.reserve(key_count);
        for (std::size_t i = 0; i < key_count; ++i)
            subset.push_back(to_brick_key(keys_xyz + i * 3));
    }
    std::vector<mesh::BrickMeshRange> ranges;
    auto* handle = new clay_mesh();
    // The document rather than its compiled tape: gradient normals and colours
    // are evaluated through per-brick CULLED tapes, so their cost follows the
    // bricks named rather than the total document (issue #73).
    handle->data = mesh::mesh_bricks(cache->cache, doc ? &doc->doc.document : nullptr, options,
                                     keys_xyz ? &subset : nullptr, out_ranges ? &ranges : nullptr);
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
    for (std::size_t i = 0; i < count; ++i) {
        math::Ray ray;
        ray.origin = kernel::cf3(rays[i * 6], rays[i * 6 + 1], rays[i * 6 + 2]);
        ray.dir = kernel::cf3(rays[i * 6 + 3], rays[i * 6 + 4], rays[i * 6 + 5]);
        const pick::SceneHit hit = pick::raycast_bricks(cache->cache, ray);
        hits[i].hit = hit.hit ? 1 : 0;
        hits[i].t = hit.t;
        write_f3(hits[i].pos, hit.position);
        write_f3(hits[i].normal, hit.normal);
    }
    write_ray_hits(hits, count, out_hits, out_t, out_positions_xyz, out_normals_xyz);
    return CLAY_OK;
}

}  // extern "C"
