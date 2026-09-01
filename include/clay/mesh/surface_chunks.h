#pragma once

// ONE CHUNK UNIT, FOR ALL THREE SURFACE REPRESENTATIONS (sculpt-runtime spec,
// add-extreme-poly-runtime).
//
// The requirement is that one unit serves the spatial-index leaf, the brush
// candidate set, the parallel work unit, the normal-recompute unit, the dirty
// set and the host's upload unit — and that a subsystem inventing a second
// granularity says why. The honest position is that this tree already had
// THREE, each defensible where it stood:
//
//   - `SurfaceLeaf` claims all six roles in its own header comment, and reaches
//     only `DynamicSurface`.
//   - `MeshSculptor` tracks dirty WELD CLASSES, because a weld class is the
//     identity its adjacency walk produces.
//   - `MultiresSurface` tracks dirty BASE PATCHES, because a base patch is the
//     only identity that survives subdivision.
//
// WHAT IS UNIFIED IS THE TABLE AND THE REVISION QUAD, NOT THE PARTITION RULE.
// Each representation supplies only a PARTITIONER — which faces are in which
// chunk — because that is the one thing that genuinely cannot be shared: the
// three do not have the same notion of a stable face. Everything downstream of
// the partition is this file: the identity space, the CSR arena, the epoch
// dirty set, the four revisions and the chunk-local vertex indexing a host
// uploads by.
//
// THE MULTIRES PARTITIONER IS WHERE THE "ONE GRANULARITY" CLAIM WOULD HAVE
// QUIETLY FAILED, so it is stated here rather than left to the caller. A base
// patch is not a fixed-size unit: Catmull-Clark quadruples faces per level, so
// one base quad owns 1024 faces at level 5 and 4096 at level 6 — four and
// sixteen times the target. So the multires chunk id is (base patch, quadrant
// at depth d), with d chosen per level to land on the target face count and
// d = 0 at levels coarse enough that the patch is already small. The identity
// is still stable under an edit, because a patch's subtree never moves between
// base faces, and the size is now fixed. That is the SAME granularity keyed by
// the only identity subdivision preserves, not a second one.
//
// ONE CSR ARENA, NOT A VECTOR PER CHUNK, and this is what makes the allocation
// gate reachable rather than aspirational. A chunk that owns its faces in a
// `std::vector<FaceId>` is a heap object per chunk: at 20M vertices and a
// 256-face chunk that is roughly 156k vectors, 156k allocations at build, and a
// reallocation every time membership changes — which is every stamp, on an
// adaptive surface. It also contradicts the transport requirement's "no heap
// object per chunk per frame" outright. So the table holds ONE face array with
// per-chunk blocks and a free list for the slack a split leaves behind, and a
// chunk holds a span into it.
//
// FOUR REVISIONS, NOT ONE. A single counter cannot say "geometry moved,
// connectivity did not, normals are still deferred, colours are unchanged", and
// a host that cannot distinguish those re-uploads an index buffer on every dab.
// The shipped single `revision` stays beside them, as the maximum of the four,
// because it is ABI a host already reads.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/math/geom.h"
#include "clay/memory/budget.h"
#include "clay/mesh/slot_pool.h"

