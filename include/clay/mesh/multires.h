#pragma once

// A MULTIRESOLUTION SURFACE (mesh-multires spec, add-mesh-multires): a base
// cage, a deterministic subdivision hierarchy over it, and per-level detail
// that survives edits to the form beneath it.
//
// THE GAP THIS CLOSES, stated as an artist would: add wrinkles at a fine level,
// go back and change the skull underneath them, come back and find the wrinkles
// still there and still attached. Nothing in this library could do that,
// because on a mesh layer the detail IS the vertex positions and there is no
// record of which part of a position was form and which part was wrinkle. A
// proportion pass therefore destroyed the pass before it.
//
// THE MODEL, and it is one line:
//
//     P(0) = the base cage's positions          authoritative, sculpted directly
//     S(n) = Subdivide(P(n-1))                  pure Catmull-Clark, no detail
//     P(n) = S(n) + Frame(n) * Detail(n)        the level as the artist sees it
//
// Levels are NOT a stack of unrelated meshes. With no relationship between a
// level and its parent, a change to the parent has no defined effect on the
// child, so either the fine detail or the coarse edit has to be discarded — and
// keeping both is the entire purpose. The relationship IS the feature.
//
// A REPRESENTATION BESIDE THE OTHER TWO, never a mode either of them slips
// into. `MeshSculptor`'s contract — no verb creates, splits, deletes or
// reorders a polygon — is untouched, and `DynamicSurface` answers the opposite
// question (make geometry where the brush needs it). The three are ordered by
// what may change:
//
//     MeshSculptor      topology never changes
//     DynamicSurface    topology changes locally
//     MultiresSurface   topology changes only by deterministic subdivision
//
// `mesh::Mesh` remains the interchange format and gains nothing: a level
// EXPORTS as an ordinary mesh, so validation, decimation, every exporter and
// the readback accessors need no knowledge of any of this.
//
// WHAT IS AUTHORITATIVE. The base cage, the per-level face lists, and the
// per-level `DetailField`. Everything else — subdivided positions, normals,
// frames, per-level adjacency, the level's own `mesh::Mesh` — is a cache that
// can be released and rebuilt bit-identically, which is what `memory()`
// separates and what `drop_inactive_caches()` acts on. Authoritative detail is
// never reported as rebuildable, because a host under pressure acts on that
// distinction.
//
// ADDING A LEVEL PREFLIGHTS. Catmull-Clark multiplies faces by four, so a 20k
// quad cage is 5.1M faces at level 4 and 20.5M at level 5, and on the device
// this library targets it is the PEAK allocation that kills an app rather than
// the steady state. `preflight_add_level` prices the level before a byte of it
// is allocated and `add_level` refuses rather than allocating half.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/mesh/adjacency.h"
#include "clay/mesh/detail_field.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/project.h"
#include "clay/mesh/subdivide.h"
#include "clay/mesh/surface_frame.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace mesh {

// Why an operation was refused. A typed reason rather than a bare false,
// because a host fixing a model needs to know which problem it hit and a host
// logging one needs it readable.
enum class MultiresError : std::uint32_t {
    None = 0,
    // The cage has no faces, or an index count that is not a multiple of its
    // arity.
    EmptyBase = 1,
    IndexOutOfRange = 2,
    // A face whose corners weld together: not a face of that arity, and every
    // level above it would carry a quad with two coincident corners.
    DegenerateFace = 3,
    // Three or more faces on one edge. "The two faces beside this edge" is not
    // a question with an answer there, and the subdivision rules ask it.
    NonManifold = 4,
    LevelOutOfRange = 5,
    // Nothing above the base to remove.
    NoLevelToRemove = 6,
    // The predicted cost exceeds the declared budget. Refused whole.
    OverBudget = 7,
    Cancelled = 8,
    // A base topology change on a hierarchy that carries detail. The stencils
    // above it are defined by the connectivity being changed, so every
    // coefficient stored against them would become uninterpretable.
    DetailPresent = 9,
    // A stream declaring a hierarchy this build will not reconstruct.
    DepthLimit = 10,
    Decode = 11,
};

