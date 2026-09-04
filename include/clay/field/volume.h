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
#include "clay/parallel/cancel.h"

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
    // `token` and `out_cancelled` behave exactly as on sample_blocks below,
    // which this routes through — so every verb that samples a field is
    // cancellable at the same window boundary, without each one growing a
    // checkpoint of its own.
    static FieldVolume sample(const std::function<float(kernel::cfloat3)>& f,
                              const math::Aabb& region, float cell_size, float band,
                              parallel::CancelToken* token = nullptr,
                              bool* out_cancelled = nullptr);

    // The same, on the thread pool — for an `f` that is SAFE TO CALL
    // CONCURRENTLY.
    //
    // That contract is the whole difference, and it is why this is a separate
    // entry point rather than a change to the one above. `sample`'s `f` may
    // hold state: `brush::mask_extrude` passes one that accumulates a running
    // minimum across the samples, and quietly threading it would have raced.
    // Silently changing a public function's threading contract is not a thing
    // to do to a consumer.
    //
    // Byte-identical to `sample` by construction, not by tolerance: samples are
    // written to disjoint output slices and each value depends only on its own
    // position, so no ordering can change any of them.
    //
    // Worth it where `f` is expensive. A mesh import is the case that motivated
    // it: a BVH signed-distance query with a generalized winding number per
    // sample, which was 4.8 seconds for a 9k-triangle model at a 0.01 cell.
    static FieldVolume sample_parallel(const std::function<float(kernel::cfloat3)>& f,
                                       const math::Aabb& region, float cell_size, float band);

    // The same, with a colour per sample. A volume built without one carries
    // NO colour array and costs exactly what it costs today — which is what
    // makes this free everywhere it is not used, and there are more of those
    // than not: every mesh import and every field bake has one colour or none.
    //
    // `c` is called at the same positions as `f`, and only for the samples
    // that are kept. Colour is stored packed to 8 bits a channel, which is
    // more than either producer resolves — a 256-entry palette, or a float
    // colour field a display quantises anyway — and costs one word per sample
    // where three floats would cost three.
    static FieldVolume sample_colored(const std::function<float(kernel::cfloat3)>& f,
                                      const std::function<kernel::cfloat3(kernel::cfloat3)>& c,
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
        // The GLOBAL cell coordinate of the same sample -- the integers
        // sample_position turns into a world point. A fill that wants to ask
        // the volume what it already stores there needs them, and deriving
        // them from the world position would be inverting arithmetic that is
        // right here.
        void sample_cell(std::size_t slot, int i, int out[3]) const;

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
    //
    // `token` (optional) makes the sampling cancellable. The checkpoint is the
    // window boundary the loop already has — 512 bricks — so the check costs
    // one relaxed load per window rather than one per sample, and the brick
    // count is an honest progress denominator. A cancelled call returns early
    // and sets `out_cancelled`, which is how a caller tells "the user stopped
    // it" from "there was no surface here": both leave a volume with no bricks.
    static FieldVolume sample_blocks(const BrickBlockFill& fill, const math::Aabb& region,
                                     float cell_size, float band,
                                     parallel::CancelToken* token = nullptr,
                                     bool* out_cancelled = nullptr);

    // A volume with the LATTICE of `region` at `cell_size` and no stored
    // samples at all: the index, the brick counts and the far bounds, and an
    // empty sample store.
    //
    // What a lazily filled working field starts from. Cheap by construction —
    // one index entry and one far bound per brick, and no evaluation of
    // anything — which is what lets a gesture allocate its working set at
    // pointer-down without sampling the model. Every brick reads as sample-free
    // until `materialize_region` fills it.
    //
    // Sharing a `region` and a `cell_size` with another volume means sharing a
    // lattice, because the origin is `region.min`; that is the property a
    // caller overlaying one volume onto another depends on.
    static FieldVolume empty_lattice(const math::Aabb& region, float cell_size, float band);

    // What this volume holds. A sampled volume is the largest payload a scene
    // node can carry — bricks of floats, plus an optional color channel — and
    // it went entirely unaccounted until roll-up-document-memory, including in
    // the undo history, which holds nodes by value.
    //
    // A volume is SHARED (nodes hold a shared_ptr), so a caller adding these up
    // across a document must count each one ONCE by address; this method
    // reports one volume and knows nothing about how many nodes point at it.
    std::size_t bytes() const;

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

    // Whether this volume carries colour of its own. False for every volume
    // built before this existed and for every one built without it, and then
    // the item's own colour is what evaluation reports, exactly as before.
    bool has_color() const { return !colors_.empty(); }

    // The colour at a world position, interpolated between the same eight
    // samples the distance is. Reading the nearest sample instead would put a
    // facet on a surface that has none.
    //
    // Meaningless where has_color() is false, and where `p` is outside the
    // sampled box: both are the caller's cue to use the item's colour, which
    // is what the tape does.
    kernel::cfloat3 eval_color(kernel::cfloat3 p) const;

    // Fill (or replace) the colour of every STORED sample, from a callable
    // asked at each sample's world position. For a producer that built its
    // volume through sample_blocks — the batched path a thread pool fills —
    // this is how colour is added afterwards without a second sampling
    // contract. Does nothing to a volume with no stored samples.
    void fill_colors(const std::function<kernel::cfloat3(kernel::cfloat3)>& c);

    // The same, in windows, so a producer with a thread pool fills colour the
    // way it fills distance. `fill` receives count packed xyz positions and
    // writes count*3 floats. A serial producer wants fill_colors above; this
    // exists because a bake that parallelised its distances and not its
    // colours stops being faster than the serial bake it replaced.
    using ColorBlockFill =
        std::function<void(const float* points_xyz, std::size_t count, float* out_rgb)>;
    void fill_colors_blocks(const ColorBlockFill& fill);

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

    // What part of the field an operator acts over.
    //
    // A box, because that is what selects bricks. Optionally narrowed to a BALL
    // inside it, because that is what a brush actually is, and a box around a
    // ball holds nearly twice its volume: measured on a relax dab, between 51%
    // and 73% of the bricks a box selected could not hold a sample the brush
    // could reach, and every sample in them was visited and weighed before
    // being handed back unchanged.
    //
    // Constructible from an Aabb, so an operator whose region really is a box
    // says nothing extra.
    struct Region {
        math::Aabb box;
        kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
        float radius = -1.0f;  // negative: the box itself, no narrowing

        Region(const math::Aabb& b) : box(b) {}  // NOLINT(*-explicit-constructor)
        static Region ball(kernel::cfloat3 c, float r) {
            Region g{math::Aabb{c - kernel::cf3(r, r, r), c + kernel::cf3(r, r, r)}};
            g.centre = c;
            g.radius = r;
            return g;
        }
        // Whether a brick's box can hold anything the region reaches. The
        // nearest point of the brick to the centre is three clamps, which is
        // cheaper than one sample's worth of the work it saves.
        bool meets(const math::Aabb& brick) const;
    };

    // The same, over the bricks that MEET `region` and no others.
    //
    // For a brush. A verb confined to a region still had to walk every stored
    // sample in the volume to find out that most of them were not in it, which
    // made a dab cost what the model cost rather than what the dab touched:
    // at a 0.01 cell a five-cell brush paid 16.7 ms, of which about 0.6 ms was
    // work inside the brush.
    //
    // `fn` MUST BE THE IDENTITY OUTSIDE `region`. That is not a hint about
    // what the caller probably wants, it is what makes this equal to
    // `rewrite`, and it is load-bearing twice over:
    //
    //   - the samples in bricks that are skipped keep their old values, which
    //     is only the same answer if `fn` would have returned them anyway;
    //   - a sample on a brick face lives in EVERY brick sharing it, and this
    //     writes only the copies held by selected bricks. If `fn` changed such
    //     a sample where one sharer was selected and another was not, the two
    //     copies would drift apart and the field would step at the brick face.
    //     It cannot: a brick that was not selected does not meet `region`, so
    //     every sample it holds lies outside `region`, where `fn` is identity.
    //
    // The selection is conservative — whole bricks, rounded outward — so
    // passing a slightly generous region costs a little work and no
    // correctness. Passing one that is too SMALL is a defect this cannot
    // detect.
    void rewrite_region(const Region& region,
                        const std::function<float(int, int, int, float)>& fn);

    // What a region rewrite actually wrote: the bricks it selected, the world
    // box they span, and whether any stored sample moved.
    //
    // A host holding a preview of this volume cannot SEE a rewrite. The volume
    // keeps its identity, its brick set and its bounds, so nothing a consumer
    // can diff from outside says which part of it went stale — and the only
    // safe answer without this is "all of it", which is the term scaling with
    // the model that a local dab exists to remove.
    //
    // The count and the bounds are GEOMETRIC: they describe the bricks the
    // region selected, not the samples whose values happened to move. That is
    // deliberate, and it is what makes them a TEST's quantity as well as a
    // host's — the same brush over the same lattice selects the same bricks
    // however much unrelated model is added around it, so a scaling test can
    // assert on a number that does not depend on how fast the machine is.
    //
    // `changed` is the value question, kept separate because it is a different
    // question: a dab whose weight came out zero everywhere still selects its
    // bricks, and a host wants to know it has nothing to redraw.
    struct RewriteTally {
        math::Aabb bounds;               // union of the selected bricks' boxes
        std::size_t touched_bricks = 0;  // bricks the rewrite wrote
        bool changed = false;            // any stored sample actually moved
    };

    // A brick's position on the lattice, in bricks. What a consumer holding a
    // copy of this volume needs in order to say WHICH part of it went stale —
    // a bounding box says where to look, and this says what to fetch.
    struct BrickCoord {
        int x = 0, y = 0, z = 0;
    };

    // The world position of a brick's first sample, and the extent of its
    // samples. A host patching a preview needs to place what it is given.
    kernel::cfloat3 brick_origin(BrickCoord c) const {
        return origin_ + kernel::cf3(static_cast<float>(c.x), static_cast<float>(c.y),
                                     static_cast<float>(c.z)) *
                             (static_cast<float>(kBrickDim) * cell_size_);
    }

    // One brick's stored samples, x-fastest over (kBrickDim+1)^3 — the same
    // order `sample_blocks` fills and `to_blob` writes. False when that brick
    // stores none, and then `out` is untouched.
    bool read_brick(BrickCoord c, float* out) const;

    // The same rewrite, reporting what it touched. `rewrite_region` is this
    // with the report dropped, so there is one walk and not two.
    //
    // `out_changed` (optional) is APPENDED with the coordinate of every brick
    // in which a stored sample actually moved — not every brick selected, which
    // is what `touched_bricks` counts. A consumer transporting a delta wants the
    // bricks whose bytes are new; a caller sizing work wants the count. The
    // vector is the CALLER'S, so a gesture reuses one across dabs rather than
    // allocating per dab, and duplicates across calls are the caller's to fold.
    RewriteTally rewrite_region_tallied(const Region& region,
                                        const std::function<float(int, int, int, float)>& fn,
                                        std::vector<BrickCoord>* out_changed = nullptr);

    // The samples `rewrite_region` is about to overwrite, kept so an operator
    // can read what was there while it writes what comes next.
    //
    // A stencil needs the pass's INPUT, not its half-written output, and the
    // obvious way to get that is to copy the volume. That copy is the whole
    // volume — six megabytes at an interactive cell — for a brush that will
    // touch a few hundred kilobytes of it, and it put a term that scales with
    // the MODEL back into a dab that had just been made to scale with itself.
    //
    // This copies only the bricks `rewrite_region` would select for the same
    // region. Reads outside them are served from the volume itself, which is
    // correct for exactly the reason the region limit is: a brick that does not
    // meet the region is never written, so what it holds during the rewrite is
    // what it held before it.
    //
    // Which makes the ORDER a requirement rather than a convention: take the
    // snapshot, then rewrite the SAME region. A snapshot of one region used
    // while another is rewritten reads half-written bricks and is silently
    // wrong.
    class RegionSnapshot {
      public:
        // The value at a global cell coordinate as it was when the snapshot was
        // taken. Nothing where no brick stores it, exactly as `sample_at`.
        std::optional<float> sample_at(int gx, int gy, int gz) const;

      private:
        friend class FieldVolume;
        const FieldVolume* volume_ = nullptr;
        int lo_[3] = {0, 0, 0};
        int span_[3] = {0, 0, 0};
        std::vector<std::int32_t> offset_;  // per brick in the box, or kBrickEmpty
        std::vector<float> data_;
    };

    RegionSnapshot snapshot_region(const Region& region) const;

    // -- resampling a region ------------------------------------------------
    //
    // What `resample_region` did, for a caller that wants to know without
    // diffing two volumes -- and for a scaling TEST, which needs a number that
    // does not depend on how fast the machine is: `evaluated` is what must stay
    // put when unrelated model is added around a brush.
    struct ResampleTally {
        std::size_t evaluated = 0;  // bricks the fill was asked for
        std::size_t kept = 0;       // stored before and after
        std::size_t added = 0;      // stored nothing before, stores samples now
        std::size_t removed = 0;    // stored samples before, stores none now
    };

    // The bricks that MEET `region`, re-evaluated from `fill` and RECLASSIFIED
    // from what it produced. Every other brick keeps its bytes.
    //
    // The primitive `rewrite_region` could not be. That one preserves which
    // bricks store samples, which is what makes it right for an operator that
    // filters an already-sampled surface: relax moves the surface by less than
    // a cell, so it never leaves the band. An operator that DISPLACES the
    // surface does leave it -- flatten moves it by many band widths -- and the
    // facet then lands in bricks that hold nothing, which a rewrite cannot
    // create. So:
    //
    //     rewrite_region()    values change, sparse support fixed
    //     resample_region()   values change, sparse support re-decided
    //
    // Inside the region a brick may keep storing samples with new values, start
    // storing them, or stop. The decision is `sample_blocks`' -- the same
    // scan, over the values `fill` produced -- so there is one definition of
    // near-the-surface and not two. That the scan runs AFTER the fill is the
    // point: a brick that looks irrelevant against the source's surface is
    // exactly where a displaced surface lands, so nothing may cull between
    // them.
    //
    // `fill` IS THE IDENTITY OUTSIDE `region`, which for a fill means it
    // reproduces the samples the volume already stores there. That is the same
    // precondition `rewrite_region` states and it is load-bearing the same two
    // ways: skipped bricks keep their old values, which is only the same answer
    // if `fill` would have produced them; and a sample on a brick face is held
    // by every brick sharing it, so a fill that changed such a sample where one
    // sharer was selected and another was not would leave the copies
    // disagreeing and the field stepping at the face. A caller reading a volume
    // gets this exactly, not nearly, by preferring `sample_at` to `eval` --
    // see field::flatten.
    //
    // The region needs NO margin for how far the operator moves the surface.
    // The field outside it is unchanged, so the zero set outside it is
    // unchanged, and whatever surface the operator creates lies inside the
    // region that created it.
    //
    // `fill` is called once per selected brick -- a window of one, since a
    // region selects a scattered set rather than the consecutive run
    // `sample_blocks` walks.
    ResampleTally resample_region(const Region& region, const BrickBlockFill& fill);

    // -- materializing a region into a volume that does not hold it yet -------
    //
    // FORCE every brick meeting `region` to store samples from `fill`, whether
    // or not the values look near the surface. Appends to the sample store, so
    // the cost is the bricks it adds and not the ones the volume already holds.
    //
    // This is `resample_region`'s job done for a different owner, and the two
    // differ in exactly the way their owners do:
    //
    //   resample_region()   re-DECIDES sparsity from the values, and rebuilds
    //                       the whole store to do it. Right for an operator
    //                       that displaces a surface into bricks that held
    //                       nothing; O(stored) per call, which is a bake's
    //                       scale.
    //   materialize_region() takes sparsity as GIVEN by the caller: a selected
    //                       brick stores samples afterwards, full stop. O(the
    //                       bricks it adds), which is a dab's scale.
    //
    // Why "force". A lazily filled working field needs to tell "this brick
    // holds no surface" apart from "nobody has asked yet", and `kBrickEmpty`
    // already means the first — it carries a SIGN and a distance that a reader
    // is entitled to believe. Storing every materialized brick, even one whose
    // samples are all past the band, keeps stored-ness an honest record of what
    // has been filled in. It costs the samples of a brick that says nothing
    // interesting, which is the price of not overloading a sentinel that
    // already has a meaning.
    //
    // The far bounds are NOT re-derived: they describe the distance from a
    // sample-free brick to the nearest stored one, and rebuilding that array is
    // a two-pass chamfer over every slot in the lattice — the term `shrink_band`
    // was changed to stop paying per dab. A caller materializing a region is by
    // definition going to read inside it, where the stored samples answer.
    //
    // `fill` is asked for one brick at a time, exactly as `resample_region`
    // asks, and must produce the same values a full sampling would at those
    // lattice points.
    // `out_added` (optional) is APPENDED with the coordinate of every brick
    // this materialized. Those bricks are new bytes to anyone holding a copy,
    // exactly as a rewritten one is.
    ResampleTally materialize_region(const Region& region, const BrickBlockFill& fill,
                                     std::vector<BrickCoord>* out_added = nullptr);

    // Whether the brick holding `p` stores samples. The lazily-filled caller's
    // question — see materialize_region — and cheaper than sample_at when the
    // answer is all that is wanted.
    bool brick_stored_at(kernel::cfloat3 p) const { return has_samples_at(p); }

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
    // The sub-volume covering `region` (in this volume's OWN coordinates), or
    // nullopt when there is nothing to gain — the region reaches every brick,
    // or reaches none.
    //
    // WHAT IT IS FOR. A per-brick culled tape carries every item the brick's
    // region touches, and for a sampled volume "the item" was its entire
    // sample payload: compiling a tape for one 8^3 brick copied 1,243,861
    // floats to read 512 of them. The item cull drops 94 of 97 primitives on
    // the same document and could drop nothing here, because a volume's
    // influence bound is its whole box and every brick inside it survives.
    //
    // SOUND ON THE CULL'S OWN TERMS, and only those. `ctape_volume_dist`
    // CLAMPS a query onto the sampled box and folds in the distance to it, so
    // a cropped volume answers differently OUTSIDE the crop — exactly as a
    // culled tape already answers differently outside its region. A caller
    // must therefore crop to a region it will not evaluate beyond, which is
    // the contract a culled tape already has. Inside, a sample projects to
    // itself, reads the same brick and the same samples, and the answer is
    // bit-identical rather than close.
    //
    // The crop is dilated by a brick on every side, so a sample on the
    // region's face still finds the neighbouring brick a trilinear tap may
    // reach.
    std::optional<FieldVolume> cropped(const math::Aabb& region) const;

    std::vector<float> to_blob() const;
    // How long that blob is, without building it. What a volume COSTS is a
    // question a host asks while deciding, and often repeatedly; materialising
    // a copy of every sample to measure one integer is the wrong way to answer
    // it.
    std::size_t blob_floats() const;
    static std::optional<FieldVolume> from_blob(const std::vector<float>& blob);

    // `with_color` false writes the volume WITHOUT its colour section, which
    // is how a document written at an older format minor drops only the
    // colours: the samples, the sparsity and the bounds are untouched, and a
    // volume that has no colour is unaffected either way.
    std::vector<std::uint8_t> serialize(bool with_color = true) const;
    static std::optional<FieldVolume> deserialize(const std::uint8_t* data, std::size_t size);

  private:
    void build_far_bounds();
    // The half of resample_region that only READS: which bricks meet the
    // region, and what `fill` produced for each. Split out so the half that
    // rewrites the sparse storage is one walk over the slots and nothing else.
    std::vector<std::size_t> fill_region_bricks(const Region& region, const BrickBlockFill& fill,
                                                std::vector<float>* blocks) const;
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
    // Packed 0x00RRGGBB per stored sample, parallel to data_, or empty when
    // this volume carries no colour. Present or absent as a WHOLE rather than
    // per brick: a per-brick flag would make every reader branch — including
    // the tape, per sample — to save memory in volumes that are mostly
    // uncoloured, which is not a case that exists.
    std::vector<std::uint32_t> colors_;
};

}  // namespace field
}  // namespace clay
