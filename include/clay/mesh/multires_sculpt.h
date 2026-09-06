#pragma once

// SCULPTING A HIERARCHY (mesh-multires spec, add-mesh-multires): the same
// verbs, the same falloffs, the same mask, the same alpha, the same automasking
// — and the same DEFORMATION MATH, because it is the fixed sculptor's, called
// rather than copied.
//
// HOW LITERALLY IT IS THE SAME CODE. This does not reimplement a gather, a
// geodesic walk or a write-back. The active level's evaluated positions already
// live in a `mesh::Mesh` inside the level's cache, and that mesh has an
// `Adjacency` built over it — so a stamp is `MeshSculptor::stamp` on the level,
// full stop. Every verb, the surface-aware reach, the weld-class write, the
// local normal recompute, the BVH refit and the write region come along
// unchanged and unchangeable. An artist who learns a brush on a mesh layer
// finds it behaves identically here because it IS that brush, not because two
// implementations were kept in step.
//
// WHAT THIS FILE OWNS is only what is about the HIERARCHY, and it is three
// steps:
//
//   1. bind a `MeshSculptor` to the sculpt level's mesh;
//   2. take the positions the stamp wrote and turn them back into what the
//      hierarchy stores — the cage's own geometry at level 0, detail
//      coefficients in the transported frame above it;
//   3. propagate: the levels above re-evaluate the descendants of what moved
//      and nothing else.
//
// THE UNDO RECORDS WHAT WAS EDITED, NOT WHAT WAS DERIVED. A stroke at level 1
// of a five-level hierarchy moves millions of vertices at level 5, and every
// one of them is `Subdivide(parent) + Detail`, which the hierarchy can
// reconstruct. Recording them would multiply an undo step by the level count
// and carry no information. So `MultiresDelta` holds coefficients and cage
// positions — the level, the vertex, the value before and the value after.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "clay/field/relax.h"  // MaskGate
#include "clay/mesh/detail_field.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/sculpt_common.h"

namespace clay {
namespace mesh {

// A sparse, coalesced record of one multiresolution gesture.
//
// COALESCED PER GESTURE, exactly as `VertexDeltas` is: a vertex touched by forty
// stamps of one stroke appears once, keeping the FIRST `before` and the LAST
// `after`. The record's size follows the vertices the stroke REACHED at the
// level it was made on — not the stamps it took, and not the levels above it.
class MultiresDelta {
   public:
    std::size_t size() const { return detail_.size() + base_vertices_.size(); }
    bool empty() const { return detail_.empty() && base_vertices_.empty(); }
    void clear();
    std::size_t bytes() const;

    // The levels this gesture touched, ascending. A host showing "what did this
    // undo step change" has no other way to ask.
    std::vector<std::uint32_t> levels() const;

    // Capture, the first time this entry is seen. Public because the sculptor
    // and a host draining a stroke drive the same record.
    void note_detail(std::uint32_t level, std::uint32_t vertex, const LocalDetail& before);
    void note_base(std::uint32_t vertex, kernel::cfloat3 before);
    // Rewrite every entry's `after` from the surface as it now is, so the last
    // stamp of the gesture wins.
    void sync_after(const MultiresSurface& surface);

    // Restore / re-apply. Both are idempotent. Refused — returning false and
    // changing nothing — against a surface whose levels or counts do not match
    // the record, which is a caller pairing a step with the wrong surface.
    bool revert(MultiresSurface& surface) const;
    bool apply(MultiresSurface& surface) const;

    // -- encoding (survive-a-crash) -------------------------------------------
    // A member for the reason `VertexDeltas::encode` is one: the `after` values
    // have no public accessors, and widening the read surface to serialize from
    // outside would be a worse trade than owning the bytes here.
    std::vector<std::uint8_t> encode() const;
    static bool decode(const std::uint8_t* data, std::size_t size, MultiresDelta* out);

