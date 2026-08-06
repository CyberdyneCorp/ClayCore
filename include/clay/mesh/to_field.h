#pragma once

// Sampling a mesh into a field (meshing spec, add-mesh-to-field-import).
//
// This is the step that makes an imported model something you can WORK on
// rather than merely display: once a mesh is a distance field it combines,
// blends, cuts and sculpts like anything the engine built itself.
//
// The distance and the sign both come from mesh::Bvh — see that header for why
// the sign is a winding number and not a ray cast.

#include <optional>

#include "clay/field/volume.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

struct ImportSettings {
    // Sample spacing. The dial on accuracy: the error inside the band falls
    // with it, and storage rises with its square (not its cube — the band is
    // sparse). Zero asks for a default derived from the mesh's own size.
    float cell_size = 0.0f;

    // Half-width of the band that keeps samples. Zero means three cells.
    float band = 0.0f;

    // How far past the mesh's bounds to sample. Zero means the band, which is
    // what keeps the band from being clipped at the surface where it matters.
    float padding = 0.0f;

    // Distance-over-radius at which a BVH node is summarized by one dipole
    // rather than descended. Larger is more accurate and slower.
    float beta = 2.0f;
};

// Sample `m` into a narrow-band volume. Returns nothing for a mesh with no
// triangles: there is no surface to measure from, and a volume built anyway
// would read as empty space everywhere, which is harder to notice than a
// failure.
std::optional<field::FieldVolume> to_field(const Mesh& m, const ImportSettings& settings = {});

// The same, against a BVH the caller already built — worth it when importing
// the same mesh at several resolutions, since the tree is the expensive part.
std::optional<field::FieldVolume> to_field(const Bvh& bvh, const ImportSettings& settings = {});

}  // namespace mesh
}  // namespace clay
