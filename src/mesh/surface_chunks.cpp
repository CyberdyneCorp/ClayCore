#include "clay/mesh/surface_chunks.h"

#include <algorithm>

namespace clay {
namespace mesh {
namespace {

// The smallest block the arena hands out, and the ladder above it. Powers of
// two from here, so a released block belongs to exactly one class and the free
// list is an index rather than a search — the alternative, a best-fit search
// over arbitrary sizes, runs on the split path during a stroke and is the kind
// of allocator this file exists to avoid re-implementing badly.
constexpr std::uint32_t kMinBlockFaces = 16;

std::uint32_t class_of(std::uint32_t capacity) {
    std::uint32_t cls = 0;
    std::uint32_t size = kMinBlockFaces;
    while (size < capacity) {
        size <<= 1;
        ++cls;
    }
    return cls;
}

std::uint32_t class_capacity(std::uint32_t cls) { return kMinBlockFaces << cls; }

}  // namespace

std::uint64_t ChunkRevisions::newest() const {
    return std::max(std::max(topology, geometry), std::max(normals, attributes));
}

std::uint32_t ChunkVertexSpan::local_of(std::uint32_t global) const {
    const std::uint32_t* found = std::lower_bound(data_, data_ + count_, global);
    if (found == data_ + count_ || *found != global) return kNoLocal;
    return static_cast<std::uint32_t>(found - data_);
}

// -- construction ---------------------------------------------------------------

ChunkTable::ChunkTable(const ChunkTable& other) { *this = other; }

ChunkTable& ChunkTable::operator=(const ChunkTable& other) {
    if (this == &other) return *this;
    options_ = other.options_;
    chunks_ = other.chunks_;
    face_arena_ = other.face_arena_;
    vertex_arena_ = other.vertex_arena_;
    chunk_vertices_ = other.chunk_vertices_;
    free_blocks_ = other.free_blocks_;
    free_chunks_ = other.free_chunks_;
    dirty_ = other.dirty_;
    dirty_epoch_ = other.dirty_epoch_;
    dirty_slot_ = other.dirty_slot_;
    epoch_ = other.epoch_;
    revision_ = other.revision_;
    live_count_ = other.live_count_;
    live_faces_ = other.live_faces_;
    // THE COPY'S SPANS POINT AT THE COPY. A defaulted copy would have handed
    // this table spans into the other one's arena, which is a dangling read the
    // moment either is destroyed — and it would have worked in every test that
    // copied a table and kept the original alive.
    repoint_spans();
    return *this;
}

// The move is safe defaulted, and only because a moved `std::vector` transfers
// its buffer rather than copying it: the spans keep pointing at the same heap
// storage, which is now the destination's. The copy above cannot borrow that
// argument, which is why it is the one that has to repoint.
ChunkTable::ChunkTable(ChunkTable&& other) noexcept = default;
ChunkTable& ChunkTable::operator=(ChunkTable&& other) noexcept = default;

void ChunkTable::clear() {
    chunks_.clear();
    face_arena_.clear();
    vertex_arena_.clear();
    chunk_vertices_.clear();
    free_blocks_.clear();
    free_chunks_.clear();
    dirty_.clear();
    dirty_epoch_.clear();
    dirty_slot_.clear();
    epoch_ = 1;
    live_count_ = 0;
    live_faces_ = 0;
    // `revision_` is NOT reset. A host holding a revision from before a
    // repartition must see it as stale rather than as equal to a fresh chunk's,
    // and a counter that restarted would make those compare equal.
}

void ChunkTable::reset(std::size_t expected_chunks, std::size_t expected_faces) {
    clear();
    chunks_.reserve(expected_chunks);
    dirty_epoch_.reserve(expected_chunks);
    dirty_slot_.reserve(expected_chunks);
    chunk_vertices_.reserve(expected_chunks);
    face_arena_.reserve(expected_faces);
}

// -- identity -------------------------------------------------------------------

std::uint32_t ChunkTable::create() {
    std::uint32_t index;
    if (!free_chunks_.empty()) {
        index = free_chunks_.back();
        free_chunks_.pop_back();
        chunks_[index] = SurfaceChunk{};
    } else {
        index = static_cast<std::uint32_t>(chunks_.size());
        chunks_.push_back(SurfaceChunk{});
        dirty_epoch_.push_back(0);
        dirty_slot_.push_back(kNoChunk);
        chunk_vertices_.push_back(ChunkVertexSpan{});
    }
    chunks_[index].live = true;
    chunk_vertices_[index] = ChunkVertexSpan{};
    ++live_count_;
    // A REUSED ID IS A DIFFERENT CHUNK. A host holding the old one's revision
    // would otherwise compare it against the new one's and conclude nothing
    // changed, which is the acknowledgement path handing back somebody else's
    // geometry.
    mark(index, ChunkDirty::Topology);
    return index;
}

void ChunkTable::release(std::uint32_t chunk) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return;
    SurfaceChunk& c = chunks_[chunk];
    release_block(c.faces.offset_, c.faces.capacity_);
    live_faces_ -= c.faces.count_;
    c.faces = ChunkFaceSpan{};
    c.live = false;
    chunk_vertices_[chunk] = ChunkVertexSpan{};
    --live_count_;
    free_chunks_.push_back(chunk);
}

const SurfaceChunk* ChunkTable::chunk(std::uint32_t index) const {
    if (index >= chunks_.size() || !chunks_[index].live) return nullptr;
    return &chunks_[index];
}

SurfaceChunk* ChunkTable::chunk_mutable(std::uint32_t index) {
    if (index >= chunks_.size() || !chunks_[index].live) return nullptr;
    return &chunks_[index];
}

// -- the arena -------------------------------------------------------------------

void ChunkTable::repoint_span(std::uint32_t chunk) {
    ChunkFaceSpan& span = chunks_[chunk].faces;
    span.data_ = span.capacity_ == 0 ? nullptr : face_arena_.data() + span.offset_;
}

void ChunkTable::repoint_spans() {
    for (std::uint32_t i = 0; i < chunks_.size(); ++i) repoint_span(i);
    for (std::uint32_t i = 0; i < chunk_vertices_.size(); ++i) {
        ChunkVertexSpan& span = chunk_vertices_[i];
        span.data_ = span.count_ == 0 ? nullptr : vertex_arena_.data() + span.offset_;
    }
}

std::uint32_t ChunkTable::allocate_block(std::uint32_t capacity) {
    const std::uint32_t cls = class_of(capacity);
    if (cls < free_blocks_.size() && !free_blocks_[cls].empty()) {
        const std::uint32_t offset = free_blocks_[cls].back();
        free_blocks_[cls].pop_back();
        return offset;
    }
    const std::uint32_t offset = static_cast<std::uint32_t>(face_arena_.size());
    const FaceId* before = face_arena_.data();
    const std::size_t needed = face_arena_.size() + class_capacity(cls);
    // GEOMETRIC BY HAND. `resize` allocates exactly what is asked for, so a
    // stroke that splits a chunk every few dabs would reallocate the whole
    // arena every few dabs; reserving to twice the capacity makes the growth
    // amortized and the span repointing below rare.
    if (needed > face_arena_.capacity())
        face_arena_.reserve(std::max(needed, face_arena_.capacity() * 2));
    face_arena_.resize(needed);
    if (face_arena_.data() != before) repoint_spans();
    return offset;
}

void ChunkTable::release_block(std::uint32_t offset, std::uint32_t capacity) {
    if (capacity == 0) return;
    const std::uint32_t cls = class_of(capacity);
    if (free_blocks_.size() <= cls) free_blocks_.resize(cls + 1);
    free_blocks_[cls].push_back(offset);
}

void ChunkTable::grow_block(std::uint32_t chunk, std::uint32_t needed) {
    ChunkFaceSpan& span = chunks_[chunk].faces;
    if (needed <= span.capacity_) return;
    const std::uint32_t new_capacity = class_capacity(class_of(needed));
    const std::uint32_t new_offset = allocate_block(new_capacity);
    if (span.count_ != 0)
        std::copy(face_arena_.begin() + span.offset_,
                  face_arena_.begin() + span.offset_ + span.count_,
                  face_arena_.begin() + new_offset);
    release_block(span.offset_, span.capacity_);
    span.offset_ = new_offset;
    span.capacity_ = new_capacity;
    repoint_span(chunk);
}

void ChunkTable::add_face(std::uint32_t chunk, FaceId face) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return;
    grow_block(chunk, chunks_[chunk].faces.count_ + 1);
    ChunkFaceSpan& span = chunks_[chunk].faces;
    face_arena_[span.offset_ + span.count_] = face;
    ++span.count_;
    ++live_faces_;
}

