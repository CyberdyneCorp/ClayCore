// C ABI implementation (c-abi spec): opaque handles over the C++ modules,
// thread-local error details, no exceptions cross this boundary (the core
// builds with -fno-exceptions on GCC/Clang).

#include "clay.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <unordered_map>
#include <optional>
#include <string>
#include <vector>

#include "desc_version.h"

#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/validate.h"
#include "clay/pick/pick.h"
#include "clay/scene/commands.h"
#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "clay/brush/stroke.h"
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
// The tape's own count: a new opcode without a clay_prim entry fails here.
static_assert(CLAY_PRIM_SWEPT + 1 == kernel::ctape_prim_count);

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

// Parameters each primitive takes, indexed by clay_prim (= the tape opcode).
// This is what the clay_prim comments document and what clay_item_create
// requires: a stroke's points and a lift's profile are out-of-line, so those
// entries count only the lift's own parameter.
// Loft takes 2: the half-depth and the ease. Its profiles are added
// separately, since a fixed block cannot carry a variable number of them.
constexpr int kPrimParams[] = {1, 3, 4, 4, 2, 7, 2, 3, 3, 3, 3, 1, 2, 1, 0, 1,
                               1, 4, 3, 3, 3, 4, 2, 3, 3, 1, 1, 1, 2, 1, 2, 2, 1};
static_assert(sizeof kPrimParams / sizeof kPrimParams[0] == kernel::ctape_prim_count);

constexpr int kProfileParams[] = {1, 2, 1, 1, 3, 2, 0};  // polygon: vertices instead
static_assert(sizeof kProfileParams / sizeof kProfileParams[0] == kernel::cprofile_polygon + 1);

constexpr int kDeformParams[] = {1, 1, 4, 2, 2, 3, 9, 3, 3, 8, 8, 10};
static_assert(sizeof kDeformParams / sizeof kDeformParams[0] == kernel::cdeform_pose_line + 1);

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
        scene::prim_carries_profiles(prim))
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

    // -- interleaved undo journal (openspec add-voxel-undo) ------------------
    // scene::UndoStack stays voxel-agnostic by the layering rule, so the
    // document interleaves its steps with voxel cell diffs here: each journal
    // step is one scene UndoStack step, one batch of voxel diffs, or (inside
    // a group) both — the two touch disjoint state, so order within a step
    // does not matter. Invariant: the number of steps with `scene` set equals
    // undo->undo_depth() (and likewise on the redo side), so popping the
    // journal and delegating scene steps can never drift.
    struct VoxelDiff {
        clay_layer_id layer = 0;
        std::vector<voxel::VoxelCoord> coords;
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
    };
    struct UndoStep {
        bool scene = false;
        std::vector<VoxelDiff> voxels;  // one entry per touched layer
        bool empty() const { return !scene && voxels.empty(); }
    };
    std::vector<UndoStep> undo_journal;
    std::vector<UndoStep> redo_journal;
    bool journal_grouping = false;
    UndoStep open_group;  // accumulates between begin/end_undo_group
};

struct clay_mesh {
    mesh::Mesh data;
};

