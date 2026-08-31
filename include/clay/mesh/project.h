#pragma once

// MOVING A SURFACE ONTO ANOTHER ONE (mesh-multires spec, add-mesh-multires).
//
// WHY THIS IS NOT `transfer_attributes`. That call moves colours, UVs and
// normals by closest point and barycentric interpolation, and it PROMISES it
// moves no position — a promise its callers depend on, and one that a mesh
// layer's whole value rests on. Reprojection needs the same spatial query and
// the opposite contract. Two functions with one BVH underneath and two
// guarantees is the honest shape; overloading the attribute call would silently
// weaken a guarantee somebody is relying on.
//
// WHAT IT IS FOR. A sculpt made on one surface, carried onto a clean cage: the
// route by which a hierarchy that already carries detail accepts a NEW base.
// `MultiresSurface::set_base_mesh` refuses that outright — the stencils above a
// changed cage are meaningless and every coefficient stored against them would
// be uninterpretable — and this is the explicit, priced conversion it names
// instead.
//
// NORMAL RAY FIRST, CLOSEST POINT AS THE FALLBACK, and the order matters. A
// closest-point query alone snaps a vertex to whatever surface is nearest,
// which across a thin wall — a lip, an eyelid, the gap between two fingers — is
// routinely the WRONG side, and the artefact is a sculpt that reads inside out
// in exactly the places an artist cares most about. Casting along the vertex's
// own normal finds the surface the vertex is looking at, in both directions,
// and only where that misses does the closest point answer.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/mesh_data.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace mesh {

struct ProjectOptions {
    // How far a vertex may travel. Zero means "as far as it takes", which is
    // what a test wants and what a host projecting between two surfaces of
    // unknown separation must not use: an unbounded search will happily drag a
    // vertex across a model to the far wall.
    float max_distance = 0.0f;

    // Cast along the vertex normal before falling back to the closest point.
    // Off makes this a pure closest-point snap, which is faster and wrong
    // across thin geometry — see the header note.
    bool normal_ray_first = true;

    // Blend toward the projected position rather than landing on it. 1 is a
    // full projection; a smaller value is a partial pass, which is how a
    // reprojection that is fighting a bad correspondence is tamed without
    // changing the query.
    float strength = 1.0f;
};

struct ProjectReport {
    std::size_t moved = 0;
    // Vertices the reference had no answer for within `max_distance`. LEFT
    // WHERE THEY WERE rather than snapped to something arbitrary: a vertex with
    // no correspondence is information, and a host can see how much of the
    // surface had none.
    std::size_t missed = 0;
    std::size_t by_ray = 0;
    std::size_t by_closest = 0;
    float max_offset = 0.0f;
    double mean_offset = 0.0;
    bool cancelled = false;
};

// Move `positions` onto `reference`. `normals` must be parallel to `positions`
// and is the direction the ray is cast along; pass an empty vector to use the
// closest point everywhere.
//
// `index` may be null, in which case one is built over `reference` and thrown
// away — which is the right call once and the wrong call in a loop, hence the
// parameter.
ProjectReport project_surface(const Mesh& reference, const std::vector<kernel::cfloat3>& normals,
                              std::vector<kernel::cfloat3>* positions,
                              const ProjectOptions& options = {}, const Bvh* index = nullptr,
                              const parallel::CancelToken* cancel = nullptr);

}  // namespace mesh
}  // namespace clay
