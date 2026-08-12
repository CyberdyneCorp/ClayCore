#pragma once

// Sampled fields (sdf-kernels spec): a sparse narrow-band signed distance
// volume, built by sampling any callable and evaluated by the tape.
//
// The Phase 2 plan expected this to need a resource mechanism outside the tape,
// on the grounds that a 256³ volume in the blob would mean re-uploading 33 MB.
// That assumed DENSE storage. A signed distance field only needs samples near
// the surface, and a narrow band is O(n²): ~1.6 MB at 256³ and a tenth of that
// at the resolution an imported prop actually wants — the same order as the
// stroke chains the blob already carries. So a volume rides in the blob like
// every other out-of-line payload, and every backend gets it from the shared
// kernel dialect with no new plumbing.
//
// A brick either stores samples or does not exist. A brick that does not exist
// is recorded as wholly inside or wholly outside, which is what keeps the band
// sparse — and what lets the field stay correct far from the surface, where a
// band alone would have no idea which side it was on.

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "clay/math/geom.h"

namespace clay {
namespace field {

// Samples per brick edge. Bricks store one extra sample per axis — a halo — so
// trilinear interpolation inside a brick never needs its neighbour, which is
// what makes the lookup a single array read.
inline constexpr int kBrickDim = 8;
inline constexpr int kBrickSamples = (kBrickDim + 1) * (kBrickDim + 1) * (kBrickDim + 1);

// A brick index entry that is not an offset: the brick has no samples because
// the whole of it is on one side of the surface. What it reports instead is
// held per brick in a parallel array, signed by which side it is on.
inline constexpr std::int32_t kBrickEmpty = -1;

class FieldVolume {
  public:
    // Sample `f` over `region` at `cell_size`, keeping samples within `band`
    // of the surface. `band` should be at least a couple of cells: it is the
    // distance over which the field stays a real distance rather than a bound,
    // and a band thinner than the marcher's step is no use to it.
    static FieldVolume sample(const std::function<float(kernel::cfloat3)>& f,
                              const math::Aabb& region, float cell_size, float band);

    // The brick lattice a batched sample() walks. Exposed so an external
    // evaluator can compute the exact positions sample() hands its callable —
    // the same integers through the same float operations — which is what
    // lets a batched bake produce the bit-identical volume.
    struct BrickGrid {
        kernel::cfloat3 origin;
        float cell_size;
        float band;
        std::int32_t bcount[3];

        // Sample `i` (x-fastest over (kBrickDim+1)^3, halo included) of the
        // brick at linear `slot` (x-fastest over bcount).
        kernel::cfloat3 sample_position(std::size_t slot, int i) const;
        // The box the brick's samples span, halo face included.
        math::Aabb brick_box(std::size_t slot) const;
    };

    // Batched sampling: the same volume sample() builds, from sample blocks an
    // external evaluator fills. `fill` is called with consecutive slot windows
    // [first, first + count) and writes count * kBrickSamples values — sample
    // index x-fastest, exactly the values `f` would produce at
    // grid.sample_position. sample() routes through this with a serial fill;
    // a caller that can evaluate whole bricks at once (per-brick culled tapes
    // across a thread pool) supplies its own and gets the identical volume:
    // blocks land in slot order whatever order they were computed in, so the
    // result does not depend on the evaluator's scheduling.
    using BrickBlockFill = std::function<void(const BrickGrid& grid, std::size_t first,
                                              std::size_t count, float* out)>;
    static FieldVolume sample_blocks(const BrickBlockFill& fill, const math::Aabb& region,
                                     float cell_size, float band);

    float cell_size() const { return cell_size_; }

    // How fast the stored samples may vary. 1 for a volume sampled from a
    // distance field; more for one an operator has steepened, which any brush
    // confined to a region does. Read by the compiler to declare the field's
    // Lipschitz bound — see cfi_volume.
    float sample_lipschitz() const { return sample_lipschitz_; }
    void set_sample_lipschitz(float v) { sample_lipschitz_ = v > 1.0f ? v : 1.0f; }

    // How far inside the sampled box a Replace placement crossfades from the
    // surrounding field to this volume, in world units; 0 is the hard replace.
    // A property of the VOLUME rather than of the op, because it is decided
    // where the volume is baked — the bake knows what the box edge will meet —
    // and because it must survive the document round trip with the samples.
    // Read by the tape compiler; every other consumer ignores it.
    float feather() const { return feather_; }
    void set_feather(float v) { feather_ = v > 0.0f ? v : 0.0f; }

    float band() const { return band_; }
    kernel::cfloat3 origin() const { return origin_; }
    math::Aabb bounds() const;
    std::size_t brick_count() const { return data_.size() / kBrickSamples; }
    std::size_t sample_count() const { return data_.size(); }
    bool empty() const { return index_.empty(); }

    // The field at a world position.
    //
    // Where the volume HAS samples this is trilinear interpolation of them.
    // Interpolating a convex field overshoots — trilinear interpolation lies
    // above the function it samples by O(cell^2 x curvature) — so the value is
    // accurate to the sampling but is NOT a lower bound there.
    //
    // Where it has NO samples the value is a genuine lower bound, so sphere
    // tracing cannot overstep across the empty majority of the region.
    //
    // The two regions do not meet continuously. Adjacent STORED bricks do —
    // that is what the halo sample buys — but a stored brick against a skipped
    // one jumps, from a flat bound up to a real distance. Marching is safe
    // across it because each side is individually a lower bound or an accurate
    // distance; gradients across it are not meaningful, and nothing takes them
    // there, since the seam sits a band away from the surface while normals
    // are taken at it. Removing the jump would mean storing the bricks the
    // sparsity exists to avoid.
    float eval(kernel::cfloat3 p) const;

