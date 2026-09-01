#pragma once

// THE ONE PLACE THAT KNOWS ALL THREE SURFACES (c-abi and sculpt-runtime specs,
// add-extreme-poly-runtime).
//
// A host at twenty million vertices asks three questions and they are the same
// three whichever representation it is holding: what changed, give me those
// bytes into a buffer I own, and what does this cost me. Answering them per
// representation is three C entry points, three pyclay methods and three host
// code paths whose dirty sets mean different things — which is the granularity
// confusion the shared chunk table exists to remove, arriving one layer up.
//
// So the transport is a READ SEAM over a `ChunkTable` plus whatever the
// representation needs to turn a chunk into vertices. The C ABI's job becomes
// marshalling rather than knowing what a multires level is.
//
// THE READ HALF ONLY, deliberately. Draining a dirty set is a WRITE, and it
// already has one shape for all three: `ChunkTable::acknowledge`, reached
// through `DynamicBvh::chunks_mutable`, `MultiresSurface::acknowledge_chunk`,
// or the table a caller owns for a fixed mesh. Duplicating it here would mean
// this type holding a mutable handle to a surface it exists to read, which is
// the seam a const readback path could not then use.
//
// CALLER-OWNED BUFFERS, AND NO BORROWED POINTERS. `copy_chunk` writes into
// storage the caller sized from a capacity query and returns nothing that
// points into the engine. The shipped dynamic-surface transport already states
// this rule and the reason has only got stronger: a mutation can move or free
// anything, and at this scale it does so mid-drag. An engine-owned buffer with
// a generation token would be a use-after-free that memory pressure finds
// first.
//
// A READBACK CARRIES BOTH REVISIONS. The one the caller asked for and the one
// the engine is at now, so a result superseded between the query and the copy
// is IDENTIFIABLE rather than merely wrong — a host that draws a stale chunk
// draws something the engine does not think it made, and nothing in the pixels
// says so.
//
// WELDED WHERE THE REPRESENTATION ALLOWS IT, UNWELDED WHERE IT DOES NOT. A
// fixed mesh and a multires level have a stable per-chunk vertex list, so a
// chunk uploads as its own vertices with local indices. An adaptive surface
// does not: its topology changes under the stamp that is being uploaded, so a
// per-chunk vertex map would have to be rebuilt per chunk per frame, which is
// exactly the heap object per chunk per frame the requirement forbids. Its
// chunks therefore upload as unwelded triangles, byte for byte as the shipped
// path already does.

#include <cstddef>
#include <cstdint>

#include "clay/kernel/shim.h"
#include "clay/memory/budget.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/subdivide.h"  // LevelTopology
#include "clay/mesh/surface_chunks.h"

namespace clay {
namespace mesh {

class DynamicSurface;
class DynamicSculptor;
class MeshSculptor;
class MultiresSurface;

enum class SurfaceKind : std::uint32_t {
    Fixed = 0,
    Adaptive = 1,
    Multires = 2,
};

// What one `copy_chunk` did, and against what.
struct ChunkReadback {
    std::uint32_t chunk = ChunkTable::kNoChunk;
    // What the chunk needs, whether or not anything was written. A capacity
    // query — every buffer null — fills exactly these two and returns.
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;

    // What the caller said it had seen, echoed back, and what the engine is at
    // now. Equal on a fresh readback.
    ChunkRevisions requested;
    ChunkRevisions current;
    // The engine moved on after the caller took its snapshot. The data written
    // is CURRENT — this is not a failure — but a host that is applying an older
    // frame's plan can tell that its plan is out of date.
    bool stale = false;
    // A buffer was too small, so nothing was written into that buffer. The
    // counts above say what it needed.
    bool truncated = false;
    bool ok = false;
};

// A non-owning read seam. Valid only while the surface it names is unchanged
// and alive; it is a call-site convenience, never something a host stores.
class SurfaceView {
  public:
    SurfaceView() = default;

    // A fixed mesh with a chunk table a caller partitioned over it. The table
    // is the caller's because `MeshSculptor` does not own one yet — see the
    // note on `partition_mesh_chunks`.
    static SurfaceView over_mesh(const Mesh& mesh, const ChunkTable& chunks);
    // The adaptive surface, whose table is its BVH's.
    static SurfaceView over_dynamic(const DynamicSurface& surface, const ChunkTable& chunks);
    // One multires level. Not const: reading a level's chunks evaluates it.
    static SurfaceView over_level(MultiresSurface& surface, std::uint32_t level);

