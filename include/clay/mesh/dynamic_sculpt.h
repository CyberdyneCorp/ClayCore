#pragma once

// SCULPTING A SURFACE THAT CAN GROW (dynamic-topology spec,
// add-dynamic-topology).
//
// The brush engine over `DynamicSurface`: the same verbs, the same falloffs,
// the same mask, and the same DEFORMATION MATH — `sculpt_kernels.h`, called
// rather than copied. That is the whole reason `add-shared-brush-kernels` came
// first. If Clay were a copy here, Clay would mean two things, and an artist who
// learned a brush on a mesh layer and found it behaved differently on an
// adaptive one would not have found a bug they could report; they would have
// found that the tool is untrustworthy.
//
// WHAT THIS OWNS, and it is deliberately only the parts that are about the
// REPRESENTATION: gathering a region by stable id, walking the mutable
// adjacency, writing the result back, recomputing normals locally, keeping the
// chunked index in step, and scheduling the remesher around the deformation.
//
// WHICH VERBS AN ADAPTIVE SURFACE OFFERS is answered in `offers`, and the one it
// declines is declined for a reason rather than left silently partial.

#include <cstdint>
#include <vector>

#include "clay/field/relax.h"  // MaskGate
#include "clay/math/geom.h"
#include "clay/mesh/dynamic_bvh.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/remesh_local.h"
#include "clay/mesh/sculpt_common.h"
#include "clay/mesh/sculpt_kernels.h"
#include "clay/mesh/topology_delta.h"

namespace clay {
namespace mesh {

// Whether an adaptive surface offers this verb.
//
// FIFTEEN OF THE SIXTEEN. The one it declines is LAYER, and the reason is
// structural rather than an omission: layer deposits up to a ceiling measured
// from where the surface was when the STROKE began, per vertex. On a fixed mesh
// the stroke's own `VertexDeltas` is that reference. On an adaptive one, half
// the vertices under the brush at the end of a stroke did not exist at the
// start — a split created them — so for those the reference does not exist, and
// a verb that silently becomes Draw for the new vertices and Layer for the old
// ones is worse than one that says it is not offered.
bool dynamic_offers(MeshBrush verb);

struct DynamicSculptOptions {
    DynamicBvhOptions index;
    // The path budget for the surface walk, in radius units, and where the rim
    // taper starts. The same values the fixed geodesic walk uses, so a brush's
    // reach means the same thing on both representations.
    float path_budget = 2.0f;
    float taper_start = 1.5f;
};

struct DynamicStampResult {
    std::size_t moved_vertices = 0;
    RemeshStats remesh;

    // What changed, for a host deciding what to re-upload.
    math::Aabb dirty_bounds;
    std::uint64_t topology_revision = 0;
    std::uint64_t geometry_revision = 0;
    std::uint64_t attribute_revision = 0;

    bool changed() const { return moved_vertices > 0 || remesh.total() > 0; }
};

class DynamicSculptor {
   public:
    explicit DynamicSculptor(DynamicSurface& surface, const DynamicSculptOptions& options = {});

    // ONE STAMP: remesh where the verb's timing says, deform through the shared
    // kernels, recompute the normals of what moved, and keep the index in step.
    //
    // `gate` is the freeze, taken exactly as the fixed path takes one, so a
    // painted mask protects a surface from every verb on both representations
    // rather than on a hand-picked few.
    //
    // `record`, when given, accumulates the whole gesture — the deformation AND
    // the topology — into one reversible step.
    DynamicStampResult stamp(MeshBrush verb, const MeshBrushSettings& brush,
                             const DynamicTopologySettings& topology,
                             const field::MaskGate& gate = {}, TopologyDelta* record = nullptr);

    const DynamicSurface& surface() const { return surface_; }
    DynamicSurface& surface() { return surface_; }
    const DynamicBvh& bvh() const { return bvh_; }
    DynamicBvh& bvh() { return bvh_; }

    // Rebuild the chunked index from scratch. BETWEEN STROKES, never mid-drag,
    // for the reason `DynamicBvh::wants_rebuild` records.
    void rebuild_index();

    // The workset the last stamp gathered, for a caller inspecting reach.
    const std::vector<VertexId>& last_region() const { return region_vertices_; }

   private:
    // Everything under the brush, by stable id, with the weights composed.
    bool gather(const MeshBrushSettings& brush, const field::MaskGate& gate, bool geodesic);
    void build_neighbors(bool want_normals, bool want_colors);
    SculptSnapshot snapshot_of() const;
    SculptNeighbors neighbors_of() const;
    std::size_t write_positions(TopologyDelta* record);
    std::size_t write_colors(TopologyDelta* record);
    // The surface walk: Dijkstra over the mutable one-ring, bounded by the ball
    // AND by a path budget, which is what keeps a brush on the upper lip from
    // dragging the chin through a closed mouth.
    void geodesic_region(kernel::cfloat3 centre, float radius, VertexId seed);
    void euclidean_region(kernel::cfloat3 centre, float radius);

    DynamicSurface& surface_;
    DynamicSculptOptions options_;
    DynamicBvh bvh_;

    // The workset, parallel arrays by stable id — the same shape the fixed
    // sculptor's `SculptWorkset` has, so the kernels see one thing.
    std::vector<VertexId> region_vertices_;
    std::vector<float> region_weights_;
    std::vector<kernel::cfloat3> region_positions_;
    std::vector<kernel::cfloat3> region_normals_;
    std::vector<float> region_distance_;
    kernel::cfloat3 average_normal_ = kernel::cf3(0, 1, 0);
    kernel::cfloat3 centroid_ = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_point_ = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal_ = kernel::cf3(0, 1, 0);

    // vertex slot -> index in the workset, kNoClass outside. Reset through the
    // workset rather than cleared, so a stamp costs what it reached.
    std::vector<std::uint32_t> slot_;

    std::vector<std::uint32_t> nb_offsets_, nb_slots_;
    std::vector<kernel::cfloat3> nb_positions_, nb_normals_, nb_colors_;

    std::vector<kernel::cfloat3> displacement_;
    std::vector<kernel::cfloat3> color_target_, color_current_;
    SculptScratch scratch_;

    // Walk scratch, kept so a stroke does not reallocate per stamp.
    std::vector<float> walk_distance_;
    std::vector<std::uint32_t> walk_dirty_;
    std::vector<std::pair<float, std::uint32_t>> walk_frontier_;
    std::vector<FaceId> touched_faces_;
    std::vector<VertexId> ring_scratch_;
    std::vector<HalfEdgeId> fan_scratch_;
};

}  // namespace mesh
}  // namespace clay
