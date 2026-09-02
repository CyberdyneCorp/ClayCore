#pragma once

// WHAT A HOST IS WILLING TO SPEND, AND WHAT IT MAY TAKE BACK (sculpt-runtime
// spec, add-extreme-poly-runtime).
//
// `io::MemoryReport` already answers "what does this document cost". It cannot
// answer the two questions a device under pressure actually asks — "what am I
// allowed to spend" and "what can I release right now" — and it cannot be
// where they are answered: `io` is the TOP of the layering table, so `mesh`
// may not include it, and `mesh` is precisely what has to consult a budget on
// every stamp. The alternative was threading a byte count through every call
// signature from the host down, which puts residency policy in the host and
// makes the engine's own scratch bound something a caller can forget to pass.
// So this is a leaf module beside `parallel` and `kernel`, depending on
// nothing, reachable from `mesh`, `voxel`, `brick`, `session` and `io` alike.
//
// THE PROFILE IS FILLED BY THE HOST AND CONTAINS NO DEVICE DETECTION. Not one
// platform call, not one model-name comparison, not one `if iPad`. A host
// knows what its operating system is telling it; an engine guessing from a
// model string is both wrong and — worse — untestable, because the test suite
// runs on a desktop. A constrained profile is a struct a desktop test fills in
// three lines, which is the whole point.
//
// THE FIELDS ARE A HINT AND NOT A CONTRACT, AND THE TYPE IS WHAT SAYS SO.
// Every field below names something that can be RECOMPUTED EXACTLY from what
// was committed: normals during a drag, index quality, display level, cache
// residency, the rate a preview drains. There is deliberately no field for
// anything that IS the committed result — the deformation, the stroke
// trajectory, split and collapse thresholds, remesh targets, detail
// coefficients, layer content, masks, brush strength and falloff. A deferred
// split would make the committed mesh a function of machine speed, and this
// tree spends real effort on determinism (chunking in slot order, per-platform
// golden hashes) that a budget-dependent topology would throw away. One global
// "interactive mode" flag was the alternative and it has to mean SOMETHING for
// topology: either it is ignored there, which is a lie a reviewer will find,
// or it defers the split, which is the nondeterminism. Making the contract
// rows unrepresentable costs more API surface and buys "a memory-saving mode
// changed my sculpt" being impossible rather than merely forbidden.
//
// THE HOST OWNS THE MOMENT, THE ENGINE OWNS THE ORDER. Nothing here evicts on
// its own high-water mark. An engine that did would mutate a document behind a
// host that may be mid-save or holding a readback — `MultiresSurface::cache_generation`
// exists precisely because a released cache is a use-after-free waiting for
// pressure to find it, and autonomous eviction adds a second invalidation
// source the host cannot predict. Eviction happens at exactly three moments: an
// explicit `trim`, a residency change the host itself caused, and the scratch
// arena falling back to its soft bound AT A STROKE BOUNDARY. `MemoryPin` is
// what makes the first of those answerable honestly while a save is running.

#include <cstddef>
#include <cstdint>

namespace clay {
namespace memory {

// How much room the host is working in. A label rather than a device: a
// desktop test sets `Constrained` and observes constrained behaviour where the
// tests run rather than only where it ships.
enum class MemoryClass : std::uint32_t {
    // No budget. Every byte field of the profile is advisory and the runtime
    // keeps what it builds. What a desktop host and most tests want.
    Full = 0,
    // The reference-iPad case: budgets are real, inactive levels hold compact
    // detail only, and maintenance runs between interactions.
    Constrained = 1,
    // What a host sets when the operating system has already warned it once.
    // Everything rebuildable is a candidate the moment it is not being read.
    Minimal = 2,
};

const char* memory_class_name(MemoryClass value);

// How hard a host is asking. Passed to `trim`; never inferred by the engine.
enum class Pressure : std::uint32_t {
    // Give back what is free anyway — scratch above its steady capacity and
    // preview staging that has already been drained.
    None = 0,
    Warning = 1,
    Urgent = 2,
    // The last stop before the operating system kills the process. Everything
    // rebuildable goes, including the active level's caches; the next edit pays
    // to rebuild what it needs.
    Critical = 3,
};

const char* pressure_name(Pressure value);

// WHAT A HOST DECLARES. Zero in a byte field means "no budget for this",
// which is what `Full` means field by field and what every existing caller
// gets by default.
//
// Every field is a HINT row by the rule in this header's preamble. Adding one
// that is not is the mistake this type exists to prevent, so a new field
// arrives with the sentence that says which committed result it cannot change.
struct SculptMemoryProfile {
    MemoryClass memory_class = MemoryClass::Full;

