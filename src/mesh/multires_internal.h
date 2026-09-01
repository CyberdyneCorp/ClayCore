#pragma once

// The shared state of a multiresolution surface, private to the translation
// units that implement it (mesh-multires spec, add-mesh-multires;
// mesh-sculpt-layers spec, add-mesh-sculpt-layers).
//
// `multires.cpp` owns the lifecycle — building from a cage, adding and removing
// levels, pricing them, accounting for them, releasing caches.
// `multires_eval.cpp` owns evaluation and propagation.
// `multires_serialize.cpp` owns the byte form.
// `sculpt_layer_eval.cpp` owns composition — turning the layer stack into the
// one field evaluation reads — and the surface operations that write outside
// the stack, which is baking a base deformation layer into the cage.
//
// Split because the first is about a surface's shape over time and the others
// are hot paths measured in microseconds, and one file holding both is a file
// nobody reads twice.

#include <memory>
#include <vector>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/detail_field.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/sculpt_layer.h"
#include "clay/mesh/subdivide.h"
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
        std::size_t total() const { return evaluated + runtime; }
    };
    Bytes byte_split() const;
    std::size_t bytes() const { return byte_split().total(); }
};

struct MultiresLevel {
    LevelTopology topology;
    // Empty at level 0: the cage's positions are the authoritative coarse
    // geometry, not a displacement layer over something else.
    //
    // STILL EXACTLY WHAT IT WAS after `add-mesh-sculpt-layers`: the BASE
    // detail, authoritative, what a stroke with no active layer writes, and
    // what `detail_checksum` hashes. The layer stack composes ON TOP of it into
    // the sibling below rather than replacing it.
    DetailField detail;
    // E(n) = B(n) + Σ sᵢ·mᵢ·Lᵢ, materialized on the SAME block grid every
    // layer and mask uses. REBUILDABLE, and released with the rest of the
    // caches.
    //
    // NULL WHEN NO LAYER REACHES THIS LEVEL, which is the whole of the
    // bit-parity promise: with no stack `apply_detail` reads `detail` through
    // the call it always made, so a hierarchy with no layers evaluates to the
    // same bits it did before this change. An implementation that always
    // composed — even summing an empty stack — would move the last bits of
    // every vertex and quietly break every existing multires golden.
    //
    // Materialized rather than summed on the fly because partial evaluation
    // re-frames and re-applies a stroke's halo every stamp, and summing 128
    // layers inside that pointer loop would put the stack depth inside the
    // gesture.
    std::unique_ptr<DetailField> composed;
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

    // The artist's channels over this hierarchy. Owned here for the reason
    // `multires.h` gives.
    SculptLayerStack stack;

    // THE CAGE'S REST FRAMES, and the third frame notion in this file — which
    // is the cost of base deformation layers and is worth stating plainly.
    //
    // At level 0 the cage IS the surface, so `LevelCache::frames` is built from
    // the evaluated positions. With a base deformation layer those positions
    // already include the layer's own contribution, so a coefficient read
    // against them would be measured in a frame that moves with the thing it
    // measures — the offset would fight itself the moment the slider moved.
    //
    // So a level-0 layer's coefficients are read against frames built over the
    // cage's REST positions: `State::base`, which stays authoritative and
    // unlayered. `LevelCache::frames` keeps its present meaning as the
    // transport frame for level 1, built from the deformed cage — which is
    // exactly right, because level-1 detail should ride on the proportion pass.
    //
    // Allocated ONLY when a level-0 layer exists, at the smallest level in the
    // hierarchy, so a hierarchy with no base layer pays nothing. Rejected:
    // world-space offsets at level 0 on the grounds that "there is nothing
    // underneath the cage" — there is, the cage is sculptable underneath a
    // proportion pass, and a world-space offset would not follow it.
    struct BaseRestFrames {
        std::vector<kernel::cfloat3> positions;  // the cage, per geometric vertex
        std::vector<kernel::cfloat3> normals;
        std::vector<SurfaceFrame> frames;
        bool valid = false;
    };
    std::unique_ptr<BaseRestFrames> base_rest;

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

// Build the cage's attributes at `level`, if it has any. Returns false when
// there is nothing to build, which is not a failure.
bool ensure_attributes(MultiresSurface::State& s, std::uint32_t level);

// The connectivity of a level, built if it is not resident.
const LevelConnectivity& connectivity_of(MultiresSurface::State& s, std::uint32_t level);

// The cage's positions gathered per geometric vertex.
void gather_class_positions(const MultiresSurface::State& s, std::vector<kernel::cfloat3>* out);

// -- shared with sculpt_layer_eval.cpp ----------------------------------------

// Tell the stack how many vertices each level has. Called whenever the level
// set changes, because a coefficient stored against a vertex count that no
// longer exists names a different vertex.
void sync_stack_levels(MultiresSurface::State& s);

// The field `apply_detail` reads at this level: the composed one when a layer
// reaches here, and the level's own base detail when none does. THE SAME CALL
// in both cases, which is what makes the no-layer path bit-identical.
const DetailField& effective_detail(const MultiresLevel& level);

// Bring the level's composed field up to date, recomposing the blocks the stack
// marked dirty and nothing else. Appends the vertices of every block it
// recomposed to `out_touched`, which is what makes a strength change propagate
// through the levels above exactly as a stroke does. A no-op — and no
// allocation — on a level no layer reaches.
void ensure_composed(MultiresSurface::State& s, std::uint32_t level,
                     std::vector<std::uint32_t>* out_touched);

// Is there composition work waiting anywhere? What `already_current` asks so a
// strength change is not silently swallowed by an up-to-date hierarchy.
bool composition_pending(const MultiresSurface::State& s);

// The cage's rest frames, built if a level-0 layer needs them. Null when none
// does.
const MultiresSurface::State::BaseRestFrames* base_rest_frames(MultiresSurface::State& s);

// A level-0 layer's total world offset at one geometric vertex, in the cage's
// rest frame. Zero — and no work — when no layer reaches level 0.
kernel::cfloat3 base_layer_offset(MultiresSurface::State& s, std::uint32_t vertex);

}  // namespace mesh
}  // namespace clay
