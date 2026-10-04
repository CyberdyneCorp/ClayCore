// Move Topological — see include/clay/field/move_topological.h for why the
// weight is a solved grid rather than a closed form.

#include "clay/field/move_topological.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "clay/kernel/ease.h"
#include "clay/math/ease_slope.h"

namespace clay {
namespace field {

namespace {

using kernel::cf3;
using kernel::cfloat3;

constexpr float kUnreached = 1e30f;

// The geodesic field, solved once and then sampled per output point.
//
// Held on a plain dense grid rather than a FieldVolume: it is local by
// construction — the warp is the identity past the radius, so nothing further
// needs solving — and Dijkstra wants random access to neighbours, which a
// sparse brick index would only make slower.
struct Geodesic {
    math::Aabb box;
    float cell = 0.0f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> distance;  // kUnreached where unreached

    int index(int x, int y, int z) const { return (z * ny + y) * nx + x; }
    bool inside(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
    }
    cfloat3 centre_of(int x, int y, int z) const {
        return cf3(box.min.x + (static_cast<float>(x) + 0.5f) * cell,
                   box.min.y + (static_cast<float>(y) + 0.5f) * cell,
                   box.min.z + (static_cast<float>(z) + 0.5f) * cell);
    }

    // TRILINEAR, and unreached cells are filled with the radius before sampling
    // so that "no weight" is a finite value the interpolation can blend toward.
    //
    // Nearest-cell was tried and is unusable: it makes the warp piecewise
    // constant, so the field steps at every cell boundary. Measured, that
    // declared a Lipschitz of 14.66 and a step scale of 0.039 — the geometry
    // was correct and the raymarcher could not resolve it, which renders as an
    // empty frame.
    float sample_at(int x, int y, int z) const {
        if (!inside(x, y, z)) return outside_value;
        return distance[index(x, y, z)];
    }
    float at(cfloat3 p) const {
        const float fx = (p.x - box.min.x) / cell - 0.5f;
        const float fy = (p.y - box.min.y) / cell - 0.5f;
        const float fz = (p.z - box.min.z) / cell - 0.5f;
        const int x = static_cast<int>(std::floor(fx));
        const int y = static_cast<int>(std::floor(fy));
        const int z = static_cast<int>(std::floor(fz));
        const float tx = fx - static_cast<float>(x);
        const float ty = fy - static_cast<float>(y);
        const float tz = fz - static_cast<float>(z);
        auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
        const float c00 = mix(sample_at(x, y, z), sample_at(x + 1, y, z), tx);
        const float c10 = mix(sample_at(x, y + 1, z), sample_at(x + 1, y + 1, z), tx);
        const float c01 = mix(sample_at(x, y, z + 1), sample_at(x + 1, y, z + 1), tx);
        const float c11 = mix(sample_at(x, y + 1, z + 1), sample_at(x + 1, y + 1, z + 1), tx);
        return mix(mix(c00, c10, ty), mix(c01, c11, ty), tz);
    }