// The item builder is a scene::Node under construction. Whether a transition
// was given (and which kind) is not a Node field, so it rides alongside: the
// op and the parameters have to agree, as they do in the Python bindings.
struct clay_item {
    scene::Node node;
    bool has_transition = false;
    bool transition_is_linear = false;
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
                        clay_node_id* out_node) {
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    node.id = layer->sdf->reserve_id();
    scene::NodeId id = node.id;
    std::vector<scene::Node> subtree;
    subtree.push_back(std::move(node));
    clay_result r = apply_edit(
        doc,
        scene::Command{scene::AddNodeCmd{layer_id, scene::kNoNode, -1, std::move(subtree)}},
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
    node.mirror = d.mirror != 0;
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
    if (!doc->undo) {
        if (!scene::apply(doc->doc.document, cmd)) return fail(CLAY_ERROR_NOT_FOUND, what);
        return CLAY_OK;
    }
    std::size_t depth_before = doc->undo->undo_depth();
    if (!doc->undo->perform(doc->doc.document, cmd)) return fail(CLAY_ERROR_NOT_FOUND, what);
    // Mirror the stack's step accounting into the journal: inside a group the
    // open step absorbs it; a coalesced command (AppendStroke) leaves the
    // depth unchanged and the journal top already names that scene step.
    doc->redo_journal.clear();
    if (doc->journal_grouping) {
        doc->open_group.scene = true;
    } else if (doc->undo->undo_depth() > depth_before) {
        clay_document::UndoStep step;
        step.scene = true;
        doc->undo_journal.push_back(std::move(step));
    }
    return CLAY_OK;
}

// Journal-aware grouping, shared by the public bracket entry points and the
// internal ones (clay_layer_apply_stroke): every group must open and close
// through here or the journal drifts from the scene stack.
void journal_begin_group(clay_document* doc) {
    doc->undo->begin_group();
    doc->journal_grouping = true;
    doc->open_group = clay_document::UndoStep{};
}

void journal_end_group(clay_document* doc) {
    doc->undo->end_group();
    doc->journal_grouping = false;
    if (!doc->open_group.empty()) {
        doc->undo_journal.push_back(std::move(doc->open_group));
        doc->redo_journal.clear();
        doc->undo->clear_redo();
    }
    doc->open_group = clay_document::UndoStep{};
}

// Record one voxel diff as (or into) an undo step. The scene stack's redo is
// cleared alongside the journal's: a fresh edit of EITHER kind invalidates
// redo of both, or a later scene redo would replay against the wrong journal.
void record_voxel_step(clay_document* doc, clay_document::VoxelDiff diff) {
    if (diff.coords.empty()) return;
    doc->redo_journal.clear();
    doc->undo->clear_redo();
    std::vector<clay_document::VoxelDiff>* target = nullptr;
    if (doc->journal_grouping) {
        target = &doc->open_group.voxels;
    } else {
        doc->undo_journal.push_back(clay_document::UndoStep{});
        target = &doc->undo_journal.back().voxels;
    }
    for (clay_document::VoxelDiff& existing : *target) {
        if (existing.layer != diff.layer) continue;
        // Merge (grouped drags): first-touch `before` wins, latest `after`.
        std::unordered_map<voxel::VoxelCoord, std::size_t, voxel::VoxelCoordHash> where;
        where.reserve(existing.coords.size());
        for (std::size_t i = 0; i < existing.coords.size(); ++i) where[existing.coords[i]] = i;
        for (std::size_t i = 0; i < diff.coords.size(); ++i) {
            auto found = where.find(diff.coords[i]);
            if (found == where.end()) {
                existing.coords.push_back(diff.coords[i]);
                existing.before.push_back(diff.before[i]);
                existing.after.push_back(diff.after[i]);
            } else {
                existing.after[found->second] = diff.after[i];
            }
        }
        return;
    }
    target->push_back(std::move(diff));
}

// Apply one side of a step's voxel diffs to the document's grids.
void apply_voxel_diffs(clay_document* doc, const clay_document::UndoStep& step, bool to_before) {
    for (const clay_document::VoxelDiff& diff : step.voxels) {
        auto grid = doc->doc.voxel_layers.find(diff.layer);
        if (grid == doc->doc.voxel_layers.end()) continue;
        const std::vector<std::uint8_t>& slots = to_before ? diff.before : diff.after;
        for (std::size_t i = 0; i < diff.coords.size(); ++i)
            grid->second.set(diff.coords[i], slots[i]);
    }
}

// -- voxel edit capture -------------------------------------------------------
// Every mutating voxel entry point below wraps its engine call with a region
// capture: snapshot the cells the verb can write (writes are bounded by the
// brush footprint — pads in sculpt.cpp are read-side), let it run, and journal
// the cells that changed. Standalone grids and undo-disabled documents skip
// straight to the engine call. Repair passes stay direct: they are grid-wide
// and out of the add-voxel-undo scope.

std::vector<voxel::VoxelCoord> region_box(voxel::VoxelCoord a, voxel::VoxelCoord b) {
    voxel::VoxelCoord lo{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    voxel::VoxelCoord hi{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    std::vector<voxel::VoxelCoord> out;
    out.reserve(static_cast<std::size_t>(hi.x - lo.x + 1) * (hi.y - lo.y + 1) *
                (hi.z - lo.z + 1));
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) out.push_back({x, y, z});
    return out;
}

// The size-n footprint spans -((n-1)/2) ..= n/2 per axis (grid.h).
std::vector<voxel::VoxelCoord> region_footprint(voxel::VoxelCoord c, int size) {
    std::int32_t lo = -((size - 1) / 2), hi = size / 2;
    return region_box({c.x + lo, c.y + lo, c.z + lo}, {c.x + hi, c.y + hi, c.z + hi});
}

std::vector<voxel::VoxelCoord> region_mirrors(voxel::VoxelCoord c, std::uint8_t axes) {
    std::vector<voxel::VoxelCoord> out;
    for (std::uint8_t combo = 0; combo < 8; ++combo) {
        if ((combo & ~axes) != 0) continue;
        out.push_back(voxel::VoxelGrid::mirrored(c, combo));
    }
    return out;
}

template <typename Fn>
void voxel_edit_with_undo(clay_voxel_grid* grid, voxel::VoxelGrid* g,
                          std::vector<voxel::VoxelCoord> region, Fn&& mutate) {
    clay_document* doc = grid ? grid->doc : nullptr;
    if (!doc || !doc->undo) {
        mutate();
        return;
    }
    std::vector<std::uint8_t> before(region.size());
    for (std::size_t i = 0; i < region.size(); ++i) before[i] = g->get(region[i]);
    mutate();
    clay_document::VoxelDiff diff;
    diff.layer = grid->layer;
    for (std::size_t i = 0; i < region.size(); ++i) {
        std::uint8_t now = g->get(region[i]);
        if (now == before[i]) continue;
        diff.coords.push_back(region[i]);
        diff.before.push_back(before[i]);
        diff.after.push_back(now);
    }
    record_voxel_step(doc, std::move(diff));
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

clay_result clay_add_item(clay_document* doc, clay_layer_id layer_id,
                          const clay_item_desc* item, clay_node_id* out_node) {
    if (!doc || !item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or item");
    clay_item_desc d;
    clay_result r = read_desc(item, kItemDescOriginal, &d);
    if (r != CLAY_OK) return r;
    r = validate_item_desc(d);
    if (r != CLAY_OK) return r;
    r = canonical_prim_params(d.prim, d.params);
    if (r != CLAY_OK) return r;
    return insert_node(doc, layer_id, item_from_desc(d).node, out_node);
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
    // An empty journal is reported, not failed: a UI drives this without
    // having to track whether anything is left.
    if (doc->undo_journal.empty()) {
        *out_undone = 0;
        return CLAY_OK;
    }
    clay_document::UndoStep step = std::move(doc->undo_journal.back());
    doc->undo_journal.pop_back();
    if (step.scene) doc->undo->undo(doc->doc.document);
    apply_voxel_diffs(doc, step, /*to_before=*/true);
    doc->redo_journal.push_back(std::move(step));
    *out_undone = 1;
    return CLAY_OK;
}

clay_result clay_document_redo(clay_document* doc, int32_t* out_redone) {
    if (!doc || !out_redone) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    if (doc->redo_journal.empty()) {
        *out_redone = 0;
        return CLAY_OK;
    }
    clay_document::UndoStep step = std::move(doc->redo_journal.back());
    doc->redo_journal.pop_back();
    if (step.scene) doc->undo->redo(doc->doc.document);
    apply_voxel_diffs(doc, step, /*to_before=*/false);
    doc->undo_journal.push_back(std::move(step));
    *out_redone = 1;
    return CLAY_OK;
}

clay_result clay_document_undo_state(const clay_document* doc, int32_t* out_enabled,
                                     size_t* out_undo_depth, size_t* out_redo_depth) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (out_enabled) *out_enabled = doc->undo ? 1 : 0;
    // Journal steps count voxel edits alongside scene steps (an open group
    // counts once as soon as it holds anything).
    std::size_t open = (doc->undo && doc->journal_grouping && !doc->open_group.empty()) ? 1 : 0;
    if (out_undo_depth) *out_undo_depth = doc->undo ? doc->undo_journal.size() + open : 0;
    if (out_redo_depth) *out_redo_depth = doc->undo ? doc->redo_journal.size() : 0;
    return CLAY_OK;
}

clay_result clay_document_begin_undo_group(clay_document* doc) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    journal_begin_group(doc);
    return CLAY_OK;
}

clay_result clay_document_end_undo_group(clay_document* doc) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!doc->undo) return fail(CLAY_ERROR_INVALID_ARGUMENT, "undo is not enabled");
    journal_end_group(doc);
    return CLAY_OK;
}

