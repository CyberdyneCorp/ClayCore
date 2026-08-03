#pragma once

// Shared by CPU and (hybrid) GPU backends: Backend::mesh() = field values
// from the backend's own eval_grid + host marching-tetrahedra triangulation.
// Topology is fully determined by the evaluated values, which is what the
// topology-invariant parity contract compares.

#include <vector>

#include "clay/eval/backend.h"
#include "clay/mesh/marching.h"

namespace clay {
namespace eval {

inline Status grid_mesh(Backend& backend, const scene::Tape& tape, const GridQuery& q,
                        std::vector<float>* out_verts, std::vector<std::uint32_t>* out_indices) {
    if (!out_verts || !out_indices || q.nx < 2 || q.ny < 2 || q.nz < 2)
        return Status::InvalidInput;
    std::vector<float> values(static_cast<std::size_t>(q.nx) * q.ny * q.nz);
    Status s = backend.eval_grid(tape, q, values.data());
    if (s != Status::Ok) return s;
    auto sample = [&](int i, int j, int k) -> float {
        if (i < 0 || j < 0 || k < 0 || i >= q.nx || j >= q.ny || k >= q.nz)
            return kernel::cabs(q.spacing);
        return values[(static_cast<std::size_t>(k) * q.ny + j) * q.nx + i];
    };
    int cmin[3] = {0, 0, 0};
    int cmax[3] = {q.nx - 1, q.ny - 1, q.nz - 1};
    mesh::Mesh m = mesh::mesh_lattice(sample, cmin, cmax, q.origin, q.spacing);
    out_verts->resize(m.positions.size() * 3);
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        (*out_verts)[i * 3] = m.positions[i].x;
        (*out_verts)[i * 3 + 1] = m.positions[i].y;
        (*out_verts)[i * 3 + 2] = m.positions[i].z;
    }
    *out_indices = std::move(m.indices);
    return Status::Ok;
}

}  // namespace eval
}  // namespace clay