namespace clay {
namespace mesh {

// The face handle, as `dynamic_surface.h` defines it. Declared rather than
// included: `SlotId` never names its tag beyond making the four element kinds
// distinct types, so the name alone is enough here — and pulling the whole
// half-edge surface into every consumer of a chunk table (the fixed sculptor,
// the hierarchy, the transport) to obtain a two-word POD would be a heavy
// include for nothing. A divergence between the two declarations is a compile
// error, not a silent mismatch.
struct FaceTag;
using FaceId = SlotId<FaceTag>;

// What a chunk aims to hold.
//
// THESE NUMBERS ARE THE NULL HYPOTHESIS AND NOT YET THE ANSWER. They are
// `DynamicBvhOptions`'s current defaults, which are prior art in the strict
// sense — they are in this tree and they were never measured here.
// `benchmarks/bench_surface_chunks.cpp` sweeps 64/128/256/512/1024 against
// query cost, false-positive touched vertices, normal recompute, upload bytes,
// a locality proxy and split/merge cost, and the decision rule is written in
// the change's design.md before the data exists. Until that has been RUN, this
// header does not claim a measured size.
struct ChunkOptions {
    std::size_t target_faces = 256;
    // Above this a chunk splits; below it two siblings merge. The gap is
    // hysteresis: a chunk hovering at the threshold must not split and merge on
    // alternate stamps.
    std::size_t max_faces = 512;
    std::size_t min_faces = 64;
};

// The parallel grain for chunk-level work. `parallel_for` runs a single task
// inline, so a footprint below this dispatches nothing at all — which is the
// serial threshold the spec asks for, expressed where the pool already reads
// it rather than as a second branch beside every call.
//
// ONE LEVEL ONLY. The pool runs a nested `parallel_for` inline by design, so
// parallel-over-chunks containing parallel-over-vertices is not a speedup, it
// is the outer loop's chunking silently becoming the whole parallelism. A
// caller picks the granularity that matches the work and does not nest.
inline constexpr std::size_t kChunkParallelGrain = 4;
// The same threshold for per-vertex work inside one chunk. Below it the gather,
// the weight pass and the write-back stay on the calling thread: at a few
// hundred vertices the dispatch is the measurement.
inline constexpr std::size_t kVertexParallelGrain = 1024;

// Which of the four revisions a change advances.
enum class ChunkDirty : std::uint32_t {
    // Membership changed: a face arrived, left, or the chunk split. The one a
    // host re-uploads an index buffer for.
    Topology = 0,
    // The same faces, in different places.
    Geometry = 1,
    // Positions unchanged, shading normals rewritten — which is what a deferred
    // normal flush produces and what a host would otherwise re-upload positions
    // for.
    Normals = 2,
    // Colour, mask, UV.
    Attributes = 3,
};

// The four counters. Monotonic and drawn from one table-wide sequence, so a
// comparison ACROSS chunks is meaningful: "this chunk changed after that one"
// is a question a host draining incrementally actually asks.
struct ChunkRevisions {
    std::uint64_t topology = 0;
    std::uint64_t geometry = 0;
    std::uint64_t normals = 0;
    std::uint64_t attributes = 0;

    std::uint64_t newest() const;
    bool operator==(const ChunkRevisions& o) const {
        return topology == o.topology && geometry == o.geometry && normals == o.normals &&
               attributes == o.attributes;
    }
    bool operator!=(const ChunkRevisions& o) const { return !(*this == o); }
};

// A chunk's faces, as a view into the table's one arena.
//
// It iterates as the `std::vector<FaceId>` it replaced did — `size`, `empty`,
// range-for, indexing — so every existing reader of a chunk's faces is
// unchanged. The pointer is refreshed by the table whenever the arena moves,
// which is the only reason this is a raw span rather than an offset resolved
// through the table on every access: the queries walk these in their innermost
// loop, and a double indirection there is the leaf test paying for the
// bookkeeping of the split that put it in place.
class ChunkFaceSpan {
  public:
    const FaceId* begin() const { return data_; }
    const FaceId* end() const { return data_ + count_; }
    const FaceId* data() const { return data_; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const FaceId& operator[](std::size_t i) const { return data_[i]; }
    // How much room the block has before it has to move. Not a promise about
    // the chunk; a diagnostic for the compaction the maintenance queue runs.
    std::size_t capacity() const { return capacity_; }

  private:
    friend class ChunkTable;
    const FaceId* data_ = nullptr;
    std::uint32_t offset_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t capacity_ = 0;
};

// A chunk's own vertices, ascending, with the mapping back to global identity.
//
// CHUNK-LOCAL INDEXING IS FOR DERIVED BUFFERS ONLY. A host uploads a chunk as
// a standalone draw whose indices start at zero, and a normal recompute over a
// chunk wants a dense array. Neither is a reason to renumber anything
// authoritative: the surface's own topology stays 32-bit global, because a
// local id is meaningless the moment a chunk splits and an authoritative
// structure that had to be rewritten on a split would make a stamp cost the
// chunk rather than the change.
class ChunkVertexSpan {
  public:
    const std::uint32_t* begin() const { return data_; }
    const std::uint32_t* end() const { return data_ + count_; }
    const std::uint32_t* data() const { return data_; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    std::uint32_t operator[](std::size_t i) const { return data_[i]; }
    // The chunk-local index of a global vertex, or `kNoLocal`. A binary search
    // over the ascending ids rather than a per-chunk hash map: the map would be
    // a heap object per chunk, which is the thing this file exists to remove.
    std::uint32_t local_of(std::uint32_t global) const;
    static constexpr std::uint32_t kNoLocal = 0xffffffffu;

  private:
    friend class ChunkTable;
    const std::uint32_t* data_ = nullptr;
    std::uint32_t offset_ = 0;
    std::uint32_t count_ = 0;
};

// ONE CHUNK. `SurfaceLeaf` is an alias for this — the adaptive surface's leaf
// IS this record now, rather than a second one beside it.
struct SurfaceChunk {
    math::Aabb bounds;
    ChunkFaceSpan faces;
    ChunkRevisions revisions;
    // The maximum of the four, and the counter the shipped ABI reports. Kept as
    // its own field rather than derived so that a host reading it does not have
    // to know the quad exists.
    std::uint64_t revision = 0;
    // Set when the faces moved but the membership did not, and when the
    // membership itself changed. A host re-uploads an index buffer only for the
    // second. A normals-only or attributes-only change reports as geometry:
    // the two booleans predate the quad and mean "re-upload this chunk's vertex
    // data", which is exactly what those are.
    bool geometry_dirty = false;
    bool topology_dirty = false;
    // The PARTITIONER's own back-pointer — a BVH node for the adaptive surface,
    // a base patch for a hierarchy. The table stores it and never reads it,
    // because every partitioner needs somewhere to put exactly one word and a
    // parallel array per representation would be a second identity space.
    std::uint32_t node = 0xffffffffu;
    bool live = false;
};

// The one table. Owns the identity space, the CSR arena, the epoch dirty set
// and the revisions; owns no geometry and knows no representation.
class ChunkTable {
  public:
    static constexpr std::uint32_t kNoChunk = 0xffffffffu;

