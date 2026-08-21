#pragma once

// Sparse narrow-band brick cache (brick-cache spec, the Dreams design).
// Bricks are dim³ fp16 lattice samples clamped to ±band; bricks entirely
// inside/outside are represented by a state byte, never allocated. Requests
// are plain data — the consumer owns threading and evaluation (via the
// backend's eval_grid) and submits results back; generation counters reject
// stale in-flight results.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/brick/half.h"
#include "clay/eval/backend.h"
#include "clay/kernel/shim.h"
#include "clay/math/geom.h"

namespace clay {
namespace brick {

struct BrickKey {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const BrickKey&) const = default;
};

struct BrickKeyHash {
    std::size_t operator()(const BrickKey& k) const {
        std::uint64_t h = static_cast<std::uint32_t>(k.x) * 0x9E3779B185EBCA87ull;
        h ^= static_cast<std::uint32_t>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6);
        h ^= static_cast<std::uint32_t>(k.z) * 0x165667B19E3779F9ull + (h >> 3);
        return static_cast<std::size_t>(h);
    }
};

enum class BrickState : std::uint8_t {
    Inside,   // uniformly d <= -band: implicit, no storage
    Outside,  // uniformly d >= +band: implicit, no storage
    Surface,  // narrow band crosses this brick: dim^3 fp16 samples
};

// One RGBA8 texel. Alpha is written as 255 and RESERVED: it is not a mask and
// not coverage, so a consumer must not read it as one. RGBA rather than RGB
// because rgba8unorm is what a GPU filters; the fourth byte buys alignment the
// three-byte form would spend on a repack.
struct BrickColor {
    std::uint8_t r = 128, g = 128, b = 128, a = 255;
};

struct Brick {
    BrickState state = BrickState::Outside;
    std::uint32_t generation = 0;
    std::vector<std::uint16_t> values;  // Surface only, dim^3, band-clamped fp16
    // Surface only, dim^3, and only when the cache was configured for color.
    std::vector<BrickColor> colors;
    // What a uniform brick answers for every texel. Inside/Outside allocate no
    // lattice, so this is the whole of their color payload — 4 bytes beside the
    // state byte they already carry, which is why it is not budgeted.
    BrickColor uniform_color{};
};

struct BrickConfig {
    int dim = 8;               // 8 or 16
    float voxel_size = 0.05f;
    int band_voxels = 3;       // ±band half-width in voxels
    std::size_t memory_budget = 0;  // bytes of surface-brick payload; 0 = unlimited
    // Carry an RGBA8 lattice beside the distances. Chosen at CREATION and not
    // per call, because a color lattice has to be evaluated to exist: a per-call
    // flag would let a consumer ask a distance-only cache for colors it never
    // computed, and the only truthful answer would be an error for a mistake
    // made three calls earlier. Costs 2x the distance payload per surface brick,
    // inside the same memory_budget — which is why it is opt-in.
    bool colors = false;

    float band() const { return static_cast<float>(band_voxels) * voxel_size; }
    std::size_t sample_count() const {
        return static_cast<std::size_t>(dim) * dim * dim;
    }
    // What ONE surface brick costs the budget: distances, plus colors when the
    // cache carries them. A budget that bounded half of a brick would not be a
    // ceiling.
    std::size_t brick_bytes() const {
        return sample_count() * (sizeof(std::uint16_t) + (colors ? sizeof(BrickColor) : 0));
    }
};

// Plain-data evaluation request: run grid through any backend, submit the
// resulting values back with the same generation.
struct BrickRequest {
    BrickKey key;
    std::uint32_t generation = 0;
    eval::GridQuery grid;
    // The band this brick's values will be clamped to, carried so that whoever
    // evaluates the request culls against the right region without needing the
    // cache. A sample keeps its true distance whenever that distance is within
    // the band, so the tape must be culled against the brick DILATED by it —
    // an item a band outside the brick still decides samples inside it. Same
    // reason the lattice rides along: so the two sides cannot disagree.
    float band = 0.0f;
};

enum class SubmitResult { Accepted, Stale, BudgetExceeded };

class BrickCache {
  public:
    explicit BrickCache(BrickConfig config) : config_(config) {}

    const BrickConfig& config() const { return config_; }

    // World-space AABB covered by a brick's lattice cells.
    math::Aabb brick_bounds(BrickKey key) const;
    // AABB every request should be evaluated against (dilated by the band —
    // hand this to the tape compiler as the CullRegion).
    math::Aabb cull_region(BrickKey key) const { return brick_bounds(key).dilated(config_.band()); }

