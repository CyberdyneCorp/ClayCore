#include "clay/memory/budget.h"

#include <algorithm>

namespace clay {
namespace memory {
namespace {

// One table rather than three switches. The classification is the whole
// contract of the ledger — an entry in the wrong column is a host releasing
// somebody's work — so it is written once, in the order the categories are
// declared, and the static assert below is what keeps the two in step.
struct CategoryRow {
    const char* name;
    bool essential;
    bool rebuildable;
    bool undoable;
};

constexpr CategoryRow kCategories[kMemoryCategoryCount] = {
    {"base_geometry", true, false, false},
    {"topology", true, false, false},
    {"multires_detail", true, false, false},
    {"sculpt_layers", true, false, false},
    {"masks", true, false, false},
    {"chunk_index", false, true, false},
    {"evaluated_cache", false, true, false},
    {"level_runtime_cache", false, true, false},
    {"layer_eval_cache", false, true, false},
    {"derived_positions", false, true, false},
    {"scratch", false, true, false},
    {"preview_staging", false, true, false},
    {"undo_history", false, false, true},
};

static_assert(static_cast<std::size_t>(MemoryCategory::UndoHistory) + 1 == kMemoryCategoryCount,
              "a category was added without a row: classify it, or the roll-ups stop adding up");

}  // namespace

const char* memory_class_name(MemoryClass value) {
    switch (value) {
        case MemoryClass::Full:
            return "full";
        case MemoryClass::Constrained:
            return "constrained";
        case MemoryClass::Minimal:
            return "minimal";
    }
    return "full";
}

const char* pressure_name(Pressure value) {
    switch (value) {
        case Pressure::None:
            return "none";
        case Pressure::Warning:
            return "warning";
        case Pressure::Urgent:
            return "urgent";
        case Pressure::Critical:
            return "critical";
    }
    return "none";
}

const char* memory_category_name(MemoryCategory value) {
    const std::size_t i = static_cast<std::size_t>(value);
    return i < kMemoryCategoryCount ? kCategories[i].name : "unknown";
}

bool category_is_essential(MemoryCategory value) {
    const std::size_t i = static_cast<std::size_t>(value);
    return i < kMemoryCategoryCount && kCategories[i].essential;
}

bool category_is_rebuildable(MemoryCategory value) {
    const std::size_t i = static_cast<std::size_t>(value);
    return i < kMemoryCategoryCount && kCategories[i].rebuildable;
}

bool category_is_undoable(MemoryCategory value) {
    const std::size_t i = static_cast<std::size_t>(value);
    return i < kMemoryCategoryCount && kCategories[i].undoable;
}

void MemoryLedger::merge(const MemoryLedger& other) {
    for (std::size_t i = 0; i < kMemoryCategoryCount; ++i) bytes[i] += other.bytes[i];
}

void MemoryLedger::clear() {
    for (std::size_t i = 0; i < kMemoryCategoryCount; ++i) bytes[i] = 0;
}

std::size_t MemoryLedger::essential() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMemoryCategoryCount; ++i)
        if (kCategories[i].essential) n += bytes[i];
    return n;
}

std::size_t MemoryLedger::rebuildable() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMemoryCategoryCount; ++i)
        if (kCategories[i].rebuildable) n += bytes[i];
    return n;
}

std::size_t MemoryLedger::undoable() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMemoryCategoryCount; ++i)
        if (kCategories[i].undoable) n += bytes[i];
    return n;
}

std::size_t MemoryLedger::total() const {
    // The sum of the three roll-ups rather than a separate walk: a category
    // that fell out of all three columns would otherwise be counted here and
    // in none of the numbers a host acts on, which is the omission the
    // scene-model requirement was written after.
    return essential() + rebuildable() + undoable();
}

void TrimReport::merge(const TrimReport& other) {
    for (std::size_t i = 0; i < kMemoryCategoryCount; ++i) released[i] += other.released[i];
    total_released += other.total_released;
    pressure = std::max(pressure, other.pressure);
    pinned = pinned || other.pinned;
}

void PeakTelemetry::observe_scratch(std::size_t n) { scratch_bytes = std::max(scratch_bytes, n); }
void PeakTelemetry::observe_workset(std::size_t n) {
    workset_vertices = std::max(workset_vertices, n);
}
void PeakTelemetry::observe_dirty(std::size_t n) { dirty_chunks = std::max(dirty_chunks, n); }
void PeakTelemetry::observe_topology(std::size_t n) { topology_ops = std::max(topology_ops, n); }

void PeakTelemetry::reset() { *this = PeakTelemetry{}; }

}  // namespace memory
}  // namespace clay
