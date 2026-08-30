#pragma once

// Global voxel remeshing (meshing spec, add-voxel-remesher).
//
// The operation an artist calls DynaMesh: take a polygonal surface, rebuild it
// through a volumetric representation at an explicit spatial resolution, fuse
// what overlaps, close what policy says to close, and hand back a completely
// new surface at approximately uniform density.
//
// Every piece of this already existed — `Bvh` answers distance and sign,
// `FieldVolume` holds the narrow band, `mesh_lattice` marches a watertight
// isosurface, `validate` checks it and `transfer_attributes` repaints it. What
// did not exist is the OPERATION, and with it the decisions the pieces do not
// make: what resolution means, what an open surface becomes, what a request
// costs before it is made, what is guaranteed about the result, and what "the
// same input twice" produces. Those are geometry semantics, so they live here
// rather than in whatever host assembles the primitives.
//
// WHAT THIS IS NOT. Not `MeshSculptor` — that layer's whole value is that
// `indices` and `quads` come out byte-identical, and this replaces both. Not
// `DynamicSurface`'s local remesh — that adapts topology under a brush, this
// discards all of it at once. Not decimation — that preserves the surface's
// triangles and this destroys them. Not quad retopology: the output is a
// lattice-derived triangulation with no edge loops following the form.
//
// WHAT IT COSTS. Vertex identity, polygon identity and UVs are gone, and any
// feature thinner than the voxel size may be gone with them. Those are stated
// in the report and in the estimate rather than left to be discovered.

#include <cstdint>
#include <vector>

#include "clay/mesh/mesh_data.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace mesh {

// How a caller spells the resolution.
enum class VoxelRemeshResolutionMode : std::uint8_t {
    // World units. THE CANONICAL FORM: it is what decides which features
    // survive, and it means the same thing whatever the model's size.
    VoxelSize,
    // The convenience an artist actually turns: the longest bounding extent
    // divided by this integer, resolved before any sampling padding, so the
    // number does not drift when the padding does.
    LongestAxisResolution,
};

enum class VoxelRemeshSurfaceMode : std::uint8_t {
    // Marching tetrahedra: watertight and 2-manifold by construction.
    Smooth,
    // Dual contouring, which the meshing spec keeps flagged and which is not
    // guaranteed manifold. EXPERIMENTAL: the result contract below is not
    // claimed for it.
    Sharp,
};

// What to do about a source with open boundaries. There is no default that is
// right for everybody and no silent answer that is honest, so this is explicit.
enum class VoxelRemeshOpenSurfacePolicy : std::uint8_t {
    Reject,      // typed failure, no mesh
    Close,       // the closed volumetric interpretation; validated
    BestEffort,  // proceed and report what the result actually is
};

enum class VoxelRemeshSmallComponentPolicy : std::uint8_t {
    // The default, and it is a judgement rather than laziness: a floating
    // component is as likely to be a tooth, a lash or an armour plate as it is
    // to be debris, and nothing here can tell which.
    Preserve,
    RemoveBelowVolume,
};

// Why a remesh did not produce a mesh. Distinct rather than collapsed into one
// failure, because "lower the resolution", "the model has holes" and "you
// stopped it" are three different things for a host to say.
enum class VoxelRemeshStatus : std::uint8_t {
    Ok,
    EmptySource,          // no triangles: nothing to measure a surface from
    InvalidResolution,    // not finite, not positive, or zero
    InvalidParameters,    // a strength, distance or threshold out of range
    Unsupported,          // asked for something this version does not do
    ExceedsBudget,        // over the caller's budget or the library's ceiling
    OpenSurfaceRejected,  // boundaries, under Reject
    ExtractionFailed,     // the field produced no surface
    ResultNotWatertight,  // Smooth mode's own output failed validation
    Cancelled,
};

