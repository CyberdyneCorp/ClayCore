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
    std::size_t stamp(MeshBrush verb, const MeshBrushSettings& settings,
                      const field::MaskGate& gate = {}, MultiresDelta* record = nullptr);

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

    // The automask factors `mesh` cannot compute for itself, set once for a
    // STROKE. Forwarded to whichever level sculptor is bound, including one
    // bound later, so changing the sculpt level mid-stroke does not silently
    // drop them.
    void set_automask_inputs(AutomaskInputs inputs);

    // The level vertices the last stamp actually moved. Not the workset: the
    // rim of a falloff and a fully masked vertex are gathered and never move.
    const std::vector<std::uint32_t>& last_write_vertices() const { return touched_; }

   private:
    void bind();

    MultiresSurface& surface_;
    std::unique_ptr<MeshSculptor> sculptor_;
    std::uint32_t bound_level_ = 0xffffffffu;
    std::uint64_t bound_generation_ = 0;
    AutomaskInputs automask_;
    bool automask_set_ = false;
    // The record the level sculptor writes, which is where a level-0 gesture's
    // "before" positions come from. Reset per stamp above level 0, kept across
    // a stroke at level 0 so the FIRST before survives.
    VertexDeltas level_deltas_;
    std::vector<std::uint32_t> touched_;
};

}  // namespace mesh
}  // namespace clay