// The name of the refusal, for a message. Never null.
const char* multires_error_text(MultiresError error);

struct MultiresOptions {
    SubdivisionRule rule = SubdivisionRule::CatmullClark;
    // Vertices closer than this are one geometric point of the cage. The same
    // rule and the same default `Adjacency` and `MeshSculptor` use, so a mesh
    // welds the same way on every path through this library.
    float weld_epsilon = kDefaultWeldEpsilon;
    // What the caller will let a level cost, in bytes. Zero means no budget,
    // which is what a test and a desktop host want and what a memory-
    // constrained one must not use.
    std::uint64_t memory_budget = 0;
};

// What adding a level would cost, asked before it is paid.
struct MultiresPreflight {
    std::uint32_t level = 0;  // the level that would come into existence
    std::uint64_t vertices = 0;
    std::uint64_t faces = 0;

    // Kept for the life of the level.
    std::uint64_t topology_bytes = 0;  // the face list and its patch ids
    std::uint64_t detail_bytes = 0;    // if every vertex were detailed

    // Held only while the level is resident.
    std::uint64_t evaluated_bytes = 0;  // subdivided positions, frames, positions, normals
    std::uint64_t runtime_bytes = 0;    // connectivity, the level mesh, adjacency

    // What remains after the call, and the high-water mark during it. The
    // second is the number that matters on a device that kills an app rather
    // than warning it: the connectivity of the PARENT level has to exist while
    // the child is generated, and it is transient.
    std::uint64_t persistent_bytes = 0;
    std::uint64_t peak_bytes = 0;

    bool allowed = true;
    MultiresError error = MultiresError::None;
};

// What a hierarchy costs, split by what a host under pressure may act on.
struct MultiresMemory {
    // -- authoritative: this IS the user's work, and none of it is droppable --
    std::size_t base = 0;      // the cage, with its attributes
    std::size_t topology = 0;  // every level's face list
    std::size_t detail = 0;    // every level's coefficients
    std::size_t authoritative = 0;

    // -- rebuildable, in the order a host should reach for it -----------------
    std::size_t evaluated = 0;      // subdivided positions, frames, positions, normals
    std::size_t runtime_index = 0;  // connectivity, level meshes, adjacency
    std::size_t rebuildable = 0;

    std::size_t total = 0;
    std::uint32_t resident_levels = 0;
};

// What the last evaluations actually did, so "propagation is local" is a
// MEASUREMENT rather than a claim.
//
// The spec asks for the cost of a dab to be measured against a cold full
// reconstruction rather than asserted, and there is no other way to see the
// difference from outside: both produce the same surface, and the fast one is
// only visibly fast on a hierarchy too large to put in a test.
struct MultiresEvalStats {
    std::uint64_t vertices_evaluated = 0;   // subdivided and re-framed
    std::uint64_t normals_recomputed = 0;   // display normals rewritten
    std::uint64_t full_level_rebuilds = 0;  // a whole level redone
    std::uint64_t partial_level_updates = 0;
};

struct MultiresExportOptions {
    // Emit each attribute the CAGE carried. A hierarchy over a mesh with no
    // colours still exports none, so a layer's attribute set does not change
    // under a round trip through this representation.
    bool normals = true;
    bool uvs = true;
    bool colors = true;
};

// -- the surface --------------------------------------------------------------

class MultiresSurface {
   public:
    MultiresSurface();
    ~MultiresSurface();
    MultiresSurface(MultiresSurface&&) noexcept;
    MultiresSurface& operator=(MultiresSurface&&) noexcept;
    MultiresSurface(const MultiresSurface&) = delete;
    MultiresSurface& operator=(const MultiresSurface&) = delete;