   private:
    struct DetailEntry {
        std::uint32_t level = 0;
        std::uint32_t vertex = 0;
        LocalDetail before, after;
    };

    static std::uint64_t key_of(std::uint32_t level, std::uint32_t vertex) {
        return (static_cast<std::uint64_t>(level) << 32) | vertex;
    }

    std::vector<DetailEntry> detail_;
    std::vector<std::uint32_t> base_vertices_;
    std::vector<kernel::cfloat3> base_before_, base_after_;
    // The slot indices are NOT encoded: they are derivable from the entries,
    // and storing a hash map's contents would be storing a rebuildable thing.
    std::unordered_map<std::uint64_t, std::uint32_t> detail_slot_;
    std::unordered_map<std::uint32_t, std::uint32_t> base_slot_;
};

// THE HIERARCHY'S OWN VIEW OF WHAT A STAMP REACHED: the bound level sculptor's
// workset with every item re-tagged from a level weld class to
// `WorkItemId::level_vertex(level, vertex)`.
//
// THIN, AND STILL WORTH HAVING. It walks nothing and weighs nothing — a
// multiresolution stamp IS `MeshSculptor::stamp` on the bound level, which is
// the whole point of this representation — but without it a multiresolution
// write region is reportable only at the LEVEL MESH's addressing, and a host
// that asked the hierarchy what moved would be answered in a number that means
// something different at every level.
//
// ONE ENTRY PER LEVEL WELD CLASS, tagged with the class's first member. That is
// the vertex whose position the workset already carries: `MeshSculptor` gathers
// a class's geometry from `members(c)[0]` and writes every member the same
// value, so a class is one point and one displacement whatever its member count.
// The vertices sharing it are not lost — `MultiresSculptor::last_write_vertices`
// expands them, and that expansion is what `absorb_level_edit` consumes — they
// are simply not separate work items, because nothing can move them separately.
//
// Declared here rather than in `sculpt_workset.h` for the reason the other two
// builders are declared beside their representations: it names a
// `MeshSculptor`, and a neutral header holding three representation-specific
// signatures is neutral in the directory listing only.
void build_multires_workset(const MeshSculptor& level_sculptor, std::uint32_t level,
                            SculptWorkset* out);

// Whether a hierarchy offers this verb.
//
// ALL SIXTEEN, including Layer — which the adaptive surface declines. The
// reason it can be offered here is the reason this representation exists: the
// topology at a level does not change under the brush, so "where the surface
// was when the stroke began" is a question every vertex under the brush has an
// answer to.
bool multires_offers(MeshBrush verb);

class MultiresSculptor {
   public:
    explicit MultiresSculptor(MultiresSurface& surface);
    ~MultiresSculptor();

