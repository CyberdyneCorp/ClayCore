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
#include "clay/mesh/sculpt_workset.h"
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
    //
    // PROJECTED OUT OF THE WORKSET rather than stored beside it: a caller
    // asking the adaptive sculptor what it reached wants the adaptive surface's
    // own handles, and the workset now addresses work by the neutral
    // `WorkItemId` that all three representations share.
    const std::vector<VertexId>& last_region() const { return last_region_; }
    // The whole workset, including entries the falloff kept and the verb did
    // not move.
    const SculptWorkset& workset() const { return region_; }

    // THE AUTOMASK INPUTS `mesh` MAY NOT COMPUTE FOR ITSELF — the cavity
    // estimator and the surface-group field, both of which live in modules that
    // depend on this one. Set once for a STROKE: they hold `std::function`s,
    // and copying those per stamp would allocate on every dab.
    //
    // THE SAME SIGNATURE `MeshSculptor` HAS, deliberately. Before
    // add-shared-brush-runtime this class had no such call and
    // `DynamicSculptor::gather` never read `brush.automask` at all — so an
    // automask an artist enabled was silently absent on the adaptive
    // representation, although `clay_dynamic_sculptor_stamp` takes the same
    // descriptor the fixed path takes and its four automask fields were being
    // filled and dropped. That is the divergence this class was changed to
    // close, and a host that was already setting factors will now see them take
    // effect.
    void set_automask_inputs(AutomaskInputs inputs) { automask_inputs_ = std::move(inputs); }
    const AutomaskInputs& automask_inputs() const { return automask_inputs_; }

    // What the per-stamp scratch arena owns and how far it has had to grow.
    // One per sculptor and never a process-global — see `brush_arena.h`.
    const BrushScratchArena& arena() const { return arena_; }

   private:
    // THE WALK, and only the walk: everything the brush reaches, into
    // `candidates_` and `region_distance_`. Declared here rather than in
    // `sculpt_workset.h` because it names a `DynamicSurface`, and a neutral
    // header holding three representation-specific signatures is neutral in the
    // directory listing only.
    void build_dynamic_surface_workset(const MeshBrushSettings& brush, bool geodesic);
    // The determinism sort both region walks end in, over arena scratch.
    void sort_candidates_by_slot();
    // Everything under the brush, by stable id, with the weights composed.
    bool gather(const MeshBrushSettings& brush, const field::MaskGate& gate, bool geodesic);
    // The brush's own facing for the normal-angle automask, fixed for the
    // stamp and never taken from the region — the region's average normal is
    // weighted by the very weights the automask is shaping.
    kernel::cfloat3 automask_reference(const MeshBrushSettings& brush);
    // The corner of the closest face nearest `p` — the adaptive counterpart of
    // `MeshSculptor::nearest_class`, shared by the walk's seed, the
    // connectivity automask's anchor and the fallback facing.
    VertexId nearest_vertex(kernel::cfloat3 p) const;
    // The two answers `compose_workset` cannot work out for itself. Function
    // pointers with a `this` context, for the reason `WorkItemReader` gives.
    static kernel::cfloat3 normal_of_item(const void* context, WorkItemId item);
    static float mask_of_item(const void* context, WorkItemId item);
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

    // THE SHARED WORKSET, not five parallel arrays of its own. It used to be
    // five — vertices, weights, positions, normals, distances — plus a private
    // slot map, which is the same shape `SculptWorkset` has and was the reason
    // the automask could not reach here: a workset typed in the adaptive
    // surface's own handles is a workset only this class can read. Its `slot`
    // array is keyed by vertex slot, which is what `WorkItemId::key()` returns
    // for a surface vertex.
    SculptWorkset region_;
    // The walk's own output, before the composition turns it into items. A
    // member so a stroke of similar stamps allocates on its first stamp only.
    std::vector<VertexId> candidates_;
    std::vector<float> region_distance_;
    // `last_region()`'s answer, projected out of the workset once per stamp.
    std::vector<VertexId> last_region_;
    // The transients one stamp needs deep inside the call: the region sort's
    // permutation and its two sorted copies, and the automask's frontiers.
    BrushScratchArena arena_;
    AutomaskInputs automask_inputs_;
    // The vertex the last gather anchored on, shared by the connectivity
    // automask and the normal-angle reference so the two cannot disagree about
    // where the brush landed.
    VertexId automask_seed_;

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
    // The ball query's own answer. A member for the same reason: it used to be
    // a local in `euclidean_region` and allocated on every dab of every
    // flatten and scrape.
    std::vector<FaceId> ball_faces_;
    std::vector<VertexId> ring_scratch_;
    std::vector<HalfEdgeId> fan_scratch_;
    // The buffers `DynamicSurface::refresh_normals` would otherwise build for
    // itself, once per stamp plus one half-edge fan per vertex it touched.
    DynamicSurface::NormalRefreshScratch normal_scratch_;
};

}  // namespace mesh
}  // namespace clay
