#pragma once

// LOCAL ADAPTIVE REMESHING (dynamic-topology spec, add-dynamic-topology).
//
// The policy that drives the three operators: split what is too long, collapse
// what is too short, flip what is badly shaped, and slide vertices tangentially
// to even the result. Everything here is bounded by the brush — a remesh pass
// over a whole surface is a different operation with a different cost, and this
// one runs inside a dab.
//
// HYSTERESIS IS NOT A REFINEMENT. With one threshold, an edge just above it
// splits into two edges just below it, which collapse back into one just above
// it — for as long as the artist holds the brush still. The gap between the
// split and collapse factors is what makes a stationary brush converge instead
// of oscillating, and the requirement names it for that reason.
//
// DETAIL IS BRUSH-RELATIVE BY DEFAULT. A world-unit target means an artist
// shrinking the brush to add detail gets the same triangles they had; making
// the target a fraction of the radius means a smaller brush makes finer
// geometry, which is what "detail" means when a sculptor says it — and it needs
// no second slider.

#include <cstdint>

#include "clay/mesh/dynamic_bvh.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/sculpt_common.h"
#include "clay/mesh/topology_delta.h"
#include "clay/mesh/topology_ops.h"

namespace clay {
namespace mesh {

// Where the target edge length comes from.
enum class DynamicDetailMode : std::uint8_t {
    // `target_edge_length` in world units. For a caller that knows the scale it
    // wants and does not want the brush changing it.
    World = 0,
    // radius / detail_resolution. The default, because it is the one a
    // sculptor's "detail" slider actually means.
    BrushRelative = 1,
    // No adaptation: the topology is left alone and only the deformation runs.
    // Not the same as switching the whole feature off — the surface is still a
    // dynamic one, and the next stamp may re-enable it.
    Constant = 2,
};

// When the remesh runs relative to the deformation.
//
// PER VERB, with a reason each, rather than one shared default: the right
// answer genuinely differs. A Grab that remeshes first stretches triangles it
// has just refined; one that remeshes after refines what the stretch produced,
// which is the point. A Clay that remeshes after deposits onto triangles too
// coarse to hold the deposit's shape.
enum class RemeshTiming : std::uint8_t {
    BeforeBrush = 0,
    AfterBrush = 1,
    BeforeAndAfter = 2,
};

RemeshTiming default_timing(MeshBrush verb);

struct DynamicTopologySettings {
    bool enabled = true;

    DynamicDetailMode detail_mode = DynamicDetailMode::BrushRelative;
    // World mode: the length itself.
    float target_edge_length = 0.01f;
    // Brush-relative mode: radius / this. Higher is finer.
    float detail_resolution = 8.0f;

    // Split above target * split_factor, collapse below target *
    // collapse_factor. THE GAP IS THE HYSTERESIS and must stay wide: the
    // classic values are 4/3 and 4/5, which is what these are.
    float split_factor = 1.33f;
    float collapse_factor = 0.8f;

    int max_passes = 3;
    // A BOUND, and a PARAMETER rather than a constant, so a host can trade
    // detail for latency on a slower device instead of choosing between the two
    // the library picked.
    int max_ops_per_stamp = 4096;

    bool allow_split = true;
    bool allow_collapse = true;
    bool allow_flip = true;
    bool relax_after_remesh = true;
    float relax_strength = 0.15f;

    bool preserve_boundaries = true;
    bool preserve_uv_seams = true;
    bool preserve_sharp_edges = true;

    // The operators' own refusal thresholds.
    TopologyOpOptions op;

