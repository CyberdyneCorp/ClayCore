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

struct Brick {
    BrickState state = BrickState::Outside;
    std::uint32_t generation = 0;
    std::vector<std::uint16_t> values;  // Surface only, dim^3, band-clamped fp16
};

struct BrickConfig {
    int dim = 8;               // 8 or 16
    float voxel_size = 0.05f;
    int band_voxels = 3;       // ±band half-width in voxels
    std::size_t memory_budget = 0;  // bytes of surface-brick payload; 0 = unlimited

    float band() const { return static_cast<float>(band_voxels) * voxel_size; }
    std::size_t brick_bytes() const {
        return static_cast<std::size_t>(dim) * dim * dim * sizeof(std::uint16_t);
    }
};

// Plain-data evaluation request: run grid through any backend, submit the
// resulting values back with the same generation.
struct BrickRequest {
    BrickKey key;
    std::uint32_t generation = 0;
    eval::GridQuery grid;
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
    SubmitResult submit(const BrickRequest& request, const float* values);

    const Brick* find(BrickKey key) const;
    float sample(BrickKey key, int i, int j, int k) const;  // decoded, band-clamped

    std::vector<BrickKey> surface_bricks() const;
    std::size_t memory_usage() const { return surface_bytes_; }
    std::size_t dirty_count() const { return dirty_.size(); }

    // -- LOD mips ------------------------------------------------------------
    // A level-1 mip brick covers 2x2x2 full-res bricks by subsampling every
    // second lattice point. Buildable only when all children are clean
    // (evaluated, not dirty); child dirtying invalidates the mip.
    bool build_mip(BrickKey coarse_key);
    const Brick* find_mip(BrickKey coarse_key) const;
    // 1 when the mip for this coarse key is valid, else 0 (full res).
    int current_lod(BrickKey coarse_key) const;

  private:
    struct Tracked {
        Brick brick;
        std::uint32_t target_generation = 0;
        bool evaluated = false;
        bool queued = false;  // present in dirty_
    };

    void invalidate_mip_of(BrickKey key);

    BrickConfig config_;
    std::unordered_map<BrickKey, Tracked, BrickKeyHash> bricks_;
    std::unordered_map<BrickKey, Brick, BrickKeyHash> mips_;
    std::vector<BrickKey> dirty_;
    std::size_t surface_bytes_ = 0;
};

}  // namespace brick
}  // namespace clay
