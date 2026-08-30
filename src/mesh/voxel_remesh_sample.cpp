// The sparse sampling domain of a voxel remesh (add-voxel-remesher).
//
// `mesh::to_field` routes through `FieldVolume::sample_parallel`, which calls
// the caller's function for EVERY brick of the region. For an import that is
// right — the caller chose the cell size for the model. For a resolution dial
// it is the one scaling property this feature is defined against: 32^3 bricks
// of 729 samples at longest-axis 256 is 24 million BVH queries each carrying a
// generalized winding number, and 1024 is 1.5 billion.
//
// So the converter is not forked and is not called: this supplies its own
// `BrickBlockFill` to the same `sample_blocks` entry point the converter uses,
// and the fill knows which bricks are worth evaluating.

#include <algorithm>
#include <cmath>
#include <vector>

#include "clay/mesh/marching.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/parallel/thread_pool.h"
#include "voxel_remesh_internal.h"

namespace clay {
namespace mesh {
namespace remesh_detail {

using kernel::cf3;

namespace {

// sample_blocks' own arithmetic, reproduced rather than approximated. The
// marking has to name the bricks the fill will be handed, and "close enough"
// there is an off-by-one brick, which is a missing patch of surface.
int floor_div(int a, int b) {
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int cells_for(float extent, float cell) {
    return std::max(1, static_cast<int>(std::ceil(extent / cell)));
}

// What a sample-free brick reports before `build_far_bounds` replaces the
// magnitude. Only the SIGN survives and only `> band` matters, which is what
// keeps `scan_block` from storing the brick; four bands is what
// `FieldVolume::empty_lattice` uses for the same purpose.
float far_fill(float band) { return band * 4.0f; }

// The brick range a world box touches, clamped to the lattice. Half-open in
// the usual way: [lo, hi) per axis.
struct BrickRange {
    int lo[3];
    int hi[3];
    bool empty() const { return lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2]; }
};

BrickRange bricks_meeting(const math::Aabb& box, const Lattice& lattice) {
    BrickRange r{{0, 0, 0}, {0, 0, 0}};
    const float inv = 1.0f / lattice.voxel_size;
    const kernel::cfloat3 lo = (box.min - lattice.region.min) * inv;
    const kernel::cfloat3 hi = (box.max - lattice.region.min) * inv;
    const float lov[3] = {lo.x, lo.y, lo.z};
    const float hiv[3] = {hi.x, hi.y, hi.z};
    for (int a = 0; a < 3; ++a) {
        // A brick's samples span cells [b*kBrickDim, b*kBrickDim + kBrickDim] —
        // NINE per axis, the last shared with its neighbour as that
        // neighbour's halo. So the brick that reaches the box from below
        // starts a whole brick below it, and the low end steps back
        // kBrickDim cells rather than one. Stepping back one is the
        // off-by-seven that leaves a patch of surface answered by a constant.
        const int cell_lo = static_cast<int>(std::floor(lov[a])) - field::kBrickDim;
        const int cell_hi = static_cast<int>(std::ceil(hiv[a]));
        r.lo[a] = std::clamp(floor_div(cell_lo, field::kBrickDim), 0, lattice.bcount[a]);
        r.hi[a] = std::clamp(floor_div(cell_hi, field::kBrickDim) + 1, 0, lattice.bcount[a]);
    }
    return r;
}

std::size_t slot_of(int bx, int by, int bz, const Lattice& lattice) {
    return static_cast<std::size_t>((static_cast<std::size_t>(bz) * lattice.bcount[1] + by) *
                                        lattice.bcount[0] +
                                    bx);
}

// The centre of a brick's sample box, which is where its region's sign is
// asked. Any interior point would do; the centre is the one furthest from the
// faces it shares with the bricks that might disagree.
kernel::cfloat3 brick_centre(std::size_t slot, const Lattice& lattice) {
    const int bx = static_cast<int>(slot % static_cast<std::size_t>(lattice.bcount[0]));
    const int by = static_cast<int>((slot / static_cast<std::size_t>(lattice.bcount[0])) %
                                    static_cast<std::size_t>(lattice.bcount[1]));
    const int bz = static_cast<int>(slot / (static_cast<std::size_t>(lattice.bcount[0]) *
                                            static_cast<std::size_t>(lattice.bcount[1])));
    const float half = 0.5f * static_cast<float>(field::kBrickDim) * lattice.voxel_size;
    const float step = static_cast<float>(field::kBrickDim) * lattice.voxel_size;
    return lattice.region.min +
           cf3(static_cast<float>(bx) * step, static_cast<float>(by) * step,
               static_cast<float>(bz) * step) +
           cf3(half, half, half);
}

// The sign of every inactive brick, by connected region.
//
// No triangle comes within the band of an inactive brick, so on a closed
// source the sign of the field is constant across any connected run of them:
// the zero set of a signed distance is the surface itself, and the surface
// lies only inside bricks that were marked. One winding-number query per
// region therefore answers for the whole region, which turns O(bounding box)
// expensive queries into O(regions) — two, for anything shaped like a model.
//
// The flood is 6-connected and seeded in slot order, so the representative of
// a region is its lowest slot on every run. That is what makes the result
// deterministic rather than dependent on the order a queue happened to fill.
//
// ON AN OPEN SOURCE the winding number's half-crossing can fall away from
// every triangle, and a region can genuinely straddle it. The header says so.
// Only sample-free bricks are affected, so nothing is stored either way.
std::vector<float> region_signs(const Bvh& bvh, const Lattice& lattice,
                                const std::vector<std::uint8_t>& active,
                                parallel::CancelToken* token) {
    const std::size_t total = active.size();
    std::vector<float> sign(total, 1.0f);
    std::vector<std::uint8_t> seen(total, 0);
    std::vector<std::size_t> stack;
    const int nx = lattice.bcount[0], ny = lattice.bcount[1], nz = lattice.bcount[2];

    for (std::size_t seed = 0; seed < total; ++seed) {
        if (active[seed] || seen[seed]) continue;
        if (parallel::cancelled(token)) return sign;
        const float s = bvh.is_inside(brick_centre(seed, lattice)) ? -1.0f : 1.0f;
        stack.clear();
        stack.push_back(seed);
        seen[seed] = 1;
        while (!stack.empty()) {
            const std::size_t at = stack.back();
            stack.pop_back();
            sign[at] = s;
            const int x = static_cast<int>(at % static_cast<std::size_t>(nx));
            const int y = static_cast<int>((at / static_cast<std::size_t>(nx)) %
                                           static_cast<std::size_t>(ny));
            const int z = static_cast<int>(at / (static_cast<std::size_t>(nx) *
                                                 static_cast<std::size_t>(ny)));
            constexpr int kFaces[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
                                          {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
            for (const auto& d : kFaces) {
                const int ax = x + d[0], ay = y + d[1], az = z + d[2];
                if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz) continue;
                const std::size_t next = slot_of(ax, ay, az, lattice);
                if (active[next] || seen[next]) continue;
                seen[next] = 1;
                stack.push_back(next);
            }
        }
    }
    return sign;
}

}  // namespace

bool resolve_lattice(const Mesh& source, const VoxelRemeshParams& params, Lattice* out,
                     VoxelRemeshStatus* out_status) {
    *out_status = VoxelRemeshStatus::Ok;
    math::Aabb bounds;
    for (const kernel::cfloat3& p : source.positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            *out_status = VoxelRemeshStatus::EmptySource;
            return false;
        }
        bounds.expand(p);
    }
    if (bounds.empty()) {
        *out_status = VoxelRemeshStatus::EmptySource;
        return false;
    }

