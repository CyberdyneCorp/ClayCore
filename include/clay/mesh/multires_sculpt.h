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
    const std::vector<std::uint32_t>& last_write_vertices() const { return touched_; }

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
    // Kept as a member rather than built on demand, for the reason every other
    // per-stamp buffer here is one: a stroke allocates on its first stamp only.
    SculptWorkset workset_;
};

}  // namespace mesh
}  // namespace clay