    float outside_value = 0.0f;  // the radius: "no weight". Set in solve().
};

struct Node {
    float distance;
    int cell;
    bool operator>(const Node& other) const { return distance > other.distance; }
};

// Solve geodesic distance from the anchor over cells the source calls material.
//
// The graph is the MATERIAL, which is the whole point: free space is not in it,
// so the distance cannot step across a gap however narrow the gap is.
// The lattice the walk runs on, sized to the reach. Split out so that filling
// its material array -- the ONLY place the walk asks the source anything -- can
// be done a point at a time or a batch at a time without the graph code
// existing twice.
Geodesic make_grid(const TopologicalMoveSettings& settings, float cell_size) {
    Geodesic g;
    g.cell = cell_size;
    // Set before anything can return. A sample point OUTSIDE the solved box has
    // no weight, and the value standing for that is the radius; leaving it zero
    // meant "full weight" everywhere beyond the box, so an anchor placed away
    // from the material warped the entire document by the full displacement.
    g.outside_value = settings.radius;

    // Sized to the reach. A geodesic path of length r cannot leave a ball of
    // radius r, and the displacement moves the sample point by at most |d|, so
    // that plus a few cells of margin bounds everything the warp can touch.
    const float reach = settings.radius + kernel::clength(settings.displacement) +
                        4.0f * cell_size;
    const cfloat3 half = cf3(reach, reach, reach);
    g.box = math::Aabb{settings.anchor - half, settings.anchor + half};
    g.nx = std::max(1, static_cast<int>(std::ceil(g.box.extent().x / cell_size)));
    g.ny = std::max(1, static_cast<int>(std::ceil(g.box.extent().y / cell_size)));
    g.nz = std::max(1, static_cast<int>(std::ceil(g.box.extent().z / cell_size)));
    g.distance.assign(static_cast<std::size_t>(g.nx) * g.ny * g.nz, kUnreached);
    return g;
}

// The walk, over a material array the caller filled. `material` is the graph:
// free space is not in it, so the distance cannot step across a gap however
// narrow the gap is.
Geodesic solve_over(Geodesic g, const std::vector<bool>& material,
                    const TopologicalMoveSettings& settings, float cell_size) {
    // Seed from the material cell nearest the anchor. The anchor is a picked
    // SURFACE point, so it usually lands a hair outside the material; seeding
    // the nearest cell inside is what makes it behave as the user aimed.
    int seed = -1;
    float best = kUnreached;
    for (int i = 0; i < static_cast<int>(material.size()); ++i) {
        if (!material[i]) continue;
        const int x = i % g.nx, y = (i / g.nx) % g.ny, z = i / (g.nx * g.ny);
        const float d = kernel::clength(g.centre_of(x, y, z) - settings.anchor);
        if (d < best) {
            best = d;
            seed = i;
        }
    }
    if (seed < 0 || best > settings.radius) return g;  // nothing within reach

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    g.distance[seed] = 0.0f;
    open.push({0.0f, seed});
    while (!open.empty()) {
        const Node node = open.top();
        open.pop();
        if (node.distance > g.distance[node.cell]) continue;  // stale entry
        if (node.distance > settings.radius) continue;        // past the reach
        const int x = node.cell % g.nx, y = (node.cell / g.nx) % g.ny,
                  z = node.cell / (g.nx * g.ny);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy && !dz) continue;
                    const int ax = x + dx, ay = y + dy, az = z + dz;
                    if (!g.inside(ax, ay, az)) continue;
                    const int next = g.index(ax, ay, az);
                    if (!material[next]) continue;  // the graph IS the material
                    // The true step length, so a diagonal costs what it is
                    // worth: counting steps would make distance depend on the
                    // grid's orientation rather than on the form.
                    const float step =
                        cell_size * std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));
                    const float through = node.distance + step;
                    if (through < g.distance[next]) {
                        g.distance[next] = through;
                        open.push({through, next});
                    }
                }
    }

    // Extend outward into free space. The warp acts on SPACE: an output point
    // at p takes its material from p - w*d, so a point up to |d| OUTSIDE the
    // original surface still needs a weight, or the material never arrives
    // there and the moved part is torn off at its own boundary. Measured, a
    // fixed three-cell shell against a drag of 0.25 left the grabbed finger
    // 0.044 wide where it started at 0.204.
    //
    // The shell carries the geodesic value of the nearest reached material
    // rather than growing it, so the weight is constant across the shell and
    // the material translates rigidly instead of shearing as it leaves.
    const float outward = kernel::clength(settings.displacement) + 2.0f * cell_size;
    // ...and only ALONG THE DRAG, carrying a CONSTANT value. Both were found by
    // measuring alternatives that look more principled and are wrong:
    //
    //  - grown omnidirectionally as a true distance, the extension is a
    //    Euclidean distance from the reached set, so it hands a large weight to
    //    the gap between two fingers and drags the far one — the exact failure
    //    this operation exists to avoid, measured as the far finger's edge
    //    moving from +0.158 to +0.088.
    //  - grown as a distance rather than carried flat, the material shears as it
    //    leaves instead of translating: the grabbed finger came out 0.136 wide
    //    where it started at 0.204.
    //
    // Constant and directional keeps the translation rigid and the weight out of
    // the gap. The cost is a step in the weight where the shell ends, which the
    // trilinear sampling above softens.
    const cfloat3 pull = settings.displacement * (1.0f / kernel::clength(settings.displacement));
    std::vector<float> carried = g.distance;  // payload: g of the nearest material
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> shell;
    for (int i = 0; i < static_cast<int>(g.distance.size()); ++i)
        if (g.distance[i] < kUnreached) shell.push({0.0f, i});  // key: distance OUT

    std::vector<float> out_dist(g.distance.size(), kUnreached);
    for (int i = 0; i < static_cast<int>(g.distance.size()); ++i)
        if (g.distance[i] < kUnreached) out_dist[i] = 0.0f;

    while (!shell.empty()) {
        const Node node = shell.top();
        shell.pop();
        if (node.distance > out_dist[node.cell]) continue;
        if (node.distance > outward) continue;
        const int x = node.cell % g.nx, y = (node.cell / g.nx) % g.ny,
                  z = node.cell / (g.nx * g.ny);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy && !dz) continue;
                    const int ax = x + dx, ay = y + dy, az = z + dz;
                    if (!g.inside(ax, ay, az)) continue;
                    const int next = g.index(ax, ay, az);
                    if (g.distance[next] < kUnreached) continue;  // already material
                    const cfloat3 dir = cf3(static_cast<float>(dx), static_cast<float>(dy),
                                            static_cast<float>(dz));
                    if (kernel::cdot(dir, pull) <= 0.0f) continue;  // only along the drag
                    const float step =
                        cell_size * std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));
                    const float through = node.distance + step;
                    if (through <= outward && through < out_dist[next]) {
                        out_dist[next] = through;
                        carried[next] = carried[node.cell];
                        shell.push({through, next});
                    }
                }
    }
    g.distance.swap(carried);

    // "Unreached" becomes the radius, which is exactly no weight — a finite
    // value the interpolation above can blend toward, instead of a sentinel
    // that would drag every neighbouring sample to infinity.
    for (float& d : g.distance)
        if (d >= kUnreached) d = settings.radius;
    return g;
}

