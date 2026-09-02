#pragma once

// WORK THAT IS NOT REQUIRED FOR CORRECTNESS (sculpt-runtime spec,
// add-extreme-poly-runtime).
//
// A sculpt runtime accumulates jobs that make the NEXT interaction cheaper and
// this one slower: rebuilding a spatial index whose partition has decayed,
// compacting a chunk arena the splits left holes in, promoting a sparse detail
// field that is now dense, compacting a slot pool full of dead slots. Every one
// of them is optional, none of them changes what is committed, and every one of
// them is a stall if it happens while a finger is on the glass.
//
// So they are QUEUED AND NOT DONE. A host services the queue with a time budget
// between interactions — the moment it, and only it, knows about. The queue
// refuses to run while a stroke is open, which is a mechanism rather than a
// convention: "we only call this between strokes" is a rule that survives until
// the second caller.
//
// NOTHING HERE IS A REBUILD ON THE ENGINE'S OWN BEHALF. `Bvh::quality` records
// the measurement that settles it: over five deformations a rebuild produced a
// better tree in exactly one and a dramatically worse one in two. The queue
// carries a REQUEST that a host may service, drop, or defer forever, and the
// index stays correct either way — a refit keeps a tree correct and does not
// keep it fast, which is the whole distinction this file is built on.
//
// THE BUDGET IS WALL CLOCK, and that is deliberate rather than sloppy. The
// alternative is a cost model per item, which would be a second thing to keep
// true; the honest statement is "spend at most this long", and an item that
// overruns it stops the NEXT one from starting rather than being interrupted —
// nothing here is cancellable mid-item, because a half-compacted arena is worse
// than a fragmented one.

#include <cstdint>
#include <functional>
#include <vector>

#include "clay/memory/budget.h"

namespace clay {
namespace mesh {

class DynamicBvh;

enum class MaintenanceKind : std::uint32_t {
    // The spatial index's partition has decayed under local edits. Advisory,
    // and the most likely one for a host to decline.
    IndexRebuild = 0,
    // The chunk table's face arena has slack a split left behind.
    ChunkCompaction = 1,
    // A sparse detail field whose coverage has passed the promotion threshold.
    // Speed, not memory: the block table is smaller than the dense form until
    // coverage passes 99.9%, and what argues for promotion is the indirection
    // on every read.
    DetailPromotion = 2,
    // Dead slots an adaptive surface's edits left in its pools.
    SlotPoolCompaction = 3,
    // Normals deferred during a drag. THE ONE ITEM THAT IS NOT OPTIONAL: the
    // committed state has to be exact, so a host that never services the queue
    // must still flush these at stroke end. It is here so a host can spend its
    // budget on them first, not so it can decide whether to do them.
    NormalFlush = 4,
};

const char* maintenance_kind_name(MaintenanceKind kind);

struct MaintenanceItem {
    MaintenanceKind kind = MaintenanceKind::IndexRebuild;
    // What the item is about: a multires level, a chunk, a surface id. The
    // queue never interprets it; it is what makes two requests the same request.
    std::uint32_t target = 0;
    // The caller's own estimate, for a host ordering its budget. Zero means
    // "unknown", which is what most callers honestly have.
    std::uint64_t estimated_micros = 0;
    // How many times this item has been requested since it was last serviced.
    // A queue entry that keeps being re-requested is one a host is starving,
    // and that is worth being able to see.
    std::uint32_t requests = 1;
};

class MaintenanceQueue {
  public:
    // Queue an item, or fold it into the identical one already queued. Never
    // allocates per request once the queue has reached its working size, which
    // matters because a stroke requests the same rebuild on every stamp.
    void request(MaintenanceKind kind, std::uint32_t target, std::uint64_t estimated_micros = 0);

    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    const std::vector<MaintenanceItem>& items() const { return items_; }
    bool has(MaintenanceKind kind, std::uint32_t target) const;

    // -- the stroke gate -------------------------------------------------------
    //
    // A pointer event is not a maintenance window. `service` does nothing while
    // a stroke is open and says so by returning zero, so a host that wired it
    // to the wrong callback finds out by nothing happening rather than by a
    // stutter it will blame on something else.
    void begin_stroke() { in_stroke_ = true; }
    void end_stroke() { in_stroke_ = false; }
    bool in_stroke() const { return in_stroke_; }

    // Run queued items until the budget is spent, in queue order.
    //
    // `run` returns whether the item was actually completed; an item it
    // declines stays queued. Returns how many were completed. A budget of zero
    // means "one item, whatever it costs", which is what a host with a frame to
    // spare and no idea what things cost actually wants.
    std::size_t service(std::uint64_t budget_micros,
                        const std::function<bool(const MaintenanceItem&)>& run);

    void clear() { items_.clear(); }
    std::size_t bytes() const;

  private:
    std::vector<MaintenanceItem> items_;
    bool in_stroke_ = false;
};

// Ask the queue for an index rebuild if the tree wants one and the profile
// allows it. Returns whether anything was queued.
//
// The two conditions are separate on purpose: `wants_rebuild` is the ENGINE's
// measurement of its own partition, and `allow_index_rebuild` is the HOST's
// statement about whether it has the room. Neither is a decision to rebuild —
// this only puts it where a host can see it.
bool request_index_rebuild(const DynamicBvh& index, const memory::SculptMemoryProfile& profile,
                           std::uint32_t target, MaintenanceQueue* queue);

}  // namespace mesh
}  // namespace clay