    // Build a hierarchy with ONE level — the cage itself. Adding levels is a
    // separate, priced operation.
    //
    // Returns nullopt and sets `out_error` when the cage cannot carry a
    // hierarchy; see `MultiresError`. Refuses rather than repairs, because a
    // conversion that quietly welds a face changes the retopology somebody paid
    // for without saying so.
    static std::optional<MultiresSurface> from_mesh(const Mesh& mesh,
                                                    const MultiresOptions& options = {},
                                                    MultiresError* out_error = nullptr,
                                                    const parallel::CancelToken* cancel = nullptr);

    bool valid() const;
    SubdivisionRule rule() const;
    const MultiresOptions& options() const;

    // The cage, as it was given and as it now is. Its positions follow a
    // level-0 sculpt; its connectivity and attributes never change.
    const Mesh& base_mesh() const;

    // 1 for a cage with nothing above it. `max_level() == level_count() - 1`.
    std::uint32_t level_count() const;
    std::uint32_t max_level() const;

    // WHERE THE BRUSH WRITES and WHAT THE HOST DRAWS, independently. Editing a
    // coarse level while displaying a fine one is the workflow the feature
    // exists for: move the broad form and watch the pores move with it.
    std::uint32_t sculpt_level() const;
    std::uint32_t display_level() const;
    bool set_sculpt_level(std::uint32_t level);
    bool set_display_level(std::uint32_t level);

    // -- levels ---------------------------------------------------------------

    // What the next level would cost. Free of side effects and free of
    // allocation: the counts follow from the rule and the level below.
    MultiresPreflight preflight_add_level() const;

    // Add one level. BUILD-THEN-PUBLISH: the level is assembled into locals and
    // attached in one step, so a refusal or a cancellation leaves the surface
    // exactly as it was rather than half subdivided.
    //
    // Sets the sculpt and display levels to the new one, which is what an
    // artist means by "subdivide".
    bool add_level(MultiresError* out_error = nullptr,
                   const parallel::CancelToken* cancel = nullptr);

    // Drop the highest level and everything stored on it. Refuses on a cage
    // with nothing above it.
    //
    // DESTRUCTIVE, and the owner above this is what makes it reversible: the
    // level's detail is handed to `out_detail` when asked for, which is how
    // `MultiresDelta` records enough to put it back.
    bool remove_highest_level(MultiresError* out_error = nullptr,
                              DetailField* out_detail = nullptr);

    // Replace the cage. REFUSED while any level carries detail: the
    // relationship between a level and its parent is defined by the stencils
    // over this connectivity, so changing it makes every coefficient above
    // meaningless. The supported route is `project_from` in `project.h` — an
    // explicit, priced conversion rather than an implicit consequence of an
    // edit.
    bool set_base_mesh(const Mesh& mesh, MultiresError* out_error = nullptr);

    // Rebuild every level's detail from a reference surface: zero the level,
    // subdivide, project the result onto the reference, and store the
    // difference as coefficients — level by level, so each one projects from a
    // parent that has already been fitted.
    //
    // THE SUPPORTED ROUTE by which a hierarchy accepts a sculpt made somewhere
    // else, and the operation `set_base_mesh` names when it refuses. Explicit
    // and expensive by design: it evaluates every level twice and builds a BVH
    // over the reference, which is the honest price of changing what a
    // coefficient is measured against.
    bool project_from(const Mesh& reference, const ProjectOptions& options = {},
                      ProjectReport* out_report = nullptr,
                      const parallel::CancelToken* cancel = nullptr);

    // -- evaluation -----------------------------------------------------------
    //
    // These BUILD, which is why they are not const. Asking for a level
    // evaluates every level below it that is out of date, and only the vertices
    // that are.

    const LevelTopology& topology_at(std::uint32_t level) const;
    const LevelConnectivity& connectivity_at(std::uint32_t level);

