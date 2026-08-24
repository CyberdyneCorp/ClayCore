#pragma once

// NAMING A REGION OF SURFACE — ZBrush's PolyGroups, Blender's Face Sets.
//
// This library had no such concept on any representation. Visibility was per
// LAYER, so "isolate the head" meant the head was authored as its own layer —
// a decision taken before the artist knew they would want it. A layer holds
// exactly ONE mask, so N named regions could not be emulated with N masks. And
// scene groups group EDIT-LIST NODES, which says how three items combine and
// nothing about which part of the resulting surface is the head.
//
// WHY A WORLD-SPACE LATTICE, and not per-representation storage. The obvious
// shape is a per-face id on a mesh, a second palette channel on a voxel grid,
// and something else for SDF. Three mechanisms is three sets of semantics for
// hide, isolate, grow and border, and they will disagree.
//
// The free answer for SDF — map a surface point to the ITEM that produced it —
// fails the two cases that matter, and fails them for the same reason: an
// artist's groups do not respect the edit list, because the edit list is how
// the shape was BUILT and a group is about what it IS. An armour panel spanning
// two items is not an item. A face that is part of one sphere is not one
// either.
//
// So: one lattice, addressed in world units, asked "which group is this surface
// point in" identically whatever the surface is made of.
//
// WHAT THAT COSTS. A group boundary is quantized to this lattice rather than to
// the representation, so a mesh that could have carried an exact per-face
// boundary does not. That is a visible edge at the group border and it is the
// price.
//
// WHAT IT BUYS. Groups survive a representation bridge BY CONSTRUCTION: they
// were never in the SDF, the voxels or the mesh, so rasterizing, meshing or
// converting cannot lose them. A voxel grid's memory is untouched — there is no
// second palette channel against a 256^3 guarantee. And it inherits world
// addressing, serialization and undo from the shape MaskField already
// established.
//
// PER DOCUMENT, not per layer. A mask is per layer because it gates edits to
// that layer; a group names a region of the MODEL, and "isolate the head" when
// the head spans two layers is the case per-layer storage makes impossible.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clay/math/geom.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace voxel {

// A group id. Zero is "no group", so an empty field costs nothing and a
// document without groups behaves exactly as one did before this existed. Ids
// are opaque to the engine: the host names them.
using GroupId = std::uint16_t;
inline constexpr GroupId kNoGroup = 0;

class GroupField {
  public:
    explicit GroupField(float cell_size = 0.05f) : cell_size_(cell_size) {}

    float cell_size() const { return cell_size_; }
    bool empty() const { return chunks_.empty(); }

    // -- addressing, in world units ------------------------------------------
    //
    // World rather than cell, because that is what makes a group independent of
    // any representation's resolution — the same property `add-mask-field`
    // built its lattice for.
    VoxelCoord cell_at(kernel::cfloat3 world) const;
    kernel::cfloat3 cell_centre(VoxelCoord c) const;

    GroupId get(VoxelCoord c) const;
    // Which group a SURFACE POINT is in. The one query every representation
    // asks, and the reason this is not three mechanisms.
    GroupId at(kernel::cfloat3 world) const { return get(cell_at(world)); }

    void set(VoxelCoord c, GroupId id);
    // Assign a whole region, addressed the way brushes already address one so a
    // host reuses the vocabulary it has. Assigning kNoGroup releases storage.
    void fill(const math::Aabb& region, GroupId id);
    // Everything currently in `from` becomes `to`. Merging into kNoGroup is how
    // a group is deleted without walking the lattice for it.
    std::size_t reassign(GroupId from, GroupId to);

    // -- set operations, defined on the REGION --------------------------------
    //
    // Defined on the lattice, which is what makes "grow a group on a mesh" and
    // "grow it on a voxel grid" the same operation rather than two that agree
    // by inspection. The spec asks for that; the shared lattice gives it by
    // construction, since neither representation is consulted.
    //
    // VOLUMETRIC, NOT GEODESIC, and worth being plain about because it is the
    // one place this differs from what a mesh tool does. ZBrush grows a face
    // set ALONG the surface; this dilates in 3D. Where a surface folds back
    // within `steps` cells of itself — the inside of a tight crease, two sides
    // of a thin wall — growth crosses the gap and claims the other side, which
    // a geodesic grow would not. Bounded by the same thing that bounds
    // everything here: the lattice knows regions, not surfaces.
    //
    // Growth claims only UNGROUPED cells. Growing one group into another would
    // silently destroy a region an artist named, and "grow" is not a verb
    // anyone expects to delete.
    std::size_t grow(GroupId id, int steps = 1);
    // Erodes from the boundary, releasing cells to kNoGroup. Shrinking a group
    // to nothing removes it, which is the same as reassigning it away.
    std::size_t shrink(GroupId id, int steps = 1);
    // The cells of `id` that touch a cell not in `id` — the seam an artist
    // would mask, crease or polish. Returned rather than assigned, because a
    // border is a query about a region and not a region of its own; a caller
    // that wants it named passes the result to `set`.
    std::vector<VoxelCoord> border(GroupId id) const;

