#include "clay/mesh/topology_delta.h"

#include <cstring>

namespace clay {
namespace mesh {
namespace {

// The `note` half, shared by the four kinds: the FIRST sighting of a slot wins,
// which is what makes the record coalesced over a gesture. A later note on the
// same slot is a no-op, so a vertex touched by forty stamps keeps the position
// it had before the first of them.
template <typename Pool, typename Id, typename Vec, typename Map>
void note_element(const Pool& pool, Id id, Vec& list, Map& index) {
    if (!id.valid()) return;
    if (index.find(id.slot) != index.end()) return;
    typename Vec::value_type entry;
    entry.before_id = id;
    entry.after_id = id;
    entry.existed_before = pool.live(id);
    if (entry.existed_before) {
        entry.before = pool.at(id);
        entry.after = entry.before;
    }
    entry.exists_after = entry.existed_before;
    index.emplace(id.slot, static_cast<std::uint32_t>(list.size()));
    list.push_back(entry);
}

// The same, for an element the operator has just created: whatever the pool
// says now, it did NOT exist when the gesture started.
template <typename Id, typename Vec, typename Map>
void note_new_element(Id id, Vec& list, Map& index) {
    if (!id.valid()) return;
    if (index.find(id.slot) != index.end()) return;
    typename Vec::value_type entry;
    entry.before_id = id;
    entry.after_id = id;
    entry.existed_before = false;
    entry.exists_after = true;
    index.emplace(id.slot, static_cast<std::uint32_t>(list.size()));
    list.push_back(entry);
}

// The `sync` half: the LAST sighting wins. A slot the gesture created and then
// deleted ends up with `exists_after` false, which is what makes an
// apply-then-revert pair exact rather than merely close.
template <typename Pool, typename Id, typename Vec, typename Map>
void sync_element(const Pool& pool, Id id, Vec& list, Map& index) {
    if (!id.valid()) return;
    auto it = index.find(id.slot);
    if (it == index.end()) return;
    auto& entry = list[it->second];
    const Id current = pool.id_at(id.slot);
    entry.exists_after = current.valid();
    if (entry.exists_after) {
        entry.after = pool.at(current);
        entry.after_id = current;
    } else {
        // Deleted. The erase bumped the generation, and THAT is the generation
        // a re-apply has to retire the slot at.
        entry.after_id = id;
    }
}

}  // namespace

void TopologyDelta::note_vertex(const DynamicSurface& s, VertexId id) {
    note_element(s.vertices(), id, vertices_, vertex_slot_);
}
void TopologyDelta::note_halfedge(const DynamicSurface& s, HalfEdgeId id) {
    note_element(s.halfedges(), id, halfedges_, halfedge_slot_);
}
void TopologyDelta::note_edge(const DynamicSurface& s, EdgeId id) {
    note_element(s.edges(), id, edges_, edge_slot_);
}
void TopologyDelta::note_face(const DynamicSurface& s, FaceId id) {
    note_element(s.faces(), id, faces_, face_slot_);
}

void TopologyDelta::note_new_vertex(VertexId id) {
    note_new_element(id, vertices_, vertex_slot_);
}
void TopologyDelta::note_new_halfedge(HalfEdgeId id) {
    note_new_element(id, halfedges_, halfedge_slot_);
}
void TopologyDelta::note_new_edge(EdgeId id) { note_new_element(id, edges_, edge_slot_); }
void TopologyDelta::note_new_face(FaceId id) { note_new_element(id, faces_, face_slot_); }

void TopologyDelta::sync_vertex(const DynamicSurface& s, VertexId id) {
    sync_element(s.vertices(), id, vertices_, vertex_slot_);
}
void TopologyDelta::sync_halfedge(const DynamicSurface& s, HalfEdgeId id) {
    sync_element(s.halfedges(), id, halfedges_, halfedge_slot_);
}
void TopologyDelta::sync_edge(const DynamicSurface& s, EdgeId id) {
    sync_element(s.edges(), id, edges_, edge_slot_);
}
void TopologyDelta::sync_face(const DynamicSurface& s, FaceId id) {
    sync_element(s.faces(), id, faces_, face_slot_);
}

namespace {

// Restore one kind to one end of the record. `to_before` picks which end.
//
// The two passes are deliberate and the order matters: every element that must
// EXIST is revived first, and only then is every element that must NOT exist
// retired. Interleaving them would let a revive land on a slot a later retire
// then kills, which is the same slot's history read out of order.
template <typename Pool, typename Vec>
void restore_kind(Pool& pool, const Vec& list, bool to_before) {
    // Room for every slot the record names, including ones a pool that has
    // since shrunk no longer reaches.
    std::size_t highest = 0;
    for (const auto& e : list) highest = std::max<std::size_t>(highest, e.before_id.slot + 1);
    pool.ensure_slots(highest);

    // TWO PASSES, and the order matters: everything that must EXIST is revived
    // first, and only then is everything that must not be retired. Interleaving
    // them would let a revive land on a slot a later retire then kills.
    for (const auto& e : list) {
        if (to_before) {
            if (e.existed_before) pool.restore(e.before_id, e.before);
        } else {
            if (e.exists_after) pool.restore(e.after_id, e.after);
        }
    }
    for (const auto& e : list) {
        const bool should_exist = to_before ? e.existed_before : e.exists_after;
        if (should_exist) continue;
        // Retire at the generation the OTHER end of the record carries, so a
        // handle taken at that end stops resolving exactly when it should.
        pool.retire(to_before ? e.before_id : e.after_id);
    }

    // THE FREE LIST IS NOT AN INVARIANT EITHER OF THE TWO PASSES KEEPS.
    // `restore` revives a slot in place and leaves it linked; `retire` pushes
    // unconditionally. After a replay the list therefore names live slots and
    // can cycle, and `create` walks it — so the damage does not show as a
    // broken surface, it shows as the NEXT allocation never returning.
    // Measured before this call: after one revert-and-reapply of a two-dab
    // gesture, the vertex list cycled and held 201 live slots against 16 dead.
    pool.rebuild_free_list();
}

}  // namespace

bool TopologyDelta::revert(DynamicSurface& surface) const {
    restore_kind(surface.vertices_mutable(), vertices_, true);
    restore_kind(surface.halfedges_mutable(), halfedges_, true);
    restore_kind(surface.edges_mutable(), edges_, true);
    restore_kind(surface.faces_mutable(), faces_, true);
    surface.bump_topology();
    surface.bump_geometry();
    surface.bump_attributes();
    return true;
}

bool TopologyDelta::apply(DynamicSurface& surface) const {
    restore_kind(surface.vertices_mutable(), vertices_, false);
    restore_kind(surface.halfedges_mutable(), halfedges_, false);
    restore_kind(surface.edges_mutable(), edges_, false);
    restore_kind(surface.faces_mutable(), faces_, false);
    surface.bump_topology();
    surface.bump_geometry();
    surface.bump_attributes();
    return true;
}

void TopologyDelta::clear() {
    vertices_.clear();
    halfedges_.clear();
    edges_.clear();
    faces_.clear();
    vertex_slot_.clear();
    halfedge_slot_.clear();
    edge_slot_.clear();
    face_slot_.clear();
}

std::size_t TopologyDelta::bytes() const {
    std::size_t n = sizeof(TopologyDelta);
    n += vertices_.capacity() * sizeof(ElementDelta<DynamicVertex, VertexId>);
    n += halfedges_.capacity() * sizeof(ElementDelta<DynamicHalfEdge, HalfEdgeId>);
    n += edges_.capacity() * sizeof(ElementDelta<DynamicEdge, EdgeId>);
    n += faces_.capacity() * sizeof(ElementDelta<DynamicFace, FaceId>);
    // The slot indices are rebuildable but really allocated, so a budget that
    // ignored them would under-report a record following a long gesture.
    const std::size_t node = sizeof(std::uint32_t) * 2 + sizeof(void*);
    n += (vertex_slot_.size() + halfedge_slot_.size() + edge_slot_.size() + face_slot_.size()) *
         node;
    return n;
}

// -- encoding -----------------------------------------------------------------
//
// Layout, little-endian throughout:
//
//   u32 magic 'CTDL'   u16 version   u16 reserved
//   u32 vertex_count  u32 halfedge_count  u32 edge_count  u32 face_count
//   then each list, fixed-width per entry.
//
// Fixed-width and positional rather than tagged, for the reason
// `VertexDeltas::encode` already gives: this is a crash artifact paired with one
// surface, so it has to be cheap to write on every step rather than forgiving to
// read years later. The version is there so a build that does not understand it
// REFUSES.

namespace {

constexpr std::uint32_t kMagic = 0x4C445443u;  // 'CTDL'
constexpr std::uint16_t kVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}
void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}
void put_v3(std::vector<std::uint8_t>& out, kernel::cfloat3 v) {
    put_f32(out, v.x);
    put_f32(out, v.y);
    put_f32(out, v.z);
}
void put_v2(std::vector<std::uint8_t>& out, kernel::cfloat2 v) {
    put_f32(out, v.x);
    put_f32(out, v.y);
}
template <typename Id>
void put_id(std::vector<std::uint8_t>& out, Id id) {
    put_u32(out, id.slot);
    put_u32(out, id.generation);
}

void put_vertex(std::vector<std::uint8_t>& out, const DynamicVertex& v) {
    put_v3(out, v.position);
    put_v3(out, v.normal);
    put_v3(out, v.color);
    put_f32(out, v.mask);
    put_id(out, v.outgoing);
    put_u32(out, v.flags);
}
void put_halfedge(std::vector<std::uint8_t>& out, const DynamicHalfEdge& h) {
    put_id(out, h.origin);
    put_id(out, h.face);
    put_id(out, h.next);
    put_id(out, h.twin);
    put_id(out, h.edge);
    put_v2(out, h.uv);
}
void put_edge(std::vector<std::uint8_t>& out, const DynamicEdge& e) {
    put_id(out, e.halfedge);
    put_u32(out, e.constraints);
}
void put_face(std::vector<std::uint8_t>& out, const DynamicFace& f) {
    put_id(out, f.halfedge);
    put_v3(out, f.normal);
    put_u32(out, f.flags);
}

// Bytes per entry, which the decoder needs BEFORE it allocates anything: a
// count larger than the buffer could hold is a malformed or hostile record, and
// sizing from it is how a reader gets asked for a gigabyte.
constexpr std::size_t kVertexBytes = 8 + 2 + 4 * (3 + 3 + 3 + 1 + 2 + 1) * 2;
constexpr std::size_t kHalfEdgeBytes = 8 + 2 + 4 * (2 * 5 + 2) * 2;
constexpr std::size_t kEdgeBytes = 8 + 2 + 4 * (2 + 1) * 2;
constexpr std::size_t kFaceBytes = 8 + 2 + 4 * (2 + 3 + 1) * 2;

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (at + 1 > size) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    std::uint32_t u32() {
        if (at + 4 > size) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(data[at]) |
                                (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return v;
    }
    float f32() {
        const std::uint32_t bits = u32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    kernel::cfloat3 v3() {
        const float x = f32(), y = f32(), z = f32();
        return kernel::cf3(x, y, z);
    }
    kernel::cfloat2 v2() {
        const float x = f32(), y = f32();
        return kernel::cf2(x, y);
    }
    template <typename Id>
    Id id() {
        Id out;
        out.slot = u32();
        out.generation = u32();
        return out;
    }
    DynamicVertex vertex() {
        DynamicVertex v;
        v.position = v3();
        v.normal = v3();
        v.color = v3();
        v.mask = f32();
        v.outgoing = id<HalfEdgeId>();
        v.flags = u32();
        return v;
    }
    DynamicHalfEdge halfedge() {
        DynamicHalfEdge h;
        h.origin = id<VertexId>();
        h.face = id<FaceId>();
        h.next = id<HalfEdgeId>();
        h.twin = id<HalfEdgeId>();
        h.edge = id<EdgeId>();
        h.uv = v2();
        return h;
    }
    DynamicEdge edge() {
        DynamicEdge e;
        e.halfedge = id<HalfEdgeId>();
        e.constraints = u32();
        return e;
    }
    DynamicFace face() {
        DynamicFace f;
        f.halfedge = id<HalfEdgeId>();
        f.normal = v3();
        f.flags = u32();
        return f;
    }
};

}  // namespace

std::vector<std::uint8_t> TopologyDelta::encode() const {
    std::vector<std::uint8_t> out;
    put_u32(out, kMagic);
    out.push_back(static_cast<std::uint8_t>(kVersion));
    out.push_back(static_cast<std::uint8_t>(kVersion >> 8));
    out.push_back(0);
    out.push_back(0);
    put_u32(out, static_cast<std::uint32_t>(vertices_.size()));
    put_u32(out, static_cast<std::uint32_t>(halfedges_.size()));
    put_u32(out, static_cast<std::uint32_t>(edges_.size()));
    put_u32(out, static_cast<std::uint32_t>(faces_.size()));

    for (const auto& e : vertices_) {
        put_id(out, e.before_id);
        put_id(out, e.after_id);
        out.push_back(e.existed_before ? 1 : 0);
        out.push_back(e.exists_after ? 1 : 0);
        put_vertex(out, e.before);
        put_vertex(out, e.after);
    }
    for (const auto& e : halfedges_) {
        put_id(out, e.before_id);
        put_id(out, e.after_id);
        out.push_back(e.existed_before ? 1 : 0);
        out.push_back(e.exists_after ? 1 : 0);
        put_halfedge(out, e.before);
        put_halfedge(out, e.after);
    }
    for (const auto& e : edges_) {
        put_id(out, e.before_id);
        put_id(out, e.after_id);
        out.push_back(e.existed_before ? 1 : 0);
        out.push_back(e.exists_after ? 1 : 0);
        put_edge(out, e.before);
        put_edge(out, e.after);
    }
    for (const auto& e : faces_) {
        put_id(out, e.before_id);
        put_id(out, e.after_id);
        out.push_back(e.existed_before ? 1 : 0);
        out.push_back(e.exists_after ? 1 : 0);
        put_face(out, e.before);
        put_face(out, e.after);
    }
    return out;
}

bool TopologyDelta::decode(const std::uint8_t* data, std::size_t size, TopologyDelta* out) {
    if (!data || !out) return false;
    Reader r{data, size};
    if (r.u32() != kMagic) return false;
    const std::uint16_t version =
        static_cast<std::uint16_t>(r.u8() | (static_cast<std::uint16_t>(r.u8()) << 8));
    r.u8();
    r.u8();
    // Refused rather than reinterpreted. A newer layout read as this one would
    // revert a surface to a connectivity that was never in it.
    if (!r.ok || version != kVersion) return false;

    const std::uint32_t nv = r.u32(), nh = r.u32(), ne = r.u32(), nf = r.u32();
    if (!r.ok) return false;

    // THE COUNTS ARE CHECKED AGAINST THE BUFFER BEFORE ANYTHING IS ALLOCATED.
    // A count larger than the remaining bytes could hold is a malformed or
    // hostile record, and sizing a vector from it is how a reader gets asked to
    // allocate a gigabyte. Same defensive shape `VertexDeltas::decode` uses.
    const std::size_t remaining = size - r.at;
    const std::size_t claimed = static_cast<std::size_t>(nv) * kVertexBytes +
                                static_cast<std::size_t>(nh) * kHalfEdgeBytes +
                                static_cast<std::size_t>(ne) * kEdgeBytes +
                                static_cast<std::size_t>(nf) * kFaceBytes;
    // Overflow-checked: the four products are each bounded by 2^32 * a small
    // constant, so the sum cannot wrap a 64-bit size_t, and comparing it is
    // enough.
    if (claimed > remaining) return false;

    TopologyDelta built;
    built.vertices_.reserve(nv);
    built.halfedges_.reserve(nh);
    built.edges_.reserve(ne);
    built.faces_.reserve(nf);

    for (std::uint32_t i = 0; i < nv && r.ok; ++i) {
        ElementDelta<DynamicVertex, VertexId> e;
        e.before_id = r.id<VertexId>();
        e.after_id = r.id<VertexId>();
        e.existed_before = r.u8() != 0;
        e.exists_after = r.u8() != 0;
        e.before = r.vertex();
        e.after = r.vertex();
        built.vertex_slot_.emplace(e.before_id.slot, static_cast<std::uint32_t>(built.vertices_.size()));
        built.vertices_.push_back(e);
    }
    for (std::uint32_t i = 0; i < nh && r.ok; ++i) {
        ElementDelta<DynamicHalfEdge, HalfEdgeId> e;
        e.before_id = r.id<HalfEdgeId>();
        e.after_id = r.id<HalfEdgeId>();
        e.existed_before = r.u8() != 0;
        e.exists_after = r.u8() != 0;
        e.before = r.halfedge();
        e.after = r.halfedge();
        built.halfedge_slot_.emplace(e.before_id.slot,
                                     static_cast<std::uint32_t>(built.halfedges_.size()));
        built.halfedges_.push_back(e);
    }
    for (std::uint32_t i = 0; i < ne && r.ok; ++i) {
        ElementDelta<DynamicEdge, EdgeId> e;
        e.before_id = r.id<EdgeId>();
        e.after_id = r.id<EdgeId>();
        e.existed_before = r.u8() != 0;
        e.exists_after = r.u8() != 0;
        e.before = r.edge();
        e.after = r.edge();
        built.edge_slot_.emplace(e.before_id.slot, static_cast<std::uint32_t>(built.edges_.size()));
        built.edges_.push_back(e);
    }
    for (std::uint32_t i = 0; i < nf && r.ok; ++i) {
        ElementDelta<DynamicFace, FaceId> e;
        e.before_id = r.id<FaceId>();
        e.after_id = r.id<FaceId>();
        e.existed_before = r.u8() != 0;
        e.exists_after = r.u8() != 0;
        e.before = r.face();
        e.after = r.face();
        built.face_slot_.emplace(e.before_id.slot, static_cast<std::uint32_t>(built.faces_.size()));
        built.faces_.push_back(e);
    }
    if (!r.ok) return false;
    *out = std::move(built);
    return true;
}

}  // namespace mesh
}  // namespace clay