// The band and the padding, in voxels.
//
// Named rather than parameters. The band is the distance over which the stored
// field is a real distance rather than a bound, and three voxels is what the
// interpolation, the gradient and the extraction ring need; the padding is what
// keeps the band from being clipped where the surface meets the region's face.
// A caller has no way to choose them better than this and every way to choose
// them worse, so they are constants a test can reason about rather than dials.
inline constexpr float kVoxelRemeshBandVoxels = 3.0f;
inline constexpr float kVoxelRemeshPaddingVoxels = 4.0f;

// Ceilings the library enforces whatever the caller's budget is, so a runaway
// request returns rather than ending the process in the allocator — the library
// builds without exceptions, so an allocation that cannot be served is not a
// failure a caller can catch.
//
// kMaxActiveSamples is the stored narrow band: 2^28 floats is a gigabyte, which
// clears any resolution this API is meant to serve. kMaxLatticeCells bounds the
// brick index and the marched lattice, which are O(bounding box) even when the
// band is not — see the sparse-domain note on `voxel_remesh` below.
inline constexpr std::uint64_t kMaxVoxelRemeshActiveSamples = 1ull << 28;
inline constexpr std::uint64_t kMaxVoxelRemeshLatticeCells = 1ull << 30;

struct VoxelRemeshParams {
    VoxelRemeshResolutionMode resolution_mode = VoxelRemeshResolutionMode::LongestAxisResolution;

    float voxel_size = 0.0f;                 // read in VoxelSize mode
    std::uint32_t longest_axis_resolution = 256;  // read in LongestAxisResolution mode

    VoxelRemeshSurfaceMode surface_mode = VoxelRemeshSurfaceMode::Smooth;
    VoxelRemeshOpenSurfacePolicy open_surface_policy = VoxelRemeshOpenSurfacePolicy::Close;
    VoxelRemeshSmallComponentPolicy small_component_policy =
        VoxelRemeshSmallComponentPolicy::Preserve;

    // Read only under RemoveBelowVolume. In cubic world units.
    float minimum_component_volume = 0.0f;

    // A uniform correction about the result's centroid toward the source's
    // signed volume, clamped hard (see kVoxelRemeshMaxVolumeCorrection) and
    // skipped where the comparison stopped meaning anything — an open source
    // closed under policy, or components removed. The report carries both
    // volumes either way, so a caller can see what it did or did not do.
    bool preserve_volume = true;

    // Move each result vertex part of the way to the closest point on the
    // source. What gives a remesh back the detail the lattice rounded off.
    bool project_to_source = true;
    // How far of the way. A lerp, never a snap: at 1.0 a vertex lands exactly
    // on the source and the surface starts to inherit the source's noise.
    float projection_strength = 0.75f;
    // ...and not at all when the closest point is further than this many voxels
    // away. Geometry can exist where the source never was — a closed hole, a
    // bridged gap — and the closest point to it means nothing.
    float max_projection_distance_voxels = 1.5f;

    // Resampled spatially by closest point, never by vertex index. A source
    // carrying no colour produces a result carrying none: nothing here invents
    // one. UVs are DROPPED and there is no flag to keep them — see the note on
    // `VoxelRemeshReport::uvs_dropped`.
    bool preserve_colors = true;

    // Reserved. Non-zero is refused with Unsupported rather than ignored: the
    // mesh multires architecture is not settled, and a second multires system
    // built for this operation is exactly what should not happen.
    std::uint32_t build_multires_levels = 0;

    // Zero means "no caller budget", and the library's own ceilings still
    // apply. A request over this fails before the field, the tree or the result
    // is allocated.
    std::uint64_t memory_budget_bytes = 0;
};

// The clamp on the volume correction, as a fraction of the linear scale. Two
// per cent moves a 10 cm model by a millimetre at its extremes, which is the
// most a correction should be allowed to do to a surface it did not measure.
inline constexpr float kVoxelRemeshMaxVolumeCorrection = 0.02f;