    const kernel::cfloat3 extent = bounds.extent();
    const float longest = std::max(extent.x, std::max(extent.y, extent.z));
    if (!(longest > 0.0f)) {
        // Every vertex at one point: a surface with no size has no resolution.
        *out_status = VoxelRemeshStatus::EmptySource;
        return false;
    }

    float voxel = 0.0f;
    if (params.resolution_mode == VoxelRemeshResolutionMode::VoxelSize) {
        voxel = params.voxel_size;
    } else {
        if (params.longest_axis_resolution == 0) {
            *out_status = VoxelRemeshStatus::InvalidResolution;
            return false;
        }
        voxel = longest / static_cast<float>(params.longest_axis_resolution);
    }
    if (!std::isfinite(voxel) || !(voxel > 0.0f)) {
        *out_status = VoxelRemeshStatus::InvalidResolution;
        return false;
    }

    out->voxel_size = voxel;
    out->band = voxel * kVoxelRemeshBandVoxels;
    const float pad = voxel * kVoxelRemeshPaddingVoxels;
    out->region = bounds.dilated(pad);

    // THE CEILING IS CHECKED IN DOUBLE, BEFORE ANYTHING IS CAST TO INT, and
    // that ordering is the whole point of it. The brick index and the marched
    // lattice are O(bounding box) even where the band is not, so they need a
    // ceiling — and a voxel size fine enough to overflow the int conversion
    // would reach it as undefined behaviour rather than as a refusal. A caller
    // asking for a 1e-9 voxel on a two-unit model is asking for 8e27 cells;
    // this is where it gets told no.
    const kernel::cfloat3 span = out->region.extent();
    const double dx = static_cast<double>(span.x) / voxel + 1.0;
    const double dy = static_cast<double>(span.y) / voxel + 1.0;
    const double dz = static_cast<double>(span.z) / voxel + 1.0;
    if (!(dx * dy * dz <= static_cast<double>(kMaxVoxelRemeshLatticeCells))) {
        *out_status = VoxelRemeshStatus::ExceedsBudget;
        return false;
    }

