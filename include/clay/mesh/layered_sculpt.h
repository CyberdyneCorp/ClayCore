#pragma once

// SCULPTING INTO A LAYER, as one transaction (mesh-sculpt-layers spec,
// add-mesh-sculpt-layers).
//
// -- why a transaction, and why THIS transaction ----------------------------
//
// `MultiresSculptor` is stateless between stamps by design: each one absorbs
// into the hierarchy immediately and the caller accumulates a `MultiresDelta`
// if it wants an undo step. That is right for a representation where a stamp is
// self-contained. A layered gesture is not self-contained, for three reasons
// that only appear once a stack exists:
//
//   1. A stroke has to enter ONE layer. Which layer that is has to be fixed at
//      pointer-down, not read again per dab — otherwise a host that changes the
//      active layer mid-stroke splits one gesture across two channels and
//      neither undoes as the artist made it.
//   2. A stamp READS the evaluated surface, which includes every visible
//      layer's contribution. Moving a slider between two stamps would author
//      one gesture against two different surfaces, so the composition is HELD
//      for the length of the stroke and the sliders refuse (see
//      `SculptLayerStack::hold_composition` for why refusing beats deferring).
//   3. CANCEL has to be exact. A layered write is `L += ΔE`, so the only exact
//      restore is the recorded `before` values — which means the record has to
//      exist from the first stamp rather than be reconstructed at the end.
//
// The SHAPE is the SDF sculpt transaction's, deliberately: begin, stamp,
// commit, cancel, with the document — here, the layer — untouched by anything
// but the stamps between them. A host that has written one of these has written
// both.
//
// -- what commit produces ---------------------------------------------------
//
// ONE delta for the whole gesture, coalesced: a hundred stamps over one vertex
// are ONE entry keeping the first `before` and the last `after`. Under symmetry
// every mirrored stamp is another stamp in the same transaction, so a mirrored
// stroke is one layer, one delta and the union of the two sides' coverage —
// which is requirement 3.5 falling out of the shape rather than being enforced.
//
// -- the verbs this adds ----------------------------------------------------
//
// `stamp` is the ordinary sixteen, unchanged, routed into the layer. What is
// new is the family that could not exist before the hierarchy stored form and
// detail apart:
//
//   smooth(Geometry)        today's Laplacian over positions.
//   smooth(DetailOnly)      averages COEFFICIENTS in the target channel and
//                           touches neither the form nor any other layer.
//   smooth(PreserveDetail)  smooths the FORM under the detail and re-applies
//                           the detail unchanged. The mode an artist correcting
//                           anatomy under pores is asking for, and the one that
//                           is impossible on a flat mesh.
//   erase                   the target channel toward zero.
//   restore                 the LEVEL'S OWN detail toward zero — the form back
//                           toward the pure subdivision — leaving every layer
//                           alone.
//
// `erase` and `restore` are BRUSHES and not undo, and the distinction is worth
// stating because a host will be tempted to wire one to the other: undo walks a
// step list backwards and restores what a gesture changed, wherever it was;
// these two move the surface toward a named target under the cursor, and they
// are themselves recorded as gestures that undo.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "clay/field/relax.h"  // MaskGate
#include "clay/mesh/detail_stamp.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt_common.h"
#include "clay/mesh/sculpt_layer.h"