    // Whether `p` lands in a brick that stores samples. The two halves of
    // eval()'s contract are not the same thing as "within the band": a brick
    // spans kBrickDim cells and is kept whole, so a stored brick holds samples
    // well beyond the band.
    bool has_samples_at(kernel::cfloat3 p) const;

    // The FLOOR on what a sample-free brick reports — what it says when it is
    // right next to a brick that has samples. Not the band itself: a brick is
    // skipped when every SAMPLE in it is beyond the band, and a point between
    // samples can be up to half a cell diagonal nearer the surface than the
    // nearest sample is. Reporting the band flat would overestimate by that
    // much, which is the overstep the sparse index exists to stay clear of.
    //
    // Bricks further out report MORE, and must: a marcher told a flat band
    // width everywhere in the empty majority of the region crawls across it in
    // steps that never grow, and runs out of iterations before it arrives.
    // Each empty brick carries its Chebyshev distance in bricks to the nearest
    // brick that has samples, less one brick for the gap between the boxes —
    // a genuine lower bound, because the surface only ever lies inside a brick
    // that has samples.
    float far_value() const;

    // -- rewriting the samples in place ------------------------------------
    //
    // For an operator that transforms a field into another field over the same
    // lattice. Going through eval() instead would be a trap: the value where
    // there are no samples is a flat BOUND, not a distance, and re-sampling a
    // field that mixes the two bakes the boundary between them into adjacent
    // samples one cell apart — turning a brick-face artifact into a genuinely
    // steep interpolant. Working on the stored samples never sees it.

    // The stored sample at global cell coordinates, or nothing where the brick
    // holding it has none. A sample on a brick face lives in both neighbours;
    // either copy answers, and they agree.
    std::optional<float> sample_at(int gx, int gy, int gz) const;

    // Replace every stored sample with `fn(gx, gy, gz, old)`, keeping the
    // brick structure. Halo duplicates cannot drift apart along a shared face
    // because `fn` is a function of the GLOBAL coordinate, so both copies of a
    // shared sample are handed the same question. The old value comes along so
    // that "leave this one alone" needs no lookup and cannot be spelt as zero.
    void rewrite(const std::function<float(int, int, int, float)>& fn);

    // Drop the bricks whose samples all lie beyond the band, and re-derive
    // what the resulting sample-free bricks report. Returns how many went.
    //
    // ONLY SOUND ON A FIELD WHOSE SAMPLES ARE A DISTANCE, and the caller owes
    // that: the whole sparse index rests on "the surface only ever lies inside
    // a brick that has samples", and a brick of a STEEP field can hold a zero
    // crossing between two samples that are both past the band. After
    // redistance() the field varies by about a cell per cell and a band of two
    // cells or more cannot hide a crossing, so the claim holds again.
    //
    // What it is for: a re-bake keeps the brick just outside the stored ones,
    // because a sample-free brick beside a stored one reports the band's floor
    // and that is within the band. Baking a volume repeatedly — which is what
    // consolidating a chain does — would otherwise grow the stored shell by one
    // brick every time, and the samples in that shell say nothing the far
    // bounds did not.
    std::size_t compact();

    // Narrow the band, and with it what a sample-free brick may claim. An
    // operator that MOVES the surface must do this by however far it moved:
    // the empty bricks were classified against the old surface, and their
    // bounds would otherwise overstate the distance to the new one.
    void shrink_band(float by);

    // World position of a global cell coordinate.
    kernel::cfloat3 cell_position(int gx, int gy, int gz) const {
        return origin_ + kernel::cf3(static_cast<float>(gx), static_cast<float>(gy),
                                     static_cast<float>(gz)) *
                             cell_size_;
    }

    // Samples along each axis: bricks times their edge, plus the halo.
    int sample_extent(int axis) const { return bcount_[axis] * kBrickDim + 1; }

    // The steepest slope between neighbouring STORED samples, over the cell
    // size. Measured, not assumed: an operator that builds a volume from a
    // blend can read back what it actually produced instead of bounding it in
    // advance and hoping the bound was generous enough.
    float measure_sample_lipschitz() const;

    // Flat float layout for the tape's blob. The kernel reads exactly this.
    std::vector<float> to_blob() const;
    // How long that blob is, without building it. What a volume COSTS is a
    // question a host asks while deciding, and often repeatedly; materialising
    // a copy of every sample to measure one integer is the wrong way to answer
    // it.
    std::size_t blob_floats() const;
    static std::optional<FieldVolume> from_blob(const std::vector<float>& blob);

    std::vector<std::uint8_t> serialize() const;
    static std::optional<FieldVolume> deserialize(const std::uint8_t* data, std::size_t size);

  private:
    void build_far_bounds();
    float eval_inside(kernel::cfloat3 p) const;

    kernel::cfloat3 origin_ = kernel::cf3(0, 0, 0);
    float cell_size_ = 0.05f;
    float band_ = 0.2f;
    float sample_lipschitz_ = 1.0f;
    float feather_ = 0.0f;
    std::int32_t bcount_[3] = {0, 0, 0}; // bricks per axis
    std::vector<std::int32_t> index_;    // bcount product; offset into data_, or kBrickEmpty
    std::vector<float> far_;             // per brick; signed lower bound where index_ is empty
    std::vector<float> data_;            // kBrickSamples per stored brick
};

}  // namespace field
}  // namespace clay