    const int cells[3] = {cells_for(span.x, voxel), cells_for(span.y, voxel),
                          cells_for(span.z, voxel)};
    for (int a = 0; a < 3; ++a)
        out->bcount[a] = floor_div(cells[a] - 1, field::kBrickDim) + 1;

    // ...and again on the rounded-out lattice the bricks actually span, which
    // is a little larger than the check above measured.
    if (out->lattice_cells() > kMaxVoxelRemeshLatticeCells) {
        *out_status = VoxelRemeshStatus::ExceedsBudget;
        return false;
    }
    return true;
}

std::vector<std::uint8_t> mark_active_bricks(const Mesh& source, const Lattice& lattice,
                                             parallel::CancelToken* token) {
    std::vector<std::uint8_t> active(static_cast<std::size_t>(lattice.brick_count()), 0);
    const std::size_t tris = source.triangle_count();
    const std::size_t vertices = source.positions.size();
    // A triangle's AABB dilated by the band: every sample within the band of
    // the triangle is inside it, so every brick the band could keep a sample
    // in meets it. The AABB is looser than the triangle, which costs marked
    // bricks that turn out empty and never costs a missing one.
    for (std::size_t t = 0; t < tris; ++t) {
        if ((t & 0xFFFFu) == 0 && parallel::cancelled(token)) return active;
        math::Aabb box;
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t vi = source.indices[t * 3 + static_cast<std::size_t>(c)];
            if (vi >= vertices) {
                ok = false;
                break;
            }
            box.expand(source.positions[vi]);
        }
        if (!ok || box.empty()) continue;
        const BrickRange r = bricks_meeting(box.dilated(lattice.band), lattice);
        if (r.empty()) continue;
        for (int bz = r.lo[2]; bz < r.hi[2]; ++bz)
            for (int by = r.lo[1]; by < r.hi[1]; ++by)
                for (int bx = r.lo[0]; bx < r.hi[0]; ++bx)
                    active[slot_of(bx, by, bz, lattice)] = 1;
    }
    return active;
}

std::uint64_t count_active(const std::vector<std::uint8_t>& active) {
    std::uint64_t n = 0;
    for (std::uint8_t a : active) n += a;
    return n;
}

field::FieldVolume sample_sparse(const Bvh& bvh, const Lattice& lattice,
                                 const std::vector<std::uint8_t>& active,
                                 parallel::CancelToken* token, bool* out_cancelled) {
    const std::vector<float> sign = region_signs(bvh, lattice, active, token);
    const float far = far_fill(lattice.band);
    // Half the diagonal of a brick's sample box: the furthest any of its
    // samples can be from its centre.
    const float half_diagonal = 0.5f * std::sqrt(3.0f) *
                                static_cast<float>(field::kBrickDim) * lattice.voxel_size;

    // The fill. Active bricks get the same query at the same positions the
    // dense path would have made, split across the pool because a signed
    // distance with a generalized winding number is pure and is the expensive
    // half of the whole operation. Inactive bricks get a constant.
    auto fill = [&](const field::FieldVolume::BrickGrid& grid, std::size_t first,
                    std::size_t count, float* out) {
        parallel::for_range(count, 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t s = b; s < e; ++s) {
                float* block = out + s * field::kBrickSamples;
                const std::size_t slot = first + s;
                if (!active[slot]) {
                    std::fill(block, block + field::kBrickSamples, sign[slot] * far);
                    continue;
                }
                // Checked per BRICK rather than per window: an active brick is
                // 729 winding-number queries, and a stage that only looked
                // between windows would hold a cancel for 512 of them.
                if (parallel::cancelled(token)) {
                    std::fill(block, block + field::kBrickSamples, far);
                    continue;
                }
                // A BRICK THAT CANNOT HOLD A BAND SAMPLE IS TWO QUERIES, NOT
                // 1458.
                //
                // The marking is conservative on purpose — a brick whose box
                // comes within the band of a TRIANGLE'S BOUNDING BOX, which is
                // looser than the triangle, and rounded out to whole bricks
                // besides. At longest-axis 128 over a sphere that marks 3404
                // bricks of which 1774 turn out to hold a sample worth
                // keeping; the other 1630 were evaluated in full and thrown
                // away.
                //
                // The centre answers for the whole brick. Every point of the
                // brick is within `half_diagonal` of it, so
                // `distance(centre) - half_diagonal` is a lower bound on the
                // distance from ANY of its samples to the surface: past the
                // band, none of them can be kept, no zero crossing can lie
                // inside, and the sign is therefore constant across the brick.
                // Bit-identical to evaluating all 729 — the brick stores
                // nothing either way, and `scan_block` reads the same
                // near_surface (false) and the same any_inside from the sign.
                const kernel::cfloat3 centre = brick_centre(slot, lattice);
                const float centre_distance = bvh.unsigned_distance(centre);
                if (centre_distance - half_diagonal > lattice.band) {
                    const float brick_sign = bvh.is_inside(centre) ? -1.0f : 1.0f;
                    std::fill(block, block + field::kBrickSamples, brick_sign * far);
                    continue;
                }

                // THE DISTANCE FIRST, THE SIGN ONLY WHERE IT IS NEEDED. What
                // the bound above cannot rule out, this rules out exactly: the
                // nearest-point query prunes hard against the tree, where the
                // generalized winding number descends it, so paying the
                // distances for a brick that turns out empty costs a fraction
                // of paying both.
                float nearest = 3.4e38f;
                for (int i = 0; i < field::kBrickSamples; ++i) {
                    block[i] = bvh.unsigned_distance(grid.sample_position(slot, i));
                    nearest = std::min(nearest, block[i]);
                }
                if (nearest > lattice.band) {
                    const float brick_sign = bvh.is_inside(centre) ? -1.0f : 1.0f;
                    std::fill(block, block + field::kBrickSamples, brick_sign * far);
                    continue;
                }
                for (int i = 0; i < field::kBrickSamples; ++i)
                    if (bvh.is_inside(grid.sample_position(slot, i))) block[i] = -block[i];
            }
        });
    };

