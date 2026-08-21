#include "clay/brush/gate_bake.h"

#include <optional>

#include "clay/brush/mask_extrude.h"

namespace clay {
namespace brush {

std::shared_ptr<const field::FieldVolume> GateBake::gate_for(const voxel::MaskField& mask,
                                                            float threshold, float width) {
    const std::uint64_t revision = mask.revision();
    if (filled_ && revision == revision_ && threshold == threshold_ && width == width_) {
        last_was_cached_ = true;
        return baked_;
    }
    last_was_cached_ = false;

    // The measured band has to reach at least as far as the gate fades, or full
    // protection is never reachable: the distance saturates at the band and the
    // smoothstep never gets to 1. Twice the width leaves margin, and the pad
    // matches so the sampled region actually contains it.
    const float band = 2.0f * width;
    std::optional<field::FieldVolume> measured = mask_to_field(mask, threshold, band, band);

    // A miss is memoised too, so a host that repeatedly gates by an empty mask
    // pays for the failed measurement once rather than every call. The stored
    // null is what a caller turns into its own error.
    baked_ = (measured && !measured->empty())
                 ? std::make_shared<const field::FieldVolume>(std::move(*measured))
                 : nullptr;
    revision_ = revision;
    threshold_ = threshold;
    width_ = width;
    filled_ = true;
    return baked_;
}

}  // namespace brush
}  // namespace clay
