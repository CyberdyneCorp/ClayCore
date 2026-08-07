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
#include "clay/voxel/mask.h"

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;

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

inline float cnearest(float v) { return v < 0.0f ? -std::floor(-v + 0.5f) : std::floor(v + 0.5f); }

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

    void set(int x, int y, int z, std::uint8_t v) {
        if (x < lo.x || y < lo.y || z < lo.z || x > hi.x || y > hi.y || z > hi.z) return;
        std::size_t i = static_cast<std::size_t>(x - lo.x) +
                        static_cast<std::size_t>(nx) *
                            (static_cast<std::size_t>(y - lo.y) +
                             static_cast<std::size_t>(ny) * static_cast<std::size_t>(z - lo.z));
        cells[i] = v;
    }

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

// Mask value at a voxel cell, sampled through world space so the mask's
// lattice and the grid's are free to differ — which is exactly what lets a
// mask outlive a resolution change.
inline float mask_at(const MaskField& mask, VoxelCoord c, float voxel_size) {
    return mask.sample(kernel::cf3((c.x + 0.5f) * voxel_size, (c.y + 0.5f) * voxel_size,
                                   (c.z + 0.5f) * voxel_size));
}

// Iterate the footprint, handing the callback each in-shape cell together
// with its world coordinate and dithered pass/fail. Keeps the verbs flat.
template <typename Fn>
void for_each_brush_cell(VoxelCoord c, const BrushParams& p, float voxel_size, Fn&& fn) {
    BrushExtent e = brush_extent(p.size);
    for (int z = e.lo; z <= e.hi; ++z)
        for (int y = e.lo; y <= e.hi; ++y)
            for (int x = e.lo; x <= e.hi; ++x) {
                if (!in_footprint(x, y, z, p.size, e, p.shape)) continue;
                VoxelCoord w{c.x + x, c.y + y, c.z + z};
                float weight = falloff_weight(p.falloff, normalized_distance(x, y, z, p.size, e)) *
                               p.strength;
                if (p.mask) weight *= 1.0f - mask_at(*p.mask, w, voxel_size);
                if (passes(w, weight, p.seed)) fn(w);
            }
}

// Fill empty cells that are mostly surrounded by material, `passes` times.
//
// This started as a morphological closing, which is the textbook answer and is
// wrong here for two reasons the code found: a ball of radius r FITS INTO a
// dent wider than r, so a bigger structuring element fills less rather than
// more; and a closing cannot seal a one-cell perforation in a one-cell wall at
// all, because the erosion reaches through from the void behind it. Both cases
// are exactly what this is for.
//
// The rule that does work is local and blunt: an empty cell with at least four
// of its six face neighbours occupied is inside a pocket, not beside a
// surface. A flat face gives one, a concave edge two, a corner three — so four
// is the line between "irregular surface" and "hole", and smoothing is the
// verb for the former.
//
// Shared with repair, which applies the same rule over a whole grid: filling a
// cavity and sealing a perforation are the same question at two scopes.
inline constexpr int kPocketNeighbours = 4;

int occupied_face_neighbours(const Region& r, int x, int y, int z) {
    return (r.at(x + 1, y, z) != 0) + (r.at(x - 1, y, z) != 0) + (r.at(x, y + 1, z) != 0) +
           (r.at(x, y - 1, z) != 0) + (r.at(x, y, z + 1) != 0) + (r.at(x, y, z - 1) != 0);
}

void fill_pockets(Region& r, int passes) {
    for (int step = 0; step < passes; ++step) {
        Region next = r;
        for (int z = r.lo.z; z <= r.hi.z; ++z)
            for (int y = r.lo.y; y <= r.hi.y; ++y)
                for (int x = r.lo.x; x <= r.hi.x; ++x)
                    if (r.at(x, y, z) == 0 &&
                        occupied_face_neighbours(r, x, y, z) >= kPocketNeighbours)
                        next.set(x, y, z, majority_colour(r, x, y, z));
        r = std::move(next);
    }
}

}  // namespace

void VoxelGrid::set_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index) {
    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) { set(w, index); });
}

void VoxelGrid::paint_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index) {
    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) { paint(w, index); });
}

void VoxelGrid::sculpt_smooth(VoxelCoord c, const BrushParams& p) {
    Region before = snapshot(*this, c, p.size, 1);
    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
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
        for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
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
    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
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

void VoxelGrid::sculpt_fill_cavities(VoxelCoord c, const BrushParams& p, int width) {
    if (width < 1) return;
    // Padded by the number of passes so a pocket touching the footprint's edge
    // sees the material on the other side of it.
    Region closed = snapshot(*this, c, p.size, width + 1);
    fill_pockets(closed, width);
    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
        if (get(w) == 0 && closed.at(w.x, w.y, w.z) != 0) set(w, closed.at(w.x, w.y, w.z));
    });
}