bool ChunkTable::remove_face(std::uint32_t chunk, std::uint32_t slot) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return false;
    ChunkFaceSpan& span = chunks_[chunk].faces;
    for (std::uint32_t i = 0; i < span.count_; ++i) {
        if (face_arena_[span.offset_ + i].slot != slot) continue;
        // SWAP WITH THE LAST rather than shift: the order inside a chunk is not
        // load-bearing — every query sorts its result by slot before it is
        // handed back — and a shift would make an erase cost the chunk.
        face_arena_[span.offset_ + i] = face_arena_[span.offset_ + span.count_ - 1];
        --span.count_;
        --live_faces_;
        return true;
    }
    return false;
}

void ChunkTable::assign_faces(std::uint32_t chunk, const FaceId* faces, std::size_t count) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return;
    grow_block(chunk, static_cast<std::uint32_t>(count));
    ChunkFaceSpan& span = chunks_[chunk].faces;
    live_faces_ = live_faces_ - span.count_ + count;
    if (count != 0) std::copy(faces, faces + count, face_arena_.begin() + span.offset_);
    span.count_ = static_cast<std::uint32_t>(count);
}

void ChunkTable::truncate_faces(std::uint32_t chunk, std::size_t count) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return;
    ChunkFaceSpan& span = chunks_[chunk].faces;
    if (count >= span.count_) return;
    live_faces_ -= span.count_ - count;
    span.count_ = static_cast<std::uint32_t>(count);
}