    // ONE STAMP at the surface's current sculpt level.
    //
    // Returns the number of weld classes that moved, which is 0 for a stamp
    // that reached nothing, that was fully masked, or whose settings amount to
    // no displacement — the same answer `MeshSculptor::stamp` gives, because it
    // is that call.
    //
    // `gate` is the freeze, taken exactly as every other representation takes
    // one. `record`, when given, accumulates into the caller's gesture.
    //
    // A WRITE MAY BE DESTINED FOR A SCULPT LAYER. When the surface's stack has
    // an active layer, the displacement this stamp made is recorded into that
    // layer rather than into the level's base detail — see
    // `MultiresSurface::absorb_level_edit`, which is still the one write path
    // and still where the arithmetic lives. Two consequences are visible here:
    //
    //   * a LOCKED active layer refuses the stamp, and refuses it BEFORE the
    //     brush moves anything, so the level's mesh is never left holding a
    //     displacement the hierarchy declined to store. Returns 0, exactly as a
    //     stamp that reached nothing does;
    //   * `layer_record`, when given, accumulates the LAYER's coefficients
    //     before and after, which is what an undo of a layered gesture needs.
    //     `record` accumulates the base's, as it always has. A gesture writes
    //     one or the other, never both, because the active layer is one thing.
    //
    // A STAMP THAT CROSSES A DEPTH BOUNDARY WRITES SEVERAL LEVELS, and the
    // count returned is the whole of it. On a regionally refined hierarchy the
    // surface under the brush is not all at the sculpt level: the patches the
    // artist refined are, and the ones beside them are one or more levels
    // coarser, with no vertex at the sculpt level for the brush to move. Before
    // this, a stamp reaching past the refined region deposited the full falloff
    // at the rim and nothing at all beyond it -- a step in the displacement
    // rather than a fade, and a silent one: the count came back looking like a
    // stamp that had done its whole job.
    //
    // WHICH LEVEL A VERTEX IS WRITTEN AT is not a new rule. It is exactly the
    // partition `mixed_mesh_at_level` emits: a vertex belongs to the level that
    // surface carries it at, which is its own level unless the level above
    // holds its vertex point -- in which case that finer vertex is the one the
    // artist is looking at, and the one the brush moves. Every vertex of the
    // mixed-depth surface therefore belongs to exactly ONE level, which is what
    // makes a doubled contribution at the seam structurally impossible rather
    // than something a tolerance has to catch.
    //
    // COARSEST FIRST, AND THE BOUND LEVEL WRITTEN AS ABSOLUTE POSITIONS. A
    // coarse write moves `S(n)` under the finer levels beside it, so a fine
    // level absorbed BEFORE it keeps a coefficient that then reconstructs to
    // the position the brush asked for PLUS that ripple. Measured on a 6x6 cage
    // with the middle 2x2 refined to level 3, one Draw stamp of radius 0.50
    // anchored on the rim, against the same stamp on a uniformly refined
    // hierarchy: writing the fine level first finishes 0.106460609 away,
    // writing one level and dropping the rest finishes 0.182510689 away, and
    // this order finishes 0.005068991 away.
    //
    // WHAT IT COSTS, in counts rather than a clock, because a clock here would
    // measure the box: one region walk per level below the sculpt level that
    // owns any vertex, over a disc of the same radius on a surface with a
    // quarter of the vertices per level down. Measured on the fixture above
    // with a radius of 0.30, the walk finds 113 classes at level 3, 37 at level
    // 2 and 12 at level 1 -- 43% of the sculpt level's walk for both coarse
    // levels together, and the tail of that series is bounded rather than
    // proportional to the depth. A uniform-depth hierarchy owns nothing below
    // its top level, takes none of these walks, and is byte-identical to what
    // it was before this existed.
    std::size_t stamp(MeshBrush verb, const MeshBrushSettings& settings,
                      const field::MaskGate& gate = {}, MultiresDelta* record = nullptr,
                      SculptLayerDelta* layer_record = nullptr);

    const MultiresSurface& surface() const { return surface_; }
    MultiresSurface& surface() { return surface_; }

    // Start a new gesture. Clears the level record `MeshBrush::Layer` measures
    // its ceiling against, so a second stroke over the same place deposits from
    // the surface as THAT stroke found it rather than as the first one did.
    // Called implicitly when the sculpt level changes.
    void begin_stroke();

    // The level this sculptor is currently bound to, and the underlying fixed
    // sculptor over it — for a caller that wants the BVH for picking, the write
    // region for an upload, or the plan-compilation count a stroke asserts on.
    std::uint32_t bound_level() const { return bound_level_; }
    MeshSculptor* level_sculptor();

    // The seed token for the level this sculptor is bound to RIGHT NOW, for a
    // host that picked a `seed_class` off the level mesh and wants the stamp to
    // be able to tell whether that class still means what it meant.
    //
    // This is the call the token exists for. A hierarchy rebinds — and so
    // renumbers — whenever the sculpt level or the cache generation moves, and
    // both happen behind a host: the first when it changes level, the second
    // when a trim releases the caches under memory pressure. A seed picked
    // before either is in bounds and wrong, and spends the dab on an empty
    // region rather than reporting anything.
    //
    // BINDS, because the answer is a property of the bound level and a caller
    // asking before the first stamp would otherwise get the token of whatever
    // was bound last. Returns `kNoSeedRevision` for a surface that cannot bind,
    // which is the value that claims nothing.
    std::uint64_t seed_revision();

