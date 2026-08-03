#pragma once

// Scene vocabulary: primitive descriptors, ops, blends, edit nodes.
// A Node is either an edit ITEM (primitive/stroke + op + blend + color) or a
// GROUP (children + group op, including None = children apply inline).
// Ordered edit-list semantics: each item applies to the combined result of
// everything before it (scene-model spec).

#include <cstdint>
#include <vector>

#include "clay/kernel/tape.h"
#include "clay/math/transform.h"

namespace clay {
namespace scene {

using NodeId = std::uint32_t;
using LayerId = std::uint32_t;
inline constexpr NodeId kNoNode = 0;

// 1:1 with the tape primitive opcodes (kernel/tape.h).
enum class PrimType : std::uint8_t {
    Sphere = kernel::ctape_sphere,
    Box = kernel::ctape_box,
    RoundBox = kernel::ctape_round_box,
    BoxFrame = kernel::ctape_box_frame,
    Torus = kernel::ctape_torus,
    Capsule = kernel::ctape_capsule,
    CappedCylinder = kernel::ctape_capped_cylinder,
    RoundedCylinder = kernel::ctape_rounded_cylinder,
    CappedCone = kernel::ctape_capped_cone,
    RoundCone = kernel::ctape_round_cone,
    Ellipsoid = kernel::ctape_ellipsoid,
    Octahedron = kernel::ctape_octahedron,
    HexPrism = kernel::ctape_hex_prism,
    Pyramid = kernel::ctape_pyramid,
    Stroke = kernel::ctape_stroke,
};

enum class Op : std::uint8_t {
    None = 255,  // groups only: children apply inline to the outer chain
    Add = kernel::ccombine_add,
    Subtract = kernel::ccombine_subtract,
    Intersect = kernel::ccombine_intersect,
    Paint = kernel::ccombine_paint,
    // Extended vocabulary (kernel/tape.h math + color semantics). blend.k is
    // the mode's radius/depth; the blend profile is ignored. Groove/Tongue
    // additionally consume the node's rounding (world units) as the channel
    // half-width rb — the item field itself still gets rounded, so the
    // channel is centered on the rounded surface.
    Groove = kernel::ccombine_groove,
    Tongue = kernel::ccombine_tongue,
    Pipe = kernel::ccombine_pipe,
    Engrave = kernel::ccombine_engrave,
    Emboss = kernel::ccombine_emboss,
    Inset = kernel::ccombine_inset,
    Shell = kernel::ccombine_shell,
    Replace = kernel::ccombine_replace,
};

inline bool op_is_extended(Op op) {
    return static_cast<int>(op) >= kernel::ccombine_groove &&
           static_cast<int>(op) <= kernel::ccombine_replace;
}

// Diagonal modes mix both gradients (Lipschitz up to sqrt(2); exactness.h).
inline bool op_is_diagonal(Op op) {
    return op == Op::Pipe || op == Op::Engrave || op == Op::Emboss;
}

// Modes that create material independent of the accumulated field. They are
// not skipped on an empty accumulator: the compiler emits their combine
// anyway and the interpreter seeds it with the far field, so per-brick
// culling of everything beneath them stays band-clamp identical.
inline bool op_creates_material(Op op) { return op == Op::Shell || op == Op::Replace; }

enum class BlendProfile : std::uint8_t {
    Hard = kernel::cblend_hard,
    Quadratic = kernel::cblend_quadratic,
    Cubic = kernel::cblend_cubic,
    Circular = kernel::cblend_circular,
    Chamfer = kernel::cblend_chamfer,
};

struct Blend {
    BlendProfile profile = BlendProfile::Hard;
    float k = 0.0f;

    float support() const {
        return kernel::ctape_blend_support(static_cast<int>(profile), k);
    }
};

inline constexpr int kMaxPrimParams = 7;

struct Prim {
    PrimType type = PrimType::Sphere;
    float params[kMaxPrimParams] = {};

    static Prim sphere(float r) { return {PrimType::Sphere, {r}}; }
    static Prim box(kernel::cfloat3 b) { return {PrimType::Box, {b.x, b.y, b.z}}; }
    static Prim round_box(kernel::cfloat3 b, float r) {
        return {PrimType::RoundBox, {b.x, b.y, b.z, r}};
    }
    static Prim box_frame(kernel::cfloat3 b, float e) {
        return {PrimType::BoxFrame, {b.x, b.y, b.z, e}};
    }
    static Prim torus(float R, float r) { return {PrimType::Torus, {R, r}}; }
    static Prim capsule(kernel::cfloat3 a, kernel::cfloat3 b, float r) {
        return {PrimType::Capsule, {a.x, a.y, a.z, b.x, b.y, b.z, r}};
    }
    static Prim capped_cylinder(float r, float h) { return {PrimType::CappedCylinder, {r, h}}; }
    static Prim rounded_cylinder(float ra, float rb, float h) {
        return {PrimType::RoundedCylinder, {ra, rb, h}};
    }
    static Prim capped_cone(float h, float r1, float r2) {
        return {PrimType::CappedCone, {h, r1, r2}};
    }
    static Prim round_cone(float r1, float r2, float h) {
        return {PrimType::RoundCone, {r1, r2, h}};
    }
    static Prim ellipsoid(kernel::cfloat3 r) { return {PrimType::Ellipsoid, {r.x, r.y, r.z}}; }
    static Prim octahedron(float s) { return {PrimType::Octahedron, {s}}; }
    static Prim hex_prism(float hx, float hy) { return {PrimType::HexPrism, {hx, hy}}; }
    static Prim pyramid(float h) { return {PrimType::Pyramid, {h}}; }
    static Prim stroke() { return {PrimType::Stroke, {}}; }
};

struct StrokePoint {
    kernel::cfloat3 pos;
    float radius;
};

struct Node {
    NodeId id = kNoNode;
    bool is_group = false;
    bool visible = true;

    Op op = Op::Add;
    Blend blend;

    // item fields
    Prim prim;
    math::Transform xform;
    float rounding = 0.0f;
    kernel::cfloat3 color = kernel::cf3(0.7f, 0.7f, 0.7f);
    bool mirror = false;  // apply through the layer's active mirror
    std::vector<StrokePoint> stroke;  // PrimType::Stroke only
    float stroke_blend_k = 0.0f;      // within-stroke segment smoothing

    // group fields
    std::vector<NodeId> children;
};

}  // namespace scene
}  // namespace clay
