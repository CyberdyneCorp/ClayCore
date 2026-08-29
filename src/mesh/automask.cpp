#include "clay/mesh/automask.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/sculpt_workset.h"

namespace clay {
namespace mesh {

namespace {

// How many triangles two classes share. A manifold interior edge is shared by
// two; an open border by one.
int shared_triangles(const Mesh& mesh, const Adjacency& adj, std::uint32_t a, std::uint32_t b) {
    std::size_t n = 0;
    const std::uint32_t* tris = adj.triangles_of(a, &n);
    int shared = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bool has_b = false;
        for (int corner = 0; corner < 3; ++corner) {
            const std::size_t at = static_cast<std::size_t>(tris[i]) * 3 +
                                   static_cast<std::size_t>(corner);
            if (at >= mesh.indices.size()) continue;
            if (adj.class_of(mesh.indices[at]) == b) has_b = true;
        }
        if (has_b) ++shared;
    }
    return shared;
}

// The two-sided fade every gate in this file uses: full strength up to `at`,
// zero at twice it, smoothstepped between. A gate that steps from 1 to 0 leaves
// a bead of protected vertices beside a fully worked one, which polish already
// learned the hard way.
float fade(float value, float at) {
    const float a = std::max(at, 1e-4f);
    const float t = std::clamp((value - a) / a, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

}  // namespace

bool is_boundary_class(const Mesh& mesh, const Adjacency& adjacency, std::uint32_t cls) {
    std::size_t n = 0;
    const std::uint32_t* ring = adjacency.ring(cls, &n);
    for (std::size_t i = 0; i < n; ++i)
        if (shared_triangles(mesh, adjacency, cls, ring[i]) < 2) return true;
    return false;
}

void compute_automask(const Mesh& mesh, const Adjacency& adjacency, const SculptWorkset& workset,
                      const AutomaskSettings& settings, const AutomaskInputs& inputs,
                      kernel::cfloat3 reference_normal, std::uint32_t seed_class, float* out) {
    const std::size_t count = workset.size();
    for (std::size_t i = 0; i < count; ++i) out[i] = 1.0f;
    if (!settings.any() || count == 0) return;

    // NORMAL ANGLE. Against the brush's own facing, which is fixed for the
    // stamp: recomputing it from the region would be circular, since the
    // region's average normal is weighted by the very weights this is shaping.
    if (has_factor(settings.factors, AutomaskFactor::NormalAngle)) {
        for (std::size_t i = 0; i < count; ++i) {
            const float d = std::clamp(kernel::cdot(workset.normals[i], reference_normal),
                                       -1.0f, 1.0f);
            out[i] *= fade(std::acos(d), settings.normal_angle);
        }
    }

    // BOUNDARY. Distance to an open border, in RINGS rather than in world
    // units: what matters is how many vertices of support a border vertex has
    // left, not how far away it is, and on an irregular triangulation those are
    // different questions.
    if (has_factor(settings.factors, AutomaskFactor::Boundary)) {
        const int rings = std::max(settings.boundary_rings, 0);
        // A breadth-first spread over the WORKSET alone. A border outside the
        // brush cannot be reached in `rings` steps without leaving the workset,
        // and a brush that is not near a border must not pay to discover that.
        std::vector<int> depth(count, -1);
        std::vector<std::uint32_t> frontier, next;
        for (std::size_t i = 0; i < count; ++i)
            if (is_boundary_class(mesh, adjacency, workset.classes[i])) {
                depth[i] = 0;
                frontier.push_back(static_cast<std::uint32_t>(i));
            }
        for (int step = 1; step <= rings && !frontier.empty(); ++step) {
            next.clear();
            for (std::uint32_t slot : frontier) {
                std::size_t n = 0;
                const std::uint32_t* ring = adjacency.ring(workset.classes[slot], &n);
                for (std::size_t k = 0; k < n; ++k) {
                    const std::uint32_t nb = ring[k];
                    if (nb >= workset.slot.size()) continue;
                    const std::uint32_t s = workset.slot[nb];
                    if (s == kNoClass || depth[s] >= 0) continue;
                    depth[s] = step;
                    next.push_back(s);
                }
            }
            frontier.swap(next);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (depth[i] < 0) continue;  // far from any border: untouched
            // Zero AT the border, ramping to one `rings` away. Smoothstepped
            // for the same reason every other gate here is.
            const float t = rings > 0 ? static_cast<float>(depth[i]) / static_cast<float>(rings)
                                      : 0.0f;
            out[i] *= t * t * (3.0f - 2.0f * t);
        }
    }

    // TOPOLOGY CONNECTED. Free under a surface walk, which already refuses to
    // cross a gap; this is for the ball footprint, where flatten and scrape
    // live and where two sheets a hair apart are exactly the hazard.
    if (has_factor(settings.factors, AutomaskFactor::TopologyConnected) &&
        seed_class != kNoClass) {
        std::vector<char> reached(count, 0);
        std::vector<std::uint32_t> stack;
        if (seed_class < workset.slot.size() && workset.slot[seed_class] != kNoClass) {
            const std::uint32_t start = workset.slot[seed_class];
            reached[start] = 1;
            stack.push_back(start);
        }
        while (!stack.empty()) {
            const std::uint32_t slot = stack.back();
            stack.pop_back();
            std::size_t n = 0;
            const std::uint32_t* ring = adjacency.ring(workset.classes[slot], &n);
            for (std::size_t k = 0; k < n; ++k) {
                const std::uint32_t nb = ring[k];
                if (nb >= workset.slot.size()) continue;
                const std::uint32_t s = workset.slot[nb];
                if (s == kNoClass || reached[s]) continue;
                reached[s] = 1;
                stack.push_back(s);
            }
        }
        for (std::size_t i = 0; i < count; ++i)
            if (!reached[i]) out[i] = 0.0f;
    }

    // CAVITY, from the caller's estimator — the same one a painted cavity mask
    // uses, because there is no other one this module could reach.
    if (has_factor(settings.factors, AutomaskFactor::Cavity) && inputs.cavity) {
        const float strength = std::clamp(settings.cavity_strength, 0.0f, 1.0f);
        for (std::size_t i = 0; i < count; ++i) {
            const float cavity = std::clamp(inputs.cavity(workset.positions[i]), 0.0f, 1.0f);
            out[i] *= 1.0f - cavity * strength;
        }
    }

    // SURFACE GROUP, from the caller's world lattice.
    if (has_factor(settings.factors, AutomaskFactor::SurfaceGroup) && inputs.group) {
        for (std::size_t i = 0; i < count; ++i)
            if (inputs.group(workset.positions[i]) != inputs.active_group) out[i] = 0.0f;
    }
}

}  // namespace mesh
}  // namespace clay