// The stages, in order. Published so a host can name what it is waiting for
// rather than drawing an unlabelled bar; they are the phase indices the
// operation writes into the cancellation token's progress.
enum class VoxelRemeshStage : std::uint8_t {
    Preflight,
    SourceAcceleration,
    Sampling,
    Extraction,
    Projection,
    AttributeTransfer,
    Validation,
    Complete,
};
inline constexpr std::uint32_t kVoxelRemeshStageCount =
    static_cast<std::uint32_t>(VoxelRemeshStage::Complete);

// What a remesh would cost, before it is made.
//
// The point is not to predict runtime. It is to let a host put a number in
// front of an artist, and to refuse a request that would otherwise be answered
// by the allocator. Computing it walks the source's triangles and marks the
// brick lattice; it does not build the tree, sample the field or mesh anything.
struct VoxelRemeshEstimate {
    float resolved_voxel_size = 0.0f;
    std::uint32_t grid_dimensions[3] = {};
    // An UPPER BOUND on the samples the narrow band will store — the number
    // that follows the surface rather than the bounding box. A bound rather
    // than a prediction because it comes from the marking, which keeps every
    // brick whose box comes within the band of a triangle's AABB, and some of
    // those hold nothing near enough to keep. `VoxelRemeshReport::active_samples`
    // is what the run actually stored, and is never larger than this.
    std::uint64_t estimated_active_samples = 0;
    std::uint64_t estimated_memory_bytes = 0;
    std::uint64_t estimated_triangle_min = 0;
    std::uint64_t estimated_triangle_max = 0;
    std::uint32_t boundary_edge_count = 0;
    std::uint32_t component_count = 0;
    bool has_open_boundaries = false;
    // Sampled evidence that the source carries material thinner than a couple
    // of voxels, which is material this resolution may delete. A boolean in
    // this version; localising the at-risk regions needs a representation this
    // change did not want to invent.
    bool thin_feature_warning = false;
    bool exceeds_memory_budget = false;
    // Set when the request is refused on its own terms — a resolution that is
    // not a number, or a lattice past the library's ceiling. `Ok` here does not
    // promise the remesh will succeed, only that it will be attempted.
    VoxelRemeshStatus status = VoxelRemeshStatus::Ok;
};

struct VoxelRemeshReport {
    float voxel_size = 0.0f;

    std::uint64_t source_vertices = 0;
    std::uint64_t source_triangles = 0;
    std::uint64_t result_vertices = 0;
    std::uint64_t result_triangles = 0;

    // Signed, by the divergence theorem, so both are positive for outward
    // orientation and the relative difference is a real comparison rather than
    // a comparison of absolute values that happen to agree.
    double source_volume = 0.0;
    double result_volume = 0.0;
    double relative_volume_error = 0.0;

    std::uint32_t source_boundary_edges = 0;
    std::uint32_t result_boundary_edges = 0;
    std::uint32_t source_components = 0;
    std::uint32_t result_components = 0;
    std::uint32_t removed_components = 0;

    // The narrow-band samples the run actually STORED — never more than the
    // estimate's bound, and the number a host checks the estimate against.
    std::uint64_t active_samples = 0;

    bool source_was_open = false;
    bool result_watertight = false;
    bool result_manifold = false;
    bool result_oriented = false;

    bool projected_to_source = false;
    std::uint64_t projected_vertices = 0;  // ...and how many actually moved
    bool volume_corrected = false;

    bool colors_transferred = false;
    // Always true when the source carried UVs, and it is not a failure: a
    // spatially reprojected UV across a seam is a stretched layout that looks
    // like a preserved one, so this operation does not pretend to keep them.
    bool uvs_dropped = false;

    bool cancelled = false;

