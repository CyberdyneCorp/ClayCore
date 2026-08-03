#include "clay/brick/cache.h"

#include <algorithm>
#include <cmath>

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
        for (auto& [key, t] : bricks_) dirty_one(key, t);
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
        out.push_back(req);
    }
    dirty_.clear();
    return out;
}

SubmitResult BrickCache::submit(const BrickRequest& request, const float* values) {
    auto it = bricks_.find(request.key);
    if (it == bricks_.end()) return SubmitResult::Stale;
    Tracked& t = it->second;
    if (request.generation != t.target_generation) return SubmitResult::Stale;

    const float band = config_.band();
    const std::size_t n =
        static_cast<std::size_t>(config_.dim) * config_.dim * config_.dim;
    bool all_inside = true, all_outside = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (values[i] > -band) all_inside = false;
        if (values[i] < band) all_outside = false;
        if (!all_inside && !all_outside) break;
    }

    std::size_t old_bytes = t.brick.state == BrickState::Surface ? config_.brick_bytes() : 0;
    if (all_inside || all_outside) {
        t.brick.state = all_inside ? BrickState::Inside : BrickState::Outside;
        t.brick.values.clear();
        t.brick.values.shrink_to_fit();
        surface_bytes_ -= old_bytes;
    } else {
        std::size_t new_usage = surface_bytes_ - old_bytes + config_.brick_bytes();
        if (config_.memory_budget != 0 && new_usage > config_.memory_budget)
            return SubmitResult::BudgetExceeded;
        t.brick.state = BrickState::Surface;
        t.brick.values.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            t.brick.values[i] = float_to_half(kernel::cclamp(values[i], -band, band));
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
    const Brick* b = find(key);
    if (!b) return config_.band();
    if (b->state == BrickState::Inside) return -config_.band();
    if (b->state == BrickState::Outside) return config_.band();
    std::size_t idx = (static_cast<std::size_t>(k) * config_.dim + j) * config_.dim + i;
    return half_to_float(b->values[idx]);
}

std::vector<BrickKey> BrickCache::surface_bricks() const {
    std::vector<BrickKey> out;
    for (const auto& [key, t] : bricks_)
        if (t.evaluated && t.brick.state == BrickState::Surface) out.push_back(key);
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
