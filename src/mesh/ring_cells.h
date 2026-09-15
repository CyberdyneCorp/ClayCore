#pragma once

#include <algorithm>
#include <array>

namespace clay::mesh::detail {

// Neighbor offsets are flattened as (z+1)*9 + (y+1)*3 + x+1.
// The owner is unrequested: callers leave element 13 false.
using RingNeighbors = std::array<bool, 27>;

inline bool reaches_ring_neighbor(const RingNeighbors& wanted, const int* ax, int nx,
                                  const int* ay, int ny, const int* az, int nz) {
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                if (wanted[(az[z] + 1) * 9 + (ay[y] + 1) * 3 + ax[x] + 1]) return true;
    return false;
}

inline std::array<std::array<unsigned, 3>, 3> ring_row_masks(const RingNeighbors& wanted) {
    // First plane, interior, last plane. Each includes the owner offset zero.
    constexpr int directions[3][2] = {{-1, 0}, {0, 0}, {0, 1}};
    constexpr int counts[3] = {2, 1, 2};
    std::array<std::array<unsigned, 3>, 3> rows{};
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                if (reaches_ring_neighbor(wanted, directions[x], counts[x], directions[y],
                                          counts[y], directions[z], counts[z]))
                    rows[z][y] |= 1u << x;
    return rows;
}

inline int ring_plane(int coordinate, int dim) {
    if (coordinate == 0) return 0;
    return coordinate == dim - 1 ? 2 : 1;
}

// Emit local cell coordinates in z/y/x order. If an interior x coordinate
// qualifies, both endpoints do too, since they also reach offset zero.
template <class Emit>
void for_each_ring_cell(int dim, const RingNeighbors& wanted, const Emit& emit) {
    if (dim <= 0) return;
    if (dim == 1) {
        if (std::any_of(wanted.begin(), wanted.end(), [](bool hit) { return hit; }))
            emit(0, 0, 0);
        return;
    }
    const auto rows = ring_row_masks(wanted);
    for (int z = 0; z < dim; ++z)
        for (int y = 0; y < dim; ++y) {
            const unsigned row = rows[ring_plane(z, dim)][ring_plane(y, dim)];
            if (row & 2u) {
                for (int x = 0; x < dim; ++x) emit(x, y, z);
            } else {
                if (row & 1u) emit(0, y, z);
                if (row & 4u) emit(dim - 1, y, z);
            }
        }
}

}  // namespace clay::mesh::detail