    // Normals follow the vertices, and a host draining a stroke can defer the
    // recompute to the end of it. Forwarded to whichever level sculptor is
    // bound, because deferring is a property of the STROKE rather than of the
    // level it lands on. Deferring changes nothing about the final surface.
    //
    // NOT FORWARDED TO THE COARSE SCULPTORS a crossing stamp runs, and that is
    // deliberate rather than an omission. Each of those exists for ONE stamp
    // and is destroyed with it -- see `CoarseLevel` for why -- so there is no
    // stroke for them to defer into, and `flush_normals` below cannot reach a
    // sculptor that no longer exists. Measured by forwarding it anyway: a
    // deferred crossing stamp then leaves the coarse level's normals stale
    // (its positions are unaffected) and marks 0 of its chunks
    // `ChunkDirty::Normals` where an immediate one marks 2, so a host draining
    // the stroke draws the coarse side of the transition with the normals it
    // had before. "Deferring changes nothing about the final surface" is a
    // promise this class keeps by paying the coarse side's recompute per stamp.
    void set_defer_normals(bool defer);
    bool defer_normals() const { return defer_normals_; }
    void flush_normals();

    // Where the level sculptor publishes the workset high-water mark a host
    // tunes a `SculptMemoryProfile` against. Borrowed and never owned; null is
    // the default.
    //
    // Forwarded to whichever level is bound, INCLUDING one bound later, for the
    // same reason `set_defer_normals` is forwarded rather than set once: a
    // rebind builds a NEW `MeshSculptor`, and a telemetry block that stopped
    // filling the moment the host changed level — or the moment a trim moved
    // the cache generation — would report a peak belonging to whichever level
    // happened to be bound first rather than to the session. That is worse than
    // reporting nothing, because it looks like an answer.
    void set_telemetry(memory::PeakTelemetry* telemetry);
    // Per-stage timing, forwarded to whichever level sculptor is bound now and
    // to every one bound after — a rebind must not silently drop it, which is
    // the same rule the automask and the peak telemetry already follow here.
    void set_stage_telemetry(StageTelemetry* stages);
    memory::PeakTelemetry* telemetry() const { return telemetry_; }

    // The automask factors `mesh` cannot compute for itself, set once for a
    // STROKE. Forwarded to whichever level sculptor is bound, including one
    // bound later, so changing the sculpt level mid-stroke does not silently
    // drop them.
    void set_automask_inputs(AutomaskInputs inputs);

    // The level vertices the last stamp actually moved. Not the workset: the
    // rim of a falloff and a fully masked vertex are gathered and never move.
    //
    // THE BOUND LEVEL'S, and it keeps that meaning: a stamp that crossed a
    // depth boundary also wrote coarser levels, and a caller reading this as
    // "everything that moved" was reading it that way before there was anything
    // else to read. The two calls below are the whole answer.
    const std::vector<std::uint32_t>& last_write_vertices() const { return touched_; }

    // Every level the last stamp wrote, ASCENDING -- the coarse ones it reached
    // past the refined region, then the bound level. One entry for a stamp that
    // stayed inside the region, and one for every stamp on a uniform-depth
    // hierarchy.
    const std::vector<std::uint32_t>& last_write_levels() const { return write_levels_; }
    // That level's vertices, or empty for a level the last stamp did not write.
    const std::vector<std::uint32_t>& last_write_vertices_at(std::uint32_t level) const;

    // The last stamp's workset, at the HIERARCHY's addressing — see
    // `build_multires_workset`. Empty before the first stamp.
    const SculptWorkset& workset() const { return workset_; }