namespace clay {
namespace mesh {

// WHICH FREQUENCY A SMOOTH ACTS ON. Three operations rather than one filter
// with a cutoff, and the split is REPRESENTATIONAL: the hierarchy already
// stores the form and the detail apart, so these are three different arrays
// rather than three settings of one pass. A plain Laplacian over pores removes
// the pores, which is rarely what was asked.
enum class MultiresSmoothMode : std::uint32_t {
    // Positions. Exactly `MeshBrush::Smooth` at the sculpt level, so it behaves
    // identically to the brush an artist already knows because it IS it.
    Geometry = 0,
    // Coefficients in the target channel. The form under them does not move,
    // and no other layer is read or written.
    DetailOnly = 1,
    // The FORM, with the detail re-applied unchanged. Implemented by smoothing
    // the level's subdivided positions and folding the difference into the
    // level's own base detail — which is how this representation records any
    // change to the form — so every layer's contribution rides through it
    // untouched.
    PreserveDetail = 2,
};

// WHERE A STROKE LANDS, chosen by the caller rather than inferred.
//
// The automatic answer is offered and is deliberately not the only one, because
// the two cases a host needs are opposite and neither is a default: "sculpt the
// pass I am working on" and "fix the form UNDER the passes without disturbing
// them" are both ordinary, and a library that only had the first would make the
// second reachable only by hiding every layer first.
enum class MultiresWriteDomain : std::uint32_t {
    // The stack's active layer if there is one, the base if there is not.
    Automatic = 0,
    // The base: the cage at level 0, the level's own detail above it. The form
    // under the passes.
    Geometry = 1,
    // The active layer. Refuses to begin when there is none, rather than
    // silently writing the form the caller asked not to touch.
    Detail = 2,
};

class LayeredMultiresSculptor {
   public:
    explicit LayeredMultiresSculptor(MultiresSurface& surface);
    ~LayeredMultiresSculptor();
    LayeredMultiresSculptor(const LayeredMultiresSculptor&) = delete;
    LayeredMultiresSculptor& operator=(const LayeredMultiresSculptor&) = delete;

    // Set before `begin`. Changing it while a stroke is open does nothing: the
    // domain is resolved once, which is the whole of point 1 in the header.
    void set_write_domain(MultiresWriteDomain domain) { domain_ = domain; }
    MultiresWriteDomain write_domain() const { return domain_; }

    // Open a gesture. Fixes the target channel, holds the composition, and
    // clears the record.
    //
    // FIXES means fixes: the target is re-asserted on the stack before every
    // dab, not written once here. Set-active is allowed while the composition
    // is held — it moves no vertex — so a host that changes channel between two
    // dabs would otherwise split one gesture across two of them. For the length
    // of the gesture the stack's active layer belongs to this transaction, and
    // `commit` and `cancel` both put back what `begin` found.
    //
    // Refuses — changing nothing — on an invalid surface, on a stroke that is
    // already open, on a LOCKED target layer, and on `Detail` with no active
    // layer.
    bool begin();
    bool open() const { return open_; }
    // The channel this stroke is writing. `kNoSculptLayer` means the base.
    SculptLayerId target_layer() const { return target_; }

    // The sixteen verbs, at the surface's sculpt level, into the target
    // channel. Returns what the underlying stamp returns: the number of weld
    // classes that moved, 0 for a stamp that reached nothing.
    std::size_t stamp(MeshBrush verb, const MeshBrushSettings& settings,
                      const field::MaskGate& gate = {});

    // A HEIGHT MAP or a TANGENT-SPACE VECTOR DISPLACEMENT, deposited into the
    // target channel through the brush's own weight — so the falloff, the mask
    // gate, the automasking and the alpha all compose with it exactly as they
    // do with a verb.
    //
    // `settings` places and weighs the brush; `stamp` places and reads the
    // image. `DetailStampMode::Weight` is refused here: a scalar alpha is
    // `MeshBrushSettings::alpha` and routing it through a second entry point
    // would be two ways to say one thing.
    //
    // The shortfall report — a map finer than the level can hold — is available
    // afterwards from `last_stamp_report()`, reported rather than smoothed
    // over.
    std::size_t stamp_detail(const DetailStampSettings& stamp, const MeshBrushSettings& settings,
                             const field::MaskGate& gate = {});
    const DetailStampReport& last_stamp_report() const { return report_; }

