#pragma once

// The shared state of a multiresolution surface, private to its three
// translation units (mesh-multires spec, add-mesh-multires).
//
// `multires.cpp` owns the lifecycle — building from a cage, adding and removing
// levels, pricing them, accounting for them, releasing caches.
// `multires_eval.cpp` owns evaluation and propagation.
// `multires_serialize.cpp` owns the byte form.
//
// Split because the first is about a surface's shape over time and the second
// is a hot path measured in microseconds, and one file holding both is a file
// nobody reads twice.

#include <memory>
#include <vector>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/detail_field.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/subdivide.h"
#include "clay/mesh/surface_chunks.h"
#include "clay/mesh/surface_frame.h"

namespace clay {
namespace mesh {

// Everything about a level that can be thrown away and rebuilt bit-identically.
struct LevelCache {
    LevelConnectivity conn;
    // S(n): the pure subdivision. Empty at level 0, where S and P are the same
    // array and duplicating it would cost twelve bytes a vertex to say so.
    std::vector<kernel::cfloat3> subdivided;
    // Built from S(n) and the parent's frames. Carries the S-normal in
    // `SurfaceFrame::normal`, which is why no separate array of those exists.
    std::vector<SurfaceFrame> frames;
    // P(n) in `positions`, the DISPLAY normals in `normals`, and the faces when
    // something has asked for them. This is the array the sculptor writes
    // through, so the level's mesh and the level's positions are one thing
    // rather than two that can drift.
    Mesh mesh;
    bool faces_built = false;
    std::unique_ptr<Adjacency> adjacency;
    bool evaluated = false;

    // The level's chunks, and the face -> chunk map that marks them.
    //
    // IN THE CACHE, so every `drop_*_caches` releases them and a rebuild
    // reproduces them exactly: the partition is a function of the level's own
    // face order and patch ids, both of which are authoritative. The cost of
    // that placement is stated rather than hidden — dropping a level's cache
    // drops its dirty set too, so a host that dropped a level it was still
    // uploading has to re-read it whole. That is the same level it just told
    // the engine it was not looking at.
    ChunkTable chunks;
    std::vector<std::uint32_t> face_chunk;

    // Split by what a memory report separates: the arrays that ARE the
    // evaluated surface, and the index structures built to work on it.
    //
    // Reported as two positive sums rather than a total with the first
    // subtracted out of it. The subtraction was correct — both sides measured
    // capacity — and it was one edit away from not being: drop a term from the
    // total and the difference underflows a `std::size_t` into a memory report
    // of sixteen exabytes, which a host would act on.
    struct Bytes {
        std::size_t evaluated = 0;
        std::size_t runtime = 0;
        std::size_t chunk_index = 0;
        std::size_t total() const { return evaluated + runtime + chunk_index; }
    };
    Bytes byte_split() const;
    std::size_t bytes() const { return byte_split().total(); }
};

struct MultiresLevel {
    LevelTopology topology;
    // Empty at level 0: the cage's positions are the authoritative coarse
    // geometry, not a displacement layer over something else.
    DetailField detail;
    // Kept so `preflight_add_level` is arithmetic rather than a build: the edge
    // count of a level follows from the level below it.
    std::uint64_t edge_count = 0;

    std::unique_ptr<LevelCache> cache;

    // Vertices at THIS level that changed and have not yet been pushed to the
    // level above. `pending_all` is the same statement about every vertex,
    // which is what a freshly built or freshly reloaded level is.
    std::vector<std::uint32_t> pending;
    bool pending_all = true;

    // Vertices at this level whose DISPLAY normal is stale because something
    // wrote detail straight into the level — an undo replaying a gesture, or a
    // verb that erases detail. The evaluation drains it. Separate from
    // `pending` because a stale display normal shades wrong and changes nothing
    // about the level above, while `pending` is the opposite.
    std::vector<std::uint32_t> normals_pending;
};

// The cage's attributes, subdivided. Rebuildable, and built only when something
// asks to export a level of a cage that carries any.
struct AttrLevel {
    // Empty unless the cage has attribute SPLITS — duplicate raw vertices at
    // one geometric point, which is how a flat mesh writes a UV seam. Without
    // them the attribute connectivity is the geometric one and a second copy
    // would be a second copy of the same numbers.
    LevelTopology topology;
    LevelConnectivity conn;
    std::vector<std::uint32_t> to_geom;
    std::vector<kernel::cfloat2> uvs;
    std::vector<kernel::cfloat3> colors;

