#pragma once

// THE PART OF A VERTEX'S SURFACE NEIGHBOURHOOD ITS OWN LEVEL DOES NOT HOLD
// (mesh-multires spec, finish-regional-multires).
//
// A regional level stores the faces of the base patches it refines and nothing
// else, so a vertex on the edge of what it stores has a SHORT ring: the surface
// continues, but at the level below. Every walk built on the level's own
// connectivity — the display normal, the transported frame, the brush's
// angle-weighted normal, the boundary automask's shared-triangle count, the
// Laplacian's mean — therefore sees an open border where there is none.
//
// The failure that produces is not a wrong neighbour but a MISSING one, and it
// is silent. There is no branch anywhere that picks the coarse side and picks
// it wrongly: a coarse neighbour has no vertex, no weld class and no face at
// the fine level to be picked at all. So this file is REPRESENTATIONAL — it
// gives the missing neighbours an identity — rather than conditional, and
// nothing that reads it branches on a level.
//
// WHAT IT IS, EXACTLY. For one level it holds the faces of the DENSE level that
// the level itself does not store but that touch a vertex it does, expressed
// over a JOINED numbering:
//
//     id <  vertex_count      a vertex of this level
//     id >= vertex_count      `outside_positions[id - vertex_count]`
//
// The faces are quads, in the same corner order the subdivision emits — vertex
// point, next edge point, face point, previous edge point — and they are the
// same faces, with the same corners, a uniformly refined hierarchy would have
// put there. That is what makes a reader's answer at a boundary the dense
// hierarchy's answer rather than an approximation of it.
//
// WHY ONE ANSWER AND NOT ONE CALLBACK. The obvious shape is a single
// `for_each_surface_neighbor(vertex, callback)` that hides which side a
// neighbour is on. The tree refuses it twice over. `smooth_targets` re-reads
// the same materialized neighbour CSR once per smoothing pass, so a callback
// would re-walk the topology per pass and per stamp — which is the cost
// `MeshSculptor::build_neighbors` was written to avoid and says so. And a
// "neighbour" is not one payload: across the multires path it is a position at
// P(n), a position at S(n), a detail coefficient triple, a geometric normal, a
// colour, a workset slot, a triangle with its corner and interior angle, an
// incident face, and a shared-triangle count. So this is ONE TOPOLOGY ANSWER
// materialized once, with a separate reader per payload.
//
// WHY IT ANSWERS IN FACES. Every reader that is wrong at a boundary is a face
// or corner walk, and the defect was measured to BE the incomplete face ring:
// the predicate "this vertex's display normal differs from the dense
// hierarchy's" and the predicate "this vertex's face ring is smaller than the
// dense hierarchy's" disagree on 0 vertices at every level. A vertex ring falls
// out of the corners, and is provided here for the readers that want one.
//
// THE OUTSIDE POSITIONS ARE THE SURFACE WITH NO DETAIL ON IT, and that is the
// only answer available rather than a simplification: a vertex the level does
// not store has nowhere to hold a coefficient, so the pure subdivision of the
// level below IS the surface there. They are computed with the subdivision's
// own stencils — the same call a regional level's own vertices are computed
// with — so they are bit-identical to the dense hierarchy's wherever the dense
// hierarchy carries no detail outside the refined region.
//
// WITH ONE CONDITION, stated because it is a property of how the hierarchy was
// BUILT and not of this file. A stencil for a vertex just outside the refined
// region reads the level below around it, and where the level below stops too
// the boundary rule fires instead of the interior one — so the value is the
// dense hierarchy's only while the level below holds one more ring than the
// level above. `refine_patches_to_level` grades exactly that way, adding a ring
// per level; `resolve_keep` on its own enforces only that a refined patch's own
// vertex ring is resident one level down, which is what a level's OWN vertices
// need and one ring short of what the vertices beyond them need.
//
// ORDER IS PART OF THE ANSWER, because float addition is not associative and
// the readers sum over it. Faces come back in the order the DENSE level would
// have numbered them — parent face order, then corner order — and a ring comes
// back ascending in the joined numbering. Both are already deterministic: the
// level's faces are patch-major in the parent's own order and `full_of` is
// ascending, so this states an order rather than imposing one.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

struct LevelTopology;
struct LevelConnectivity;

struct CrossLevelNeighborhood {
    // The level this describes: joined ids below it are its own vertices.
    std::uint32_t vertex_count = 0;

    // The neighbourhood's vertices this level does not store, and the pure
    // subdivision's value at each. Ascending in the parent's child layout, so
    // the numbering is a function of the topology and not of visit order.
    std::vector<std::uint32_t> outside_layout;
    std::vector<kernel::cfloat3> outside_positions;

    // The derived faces: four joined corner ids each, and the index the DENSE
    // level would have given the face. Ascending in that index.
    std::vector<std::uint32_t> corners;
    std::vector<std::uint32_t> dense_face;
    // The base patch of the coarse face each one descends from — the same
    // identity `LevelTopology::face_patch` carries, so a derived face needs no
    // new notion of where it belongs.
    std::vector<std::uint32_t> face_patch;