    // The resolved target for a given brush radius.
    float target_for(float brush_radius) const {
        switch (detail_mode) {
            case DynamicDetailMode::World:
                return target_edge_length;
            case DynamicDetailMode::BrushRelative:
                return brush_radius / (detail_resolution > 0.0f ? detail_resolution : 1.0f);
            case DynamicDetailMode::Constant:
            default:
                return 0.0f;  // no adaptation
        }
    }
};

struct RemeshStats {
    std::size_t split = 0;
    std::size_t collapsed = 0;
    std::size_t flipped = 0;
    std::size_t relaxed = 0;
    // Operations refused, by the reason that refused them. A remesher that
    // silently does nothing and one that is being refused by a constraint look
    // identical from the outside, and a host tuning its thresholds needs to
    // tell them apart.
    std::size_t refused_constrained = 0;
    std::size_t refused_topology = 0;
    std::size_t refused_geometry = 0;
    // Whether the operation count hit its bound, so a caller can tell "the
    // region converged" from "the region ran out of budget".
    bool hit_budget = false;

    std::size_t total() const { return split + collapsed + flipped; }
};

// WHAT THE REMESH DID TO A CALLER'S OWN SET OF VERTICES, and the one veto it
// accepts.
//
// `remesh_region` retires vertex ids when it collapses and creates them when it
// splits, and it does so BETWEEN the stamps of a gesture. A caller that is
// carrying a set of vertices across a gesture — `DynamicSculptor`'s carried grab
// region is the one in the tree — cannot rebuild that set from the surface
// afterwards, because a vertex born mid-gesture has no captured position to read
// off it. So the remesh tells it, as it goes.
//
// BORROWED, NULL BY DEFAULT, AND NOTHING IN THE TREE HAS TO PASS ONE. Every
// hook is independently optional; a null pointer here costs one predictable
// branch per operator on a path that is about to allocate a vertex.
//
// Function pointers with a context, never `std::function`: `may_collapse` is
// consulted once per candidate edge on a per-dab path, and a `std::function`
// that fits its small buffer today allocates on the day somebody captures a
// second thing. `WorkItemReader` in `sculpt_workset.h` gives the same reason.
struct RemeshHooks {
    void* context = nullptr;
    // The edge (`a`, `b`) was split at its midpoint, producing `child`.
    void (*on_split)(void*, VertexId a, VertexId b, VertexId child) = nullptr;
    // `removed` was retired by a collapse; `kept` survived it.
    void (*on_collapse)(void*, VertexId kept, VertexId removed) = nullptr;
    // THE REMESHER ITSELF moved `v` — a collapse placing its survivor at the
    // midpoint, or the relaxation sliding a vertex tangentially. Never the
    // brush: a caller carrying captured positions has to follow this shift or
    // its next write puts the vertex back where the remesh moved it from.
    void (*on_move)(void*, VertexId v, kernel::cfloat3 before, kernel::cfloat3 after) = nullptr;
    // Consulted BEFORE a collapse that would retire `removed`; false refuses it
    // and the pass moves to the next edge. `collapse_edge` keeps the origin of
    // `halfedge_of(edge)` and removes its target, so both ends are known before
    // the operator runs.
    bool (*may_collapse)(void*, VertexId kept, VertexId removed) = nullptr;
};

// Adapt the surface under a brush toward the settings' target edge length.
//
// `bvh`, when given, is kept in step: every face an operator creates, destroys
// or moves is fed back to it, so the index does not have to be rebuilt after a
// stamp. Passing null is supported and means the caller maintains it.
RemeshStats remesh_region(DynamicSurface& surface, DynamicBvh* bvh, kernel::cfloat3 centre,
                          float radius, const DynamicTopologySettings& settings,
                          TopologyDelta* delta = nullptr, const RemeshHooks* hooks = nullptr);

// Slide the vertices in a region ALONG the surface toward the centroid of their
// neighbours, without moving the surface itself.
//
// The same idea `MeshBrush::Relax` implements on a fixed mesh, and for the same
// reason: after a remesh the triangles are the right SIZE and not yet evenly
// spaced. Constrained vertices — on a boundary, a seam or a crease — are left
// exactly where they are, because sliding one along the surface moves the
// feature it defines.
std::size_t relax_region(DynamicSurface& surface, DynamicBvh* bvh, kernel::cfloat3 centre,
                         float radius, float strength,
                         const DynamicTopologySettings& settings,
                         TopologyDelta* delta = nullptr, const RemeshHooks* hooks = nullptr);

}  // namespace mesh
}  // namespace clay