void VoxelGrid::sculpt_scrape(VoxelCoord c, const BrushParams& p, cfloat3 normal,
                              float offset_cells) {
    float len = kernel::clength(normal);
    if (len < 1e-9f) return;
    cfloat3 n = normal * (1.0f / len);
    // ONE snapshot for both decisions. Flattening then smoothing as two calls
    // would let the flatten's output feed the smooth's neighbourhood, which is
    // the thing every verb here is written to avoid.
    Region before = snapshot(*this, c, p.size, 1);
    float plane = kernel::cdot(cf3(static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f,
                                   static_cast<float>(c.z) + 0.5f),
                               n) +
                  offset_cells;
    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
        float height = kernel::cdot(cf3(static_cast<float>(w.x) + 0.5f,
                                        static_cast<float>(w.y) + 0.5f,
                                        static_cast<float>(w.z) + 0.5f),
                                    n) -
                       plane;
        bool occupied = before.at(w.x, w.y, w.z) != 0;
        int neighbours = occupied_neighbours_26(before, w.x, w.y, w.z);
        if (occupied && height > 0.0f) {
            set(w, 0);  // proud of the plane: scraped off
        } else if (occupied && neighbours < 13) {
            set(w, 0);  // under-supported: the smoothing half
        } else if (!occupied && height <= 0.0f && neighbours > 13) {
            set(w, majority_colour(before, w.x, w.y, w.z));  // a hollow below the plane
        }
    });
}

void VoxelGrid::sculpt_smudge(VoxelCoord c, const BrushParams& p, cfloat3 displacement) {
    // Padded by the displacement so material dragged in from outside the
    // footprint is seen.
    int pad = 1 + static_cast<int>(std::ceil(kernel::clength(displacement) / voxel_size_));
    Region before = snapshot(*this, c, p.size, pad);
    cfloat3 step = displacement * (1.0f / voxel_size_);

    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
        bool occupied = before.at(w.x, w.y, w.z) != 0;
        // Only the skin moves. An interior cell has no empty face neighbour,
        // so it is left exactly where it was — that is the whole difference
        // from grab, which translates every cell in its region.
        bool surface = occupied ? has_empty_face_neighbour(before, w.x, w.y, w.z)
                                : has_occupied_face_neighbour(before, w.x, w.y, w.z);
        if (!surface) return;
        VoxelCoord from{w.x - static_cast<std::int32_t>(cnearest(step.x)),
                        w.y - static_cast<std::int32_t>(cnearest(step.y)),
                        w.z - static_cast<std::int32_t>(cnearest(step.z))};
        std::uint8_t source = before.at(from.x, from.y, from.z);
        // Dragged material lands; behind the drag the skin is left to the
        // interior it uncovered rather than punched through.
        if (source != 0) {
            set(w, source);
        } else if (occupied && !has_occupied_face_neighbour(before, from.x, from.y, from.z)) {
            set(w, 0);
        }
    });
}

bool VoxelGrid::sculpt_carve_alpha(VoxelCoord c, const BrushParams& p, const float* alpha,
                                   int alpha_width, int alpha_height, cfloat3 direction,
                                   std::uint8_t index) {
    if (!alpha || alpha_width <= 0 || alpha_height <= 0) return false;
    float len = kernel::clength(direction);
    if (len < 1e-9f) return false;
    cfloat3 n = direction * (1.0f / len);
    // Any two axes perpendicular to the direction will do: the stamp's own
    // rotation is the caller's business, and picking one here would be an
    // opinion the engine has no basis for.
    cfloat3 helper = std::abs(n.y) < 0.9f ? cf3(0, 1, 0) : cf3(1, 0, 0);
    cfloat3 u = kernel::cnormalize(kernel::ccross(helper, n));
    cfloat3 v = kernel::ccross(n, u);

    BrushExtent e = brush_extent(p.size);
    const float radius = std::max(p.size * 0.5f, 1e-6f);
    for (int z = e.lo; z <= e.hi; ++z)
        for (int y = e.lo; y <= e.hi; ++y)
            for (int x = e.lo; x <= e.hi; ++x) {
                if (!in_footprint(x, y, z, p.size, e, p.shape)) continue;
                VoxelCoord w{c.x + x, c.y + y, c.z + z};
                cfloat3 offset = cf3(static_cast<float>(x), static_cast<float>(y),
                                     static_cast<float>(z));
                // Projected onto the plane perpendicular to the direction, then
                // mapped to the stamp's [0,1] square.
                float su = (kernel::cdot(offset, u) / radius) * 0.5f + 0.5f;
                float sv = (kernel::cdot(offset, v) / radius) * 0.5f + 0.5f;
                if (su < 0.0f || su >= 1.0f || sv < 0.0f || sv >= 1.0f) continue;
                int ai = std::min(static_cast<int>(su * alpha_width), alpha_width - 1);
                int aj = std::min(static_cast<int>(sv * alpha_height), alpha_height - 1);
                float a = std::clamp(alpha[aj * alpha_width + ai], 0.0f, 1.0f);
                if (a <= 0.0f) continue;

                float weight = falloff_weight(p.falloff, normalized_distance(x, y, z, p.size, e)) *
                               p.strength * a;
                if (p.mask) weight *= 1.0f - mask_at(*p.mask, w, voxel_size_);
                if (passes(w, weight, p.seed)) set(w, index);
            }
    return true;
}

