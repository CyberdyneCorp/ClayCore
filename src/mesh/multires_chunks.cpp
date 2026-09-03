// The multiresolution PARTITIONER: which faces of a level are in which chunk
// (sculpt-runtime spec, add-extreme-poly-runtime).
//
// A SEPARATE TRANSLATION UNIT, and not because this file is large. The
// hierarchy's evaluation and its detail summation are being changed on a
// sibling branch, and this change is stacked last; a partitioner appended to
// `multires_eval.cpp` would collide with that work for no reason other than
// sharing a file. Everything here is additive and reads the level's own
// authoritative topology.
//
// WHY THE CHUNK IS NOT SIMPLY THE BASE PATCH. `MultiresSurface` already tracks
// dirty base patches and `build_block` already uploads by one, so a patch looks
// like the chunk this hierarchy already has. It is not a FIXED-SIZE unit:
// Catmull-Clark quadruples faces per level, so one base quad owns 256 faces at
// level 4, 1024 at level 5 and 4096 at level 6, against a target of a few
// hundred. Treating a patch as a chunk would have satisfied "one unit" in the
// header and broken it in the arithmetic — a chunk that grows by 4x a level is
// not the same granularity as one that does not.
//
// So a chunk is a RUN OF CONSECUTIVE FACES IN PATCH-MAJOR ORDER, which is the
// (base patch, quadrant at depth d) identity from two directions:
//
//   - Where a patch holds MORE faces than the target, the run is a power-of-four
//     block of the patch's own face order — which IS the depth-d quadrant
//     subtree, because subdivision emits a patch's children depth-first.
//   - Where a patch holds FEWER (the cage, and the first level or two above
//     it), a chunk holds whole patches up to the target, so a 20k-quad cage is
//     not 20k chunks of one face each.
//
// Both are stable under an edit for the same reason the patch is: a patch's
// subtree never moves between base faces and the base face ids never change. A
// spatial partition would not be — it would re-partition as the surface moves,
// invalidating what a host has already uploaded, which is the failure
// `build_block`'s own comment records.

#include <algorithm>

#include "multires_internal.h"

