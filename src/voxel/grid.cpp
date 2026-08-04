#include "clay/voxel/grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;

namespace {
// floor division/modulo for negative lattice coordinates
inline std::int32_t fdiv(std::int32_t a, std::int32_t b) {
    return (a >= 0) ? a / b : -(((-a) + b - 1) / b);
}
inline std::int32_t fmod_pos(std::int32_t a, std::int32_t b) { return a - fdiv(a, b) * b; }
}  // namespace

VoxelCoord VoxelGrid::chunk_key(VoxelCoord c) {
    return {fdiv(c.x, kChunkDim), fdiv(c.y, kChunkDim), fdiv(c.z, kChunkDim)};
}

std::size_t VoxelGrid::chunk_offset(VoxelCoord c) {
    std::int32_t x = fmod_pos(c.x, kChunkDim);
    std::int32_t y = fmod_pos(c.y, kChunkDim);
    std::int32_t z = fmod_pos(c.z, kChunkDim);
    return (static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x;
}

// -- palette -----------------------------------------------------------------

std::uint8_t VoxelGrid::palette_add(cfloat3 color, float tolerance) {
    for (std::size_t i = 1; i < palette_.size(); ++i)
        if (kernel::clength(palette_[i] - color) <= tolerance)
            return static_cast<std::uint8_t>(i);
    if (palette_.size() >= 256) return static_cast<std::uint8_t>(palette_.size() - 1);
    palette_.push_back(color);
    return static_cast<std::uint8_t>(palette_.size() - 1);
}

cfloat3 VoxelGrid::palette_color(std::uint8_t index) const {
    return index < palette_.size() ? palette_[index] : cf3(0, 0, 0);
}

void VoxelGrid::palette_set(std::uint8_t index, cfloat3 color) {
    if (index > 0 && index < palette_.size()) palette_[index] = color;
}

// -- single voxel ------------------------------------------------------------

std::uint8_t VoxelGrid::get(VoxelCoord c) const {
    auto it = chunks_.find(chunk_key(c));
    return it == chunks_.end() ? 0 : it->second.data[chunk_offset(c)];
}

void VoxelGrid::set(VoxelCoord c, std::uint8_t index) {
    VoxelCoord key = chunk_key(c);
    auto it = chunks_.find(key);
    if (it == chunks_.end()) {
        if (index == 0) return;
        it = chunks_.emplace(key, Chunk{}).first;
        it->second.data.assign(static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim, 0);
    }
    std::uint8_t& cell = it->second.data[chunk_offset(c)];
    if (cell == 0 && index != 0) ++it->second.occupied;
    if (cell != 0 && index == 0) --it->second.occupied;
    cell = index;
    if (it->second.occupied == 0) chunks_.erase(it);
}

void VoxelGrid::paint(VoxelCoord c, std::uint8_t index) {
    if (index != 0 && get(c) != 0) set(c, index);
}

// -- brushes and fills -------------------------------------------------------

namespace {

// Whether offset (x, y, z) lies inside a brush of radius r. The sphere test
// is on cell centres, so it is exactly the sphere inscribed in the cube and
// therefore always a subset of it.
inline bool in_brush(int x, int y, int z, int r, BrushShape shape) {
    if (shape == BrushShape::Cube) return true;
    return x * x + y * y + z * z <= r * r;
}

}  // namespace

void VoxelGrid::set_brush(VoxelCoord c, int n, std::uint8_t index, BrushShape shape) {
    int r = (n - 1) / 2;
    for (int z = -r; z <= r; ++z)
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (in_brush(x, y, z, r, shape)) set({c.x + x, c.y + y, c.z + z}, index);
}

void VoxelGrid::paint_brush(VoxelCoord c, int n, std::uint8_t index, BrushShape shape) {
    int r = (n - 1) / 2;
    for (int z = -r; z <= r; ++z)
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (in_brush(x, y, z, r, shape)) paint({c.x + x, c.y + y, c.z + z}, index);
}

void VoxelGrid::fill_box(VoxelCoord a, VoxelCoord b, std::uint8_t index) {
    VoxelCoord lo{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    VoxelCoord hi{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    for (std::int32_t z = lo.z; z <= hi.z; ++z)
        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) set({x, y, z}, index);
}

void VoxelGrid::fill_line(VoxelCoord a, VoxelCoord b, std::uint8_t index) {
    // 3D DDA over cell centers
    int steps = std::max({std::abs(b.x - a.x), std::abs(b.y - a.y), std::abs(b.z - a.z)});
    if (steps == 0) {
        set(a, index);
        return;
    }
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        VoxelCoord c{static_cast<std::int32_t>(std::lround(a.x + (b.x - a.x) * t)),
                     static_cast<std::int32_t>(std::lround(a.y + (b.y - a.y) * t)),
                     static_cast<std::int32_t>(std::lround(a.z + (b.z - a.z) * t))};
        set(c, index);
    }
}

// -- mirror ------------------------------------------------------------------

VoxelCoord VoxelGrid::mirrored(VoxelCoord c, std::uint8_t axes) {
    VoxelCoord m = c;
    if (axes & kVoxMirrorX) m.x = -1 - m.x;
    if (axes & kVoxMirrorY) m.y = -1 - m.y;
    if (axes & kVoxMirrorZ) m.z = -1 - m.z;
    return m;
}

namespace {
template <typename Fn>
void for_each_mirror(VoxelCoord c, std::uint8_t axes, Fn fn) {
    for (std::uint8_t combo = 0; combo < 8; ++combo) {
        if ((combo & ~axes) != 0) continue;  // only active axes participate
        fn(VoxelGrid::mirrored(c, combo));
    }
}
}  // namespace

void VoxelGrid::set_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes) {
    for_each_mirror(c, axes, [&](VoxelCoord m) { set(m, index); });
}

