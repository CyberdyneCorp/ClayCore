#pragma once

// Triangle mesh interchange type shared by every producer (SDF meshing,
// voxel greedy meshing) and consumer (decimation, validation, io, C ABI).
// Flat buffers, indices into positions; normals/colors/uvs are optional and
// vertex-aligned when present.

#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

struct Mesh {
    std::vector<kernel::cfloat3> positions;
    std::vector<kernel::cfloat3> normals;  // empty or positions.size()
    std::vector<kernel::cfloat3> colors;   // empty or positions.size()
    std::vector<kernel::cfloat2> uvs;      // empty or positions.size()
    std::vector<std::uint32_t> indices;    // triangle list

    std::size_t triangle_count() const { return indices.size() / 3; }
    bool empty() const { return indices.empty(); }
};

}  // namespace mesh
}  // namespace clay
