#include "clay/mesh/maintenance.h"

#include <chrono>

#include "clay/mesh/dynamic_bvh.h"

namespace clay {
namespace mesh {

const char* maintenance_kind_name(MaintenanceKind kind) {
    switch (kind) {
        case MaintenanceKind::IndexRebuild:
            return "index_rebuild";
        case MaintenanceKind::ChunkCompaction:
            return "chunk_compaction";
        case MaintenanceKind::DetailPromotion:
            return "detail_promotion";
        case MaintenanceKind::SlotPoolCompaction:
            return "slot_pool_compaction";
        case MaintenanceKind::NormalFlush:
            return "normal_flush";
    }
    return "unknown";
}

bool MaintenanceQueue::has(MaintenanceKind kind, std::uint32_t target) const {
    for (const MaintenanceItem& item : items_)
        if (item.kind == kind && item.target == target) return true;
    return false;
}

void MaintenanceQueue::request(MaintenanceKind kind, std::uint32_t target,
                               std::uint64_t estimated_micros) {
    // A LINEAR SCAN, and it is the right structure here. The queue holds a
    // handful of entries — there are five kinds and a target apiece — so a
    // hash set would be a heap allocation and a hash per stamp to avoid
    // comparing five integers.
    for (MaintenanceItem& item : items_) {
        if (item.kind != kind || item.target != target) continue;
        ++item.requests;
        // The LATEST estimate wins: a caller that has learned more about the
        // cost since the first request is telling the host something.
        if (estimated_micros != 0) item.estimated_micros = estimated_micros;
        return;
    }
    MaintenanceItem item;
    item.kind = kind;
    item.target = target;
    item.estimated_micros = estimated_micros;
    items_.push_back(item);
}

std::size_t MaintenanceQueue::service(std::uint64_t budget_micros,
                                      const std::function<bool(const MaintenanceItem&)>& run) {
    // THE GATE. Not a convention in a comment: a host that called this from a
    // pointer handler gets nothing done rather than a stall it will attribute
    // to the brush.
    if (in_stroke_ || items_.empty() || !run) return 0;

    const auto started = std::chrono::steady_clock::now();
    std::size_t completed = 0;
    std::size_t at = 0;
    while (at < items_.size()) {
        // An item is never interrupted part way — a half-compacted arena is
        // worse than a fragmented one — so the budget decides whether the NEXT
        // one starts.
        if (completed != 0 && budget_micros != 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
            if (static_cast<std::uint64_t>(spent) >= budget_micros) break;
        }
        if (!run(items_[at])) {
            // Declined: it stays queued, in place, so the order a host sees is
            // the order it asked for.
            ++at;
            continue;
        }
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(at));
        ++completed;
        if (budget_micros == 0) break;  // "one item, whatever it costs"
    }
    return completed;
}

std::size_t MaintenanceQueue::bytes() const {
    return sizeof(MaintenanceQueue) + items_.capacity() * sizeof(MaintenanceItem);
}

bool request_index_rebuild(const DynamicBvh& index, const memory::SculptMemoryProfile& profile,
                           std::uint32_t target, MaintenanceQueue* queue) {
    if (queue == nullptr || !profile.allow_index_rebuild || !index.wants_rebuild()) return false;
    // The estimate is the leaf count rather than a time: a rebuild is O(leaves)
    // and this library does not carry a machine model to turn that into
    // microseconds. A host that has measured its own device can replace it.
    queue->request(MaintenanceKind::IndexRebuild, target,
                   static_cast<std::uint64_t>(index.leaf_count()));
    return true;
}

}  // namespace mesh
}  // namespace clay