    return field::FieldVolume::sample_blocks(fill, lattice.region, lattice.voxel_size,
                                             lattice.band, token, out_cancelled);
}

Mesh extract_surface(const field::FieldVolume& volume, const Lattice& lattice,
                     VoxelRemeshSurfaceMode mode) {
    if (volume.empty()) return {};
    const float spacing = volume.cell_size();
    const kernel::cfloat3 origin = volume.origin();
    const int nx = lattice.bcount[0] * field::kBrickDim;
    const int ny = lattice.bcount[1] * field::kBrickDim;
    const int nz = lattice.bcount[2] * field::kBrickDim;
    // A ring of out-of-range samples around the lattice, positive, so anything
    // that reaches the boundary is CLOSED against them rather than left open.
    // The region is padded by four voxels so nothing should get there; a mesher
    // that is watertight only when the padding was enough is not watertight.
    const float outside = volume.band() * 4.0f;

    auto sample = [&](int i, int j, int k) -> float {
        if (i < 0 || j < 0 || k < 0 || i > nx || j > ny || k > nz) return outside;
        // The STORED sample where there is one — exactly, not interpolated to
        // it. `eval` at a lattice point is trilinear with zero weights and
        // ought to agree, but "ought to" is not a contract the extraction
        // should rest on.
        if (const std::optional<float> s = volume.sample_at(i, j, k)) return *s;
        return volume.eval(origin + cf3(static_cast<float>(i), static_cast<float>(j),
                                        static_cast<float>(k)) *
                                        spacing);
    };

    int cell_min[3] = {-1, -1, -1};
    int cell_max[3] = {nx + 1, ny + 1, nz + 1};
    if (mode == VoxelRemeshSurfaceMode::Sharp)
        return mesh_lattice_dc(sample, cell_min, cell_max, origin, spacing);
    // Parallel: `sample` above is a pure read of an immutable volume, which is
    // that entry point's stated precondition, and it is byte-identical to the
    // serial march by construction.
    return mesh_lattice_parallel(sample, cell_min, cell_max, origin, spacing);
}

}  // namespace remesh_detail
}  // namespace mesh
}  // namespace clay