    // -- byte budgets --------------------------------------------------------
    // Rebuildable caches: chunk indices, per-level runtime caches, evaluated
    // layer caches, derived positions. What `trim` reaches for first.
    std::uint64_t cache_budget = 0;
    // Undo. The engine never trims this on its own; the figure exists so a
    // host can set its own history budget from the same struct it fills for
    // everything else.
    std::uint64_t undo_budget = 0;
    // The per-stamp working set. `ScratchArena` treats this as a HARD bound and
    // processes a footprint larger than it in blocks rather than allocating —
    // which is what stops a 500k-vertex footprint on a constrained profile from
    // becoming the peak that kills the app.
    std::uint64_t scratch_budget = 0;
    // What a host may be holding for the next frame's upload.
    std::uint64_t preview_budget = 0;

    // -- residency and deferral ----------------------------------------------
    // How many multires levels keep their rebuildable caches. Zero means no
    // limit. On a constrained profile the sculpt level and the display level
    // are resident and the rest hold compact detail only, which is 4.7.
    std::uint32_t max_resident_levels = 0;
    // Recompute exact normals at stroke end rather than per stamp. The final
    // state is exact either way — that is the gate — and this only decides when
    // the work happens.
    bool defer_normals_in_stroke = false;
    // Whether a spatial index may be REBUILT (never whether it may be refitted:
    // a refit is correctness). Deferred to between strokes regardless; this
    // says whether it happens at all. A refit keeps a tree correct and does not
    // keep it fast, and `Bvh::quality` records that a rebuild helped one of
    // five measured deformations and hurt two — so this defaults to on and
    // stays advisory.
    bool allow_index_rebuild = true;
    // How many dirty chunks a host expects to drain per frame, so the runtime
    // can stop staging past what will be read. Zero means "as many as there
    // are". Lossless at any value because the transport acknowledges per chunk.
    std::uint32_t preview_chunks_per_frame = 0;

    // Whether this profile imposes anything at all. A `Full` profile with no
    // budgets set behaves exactly as the library did before it existed.
    bool constrained() const { return memory_class != MemoryClass::Full; }
};

// -- the ledger ----------------------------------------------------------------

// The vocabulary every subsystem answers in. One enum rather than a struct per
// representation, because the three roll-ups below have to be a sum over
// categories and not a sum over types a caller must know the names of.
//
// The split that matters is not "big versus small" but WHAT IT COSTS TO LET IT
// GO: the first group is the user's work and is never released, the second
// reconstructs bit-identically, the third is undo depth and belongs to the
// host's own policy.
enum class MemoryCategory : std::uint32_t {
    // -- authoritative: releasing any of this destroys work -------------------
    BaseGeometry = 0,     // cages, fixed meshes, adaptive surface elements
    Topology,             // per-level face lists, half-edge connectivity
    MultiresDetail,       // the coefficients: the wrinkles themselves
    SculptLayers,         // a layer stack's content
    Masks,                // authoring state
    // -- rebuildable: reconstructs to an identical surface --------------------
    ChunkIndex,           // the chunk table, its CSR arena and the trees over it
    EvaluatedCache,       // subdivided positions, frames, normals
    LevelRuntimeCache,    // per-level meshes, adjacency, connectivity
    LayerEvalCache,       // an evaluated layer stack
    DerivedPositions,     // positions of levels nobody is looking at
    Scratch,              // the per-stamp working set
    PreviewStaging,       // what is queued for the host's next upload
    // -- undoable: the host's policy, never the engine's -----------------------
    UndoHistory,
};

inline constexpr std::size_t kMemoryCategoryCount = 13;

const char* memory_category_name(MemoryCategory value);
// Which of the three roll-ups a category belongs to. Written once here so the
// report, the trim order and the C ABI cannot disagree about whether a category
// is safe to release.
bool category_is_essential(MemoryCategory value);
bool category_is_rebuildable(MemoryCategory value);
bool category_is_undoable(MemoryCategory value);

// Bytes by category, plus the three totals a host under pressure can act on.
//
// A SINGLE TOTAL IS NOT THE ANSWER, which is the finding `io::MemoryReport`
// already records: under pressure a host does not need to know how big the
// document is, it needs to know WHICH PART, because that is what decides what
// it is allowed to release.
struct MemoryLedger {
    std::size_t bytes[kMemoryCategoryCount] = {};