    // Mark every brick whose (band-dilated) volume intersects the bound as
    // dirty, tracking previously unseen keys in the region. Infinite bounds
    // dirty everything tracked.
    void mark_dirty(const math::Aabb& world_bound);

    // Drain the dirty set into evaluation requests (bumps generations).
    std::vector<BrickRequest> take_dirty();

    // Submit dim^3 evaluated distances for a request. Values are classified
    // (Inside/Outside/Surface), band-clamped, and quantized to fp16.
    //
    // colors_rgb is dim^3 * 3 floats in the same lattice order, REQUIRED when
    // the cache was configured for color and ignored when it was not. Like
    // `values`, it is a precondition rather than a checked argument: the C
    // boundary validates it, and an engine-side caller that gets it wrong has
    // the same bug it would have passing the wrong `values`. Classification is
    // decided by the distances alone — color never makes a brick a surface.
    SubmitResult submit(const BrickRequest& request, const float* values,
                        const float* colors_rgb = nullptr);

    const Brick* find(BrickKey key) const;
    // The brick a level answers with: level 0 is the evaluated brick, level 1
    // its mip. Any other level has no brick, which is a caller's error to
    // detect rather than something to clamp.
    const Brick* find_lod(int lod, BrickKey key) const {
        if (lod == 0) return find(key);
        return lod == 1 ? find_mip(key) : nullptr;
    }
    float sample(BrickKey key, int i, int j, int k) const;  // decoded, band-clamped
    // The same decoded sample at a LEVEL, `lod` selecting the brick as find_lod
    // does. A level that holds no brick for the key answers +band — the same
    // "outside" a never-evaluated brick answers — so a lattice walk over any
    // level is total and a mesher needs no special case at the edge of the
    // built region. Level 0 is sample() exactly.
    float sample_lod(int lod, BrickKey key, int i, int j, int k) const;
    // The color at a lattice point. A brick with no color lattice — uniform, or
    // in a cache not configured for color — answers its uniform color, which is
    // the neutral seed for a cache that stores none.
    BrickColor sample_color(BrickKey key, int i, int j, int k) const;

    // Whole-brick readback in STORED form: (dim + 2*apron)^3 values, the
    // brick's own lattice in the middle and a halo of `apron` voxels on every
    // face taken from the neighbouring bricks, so a consumer uploading the
    // result as a texture tile can filter across the brick boundary without
    // fetching neighbours itself.
    //
    // The halo is defined for EVERY neighbour: implicit and never-evaluated
    // bricks answer the same band values a single sample of them reports, so no
    // output element is undefined and a tile at the edge of the sculpted region
    // filters against the band rather than against whatever was in the buffer.
    //
    // apron == 0 is the brick alone, and is the memcpy the fp16 storage exists
    // for. `lod` selects the brick as find_lod does; colors exist at level 0
    // only, since a mip subsamples distances and averaging color across its
    // 2x2x2 block would be a filtering policy chosen on the consumer's behalf.
    void read_padded(int lod, BrickKey key, int apron, std::uint16_t* dst) const;
    void read_colors_padded(BrickKey key, int apron, BrickColor* dst) const;

    std::vector<BrickKey> surface_bricks() const;
    // The keys that store a lattice AT A LEVEL: surface_bricks() at level 0,
    // the coarse keys whose mip is built at level 1, and nothing at any other
    // level. What a whole-level mesh marches, so that "level 1 holds no bricks"
    // and "level 1 was never built" are the same enumeration rather than two.
    std::vector<BrickKey> surface_bricks_lod(int lod) const;
    std::size_t memory_usage() const { return surface_bytes_; }
    std::size_t dirty_count() const { return dirty_.size(); }
    // Bricks tracked at all — evaluated, implicit and never-evaluated alike.
    // memory_usage() bounds only the fp16 payloads, so this is the count that
    // makes visible that the per-key bookkeeping grows with how much space has
    // ever been marked dirty, OUTSIDE the memory budget.
    std::size_t tracked_count() const { return bricks_.size(); }

    // -- eviction ------------------------------------------------------------
    //
    // The budget was a wall and is now a ceiling. Before this, a submit past
    // the budget was REFUSED and the only recourse was to destroy the cache and
    // rebuild from nothing — the most expensive thing available, taken when the
    // device can least afford it. On iOS a memory warning ignored is how an app
    // gets killed, and this cache is likely the largest allocation in the
    // process.
    //
    // Eviction loses no information. A dropped brick is one that has to be
    // re-evaluated if it is looked at again, which is exactly what the
    // dirty/request/submit cycle already does — so it is expressible as
    // "return it to never-evaluated", a state the cache already has.
    //
    // NO AUTOMATIC LOOP, for the reason the cache publishes no refill loop,
    // thread pool or timer: the consumer owns scheduling. A host evicts on a
    // memory warning, on a view change, or on its own clock.