clay_result clay_layer_set_transform(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float position[3], const float rotation_axis[3],
                                     float rotation_angle, float scale) {
    math::Transform xform;
    clay_result r = read_transform(position, rotation_axis, rotation_angle, scale, &xform);
    if (r != CLAY_OK) return r;
    return apply_edit(doc, scene::Command{scene::SetTransformCmd{layer, node, xform}},
                      "node not found");
}

clay_result clay_layer_set_prim(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                int32_t prim, const float* params, size_t param_count) {
    if (!prim_is_known(prim)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown primitive type");
    clay_result r = check_params("primitive", params, param_count, kPrimParams[prim]);
    if (r != CLAY_OK) return r;
    float p[scene::kMaxPrimParams] = {};
    for (size_t i = 0; i < param_count; ++i) p[i] = params[i];
    r = canonical_prim_params(prim, p);
    if (r != CLAY_OK) return r;

    scene::Prim replacement;
    replacement.type = static_cast<scene::PrimType>(prim);
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
    if (!op_is_known(op)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown combine op");
    if (!blend_is_known(blend)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown blend");
    if (!(blend_k >= 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "blend k must be >= 0");
    if (!(rounding >= 0.0f)) return fail(CLAY_ERROR_INVALID_ARGUMENT, "rounding must be >= 0");

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

clay_result clay_document_remove_layer(clay_document* doc, clay_layer_id layer) {
    return apply_edit(doc, scene::Command{scene::RemoveLayerCmd{layer}}, "layer not found");
}

clay_result clay_document_move_layer(clay_document* doc, clay_layer_id layer, int32_t index) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    const scene::Layer* found = doc->doc.document.find_layer(layer);
    if (!found) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    scene::Layer copy = *found;
    clay_result r = apply_edit(doc, scene::Command{scene::RemoveLayerCmd{layer}},
                               "layer not found");
    if (r != CLAY_OK) return r;
    return apply_edit(doc, scene::Command{scene::AddLayerCmd{std::move(copy), index}},
                      "layer could not be reinserted");
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
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    layer->mirror_axes = static_cast<std::uint8_t>((axis_x ? scene::kMirrorX : 0) |
                                                   (axis_y ? scene::kMirrorY : 0) |
                                                   (axis_z ? scene::kMirrorZ : 0));
    layer->mirror_k = mirror_k;
    return CLAY_OK;
}

// -- item builder (c-abi spec: item builder for composed edits) --------------

clay_item* clay_item_create(int32_t prim, const float* params, size_t param_count) {
    if (!prim_is_known(prim)) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown primitive type");
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
    item->node.mirror = mirror != 0;
    return CLAY_OK;
}

clay_result clay_item_add_deformer(clay_item* item, int32_t deform, const float* params,
                                   size_t param_count, int32_t ease) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (deform < 0 || deform > CLAY_DEFORM_POSE_LINE)
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

clay_result clay_item_set_curve_points(clay_item* item, const float* xyzr, size_t count,
                                       const int32_t* types, const float* in_handles_xyz,
                                       const float* out_handles_xyz) {
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (item->node.prim.type != scene::PrimType::Stroke &&
        !scene::prim_is_swept(item->node.prim.type))
        return fail(CLAY_ERROR_INVALID_ARGUMENT,
                    "curve points need CLAY_PRIM_STROKE or CLAY_PRIM_SWEPT");
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

clay_result clay_layer_set_stroke_points(clay_document* doc, clay_layer_id layer,
                                         clay_node_id node, const float* xyzr, size_t count,
                                         const int32_t* types, const float* in_handles_xyz,
                                         const float* out_handles_xyz, int32_t closed,
                                         float tolerance) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    if (!(tolerance > 0.0f))
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "curve tolerance must be > 0");
    std::vector<scene::StrokePoint> points;
    clay_result r = read_curve_points(xyzr, count, types, in_handles_xyz, out_handles_xyz,
                                      &points);
    if (r != CLAY_OK) return r;
    return apply_edit(doc,
                      scene::Command{scene::SetStrokePointsCmd{layer, node, std::move(points),
                                                               closed != 0, tolerance}},
                      "no stroke or curve with that id in that layer");
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
    // is one step to undo. Routed through the journal-aware brackets so the
    // step counts once there too; a caller's open group is left alone.
    bool own_group = doc->undo && !doc->journal_grouping;
    if (own_group) journal_begin_group(doc);
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
    if (own_group) journal_end_group(doc);
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
    return eval_into(scene::compile_document(doc->doc.document), backend, points_xyz, count,
                     eval::PointResults{out_distances, nullptr, out_colors_rgb});
}

clay_result clay_eval_gradients(const clay_document* doc, const char* backend,
                                const float* points_xyz, size_t count,
                                float* out_gradients_xyz) {
    if (!doc || !points_xyz || !out_gradients_xyz)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    return gradients_into(scene::compile_document(doc->doc.document), backend, points_xyz,
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
    *out_scale = scene::compile_document(doc->doc.document).safe_step_scale();
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

clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]) {
    if (!doc || !origin || !dir || !out_hit)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    eval::Backend* b = eval::Registry::instance().find("cpu");
    scene::Tape tape = pick::pickable_tape(doc->doc.document);
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
    scene::Tape tape = pick::pickable_tape(doc->doc.document);
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
    scene::Tape tape = pick::pickable_tape(doc->doc.document);
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
    scene::Tape tape = scene::compile_document(doc->doc.document);
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

void clay_mesh_destroy(clay_mesh* mesh) { delete mesh; }

size_t clay_mesh_vertex_count(const clay_mesh* mesh) {
    return mesh ? mesh->data.positions.size() : 0;
}
size_t clay_mesh_index_count(const clay_mesh* mesh) {
    return mesh ? mesh->data.indices.size() : 0;
}
const float* clay_mesh_positions(const clay_mesh* mesh) {
    return mesh && !mesh->data.positions.empty() ? &mesh->data.positions[0].x : nullptr;
}
// !empty() before the size comparison, not instead of it: clay_voxel_mesh is
// the one call that hands back a mesh with nothing in it, and indexing an
// empty vector to take the address of its first field is undefined even when
// the result is never dereferenced.
const float* clay_mesh_normals(const clay_mesh* mesh) {
    return mesh && !mesh->data.normals.empty() &&
                   mesh->data.normals.size() == mesh->data.positions.size()
               ? &mesh->data.normals[0].x
               : nullptr;
}
const float* clay_mesh_colors(const clay_mesh* mesh) {
    return mesh && !mesh->data.colors.empty() &&
                   mesh->data.colors.size() == mesh->data.positions.size()
               ? &mesh->data.colors[0].x
               : nullptr;
}
const uint32_t* clay_mesh_indices(const clay_mesh* mesh) {
    return mesh && !mesh->data.indices.empty() ? mesh->data.indices.data() : nullptr;
}

clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                               int32_t* out_manifold) {
    if (!mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh");
    mesh::ValidationReport r = mesh::validate(mesh->data);
    if (out_watertight) *out_watertight = r.watertight ? 1 : 0;
    if (out_manifold) *out_manifold = r.manifold ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mesh_save(const clay_mesh* mesh, const char* path) {
    if (!mesh || !path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh or path");
    std::string p(path);
    std::size_t dot = p.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot + 1);
    if (ext == "obj") return from_io(io::save_obj_file(mesh->data, p));
    if (ext == "ply") return from_io(io::save_ply_file(mesh->data, p));
    if (ext == "fbx") return from_io(io::save_fbx_file(mesh->data, p));
    if (ext == "glb") return from_io(io::save_glb_file(mesh->data, p));
    return fail(CLAY_ERROR_UNSUPPORTED, "unknown extension: " + ext);
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

    math::Aabb region(kernel::cf3(d.region_min[0], d.region_min[1], d.region_min[2]),
                      kernel::cf3(d.region_max[0], d.region_max[1], d.region_max[2]));
    std::optional<scene::Node> node = cut::cut_item(frame, shape, region, options);
    if (!node) {
        fail(CLAY_ERROR_INVALID_ARGUMENT, "the cut is degenerate: a shape with no area");
        return nullptr;
    }
    auto* item = new clay_item();
    item->node = std::move(*node);
    return item;
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

clay_result clay_voxel_size(const clay_voxel_grid* grid, float* out_voxel_size) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve(grid, &g);
    if (r != CLAY_OK) return r;
    if (out_voxel_size) *out_voxel_size = g->voxel_size();
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
    voxel_edit_with_undo(grid, g, {to_coord(cell)}, [&] { g->set(to_coord(cell), slot); });
    return CLAY_OK;
}

clay_result clay_voxel_erase(clay_voxel_grid* grid, const int32_t cell[3]) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_at(grid, cell, &g);
    if (r != CLAY_OK) return r;
    voxel_edit_with_undo(grid, g, {to_coord(cell)}, [&] { g->erase(to_coord(cell)); });
    return CLAY_OK;
}

clay_result clay_voxel_paint(clay_voxel_grid* grid, const int32_t cell[3], int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, cell, index, &g, &slot);
    if (r != CLAY_OK) return r;
    voxel_edit_with_undo(grid, g, {to_coord(cell)}, [&] { g->paint(to_coord(cell), slot); });
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
    std::vector<voxel::VoxelCoord> region(count);
    for (size_t i = 0; i < count; ++i) region[i] = to_coord(cells_xyz + i * 3);
    voxel_edit_with_undo(grid, g, std::move(region), [&] {
        for (size_t i = 0; i < count; ++i) g->set(to_coord(cells_xyz + i * 3), slot);
    });
    return CLAY_OK;
}

clay_result clay_voxel_erase_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count) {
    voxel::VoxelGrid* g = nullptr;
    clay_result r = resolve_batch(grid, cells_xyz, count, &g);
    if (r != CLAY_OK) return r;
    std::vector<voxel::VoxelCoord> region(count);
    for (size_t i = 0; i < count; ++i) region[i] = to_coord(cells_xyz + i * 3);
    voxel_edit_with_undo(grid, g, std::move(region), [&] {
        for (size_t i = 0; i < count; ++i) g->erase(to_coord(cells_xyz + i * 3));
    });
    return CLAY_OK;
}

clay_result clay_voxel_fill_box(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, a, index, &g, &slot);
    if (r != CLAY_OK) return r;
    if (!b) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    voxel_edit_with_undo(grid, g, region_box(to_coord(a), to_coord(b)),
                         [&] { g->fill_box(to_coord(a), to_coord(b), slot); });
    return CLAY_OK;
}

clay_result clay_voxel_fill_line(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                 int32_t index) {
    voxel::VoxelGrid* g = nullptr;
    std::uint8_t slot = 0;
    clay_result r = resolve_at_index(grid, a, index, &g, &slot);
    if (r != CLAY_OK) return r;
    if (!b) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null cell");
    voxel_edit_with_undo(grid, g, region_box(to_coord(a), to_coord(b)),
                         [&] { g->fill_line(to_coord(a), to_coord(b), slot); });
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
    voxel_edit_with_undo(grid, g, region_mirrors(to_coord(cell), mask),
                         [&] { g->set_mirrored(to_coord(cell), slot, mask); });
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
    voxel_edit_with_undo(grid, g, region_mirrors(to_coord(cell), mask),
                         [&] { g->paint_mirrored(to_coord(cell), slot, mask); });
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
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->set_brush(to_coord(cell), p, slot); });
    return CLAY_OK;
}

