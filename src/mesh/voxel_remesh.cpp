// Global voxel remeshing (meshing spec, add-voxel-remesher).
//
// The pipeline and the decisions between the stages. The stages themselves are
// in voxel_remesh_sample.cpp (the sparse domain) and voxel_remesh_finish.cpp
// (everything done to the mesh after the lattice).

#include "clay/mesh/voxel_remesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "clay/mesh/transfer.h"
#include "clay/mesh/validate.h"
#include "voxel_remesh_internal.h"

namespace clay {
namespace mesh {

using remesh_detail::Lattice;

namespace {

// The safety factor on the memory estimate. Measured against the pieces that
// are easy to forget rather than guessed: the marching builder's edge map and
// its vertex/index growth, the component labelling, the projection's normals
// and its moved-position copy. Under-estimating is the failure that matters —
// it lets a request through that then dies in the allocator — so this errs
// high, and the report carries the ACTUAL active sample count so a host can
// see how far off the estimate was.
constexpr double kMemorySafetyFactor = 1.5;

// A remesh's working set, before the safety factor.
//
// Everything here is proportional to something the estimate already knows, and
// nothing is proportional to the runtime, which is why this is honest about
// memory and says nothing about time.
std::uint64_t working_bytes(const Lattice& lattice, std::uint64_t active_samples,
                            std::uint64_t source_triangles, std::uint64_t result_triangles) {
    const std::uint64_t bricks = lattice.brick_count();
    std::uint64_t bytes = 0;
    bytes += active_samples * sizeof(float);   // the stored narrow band
    bytes += bricks * (sizeof(std::int32_t) +  // FieldVolume::index_
                       sizeof(float) +         // FieldVolume::far_
                       2);                     // the active marking and its flood's seen flags
    // The BVH: a triangle carries three positions and a source index, and the
    // tree holds roughly one node per two triangles at a comfortable leaf size.
    bytes += source_triangles * (sizeof(kernel::cfloat3) * 3 + sizeof(std::uint32_t));
    bytes += source_triangles * 96;
    // The result: a position, a normal and three indices per triangle, at the
    // roughly one-vertex-per-triangle a marched isosurface produces.
    bytes += result_triangles * (sizeof(kernel::cfloat3) * 2 + sizeof(std::uint32_t) * 3);
    return bytes;
}

// The triangle count a marched isosurface of this area at this spacing tends to
// produce.
//
// The crossed cells are the surface's area over a cell's face area. Marching
// tetrahedra decomposes each cell into SIX tetrahedra and a crossed tetrahedron
// emits one triangle or two, so a crossed cell is worth up to twelve — which is
// why this range is wide, and why a host should use it to warn rather than to
// allocate. Measured across the fixtures the ratio sits near seven; the bounds
// are two and twelve so that neither end is a number one shape happens to hit.
void estimate_triangles(double area, float voxel_size, std::uint64_t* out_min,
                        std::uint64_t* out_max) {
    const double face = static_cast<double>(voxel_size) * static_cast<double>(voxel_size);
    const double crossed = face > 0.0 ? area / face : 0.0;
    *out_min = static_cast<std::uint64_t>(std::max(0.0, crossed * 2.0));
    *out_max = static_cast<std::uint64_t>(std::max(0.0, crossed * 12.0));
}

bool validate_params(const VoxelRemeshParams& p, VoxelRemeshStatus* out) {
    if (p.build_multires_levels != 0) {
        // Refused rather than ignored. The mesh multires architecture is not
        // settled, and a second multires system built for this operation is
        // exactly what should not happen.
        *out = VoxelRemeshStatus::Unsupported;
        return false;
    }
    const bool bad = !std::isfinite(p.projection_strength) || p.projection_strength < 0.0f ||
                     p.projection_strength > 1.0f ||
                     !std::isfinite(p.max_projection_distance_voxels) ||
                     p.max_projection_distance_voxels < 0.0f ||
                     !std::isfinite(p.minimum_component_volume) ||
                     p.minimum_component_volume < 0.0f;
    if (bad) {
        *out = VoxelRemeshStatus::InvalidParameters;
        return false;
    }
    return true;
}

double relative_error(double a, double b) {
    const double scale = std::max(std::abs(a), 1e-12);
    return std::abs(a - b) / scale;
}

// Whether the source's attribute arrays are the length they claim to be. A
// malformed array is treated as ABSENT rather than read: the alternative is
// reading past a caller's buffer because it agreed with itself about a length
// nobody checked.
bool source_has_colors(const Mesh& m) {
    return !m.colors.empty() && m.colors.size() == m.positions.size();
}

// The wall clock, per stage.
//
// Diagnostics and not a contract: nothing about the RESULT depends on a
// reading, which is why the determinism test compares the mesh and never the
// report. A host asking "where did my four seconds go" has no other way to find
// out, because the stages are not separately callable.
class StageClock {
  public:
    explicit StageClock(VoxelRemeshReport* report) : report_(report) {}
    // Close the stage that was running and open `next`.
    void enter(VoxelRemeshStage next) {
        const auto now = std::chrono::steady_clock::now();
        close(now);
        open_ = next;
        started_ = now;
        running_ = true;
    }
    ~StageClock() { close(std::chrono::steady_clock::now()); }
    StageClock(const StageClock&) = delete;
    StageClock& operator=(const StageClock&) = delete;

