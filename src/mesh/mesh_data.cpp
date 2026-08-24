#include "clay/mesh/mesh_data.h"

#include "clay/bytes.h"

namespace clay {
namespace mesh {

std::size_t Mesh::bytes() const {
    // Capacity, not size. A mesh that was decimated from two million triangles
    // to fifty thousand still holds the two million until someone shrinks it,
    // and the allocator is the one telling the truth about that.
    return sizeof(Mesh) + vector_bytes(positions) + vector_bytes(normals) +
           vector_bytes(colors) + vector_bytes(uvs) + vector_bytes(indices) +
           vector_bytes(quads);
}

}  // namespace mesh
}  // namespace clay