// Where one output sample takes its material from, through ONE step. The
// identity past the reach, and the pulled-back point inside it.
cfloat3 pull_back(const Geodesic& geo, const TopologicalMoveSettings& settings, cfloat3 p) {
    const float g = geo.at(p);
    if (g >= settings.radius) return p;  // past the reach: identity
    const float t = kernel::cclamp(1.0f - g / settings.radius, 0.0f, 1.0f);
    const float w = kernel::cease(settings.ease, t);
    // The same displacement map grab uses, with geodesic distance in place of
    // the straight line. Sampling the source at the pulled-back point is what
    // moves the material to p.
    return p - settings.displacement * w;
}

// HOW LONG ONE STEP MAY BE, as a fraction of the reach (#657).
//
// One step's map p -> p - d * w(g(p)) is one-to-one only while |d| times the
// weight's slope stays under one, and the weight's slope is the curve's
// steepest |E'| over the radius. Past that two output points read the same
// source point: measured on a unit sphere, a 0.64 drag at radius 0.3 left the
// anchor at 0.940 against an original 1.000, with a crater under the grip and
// a lump that fell away before the end of the drag -- returned as CLAY_OK.
//
// Half the fold limit, not the limit itself: the geodesic is solved on a
// 26-neighbour lattice and sampled trilinearly, so its own slope runs a little
// over one, and a step sized exactly to the bound would sit on the edge of the
// fold it exists to avoid. n = 1 for any drag under half the radius on a
// linear curve, which keeps every such drag bit-identical to the single step.
constexpr float kStepReach = 0.5f;

// The most steps one call will take. Each costs a geodesic solve over a grid
// sized to the reach plus its slice, and every output sample composes all of
// them; past this the drag is over thirty radii long on a linear curve, and is
// not one gesture. Beyond it the slices are longer than kStepReach allows and
// the map may fold again -- stated in the header rather than refused.
constexpr int kMaxSteps = 64;

