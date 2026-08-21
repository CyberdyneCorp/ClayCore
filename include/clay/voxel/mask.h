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
// Masking gates edits where they are AUTHORED: voxel edits consume the mask per
// cell at apply time, and SDF strokes consume it when a stroke is turned into
// items. That protects a region from what you do NEXT.
//
// It is no longer the only way a mask acts. An SDF item can also carry a GATE
// (`scene::Node::gate`) and then does not act where the mask protects — which
// is what makes a mask protect a surface from an arbitrary operation, a boolean
// included, rather than only from a brush. The gate is not this field: it is the
// signed distance to { mask >= threshold }, measured by `brush::mask_to_field`,
// because a [0,1] paint value composed into a field expression puts a step in
// the result and the Lipschitz bound stops meaning anything.

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

    // -- bounded region operations -------------------------------------------
    // The complement `invert()` cannot express. Inverting the painted region is
    // the only thing a sparse unbounded lattice CAN do, and it is not what "mask
    // a limb, invert, sculpt everything else" means: the untouched storage stays
    // unmasked, and the boundary lands on chunk edges rather than on the painted
    // region. So the caller supplies the finite region the complement is taken
    // over — it always has one, from a grid's bounds or an item's.
    //
    // A cell is in the region when its CENTRE is. Both cost the region's volume
    // in cells, so both are no-ops for a region `region_is_walkable` rejects —
    // ask first if you need to tell "did nothing" from "could not".
    void fill(const math::Aabb& region, float value);
    void invert_within(const math::Aabb& region);

    // Whether fill and invert_within can walk this region on this lattice.
    // False for an empty one, and for one whose cell count exceeds the budget
    // below — a bound rather than a hang, since a caller that passes a
    // near-infinite box has made an arithmetic mistake rather than a request,
    // and the cell indices for one do not fit in the lattice's own int32.
    static constexpr double kMaxRegionCells = 268435456.0;  // 1 << 28
    bool region_is_walkable(const math::Aabb& region) const;

    // -- queries -------------------------------------------------------------
    // A counter bumped by every mutation, so a consumer that derives something
    // EXPENSIVE from this mask can tell whether its derivation is still good.
    //
    // It exists for one measured reason: `brush::mask_to_field` — the bake a
    // gated item needs — costs 21 ms at four thousand painted cells and 145 ms
    // at thirty thousand, and gating N items by one mask used to pay that N
    // times. It is a CHANGE TOKEN, not a version: compare it for equality
    // against a value you stored, never order two of them, and never compare
    // across two different MaskFields.
    std::uint64_t revision() const { return revision_; }

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

    // Every mutator calls this. A mutator that forgets to leaves a consumer
    // holding a stale derivation, which is why a test walks all of them.
    void touch() { ++revision_; }

    float cell_size_;
    std::uint64_t revision_ = 0;
    std::unordered_map<VoxelCoord, Chunk, VoxelCoordHash> chunks_;
};

}  // namespace voxel
}  // namespace clay