void VoxelGrid::paint_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes) {
    for_each_mirror(c, axes, [&](VoxelCoord m) { paint(m, index); });
}

// -- queries -----------------------------------------------------------------

std::size_t VoxelGrid::occupied_count() const {
    std::size_t n = 0;
    for (const auto& [key, chunk] : chunks_) n += static_cast<std::size_t>(chunk.occupied);
    return n;
}

std::optional<VoxelCoord> VoxelGrid::bounds_min() const {
    std::optional<VoxelCoord> out;
    for (const auto& [key, chunk] : chunks_) {
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    if (!chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x])
                        continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!out)
                        out = c;
                    else
                        *out = {std::min(out->x, c.x), std::min(out->y, c.y),
                                std::min(out->z, c.z)};
                }
    }
    return out;
}

std::optional<VoxelCoord> VoxelGrid::bounds_max() const {
    std::optional<VoxelCoord> out;
    for (const auto& [key, chunk] : chunks_) {
        for (int z = 0; z < kChunkDim; ++z)
            for (int y = 0; y < kChunkDim; ++y)
                for (int x = 0; x < kChunkDim; ++x) {
                    if (!chunk.data[(static_cast<std::size_t>(z) * kChunkDim + y) * kChunkDim + x])
                        continue;
                    VoxelCoord c{key.x * kChunkDim + x, key.y * kChunkDim + y,
                                 key.z * kChunkDim + z};
                    if (!out)
                        out = c;
                    else
                        *out = {std::max(out->x, c.x), std::max(out->y, c.y),
                                std::max(out->z, c.z)};
                }
    }
    return out;
}

std::optional<VoxelCoord> VoxelGrid::build_plane_pick(const math::Ray& ray,
                                                      std::int32_t plane_cell) const {
    float plane_y = static_cast<float>(plane_cell) * voxel_size_;
    if (kernel::cabs(ray.dir.y) < 1e-9f) return std::nullopt;
    float t = (plane_y - ray.origin.y) / ray.dir.y;
    if (t < 0.0f) return std::nullopt;
    kernel::cfloat3 p = ray.at(t);
    return VoxelCoord{static_cast<std::int32_t>(std::floor(p.x / voxel_size_)), plane_cell,
                      static_cast<std::int32_t>(std::floor(p.z / voxel_size_))};
}