// The drag as the chain of steps it runs as.
//
// Step i drags d/n from where the grip has reached, anchor + (i-1) * d/n, and
// its geodesic is solved over the material the steps before it left -- which
// is exactly what a host splitting the drag into n calls gets. What it does not
// do is re-sample the volume n times: an output point's source position is
// pb_1(pb_2(...pb_n(p))), and the source is read ONCE, there.
struct MoveChain {
    std::vector<TopologicalMoveSettings> steps;
    std::vector<Geodesic> solved;  // steps[0 .. solved.size()) are solved

    explicit MoveChain(const TopologicalMoveSettings& settings) {
        const int n = topological_move_steps(settings);
        // d * 1.0f is d exactly, and anchor + 0 is the anchor, so a one-step
        // chain is the single step it replaced to the bit.
        const cfloat3 slice = settings.displacement * (1.0f / static_cast<float>(n));
        for (int i = 0; i < n; ++i) {
            TopologicalMoveSettings step = settings;
            step.anchor = settings.anchor + slice * static_cast<float>(i);
            step.displacement = slice;
            steps.push_back(step);
        }
    }

    // Through every step solved so far, LAST FIRST: the output point is where
    // the final step put material, so it is undone first.
    cfloat3 pull_back(cfloat3 p) const {
        for (std::size_t i = solved.size(); i-- > 0;)
            p = field::pull_back(solved[i], steps[i], p);
        return p;
    }

    const TopologicalMoveSettings& next() const { return steps[solved.size()]; }
    bool complete() const { return solved.size() == steps.size(); }
};

// The two ways to fill a step's material array, over the same grid and the
// same walk. Sampled once per cell either way, at the cell's position pulled
// back through the steps already solved -- the material as those steps left
// it. The batched one asks for every cell at once, which is what lets an
// evaluator compile a tape once and spread the cells across a pool.
void solve_next(const std::function<float(cfloat3)>& source, MoveChain& chain,
                float cell_size) {
    Geodesic g = make_grid(chain.next(), cell_size);
    std::vector<bool> material(g.distance.size(), false);
    for (int z = 0; z < g.nz; ++z)
        for (int y = 0; y < g.ny; ++y)
            for (int x = 0; x < g.nx; ++x)
                material[g.index(x, y, z)] =
                    source(chain.pull_back(g.centre_of(x, y, z))) <= 0.0f;
    chain.solved.push_back(solve_over(std::move(g), material, chain.next(), cell_size));
}

void solve_next(const PointBatch& source, MoveChain& chain, float cell_size) {
    Geodesic g = make_grid(chain.next(), cell_size);
    const std::size_t n = g.distance.size();
    std::vector<float> points(n * 3);
    for (int z = 0; z < g.nz; ++z)
        for (int y = 0; y < g.ny; ++y)
            for (int x = 0; x < g.nx; ++x) {
                const cfloat3 c = chain.pull_back(g.centre_of(x, y, z));
                const std::size_t at = static_cast<std::size_t>(g.index(x, y, z)) * 3;
                points[at] = c.x;
                points[at + 1] = c.y;
                points[at + 2] = c.z;
            }
    std::vector<float> d(n, 0.0f);
    source(points.data(), n, d.data());
    std::vector<bool> material(n, false);
    for (std::size_t i = 0; i < n; ++i) material[i] = d[i] <= 0.0f;
    chain.solved.push_back(solve_over(std::move(g), material, chain.next(), cell_size));
}

template <typename Source>
MoveChain solve_chain(const Source& source, const TopologicalMoveSettings& settings,
                      float cell_size) {
    MoveChain chain(settings);
    while (!chain.complete()) solve_next(source, chain, cell_size);
    return chain;
}

}  // namespace

FieldVolume move_topological(const std::function<float(cfloat3)>& source,
                             const math::Aabb& region, float cell_size, float band,
                             const TopologicalMoveSettings& settings) {
    // A drag that touches nothing is not an error, so each of these hands back
    // the source unchanged rather than refusing.
    if (!(settings.radius > 0.0f) || !(cell_size > 0.0f) ||
        kernel::clength(settings.displacement) <= 0.0f)
        return FieldVolume::sample(source, region, cell_size, band);

    const MoveChain chain = solve_chain(source, settings, cell_size);

    FieldVolume out = FieldVolume::sample(
        [&source, &chain](cfloat3 p) { return source(chain.pull_back(p)); }, region, cell_size,
        band);

    // Measured, as flatten's is. A weight that varies along the surface can
    // steepen the field, and by how much depends on the form rather than on any
    // envelope that could be written down in advance.
    out.set_sample_lipschitz(out.measure_sample_lipschitz());
    return out;
}

