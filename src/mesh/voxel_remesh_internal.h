#pragma once

// The pieces `voxel_remesh` is assembled from, split across three translation
// units because one would have been a thousand lines of unrelated concerns:
// the pipeline and its resource arithmetic (voxel_remesh.cpp), the sparse
// sampling domain (voxel_remesh_sample.cpp), and everything done to the mesh
// after it comes off the lattice (voxel_remesh_finish.cpp).
//
// Internal. Nothing here is installed and nothing outside these three files
// may include it.

#include <cstdint>
#include <vector>

#include "clay/field/volume.h"
#include "clay/math/geom.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/voxel_remesh.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace mesh {
namespace remesh_detail {

// The lattice a remesh works over: the region, the voxel size and the band,
// all resolved from the params and the source's bounds. Everything downstream
// reads these rather than re-deriving them, because the brick arithmetic in
// `FieldVolume::sample_blocks` has to be reproduced EXACTLY for the marking to
// name the same bricks the fill will be asked for.
struct Lattice {
    math::Aabb region;
    float voxel_size = 0.0f;
    float band = 0.0f;
    // Bricks along each axis, by sample_blocks' own arithmetic.
    std::int32_t bcount[3] = {0, 0, 0};

    std::uint64_t brick_count() const {
        return static_cast<std::uint64_t>(bcount[0]) * static_cast<std::uint64_t>(bcount[1]) *
               static_cast<std::uint64_t>(bcount[2]);
    }
    // Lattice points along each axis — bricks times the brick dimension, plus
    // the one that closes the last brick.
    std::uint64_t lattice_cells() const {
        return static_cast<std::uint64_t>(bcount[0] * field::kBrickDim + 1) *
               static_cast<std::uint64_t>(bcount[1] * field::kBrickDim + 1) *
               static_cast<std::uint64_t>(bcount[2] * field::kBrickDim + 1);
    }
};

// Resolve the region, voxel size, band and brick counts for `source` under
// `params`. Returns false with `out_status` set when the resolution is not a
// usable number or the lattice is past the library's ceiling.
bool resolve_lattice(const Mesh& source, const VoxelRemeshParams& params, Lattice* out,
                     VoxelRemeshStatus* out_status);

// Which bricks of `lattice` hold a sample the band could keep — every brick
// whose sample box, dilated by the band, meets a source triangle's AABB.
//
// CONSERVATIVE BY CONSTRUCTION, and it has to be: a brick this misses is a
// brick the fill will answer with a constant, so a missed brick is a hole in
// the surface. Marking one that turns out empty costs 729 wasted queries and
// nothing else.
//
// One byte per brick rather than a bitset: the marking is written from many
// triangles and read from many threads, and a bitset would need either a lock
// or an atomic per word to be either.
std::vector<std::uint8_t> mark_active_bricks(const Mesh& source, const Lattice& lattice,
                                             parallel::CancelToken* token);

std::uint64_t count_active(const std::vector<std::uint8_t>& active);

// Sample the field over `lattice`, evaluating `bvh` only in the active bricks
// and filling the rest with the sign of the region they belong to.
//
// `out_cancelled` distinguishes "the user stopped it" from "there was no
// surface here": both leave a volume with no bricks.
field::FieldVolume sample_sparse(const Bvh& bvh, const Lattice& lattice,
                                 const std::vector<std::uint8_t>& active,
                                 parallel::CancelToken* token, bool* out_cancelled);

// March the volume's own lattice. Reads stored samples exactly and the
// volume's far bound elsewhere, with one ring of positive samples around the
// region so anything reaching the boundary is closed rather than left open.
Mesh extract_surface(const field::FieldVolume& volume, const Lattice& lattice,
                     VoxelRemeshSurfaceMode mode);

// -- after the lattice ------------------------------------------------------

// Per-vertex connected-component labels over `m`, by shared vertex index.
// Returns the component count; `out_label` is sized to the vertex count.
std::uint32_t label_components(const Mesh& m, std::vector<std::uint32_t>* out_label);

// Drop components whose absolute signed volume is below `minimum_volume`.
// Returns how many went. Leaves `m` untouched and returns 0 when none qualify,
// so the common path pays a measurement and no rewrite — and also when they ALL
// qualify, because a threshold above the whole model is a caller misjudging the
// scale and the useful answer to that is the model back.
std::uint32_t remove_small_components(Mesh* m, double minimum_volume);

// Move each vertex part of the way to the closest point on the source, clamped
// by distance and rejected where the source there faces away. Returns how many
// vertices actually moved.
// `source` accompanies `source_bvh` because the sheet test needs the SOURCE
// triangle's normal, and a tree reports the triangle index rather than the
// triangle.
std::uint64_t project_to_source(Mesh* m, const Mesh& source, const Bvh& source_bvh,
                                float voxel_size, float strength, float max_distance_voxels,
                                parallel::CancelToken* token);

// Every result vertex's distance to the source surface, reduced to an RMS, a
// 95th percentile and a maximum. One-sided by design — see the report's own
// note on what that cannot see.
struct SurfaceError {
    double rms = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};
SurfaceError measure_surface_error(const Mesh& result, const Bvh& source_bvh);

// Scale `m` about its own centroid toward `target_volume`, clamped to
// kVoxelRemeshMaxVolumeCorrection. Returns whether it did anything.
bool correct_volume(Mesh* m, double target_volume, double current_volume);

// Deterministic evidence that the source carries material thinner than
// `voxels` voxels: surface points stepped inward along their own normal and
// asked whether they are still inside. Walks a bounded, strided sample of the
// triangles, so its cost does not follow the model's.
bool has_thin_features(const Mesh& source, const Bvh& bvh, float voxel_size, float voxels);

// Boundary edges and connected components of a mesh, without the rest of what
// `validate` computes. Used by the estimate, which must stay cheap.
void count_boundaries_and_components(const Mesh& m, std::uint32_t* out_boundary_edges,
                                     std::uint32_t* out_components);

}  // namespace remesh_detail
}  // namespace mesh
}  // namespace clay