  private:
    void close(std::chrono::steady_clock::time_point now) {
        if (!running_) return;
        const auto index = static_cast<std::size_t>(open_);
        if (index < kVoxelRemeshStageCount)
            report_->stage_ms[index] +=
                std::chrono::duration<double, std::milli>(now - started_).count();
        running_ = false;
    }
    VoxelRemeshReport* report_;
    VoxelRemeshStage open_ = VoxelRemeshStage::Preflight;
    std::chrono::steady_clock::time_point started_{};
    bool running_ = false;
};

// Every stage ends the same way, and spelling it out at each of them was six
// chances to set the status and forget the report's flag. A cancelled remesh
// has to be distinguishable from every other empty result, and the flag is how.
bool stopped(const parallel::ProgressScope& progress, VoxelRemeshResult* result) {
    if (!progress.cancelled()) return false;
    result->status = VoxelRemeshStatus::Cancelled;
    result->report.cancelled = true;
    return true;
}

// What the run can decide from the SOURCE alone: the lattice, the numbers the
// report carries about the input, and the two refusals that do not need a tree
// — a resolution that is not a number, and an open surface under a policy that
// rejects one.
//
// Separated from the pipeline because it is where the answer is "no" most
// often, and a stage list with four early returns threaded through it reads as
// four stages rather than one.
bool preflight(const Mesh& source, const VoxelRemeshParams& params, Lattice* lattice,
               VoxelRemeshResult* result) {
    if (!validate_params(params, &result->status)) return false;
    if (source.triangle_count() == 0) {
        result->status = VoxelRemeshStatus::EmptySource;
        return false;
    }
    if (!remesh_detail::resolve_lattice(source, params, lattice, &result->status)) return false;

    VoxelRemeshReport& report = result->report;
    report.voxel_size = lattice->voxel_size;
    report.source_vertices = source.positions.size();
    report.source_triangles = source.triangle_count();
    report.source_volume = signed_volume(source);
    remesh_detail::count_boundaries_and_components(source, &report.source_boundary_edges,
                                                   &report.source_components);
    report.source_was_open = report.source_boundary_edges > 0;

    if (report.source_was_open &&
        params.open_surface_policy == VoxelRemeshOpenSurfacePolicy::Reject) {
        result->status = VoxelRemeshStatus::OpenSurfaceRejected;
        return false;
    }
    return true;
}

// The volume comparison and the paint, which are one stage because both are
// read from the SOURCE against a result that already has its final geometry.
//
// Volume correction is skipped where the comparison stopped meaning anything: a
// hole closed under policy adds material the source never had, and a removed
// component takes material away, so scaling toward the source's volume in
// either case distorts a surface to chase a number that is no longer about it.
void measure_and_repaint(const Mesh& source, const Lattice& lattice,
                         const VoxelRemeshParams& params, Mesh* out,
                         VoxelRemeshReport* report) {
    report->result_volume = signed_volume(*out);
    const bool topology_shifted = report->source_was_open || report->removed_components > 0;
    if (params.preserve_volume && !topology_shifted) {
        report->volume_corrected =
            remesh_detail::correct_volume(out, report->source_volume, report->result_volume);
        if (report->volume_corrected) report->result_volume = signed_volume(*out);
    }
    report->relative_volume_error = relative_error(report->source_volume, report->result_volume);

    report->uvs_dropped = !source.uvs.empty();
    if (params.preserve_colors && source_has_colors(source)) {
        TransferOptions options;
        options.colors = true;
        // Dropped, not reprojected. A UV resampled across a seam is a stretched
        // layout that looks like a preserved one, and the requirement says
        // dropped for exactly that reason.
        options.uvs = false;
        options.normals = false;
        // Two voxels: a reconstructed vertex further than that from the source
        // is standing on geometry the source did not have — a closed hole, a
        // bridged gap — and the colour there would be borrowed from somewhere
        // it does not belong.
        options.max_distance = lattice.voxel_size * 2.0f;
        report->colors_transferred = transfer_attributes(source, out, options).colors;
    }
    // The result's own normals, from the result's own geometry — after
    // projection moved it. Taking the source's would make new geometry shade
    // like the old shape.
    out->normals = vertex_normals(*out);
}

// Whether this request may proceed to allocate.
//
// Checked AFTER the marking, which costs one byte per brick, and BEFORE the
// tree, the band and the result mesh, which are the allocations that matter. A
// guard that ran after them would be a comment.
bool within_budget(const Mesh& source, const VoxelRemeshParams& params, const Lattice& lattice,
                   std::uint64_t active_samples, std::uint64_t* out_bytes) {
    // Computed whatever the caller's budget is, because the REPORT carries it:
    // a host that wants to see how close it came needs the number even on the
    // runs that were never in danger.
    std::uint64_t triangle_min = 0, triangle_max = 0;
    estimate_triangles(surface_area(source), lattice.voxel_size, &triangle_min, &triangle_max);
    *out_bytes = static_cast<std::uint64_t>(
        static_cast<double>(working_bytes(lattice, active_samples, source.triangle_count(),
                                          triangle_max)) *
        kMemorySafetyFactor);
    if (active_samples > kMaxVoxelRemeshActiveSamples) return false;
    if (params.memory_budget_bytes == 0) return true;
    return *out_bytes <= params.memory_budget_bytes;
}

}  // namespace

VoxelRemeshEstimate voxel_remesh_estimate(const Mesh& source, const VoxelRemeshParams& params) {
    VoxelRemeshEstimate est;
    if (!validate_params(params, &est.status)) return est;
    if (source.triangle_count() == 0) {
        est.status = VoxelRemeshStatus::EmptySource;
        return est;
    }

    Lattice lattice;
    if (!remesh_detail::resolve_lattice(source, params, &lattice, &est.status)) return est;

    est.resolved_voxel_size = lattice.voxel_size;
    for (int a = 0; a < 3; ++a)
        est.grid_dimensions[a] =
            static_cast<std::uint32_t>(lattice.bcount[a] * field::kBrickDim);

    const std::vector<std::uint8_t> active =
        remesh_detail::mark_active_bricks(source, lattice, nullptr);
    est.estimated_active_samples =
        remesh_detail::count_active(active) * static_cast<std::uint64_t>(field::kBrickSamples);

    estimate_triangles(surface_area(source), lattice.voxel_size, &est.estimated_triangle_min,
                       &est.estimated_triangle_max);
    remesh_detail::count_boundaries_and_components(source, &est.boundary_edge_count,
                                                   &est.component_count);
    est.has_open_boundaries = est.boundary_edge_count > 0;

    est.estimated_memory_bytes = static_cast<std::uint64_t>(
        static_cast<double>(working_bytes(lattice, est.estimated_active_samples,
                                          source.triangle_count(),
                                          est.estimated_triangle_max)) *
        kMemorySafetyFactor);

    // The thin-feature probe needs the tree, and the tree is the one thing here
    // that costs the model rather than the estimate. It is built anyway on the
    // path that matters — a host calls estimate then remesh — and skipping it
    // would mean the warning that stops an artist losing a feature is the one
    // number this does not supply.
    const Bvh bvh = Bvh::build(source);
    est.thin_feature_warning =
        remesh_detail::has_thin_features(source, bvh, lattice.voxel_size, 2.0f);

    if (est.estimated_active_samples > kMaxVoxelRemeshActiveSamples) {
        est.exceeds_memory_budget = true;
        est.status = VoxelRemeshStatus::ExceedsBudget;
    } else if (params.memory_budget_bytes != 0 &&
               est.estimated_memory_bytes > params.memory_budget_bytes) {
        est.exceeds_memory_budget = true;
        est.status = VoxelRemeshStatus::ExceedsBudget;
    }
    return est;
}

VoxelRemeshResult voxel_remesh(const Mesh& source, const VoxelRemeshParams& params,
                               parallel::CancelToken* token) {
    VoxelRemeshResult result;
    parallel::ProgressScope progress(token, kVoxelRemeshStageCount);
    StageClock clock(&result.report);

    // -- preflight ----------------------------------------------------------
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::Preflight));
    clock.enter(VoxelRemeshStage::Preflight);
    Lattice lattice;
    if (!preflight(source, params, &lattice, &result)) return result;
    VoxelRemeshReport& report = result.report;

