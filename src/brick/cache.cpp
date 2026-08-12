#include "clay/brick/cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clay {
namespace brick {

using kernel::cf3;

math::Aabb BrickCache::brick_bounds(BrickKey key) const {
    float bs = static_cast<float>(config_.dim) * config_.voxel_size;
    math::Aabb b;
    b.min = cf3(static_cast<float>(key.x) * bs, static_cast<float>(key.y) * bs,
                static_cast<float>(key.z) * bs);
    b.max = b.min + cf3(bs, bs, bs);
    return b;
}

void BrickCache::mark_dirty(const math::Aabb& world_bound) {
    auto dirty_one = [&](BrickKey key, Tracked& t) {
        ++t.target_generation;
        if (!t.queued) {
            t.queued = true;
            dirty_.push_back(key);
        }
        invalidate_mip_of(key);
    };

    if (world_bound.is_infinite()) {
        dirty_.clear();
        // The queue is dropped but the queued FLAGS survive it, and dirty_one
        // only pushes a brick that is not already queued. Without clearing the
        // flag first, every brick that was already waiting is stranded: never
        // re-queued by this call and never again by any later one, so the cache
        // serves its stale samples for the rest of its life and only being
        // destroyed recovers. Reached by the documented way to say "this edit's
        // influence is unbounded".
        for (auto& [key, t] : bricks_) {
            t.queued = false;
            dirty_one(key, t);
        }
        return;
    }
    if (world_bound.empty()) return;

    math::Aabb region = world_bound.dilated(config_.band());
    float bs = static_cast<float>(config_.dim) * config_.voxel_size;
    int x0 = static_cast<int>(std::floor(region.min.x / bs));
    int y0 = static_cast<int>(std::floor(region.min.y / bs));
    int z0 = static_cast<int>(std::floor(region.min.z / bs));
    int x1 = static_cast<int>(std::floor(region.max.x / bs));
    int y1 = static_cast<int>(std::floor(region.max.y / bs));
    int z1 = static_cast<int>(std::floor(region.max.z / bs));
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                BrickKey key{x, y, z};
                dirty_one(key, bricks_[key]);  // tracks unseen keys
            }
}

std::vector<BrickRequest> BrickCache::take_dirty() {
    std::vector<BrickRequest> out;
    out.reserve(dirty_.size());
    for (BrickKey key : dirty_) {
        auto it = bricks_.find(key);
        if (it == bricks_.end()) continue;
        it->second.queued = false;
        BrickRequest req;
        req.key = key;
        req.generation = it->second.target_generation;
        req.grid.origin = brick_bounds(key).min;
        req.grid.spacing = config_.voxel_size;
        req.grid.nx = req.grid.ny = req.grid.nz = config_.dim;
        req.band = config_.band();
        out.push_back(req);
    }
    dirty_.clear();
    return out;
}

