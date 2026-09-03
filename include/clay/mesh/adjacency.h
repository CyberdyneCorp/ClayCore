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
#include <utility>
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
    std::size_t class_count() const {
        return class_members_offsets_.empty() ? 0 : class_members_offsets_.size() - 1;
    }
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
        return m.positions.size() == class_of_.size() && m.indices.size() == triangle_count_ * 3;
    }

   private:
    static const std::uint32_t* span(const std::vector<std::uint32_t>& offsets,
                                     const std::vector<std::uint32_t>& values, std::uint32_t i,
                                     std::size_t* count) {
        std::uint32_t begin = offsets[i], end = offsets[i + 1];
        *count = end - begin;
        return values.data() + begin;
    }

    std::vector<std::uint32_t> class_of_;               // per raw vertex
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
    std::vector<float> distance;       // per class; kUnreached where untouched
    std::vector<std::uint32_t> dirty;  // classes to reset, so the reset is O(reached)
    // The walk's frontier, as an explicit binary heap rather than a
    // `std::priority_queue` built per call.
    //
    // THE CONTAINER IS THE WHOLE REASON. A `priority_queue` owns its vector, so
    // one constructed inside the walk allocated on EVERY dab of every geodesic
    // stroke — which is the default footprint for fourteen of the sixteen
    // verbs. `std::push_heap` and `std::pop_heap` over a buffer that lives here
    // are exactly what `priority_queue` calls internally, so the pop sequence
    // is unchanged; what changes is that the storage is reused.
    std::vector<std::pair<float, std::uint32_t>> frontier;
    // How many class positions the walk has MEASURED to find its own seed,
    // accumulated over every walk this scratch has served.
    //
    // The walk scans every class when it is given no seed, and that scan is the
    // one term in a stamp that follows the MODEL rather than the brush. It
    // cannot be timed on a shared box and it cannot be seen from outside any
    // other way — the region it produces is identical either way, which is the
    // whole point — so it is counted. A test asserts on it; nothing else reads
    // it.
    std::size_t seed_scan = 0;
    static constexpr float kUnreached = -1.0f;
};

// The classes a brush REACHES, which is not the same question as which
// classes are near it.
//
// A class is in the region when a path over the one-ring leads to it that
//
//   (a) never leaves the ball of `radius` about `seed_position`, and
//   (b) is itself no longer than `path_budget`.
//
// (a) is what makes the region exactly the set a straight-line falloff has
// something to say about: nothing outside the ball is admitted, so the weight
// at the rim is the falloff's own zero rather than wherever a walk happened to
// stop. (b) is what makes it GEODESIC rather than merely connected — the chin
// is inside the ball when the brush is on the upper lip, and the only path to
// it runs the long way round the mouth.
//
// WHY BOTH, rather than path length alone. The walk measures a path along
// EDGES, which overestimates true geodesic distance — by a few percent on a
// well-shaped irregular triangulation and by up to sqrt(2) on a structured
// grid, where a diagonal direction has to zigzag. A region bounded by path
// length alone therefore stops SHORT in some directions and not others, and
// the ragged rim that leaves is visible in a render. Bounding the region by
// the ball removes that entirely, and the path budget — deliberately looser
// than the radius, see kDefaultPathBudget — still costs the chin its place.
//
// Deterministic: frontier ties break on class index, so the same brush reaches
// the same classes in the same order on every platform.
//
// Appends (class, PATH distance) pairs to `out_classes` / `out_distance`,
// which are CLEARED first. The path distance is reported because a caller may
// want it; the weights the brushes apply come from the straight line.

// How much longer than the radius a path may be, and where the weight starts
// fading out toward it.
//
// The budget covers the structured-grid worst case (a zigzagged diagonal costs
// sqrt(2) times the straight line) with room to spare, and is still short
// enough that a long detour inside the ball costs a class its place. It buys no
// extra work on an ordinary surface, because the ball bounds the region first.
//
// The TAPER is what keeps the region's rim smooth where the two bounds
// disagree. On a strongly curved surface — the inside of one prong of a fork —
// the walk can stop at a class whose straight-line weight is still appreciable,
// and an abrupt stop there is a visible step. Fading the weight to zero over
// the last stretch of the budget removes it. The taper starts ABOVE the
// structured-grid worst case on purpose: below that it would fire on an
// ordinary flat sheet, where there is nothing to fix and the straight-line
// falloff is exactly right.
inline constexpr float kDefaultPathBudget = 2.0f;
inline constexpr float kPathTaperStart = 1.5f;

void geodesic_region(const Mesh& m, const Adjacency& adj, kernel::cfloat3 seed_position,
                     float radius, WalkScratch& scratch, std::vector<std::uint32_t>* out_classes,
                     std::vector<float>* out_distance, std::uint32_t seed_hint = 0xffffffffu,
                     float path_budget_scale = kDefaultPathBudget);

// The same set measured in a straight line: every class whose position is
// within `radius` of `centre`. What a verb whose meaning is "everything under
// this disc" wants, and what a surface walk would wrongly refuse to do across
// a groove.
void euclidean_region(const Mesh& m, const Adjacency& adj, kernel::cfloat3 centre, float radius,
                      std::vector<std::uint32_t>* out_classes, std::vector<float>* out_distance);

}  // namespace mesh
}  // namespace clay
