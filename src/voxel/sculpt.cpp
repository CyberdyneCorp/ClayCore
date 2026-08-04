// Falloff brushes and the sculpting verbs (voxel-engine spec).
//
// Two ideas carry this file. First, occupancy is binary, so a falloff weight
// between 0 and 1 is resolved by dithering against a hash of the cell
// coordinate: the brush softens as fractional coverage over the footprint,
// not as partial density in a cell, and it does so reproducibly. Second,
// every verb reads a snapshot of the region before writing, so no cell's
// outcome depends on a neighbour the same call already modified.

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/voxel/grid.h"

namespace clay {
namespace voxel {

namespace {

struct BrushExtent {
    int lo, hi;
};
inline BrushExtent brush_extent(int n) { return {-((n - 1) / 2), n / 2}; }

// Same footprint test as the plain brushes, in half-units so it stays exact
// for the half-integer centre an even size has.
inline bool in_footprint(int x, int y, int z, int n, BrushExtent e, BrushShape shape) {
    if (shape == BrushShape::Cube) return true;
    int mid = e.lo + e.hi;
    int dx = 2 * x - mid, dy = 2 * y - mid, dz = 2 * z - mid;
    return dx * dx + dy * dy + dz * dz <= n * n;
}

// Normalized distance from the footprint centre: 0 at the centre, 1 at the
// nominal radius. Values past 1 are possible at a cube's corners.
inline float normalized_distance(int x, int y, int z, int n, BrushExtent e) {
    int mid = e.lo + e.hi;
    float dx = (2.0f * x - mid) * 0.5f;
    float dy = (2.0f * y - mid) * 0.5f;
    float dz = (2.0f * z - mid) * 0.5f;
    float radius = n * 0.5f;
    if (radius <= 0.0f) return 0.0f;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / radius;
}

inline float falloff_weight(BrushFalloff curve, float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    switch (curve) {
        case BrushFalloff::Linear:
            return 1.0f - d;
        case BrushFalloff::Smooth: {
            float t = 1.0f - d;  // smoothstep from rim to centre
            return t * t * (3.0f - 2.0f * t);
        }
        case BrushFalloff::Gaussian:
            // ~exp(-4.5 d^2): about 1% weight left at the rim
            return std::exp(-4.5f * d * d);
        case BrushFalloff::Constant:
        default:
            return 1.0f;
    }
}

// Deterministic per-cell threshold in [0, 1). Same cell + seed => same value
// on every platform: integer mixing only, no floating point until the end.
inline float cell_threshold(VoxelCoord c, std::uint32_t seed) {
    std::uint32_t h = seed * 0x9E3779B9u;
    h ^= static_cast<std::uint32_t>(c.x) * 0x85EBCA6Bu;
    h = (h ^ (h >> 13)) * 0xC2B2AE35u;
    h ^= static_cast<std::uint32_t>(c.y) * 0x27D4EB2Fu;
    h = (h ^ (h >> 15)) * 0x165667B1u;
    h ^= static_cast<std::uint32_t>(c.z) * 0x9E3779B1u;
    h = (h ^ (h >> 16)) * 0x7FEB352Du;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

// Whether this cell is touched by a stamp of the given weight. Weight >= 1
// always passes and <= 0 never does, so a constant-falloff brush at full
// strength is exactly the hard-edged brush.
inline bool passes(VoxelCoord world, float weight, std::uint32_t seed) {
    if (weight >= 1.0f) return true;
    if (weight <= 0.0f) return false;
    return weight > cell_threshold(world, seed);
}

// A dense copy of a cell box, so verbs read pre-operation state. Reads
// outside the box return 0 (empty), which is correct: the box is always
// padded past the footprint by the neighbourhood radius the verb needs.
struct Region {
    VoxelCoord lo{}, hi{};
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> cells;

    std::uint8_t at(int x, int y, int z) const {
        if (x < lo.x || y < lo.y || z < lo.z || x > hi.x || y > hi.y || z > hi.z) return 0;
        std::size_t i = static_cast<std::size_t>(x - lo.x) +
                        static_cast<std::size_t>(nx) *
                            (static_cast<std::size_t>(y - lo.y) +
                             static_cast<std::size_t>(ny) * static_cast<std::size_t>(z - lo.z));
        return cells[i];
    }
};

Region snapshot(const VoxelGrid& g, VoxelCoord c, int n, int pad) {
    BrushExtent e = brush_extent(n);
    Region r;
    r.lo = {c.x + e.lo - pad, c.y + e.lo - pad, c.z + e.lo - pad};
    r.hi = {c.x + e.hi + pad, c.y + e.hi + pad, c.z + e.hi + pad};
    r.nx = r.hi.x - r.lo.x + 1;
    r.ny = r.hi.y - r.lo.y + 1;
    r.nz = r.hi.z - r.lo.z + 1;
    r.cells.resize(static_cast<std::size_t>(r.nx) * r.ny * r.nz);
    std::size_t i = 0;
    for (int z = r.lo.z; z <= r.hi.z; ++z)
        for (int y = r.lo.y; y <= r.hi.y; ++y)
            for (int x = r.lo.x; x <= r.hi.x; ++x) r.cells[i++] = g.get({x, y, z});
    return r;
}

int occupied_neighbours_26(const Region& r, int x, int y, int z) {
    int count = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                if (r.at(x + dx, y + dy, z + dz) != 0) ++count;
            }
    return count;
}

// Most common non-empty palette index among the 26 neighbours (0 if none).
std::uint8_t majority_colour(const Region& r, int x, int y, int z) {
    int tally[256] = {0};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                std::uint8_t v = r.at(x + dx, y + dy, z + dz);
                if (v != 0) ++tally[v];
            }
    std::uint8_t best = 0;
    int best_count = 0;
    for (int i = 1; i < 256; ++i)
        if (tally[i] > best_count) {
            best_count = tally[i];
            best = static_cast<std::uint8_t>(i);
        }
    return best;
}

bool has_empty_face_neighbour(const Region& r, int x, int y, int z) {
    return r.at(x + 1, y, z) == 0 || r.at(x - 1, y, z) == 0 || r.at(x, y + 1, z) == 0 ||
           r.at(x, y - 1, z) == 0 || r.at(x, y, z + 1) == 0 || r.at(x, y, z - 1) == 0;
}

bool has_occupied_face_neighbour(const Region& r, int x, int y, int z) {
    return r.at(x + 1, y, z) != 0 || r.at(x - 1, y, z) != 0 || r.at(x, y + 1, z) != 0 ||
           r.at(x, y - 1, z) != 0 || r.at(x, y, z + 1) != 0 || r.at(x, y, z - 1) != 0;
}

// Iterate the footprint, handing the callback each in-shape cell together
// with its world coordinate and dithered pass/fail. Keeps the verbs flat.
template <typename Fn>
void for_each_brush_cell(VoxelCoord c, const BrushParams& p, Fn&& fn) {
    BrushExtent e = brush_extent(p.size);
    for (int z = e.lo; z <= e.hi; ++z)
        for (int y = e.lo; y <= e.hi; ++y)
            for (int x = e.lo; x <= e.hi; ++x) {
                if (!in_footprint(x, y, z, p.size, e, p.shape)) continue;
                VoxelCoord w{c.x + x, c.y + y, c.z + z};
                float weight = falloff_weight(p.falloff, normalized_distance(x, y, z, p.size, e)) *
                               p.strength;
                if (passes(w, weight, p.seed)) fn(w);
            }
}

}  // namespace

void VoxelGrid::set_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index) {
    for_each_brush_cell(c, p, [&](VoxelCoord w) { set(w, index); });
}