namespace clay {
namespace mesh {
namespace {

// The largest power of four not exceeding `target`, and at least one. A run of
// 4^k faces inside a patch is exactly a quadrant subtree k levels down, so
// rounding the target to a power of four is what makes the chunk a subtree
// rather than an arbitrary slice of one.
std::size_t quadrant_run(std::size_t target) {
    std::size_t run = 1;
    while (run * 4 <= target) run *= 4;
    return run;
}

}  // namespace

void ensure_level_chunks(MultiresSurface::State& s, std::uint32_t level) {
    if (!s.level_ok(level) || s.levels[level].cache == nullptr) return;
    LevelCache& cache = *s.levels[level].cache;
    const LevelTopology& topology = s.levels[level].topology;
    if (!cache.face_chunk.empty() || topology.face_count == 0) return;

    const std::size_t target = std::max<std::size_t>(cache.chunks.options().target_faces, 1);
    const std::size_t run = quadrant_run(target);

    // Faces in PATCH-MAJOR ORDER, by counting sort. The level's own face order
    // is already patch-major everywhere subdivision produced it, but a counting
    // sort costs one pass and does not depend on that being true — and if it
    // ever stopped being true, the chunk would silently start spanning patches
    // rather than fail.
    const std::uint32_t patches = std::max<std::uint32_t>(topology.patch_count, 1);
    std::vector<std::uint32_t> counts(static_cast<std::size_t>(patches) + 1, 0);
    for (std::uint32_t f = 0; f < topology.face_count; ++f) {
        const std::uint32_t p = std::min(topology.patch_of(f), patches - 1);
        ++counts[static_cast<std::size_t>(p) + 1];
    }
    for (std::size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
    std::vector<std::uint32_t> offsets = counts;
    std::vector<std::uint32_t> ordered(topology.face_count);
    for (std::uint32_t f = 0; f < topology.face_count; ++f) {
        const std::uint32_t p = std::min(topology.patch_of(f), patches - 1);
        ordered[offsets[p]++] = f;
    }

    cache.chunks.reset(topology.face_count / target + patches + 1,
                       topology.face_count + target);
    cache.face_chunk.assign(topology.face_count, ChunkTable::kNoChunk);

    std::vector<FaceId> block;
    std::vector<std::uint32_t> vertices;
    block.reserve(target + run);
    auto publish = [&]() {
        if (block.empty()) return;
        const std::uint32_t chunk = cache.chunks.create();
        cache.chunks.assign_faces(chunk, block.data(), block.size());
        math::Aabb bounds;
        vertices.clear();
        for (FaceId f : block) {
            cache.face_chunk[f.slot] = chunk;
            std::uint32_t arity = 0;
            const std::uint32_t* corners = topology.face(f.slot, &arity);
            for (std::uint32_t i = 0; i < arity; ++i) {
                vertices.push_back(corners[i]);
                if (corners[i] < cache.mesh.positions.size())
                    bounds.expand(cache.mesh.positions[corners[i]]);
            }
        }
        std::sort(vertices.begin(), vertices.end());
        vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
        cache.chunks.set_vertices(chunk, vertices.data(), vertices.size());
        cache.chunks.set_bounds(chunk, bounds);
        block.clear();
    };

    std::uint32_t current_patch = patches;  // no patch yet
    std::size_t in_patch = 0;
    for (std::uint32_t index = 0; index < ordered.size(); ++index) {
        const std::uint32_t f = ordered[index];
        const std::uint32_t p = std::min(topology.patch_of(f), patches - 1);
        if (p != current_patch) {
            // A chunk never spans a patch boundary once it holds a whole
            // patch's worth: crossing one would give the chunk two subtrees
            // whose ids move independently, which is the stable identity gone.
            if (!block.empty() && block.size() + (counts[p + 1] - counts[p]) > target) publish();
            current_patch = p;
            in_patch = 0;
        }
        block.push_back(FaceId{f, 0});
        ++in_patch;
        // Inside an oversized patch the cut is at a quadrant boundary, so the
        // chunk is a subtree and not a slice.
        if (in_patch % run == 0 && block.size() >= target) publish();
    }
    publish();

    // A fresh partition is the state a host is about to read for the first
    // time, not a change it has to redraw.
    cache.chunks.clear_dirty();
}

ChunkTable& MultiresSurface::level_chunks(std::uint32_t level) {
    // Deliberately the same preparation as the const reader below: a caller
    // that is about to WRITE the bounds still needs them to have been built
    // from an evaluated level first, or its first refit repairs a partition
    // that was never made.
    static ChunkTable kEmptyMutable;
    if (!state_ || !state_->level_ok(level)) {
        kEmptyMutable.clear();
        return kEmptyMutable;
    }
    positions_at(level);
    ensure_level_chunks(*state_, level);
    return state_->levels[level].cache->chunks;
}

const ChunkTable& MultiresSurface::chunks_at(std::uint32_t level) {
    static const ChunkTable kEmpty;
    if (!state_ || !state_->level_ok(level)) return kEmpty;
    // The bounds are read from P(n), so the level has to have been evaluated.
    // Asking for the positions is how that is said everywhere else in this
    // class, and it does only the work the edits since the last call require.
    positions_at(level);
    ensure_level_chunks(*state_, level);
    return state_->levels[level].cache->chunks;
}

bool MultiresSurface::acknowledge_chunk(std::uint32_t level, std::uint32_t chunk,
                                        const ChunkRevisions& seen) {
    if (!state_ || !state_->level_ok(level) || state_->levels[level].cache == nullptr) return false;
    return state_->levels[level].cache->chunks.acknowledge(chunk, seen);
}

void MultiresSurface::clear_dirty_chunks(std::uint32_t level) {
    if (!state_ || !state_->level_ok(level) || state_->levels[level].cache == nullptr) return;
    state_->levels[level].cache->chunks.clear_dirty();
}

}  // namespace mesh
}  // namespace clay
