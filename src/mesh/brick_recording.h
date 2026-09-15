#pragma once

#include "clay/mesh/marching.h"

namespace clay::mesh::detail {

// Internal reference switch: tests compare optimized recording against the
// original recorder, including the complete welded mesh and its brick ranges.
Mesh mesh_bricks_recorded(const brick::BrickCache& cache,
                         const scene::Document* doc_for_attributes,
                         const MeshingOptions& options,
                         const std::vector<brick::BrickKey>* keys,
                         std::vector<BrickMeshRange>* out_ranges,
                         const scene::CullIndex* cull_index, int lod,
                         bool deduplicate);

}  // namespace clay::mesh::detail