    bool valid() const { return chunks_ != nullptr; }
    SurfaceKind kind() const { return kind_; }
    const ChunkTable& chunks() const { return *chunks_; }
    std::size_t chunk_count() const;
    // The dirty set, in chunk ids. Drained by acknowledging each one.
    const std::vector<std::uint32_t>& dirty_chunks() const;

    // Copy one chunk into caller-owned buffers.
    //
    // A null `positions`, `normals` and `indices` is the CAPACITY QUERY: it
    // writes nothing and reports what the chunk needs, so a host sizes once and
    // copies once. `expected` may be null, which means "I have not seen this
    // chunk before" and never reports stale.
    //
    // Positions and normals are three floats per vertex; indices are triangles.
    ChunkReadback copy_chunk(std::uint32_t chunk, const ChunkRevisions* expected,
                             float* positions, std::size_t position_capacity, float* normals,
                             std::size_t normal_capacity, std::uint32_t* indices,
                             std::size_t index_capacity) const;

  private:
    SurfaceKind kind_ = SurfaceKind::Fixed;
    const ChunkTable* chunks_ = nullptr;
    // Fixed and multires: a corner array plus the arrays a corner indexes.
    const std::uint32_t* corners_ = nullptr;   // fixed: mesh.indices
    const LevelTopology* topology_ = nullptr;  // multires: variable arity
    const kernel::cfloat3* positions_ = nullptr;
    const kernel::cfloat3* normals_ = nullptr;
    std::size_t vertex_count_ = 0;
    // Adaptive: the surface itself, because a face's corners are a walk rather
    // than an array and a position lives behind a generation check.
    const DynamicSurface* surface_ = nullptr;
};

// Partition a flat mesh's triangles into chunks.
//
// A FREE FUNCTION AND NOT A MEMBER OF `MeshSculptor`, deliberately and for now.
// The fixed sculptor tracks dirty WELD CLASSES, and whether that list is
// retired in favour of the chunk dirty set or kept beside it depends on how
// many classes a chunk holds — which is the measurement task 1.1 has not run
// yet. Answering it by wiring a second dirty set into the sculptor first would
// be the second granularity this change exists to remove, so what lands now is
// the partitioner the benchmark and the transport need, and the sculptor keeps
// its own list until there is a number to decide on.
//
// Deterministic: a median split in INDEX order with an index tie-break, so the
// same mesh partitions the same way on every run and every platform, exactly as
// the adaptive partitioner does in slot order.
void partition_mesh_chunks(const Mesh& mesh, const ChunkOptions& options, ChunkTable* out);

// -- the ledger ------------------------------------------------------------------
//
// Each representation answers for itself, in the shared vocabulary, so a host
// holding one of each gets one set of three totals rather than three reports it
// has to reconcile. `io::MemoryReport` is what adds them to a document.

void report_surface_memory(const MeshSculptor& sculptor, memory::MemoryLedger* ledger);
void report_surface_memory(const DynamicSculptor& sculptor, memory::MemoryLedger* ledger);
void report_surface_memory(const MultiresSurface& surface, memory::MemoryLedger* ledger);

// -- the trim --------------------------------------------------------------------

// Release rebuildable caches at a stated pressure, in the order the spec fixes:
// transient scratch beyond its steady capacity, preview staging, evaluated
// caches, spatial indices for inactive levels, derived positions for inactive
// levels, other rebuildable caches — and history only to the host's own policy,
// which is why nothing here touches it.
//
// NEVER AUTHORITATIVE CONTENT. Not the cage, not a level's topology, not a
// `DetailField`, not a sculpt layer, not a mask. The test for this is a
// checksum before and after rather than a reading of this file.
//
// A held `memory::MemoryPin` makes the call a no-op that reports what it WOULD
// have released, so a memory warning arriving mid-save gets an honest answer
// instead of a document mutating under the writer.
memory::TrimReport trim_surface(MultiresSurface& surface, memory::Pressure pressure,
                                const memory::TrimGate* gate = nullptr);
memory::TrimReport trim_surface(DynamicSculptor& sculptor, memory::Pressure pressure,
                                const memory::TrimGate* gate = nullptr);

}  // namespace mesh
}  // namespace clay
