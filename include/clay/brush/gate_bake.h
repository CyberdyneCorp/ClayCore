#pragma once

// Memoised gate bakes (add-masking-that-gates-any-op).
//
// Gating an item by a painted mask MEASURES the mask — `brush::mask_to_field`
// builds a signed distance to { mask >= threshold } — and that measurement is
// not cheap. Timed through the Python binding on this machine:
//
//     painted cells   one gate() call
//         3 912          21 ms
//        30 976         145 ms
//
// paid on EVERY gate call. Gating fifty items by one painted mask is a second
// at the small size and seven at the larger one, for fifty identical bakes.
//
// So a consumer holds one of these beside its mask handle and asks it for the
// gate instead of calling `mask_to_field` directly. Repeat asks with the same
// mask and the same parameters return the same shared field, which saves the
// memory as well as the time: fifty gated items then reference one volume.

#include <cstdint>
#include <memory>

#include "clay/field/volume.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace brush {

class GateBake {
  public:
    // The gate field for `mask` at these parameters, baked only when the memo
    // does not already hold it. Null when the mask describes no region — an
    // empty mask, or nothing reaching the threshold — which callers must
    // report rather than treat as a gate that protects nothing.
    //
    // `width` is the gate's fade distance, and the measured band is derived
    // from it HERE rather than at each call site. The rule (twice the width,
    // with a matching pad) has to hold for the gate to reach full protection
    // at all, and it was previously spelled out identically in two bindings —
    // two copies of an arithmetic invariant that must not drift.
    std::shared_ptr<const field::FieldVolume> gate_for(const voxel::MaskField& mask,
                                                       float threshold, float width);

    // Whether the last gate_for was answered from the memo. For tests and for
    // a host that wants to know what it is paying; nothing depends on it.
    bool last_was_cached() const { return last_was_cached_; }

  private:
    // The mask's own change token. Compared for equality only, and only
    // against the mask this memo was filled from — which is why a memo lives
    // beside ONE mask handle rather than in a table keyed by address. An
    // address is reused after a free; a handle's memo dies with the handle.
    std::uint64_t revision_ = 0;
    float threshold_ = 0.0f;
    float width_ = 0.0f;
    bool filled_ = false;
    bool last_was_cached_ = false;
    std::shared_ptr<const field::FieldVolume> baked_;
};

}  // namespace brush
}  // namespace clay