    void add(MemoryCategory category, std::size_t n) {
        bytes[static_cast<std::size_t>(category)] += n;
    }
    std::size_t of(MemoryCategory category) const {
        return bytes[static_cast<std::size_t>(category)];
    }
    void merge(const MemoryLedger& other);
    void clear();

    // The three roll-ups. Derived rather than stored, so a category added
    // without being classified is a compile error in `category_is_*` rather
    // than a number that quietly stops adding up.
    std::size_t essential() const;
    std::size_t rebuildable() const;
    std::size_t undoable() const;
    std::size_t total() const;
};

// What a trim actually did, per category. Reported rather than returned as one
// number for the same reason the ledger is: a host that asked for 40 MB and got
// it out of preview staging made a different decision from one that got it out
// of the evaluated caches it is about to need again.
struct TrimReport {
    Pressure pressure = Pressure::None;
    std::size_t released[kMemoryCategoryCount] = {};
    std::size_t total_released = 0;
    // True when a `MemoryPin` was held: NOTHING was released and the figures
    // above are what the call WOULD have released. A host that receives a
    // memory warning while a save is running gets an honest answer instead of
    // a document mutating under the writer.
    bool pinned = false;

    void release(MemoryCategory category, std::size_t n) {
        released[static_cast<std::size_t>(category)] += n;
        total_released += n;
    }
    void merge(const TrimReport& other);
};

// -- the pin -------------------------------------------------------------------

// The thing a serializer or a readback holds. Consulted by every trim path;
// held beside whatever owns the memory, so a document with two independent
// surfaces can pin one and trim the other.
class TrimGate {
  public:
    bool pinned() const { return pins_ != 0; }
    std::uint32_t pins() const { return pins_; }

  private:
    friend class MemoryPin;
    std::uint32_t pins_ = 0;
};

// RAII, and reentrant: a readback inside a save must not un-pin the save when
// it returns. An early return — which is what a cancelled save IS — cannot
// leave a gate pinned forever.
class MemoryPin {
  public:
    explicit MemoryPin(TrimGate& gate) : gate_(&gate) { ++gate_->pins_; }
    ~MemoryPin() {
        if (gate_ != nullptr && gate_->pins_ != 0) --gate_->pins_;
    }
    MemoryPin(const MemoryPin&) = delete;
    MemoryPin& operator=(const MemoryPin&) = delete;

  private:
    TrimGate* gate_;
};

// True when a trim must do nothing. A null gate is "nothing is pinned", which
// is what every caller that has not adopted the pin passes.
inline bool trim_blocked(const TrimGate* gate) { return gate != nullptr && gate->pinned(); }

// -- telemetry ------------------------------------------------------------------

// HIGH-WATER MARKS, not averages, and this is the type the allocation gate
// reads. A `std::vector<char>` sized to the vertex count, allocated once during
// warm-up and reused forever, performs no allocation and costs no bytes on a
// warm stamp — it passes both of the assertions the allocation harness already
// makes, and it is still O(model) storage whose touch cost scales with the
// surface. The only thing that catches it is a peak that does not move between
// a 1M and a 20M fixture at the same footprint.
struct PeakTelemetry {
    std::size_t scratch_bytes = 0;      // the largest working set a stamp needed
    std::size_t workset_vertices = 0;   // the largest footprint gathered
    std::size_t dirty_chunks = 0;       // the deepest the dirty set has been
    std::size_t topology_ops = 0;       // splits and collapses in one stamp

    void observe_scratch(std::size_t n);
    void observe_workset(std::size_t n);
    void observe_dirty(std::size_t n);
    void observe_topology(std::size_t n);
    void reset();
};

}  // namespace memory
}  // namespace clay