namespace {

std::uint8_t quantize_channel(float v) {
    // The color field is an authored albedo, not radiance: ctape_eval seeds it
    // at 0.5 and every combine mode mixes between item colors, so [0, 1] is the
    // range. Clamped rather than scaled, because a value outside it is a
    // caller's, not a signal to rescale everything else.
    return static_cast<std::uint8_t>(kernel::cclamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

BrickColor to_brick_color(const float* rgb) {
    return BrickColor{quantize_channel(rgb[0]), quantize_channel(rgb[1]),
                      quantize_channel(rgb[2]), 255};
}

}  // namespace

SubmitResult BrickCache::submit(const BrickRequest& request, const float* values,
                                const float* colors_rgb) {
    auto it = bricks_.find(request.key);
    if (it == bricks_.end()) return SubmitResult::Stale;
    Tracked& t = it->second;
    if (request.generation != t.target_generation) return SubmitResult::Stale;

    const float band = config_.band();
    const std::size_t n = config_.sample_count();
    const bool want_colors = config_.colors && colors_rgb != nullptr;
    bool all_inside = true, all_outside = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (values[i] > -band) all_inside = false;
        if (values[i] < band) all_outside = false;
        if (!all_inside && !all_outside) break;
    }

    std::size_t old_bytes = t.brick.state == BrickState::Surface ? config_.brick_bytes() : 0;
    // A uniform brick's whole color payload, so a padded or unpadded readback
    // still answers one value per texel without allocating a lattice for it.
    // The brick's CENTRE sample rather than a fixed constant: near the surface
    // the field carries the neighbouring item's color there, and inventing a
    // neutral grey instead would put a grey shell around every sculpt.
    if (want_colors) t.brick.uniform_color = to_brick_color(colors_rgb + (n / 2) * 3);
    if (all_inside || all_outside) {
        t.brick.state = all_inside ? BrickState::Inside : BrickState::Outside;
        t.brick.values.clear();
        t.brick.values.shrink_to_fit();
        t.brick.colors.clear();
        t.brick.colors.shrink_to_fit();
        surface_bytes_ -= old_bytes;
    } else {
        std::size_t new_usage = surface_bytes_ - old_bytes + config_.brick_bytes();
        if (config_.memory_budget != 0 && new_usage > config_.memory_budget)
            return SubmitResult::BudgetExceeded;
        t.brick.state = BrickState::Surface;
        t.brick.values.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            t.brick.values[i] = float_to_half(kernel::cclamp(values[i], -band, band));
        if (want_colors) {
            t.brick.colors.resize(n);
            for (std::size_t i = 0; i < n; ++i)
                t.brick.colors[i] = to_brick_color(colors_rgb + i * 3);
        }
        surface_bytes_ = new_usage;
    }
    t.brick.generation = request.generation;
    t.evaluated = true;
    return SubmitResult::Accepted;
}

const Brick* BrickCache::find(BrickKey key) const {
    auto it = bricks_.find(key);
    return (it != bricks_.end() && it->second.evaluated) ? &it->second.brick : nullptr;
}

float BrickCache::sample(BrickKey key, int i, int j, int k) const {
    return sample_lod(0, key, i, j, k);
}

float BrickCache::sample_lod(int lod, BrickKey key, int i, int j, int k) const {
    const Brick* b = find_lod(lod, key);
    if (!b) return config_.band();
    if (b->state == BrickState::Inside) return -config_.band();
    if (b->state == BrickState::Outside) return config_.band();
    std::size_t idx = (static_cast<std::size_t>(k) * config_.dim + j) * config_.dim + i;
    return half_to_float(b->values[idx]);
}

BrickColor BrickCache::sample_color(BrickKey key, int i, int j, int k) const {
    const Brick* b = find(key);
    if (!b) return BrickColor{};  // never evaluated: the neutral seed
    if (b->colors.empty()) return b->uniform_color;
    std::size_t idx = (static_cast<std::size_t>(k) * config_.dim + j) * config_.dim + i;
    return b->colors[idx];
}

namespace {

// Floor division, so a negative global lattice coordinate lands in the brick
// below rather than in the one above.
int fdiv(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

}  // namespace

std::uint16_t BrickCache::stored_half_at(int lod, int gx, int gy, int gz) const {
    const int dim = config_.dim;
    const BrickKey key{fdiv(gx, dim), fdiv(gy, dim), fdiv(gz, dim)};
    const Brick* b = find_lod(lod, key);
    // A brick the cache has nothing for reads as OUTSIDE here rather than as
    // missing: this is a halo sample, and a halo has no state to report. It is
    // the same answer sample() gives, in stored bits instead of decoded ones.
    if (!b || b->state == BrickState::Outside) return float_to_half(config_.band());
    if (b->state == BrickState::Inside) return float_to_half(-config_.band());
    const int i = gx - key.x * dim, j = gy - key.y * dim, k = gz - key.z * dim;
    return b->values[(static_cast<std::size_t>(k) * dim + j) * dim + i];
}

BrickColor BrickCache::stored_color_at(int gx, int gy, int gz) const {
    const int dim = config_.dim;
    const BrickKey key{fdiv(gx, dim), fdiv(gy, dim), fdiv(gz, dim)};
    return sample_color(key, gx - key.x * dim, gy - key.y * dim, gz - key.z * dim);
}

void BrickCache::read_padded(int lod, BrickKey key, int apron, std::uint16_t* dst) const {
    const int dim = config_.dim;
    const int w = dim + 2 * apron;
    const Brick* self = find_lod(lod, key);
    const bool run_copyable = self && self->state == BrickState::Surface;
    for (int dk = 0; dk < w; ++dk) {
        const int gz = key.z * dim + dk - apron;
        for (int dj = 0; dj < w; ++dj) {
            const int gy = key.y * dim + dj - apron;
            std::uint16_t* row = dst + (static_cast<std::size_t>(dk) * w + dj) * w;
            const bool own_row = dk >= apron && dk < apron + dim && dj >= apron && dj < apron + dim;
            if (run_copyable && own_row) {
                // The brick's own span of this row is contiguous in storage, so
                // the interior stays a copy and only the two halo ends are
                // gathered. At apron 0 this is the whole row and the gather
                // loops do not run.
                const std::size_t src =
                    (static_cast<std::size_t>(dk - apron) * dim + (dj - apron)) * dim;
                std::memcpy(row + apron, self->values.data() + src, dim * sizeof(std::uint16_t));
                for (int di = 0; di < apron; ++di)
                    row[di] = stored_half_at(lod, key.x * dim + di - apron, gy, gz);
                for (int di = apron + dim; di < w; ++di)
                    row[di] = stored_half_at(lod, key.x * dim + di - apron, gy, gz);
            } else {
                for (int di = 0; di < w; ++di)
                    row[di] = stored_half_at(lod, key.x * dim + di - apron, gy, gz);
            }
        }
    }
}

void BrickCache::read_colors_padded(BrickKey key, int apron, BrickColor* dst) const {
    const int dim = config_.dim;
    const int w = dim + 2 * apron;
    const Brick* self = find(key);
    const bool run_copyable = self && !self->colors.empty();
    for (int dk = 0; dk < w; ++dk) {
        const int gz = key.z * dim + dk - apron;
        for (int dj = 0; dj < w; ++dj) {
            const int gy = key.y * dim + dj - apron;
            BrickColor* row = dst + (static_cast<std::size_t>(dk) * w + dj) * w;
            const bool own_row = dk >= apron && dk < apron + dim && dj >= apron && dj < apron + dim;
            if (run_copyable && own_row) {
                const std::size_t src =
                    (static_cast<std::size_t>(dk - apron) * dim + (dj - apron)) * dim;
                std::memcpy(row + apron, self->colors.data() + src, dim * sizeof(BrickColor));
                for (int di = 0; di < apron; ++di)
                    row[di] = stored_color_at(key.x * dim + di - apron, gy, gz);
                for (int di = apron + dim; di < w; ++di)
                    row[di] = stored_color_at(key.x * dim + di - apron, gy, gz);
            } else {
                for (int di = 0; di < w; ++di)
                    row[di] = stored_color_at(key.x * dim + di - apron, gy, gz);
            }
        }
    }
}

std::vector<BrickKey> BrickCache::surface_bricks() const {
    std::vector<BrickKey> out;
    for (const auto& [key, t] : bricks_)
        if (t.evaluated && t.brick.state == BrickState::Surface) out.push_back(key);
    return out;
}

std::vector<BrickKey> BrickCache::surface_bricks_lod(int lod) const {
    if (lod == 0) return surface_bricks();
    std::vector<BrickKey> out;
    if (lod != 1) return out;  // there is one mip level; any other holds nothing
    out.reserve(mips_.size());
    // Every stored mip is a Surface brick by construction — build_mip writes a
    // lattice or writes nothing — so there is no state to filter here.
    for (const auto& [key, mip] : mips_) out.push_back(key);
    return out;
}

void BrickCache::invalidate_mip_of(BrickKey key) {
    BrickKey coarse{key.x >= 0 ? key.x / 2 : (key.x - 1) / 2,
                    key.y >= 0 ? key.y / 2 : (key.y - 1) / 2,
                    key.z >= 0 ? key.z / 2 : (key.z - 1) / 2};
    mips_.erase(coarse);
}

bool BrickCache::build_mip(BrickKey ck) {
    const int dim = config_.dim;
    // all 8 children must be evaluated and clean
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                BrickKey child{ck.x * 2 + dx, ck.y * 2 + dy, ck.z * 2 + dz};
                auto it = bricks_.find(child);
                if (it == bricks_.end() || !it->second.evaluated) return false;
                if (it->second.brick.generation != it->second.target_generation) return false;
            }
    Brick mip;
    mip.state = BrickState::Surface;
    mip.values.resize(static_cast<std::size_t>(dim) * dim * dim);
    for (int k = 0; k < dim; ++k)
        for (int j = 0; j < dim; ++j)
            for (int i = 0; i < dim; ++i) {
                // coarse lattice point (i,j,k) = fine lattice point (2i,2j,2k)
                int fi = i * 2, fj = j * 2, fk = k * 2;
                BrickKey child{ck.x * 2 + fi / dim, ck.y * 2 + fj / dim, ck.z * 2 + fk / dim};
                float v = sample(child, fi % dim, fj % dim, fk % dim);
                std::size_t idx = (static_cast<std::size_t>(k) * dim + j) * dim + i;
                mip.values[idx] = float_to_half(v);
            }
    mips_[ck] = std::move(mip);
    return true;
}

const Brick* BrickCache::find_mip(BrickKey ck) const {
    auto it = mips_.find(ck);
    return it == mips_.end() ? nullptr : &it->second;
}

int BrickCache::current_lod(BrickKey ck) const { return find_mip(ck) ? 1 : 0; }

}  // namespace brick
}  // namespace clay
