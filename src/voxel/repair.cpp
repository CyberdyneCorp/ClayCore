// Voxel repair (voxel-engine spec, add-voxel-repair): close perforations and
// fill enclosed voids, so a sculpted layer can be made airtight before a bake.
//
// Worth stating what "not airtight" means here, because two obvious readings
// are already true by construction and neither is the problem. An SDF layer is
// watertight because a field has no holes, and a voxel grid's greedy mesh is
// closed because every face between an occupied and an empty cell is emitted.
// What is real is narrower: a shell with single-cell perforations lets the
// outside reach the inside, and enclosed pockets of empty cells survive into
// the bake as interior surface nobody asked for.
//
// Enclosure is DECIDED, not guessed at. A local neighbourhood cannot tell an
// enclosed pocket from a deep dent, so this floods empty cells inward from
// outside the occupied bounds; whatever the flood cannot reach is enclosed.

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <vector>

#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace voxel {

namespace {

// A dense inside/outside window over the occupied bounds, padded by one cell
// so the flood has somewhere outside to start from.
struct Window {
    VoxelCoord lo{}, hi{};
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> occupied;

    std::size_t index(int x, int y, int z) const {
        return static_cast<std::size_t>(x - lo.x) +
               static_cast<std::size_t>(nx) *
                   (static_cast<std::size_t>(y - lo.y) +
                    static_cast<std::size_t>(ny) * static_cast<std::size_t>(z - lo.z));
    }
    bool contains(int x, int y, int z) const {
        return x >= lo.x && y >= lo.y && z >= lo.z && x <= hi.x && y <= hi.y && z <= hi.z;
    }
};

bool build_window(const VoxelGrid& g, int pad, Window* out) {
    std::optional<VoxelCoord> lo = g.bounds_min();
    std::optional<VoxelCoord> hi = g.bounds_max();
    if (!lo || !hi) return false;  // an empty grid has nothing to enclose
    out->lo = {lo->x - pad, lo->y - pad, lo->z - pad};
    out->hi = {hi->x + pad, hi->y + pad, hi->z + pad};
    out->nx = out->hi.x - out->lo.x + 1;
    out->ny = out->hi.y - out->lo.y + 1;
    out->nz = out->hi.z - out->lo.z + 1;
    out->occupied.assign(static_cast<std::size_t>(out->nx) * out->ny * out->nz, 0);
    for (int z = out->lo.z; z <= out->hi.z; ++z)
        for (int y = out->lo.y; y <= out->hi.y; ++y)
            for (int x = out->lo.x; x <= out->hi.x; ++x)
                out->occupied[out->index(x, y, z)] = g.get({x, y, z}) != 0 ? 1 : 0;
    return true;
}

// Empty cells reachable from outside, by face adjacency. Seeded from the
// window's whole border, which is guaranteed empty because the window is
// padded past the occupied bounds.
std::vector<std::uint8_t> flood_exterior(const Window& w) {
    std::vector<std::uint8_t> outside(w.occupied.size(), 0);
    std::deque<VoxelCoord> queue;
    auto push = [&](int x, int y, int z) {
        if (!w.contains(x, y, z)) return;
        std::size_t i = w.index(x, y, z);
        if (w.occupied[i] || outside[i]) return;
        outside[i] = 1;
        queue.push_back({x, y, z});
    };
    for (int z = w.lo.z; z <= w.hi.z; ++z)
        for (int y = w.lo.y; y <= w.hi.y; ++y)
            for (int x = w.lo.x; x <= w.hi.x; ++x)
                if (x == w.lo.x || x == w.hi.x || y == w.lo.y || y == w.hi.y || z == w.lo.z ||
                    z == w.hi.z)
                    push(x, y, z);

    while (!queue.empty()) {
        VoxelCoord c = queue.front();
        queue.pop_front();
        push(c.x + 1, c.y, c.z);
        push(c.x - 1, c.y, c.z);
        push(c.x, c.y + 1, c.z);
        push(c.x, c.y - 1, c.z);
        push(c.x, c.y, c.z + 1);
        push(c.x, c.y, c.z - 1);
    }
    return outside;
}

bool fully_masked(const MaskField* mask, VoxelCoord c, float voxel_size) {
    if (!mask) return false;
    return mask->sample(kernel::cf3((c.x + 0.5f) * voxel_size, (c.y + 0.5f) * voxel_size,
                                    (c.z + 0.5f) * voxel_size)) >= 1.0f;
}

// The colour of the shell around a void: whatever its occupied neighbours are,
// so a filled void does not appear as an arbitrary palette entry inside an
// otherwise uniform object.
std::uint8_t enclosing_colour(const VoxelGrid& g, VoxelCoord c) {
    int counts[256] = {};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy && !dz) continue;
                std::uint8_t v = g.get({c.x + dx, c.y + dy, c.z + dz});
                if (v != 0) ++counts[v];
            }
    int best = 0, best_count = 0;
    for (int i = 1; i < 256; ++i)
        if (counts[i] > best_count) {
            best_count = counts[i];
            best = i;
        }
    return static_cast<std::uint8_t>(best);
}

}  // namespace