std::vector<VoxelCoord> VoxelGrid::flood_select(VoxelCoord seed, bool same_color,
                                                std::size_t max_count) const {
    std::vector<VoxelCoord> out;
    std::uint8_t target = get(seed);
    if (target == 0) return out;
    std::unordered_map<VoxelCoord, bool, VoxelCoordHash> seen;
    std::deque<VoxelCoord> queue{seed};
    seen[seed] = true;
    const VoxelCoord dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (!queue.empty() && out.size() < max_count) {
        VoxelCoord c = queue.front();
        queue.pop_front();
        out.push_back(c);
        for (const VoxelCoord& d : dirs) {
            VoxelCoord n{c.x + d.x, c.y + d.y, c.z + d.z};
            if (seen.count(n)) continue;
            std::uint8_t v = get(n);
            if (v == 0 || (same_color && v != target)) continue;
            seen[n] = true;
            queue.push_back(n);
        }
    }
    return out;
}

// -- serialization -----------------------------------------------------------

std::vector<std::uint8_t> VoxelGrid::serialize() const {
    std::vector<std::uint8_t> out;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    };
    auto putf = [&](float f) {
        std::uint32_t v;
        std::memcpy(&v, &f, 4);
        put32(v);
    };
    putf(voxel_size_);
    put32(static_cast<std::uint32_t>(palette_.size()));
    for (const cfloat3& c : palette_) {
        putf(c.x);
        putf(c.y);
        putf(c.z);
    }
    // deterministic chunk order
    std::map<std::tuple<int, int, int>, const Chunk*> ordered;
    for (const auto& [key, chunk] : chunks_) ordered[{key.x, key.y, key.z}] = &chunk;
    put32(static_cast<std::uint32_t>(ordered.size()));
    for (const auto& [key, chunk] : ordered) {
        put32(static_cast<std::uint32_t>(std::get<0>(key)));
        put32(static_cast<std::uint32_t>(std::get<1>(key)));
        put32(static_cast<std::uint32_t>(std::get<2>(key)));
        // RLE: (value, run) pairs over the linear chunk data
        std::vector<std::uint8_t> rle;
        const std::vector<std::uint8_t>& d = chunk->data;
        for (std::size_t i = 0; i < d.size();) {
            std::uint8_t v = d[i];
            std::size_t run = 1;
            while (i + run < d.size() && d[i + run] == v && run < 0xFFFF) ++run;
            rle.push_back(v);
            rle.push_back(static_cast<std::uint8_t>(run & 0xFF));
            rle.push_back(static_cast<std::uint8_t>(run >> 8));
            i += run;
        }
        put32(static_cast<std::uint32_t>(rle.size()));
        out.insert(out.end(), rle.begin(), rle.end());
    }
    return out;
}

std::optional<VoxelGrid> VoxelGrid::deserialize(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 0;
    auto get32 = [&](std::uint32_t* v) {
        if (pos + 4 > size) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) *v |= static_cast<std::uint32_t>(data[pos++]) << (i * 8);
        return true;
    };
    auto getf = [&](float* f) {
        std::uint32_t v;
        if (!get32(&v)) return false;
        std::memcpy(f, &v, 4);
        return true;
    };
    float vs;
    std::uint32_t palette_count;
    if (!getf(&vs) || !get32(&palette_count) || palette_count == 0 || palette_count > 256)
        return std::nullopt;
    VoxelGrid grid(vs);
    grid.palette_.resize(palette_count, cf3(0, 0, 0));
    for (std::uint32_t i = 0; i < palette_count; ++i)
        if (!getf(&grid.palette_[i].x) || !getf(&grid.palette_[i].y) ||
            !getf(&grid.palette_[i].z))
            return std::nullopt;
    std::uint32_t chunk_count;
    if (!get32(&chunk_count)) return std::nullopt;
    const std::size_t chunk_cells = static_cast<std::size_t>(kChunkDim) * kChunkDim * kChunkDim;
    for (std::uint32_t c = 0; c < chunk_count; ++c) {
        std::uint32_t kx, ky, kz, rle_size;
        if (!get32(&kx) || !get32(&ky) || !get32(&kz) || !get32(&rle_size)) return std::nullopt;
        if (pos + rle_size > size || rle_size % 3 != 0) return std::nullopt;
        Chunk chunk;
        chunk.data.assign(chunk_cells, 0);
        std::size_t cell = 0;
        for (std::uint32_t i = 0; i < rle_size; i += 3) {
            std::uint8_t v = data[pos + i];
            std::size_t run = data[pos + i + 1] | (static_cast<std::size_t>(data[pos + i + 2]) << 8);
            if (cell + run > chunk_cells) return std::nullopt;
            std::memset(chunk.data.data() + cell, v, run);
            if (v != 0) chunk.occupied += static_cast<int>(run);
            cell += run;
        }
        pos += rle_size;
        if (cell != chunk_cells) return std::nullopt;
        if (chunk.occupied > 0)
            grid.chunks_.emplace(
                VoxelCoord{static_cast<std::int32_t>(kx), static_cast<std::int32_t>(ky),
                           static_cast<std::int32_t>(kz)},
                std::move(chunk));
    }
    return grid;
}