clay_result clay_voxel_erase_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->erase_brush(to_coord(cell), p); });
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
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->paint_brush(to_coord(cell), p, slot); });
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_smooth(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_smooth(to_coord(cell), p); });
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_inflate(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, int32_t amount) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_inflate(to_coord(cell), p, amount); });
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
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_flatten(to_coord(cell), p, n, offset_cells); });
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_pinch(clay_voxel_grid* grid, const int32_t cell[3],
                                    const clay_brush_params* brush) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_pinch(to_coord(cell), p); });
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
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size), [&] {
        g->sculpt_grab(to_coord(cell), p,
                       kernel::cf3(displacement[0], displacement[1], displacement[2]),
                       front_only != 0);
    });
    return CLAY_OK;
}

clay_result clay_voxel_sculpt_fill_cavities(clay_voxel_grid* grid, const int32_t cell[3],
                                            const clay_brush_params* brush, int32_t passes) {
    voxel::VoxelGrid* g = nullptr;
    voxel::BrushParams p;
    clay_result r = resolve_brush(grid, cell, brush, &g, &p);
    if (r != CLAY_OK) return r;
    if (passes <= 0) return fail(CLAY_ERROR_INVALID_ARGUMENT, "passes must be > 0");
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_fill_cavities(to_coord(cell), p, passes); });
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
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_scrape(to_coord(cell), p, n, offset_cells); });
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
    voxel_edit_with_undo(grid, g, region_footprint(to_coord(cell), p.size),
                         [&] { g->sculpt_smudge(to_coord(cell), p,
                     kernel::cf3(displacement[0], displacement[1], displacement[2])); });
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
    scene::Tape tape = scene::compile_document(doc->doc.document);
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

}  // extern "C"