    std::size_t bytes() const;
};

struct MultiresSurface::State {
    MultiresOptions options;

    // The cage. Its positions are kept in step with level 0's, member for
    // member, so a seam's duplicates stay coincident and cannot open a crack.
    Mesh base;
    std::vector<std::uint32_t> class_of;  // raw vertex -> geometric vertex
    std::vector<std::uint32_t> class_offsets, class_members;
    std::uint32_t class_count = 0;
    // Whether the cage duplicates any position. TRUE means a UV seam or a hard
    // edge is represented the way a flat mesh represents one, and the
    // attributes need their own connectivity.
    bool attribute_split = false;

    std::vector<MultiresLevel> levels;
    std::uint32_t sculpt_level = 0;
    std::uint32_t display_level = 0;

    std::uint64_t base_revision = 1;
    std::uint64_t detail_revision = 1;
    std::uint64_t evaluated_revision = 1;
    std::uint64_t cache_generation = 1;
    MultiresEvalStats stats;

    // Base patches touched since the host last drained them.
    std::vector<std::uint32_t> dirty_patches;
    std::vector<char> patch_dirty;

    // Level-0 vertices whose normal and frame are stale because the cage moved
    // under them. Drained at the start of an evaluation rather than at the
    // moment of the write, so a stroke pays one refresh instead of one per dab.
    std::vector<std::uint32_t> base_frames_dirty;
    bool base_frames_all = true;

    std::vector<AttrLevel> attr;

    // Scratch the hot path reuses rather than reallocating per stamp.
    std::vector<std::uint32_t> scratch_a, scratch_b, scratch_c;
    std::vector<kernel::cfloat3> scratch_normals;
    std::vector<char> scratch_mark;

    bool level_ok(std::uint32_t level) const {
        return level < static_cast<std::uint32_t>(levels.size());
    }
};

// -- shared between multires.cpp and multires_eval.cpp ------------------------

// Every vertex sharing a face with one of `in`, plus `in` itself. Sorted and
// free of duplicates.
//
// THE HALO, and why every level needs one. A vertex whose own position did not
// move still has a changed NORMAL when a neighbour moved, and a changed normal
// is a changed frame, and a changed frame moves whatever detail is stored
// against it. Propagating only the vertices whose subdivided position changed
// leaves a ring of stale detail around every dab.
void expand_by_face_ring(const LevelTopology& topology, const LevelConnectivity& conn,
                         const std::vector<std::uint32_t>& in, std::vector<char>* mark,
                         std::vector<std::uint32_t>* out);

// Evaluate every level up to and including `level`, doing only the work the
// edits since the last call actually require.
void evaluate_up_to(MultiresSurface::State& s, std::uint32_t level);

// Note that these level vertices changed, for the host's changed-block drain.
void mark_patches(MultiresSurface::State& s, std::uint32_t level,
                  const std::vector<std::uint32_t>& vertices);

// Partition a level's faces into chunks, once per cache lifetime. Requires the
// level to be evaluated: the chunk bounds are read from P(n). A no-op when the
// level's table is already built. Defined in `multires_chunks.cpp`.
void ensure_level_chunks(MultiresSurface::State& s, std::uint32_t level);

// Build the cage's attributes at `level`, if it has any. Returns false when
// there is nothing to build, which is not a failure.
bool ensure_attributes(MultiresSurface::State& s, std::uint32_t level);

// The connectivity of a level, built if it is not resident.
const LevelConnectivity& connectivity_of(MultiresSurface::State& s, std::uint32_t level);

// The cage's positions gathered per geometric vertex.
void gather_class_positions(const MultiresSurface::State& s, std::vector<kernel::cfloat3>* out);

}  // namespace mesh
}  // namespace clay