// -- greedy meshing ----------------------------------------------------------

mesh::Mesh VoxelGrid::mesh_greedy() const {
    mesh::Mesh out;
    auto b0 = bounds_min();
    auto b1 = bounds_max();
    if (!b0 || !b1) return out;

    // For each of 6 face directions, sweep slices and greedy-merge rectangles
    // of equal palette index whose faces are exposed.
    struct Dir {
        int axis;     // face normal axis 0/1/2
        int sign;     // +1 or -1
    };
    const Dir dirs[6] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, 1}, {2, -1}};

    auto cell_at = [&](int a, int u, int v, int axis) {
        // map (slice a, u, v) back to xyz for a given axis
        VoxelCoord c;
        if (axis == 0) c = {a, u, v};
        if (axis == 1) c = {u, a, v};
        if (axis == 2) c = {u, v, a};
        return c;
    };

    for (const Dir& dir : dirs) {
        int a0, a1, u0, u1, v0, v1;
        if (dir.axis == 0) {
            a0 = b0->x; a1 = b1->x; u0 = b0->y; u1 = b1->y; v0 = b0->z; v1 = b1->z;
        } else if (dir.axis == 1) {
            a0 = b0->y; a1 = b1->y; u0 = b0->x; u1 = b1->x; v0 = b0->z; v1 = b1->z;
        } else {
            a0 = b0->z; a1 = b1->z; u0 = b0->x; u1 = b1->x; v0 = b0->y; v1 = b1->y;
        }
        int nu = u1 - u0 + 1, nv = v1 - v0 + 1;
        std::vector<std::uint8_t> mask(static_cast<std::size_t>(nu) * nv);

        for (int a = a0; a <= a1; ++a) {
            // build exposure mask for this slice
            for (int v = 0; v < nv; ++v)
                for (int u = 0; u < nu; ++u) {
                    VoxelCoord c = cell_at(a, u + u0, v + v0, dir.axis);
                    std::uint8_t idx = get(c);
                    if (idx != 0) {
                        VoxelCoord n = c;
                        if (dir.axis == 0) n.x += dir.sign;
                        if (dir.axis == 1) n.y += dir.sign;
                        if (dir.axis == 2) n.z += dir.sign;
                        if (get(n) != 0) idx = 0;  // covered face
                    }
                    mask[static_cast<std::size_t>(v) * nu + u] = idx;
                }
            // greedy merge
            for (int v = 0; v < nv; ++v)
                for (int u = 0; u < nu;) {
                    std::uint8_t idx = mask[static_cast<std::size_t>(v) * nu + u];
                    if (idx == 0) {
                        ++u;
                        continue;
                    }
                    int w = 1;
                    while (u + w < nu && mask[static_cast<std::size_t>(v) * nu + u + w] == idx)
                        ++w;
                    int h = 1;
                    bool grow = true;
                    while (grow && v + h < nv) {
                        for (int k = 0; k < w; ++k)
                            if (mask[static_cast<std::size_t>(v + h) * nu + u + k] != idx) {
                                grow = false;
                                break;
                            }
                        if (grow) ++h;
                    }
                    emit_quad(out, dir.axis, dir.sign, a, u + u0, v + v0, w, h, idx);
                    for (int dv = 0; dv < h; ++dv)
                        for (int du = 0; du < w; ++du)
                            mask[static_cast<std::size_t>(v + dv) * nu + u + du] = 0;
                    u += w;
                }
        }
    }
    return out;
}

