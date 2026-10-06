// Mask extrude internals, for tests only. Not installed and not part of the
// library's API: hosts call brush::mask_extrude.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "clay/brush/mask_extrude.h"

namespace clay {
namespace brush {
namespace detail {

// What the field extrude's fill did, counted over every lattice sample it
// visited. The skip is exact, so a volume cannot show whether it fired; this
// can.
struct MaskExtrudeTally {
    std::size_t samples = 0;           // lattice samples visited
    std::size_t bricks_beyond_band = 0;  // bricks skipped whole: every shell past the band
    std::size_t bound_skips = 0;       // samples skipped because the shell beat the bound
    std::size_t bound_skips_in_band = 0;  // ...of which the volume stores (|shell| <= band)
    std::size_t projected = 0;         // samples that were projected onto the source
};

// The field extrude with the skip on (`cull`) or off. Off projects every
// sample: the reference the culled extrude must match bit for bit. `tally`,
// when given, receives the counts above.
std::optional<field::FieldVolume> mask_extrude_field(
    const std::function<float(kernel::cfloat3)>& source, const voxel::MaskField& mask,
    const MaskExtrudeSettings& settings, bool cull, MaskExtrudeTally* tally = nullptr);

}  // namespace detail
}  // namespace brush
}  // namespace clay