    const std::vector<std::uint8_t> active =
        remesh_detail::mark_active_bricks(source, lattice, token);
    if (stopped(progress, &result)) return result;

    // The marking is an UPPER BOUND on the band: a marked brick is one whose
    // box comes within the band of a triangle's AABB, and some of those turn
    // out to hold no sample near enough to keep. That bound is what the budget
    // is enforced against, because the budget has to be decided before the
    // samples exist. What the REPORT carries is the count actually stored, so
    // a host can see how far the estimate ran ahead of the run.
    const std::uint64_t active_samples =
        remesh_detail::count_active(active) * static_cast<std::uint64_t>(field::kBrickSamples);
    if (!within_budget(source, params, lattice, active_samples, &report.estimated_memory_bytes)) {
        result.status = VoxelRemeshStatus::ExceedsBudget;
        return result;
    }

    // -- the tree -----------------------------------------------------------
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::SourceAcceleration));
    clock.enter(VoxelRemeshStage::SourceAcceleration);
    const Bvh bvh = Bvh::build(source);
    if (stopped(progress, &result)) return result;

    // -- the field ----------------------------------------------------------
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::Sampling),
                   lattice.brick_count());
    clock.enter(VoxelRemeshStage::Sampling);
    bool cancelled = false;
    const field::FieldVolume volume =
        remesh_detail::sample_sparse(bvh, lattice, active, token, &cancelled);
    if (cancelled) {
        result.status = VoxelRemeshStatus::Cancelled;
        report.cancelled = true;
        return result;
    }
    if (stopped(progress, &result)) return result;
    report.active_samples = volume.sample_count();
    if (volume.empty()) {
        // No brick held a crossing. A source whose triangles are all further
        // from every sample than the band is one whose features are finer than
        // the resolution can see at all.
        result.status = VoxelRemeshStatus::ExtractionFailed;
        return result;
    }

    // -- extraction ---------------------------------------------------------
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::Extraction));
    clock.enter(VoxelRemeshStage::Extraction);
    Mesh out = remesh_detail::extract_surface(volume, lattice, params.surface_mode);
    if (stopped(progress, &result)) return result;
    if (out.indices.empty()) {
        result.status = VoxelRemeshStatus::ExtractionFailed;
        return result;
    }

    if (params.small_component_policy == VoxelRemeshSmallComponentPolicy::RemoveBelowVolume)
        report.removed_components = remesh_detail::remove_small_components(
            &out, static_cast<double>(params.minimum_component_volume));

    // -- projection ---------------------------------------------------------
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::Projection),
                   out.positions.size());
    clock.enter(VoxelRemeshStage::Projection);
    if (params.project_to_source) {
        report.projected_vertices = remesh_detail::project_to_source(
            &out, source, bvh, lattice.voxel_size, params.projection_strength,
            params.max_projection_distance_voxels, token);
        report.projected_to_source = true;
    }
    if (stopped(progress, &result)) return result;

    // -- volume and attributes ----------------------------------------------
    measure_and_repaint(source, lattice, params, &out, &report);
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::AttributeTransfer),
                   out.positions.size());
    clock.enter(VoxelRemeshStage::AttributeTransfer);
    if (stopped(progress, &result)) return result;

    // -- validation ---------------------------------------------------------
    progress.phase(static_cast<std::uint32_t>(VoxelRemeshStage::Validation));
    clock.enter(VoxelRemeshStage::Validation);
    const remesh_detail::SurfaceError error = remesh_detail::measure_surface_error(out, bvh);
    report.result_to_source_rms = error.rms;
    report.result_to_source_p95 = error.p95;
    report.result_to_source_max = error.max;

    const ValidationReport check = validate(out);
    report.result_vertices = out.positions.size();
    report.result_triangles = out.triangle_count();
    report.result_boundary_edges = static_cast<std::uint32_t>(check.boundary_edges);
    report.result_watertight = check.watertight;
    report.result_manifold = check.manifold;
    report.result_oriented = check.oriented;
    {
        std::vector<std::uint32_t> label;
        report.result_components = remesh_detail::label_components(out, &label);
    }

    // The contract is claimed for the default mode only: dual contouring is
    // flagged in the meshing spec and is not guaranteed manifold, so Sharp is
    // returned as it is with the report saying what it is. BestEffort likewise
    // hands back what it got, having already said the source was open.
    const bool enforce = params.surface_mode == VoxelRemeshSurfaceMode::Smooth &&
                         params.open_surface_policy != VoxelRemeshOpenSurfacePolicy::BestEffort;
    if (enforce && !(check.watertight && check.manifold && check.oriented &&
                     check.degenerate_triangles == 0)) {
        result.status = VoxelRemeshStatus::ResultNotWatertight;
        return result;
    }

    result.mesh = std::move(out);
    result.status = VoxelRemeshStatus::Ok;
    return result;
}

}  // namespace mesh
}  // namespace clay