void VoxelGrid::emit_quad(mesh::Mesh& out, int axis, int sign, int a, int u, int v, int w, int h,
                          std::uint8_t idx) const {
    float s = voxel_size_;
    float face = static_cast<float>(a) + (sign > 0 ? 1.0f : 0.0f);
    auto corner = [&](float uu, float vv) {
        if (axis == 0) return cf3(face, uu, vv) * s;
        if (axis == 1) return cf3(uu, face, vv) * s;
        return cf3(uu, vv, face) * s;
    };
    float uf = static_cast<float>(u), vf = static_cast<float>(v);
    cfloat3 p0 = corner(uf, vf);
    cfloat3 p1 = corner(uf + w, vf);
    cfloat3 p2 = corner(uf + w, vf + h);
    cfloat3 p3 = corner(uf, vf + h);
    cfloat3 n = cf3(0, 0, 0);
    if (axis == 0) n.x = static_cast<float>(sign);
    if (axis == 1) n.y = static_cast<float>(sign);
    if (axis == 2) n.z = static_cast<float>(sign);
    cfloat3 color = palette_color(idx);

    std::uint32_t base = static_cast<std::uint32_t>(out.positions.size());
    for (const cfloat3& p : {p0, p1, p2, p3}) {
        out.positions.push_back(p);
        out.normals.push_back(n);
        out.colors.push_back(color);
    }
    // winding so the face normal points along `n`
    bool flip;
    if (axis == 0)
        flip = sign < 0;
    else if (axis == 1)
        flip = sign > 0;
    else
        flip = sign < 0;
    std::uint32_t tri[6] = {0, 1, 2, 0, 2, 3};
    if (flip) {
        std::swap(tri[1], tri[2]);
        std::swap(tri[4], tri[5]);
    }
    for (std::uint32_t i : tri) out.indices.push_back(base + i);
}

// -- voxel <-> SDF bridges ---------------------------------------------------

float VoxelGrid::sample_step_field(cfloat3 world_p) const {
    VoxelCoord c{static_cast<std::int32_t>(std::floor(world_p.x / voxel_size_)),
                 static_cast<std::int32_t>(std::floor(world_p.y / voxel_size_)),
                 static_cast<std::int32_t>(std::floor(world_p.z / voxel_size_))};
    return get(c) != 0 ? -0.5f * voxel_size_ : 0.5f * voxel_size_;
}

void VoxelGrid::rasterize_tape(const scene::Tape& tape, const math::Aabb& world_region) {
    if (world_region.empty() || world_region.is_infinite()) return;
    std::int32_t x0 = static_cast<std::int32_t>(std::floor(world_region.min.x / voxel_size_));
    std::int32_t y0 = static_cast<std::int32_t>(std::floor(world_region.min.y / voxel_size_));
    std::int32_t z0 = static_cast<std::int32_t>(std::floor(world_region.min.z / voxel_size_));
    std::int32_t x1 = static_cast<std::int32_t>(std::floor(world_region.max.x / voxel_size_));
    std::int32_t y1 = static_cast<std::int32_t>(std::floor(world_region.max.y / voxel_size_));
    std::int32_t z1 = static_cast<std::int32_t>(std::floor(world_region.max.z / voxel_size_));
    for (std::int32_t z = z0; z <= z1; ++z)
        for (std::int32_t y = y0; y <= y1; ++y)
            for (std::int32_t x = x0; x <= x1; ++x) {
                cfloat3 center = cf3((static_cast<float>(x) + 0.5f) * voxel_size_,
                                     (static_cast<float>(y) + 0.5f) * voxel_size_,
                                     (static_cast<float>(z) + 0.5f) * voxel_size_);
                kernel::CTapeValue v = tape.eval(center);
                if (v.d < 0.0f) set({x, y, z}, palette_add(v.color, 1.0f / 64.0f));
            }
}

}  // namespace voxel
}  // namespace clay