    // CSR over the JOINED numbering: the derived faces incident to each id,
    // outside ids included, so a reader asks the same question of a neighbour
    // whichever side of the boundary it is on.
    std::vector<std::uint32_t> face_offsets, faces;
    // CSR over the joined numbering: the neighbours a derived face gives an id,
    // ascending and free of duplicates.
    //
    // A QUAD'S TRIANGULATION DIAGONAL IS IN IT, because the level mesh's own
    // adjacency has it: `level_faces_into` splits quad (a,b,c,d) into (a,b,c)
    // and (a,c,d), so a and c are ring neighbours there. Leaving it out here
    // would make a boundary vertex's ring differ from the dense hierarchy's by
    // exactly the face points.
    std::vector<std::uint32_t> ring_offsets, ring;

    bool empty() const { return corners.empty(); }
    std::uint32_t face_count() const { return static_cast<std::uint32_t>(dense_face.size()); }
    bool inside(std::uint32_t joined) const { return joined < vertex_count; }
    std::uint32_t joined_count() const {
        return vertex_count + static_cast<std::uint32_t>(outside_layout.size());
    }

    const std::uint32_t* face_corners(std::uint32_t f) const {
        return corners.data() + static_cast<std::size_t>(f) * 4u;
    }
    const std::uint32_t* faces_of(std::uint32_t v, std::size_t* count) const {
        return span(face_offsets, faces, v, count);
    }
    const std::uint32_t* ring_of(std::uint32_t v, std::size_t* count) const {
        return span(ring_offsets, ring, v, count);
    }

    // The joined id's position: the level's own array below `vertex_count`, and
    // the pure subdivision above it.
    kernel::cfloat3 position(const std::vector<kernel::cfloat3>& own,
                             std::uint32_t joined) const {
        return joined < vertex_count ? own[joined]
                                     : outside_positions[joined - vertex_count];
    }

    // The angle-weighted contribution the derived faces make to the geometric
    // normal at a joined id, over the same triangulation and the same
    // arithmetic the level's own triangles are summed with. Zero — and no work
    // — for a vertex with no derived face, which is every vertex away from a
    // depth boundary.
    //
    // For an id this level STORES the contribution completes its normal, because
    // every incident face it does not store is here. For an OUTSIDE id it is
    // only the faces this structure holds, which is a subset once the id is
    // further out than the first ring; the one reader of that is polish's
    // per-neighbour normal, and a subset there is a shading weight rather than a
    // stored value.
    kernel::cfloat3 normal_contribution(const std::vector<kernel::cfloat3>& own,
                                        std::uint32_t joined) const;

    // How many DERIVED triangles have both `a` and `b` among their corners:
    // exactly the count `is_boundary_class`'s own shared-triangle count is
    // short by at a depth boundary, and 0 at a real open border.
    int shared_triangles(std::uint32_t a, std::uint32_t b) const;

    std::size_t bytes() const;

   private:
    static const std::uint32_t* span(const std::vector<std::uint32_t>& offsets,
                                     const std::vector<std::uint32_t>& values, std::uint32_t i,
                                     std::size_t* count) {
        if (i + 1 >= offsets.size()) {
            *count = 0;
            return nullptr;
        }
        const std::uint32_t begin = offsets[i], end = offsets[i + 1];
        *count = end - begin;
        return values.data() + begin;
    }
};

// Whether a level stores every child of every face of its parent — so there is
// nothing outside it and `build_cross_level` answers empty without reading the
// parent at all.
//
// It is a property of the CHILD alone, and that is the whole reason it is named
// here rather than left inside `build_cross_level`: a caller holding a level
// whose parent's cache has been released (`drop_intermediate_caches` releases
// exactly the levels between the cage and the one being worked on) has to know
// whether it must pay that trim back before it can ask. On a uniform hierarchy
// it never does.
bool level_is_self_contained(const LevelTopology& child, const std::vector<char>& keep);

// The neighbourhood of the level `child` describes, whose parent is `parent`.
//
// `keep` is the child level's `patch_kept`: one entry per base patch, and EMPTY
// on a level that stores every patch — where the answer is empty, because a
// dense level's own connectivity is already complete. `parent_positions` is
// P(parent), which is what the subdivision reads.
//
// The result is a pure function of its inputs, so releasing it and rebuilding
// it produces the same bytes — which is what lets it live in a level cache
// beside everything else derived.
CrossLevelNeighborhood build_cross_level(const LevelTopology& parent,
                                         const LevelConnectivity& parent_conn,
                                         const std::vector<kernel::cfloat3>& parent_positions,
                                         const LevelTopology& child,
                                         const std::vector<char>& keep);

// Re-read the outside positions from a parent that has moved, leaving the
// topology alone. What a level below being sculpted costs the level above.
void refresh_cross_level(const LevelTopology& parent, const LevelConnectivity& parent_conn,
                         const std::vector<kernel::cfloat3>& parent_positions,
                         CrossLevelNeighborhood* inout);

}  // namespace mesh
}  // namespace clay