void VoxelGrid::paint_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index) {
    for_each_brush_cell(c, p, [&](VoxelCoord w) { paint(w, index); });
}

void VoxelGrid::sculpt_smooth(VoxelCoord c, const BrushParams& p) {
    Region before = snapshot(*this, c, p.size, 1);
    for_each_brush_cell(c, p, [&](VoxelCoord w) {
        int n = occupied_neighbours_26(before, w.x, w.y, w.z);
        bool occupied = before.at(w.x, w.y, w.z) != 0;
        // Majority of the 26 neighbours decides: under-supported material
        // goes, well-surrounded gaps fill.
        if (occupied && n < 13) {
            set(w, 0);
        } else if (!occupied && n > 13) {
            set(w, majority_colour(before, w.x, w.y, w.z));
        }
    });
}

void VoxelGrid::sculpt_inflate(VoxelCoord c, const BrushParams& p, int amount) {
    int steps = amount < 0 ? -amount : amount;
    bool grow = amount > 0;
    for (int step = 0; step < steps; ++step) {
        Region before = snapshot(*this, c, p.size, 1);
        for_each_brush_cell(c, p, [&](VoxelCoord w) {
            bool occupied = before.at(w.x, w.y, w.z) != 0;
            if (grow && !occupied && has_occupied_face_neighbour(before, w.x, w.y, w.z)) {
                set(w, majority_colour(before, w.x, w.y, w.z));
            } else if (!grow && occupied && has_empty_face_neighbour(before, w.x, w.y, w.z)) {
                set(w, 0);
            }
        });
    }
}

void VoxelGrid::sculpt_flatten(VoxelCoord c, const BrushParams& p, kernel::cfloat3 normal,
                               float offset_cells) {
    kernel::cfloat3 n = kernel::cnormalize(normal);
    Region before = snapshot(*this, c, p.size, 1);
    for_each_brush_cell(c, p, [&](VoxelCoord w) {
        // Signed distance from the plane through the brush centre, in cells.
        float side = (static_cast<float>(w.x - c.x)) * n.x + (static_cast<float>(w.y - c.y)) * n.y +
                     (static_cast<float>(w.z - c.z)) * n.z - offset_cells;
        bool occupied = before.at(w.x, w.y, w.z) != 0;
        if (side > 0.0f && occupied) {
            set(w, 0);  // material proud of the plane comes off
        } else if (side <= 0.0f && !occupied &&
                   has_occupied_face_neighbour(before, w.x, w.y, w.z)) {
            set(w, majority_colour(before, w.x, w.y, w.z));  // hollows fill in
        }
    });
}

void VoxelGrid::sculpt_pinch(VoxelCoord c, const BrushParams& p) {
    Region before = snapshot(*this, c, p.size, 1);
    for_each_brush_cell(c, p, [&](VoxelCoord w) {
        if (before.at(w.x, w.y, w.z) == 0) return;
        if (!has_empty_face_neighbour(before, w.x, w.y, w.z)) return;  // interior

        // Step one cell along the dominant axis toward the brush centre.
        int dx = c.x - w.x, dy = c.y - w.y, dz = c.z - w.z;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
        VoxelCoord target = w;
        if (ax >= ay && ax >= az && ax > 0) {
            target.x += dx > 0 ? 1 : -1;
        } else if (ay >= az && ay > 0) {
            target.y += dy > 0 ? 1 : -1;
        } else if (az > 0) {
            target.z += dz > 0 ? 1 : -1;
        } else {
            return;  // already at the centre
        }

        std::uint8_t colour = before.at(w.x, w.y, w.z);
        set(w, 0);
        if (before.at(target.x, target.y, target.z) == 0) set(target, colour);
    });
}

}  // namespace voxel
}  // namespace clay