    // P(n): the level as the artist sees it.
    const std::vector<kernel::cfloat3>& positions_at(std::uint32_t level);
    // S(n): the pure subdivision, with no detail applied. What a coefficient is
    // measured FROM, and the reason a coefficient survives an edit below it.
    const std::vector<kernel::cfloat3>& subdivided_at(std::uint32_t level);
    const std::vector<SurfaceFrame>& frames_at(std::uint32_t level);
    // Normals of P(n), for display and for the brushes.
    const std::vector<kernel::cfloat3>& normals_at(std::uint32_t level);

    // The level as an ordinary mesh. Positions are P(n); the attributes are the
    // cage's, subdivided over their OWN connectivity so that a UV seam is
    // interpolated along itself and never across itself.
    Mesh mesh_at_level(std::uint32_t level, const MultiresExportOptions& options = {},
                       const parallel::CancelToken* cancel = nullptr);

    // One base patch's faces at a level, as a STANDALONE chunk: the level
    // vertices it uses, ascending, and triangle indices LOCAL to that list, so
    // a host uploads it as its own draw.
    //
    // THE UNIT OF HOST TRANSPORT, and the reason it is a base patch rather than
    // a spatial cell: a base face owns a subtree that never moves between
    // faces, so a chunk's identity is stable for the life of the hierarchy and
    // a re-partition can never invalidate what a host has already uploaded.
    // Copying the display level after every dab is the alternative, and on a
    // deep hierarchy it is the difference between a preview that keeps up and
    // one that does not.
    struct Block {
        std::uint32_t patch = 0;
        std::uint32_t level = 0;
        std::vector<std::uint32_t> vertices;  // level vertex ids, ascending
        std::vector<std::uint32_t> indices;   // triangles, local to `vertices`
    };
    bool build_block(std::uint32_t level, std::uint32_t patch, Block* out);

    // The level's own mesh and adjacency, which the sculptor drives directly.
    // Public because `multires_sculpt.cpp` is a separate translation unit and
    // these are its vocabulary, not because a caller should write through them
    // — a position written here is a position the hierarchy does not know about
    // until `absorb_level_edit` is told.
    Mesh& level_mesh(std::uint32_t level);
    const Adjacency& level_adjacency(std::uint32_t level);

    // How many GEOMETRIC vertices the cage has — its weld classes, which is what
    // a level-0 vertex index means everywhere in this API. Not the same as
    // `base_mesh().positions.size()` on a cage with a seam, where one geometric
    // point is several raw vertices.
    std::uint32_t base_vertex_count() const;
    // One geometric vertex's cage position. Const, so an undo record can read
    // what it is about to overwrite without forcing an evaluation.
    kernel::cfloat3 base_position(std::uint32_t vertex) const;

    // -- detail ---------------------------------------------------------------

    const DetailField& detail_at(std::uint32_t level) const;
    DetailField& detail_mutable(std::uint32_t level);

    // A hash of every level's authoritative detail. What a test compares to
    // assert that releasing and rebuilding the caches changed nothing that
    // matters.
    std::uint64_t detail_checksum() const;

    // -- editing --------------------------------------------------------------

    // Take the positions the caller has written into `level_mesh(level)` for
    // these vertices and turn them into what the hierarchy stores: base
    // positions at level 0, detail coefficients above it. Marks the level dirty
    // so the levels above it re-evaluate locally.
    //
    // The ONE write path. A caller that moved a vertex and did not call this
    // has moved a cache.
    void absorb_level_edit(std::uint32_t level, const std::vector<std::uint32_t>& vertices);

    // Write one vertex's detail directly, in coefficients. What undo replays
    // and what a detail-erasing verb writes.
    void set_detail(std::uint32_t level, std::uint32_t vertex, const LocalDetail& value);
    // Write one base-cage position, propagating it to every raw vertex of the
    // class so a seam cannot open into a crack.
    void set_base_position(std::uint32_t vertex, kernel::cfloat3 position);