    // -- how far the result strayed from the source ------------------------
    //
    // ONE-SIDED, AND NAMED SO. Every result vertex against the source surface,
    // through the tree the operation already built — so this costs one
    // closest-point query per vertex and no second tree.
    //
    // What it CANNOT see is the other direction: a source feature the result
    // missed entirely leaves no result vertex near it to report, so a remesh
    // that deleted a spike scores well here. The estimate's thin-feature
    // warning is what answers that question, before the fact rather than
    // after, and `relative_volume_error` catches gross loss. A symmetric
    // distance would need a tree over the RESULT, which is the size of the
    // output and is not worth building for a diagnostic.
    //
    // In world units. p95 rather than a mean alone because a marched surface's
    // error is not normally distributed — it is small nearly everywhere and
    // concentrated where the lattice could not follow the source.
    double result_to_source_rms = 0.0;
    double result_to_source_p95 = 0.0;
    double result_to_source_max = 0.0;

    // Wall-clock per stage, indexed by VoxelRemeshStage. Diagnostics, not a
    // contract: they vary run to run, and nothing about the RESULT does. A host
    // showing "where did my four seconds go" needs them and has no other way to
    // get them, since the stages are not separately callable.
    double stage_ms[kVoxelRemeshStageCount] = {};

    // What the resource guard actually compared against the budget — the same
    // number `VoxelRemeshEstimate::estimated_memory_bytes` reports, recomputed
    // from the marking this run did rather than from a separate estimate call.
    //
    // NOT a measured peak. This library has no allocator hook and does not want
    // one; a caller who needs a true high-water mark measures it from outside,
    // which is what the repository's own allocation gate does.
    std::uint64_t estimated_memory_bytes = 0;
};

struct VoxelRemeshResult {
    VoxelRemeshStatus status = VoxelRemeshStatus::EmptySource;
    Mesh mesh;
    VoxelRemeshReport report;

    bool ok() const { return status == VoxelRemeshStatus::Ok; }
};

// What a remesh would cost. Cheap enough to call on every tick of a resolution
// slider: it walks the source's triangles once and marks a brick lattice, and
// it allocates nothing proportional to the sample count it is predicting.
VoxelRemeshEstimate voxel_remesh_estimate(const Mesh& source, const VoxelRemeshParams& params);

// Rebuild `source` through a signed narrow-band field at the requested spatial
// resolution.
//
// THE SAMPLING DOMAIN FOLLOWS THE SURFACE, which is the property that separates
// this from calling `to_field` and marching the answer. `to_field` evaluates the
// caller's function for every brick of the bounding box — 32^3 bricks of 729
// samples at longest-axis 256, each sample a BVH distance query with a
// generalized winding number, and 128^3 bricks at 1024. That is right for an
// import, where the caller chose the cell size for the model, and wrong for a
// resolution dial. So the bricks near a source triangle are marked and
// evaluated, and the rest are filled with the sign of the region they fall in,
// one winding query per connected region.
//
// The samples that are STORED come out bit-identical to what a dense evaluation
// of the same region would have stored — same bricks, same values — because a
// brick holding a sample within the band of the surface is necessarily within
// the band of some triangle, and so is necessarily marked. Sparsity here is an
// optimisation of one field, not a second field.
//
// The one place the two differ is a source with OPEN boundaries: the
// generalized winding number's half-crossing can fall away from every triangle,
// so a brick can straddle it while holding no sample near a triangle. The dense
// path records that brick's sign per brick and this records it per connected
// region. Only sample-FREE bricks are affected — neither path stores anything
// there — and the difference is stated rather than hidden because a caller
// comparing the two would otherwise find it themselves.
//
// `token` is optional. Cancellation is checked inside every expensive stage
// rather than between them, and a cancelled call returns `Cancelled` with no
// mesh. The source is a `const&` and is never written, so "cancellation leaves
// the source unchanged" is a property of this signature rather than a promise
// about a rollback.
//
// DETERMINISTIC. The same source and parameters produce a bit-identical mesh on
// every run: every parallel stage writes disjoint outputs computed from
// position-only inputs, so no scheduling can reorder a value into a different
// one.
VoxelRemeshResult voxel_remesh(const Mesh& source, const VoxelRemeshParams& params,
                               parallel::CancelToken* token = nullptr);

}  // namespace mesh
}  // namespace clay
