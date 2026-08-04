// C ABI implementation (c-abi spec): opaque handles over the C++ modules,
// thread-local error details, no exceptions cross this boundary (the core
// builds with -fno-exceptions on GCC/Clang).

#include "clay.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "desc_version.h"

#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/validate.h"
#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"

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
// The tape's own count: a new opcode without a clay_prim entry fails here.
static_assert(CLAY_PRIM_LNORM_SPHERE + 1 == kernel::ctape_prim_count);

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

static_assert(CLAY_PROFILE_CIRCLE == static_cast<int>(kernel::cprofile_circle));
static_assert(CLAY_PROFILE_BOX == static_cast<int>(kernel::cprofile_box));
static_assert(CLAY_PROFILE_HEXAGON == static_cast<int>(kernel::cprofile_hexagon));
static_assert(CLAY_PROFILE_TRIANGLE == static_cast<int>(kernel::cprofile_triangle));
static_assert(CLAY_PROFILE_TRAPEZOID == static_cast<int>(kernel::cprofile_trapezoid));
static_assert(CLAY_PROFILE_VESICA == static_cast<int>(kernel::cprofile_vesica));
static_assert(CLAY_PROFILE_POLYGON == static_cast<int>(kernel::cprofile_polygon));

static_assert(CLAY_EASE_LINEAR == kernel::ease_linear);
static_assert(CLAY_EASE_COUNT == kernel::ease_count);

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

// Parameters each primitive takes, indexed by clay_prim (= the tape opcode).
// This is what the clay_prim comments document and what clay_item_create
// requires: a stroke's points and a lift's profile are out-of-line, so those
// entries count only the lift's own parameter.
constexpr int kPrimParams[] = {1, 3, 4, 4, 2, 7, 2, 3, 3, 3, 3, 1, 2, 1, 0, 1,
                               1, 4, 3, 3, 3, 4, 2, 3, 3, 1, 1, 1, 2, 1, 2};
static_assert(sizeof kPrimParams / sizeof kPrimParams[0] == kernel::ctape_prim_count);

constexpr int kProfileParams[] = {1, 2, 1, 1, 3, 2, 0};  // polygon: vertices instead
static_assert(sizeof kProfileParams / sizeof kProfileParams[0] == kernel::cprofile_polygon + 1);

constexpr int kDeformParams[] = {1, 1, 4, 2};
static_assert(sizeof kDeformParams / sizeof kDeformParams[0] == kernel::cdeform_displace + 1);

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

// Out-of-line payloads: counts cross the ABI as size_t but serialize as u32.
clay_result check_payload(const char* what, const float* data, std::size_t count) {
    if (count > 0 && !data) return fail(CLAY_ERROR_INVALID_ARGUMENT, std::string("null ") + what);
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        if (count > 0xffffffffu)
            return fail(CLAY_ERROR_INVALID_ARGUMENT, std::string("too many ") + what);
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
    if (prim == scene::PrimType::Stroke || scene::prim_is_lift(prim))
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

struct clay_document {
    io::ClaySpaceDoc doc;
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

// The one insertion path: everything authored through this ABI, flat
// descriptor included, ends here.
clay_result insert_node(clay_document* doc, clay_layer_id layer_id, scene::Node node,
                        clay_node_id* out_node) {
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    scene::NodeId id = layer->sdf->insert(std::move(node));
    if (out_node) *out_node = id;
    return CLAY_OK;
}

// The transition cross-check the Python bindings do in Layer.add: a morph op
// needs parameters, of its own kind, and no other op accepts them.
clay_result validate_item(const clay_item& item) {
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
    scene::Layer& layer = doc->doc.document.add_sdf_layer(name);
    if (out_layer) *out_layer = layer.id;
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
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (layer->sdf->remove(node).empty())
        return fail(CLAY_ERROR_NOT_FOUND, "node not found");
    return CLAY_OK;
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
    if (deform < 0 || deform > CLAY_DEFORM_DISPLACE)
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
    if (!item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null item");
    if (item->node.prim.type != scene::PrimType::Stroke)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke points need CLAY_PRIM_STROKE");
    clay_result r = check_payload("stroke points", xyzr, count);
    if (r != CLAY_OK) return r;
    std::vector<scene::StrokePoint> points;
    points.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const float* p = xyzr + i * 4;
        if (p[3] < 0.0f) return fail(CLAY_ERROR_INVALID_ARGUMENT, "stroke radius must be >= 0");
        points.push_back(scene::StrokePoint{kernel::cf3(p[0], p[1], p[2]), p[3]});
    }
    item->node.stroke = std::move(points);
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
    eval::Backend* b = eval::Registry::instance().find(backend ? backend : "cpu");
    if (!b) return fail(CLAY_ERROR_NOT_FOUND, std::string("backend not registered: ") +
                                                  (backend ? backend : "cpu"));
    scene::Tape tape = scene::compile_document(doc->doc.document);
    eval::PointQuery q{points_xyz, count, 1e-4f};
    eval::PointResults out{out_distances, nullptr, out_colors_rgb};
    eval::Status s = b->eval_points(tape, q, out);
    if (s != eval::Status::Ok) return fail(CLAY_ERROR_BACKEND, "eval_points failed");
    return CLAY_OK;
}

clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]) {
    if (!doc || !origin || !dir || !out_hit)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    eval::Backend* b = eval::Registry::instance().find("cpu");
    scene::Tape tape = scene::compile_document(doc->doc.document);
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
    mesh::Mesh m = mesh::mesh_tape(tape, region, voxel);
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
const float* clay_mesh_normals(const clay_mesh* mesh) {
    return mesh && mesh->data.normals.size() == mesh->data.positions.size()
               ? &mesh->data.normals[0].x
               : nullptr;
}
const float* clay_mesh_colors(const clay_mesh* mesh) {
    return mesh && mesh->data.colors.size() == mesh->data.positions.size()
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

}  // extern "C"
