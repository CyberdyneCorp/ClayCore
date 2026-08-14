#pragma once

// Vertex neighbourhoods over a triangle soup (meshing spec): what a mesh needs
// before a brush can smooth it, relax it, or measure a falloff along it.
//
// `mesh::Mesh` is a flat index buffer. Nothing in it says which vertices touch
// which, and nothing in it says that two vertices at the same POSITION are the
// same point of the surface — which they routinely are not, as far as the
// index buffer is concerned. A UV seam, a hard edge or a per-face normal all
// duplicate a position into two or more independent indices, and every mesh
// that came out of a DCC has them.
//
// So the ring is built over WELD CLASSES rather than over raw indices: vertices
// whose positions coincide within an epsilon are one class, and the
// neighbourhood graph is over classes. Two consequences, and they are the
// reason this is not a five-line loop over `indices`:
//
//   - A walk across the surface crosses a seam. Built over raw indices it
//     cannot: the two sides of a UV seam share no index, so a geodesic falloff
//     stops dead at the seam and a brush that straddles one moves half of what
//     it should.
//   - An operation writes one displacement per class, to ALL of that class's
//     members. Coincident duplicates stay coincident, so a seam cannot open
//     into a visible crack. That is structural here rather than a rule each
//     caller has to remember.
//
// Storage is CSR throughout — three offset/value pairs, no per-vertex
// containers — because a stroke rebuilds nothing and walks these arrays
// hundreds of times.
//
// This structure describes a TOPOLOGY. Positions may move under it freely (the
// mesh brushes do exactly that and nothing else); it goes stale only if the
// vertex or index count changes, which `matches` is what checks.

#include <cstdint>
#include <vector>

#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

// Weld epsilon as a fraction of the mesh's bounding-box diagonal. Relative
// rather than absolute so it means the same thing on a model authored in
// millimetres and one authored in metres — an absolute epsilon is either
// useless at one scale or fuses a thin wall to itself at the other.
inline constexpr float kDefaultWeldEpsilon = 1e-5f;

class Adjacency {
  public:
    // Build over `m`. `weld_epsilon` is relative to the bounding-box diagonal;
    // pass 0 for exact-bit welding, which is what a caller who generated the
    // mesh themselves and knows there are no near-duplicates wants.
    //
    // Degenerate triangles (two or three corners in one class) contribute no
    // ring edges — an edge from a class to itself is not a neighbourhood — but
    // they stay in `triangles_of`, because they are still triangles whose
    // normal a moved vertex invalidates.
    static Adjacency build(const Mesh& m, float weld_epsilon = kDefaultWeldEpsilon);

    std::size_t vertex_count() const { return class_of_.size(); }
    std::size_t class_count() const { return class_members_offsets_.empty()
                                          ? 0
                                          : class_members_offsets_.size() - 1; }
    std::size_t triangle_count() const { return triangle_count_; }

    // Which weld class a raw vertex index belongs to.
    std::uint32_t class_of(std::uint32_t vertex) const { return class_of_[vertex]; }

    // The raw vertex indices that make up a class. Never empty.
    const std::uint32_t* members(std::uint32_t cls, std::size_t* count) const {
        return span(class_members_offsets_, class_members_, cls, count);
    }

    // The classes sharing an edge with this one. Sorted ascending and free of
    // duplicates, which is what makes a walk over them deterministic.
    const std::uint32_t* ring(std::uint32_t cls, std::size_t* count) const {
        return span(ring_offsets_, ring_, cls, count);
    }

    // The triangles that have a corner in this class — what a normal
    // recomputation needs, and what tells a polish brush which faces to
    // compare.
    const std::uint32_t* triangles_of(std::uint32_t cls, std::size_t* count) const {
        return span(tri_offsets_, tris_, cls, count);
    }

    // Whether this adjacency still describes `m`. Fixed-topology operations
    // never invalidate it; a caller who reused one across two meshes has a bug
    // that this turns into a refusal instead of a read out of bounds.
    bool matches(const Mesh& m) const {
        return m.positions.size() == class_of_.size() &&
               m.indices.size() == triangle_count_ * 3;
    }

  private:
    static const std::uint32_t* span(const std::vector<std::uint32_t>& offsets,
                                     const std::vector<std::uint32_t>& values, std::uint32_t i,
                                     std::size_t* count) {
        std::uint32_t begin = offsets[i], end = offsets[i + 1];
        *count = end - begin;
        return values.data() + begin;
    }

    std::vector<std::uint32_t> class_of_;             // per raw vertex
    std::vector<std::uint32_t> class_members_offsets_;  // class_count + 1
    std::vector<std::uint32_t> class_members_;
    std::vector<std::uint32_t> ring_offsets_;  // class_count + 1
    std::vector<std::uint32_t> ring_;
    std::vector<std::uint32_t> tri_offsets_;  // class_count + 1
    std::vector<std::uint32_t> tris_;
    std::size_t triangle_count_ = 0;
};

// Scratch a bounded surface walk reuses between calls. A stroke resolves
// hundreds of stamps and each one walks a small neighbourhood of a mesh that
// may have a million vertices; allocating a per-class array per stamp is the
// entire cost of the stroke, so the caller keeps one of these.
struct WalkScratch {
    std::vector<float> distance;      // per class; kUnreached where untouched
    std::vector<std::uint32_t> dirty;  // classes to reset, so the reset is O(reached)
    static constexpr float kUnreached = -1.0f;
};

// Classes within `radius` of `seed_position`, measured ALONG THE SURFACE:
// bounded Dijkstra over the ring with Euclidean edge lengths, seeded at the
// class nearest `seed_position` among `seed_hint`'s ring (or searched linearly
// when no hint is given).
//
// WHAT THIS APPROXIMATES, so the header says it rather than a reader
// discovering it: the shortest path along EDGES, which overestimates true
// geodesic distance — by up to about 4% on a regular triangulation, more on a
// coarse or badly shaped one. That is the right approximation for a falloff,
// which is a soft weight: a slightly and uniformly smaller effective radius is
// invisible, while the property that matters — the chin is not reachable from
// the upper lip without walking around the mouth — is exact and is the whole
// reason this exists.
//
// Deterministic: frontier ties break on class index, so the same brush reaches
// the same classes in the same order on every platform.
//
// Appends (class, surface distance) pairs to `out_classes` / `out_distance`,
// which are CLEARED first.
void geodesic_region(const Mesh& m, const Adjacency& adj, kernel::cfloat3 seed_position,
                     float radius, WalkScratch& scratch, std::vector<std::uint32_t>* out_classes,
                     std::vector<float>* out_distance, std::uint32_t seed_hint = 0xffffffffu);

// The same set measured in a straight line: every class whose position is
// within `radius` of `centre`. What a verb whose meaning is "everything under
// this disc" wants, and what a surface walk would wrongly refuse to do across
// a groove.
void euclidean_region(const Mesh& m, const Adjacency& adj, kernel::cfloat3 centre, float radius,
                      std::vector<std::uint32_t>* out_classes, std::vector<float>* out_distance);

}  // namespace mesh
}  // namespace clay
