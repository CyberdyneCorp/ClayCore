#include "clay/mesh/preflight.h"

#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/multires.h"

namespace clay {
namespace mesh {
namespace {

// A slot costs its value plus the pool's own per-slot bookkeeping: the
// generation, the free-list link and the live flag, rounded to alignment. Taken
// from the type rather than written as a number, so a field added to a record
// changes the estimate with it.
template <typename T>
constexpr std::uint64_t slot_bytes() {
    return sizeof(T) + 3u * sizeof(std::uint32_t);
}

// What a `std::vector` holds over what it was asked for, on the arrays that are
// grown rather than reserved. The figure `multires.cpp` measured on a level-3
// hierarchy: evaluated arrays came out 1.34x their structural cost and runtime
// ones 1.51x, and 1.75 is the ceiling it settled on. Applied to the PREDICTION
// because a budget that errs low is the one that gets an app killed.
constexpr double kCapacitySlack = 1.75;

std::uint64_t with_slack(std::uint64_t exact) {
    return static_cast<std::uint64_t>(static_cast<double>(exact) * kCapacitySlack);
}

SurfacePreflight finish(const memory::CapacityBuilder& builder, std::uint64_t budget) {
    const memory::CapacityEstimate estimate = builder.finish(budget);
    SurfacePreflight out;
    out.authoritative_bytes = estimate.authoritative_bytes;
    out.runtime_bytes = estimate.runtime_bytes;
    out.persistent_bytes = estimate.persistent_bytes;
    out.peak_bytes = estimate.peak_bytes;
    out.allowed = estimate.allowed;
    out.error = estimate.error;
    return out;
}

}  // namespace

SurfacePreflight preflight_to_dynamic(const Mesh& mesh, std::uint64_t budget) {
    const std::uint64_t vertices = mesh.positions.size();
    const std::uint64_t faces = mesh.indices.size() / 3;
    memory::CapacityBuilder b;
    b.authoritative(vertices, with_slack(slot_bytes<DynamicVertex>()));
    b.authoritative(faces * 3, with_slack(slot_bytes<DynamicHalfEdge>()));
    // Euler on a closed triangle surface: E = 3F/2. A surface with a boundary
    // has fewer, so this is the ceiling, which is the direction to err in.
    b.authoritative(faces * 3 / 2 + 1, with_slack(slot_bytes<DynamicEdge>()));
    b.authoritative(faces, with_slack(slot_bytes<DynamicFace>()));
    // TRANSIENT: the weld map from raw vertex to geometric vertex, and the
    // spatial bucketing the weld uses. The source mesh is the caller's and is
    // not counted — it exists before the call and after it.
    b.transient(vertices, 2u * sizeof(std::uint32_t));
    return finish(b, budget);
}

SurfacePreflight preflight_to_mesh(const DynamicSurface& surface, std::uint64_t budget) {
    const DynamicSurfaceStats stats = surface.stats();
    // BOUNDED BY CORNERS, not by vertices. The export splits a geometric vertex
    // into as many export vertices as it has distinct corner attributes, which
    // is how a flat mesh represents a seam — so a seam-heavy model exports
    // larger than its vertex count suggests, and an estimate that used the
    // vertex count would be under exactly where it matters.
    const std::uint64_t out_vertices = stats.halfedges;
    memory::CapacityBuilder b;
    b.authoritative(out_vertices, sizeof(kernel::cfloat3) * 3 + sizeof(kernel::cfloat2));
    b.authoritative(stats.faces * 3, sizeof(std::uint32_t));
    // The corner-to-export-vertex map, which lives only for the call.
    b.transient(stats.halfedges, sizeof(std::uint32_t));
    return finish(b, budget);
}

SurfacePreflight preflight_global_remesh(const Mesh& mesh, std::uint64_t target_triangles,
                                         std::uint64_t budget) {
    memory::CapacityBuilder b;
    // The RESULT: a triangle mesh at the target count, with roughly half as
    // many vertices as triangles on a closed surface.
    const std::uint64_t out_vertices = target_triangles / 2 + 1;
    b.authoritative(out_vertices, sizeof(kernel::cfloat3) * 2);
    b.authoritative(target_triangles * 3, sizeof(std::uint32_t));
    // THE SOURCE IS LIVE THROUGHOUT, and that is the whole reason this is
    // asked: a remesh is not a transformation in place, it is two meshes at
    // once plus the index over the source that the projection queries.
    b.transient_bytes(mesh.bytes());
    b.transient(mesh.indices.size() / 3, 8u * sizeof(std::uint32_t));
    return finish(b, budget);
}

SurfacePreflight preflight_encode(const DynamicSurface& surface, std::uint64_t budget) {
    const DynamicSurfaceStats stats = surface.stats();
    memory::CapacityBuilder b;
    // The blob IS the result, so it is persistent from this call's point of
    // view even though the caller usually writes it to a file and drops it.
    b.runtime(stats.vertices, sizeof(DynamicVertex));
    b.runtime(stats.halfedges, sizeof(DynamicHalfEdge));
    b.runtime(stats.edges, sizeof(DynamicEdge));
    b.runtime(stats.faces, sizeof(DynamicFace));
    b.runtime_bytes(1024);  // the header and the per-pool preamble
    // AND THE SURFACE IS STILL THERE. An encode holds a second copy of
    // everything it is encoding, which is exactly the shape of operation whose
    // peak exceeds its result.
    b.transient_bytes(surface.bytes());
    return finish(b, budget);
}

SurfacePreflight preflight_encode(const MultiresSurface& surface, std::uint64_t budget) {
    const MultiresMemory m = surface.memory();
    memory::CapacityBuilder b;
    // The per-level FACE LISTS are not written — they follow from the cage and
    // the rule — so the blob is the cage plus every level's detail, and not the
    // topology. That is the one place this estimate is much smaller than the
    // thing it encodes, and it is worth stating rather than rounding up out of
    // caution: a host that budgeted for the topology would refuse saves that
    // fit.
    b.runtime_bytes(m.base);
    b.runtime_bytes(m.detail);
    b.runtime_bytes(1024);
    b.transient_bytes(m.base + m.detail);
    return finish(b, budget);
}

}  // namespace mesh
}  // namespace clay