    ChunkTable() = default;
    // Copyable, because the spans are pointers INTO the copy's own arena and
    // therefore have to be re-pointed rather than memcpy'd. A defaulted copy
    // would leave the copy reading the ORIGINAL's storage — a dangling read the
    // moment either is destroyed, and one that would have worked in every test
    // that copied a table and kept the original alive.
    ChunkTable(const ChunkTable& other);
    ChunkTable& operator=(const ChunkTable& other);
    ChunkTable(ChunkTable&&) noexcept;
    ChunkTable& operator=(ChunkTable&&) noexcept;

    void clear();
    // Drop every chunk and hand the arena back. What a partitioner calls before
    // it repartitions.
    void reset(std::size_t expected_chunks, std::size_t expected_faces);

    const ChunkOptions& options() const { return options_; }
    void set_options(const ChunkOptions& options) { options_ = options; }

    // -- identity -------------------------------------------------------------

    // A new live chunk with no faces. Reuses a released slot when there is one,
    // so a stroke of splits and merges does not grow the identity space without
    // bound — and a reused id is a DIFFERENT chunk, which is why every reuse
    // bumps the topology revision.
    std::uint32_t create();
    void release(std::uint32_t chunk);

    std::size_t slot_count() const { return chunks_.size(); }
    std::size_t live_count() const { return live_count_; }
    const SurfaceChunk* chunk(std::uint32_t index) const;
    SurfaceChunk* chunk_mutable(std::uint32_t index);

    // -- membership -----------------------------------------------------------

    void add_face(std::uint32_t chunk, FaceId face);
    // Removes by SLOT, not by handle: the caller erasing a face has usually
    // already killed it, so the generation it holds no longer matches anything.
    bool remove_face(std::uint32_t chunk, std::uint32_t slot);
    void assign_faces(std::uint32_t chunk, const FaceId* faces, std::size_t count);
    // Keep the first `count`. What a split does to the half it keeps.
    void truncate_faces(std::uint32_t chunk, std::size_t count);
    // The block, writable in place, for a partitioner that sorts it. NEVER
    // resizes; the span's own bounds are the contract.
    FaceId* faces_mutable(std::uint32_t chunk);

    // -- chunk-local vertices --------------------------------------------------

    // `ids` must be ascending and unique; that is what makes `local_of` a
    // binary search. Copied into the vertex arena, so the caller's buffer is
    // its own.
    void set_vertices(std::uint32_t chunk, const std::uint32_t* ids, std::size_t count);
    ChunkVertexSpan vertices(std::uint32_t chunk) const;
    void clear_vertices();

