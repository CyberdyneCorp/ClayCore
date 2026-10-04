#pragma once

// ONE MULTIRESOLUTION GESTURE AS A HOST HOLDS IT (undo-a-multires-gesture-
// across-the-abi, issue #671): the two records a gesture can write, and the
// hierarchy they were captured on.
//
// WHY A THIRD TYPE AND NOT ONE OF THE TWO. `MultiresDelta` restores the base
// (the cage and each level's own detail) and `SculptLayerDelta` restores one
// sculpt pass; `session::History` keeps them as two step kinds because the
// document reverses them against different owners. A host undo stack has one
// slot per gesture and no owner to dispatch on, and the plain sculptor writes
// whichever of the two the stack's active layer selects -- so a host recording
// "this stroke" cannot know in advance which record it needs. This holds both,
// and a half that captured nothing costs nothing.
//
// WHAT IT ADDS IS THE BINDING. Neither record knows which hierarchy it came
// from: their own `revert` refuses only a surface whose COUNTS cannot hold the
// entries, so a twin built from the same cage -- or this hierarchy after its top
// level was removed and added back -- accepts a record that describes a
// different surface. The gesture remembers the `structure_revision` it was
// captured at, which is drawn from one process-wide counter and moves on every
// renumbering, and replays only onto a hierarchy still carrying that value.
//
// THE TRADE, STATED. A revision is a value, not a fingerprint of the content:
// a hierarchy decoded from its own bytes, or relevelled and relevelled back,
// is refused even where every vertex is numbered as it was. Refusing a
// correct replay costs a host an undo step it then takes from a snapshot;
// accepting a wrong one writes coefficients into the wrong vertices silently.
// The second is the one this cannot afford.
//
// THE PROCESS IS PART OF THE BINDING, because the counter is not: it restarts
// at the same value in every process, so a record spilled by one process would
// otherwise match an unrelated hierarchy in the next. Each process draws one
// random origin, the bytes carry it, and a record from another process
// decodes, reports its statistics, and replays onto nothing.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt_layer.h"
#include "clay/mesh/topology_delta.h"  // GestureDecode

namespace clay {
namespace mesh {

class MultiresGesture {
   public:
    // The two halves, for the capture. A caller writing entries here must call
    // `bind` afterwards, or the gesture replays onto nothing.
    MultiresDelta& base() { return base_; }
    SculptLayerDelta& layer() { return layer_; }
    const MultiresDelta& base() const { return base_; }
    const SculptLayerDelta& layer() const { return layer_; }

    bool empty() const { return base_.empty() && layer_.empty(); }
    // Empties both halves and the binding; keeps neither half's capacity
    // promise beyond what their own `clear` keeps.
    void clear();

    // Whether more of a gesture may be captured into this one on `surface` as
    // it is now: an empty gesture always, a bound one only on the hierarchy it
    // is bound to and -- when it already holds a layer half -- not while a
    // DIFFERENT pass is active, because that pass's coefficients under the
    // first one's id would replay into the wrong pass. A base write joins
    // either half: the two are separate storage and replay independently.
    bool accepts(const MultiresSurface& surface) const;
    // Bind to `surface` if anything was captured. Idempotent.
    void bind(const MultiresSurface& surface);

    // Whether a replay onto `surface` would be accepted: an empty gesture
    // anywhere, a bound one onto its own hierarchy while both halves still fit.
    // The replays below make this check first and write nothing on a refusal.
    bool replayable(const MultiresSurface& surface) const;
    // Put the hierarchy back as the gesture found it / left it. Idempotent; an
    // empty gesture is a no-op that succeeds. A replay that wrote a pass
    // evaluates the display level before returning, so the patches it moved
    // are in `dirty_patches` as they are after a stamp.
    bool revert(MultiresSurface& surface) const;
    bool apply(MultiresSurface& surface) const;

    // The levels either half touched, ascending, without repeats.
    std::vector<std::uint32_t> levels() const;

    // EXACT: the length `encode` writes, a fixed function of the four entry
    // counts -- 40, plus 16 + 32 d + 28 c for a non-empty base half, plus
    // 24 + 32 ld + 16 lm for a non-empty layer half.
    std::size_t encoded_size() const;
    // What the gesture holds in memory, capacities and slot maps included. The
    // number to budget against, not the one to assert.
    std::size_t bytes() const;

    std::vector<std::uint8_t> encode() const;
    static GestureDecode decode(const std::uint8_t* data, std::size_t size,
                                MultiresGesture* out);

   private:
    bool bound_to(const MultiresSurface& surface) const;
    bool replay(MultiresSurface& surface, bool forward) const;

    MultiresDelta base_;
    SculptLayerDelta layer_;
    // The process that captured it and the hierarchy's structure revision at
    // the capture. Both zero while nothing has been captured.
    std::uint64_t origin_ = 0;
    std::uint64_t structure_ = 0;
};

}  // namespace mesh
}  // namespace clay
