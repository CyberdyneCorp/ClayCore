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
    bool any_hidden() const { return !hidden_.empty(); }
    // Whether a surface point is hidden by its group. The query a mesher will
    // ask when the visible half of this lands.
    bool point_hidden(kernel::cfloat3 world) const { return !visible(at(world)); }

    // -- what is here --------------------------------------------------------
    std::size_t cell_count() const;              // cells carrying any group
    std::size_t cell_count(GroupId id) const;    // cells carrying this one
    std::vector<GroupId> ids() const;            // present, ascending, never kNoGroup
    std::optional<VoxelCoord> bounds_min() const;
    std::optional<VoxelCoord> bounds_max() const;

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