FaceId* ChunkTable::faces_mutable(std::uint32_t chunk) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return nullptr;
    const ChunkFaceSpan& span = chunks_[chunk].faces;
    if (span.count_ == 0) return nullptr;
    return face_arena_.data() + span.offset_;
}

void ChunkTable::compact() {
    if (arena_slack() == 0) return;
    std::vector<FaceId> packed;
    packed.reserve(live_faces_);
    for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
        SurfaceChunk& c = chunks_[i];
        if (!c.live) continue;
        const std::uint32_t offset = static_cast<std::uint32_t>(packed.size());
        packed.insert(packed.end(), face_arena_.begin() + c.faces.offset_,
                      face_arena_.begin() + c.faces.offset_ + c.faces.count_);
        c.faces.offset_ = offset;
        // The block is now exactly its contents, so the next `add_face` grows
        // it. That is the trade a compaction makes: the memory back now against
        // one block move on the next membership change, which is why it is a
        // maintenance item and not something a stamp does.
        c.faces.capacity_ = c.faces.count_;
    }
    // `packed` was RESERVED at the live face count above, so the swap IS the
    // release: the old arena goes with `packed`'s destructor. The
    // `shrink_to_fit` that used to follow was doing nothing at all — libstdc++
    // implements it through `__shrink_to_fit_aux`, whose no-exceptions form
    // returns false without acting, and this library compiles its core with
    // `-fno-exceptions`. Harmless here and not harmless in
    // `memory/scratch.cpp`, where it was the whole of a release path; see the
    // note there.
    face_arena_.swap(packed);
    free_blocks_.clear();
    repoint_spans();
}

// -- chunk-local vertices ---------------------------------------------------------

void ChunkTable::set_vertices(std::uint32_t chunk, const std::uint32_t* ids, std::size_t count) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return;
    // APPEND-ONLY, with no free list of its own. The vertex map is rebuilt
    // wholesale by a partitioner and read by the transport; nothing adds one
    // vertex to one chunk on the interactive path, so a block allocator here
    // would be machinery serving no caller. `clear_vertices` is the reset.
    const std::uint32_t offset = static_cast<std::uint32_t>(vertex_arena_.size());
    const std::uint32_t* before = vertex_arena_.data();
    vertex_arena_.insert(vertex_arena_.end(), ids, ids + count);
    ChunkVertexSpan span;
    span.offset_ = offset;
    span.count_ = static_cast<std::uint32_t>(count);
    chunk_vertices_[chunk] = span;
    if (vertex_arena_.data() != before)
        repoint_spans();
    else
        chunk_vertices_[chunk].data_ = count == 0 ? nullptr : vertex_arena_.data() + offset;
}

