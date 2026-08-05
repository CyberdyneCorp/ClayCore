#pragma once

// Paintable mask field (voxel-engine spec): a sparse scalar field in [0,1]
// that gates how strongly an edit is allowed to act.
//
// The mask is addressed in WORLD units on its own lattice, deliberately not in
// a layer's voxel cells. That is the whole point: a layer's resolution is an
// evaluation parameter here, and a mask stored in cell indices would silently
// misalign — or die outright — the moment the resolution changed or content
// moved between the SDF and voxel representations. 3DCoat's masks do exactly
// that, and it is their worst-rated defect. World addressing makes the failure
// unrepresentable rather than merely untested.
//
// Masking gates edits where they are AUTHORED, not where the field is
// evaluated. Voxel edits consume the mask per cell at apply time; SDF edits
// are declarative items with no per-point strength, so they consume it when a
// stroke is turned into items (the stroke engine). A mask therefore protects a
// region from what you do next; it does not retroactively gate items already
// in the edit list.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/math/geom.h"
#include "clay/voxel/grid.h"

namespace clay {
namespace voxel {

// A scalar mask on a chunked lattice. Values are stored as uint8: 256 levels
// is more than a falloff needs and keeps a fully masked region cheap.
class MaskField {
  public:
    explicit MaskField(float cell_size = 0.1f) : cell_size_(cell_size > 0.0f ? cell_size : 0.1f) {}

    float cell_size() const { return cell_size_; }

    // Lattice cell containing a world position. Cell (x,y,z) occupies
    // [x,x+1)*cell_size, matching VoxelGrid's convention.
    VoxelCoord cell_at(kernel::cfloat3 world_p) const;
    // World-space centre of a cell.
    kernel::cfloat3 cell_centre(VoxelCoord c) const;

    // -- values --------------------------------------------------------------
    float get(VoxelCoord c) const;
    void set(VoxelCoord c, float value);  // clamped to [0,1]; 0 releases storage
    // Mask at a world position, 0 where nothing has been painted. Nearest-cell:
    // the mask is a gate, not a surface, so interpolation would only blur the
    // boundary the falloff already shaped.
    float sample(kernel::cfloat3 world_p) const { return get(cell_at(world_p)); }

    // -- painting ------------------------------------------------------------
    // Same brush vocabulary as voxel edits — footprint size in mask cells,
    // cube or sphere, falloff curve, strength — so masking is the same gesture
    // as sculpting. Each cell moves toward `target` by the brush weight, so
    // paint is target 1 and erase is target 0. Unlike voxel occupancy this is
    // a scalar, so the weight is stored directly rather than dithered:
    // `p.seed` is unused here.
    void paint(kernel::cfloat3 world_centre, const BrushParams& p, float target);
    void paint(VoxelCoord c, const BrushParams& p, float target);

    // -- region operations ---------------------------------------------------
    // Over the whole field: what "freeze the rest" and "grow the selection"
    // need. All are no-ops on an empty mask except invert, which is defined
    // over the painted region only (inverting the infinite lattice is not a
    // representable operation on a sparse field).
    void invert();
    void clear();
    void expand(int steps = 1);    // grey dilation, 6-neighbourhood max
    void contract(int steps = 1);  // grey erosion, 6-neighbourhood min
    void smooth(int iterations = 1);

    // -- queries -------------------------------------------------------------
    std::size_t painted_count() const;  // cells with a nonzero value
    bool empty() const { return chunks_.empty(); }
    std::optional<VoxelCoord> bounds_min() const;
    std::optional<VoxelCoord> bounds_max() const;

    // -- serialization (RLE, deterministic) ----------------------------------
    std::vector<std::uint8_t> serialize() const;
    static std::optional<MaskField> deserialize(const std::uint8_t* data, std::size_t size);

  private:
    struct Chunk {
        std::vector<std::uint8_t> data;  // kChunkDim^3
        int painted = 0;
    };
    static VoxelCoord chunk_key(VoxelCoord c);
    static std::size_t chunk_offset(VoxelCoord c);
    // Run a 3x3x3-separable neighbourhood op over the painted region padded by
    // `pad`, through a dense scratch buffer so sparsity cannot clip the result.
    template <typename Fn>
    void neighbourhood_op(int pad, Fn&& reduce);

    float cell_size_;
    std::unordered_map<VoxelCoord, Chunk, VoxelCoordHash> chunks_;
};

}  // namespace voxel
}  // namespace clay
