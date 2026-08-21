#pragma once

// Attribute transfer between meshes (add-mesh-attribute-transfer).
//
// A mesh layer's whole reason to exist is that it holds triangles somebody
// meant: a retopology, a uv layout, painted colour. Sculpting one preserves all
// of that. Anything that LEAVES the mesh layer does not — `Volume::from_mesh`
// samples the model onto a lattice, and what a mesher hands back is new
// geometry with new vertices. The shape survives; the colours and uvs are gone.
//
// Most of that is refundable. The nearest point on the ORIGINAL surface knows
// what belonged there, and `Bvh::closest` returns the triangle and the
// barycentrics needed to read it — which is what that query exists for.
//
// WHAT THIS DOES NOT GIVE BACK: topology. The target is still the mesher's
// geometry, with new vertices and no relationship to the retopology that went
// in. This refunds the paint and most of the uvs; it does not refund the mesh.

#include <cstddef>

#include "clay/kernel/shim.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

struct TransferOptions {
    bool colors = true;
    bool uvs = true;

    // OFF by default, and the default is the point. A resampled mesh has its
    // own geometry and its normals should describe IT; taking the source's
    // would make new geometry shade like the old shape. Turn this on only when
    // the two meshes are near-identical and the source's normals were authored
    // rather than computed.
    bool normals = false;

    // How far a target vertex may be from the source before it takes the
    // fallback instead of an attribute. Geometry can exist where the source
    // never was — after a boolean, or where a mesher bridged a gap — and the
    // closest point to it carries no meaning.
    //
    // Zero means "derive it": five per cent of the source's bounding diagonal,
    // which is generous for a resample and tight enough to reject geometry that
    // belongs to something else. An absolute value overrides that, for a host
    // that knows its own tolerance.
    float max_distance = 0.0f;
};

// What actually happened. A transfer that fell back across most of the mesh is
// otherwise indistinguishable from a good one, which is the whole reason this
// is returned rather than a bool.
struct TransferReport {
    std::size_t transferred = 0;  // vertices that took a source attribute
    std::size_t fell_back = 0;    // ...and vertices too far from it

    // Which channels actually moved. A channel the SOURCE does not carry is
    // left alone on the target rather than cleared: that composes (take colour
    // from A and uvs from B) and these flags are what make it visible.
    bool colors = false;
    bool uvs = false;
    bool normals = false;

    float max_distance = 0.0f;  // the threshold used, derived or given
};

// Give `target` the attributes of `source`, by closest point.
//
// Positions and topology are NEVER modified. This is an attribute transfer and
// not a projection: a verb that moved the target's vertices toward the source
// would be a different operation, and conflating the two turns "transfer" into
// "deform" without saying so.
//
// THE UV SEAM LIMITATION, stated because it follows from the representation
// rather than from a defect: uvs are per VERTEX, which is how a seam exists at
// all — the source duplicates a position into two vertices carrying different
// uvs. A target vertex lying on such a seam has one uv slot and two correct
// answers, and takes whichever triangle the closest-point query returned. That
// can stretch a triangle across the uv layout. Colour is unaffected, being
// continuous across a seam.
// Read one attribute where a closest-point query landed: triangle `triangle`
// of `m`, at barycentrics (u, v). Exposed because two callers need exactly
// this and must not drift apart — `transfer_attributes` below, and
// `VoxelGrid::rasterize_mesh`, which is how an imported model's vertex colours
// reach a palette.
//
// Out of range, or an attribute the mesh does not carry, gives `fallback`.
kernel::cfloat3 sample_color(const Mesh& m, std::uint32_t triangle, float u, float v,
                             kernel::cfloat3 fallback);
kernel::cfloat2 sample_uv(const Mesh& m, std::uint32_t triangle, float u, float v,
                          kernel::cfloat2 fallback);

TransferReport transfer_attributes(const Mesh& source, Mesh* target,
                                   const TransferOptions& options = {});

}  // namespace mesh
}  // namespace clay