void VoxelGrid::sculpt_grab(VoxelCoord c, const BrushParams& p, kernel::cfloat3 displacement,
                            bool front_only) {
    // Pad by the displacement so material pulled in from outside the footprint
    // is still in the snapshot we read from.
    int pad = 1 + static_cast<int>(kernel::clength(displacement) /
                                   kernel::cmax(voxel_size_, 1e-6f));
    Region before = snapshot(*this, c, p.size, pad);

    BrushExtent e = brush_extent(p.size);
    float radius = static_cast<float>(p.size) * 0.5f;
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);  // offsets are relative to c

    for_each_brush_cell(c, p, voxel_size_, [&](VoxelCoord w) {
        // Where this cell's material came from, in cell units, through the same
        // map cgrab_point applies to a point.
        kernel::cfloat3 local = kernel::cf3(static_cast<float>(w.x - c.x),
                                            static_cast<float>(w.y - c.y),
                                            static_cast<float>(w.z - c.z));
        kernel::cfloat3 cells = displacement * (1.0f / kernel::cmax(voxel_size_, 1e-6f));
        kernel::cfloat3 src = cgrab_point(local, centre, radius, cells,
                                          front_only ? 1.0f : 0.0f,
                                          static_cast<int>(p.falloff));
        VoxelCoord from{c.x + static_cast<std::int32_t>(cnearest(src.x)),
                        c.y + static_cast<std::int32_t>(cnearest(src.y)),
                        c.z + static_cast<std::int32_t>(cnearest(src.z))};
        set(w, before.at(from.x, from.y, from.z));
    });
    (void)e;
}

namespace {

// One cell along the dominant axis, toward the brush centre or away from it.
// Returns false where there is no dominant axis to step along, which is the
// cell sitting on the centre itself.
bool radial_step(VoxelCoord centre, VoxelCoord w, bool inward, VoxelCoord* target) {
    const int sign = inward ? 1 : -1;
    int dx = centre.x - w.x, dy = centre.y - w.y, dz = centre.z - w.z;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
    *target = w;
    if (ax >= ay && ax >= az && ax > 0) {
        target->x += (dx > 0 ? 1 : -1) * sign;
    } else if (ay >= az && ay > 0) {
        target->y += (dy > 0 ? 1 : -1) * sign;
    } else if (az > 0) {
        target->z += (dz > 0 ? 1 : -1) * sign;
    } else {
        return false;  // on the centre: no direction to step
    }
    return true;
}

// Pinch and magnify are the same walk with the step reversed, which is what
// "magnify is the inverse of pinch" means concretely. Sharing the body is how
// the two stay each other's inverse as either changes.
void radial_sculpt(VoxelGrid& grid, VoxelCoord c, const BrushParams& p, float voxel_size,
                   bool inward) {
    Region before = snapshot(grid, c, p.size, 1);
    for_each_brush_cell(c, p, voxel_size, [&](VoxelCoord w) {
        if (before.at(w.x, w.y, w.z) == 0) return;
        if (!has_empty_face_neighbour(before, w.x, w.y, w.z)) return;  // interior

        VoxelCoord target;
        if (!radial_step(c, w, inward, &target)) return;

        std::uint8_t colour = before.at(w.x, w.y, w.z);
        grid.set(w, 0);
        if (before.at(target.x, target.y, target.z) == 0) grid.set(target, colour);
    });
}

}  // namespace

void VoxelGrid::sculpt_pinch(VoxelCoord c, const BrushParams& p) {
    radial_sculpt(*this, c, p, voxel_size_, true);
}

// Magnify: the inverse of pinch, and deliberately the same walk with the step
// reversed. Maxon's own page has it that way — "Magnify: pushes vertices away
// from cursor; inverse of Pinch" — and the SDF side spells it as one signed
// strength for the same reason.
void VoxelGrid::sculpt_magnify(VoxelCoord c, const BrushParams& p) {
    radial_sculpt(*this, c, p, voxel_size_, false);
}

}  // namespace voxel
}  // namespace clay
