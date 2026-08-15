// Sampling a mesh into a field (meshing spec, add-mesh-to-field-import).

#include "clay/mesh/to_field.h"

#include <algorithm>
#include <cmath>

namespace clay {
namespace mesh {

using kernel::cf3;

namespace {

// A cell size for a caller who did not choose one. A fixed fraction of the
// longest side rather than a fixed number: a mesh is imported at whatever scale
// it was authored, and a default in world units would be far too fine for a
// building and far too coarse for a bolt.
float default_cell(const math::Aabb& box) {
    kernel::cfloat3 extent = box.max - box.min;
    float longest = std::max(extent.x, std::max(extent.y, extent.z));
    return longest > 0.0f ? longest / 96.0f : 0.05f;
}

}  // namespace

std::optional<field::FieldVolume> to_field(const Bvh& bvh, const ImportSettings& settings) {
    if (bvh.empty()) return std::nullopt;
    math::Aabb box = bvh.bounds();
    if (box.empty()) return std::nullopt;

    const float cell = settings.cell_size > 0.0f ? settings.cell_size : default_cell(box);
    const float band = settings.band > 0.0f ? settings.band : cell * 3.0f;
    const float padding = settings.padding > 0.0f ? settings.padding : band;
    const float beta = settings.beta;

    kernel::cfloat3 pad = cf3(padding, padding, padding);
    math::Aabb region{box.min - pad, box.max + pad};

    // sample_parallel: a BVH signed-distance query is pure — a const tree and a
    // generalized winding number — and it is the expensive half of every mesh
    // import. 4767 -> 216 ms for a 9k-triangle model at a 0.01 cell.
    return field::FieldVolume::sample_parallel(
        [&bvh, beta](kernel::cfloat3 p) { return bvh.signed_distance(p, beta); }, region, cell,
        band);
}

std::optional<field::FieldVolume> to_field(const Mesh& m, const ImportSettings& settings) {
    if (m.triangle_count() == 0) return std::nullopt;
    Bvh bvh = Bvh::build(m);
    return to_field(bvh, settings);
}

}  // namespace mesh
}  // namespace clay