VoxelGrid::RepairReport VoxelGrid::repair_report() const {
    RepairReport report;
    Window w;
    if (!build_window(*this, 1, &w)) {
        report.airtight = true;  // nothing occupied encloses nothing
        return report;
    }
    std::vector<std::uint8_t> outside = flood_exterior(w);

    // Count the enclosed regions separately rather than only their total, so a
    // caller can tell one big hollow from a hundred pinholes — they call for
    // different fixes.
    std::vector<std::uint8_t> seen(w.occupied.size(), 0);
    for (int z = w.lo.z; z <= w.hi.z; ++z)
        for (int y = w.lo.y; y <= w.hi.y; ++y)
            for (int x = w.lo.x; x <= w.hi.x; ++x) {
                std::size_t start = w.index(x, y, z);
                if (w.occupied[start] || outside[start] || seen[start]) continue;
                std::size_t size = 0;
                std::deque<VoxelCoord> queue{{x, y, z}};
                seen[start] = 1;
                while (!queue.empty()) {
                    VoxelCoord c = queue.front();
                    queue.pop_front();
                    ++size;
                    const int dx[6] = {1, -1, 0, 0, 0, 0};
                    const int dy[6] = {0, 0, 1, -1, 0, 0};
                    const int dz[6] = {0, 0, 0, 0, 1, -1};
                    for (int i = 0; i < 6; ++i) {
                        int nx2 = c.x + dx[i], ny2 = c.y + dy[i], nz2 = c.z + dz[i];
                        if (!w.contains(nx2, ny2, nz2)) continue;
                        std::size_t n = w.index(nx2, ny2, nz2);
                        if (w.occupied[n] || outside[n] || seen[n]) continue;
                        seen[n] = 1;
                        queue.push_back({nx2, ny2, nz2});
                    }
                }
                ++report.enclosed_voids;
                report.void_cells += size;
                report.largest_void = std::max(report.largest_void, size);
            }
    report.airtight = report.enclosed_voids == 0;
    return report;
}

void VoxelGrid::repair_close_holes(int passes, const MaskField* mask) {
    if (passes < 1) return;
    Window w;
    if (!build_window(*this, passes + 1, &w)) return;

    // The same rule the fill-cavities verb uses, over the whole grid: an empty
    // cell with at least four of its six face neighbours occupied is a
    // perforation, not an opening. A closing was the first attempt and cannot
    // do this — the erosion reaches through from the void behind a one-cell
    // wall and reopens every hole it just sealed.
    const int kPocketNeighbours = 4;
    std::vector<std::uint8_t> filled = w.occupied;
    for (int step = 0; step < passes; ++step) {
        std::vector<std::uint8_t> next = filled;
        for (int z = w.lo.z + 1; z < w.hi.z; ++z)
            for (int y = w.lo.y + 1; y < w.hi.y; ++y)
                for (int x = w.lo.x + 1; x < w.hi.x; ++x) {
                    std::size_t i = w.index(x, y, z);
                    if (filled[i]) continue;
                    int n = filled[w.index(x + 1, y, z)] + filled[w.index(x - 1, y, z)] +
                            filled[w.index(x, y + 1, z)] + filled[w.index(x, y - 1, z)] +
                            filled[w.index(x, y, z + 1)] + filled[w.index(x, y, z - 1)];
                    if (n >= kPocketNeighbours) next[i] = 1;
                }
        filled = std::move(next);
    }

    for (int z = w.lo.z; z <= w.hi.z; ++z)
        for (int y = w.lo.y; y <= w.hi.y; ++y)
            for (int x = w.lo.x; x <= w.hi.x; ++x) {
                std::size_t i = w.index(x, y, z);
                if (!filled[i] || w.occupied[i]) continue;  // additions only
                VoxelCoord c{x, y, z};
                if (fully_masked(mask, c, voxel_size_)) continue;
                set(c, enclosing_colour(*this, c));
            }
}

void VoxelGrid::repair_fill_voids(const MaskField* mask) {
    Window w;
    if (!build_window(*this, 1, &w)) return;
    std::vector<std::uint8_t> outside = flood_exterior(w);

    // Collected before writing: filling as we walk would let a freshly filled
    // cell supply the colour for its neighbour, so a large void would take its
    // colour from one corner rather than from the shell around it.
    std::vector<VoxelCoord> fill;
    for (int z = w.lo.z; z <= w.hi.z; ++z)
        for (int y = w.lo.y; y <= w.hi.y; ++y)
            for (int x = w.lo.x; x <= w.hi.x; ++x) {
                std::size_t i = w.index(x, y, z);
                if (w.occupied[i] || outside[i]) continue;
                VoxelCoord c{x, y, z};
                if (fully_masked(mask, c, voxel_size_)) continue;
                fill.push_back(c);
            }

    std::vector<std::uint8_t> colours;
    colours.reserve(fill.size());
    for (VoxelCoord c : fill) colours.push_back(enclosing_colour(*this, c));
    // A cell deep inside a void has no occupied neighbour at all, so it takes
    // the colour of whatever the shell gave the rest — resolved by a second
    // pass rather than left as the empty index, which would fill nothing.
    std::uint8_t fallback = 0;
    for (std::uint8_t v : colours)
        if (v != 0) {
            fallback = v;
            break;
        }
    if (fallback == 0) fallback = palette_size() > 1 ? 1 : palette_add(kernel::cf3(0.5f, 0.5f, 0.5f));
    for (std::size_t i = 0; i < fill.size(); ++i)
        set(fill[i], colours[i] != 0 ? colours[i] : fallback);
}

}  // namespace voxel
}  // namespace clay