    // -- bounds ----------------------------------------------------------------

    void set_bounds(std::uint32_t chunk, const math::Aabb& bounds);
    void expand_bounds(std::uint32_t chunk, const math::Aabb& bounds);

    // -- what changed -----------------------------------------------------------

    // Advance one revision and put the chunk in the dirty set.
    //
    // BY EPOCH MARK, not by a hash set per dab: a chunk carries the epoch it
    // was last marked in, so the list holds one entry per chunk however many
    // times a stamp touches it, and the clear is an increment.
    void mark(std::uint32_t chunk, ChunkDirty what);

    // The chunks marked since the last `clear_dirty`, in the order they were
    // first marked.
    //
    // MAY NAME A CHUNK THAT HAS SINCE BEEN RELEASED. A partitioner that merges
    // or retires a chunk does not walk this list to remove it — the whole point
    // of the epoch mark is that nothing walks the list — so `chunk()` returns
    // null for it and a drain SKIPS it rather than assuming every id here is
    // live. The alternative is an O(dirty) erase on the release path, which
    // runs during a stroke to save a null check that does not.
    const std::vector<std::uint32_t>& dirty() const { return dirty_; }
    void clear_dirty();

    // Retire ONE chunk from the dirty set, and only if it has not changed since
    // the caller read it.
    //
    // This is what `clear_dirty` structurally cannot express. A host that
    // drains half a dirty set and then drops a frame has to either re-upload
    // everything or lose a change; acknowledging per chunk against the revision
    // the host actually copied means a chunk that changed again between the
    // copy and the acknowledgement stays dirty. Returns whether the chunk is
    // clean afterwards — true when it was retired and true when it was never
    // dirty, false only when it moved on after the caller read it.
    bool acknowledge(std::uint32_t chunk, const ChunkRevisions& seen);

    std::uint64_t revision() const { return revision_; }

    // -- storage ----------------------------------------------------------------

    std::size_t bytes() const;
    void report(memory::MemoryLedger* ledger) const;
    // Face slots the arena holds and no chunk is using. What a compaction would
    // give back, and the figure the maintenance queue schedules on.
    std::size_t arena_slack() const { return face_arena_.size() - live_faces_; }
    // Rewrite the arena with every live chunk's faces packed in chunk order.
    // A MAINTENANCE ITEM, never a stamp: it moves every span, so it runs
    // between interactions and not inside a pointer event.
    void compact();

  private:
    void repoint_spans();
    void repoint_span(std::uint32_t chunk);
    std::uint32_t allocate_block(std::uint32_t capacity);
    void release_block(std::uint32_t offset, std::uint32_t capacity);
    void grow_block(std::uint32_t chunk, std::uint32_t needed);
    void enter_dirty(std::uint32_t chunk);

    ChunkOptions options_;
    std::vector<SurfaceChunk> chunks_;
    std::vector<FaceId> face_arena_;
    std::vector<std::uint32_t> vertex_arena_;
    std::vector<ChunkVertexSpan> chunk_vertices_;
    // Released blocks by size class, smallest first. A block's capacity is
    // always a power of two times the minimum, so a released block fits exactly
    // one class and the free list never has to search.
    std::vector<std::vector<std::uint32_t>> free_blocks_;
    std::vector<std::uint32_t> free_chunks_;
    std::vector<std::uint32_t> dirty_;
    std::vector<std::uint32_t> dirty_epoch_;
    // Where a dirty chunk sits in `dirty_`, so an acknowledgement retires it in
    // constant time instead of searching the list a stroke keeps refilling.
    std::vector<std::uint32_t> dirty_slot_;
    std::uint32_t epoch_ = 1;
    std::uint64_t revision_ = 0;
    std::size_t live_count_ = 0;
    // The faces the live chunks actually hold, so the arena's slack is a
    // subtraction rather than a walk over every chunk on every query.
    std::size_t live_faces_ = 0;
};

// The name the adaptive surface's leaf had, and still has. It IS a chunk now
// rather than a structure beside one — which is the whole of "one unit".
using SurfaceLeaf = SurfaceChunk;

}  // namespace mesh
}  // namespace clay
