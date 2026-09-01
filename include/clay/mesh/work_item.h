#pragma once

// THE NEUTRAL IDENTITY OF A THING UNDER THE BRUSH (meshing spec,
// add-shared-brush-runtime).
//
// WHAT THIS IS FOR. `SculptWorkset` used to address work by 32-bit weld class,
// which is the FIXED mesh's identity and nothing else's. The adaptive surface
// addresses a vertex by a `VertexId` — a slot plus the generation that makes a
// stale handle detectable — and a hierarchy addresses one as (level, vertex).
// So a workset typed in weld classes is a workset only the fixed sculptor can
// fill, and the automask, the weight composition and the write-region report
// that read it have to be written three times to serve three representations.
// Three copies of a gate is three chances to get a gate wrong; this type is
// what makes it one.
//
// WHY 64 BITS, AND WHY NOT A TEMPLATE. The width is not a guess at the future.
// A `VertexId` must carry its generation or a retired handle sitting in a write
// region is indistinguishable from a live one, and (level, vertex) is two
// 32-bit numbers. Sixty-four is what the two non-fixed representations already
// are. A template on the identity type was the alternative and is worse in
// precisely the place this exists to improve: `compute_automask`, the
// write-region report and the weight composition would each be instantiated
// three times, which is three copies of the thing whose single-copy-ness is the
// point. The cost is four bytes per workset entry on a footprint-sized array.
//
// IT CARRIES NO TAG, AND THAT IS DELIBERATE. Sixty-four bits are exactly
// consumed by (slot, generation) and by (level, vertex), so a discriminator
// would have to steal bits from one of them — and there is nothing to
// discriminate: a workset is filled by ONE representation's adapter and read by
// its own sculptor, so the kind is a property of the workset rather than of the
// item. Reading a weld class back out of an id built from a `VertexId` is a
// caller mixing two representations' worksets, which no code path can reach.
//
// THE LOW HALF IS THE DENSE INDEX, IN ALL THREE, AND THAT IS LOAD-BEARING.
// `SculptWorkset::slot` is the reverse map — identity to workset index — and it
// is a dense array whose size belongs to the representation rather than to the
// workset. The adapter still sizes and resets it, because only the adapter
// knows how big it is. But the composition has to PUBLISH into it, and it can:
// the weld class, the vertex slot and the level vertex are each the low 32 bits
// of the id by construction, so `slot[item.key()]` is one spelling that is
// correct on all three. Putting the generation and the level in the HIGH half
// is what buys that, and is the reason the encoding is written down here rather
// than left to each constructor.

#include <cstdint>

#include "clay/mesh/brush_arena.h"  // ScratchVector
#include "clay/mesh/slot_pool.h"    // VertexId

namespace clay {
namespace mesh {

class WorkItemId {
   public:
    WorkItemId() = default;

    // -- the three identities ------------------------------------------------

    static WorkItemId weld_class(std::uint32_t cls) { return WorkItemId(cls); }
    static WorkItemId surface_vertex(VertexId v) {
        return WorkItemId(static_cast<std::uint64_t>(v.slot) |
                          (static_cast<std::uint64_t>(v.generation) << 32));
    }
    static WorkItemId level_vertex(std::uint32_t level, std::uint32_t vertex) {
        return WorkItemId(static_cast<std::uint64_t>(vertex) |
                          (static_cast<std::uint64_t>(level) << 32));
    }

    // -- reading one back ----------------------------------------------------

    std::uint32_t as_weld_class() const { return low(); }
    VertexId as_surface_vertex() const { return VertexId{low(), high()}; }
    std::uint32_t level() const { return high(); }
    std::uint32_t level_vertex_index() const { return low(); }

    // The dense half — see the header note. This is the ONE readback the
    // neutral code uses, because it means the same thing on all three
    // representations.
    std::uint32_t key() const { return low(); }

    std::uint64_t bits() const { return bits_; }

    friend bool operator==(WorkItemId a, WorkItemId b) { return a.bits_ == b.bits_; }
    friend bool operator!=(WorkItemId a, WorkItemId b) { return a.bits_ != b.bits_; }
    // ORDERED BY THE DENSE HALF FIRST, which is what `slot` is keyed by and
    // what every representation's own determinism rule already sorts on —
    // `SlotId::operator<` orders by slot for exactly this reason, and ordering
    // by generation first would make a sort depend on the history of edits.
    friend bool operator<(WorkItemId a, WorkItemId b) {
        return a.low() != b.low() ? a.low() < b.low() : a.high() < b.high();
    }

   private:
    explicit WorkItemId(std::uint64_t bits) : bits_(bits) {}
    std::uint32_t low() const { return static_cast<std::uint32_t>(bits_ & 0xffffffffu); }
    std::uint32_t high() const { return static_cast<std::uint32_t>(bits_ >> 32); }

    std::uint64_t bits_ = 0;
};

// THE TWO TOPOLOGICAL QUESTIONS THE AUTOMASK ASKS, and the only two it asks.
//
// Three of the five automask factors need no topology at all: NormalAngle reads
// `workset.normals`, Cavity and SurfaceGroup call the caller's own functions on
// `workset.positions`. Only the boundary fade and the connectivity flood need
// to know how the surface is joined up, and they need exactly this much of it.
//
// ANSWERED IN WORKSET SLOTS, never in a representation's own identities. That
// is what keeps the automask's core neutral: it never learns what a weld class
// or a `VertexId` is, and it never indexes `SculptWorkset::slot`, which is the
// adapter's array.
//
// WHY A VIRTUAL HERE, WHEN `sculpt_kernels.h` REFUSED ONE. That refusal was
// specific and its reason does not carry: a neighbour callback there would have
// been a virtual call in the INNERMOST loop of a smoothing pass, per neighbour,
// per pass, on a million-vertex surface at pointer rates. This is one call per
// workset entry, on two of five factors, in a pass that already calls `acos`
// per entry and a caller-supplied `std::function` per entry for cavity. The
// alternative — a template on the topology — reinstates the three
// instantiations `WorkItemId` exists to avoid.
struct WorkItemTopology {
    virtual ~WorkItemTopology() = default;

    // The workset slots of `slot`'s one-ring that are THEMSELVES in the
    // workset. Neighbours outside it are not reported, and that is the
    // definition rather than an optimisation: both topological factors spread
    // over the workset alone, so a border outside the brush cannot be reached
    // and a brush nowhere near one must not pay to discover that.
    //
    // `out` is cleared first. Its capacity is `workset.size()`, which is a hard
    // bound: a ring cannot hold more DISTINCT in-workset slots than the workset
    // has entries.
    virtual void ring_slots(std::uint32_t slot, ScratchVector<std::uint32_t>* out) const = 0;

    // Whether the item at `slot` sits on an open border of the surface.
    virtual bool on_open_border(std::uint32_t slot) const = 0;
};

}  // namespace mesh
}  // namespace clay