ChunkVertexSpan ChunkTable::vertices(std::uint32_t chunk) const {
    if (chunk >= chunk_vertices_.size()) return ChunkVertexSpan{};
    return chunk_vertices_[chunk];
}

void ChunkTable::clear_vertices() {
    vertex_arena_.clear();
    for (ChunkVertexSpan& span : chunk_vertices_) span = ChunkVertexSpan{};
}

// -- bounds -------------------------------------------------------------------------

void ChunkTable::set_bounds(std::uint32_t chunk, const math::Aabb& bounds) {
    if (SurfaceChunk* c = chunk_mutable(chunk)) c->bounds = bounds;
}

void ChunkTable::expand_bounds(std::uint32_t chunk, const math::Aabb& bounds) {
    if (SurfaceChunk* c = chunk_mutable(chunk)) c->bounds.expand(bounds);
}

// -- what changed ---------------------------------------------------------------------

void ChunkTable::enter_dirty(std::uint32_t chunk) {
    if (dirty_epoch_[chunk] == epoch_) return;
    dirty_epoch_[chunk] = epoch_;
    dirty_slot_[chunk] = static_cast<std::uint32_t>(dirty_.size());
    dirty_.push_back(chunk);
}

void ChunkTable::mark(std::uint32_t chunk, ChunkDirty what) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return;
    SurfaceChunk& c = chunks_[chunk];
    const std::uint64_t next = ++revision_;
    switch (what) {
        case ChunkDirty::Topology:
            c.revisions.topology = next;
            c.topology_dirty = true;
            break;
        case ChunkDirty::Geometry:
            c.revisions.geometry = next;
            c.geometry_dirty = true;
            break;
        case ChunkDirty::Normals:
            c.revisions.normals = next;
            c.geometry_dirty = true;
            break;
        case ChunkDirty::Attributes:
            c.revisions.attributes = next;
            c.geometry_dirty = true;
            break;
    }
    c.revision = next;
    enter_dirty(chunk);
}

void ChunkTable::clear_dirty() {
    for (std::uint32_t i : dirty_) {
        if (i >= chunks_.size()) continue;
        chunks_[i].geometry_dirty = false;
        chunks_[i].topology_dirty = false;
        dirty_slot_[i] = kNoChunk;
    }
    dirty_.clear();
    ++epoch_;
}

bool ChunkTable::acknowledge(std::uint32_t chunk, const ChunkRevisions& seen) {
    if (chunk >= chunks_.size() || !chunks_[chunk].live) return false;
    if (dirty_epoch_[chunk] != epoch_) return true;  // never dirty; nothing to retire
    if (chunks_[chunk].revisions != seen) return false;

    const std::uint32_t slot = dirty_slot_[chunk];
    const std::uint32_t last = dirty_.back();
    dirty_[slot] = last;
    dirty_slot_[last] = slot;
    dirty_.pop_back();
    dirty_slot_[chunk] = kNoChunk;
    dirty_epoch_[chunk] = 0;
    chunks_[chunk].geometry_dirty = false;
    chunks_[chunk].topology_dirty = false;
    return true;
}

// -- storage ----------------------------------------------------------------------------

std::size_t ChunkTable::bytes() const {
    std::size_t n = sizeof(ChunkTable);
    n += chunks_.capacity() * sizeof(SurfaceChunk);
    n += face_arena_.capacity() * sizeof(FaceId);
    n += vertex_arena_.capacity() * sizeof(std::uint32_t);
    n += chunk_vertices_.capacity() * sizeof(ChunkVertexSpan);
    n += free_blocks_.capacity() * sizeof(std::vector<std::uint32_t>);
    for (const std::vector<std::uint32_t>& list : free_blocks_)
        n += list.capacity() * sizeof(std::uint32_t);
    n += free_chunks_.capacity() * sizeof(std::uint32_t);
    n += (dirty_.capacity() + dirty_epoch_.capacity() + dirty_slot_.capacity()) *
         sizeof(std::uint32_t);
    return n;
}

void ChunkTable::report(memory::MemoryLedger* ledger) const {
    if (ledger != nullptr) ledger->add(memory::MemoryCategory::ChunkIndex, bytes());
}

}  // namespace mesh
}  // namespace clay