    // -- what changed ---------------------------------------------------------
    //
    // THREE revisions, not one. A host re-uploads an index buffer only when the
    // hierarchy's shape changed, re-reads detail only when it changed, and
    // redraws only when the evaluated surface moved; one counter cannot say
    // which happened.
    std::uint64_t base_revision() const;
    std::uint64_t detail_revision() const;
    std::uint64_t evaluated_revision() const;

    // The BASE PATCHES touched since the last `clear_dirty`. A base face owns a
    // subtree that never moves between faces, so its id is the stable chunk
    // identity a host uploads by — and the alternative, copying the display
    // level after every dab, is what makes a fine hierarchy unusable over a
    // host boundary.
    const std::vector<std::uint32_t>& dirty_patches() const;
    void clear_dirty();

    const MultiresEvalStats& eval_stats() const;
    void reset_eval_stats();

    // -- residency ------------------------------------------------------------

    // Bumped whenever a level's rebuildable cache is created. A sculptor bound
    // to a level's mesh compares it and rebinds rather than holding a reference
    // into storage a `drop_*_caches` released underneath it — which is a
    // use-after-free that a host under memory pressure would find first.
    std::uint64_t cache_generation() const;

    MultiresMemory memory() const;
    // Release the rebuildable caches of every level that is neither the sculpt
    // level nor the display level nor an ancestor one of them needs. Rebuilding
    // reproduces them bit-identically; the authoritative detail is untouched.
    void drop_inactive_caches();
    // Release every level's cache EXCEPT the sculpt level's and the display
    // level's — including the levels between them and the cage.
    //
    // The middle option between the two above, and the one a host reaches for
    // while an artist is working at a fine level: a stamp there reads that
    // level's own subdivided positions and frames and nothing else, so on a
    // deep hierarchy the levels below it are holding memory that the next dab
    // will not touch. Pending work is flushed first, so nothing is lost; the
    // cost is that the next edit BELOW the active levels rebuilds what it
    // needs.
    void drop_intermediate_caches();

    // Release every cache, including the active levels'. What a host does under
    // real pressure, and what a test does to prove the caches carry nothing.
    void drop_all_caches();
    bool level_resident(std::uint32_t level) const;

    // -- serialization --------------------------------------------------------
    //
    // A versioned form of its own carrying the RULE it was built with. A
    // hierarchy reconstructed with a different rule than it was authored with is
    // a different surface, and nothing else in the stream reveals the
    // substitution.
    //
    // The per-level FACE LISTS are not written: they follow from the cage and
    // the rule, and a level 4 face list is eighty megabytes of something a
    // reader can derive. What is written is the cage, the rule, the level
    // count, the active levels, and each level's detail.
    std::vector<std::uint8_t> encode() const;
    // Refuses a truncated, hostile or newer buffer rather than returning a
    // surface whose stencils point at nothing — including a stream that
    // declares a depth whose reconstruction over its own cage would exceed this
    // build's ceiling, which is refused BEFORE anything is allocated.
    static bool decode(const std::uint8_t* data, std::size_t size, MultiresSurface* out);

    // The deepest hierarchy this build will reconstruct from a stream, and the
    // most vertices it will accept at any level. Both are arithmetic guards
    // rather than preferences: a few hundred bytes declaring level 20 over a
    // 20k cage is a request for more memory than a machine holds.
    static constexpr std::uint32_t kMaxLevels = 12;
    static constexpr std::uint64_t kMaxLevelVertices = 1ull << 30;

    // The implementation's own state. Declared here — rather than hidden
    // behind the pointer alone — because the three translation units that
    // implement this class have to name it, and it is DEFINED in
    // `src/mesh/multires_internal.h`, which nothing outside them includes. A
    // caller can see the name and nothing else about it.
    struct State;

   private:
    std::unique_ptr<State> state_;
};

}  // namespace mesh
}  // namespace clay
