#include "clay/mesh/project.h"

#include <algorithm>
#include <cmath>

#include "multires_internal.h"

namespace clay {
namespace mesh {
namespace {

struct Landing {
    bool found = false;
    bool by_ray = false;
    kernel::cfloat3 point = kernel::cf3(0, 0, 0);
    float distance = 0.0f;
};

// The nearest hit along the vertex's own normal, IN BOTH DIRECTIONS. Both,
// because a cage sits inside the sculpt in some places and outside it in
// others, and a one-sided cast would fail exactly on the half of the model that
// happens to be on the wrong side.
Landing along_normal(const Bvh& index, kernel::cfloat3 origin, kernel::cfloat3 normal,
                     float limit) {
    Landing best;
    const float reach = limit > 0.0f ? limit : 1e30f;
    for (int sign = 0; sign < 2; ++sign) {
        math::Ray ray;
        ray.origin = origin;
        ray.dir = sign == 0 ? normal : -normal;
        const Bvh::RayHit hit = index.raycast(ray, 0.0f, reach);
        if (!hit.hit) continue;
        if (best.found && hit.t >= best.distance) continue;
        best.found = true;
        best.by_ray = true;
        best.distance = hit.t;
        best.point = ray.at(hit.t);
    }
    return best;
}

// Where one vertex lands: the ray along its own normal if that finds the
// reference, and the closest point where it does not.
Landing find_landing(const Bvh& index, kernel::cfloat3 p, const kernel::cfloat3* normal,
                     float max_distance) {
    Landing landing;
    if (normal) landing = along_normal(index, p, *normal, max_distance);
    if (landing.found) return landing;
    const Bvh::ClosestPoint c = index.closest(p);
    if (!c.found || (max_distance > 0.0f && c.distance > max_distance)) return landing;
    landing.found = true;
    landing.by_ray = false;
    landing.point = c.point;
    landing.distance = c.distance;
    return landing;
}

}  // namespace

ProjectReport project_surface(const Mesh& reference, const std::vector<kernel::cfloat3>& normals,
                              std::vector<kernel::cfloat3>* positions,
                              const ProjectOptions& options, const Bvh* index,
                              const parallel::CancelToken* cancel) {
    ProjectReport report;
    if (!positions || positions->empty() || reference.indices.empty()) return report;

    Bvh owned;
    if (!index) {
        owned = Bvh::build(reference);
        index = &owned;
    }
    if (index->empty()) return report;

    const bool use_ray = options.normal_ray_first && normals.size() == positions->size();
    const float strength = std::max(0.0f, std::min(1.0f, options.strength));
    double sum = 0.0;

    for (std::size_t v = 0; v < positions->size(); ++v) {
        if (cancel && (v & 0xffu) == 0u && cancel->cancelled()) {
            report.cancelled = true;
            return report;
        }
        const kernel::cfloat3 p = (*positions)[v];
        const Landing landing =
            find_landing(*index, p, use_ray ? &normals[v] : nullptr, options.max_distance);
        if (!landing.found) {
            // LEFT WHERE IT WAS. A vertex with no correspondence inside the
            // stated distance is information; snapping it to whatever the tree
            // happened to hold would hide the fact that this part of the
            // surface has no counterpart.
            ++report.missed;
            continue;
        }
        (*positions)[v] = p + (landing.point - p) * strength;
        ++report.moved;
        if (landing.by_ray)
            ++report.by_ray;
        else
            ++report.by_closest;
        const float offset = clength((*positions)[v] - p);
        report.max_offset = std::max(report.max_offset, offset);
        sum += offset;
    }
    if (report.moved > 0) report.mean_offset = sum / static_cast<double>(report.moved);
    return report;
}

bool MultiresSurface::project_from(const Mesh& reference, const ProjectOptions& options,
                                   ProjectReport* out_report,
                                   const parallel::CancelToken* cancel) {
    if (out_report) *out_report = ProjectReport{};
    if (!state_ || state_->levels.size() < 2 || reference.indices.empty()) return false;

    const Bvh index = Bvh::build(reference);
    if (index.empty()) return false;

    ProjectReport total;
    double offset_sum = 0.0;
    std::vector<kernel::cfloat3> projected, ray_normals;
    for (std::uint32_t l = 1; l < state_->levels.size(); ++l) {
        if (cancel && cancel->cancelled()) {
            if (out_report) *out_report = total;
            return false;
        }
        MultiresLevel& lev = state_->levels[l];
        // Zero the level and rebuild it, so what we project FROM is the pure
        // subdivision of an already-fitted parent rather than the old detail
        // laid over it.
        lev.detail.reset(lev.topology.vertex_count);
        state_->levels[l - 1].pending_all = true;
        evaluate_up_to(*state_, l);

        projected = positions_at(l);
        const std::vector<SurfaceFrame>& frames = frames_at(l);
        ray_normals.resize(frames.size());
        for (std::size_t v = 0; v < frames.size(); ++v) ray_normals[v] = frames[v].normal;

        const ProjectReport r =
            project_surface(reference, ray_normals, &projected, options, &index, cancel);
        if (r.cancelled) {
            if (out_report) *out_report = total;
            return false;
        }
        total.moved += r.moved;
        total.missed += r.missed;
        total.by_ray += r.by_ray;
        total.by_closest += r.by_closest;
        total.max_offset = std::max(total.max_offset, r.max_offset);
        offset_sum += r.mean_offset * static_cast<double>(r.moved);

        const std::vector<kernel::cfloat3>& sub = subdivided_at(l);
        for (std::uint32_t v = 0; v < lev.topology.vertex_count; ++v) {
            LocalDetail d;
            world_to_frame(frames[v], projected[v] - sub[v], &d.tangent, &d.bitangent, &d.normal);
            lev.detail.set(v, d);
        }
        // Rebuild with the new coefficients, so the level ABOVE subdivides the
        // fitted surface and not the one we started from — and so that what the
        // hierarchy holds is what a reload would reconstruct rather than the
        // projected points we happened to compute.
        state_->levels[l - 1].pending_all = true;
        evaluate_up_to(*state_, l);
    }
    if (total.moved > 0) total.mean_offset = offset_sum / static_cast<double>(total.moved);
    ++state_->detail_revision;
    ++state_->evaluated_revision;
    if (out_report) *out_report = total;
    return true;
}

}  // namespace mesh
}  // namespace clay
