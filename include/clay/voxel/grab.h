#pragma once

// A voxel grab as a GESTURE rather than as a call (voxel-engine spec,
// add-voxel-grab-transaction; issue #393).
//
// WHY `sculpt_grab` ALONE IS NOT ENOUGH. A grab of N cells is not N grabs of
// one cell, and the difference is not small. Each call reads the grid, resamples
// occupancy through the falloff, and writes the result back — so the next call
// reads its own output. Two things go wrong at once:
//
//   * the displacement is weighted by the footprint's falloff and then rounded
//     to whole cells, so at one cell of displacement only the very middle of
//     the region rounds to a cell and everything else rounds to zero. Inside
//     solid material, moving only the middle changes no occupancy at all.
//     Split finely enough, the whole drag evaporates;
//   * a translation is supposed to conserve occupancy, and composed calls do
//     not. Material is copied rather than moved, so repeated grabs smear and
//     duplicate instead of translating.
//
// Measured on a solid ball 16 cells across at cell 0.04, smooth falloff,
// strength 1, front-gated, the same total drag of 8 cells in +y split into
// 1, 2, 4 and 8 emissions. "New" counts occupied cells whose coordinate was not
// occupied before; occupancy at rest is 2109:
//
//     footprint    1 x 8      2 x 4      4 x 2      8 x 1
//     24 cells    59/2157    61/2170     0/2109     0/2109
//     32 cells   205/2200   169/2215   190/2298     0/2109
//     40 cells   357/2205   376/2326   293/2371   126/2235
//
// The same gesture, the same total displacement, every emission a whole number
// of cells — and at the footprints a brush that size actually uses, delivering
// it the way a pointer delivers it moves nothing.
//
// That is not a rounding bug to be fixed inside `sculpt_grab`: a stateless call
// has no gesture to be idempotent over. What it needs is somewhere to keep the
// material as it was when the drag began, which is what this is.
//
// WHAT THIS IS. `begin` captures the region the gesture can reach. Every
// `update` takes the TOTAL displacement from the anchor — never an increment on
// the last frame — and resamples from that capture, so update(1), update(2),
// update(8) ends at exactly what a single update(8) produces, and so does
// update(8) alone. Idempotent by construction, which is what makes a live
// preview possible: the same shape `SdfMoveTransaction` has on the field side,
// and for the same reason.
//
// `cancel` puts the capture back. `commit` keeps what the last update wrote —
// the grid already holds it, so committing costs nothing.
//
// THE CAPTURE GROWS WITH THE DRAG. A drag does not say at the start how far it
// will go, so the ring outside the footprint is captured lazily as the
// displacement reaches for it. That is safe because only the FOOTPRINT is ever
// written: a cell outside it is still pristine whenever the capture widens to
// include it. Asking a host to declare a maximum reach up front would be one
// more number to get wrong, and getting it wrong would read back the gesture's
// own output.
//
// WHAT IT DOES NOT FIX. Occupancy is binary and the resample is nearest-cell,
// so a total displacement under half a cell on every axis still moves nothing —
// there is no sub-cell state for it to move. What changes is that the drag is
// measured from the ANCHOR rather than from the last frame, so a slow drag
// accumulates toward that half cell instead of rounding to zero over and over.

#include <cstdint>
#include <optional>
#include <vector>

#include "clay/voxel/grid.h"

namespace clay {
namespace voxel {

class GrabTransaction {
  public:
    // Null for a non-positive brush size, which is not a footprint. A gesture
    // that reaches only empty space is a valid transaction that changes
    // nothing — the artist pressed on nothing, which is not an error.
    static std::optional<GrabTransaction> begin(VoxelGrid& grid, VoxelCoord anchor,
                                                const BrushParams& brush,
                                                bool front_only = false);

    GrabTransaction(GrabTransaction&&) = default;
    GrabTransaction& operator=(GrabTransaction&&) = default;
    GrabTransaction(const GrabTransaction&) = delete;
    GrabTransaction& operator=(const GrabTransaction&) = delete;
    ~GrabTransaction() = default;

    // The drag so far, measured from the ANCHOR — the total, never an increment
    // on the last frame. Resampled from the capture, so a run of updates ends
    // where a single update to the same total would, and repeating one changes
    // nothing.
    void update(kernel::cfloat3 total_displacement);

    // Keep what the last update wrote. The grid already holds it.
    void commit();
    // Put the captured material back, exactly as it was at `begin`.
    void cancel();

    bool live() const { return grid_ != nullptr; }
    VoxelCoord anchor() const { return anchor_; }
    // The box the gesture writes: the brush's footprint, fixed for the gesture
    // whatever the displacement grows to, because a grab only ever writes
    // inside its own footprint.
    VoxelCoord written_lo() const { return written_lo_; }
    VoxelCoord written_hi() const { return written_hi_; }
    // How far the capture currently reaches past the footprint, in cells. Grows
    // with the drag; a test that asserts a gesture does not re-read the grid
    // has this to watch.
    int captured_pad() const { return pad_; }

  private:
    GrabTransaction() = default;

    // Widen the capture to `pad` cells past the footprint, reading only what it
    // does not already hold.
    void capture(int pad);

    VoxelGrid* grid_ = nullptr;
    VoxelCoord anchor_{};
    BrushParams brush_{};
    bool front_only_ = false;
    VoxelCoord written_lo_{}, written_hi_{};
    // The material as it was at `begin`, over the footprint and the pad the
    // drag has reached. Every update reads THIS and never the grid, which is
    // the whole mechanism.
    VoxelCoord source_lo_{}, source_hi_{};
    std::vector<std::uint8_t> source_;
    int pad_ = 0;
};

}  // namespace voxel
}  // namespace clay