    std::size_t smooth(MultiresSmoothMode mode, const MeshBrushSettings& settings,
                       const field::MaskGate& gate = {});
    // The target channel toward zero. Touches neither the base nor any other
    // layer, which is what makes it an eraser for THIS pass rather than a
    // flattening brush.
    std::size_t erase(const MeshBrushSettings& settings, const field::MaskGate& gate = {});
    // The LEVEL'S OWN detail toward zero: the form back toward the pure
    // subdivision, with every layer left alone. Refused at level 0, where the
    // cage has no "pure subdivision" to return to — the honest target there is
    // a proportion pass the artist has to name, and inventing one would be the
    // library deciding what an artist's rest shape is.
    std::size_t restore(const MeshBrushSettings& settings, const field::MaskGate& gate = {});

    // Close the gesture. Hands over the record — `out_layer` when the stroke
    // entered a layer, `out_base` when it entered the base — releases the
    // composition hold, and restores the stack's active layer.
    //
    // A gesture that changed nothing produces an EMPTY record rather than a
    // step, for the reason every recorder in this library drops a no-op:
    // pointer-down and pointer-up with nothing in between must not add an undo
    // that does nothing.
    bool commit(SculptLayerDelta* out_layer = nullptr, MultiresDelta* out_base = nullptr);

    // Discard. Restores the target channel EXACTLY — the recorded `before`
    // values, not a recomputation — and leaves the composition and the active
    // layer as they were found.
    void cancel();

    // How many stamps this gesture has taken, and how many entries the record
    // holds. A hundred stamps over one vertex is one entry, and a test says so
    // by comparing these two.
    std::size_t stamps() const { return stamps_; }
    std::size_t record_size() const { return layer_delta_.size() + base_delta_.size(); }

    // The underlying hierarchy sculptor, for a caller that wants the level's
    // BVH for picking, the write region for an upload, or to set the automask
    // inputs for the stroke.
    MultiresSculptor& level_sculptor() { return sculptor_; }
    const MultiresSurface& surface() const { return surface_; }

   private:
    // One entry of the region under the brush: a level vertex and the composed
    // weight the brush would have applied there.
    struct Weighted {
        std::uint32_t vertex = 0;
        float weight = 0.0f;
    };
    // The region and its weights, from the SCULPTOR'S OWN GATHER rather than a
    // second one. See the implementation: a second region builder would be a
    // second answer to "what is under the brush", and the falloff, the geodesic
    // walk, the path taper, the mask gate, the alpha and the automasking would
    // all have to be kept in step by hand.
    bool gather(const MeshBrushSettings& settings, const field::MaskGate& gate);
    // Read and write the target channel's coefficients at one vertex, recording
    // the first `before` as it goes.
    LocalDetail read_target(std::uint32_t level, std::uint32_t vertex) const;
    bool write_target(std::uint32_t level, std::uint32_t vertex, const LocalDetail& value);

    std::size_t smooth_detail(const MeshBrushSettings& settings, const field::MaskGate& gate);
    std::size_t smooth_form(const MeshBrushSettings& settings, const field::MaskGate& gate);
    // The Laplacian shift for every vertex of the region, computed BEFORE any
    // of it is written. Separate from the write for a reason the implementation
    // spells out: at level 0 the array being read is the array a write moves,
    // so a fused loop would be a Gauss-Seidel sweep whose answer depends on the
    // order the region happens to sit in.
    void form_shift(const Adjacency& adjacency, const std::vector<kernel::cfloat3>& form,
                    float strength, std::vector<kernel::cfloat3>* shift) const;
    std::size_t fade_toward_zero(const MeshBrushSettings& settings, const field::MaskGate& gate,
                                 bool base);

    MultiresSurface& surface_;
    MultiresSculptor sculptor_;
    MultiresWriteDomain domain_ = MultiresWriteDomain::Automatic;
    SculptLayerId target_ = kNoSculptLayer;
    SculptLayerId restore_active_ = kNoSculptLayer;
    bool open_ = false;
    std::size_t stamps_ = 0;
    SculptLayerDelta layer_delta_;
    MultiresDelta base_delta_;
    DetailStampReport report_;
    // Reused across dabs rather than allocated per dab, which is the same
    // allocation rule the workset keeps.
    std::vector<Weighted> region_;
    std::vector<LocalDetail> scratch_;
};

}  // namespace mesh
}  // namespace clay