FieldVolume move_topological(const PointBatch& source, const math::Aabb& region,
                             float cell_size, float band,
                             const TopologicalMoveSettings& settings) {
    // A drag that touches nothing is not an error, so each of these hands back
    // the source unchanged rather than refusing.
    if (!(settings.radius > 0.0f) || !(cell_size > 0.0f) ||
        kernel::clength(settings.displacement) <= 0.0f)
        return FieldVolume::sample_blocks(
            [&source](const FieldVolume::BrickGrid& grid, std::size_t first, std::size_t count,
                      float* out) {
                const std::size_t n = count * kBrickSamples;
                std::vector<float> points(n * 3);
                for (std::size_t s = 0; s < count; ++s)
                    for (int i = 0; i < kBrickSamples; ++i) {
                        const cfloat3 p = grid.sample_position(first + s, i);
                        const std::size_t at = (s * kBrickSamples + static_cast<std::size_t>(i)) * 3;
                        points[at] = p.x;
                        points[at + 1] = p.y;
                        points[at + 2] = p.z;
                    }
                source(points.data(), n, out);
            },
            region, cell_size, band);

    const MoveChain chain = solve_chain(source, settings, cell_size);

    // The query positions are the PULLED-BACK points, not the lattice, so the
    // window builds them and hands the whole window over at once.
    std::vector<float> points;
    FieldVolume out = FieldVolume::sample_blocks(
        [&source, &chain, &points](const FieldVolume::BrickGrid& grid, std::size_t first,
                                   std::size_t count, float* block) {
            const std::size_t n = count * kBrickSamples;
            points.resize(n * 3);
            for (std::size_t s = 0; s < count; ++s)
                for (int i = 0; i < kBrickSamples; ++i) {
                    const cfloat3 q = chain.pull_back(grid.sample_position(first + s, i));
                    const std::size_t at = (s * kBrickSamples + static_cast<std::size_t>(i)) * 3;
                    points[at] = q.x;
                    points[at + 1] = q.y;
                    points[at + 2] = q.z;
                }
            source(points.data(), n, block);
        },
        region, cell_size, band);

    // Measured, as flatten's is. A weight that varies along the surface can
    // steepen the field, and by how much depends on the form rather than on any
    // envelope that could be written down in advance.
    out.set_sample_lipschitz(out.measure_sample_lipschitz());
    return out;
}

int topological_move_steps(const TopologicalMoveSettings& settings) {
    if (!(settings.radius > 0.0f)) return 1;
    const float reach = kStepReach * settings.radius;
    const float wanted = std::ceil(kernel::clength(settings.displacement) *
                                   math::ease_max_slope(settings.ease) / reach);
    if (!(wanted > 1.0f)) return 1;
    return static_cast<int>(std::min(wanted, static_cast<float>(kMaxSteps)));
}

FieldVolume move_topological(const FieldVolume& v, const TopologicalMoveSettings& settings) {
    if (v.empty()) return v;
    // The free functions above take a callable and a region, so they have no
    // volume to inherit from: bounds, cell size and band are passed explicitly
    // and the FEATHER was simply dropped. A caller setting it on the source got
    // a hard-edged result and no diagnostic, because the value was not ignored
    // by a rule -- it was never carried.
    //
    // Measured by a host on a verified-clean sphere, in a metric that sees a
    // shading defect rather than a displacement one: 1.756x of the reference
    // roughness at one cell, 2.064x at the three this ships, 5.274x at twelve.
    // The displacement itself was correct throughout (0.3832 of a 0.4 pull), so
    // what they saw was a hard box of visible lattice around a move that had
    // worked.
    FieldVolume out = move_topological([&v](cfloat3 p) { return v.eval(p); }, v.bounds(),
                                       v.cell_size(), v.band(), settings);
    out.set_feather(v.feather());
    return out;
}

}  // namespace field
}  // namespace clay