    // Drop this brick's payload and return it to never-evaluated. False when
    // the key holds no evaluated brick, so a caller can tell a miss from a
    // reclaim. A brick that is currently dirty is NOT evicted: it is already
    // scheduled to be rewritten, so dropping it would trade memory for the one
    // thing the host is actively waiting on.
    bool evict(BrickKey key);

    // Get down to `target_bytes`, dropping the bricks FURTHEST from `focus`
    // first, and report how many went.
    //
    // The policy is spatial rather than temporal, and that is the decision this
    // change had to make. "Least recently used" is the reflex answer and is the
    // wrong shape here: a sculptor works in a NEIGHBOURHOOD, so the bricks they
    // come back to are the ones near where they are working, not the ones they
    // touched most recently. Evicting near the focus is the worst available
    // choice — it is re-requested on the next dab — while a distant brick may
    // never be looked at again. Measured on a walking-stroke fixture, that is
    // worth a large factor in re-request rate over dropping arbitrarily; see
    // the change's design note.
    //
    // The cache cannot know where the focus is — only the host knows where the
    // camera points and where the last edit landed — so it is a parameter. One
    // point rather than a full ordering, because a memory warning wants an
    // answer now and building an ordering per eviction is the wrong cost.
    //
    // Never drops dirty bricks, so the target may not be reachable; the return
    // says what actually happened and `memory_usage()` says where it got to.
    std::size_t trim_to(std::size_t target_bytes, kernel::cfloat3 focus);
    // The same, with no spatial preference — for a host that has no meaningful
    // focus (an offline bake, a background document). Kept separate rather than
    // defaulted so that "I have no focus" is a statement rather than an
    // accident of leaving an argument off.
    std::size_t trim_to(std::size_t target_bytes);

    // Forget keys that hold nothing: not evaluated, not dirty, no mip. This is
    // the growth `tracked_count()` exists to make visible — the per-key
    // bookkeeping grows with how much space has ever been dirtied and is
    // OUTSIDE the memory budget. Returns how many were forgotten.
    //
    // Separate from trim_to because it frees a different pool: trim_to reclaims
    // fp16 payloads, this reclaims map entries. A host under pressure wants
    // both, in that order.
    std::size_t forget_empty();

    // What the per-key bookkeeping costs, which `memory_usage()` deliberately
    // does not count.
    //
    // A SEPARATE number rather than folded into memory_usage(), because that
    // one is compared against a budget a host already configured: making it
    // grow by a term it never included would change the meaning of a number in
    // the field rather than report a new fact.
    std::size_t bookkeeping_bytes() const;

    // -- LOD mips ------------------------------------------------------------
    // A level-1 mip brick covers 2x2x2 full-res bricks by subsampling every
    // second lattice point. Buildable only when all children are clean
    // (evaluated, not dirty); child dirtying invalidates the mip.
    bool build_mip(BrickKey coarse_key);
    const Brick* find_mip(BrickKey coarse_key) const;
    // 1 when the mip for this coarse key is valid, else 0 (full res).
    int current_lod(BrickKey coarse_key) const;
    // How many coarse keys currently hold a valid mip. O(1), so a caller can
    // ask "has level 1 been built at all" without enumerating it.
    std::size_t mip_count() const { return mips_.size(); }

  private:
    struct Tracked {
        Brick brick;
        std::uint32_t target_generation = 0;
        bool evaluated = false;
        bool queued = false;  // present in dirty_
    };

    void invalidate_mip_of(BrickKey key);

    // Global lattice coordinates -> the stored value there. The coordinate is
    // split back into a brick key and a local index, so these answer for any
    // point in space, which is what makes the halo total.
    std::uint16_t stored_half_at(int lod, int gx, int gy, int gz) const;
    BrickColor stored_color_at(int gx, int gy, int gz) const;

    BrickConfig config_;
    std::unordered_map<BrickKey, Tracked, BrickKeyHash> bricks_;
    std::unordered_map<BrickKey, Brick, BrickKeyHash> mips_;
    std::vector<BrickKey> dirty_;
    std::size_t surface_bytes_ = 0;
};

}  // namespace brick
}  // namespace clay