    // The per-stamp scratch arena, which is the BOUND LEVEL SCULPTOR's: a
    // multiresolution stamp is that sculptor's stamp, so it is that sculptor's
    // arena that grows, and reporting a second empty one here would tell a host
    // budgeting memory that a hierarchy costs nothing. Null when no level is
    // bound yet.
    const BrushScratchArena* arena() const;

   private:
    void bind();
    // The levels below the bound one that OWN vertices of the mixed-depth
    // surface: the ones a stamp reaching past the refined region has to write.
    void bind_coarse(std::uint32_t level);
    // One stamp on each of them, coarsest first. Returns the classes it moved.
    std::size_t stamp_coarse(MeshBrush verb, const MeshBrushSettings& settings,
                             const field::MaskGate& gate, SculptLayerId active_layer,
                             MultiresDelta* record, SculptLayerDelta* layer_record);
    // The values the hierarchy is about to overwrite, captured while the fields
    // still hold them.
    void note_before(std::uint32_t level, const std::vector<std::uint32_t>& vertices,
                     const VertexDeltas& deltas, SculptLayerId active_layer, MultiresDelta* record,
                     SculptLayerDelta* layer_record);
    void note_layer_before(std::uint32_t level, const std::vector<std::uint32_t>& vertices,
                           SculptLayerId active_layer, SculptLayerDelta* layer_record);
    void sync_after(SculptLayerId active_layer, MultiresDelta* record,
                    SculptLayerDelta* layer_record);
    // The bound level's write list and the positions the brush asked for at it.
    void capture_fine_targets();

    // A level below the bound one, carrying only what has to survive between
    // the stamps of a stroke.
    //
    // THE `MeshSculptor` DOES NOT, and the trade is stated rather than assumed.
    // It takes its `Adjacency` BY VALUE, so keeping one per level below would
    // hold a second copy of every intermediate level's adjacency for the life
    // of the session — against `drop_intermediate_caches`, which exists because
    // those levels are exactly what a deep hierarchy can afford to release. It
    // is built for the stamp and dropped with it, which costs that copy per dab
    // instead. What survives is the RECORD, because `MeshBrush::Layer` measures
    // its ceiling from where the STROKE found the surface and a coarse level
    // under a crossing stroke has that question to answer about its own
    // vertices.
    struct CoarseLevel {
        std::uint32_t level = 0;
        VertexDeltas deltas;
        std::vector<std::uint32_t> written;
    };

    MultiresSurface& surface_;
    std::unique_ptr<MeshSculptor> sculptor_;
    std::uint32_t bound_level_ = 0xffffffffu;
    std::uint64_t bound_generation_ = 0;
    AutomaskInputs automask_;
    bool automask_set_ = false;
    memory::PeakTelemetry* telemetry_ = nullptr;
    StageTelemetry* stages_ = nullptr;
    bool defer_normals_ = false;
    // The record the level sculptor writes, which is where a level-0 gesture's
    // "before" positions come from. Reset per stamp above level 0, kept across
    // a stroke at level 0 so the FIRST before survives.
    VertexDeltas level_deltas_;
    std::vector<std::uint32_t> touched_;
    // Ascending in level, and only the levels that own a vertex the bound level
    // does not. Rebuilt with the level sculptor, for the same reasons.
    std::vector<CoarseLevel> coarse_;
    std::vector<std::uint32_t> write_levels_;
    // The positions the brush asked for at the bound level, kept across the
    // coarse writes because those re-evaluate the level above them.
    std::vector<kernel::cfloat3> fine_targets_;
    // The vertices a coarse stamp moved that the level above owns, and that go
    // back where they were rather than into a coefficient.
    std::vector<std::uint32_t> not_owned_;
    // Kept as a member rather than built on demand, for the reason every other
    // per-stamp buffer here is one: a stroke allocates on its first stamp only.
    SculptWorkset workset_;
};

}  // namespace mesh
}  // namespace clay