    // -- visibility ----------------------------------------------------------
    //
    // A property of the ID, not of the cell: hiding a group is one flag rather
    // than a rewrite of every cell carrying it, which is also what makes
    // "isolate" cheap — show one, hide the rest.
    void set_visible(GroupId id, bool visible);
    bool visible(GroupId id) const;
    // Show `id` and hide every other group that exists. kNoGroup is left
    // visible: ungrouped surface is not something an artist hid.
    void isolate(GroupId id);
    void show_all();
    // Every group that was hidden shows, every group that was shown hides.
    // kNoGroup is untouched for the reason isolate() leaves it alone: ungrouped
    // surface is not something an artist hid.
    void invert_visibility();
    bool any_hidden() const { return !hidden_.empty(); }
    // Whether a surface point is hidden by its group. The query a mesher will
    // ask when the visible half of this lands.
    bool point_hidden(kernel::cfloat3 world) const { return !visible(at(world)); }

    // -- what is here --------------------------------------------------------
    std::size_t cell_count() const;              // cells carrying any group
    std::size_t cell_count(GroupId id) const;    // cells carrying this one
    std::vector<GroupId> ids() const;            // present, ascending, never kNoGroup
    // Every cell carrying this id. The walk grow, shrink and border share, and
    // public because a host wanting the region itself — to bound a brush, to
    // frame a camera — would otherwise walk the lattice guessing.
    std::vector<VoxelCoord> cells_of(GroupId id) const;
    std::optional<VoxelCoord> bounds_min() const;
    std::optional<VoxelCoord> bounds_max() const;

    // -- from a mask ---------------------------------------------------------
    //
    // The spec asks that a region be addressable "by surface group or by mask",
    // and this is how the second becomes the first rather than a parallel
    // visibility mechanism. A host paints a mask — which it already knows how
    // to do, with a brush, a cavity measure or an extrude — and names the
    // result. One hidden set, one set of semantics, two ways in.
    //
    // Cells at or above `threshold` take `id`. The two lattices need not share
    // a cell size: membership is decided by sampling the mask at this field's
    // cell CENTRE, the same rule fill() uses, so a coarse group over a fine
    // mask quantises rather than misaligns.
    std::size_t fill_from_mask(const MaskField& mask, GroupId id, float threshold = 0.5f);

    // -- serialization (RLE, deterministic) ------------------------------------
    //
    // The hidden set rides WITH the ids, in the same blob, because a document
    // that reloaded its groups and forgot which were hidden would show an
    // artist geometry they had put away — and "hiding is not deleting" is a
    // guarantee that has to survive a save to mean anything.
    std::vector<std::uint8_t> serialize() const;
    static std::optional<GroupField> deserialize(const std::uint8_t* data, std::size_t size);

    // A change token for consumers holding a derivation, exactly as MaskField
    // has. Compare for equality against a value you stored; never order two.
    std::uint64_t revision() const { return revision_; }

  private:
    struct Chunk {
        std::vector<GroupId> data;  // kChunkDim^3
        int assigned = 0;
    };
    static VoxelCoord chunk_key(VoxelCoord c);
    static std::size_t chunk_offset(VoxelCoord c);
    void touch() { ++revision_; }

    float cell_size_;
    std::uint64_t revision_ = 0;
    std::unordered_map<VoxelCoord, Chunk, VoxelCoordHash> chunks_;
    // Only the HIDDEN ones are stored, so a document where nothing is hidden
    // carries nothing and `visible()` is a miss on an empty set.
    std::unordered_set<GroupId> hidden_;
};

}  // namespace voxel
}  // namespace clay
