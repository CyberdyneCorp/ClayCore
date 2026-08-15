// Performance benchmarks with CI regression gates (build-packaging spec):
// points/sec, bricks/sec, mesh time on fixed scenes. tools/check_bench.py
// enforces generous floor thresholds in CI — they catch order-of-magnitude
// regressions; tight deltas need dedicated hardware.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "clay.h"
#include "clay/brick/cache.h"
#include "clay/eval/backend.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/surface_nets.h"
#include "clay/field/redistance.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/eval/bake_points.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"

using namespace clay;
using kernel::cf3;

namespace {

// fixed benchmark scene: 12 items with blends, mirror, subtract
scene::Document bench_document() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("bench");
    l.mirror_axes = scene::kMirrorX;
    l.mirror_k = 0.05f;
    auto add = [&](scene::Prim prim, kernel::cfloat3 pos, scene::Op op, float k) {
        scene::Node n;
        n.prim = prim;
        n.xform.position = pos;
        n.op = op;
        n.blend = scene::Blend{k > 0 ? scene::BlendProfile::Quadratic : scene::BlendProfile::Hard,
                               k};
        l.sdf->insert(n);
    };
    add(scene::Prim::sphere(0.8f), cf3(0, 0, 0), scene::Op::Add, 0);
    add(scene::Prim::box(cf3(0.5f, 0.4f, 0.6f)), cf3(0.6f, 0.3f, 0), scene::Op::Add, 0.1f);
    add(scene::Prim::torus(0.7f, 0.15f), cf3(0, 0.8f, 0), scene::Op::Add, 0.08f);
    add(scene::Prim::capped_cylinder(0.3f, 0.9f), cf3(-0.4f, 0.4f, 0), scene::Op::Subtract,
        0.05f);
    add(scene::Prim::round_cone(0.3f, 0.12f, 0.8f), cf3(0.8f, -0.4f, 0.2f), scene::Op::Add,
        0.12f);
    add(scene::Prim::octahedron(0.5f), cf3(-0.7f, -0.5f, 0.3f), scene::Op::Add, 0.07f);
    add(scene::Prim::hex_prism(0.35f, 0.25f), cf3(0.2f, -0.8f, -0.4f), scene::Op::Add, 0.06f);
    add(scene::Prim::capsule(cf3(-0.5f, 0, 0), cf3(0.5f, 0.4f, 0.3f), 0.2f), cf3(0, 0, 0.7f),
        scene::Op::Add, 0.09f);
    add(scene::Prim::ellipsoid(cf3(0.4f, 0.25f, 0.3f)), cf3(0, -0.3f, -0.8f), scene::Op::Add,
        0.05f);
    add(scene::Prim::round_box(cf3(0.3f, 0.3f, 0.3f), 0.05f), cf3(-0.8f, 0.6f, -0.3f),
        scene::Op::Add, 0.1f);
    add(scene::Prim::capped_cone(0.4f, 0.35f, 0.15f), cf3(0.5f, 0.9f, 0.4f), scene::Op::Add,
        0.08f);
    add(scene::Prim::sphere(0.25f), cf3(0, 0.2f, 0.9f), scene::Op::Subtract, 0.06f);
    return doc;
}

std::vector<float> random_points(std::size_t count, float extent) {
    std::vector<float> pts(count * 3);
    std::uint64_t state = 12345;
    for (float& v : pts) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        v = (static_cast<float>((state >> 40) & 0xFFFFFF) / 16777216.0f * 2.0f - 1.0f) * extent;
    }
    return pts;
}

void BM_EvalPoints(benchmark::State& state) {
    scene::Document doc = bench_document();
    scene::Tape tape = scene::compile_document(doc);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    const std::size_t n = 100000;
    std::vector<float> pts = random_points(n, 2.0f);
    std::vector<float> dists(n);
    for (auto _ : state) {
        eval::PointQuery q{pts.data(), n, 1e-4f};
        eval::PointResults out{dists.data(), nullptr, nullptr};
        cpu->eval_points(tape, q, out);
        benchmark::DoNotOptimize(dists.data());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * n);
}
BENCHMARK(BM_EvalPoints)->Unit(benchmark::kMillisecond);

void BM_BrickFill(benchmark::State& state) {
    scene::Document doc = bench_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    std::int64_t bricks = 0;
    for (auto _ : state) {
        brick::BrickCache cache(brick::BrickConfig{8, 0.05f, 3, 0});
        cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
        for (const brick::BrickRequest& req : cache.take_dirty()) {
            scene::CullRegion cull{cache.cull_region(req.key)};
            scene::Tape tape = scene::compile_document(doc, &cull);
            std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                      req.grid.nz);
            cpu->eval_grid(tape, req.grid, values.data());
            cache.submit(req, values.data());
            ++bricks;
        }
        benchmark::DoNotOptimize(cache.memory_usage());
    }
    state.SetItemsProcessed(bricks);
}
BENCHMARK(BM_BrickFill)->Unit(benchmark::kMillisecond);

// Issue #43 item 1's claim, measured rather than asserted: without a key list
// a host has to re-mesh the WHOLE surface to see one brush dab, and "on a 2.3M
// triangle bust that is not a 50 ms operation". These two benchmarks are the
// same cache and the same mesher, differing only in how many bricks are
// marched — the whole surface against the handful a dab dirties. Their ratio is
// what subset meshing buys, and check_bench.py gates the direction of it.
namespace {

brick::BrickCache filled_cache(const scene::Document& doc) {
    brick::BrickCache cache(brick::BrickConfig{8, 0.05f, 3, 0});
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape tape = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        cpu->eval_grid(tape, req.grid, values.data());
        cache.submit(req, values.data());
    }
    return cache;
}

}  // namespace

void BM_MeshBricksWhole(benchmark::State& state) {
    scene::Document doc = bench_document();
    brick::BrickCache cache = filled_cache(doc);
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_bricks(cache, nullptr, {});
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["bricks"] = static_cast<double>(cache.surface_bricks().size());
}
BENCHMARK(BM_MeshBricksWhole)->Unit(benchmark::kMillisecond);

// What a dab's worth of dirty bricks costs — 8 of them, the scale
// clay_brick_cache_take_dirty reports after a small edit.
void BM_MeshBricksSubset(benchmark::State& state) {
    scene::Document doc = bench_document();
    brick::BrickCache cache = filled_cache(doc);
    std::vector<brick::BrickKey> all = cache.surface_bricks();
    std::vector<brick::BrickKey> dab(all.begin(),
                                     all.begin() + std::min<std::size_t>(8, all.size()));
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_bricks(cache, nullptr, {}, &dab);
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["bricks"] = static_cast<double>(dab.size());
}
BENCHMARK(BM_MeshBricksSubset)->Unit(benchmark::kMillisecond);

// Issue #73: gradient normals over a FIXED brick set must cost what the bricks
// cost, not what the document does. Same sphere, same 80-brick subset at the +x
// pole, gradient normals on — against a document holding 1 node and against one
// holding 193, the extra 192 dabs at the -x pole, two world units from every
// measured brick. Before the per-brick culled attribute pass the grown document
// was ~18x the fresh one (4.8 ms -> 120 ms in the report); check_bench.py gates
// the ratio.
namespace {

scene::Document sculpted_sphere(int nodes) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("sculpt");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i < nodes; ++i) {
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.04f);
        const float a = 0.3f * std::sin(static_cast<float>(i) * 0.7f);
        const float b = 0.3f * std::cos(static_cast<float>(i) * 1.3f);
        const float x = -std::sqrt(std::max(0.0f, 1.0f - a * a - b * b));
        dab.xform.position = cf3(x, a, b);
        l.sdf->insert(dab);
    }
    return doc;
}

// The 80 surface bricks nearest the +x pole: a fixed re-mesh region on the far
// side of the sphere from every dab.
std::vector<brick::BrickKey> pole_subset(const brick::BrickCache& cache) {
    std::vector<brick::BrickKey> keys = cache.surface_bricks();
    const float w = static_cast<float>(cache.config().dim) * cache.config().voxel_size;
    auto dist = [&](const brick::BrickKey& k) {
        const float cx = (static_cast<float>(k.x) + 0.5f) * w - 1.0f;
        const float cy = (static_cast<float>(k.y) + 0.5f) * w;
        const float cz = (static_cast<float>(k.z) + 0.5f) * w;
        return cx * cx + cy * cy + cz * cz;
    };
    std::sort(keys.begin(), keys.end(),
              [&](const brick::BrickKey& a, const brick::BrickKey& b) {
                  return dist(a) < dist(b);
              });
    keys.resize(std::min<std::size_t>(80, keys.size()));
    return keys;
}

void mesh_pole_with_gradients(benchmark::State& state, int nodes) {
    scene::Document doc = sculpted_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    std::vector<brick::BrickKey> subset = pole_subset(cache);
    mesh::MeshingOptions options;
    options.normals = mesh::NormalMode::Gradient;
    options.colors = false;
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_bricks(cache, &doc, options, &subset);
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["bricks"] = static_cast<double>(subset.size());
    state.counters["nodes"] = static_cast<double>(nodes);
}

}  // namespace

void BM_MeshBricksGradFreshDoc(benchmark::State& state) { mesh_pole_with_gradients(state, 1); }
BENCHMARK(BM_MeshBricksGradFreshDoc)->Unit(benchmark::kMillisecond);

void BM_MeshBricksGradGrownDoc(benchmark::State& state) { mesh_pole_with_gradients(state, 193); }
BENCHMARK(BM_MeshBricksGradGrownDoc)->Unit(benchmark::kMillisecond);

// Cull index: refilling a FIXED dab's worth of bricks must cost what the
// bricks cost, not what the document does. Every per-brick culled compile
// used to walk the whole document (bounds recomputed per item per brick), so
// a dab's refill grew with everything already sculpted. With the
// per-revision CullIndex and a per-batch coarse CullPlan the per-brick
// compiles walk only the batch's neighbourhood; the index rebuild — one
// bounds pass, which is what a SINGLE per-brick compile used to pay — is
// timed inside the loop because a real dab bumps the revision and rebuilds
// it. Same +x-pole bricks, against 1 node and against 193 far dabs;
// check_bench.py gates the ratio the same way it gates MeshBricksGrad.
namespace {

void refill_pole_dab(benchmark::State& state, int nodes) {
    scene::Document doc = sculpted_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    // A dab-sized dirty region at the +x pole, far from every grown dab.
    cache.mark_dirty(math::Aabb{cf3(0.92f, -0.12f, -0.12f), cf3(1.08f, 0.12f, 0.12f)});
    const std::vector<brick::BrickRequest> reqs = cache.take_dirty();
    std::vector<float> values;
    for (auto _ : state) {
        scene::CullIndex index(doc);
        math::Aabb batch;
        for (const brick::BrickRequest& req : reqs) batch.expand(cache.cull_region(req.key));
        const scene::CullPlan plan = index.plan(batch);
        for (const brick::BrickRequest& req : reqs) {
            scene::CullRegion cull{cache.cull_region(req.key)};
            scene::Tape tape = scene::compile_document(doc, &cull, &index, &plan);
            values.resize(static_cast<std::size_t>(req.grid.nx) * req.grid.ny * req.grid.nz);
            cpu->eval_grid(tape, req.grid, values.data());
        }
        benchmark::DoNotOptimize(values.data());
    }
    state.counters["bricks"] = static_cast<double>(reqs.size());
    state.counters["nodes"] = static_cast<double>(nodes);
}

}  // namespace

void BM_DabRefillFreshDoc(benchmark::State& state) { refill_pole_dab(state, 1); }
BENCHMARK(BM_DabRefillFreshDoc)->Unit(benchmark::kMillisecond);

void BM_DabRefillGrownDoc(benchmark::State& state) { refill_pole_dab(state, 193); }
BENCHMARK(BM_DabRefillGrownDoc)->Unit(benchmark::kMillisecond);

// Batched brick-mesh attributes: on a DENSELY sculpted region — every dab on
// top of the measured bricks, so the per-brick culled tapes are long — the
// attribute pass costs a bounded multiple of refilling the same bricks. Both
// evaluate similar point counts against the same culled tapes; refill has
// always gone through the CPU backend's pool, and the attribute pass now
// goes through eval_points_batch, so their ratio is small and stays small.
// Before the batch the attribute taps ran one vertex at a time on one core
// and the ratio scaled with core count; check_bench.py gates it.
namespace {

// The sculpted sphere with every dab AT the +x pole: the pole subset's
// culled tapes then hold ~all 193 nodes, the dense-stroke worst case.
scene::Document pole_dense_sphere(int nodes) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("sculpt");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i < nodes; ++i) {
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.04f);
        const float a = 0.3f * std::sin(static_cast<float>(i) * 0.7f);
        const float b = 0.3f * std::cos(static_cast<float>(i) * 1.3f);
        const float x = std::sqrt(std::max(0.0f, 1.0f - a * a - b * b));
        dab.xform.position = cf3(x, a, b);
        l.sdf->insert(dab);
    }
    return doc;
}

}  // namespace

void BM_MeshBricksGradDenseDoc(benchmark::State& state) {
    scene::Document doc = pole_dense_sphere(193);
    brick::BrickCache cache = filled_cache(doc);
    std::vector<brick::BrickKey> subset = pole_subset(cache);
    mesh::MeshingOptions options;
    options.normals = mesh::NormalMode::Gradient;
    options.colors = true;
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_bricks(cache, &doc, options, &subset);
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["bricks"] = static_cast<double>(subset.size());
}
BENCHMARK(BM_MeshBricksGradDenseDoc)->Unit(benchmark::kMillisecond);

void BM_DabRefillDenseDoc(benchmark::State& state) {
    scene::Document doc = pole_dense_sphere(193);
    brick::BrickCache cache = filled_cache(doc);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    cache.mark_dirty(math::Aabb{cf3(0.92f, -0.12f, -0.12f), cf3(1.08f, 0.12f, 0.12f)});
    const std::vector<brick::BrickRequest> reqs = cache.take_dirty();
    std::vector<float> values;
    for (auto _ : state) {
        scene::CullIndex index(doc);
        math::Aabb batch;
        for (const brick::BrickRequest& req : reqs) batch.expand(cache.cull_region(req.key));
        const scene::CullPlan plan = index.plan(batch);
        for (const brick::BrickRequest& req : reqs) {
            scene::CullRegion cull{cache.cull_region(req.key)};
            scene::Tape tape = scene::compile_document(doc, &cull, &index, &plan);
            values.resize(static_cast<std::size_t>(req.grid.nx) * req.grid.ny * req.grid.nz);
            cpu->eval_grid(tape, req.grid, values.data());
        }
        benchmark::DoNotOptimize(values.data());
    }
    state.counters["bricks"] = static_cast<double>(reqs.size());
}
BENCHMARK(BM_DabRefillDenseDoc)->Unit(benchmark::kMillisecond);

void BM_MeshTape(benchmark::State& state) {
    scene::Document doc = bench_document();
    scene::Tape tape = scene::compile_document(doc);
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_tape(tape, tape.bounds, 0.02f);
        benchmark::DoNotOptimize(m.triangle_count());
    }
}
BENCHMARK(BM_MeshTape)->Unit(benchmark::kMillisecond);

// same scene/resolution as BM_MeshTape: the meshing spec's "preview is
// cheaper than marching" claim is gated on this pair by check_bench.py
void BM_SurfaceNets(benchmark::State& state) {
    scene::Document doc = bench_document();
    scene::Tape tape = scene::compile_document(doc);
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_tape_nets(tape, tape.bounds, 0.02f);
        benchmark::DoNotOptimize(m.triangle_count());
    }
}
BENCHMARK(BM_SurfaceNets)->Unit(benchmark::kMillisecond);

// Issue #86: greedy meshing cost a flat ~4 ms per occupied chunk however empty
// the chunk was, because the exposure mask probed the chunk map — a hash and a
// find — once per cell, six directions x 32 slices x a 32x32 window deep. Both
// benches below hold one voxel per chunk, which is the shape that made the bug
// visible; the second is the per-chunk cost Part 2 gets sized from.
//
// Gated by absolute ceilings rather than against a full chunk. The obvious
// ratio — an almost-empty chunk against a solid one — is not the instrument it
// looks like: with the lookup hoisted, what is left per chunk is the greedy
// merge's scan of the mask window, which is the same work whether the window
// held one voxel or 32768. The ratio was 0.50x before the fix and 0.66x after,
// so it moves the WRONG WAY while the wall clock drops 20x. The ceilings are
// generous in the style of the floors above: ~12x headroom on this machine
// (0.16 ms against the 2 ms ceiling, 9.6 ms against the 120 ms one), and still
// far below the 3.8 ms / 256 ms the per-cell lookup cost here, which is what
// they are there to catch coming back.
// A LARGE-RADIUS voxel verb, which is the shape #119 wants parallel: the
// footprint is n^3 cells, each decided from a 26-neighbour read of an
// immutable snapshot, and only the cells that CHANGE are written. Size 32 is
// the device gate's own `voxel_smooth_r32` radius, so this measures the same
// thing on a machine with a fan.
//
// Split into two counters, because the design question is which half dominates:
// deciding (parallelizable, pure) or writing (serial, mutates the chunk map).
// Rasterizing a document into cells: a tape evaluation PER CELL, which is the
// shape #119 predicts is parallel — a compiled tape is const during eval and
// the kernels hold no state, so any number of threads can hammer one.
//
// The shared mutation is the PALETTE: `palette_add` inserts a nearest-entry
// match, so it cannot run concurrently. That is what the two-phase split is
// for here, exactly as it is for the verbs.
void BM_VoxelRasterizeTape(benchmark::State& state) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node ball;
    ball.prim = scene::Prim::sphere(0.55f);
    l.sdf->insert(ball);
    scene::Node cap;
    cap.prim = scene::Prim::capsule(cf3(0, 0.2f, 0), cf3(0, 0.8f, 0), 0.2f);
    cap.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.12f};
    l.sdf->insert(cap);
    const scene::Tape tape = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.8f, -0.8f, -0.8f), cf3(0.8f, 1.0f, 0.8f)};

    for (auto _ : state) {
        voxel::VoxelGrid g(0.02f);
        g.rasterize_tape(tape, region);
        benchmark::DoNotOptimize(g.occupied_count());
        state.counters["cells"] = static_cast<double>(g.occupied_count());
    }
}
BENCHMARK(BM_VoxelRasterizeTape)->Unit(benchmark::kMillisecond);

void BM_VoxelSculptSmoothR32(benchmark::State& state) {
    voxel::VoxelGrid g(0.02f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    // A blob big enough that a size-32 brush lands entirely inside material,
    // so the verb does the work rather than skipping empty space.
    for (int z = -24; z <= 24; ++z)
        for (int y = -24; y <= 24; ++y)
            for (int x = -24; x <= 24; ++x)
                if (x * x + y * y + z * z <= 24 * 24) g.set({x, y, z}, c);

    voxel::BrushParams p;
    p.size = 32;
    p.shape = voxel::BrushShape::Sphere;
    p.falloff = voxel::BrushFalloff::Smooth;
    p.strength = 1.0f;

    const std::size_t before = g.change_count();
    for (auto _ : state) {
        g.sculpt_smooth({0, 0, 0}, p);
        benchmark::DoNotOptimize(g.change_count());
    }
    state.counters["writes"] = static_cast<double>(g.change_count() - before);
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelSculptSmoothR32)->Unit(benchmark::kMillisecond);

void BM_VoxelMeshSparseChunk(benchmark::State& state) {
    voxel::VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    g.set({16, 16, 16}, c);
    for (auto _ : state) {
        mesh::Mesh m = g.mesh_greedy();
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelMeshSparseChunk)->Unit(benchmark::kMillisecond);

// Subdividing a WHOLE level: the cost the region-refined level stack (#134)
// added to the path that does not use it. `chunk_is_refined` short-circuits on
// `whole`, but reaching it costs a chunk_key() — three divisions — for each of
// the eight children of every material cell, so the test is dead work exactly
// where it is hottest. Measured on the device gate at 2.36x (voxel_add_level,
// 0.51 -> 1.21 ms) and 2.47x here (0.53 -> 1.31 ms at 1000 stamps).
//
// The ceiling here is honest about what it can do. Fixed 0.46 ms against
// unfixed 0.84 ms on an M-series Mac: 1.84x, and a shared CI runner is easily
// 2x slower than this machine, so no threshold catches 1.84x without flaking on
// the runner. The ceiling is set for the order-of-magnitude case this file's
// header describes, and THE DEVICE GATE is what catches this size of change —
// which is how it was found. The benchmark still earns its place by putting the
// number in CI output where a 5x would be obvious.
void BM_VoxelAddLevelWhole(benchmark::State& state) {
    voxel::VoxelGrid g(0.02f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    // SCATTERED material, not one solid block. The shape matters more than the
    // amount: the cost this gates is a per-child chunk_key(), and a solid block
    // lives in a handful of chunks where write_cell's own bookkeeping dominates
    // it — measured at 1.14x there against 2.47x here, which would have let the
    // regression back in. This is the device case's own spread: the same
    // low-discrepancy walk over a 40-cell scale, a small blob at each stop.
    const double golden = 0.6180339887;
    for (int i = 0; i < 400; ++i) {
        const int bx = int((std::fmod(double(i) * golden, 1.0) * 1.6 - 0.8) * 40);
        const int by = int((std::fmod(double(i) * golden * golden, 1.0) * 1.6 - 0.8) * 40);
        const int bz = int((std::fmod(double(i) * golden * golden * golden, 1.0) * 1.6 - 0.8) * 40);
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) g.set({bx + x, by + y, bz + z}, c);
    }
    for (auto _ : state) {
        g.add_level();
        benchmark::DoNotOptimize(g.level_count());
        state.PauseTiming();
        g.drop_level();
        state.ResumeTiming();
    }
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelAddLevelWhole)->Unit(benchmark::kMillisecond);

// WRITING into a whole level stack, which is the same defect #137 fixed in
// subdivide_into and the hotter half of it. `add_level` pays the per-child
// chunk_key() once; a WRITE pays it in both directions, every time:
//
//   propagate_down -> refresh_detail -> record_detail, once per child
//   propagate_up,                       once per child, recursing per level
//
// Both tests are constant-true on a whole level, and both cost three divisions
// to reach. A sculpting session is writes, not add_levels, so this is the path
// that carries the cost in practice.
//
// SCATTERED for the reason BM_VoxelAddLevelWhole scatters: a solid block lives
// in a handful of chunks where write_cell's own bookkeeping dominates the key,
// which is exactly the fixture that would let the regression back in.
//
// The ceiling is the same order-of-magnitude gate the file's header describes;
// the device case (`voxel_smooth_l2`, a verb one level finer) is what holds the
// tighter line.
void BM_VoxelWriteUnderLevels(benchmark::State& state) {
    voxel::VoxelGrid g(0.02f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    const double golden = 0.6180339887;
    for (int i = 0; i < 200; ++i) {
        const int bx = int((std::fmod(double(i) * golden, 1.0) * 1.6 - 0.8) * 40);
        const int by = int((std::fmod(double(i) * golden * golden, 1.0) * 1.6 - 0.8) * 40);
        const int bz = int((std::fmod(double(i) * golden * golden * golden, 1.0) * 1.6 - 0.8) * 40);
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) g.set({bx + x, by + y, bz + z}, c);
    }
    // Two levels above the one being written, so propagation runs in both
    // directions and more than one level deep.
    g.add_level();
    g.add_level();
    g.set_active_level(1);

    int n = 0;
    for (auto _ : state) {
        // A moving brush, so the writes do not all land in one chunk and the
        // key is recomputed the way a stroke recomputes it.
        const int bx = int((std::fmod(double(n) * golden, 1.0) * 1.6 - 0.8) * 60);
        const int by = int((std::fmod(double(n) * golden * golden, 1.0) * 1.6 - 0.8) * 60);
        const int bz = int((std::fmod(double(n) * golden * golden * golden, 1.0) * 1.6 - 0.8) * 60);
        for (int z = 0; z < 6; ++z)
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 6; ++x) g.set({bx + x, by + y, bz + z}, c);
        ++n;
    }
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelWriteUnderLevels)->Unit(benchmark::kMillisecond);

// The scaling shape: one voxel in each of 64 chunks. Cost is linear in occupied
// chunks and independent of how much material each holds, so this is the
// per-chunk number times 64 and catches the same regression with more signal
// above the noise.
void BM_VoxelMeshSparse64Chunks(benchmark::State& state) {
    voxel::VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    for (int i = 0; i < 64; ++i) g.set({i * 32 + 16, 16, 16}, c);
    for (auto _ : state) {
        mesh::Mesh m = g.mesh_greedy();
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelMeshSparse64Chunks)->Unit(benchmark::kMillisecond);

// Issue #86 part 2: the same 64-chunk grid, meshing only the two chunks a dab
// would have dirtied. What this gates is the SHAPE — that the regional call
// costs the chunks named and not the grid — so it is the bench above divided
// by 32, and a regression that reintroduced a whole-grid sweep would blow the
// ceiling by an order of magnitude rather than a few percent.
void BM_VoxelMeshDirtyChunks(benchmark::State& state) {
    voxel::VoxelGrid g(0.1f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    for (int i = 0; i < 64; ++i) g.set({i * 32 + 16, 16, 16}, c);
    (void)g.take_dirty_chunks();
    g.set({16, 17, 16}, c);  // one dab's worth of writes, in two chunks
    g.set({32 + 16, 17, 16}, c);
    const std::vector<voxel::VoxelCoord> dirty = g.take_dirty_chunks();
    for (auto _ : state) {
        mesh::Mesh m = g.mesh_greedy_chunks(dirty);
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["chunks"] = static_cast<double>(dirty.size());
}
BENCHMARK(BM_VoxelMeshDirtyChunks)->Unit(benchmark::kMillisecond);

// Issue #108: the same sculpt as a rounded form rather than as boxes. Paired
// with the greedy bench on an IDENTICAL grid, because the number worth having
// is the ratio — a host choosing between the two pictures is choosing what it
// pays, and "surface nets is slower" is not actionable without how much.
//
// A solid-ish blob rather than the sparse grids above: the smooth mesher costs
// its SURFACE (one vertex per surface cell) where greedy costs its occupied
// chunks, so a one-voxel-per-chunk grid would flatter it for the wrong reason.
namespace {
voxel::VoxelGrid smooth_bench_grid() {
    voxel::VoxelGrid g(0.05f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    g.set_brush({0, 0, 0}, 24, c, voxel::BrushShape::Sphere);
    g.set_brush({14, 8, 0}, 12, c, voxel::BrushShape::Sphere);
    return g;
}
}  // namespace

void BM_VoxelMeshGreedyBlob(benchmark::State& state) {
    voxel::VoxelGrid g = smooth_bench_grid();
    for (auto _ : state) {
        mesh::Mesh m = g.mesh_greedy();
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelMeshGreedyBlob)->Unit(benchmark::kMillisecond);

void BM_VoxelMeshSmoothBlob(benchmark::State& state) {
    voxel::VoxelGrid g = smooth_bench_grid();
    for (auto _ : state) {
        mesh::Mesh m = g.mesh_smooth();
        benchmark::DoNotOptimize(m.triangle_count());
    }
    state.counters["cells"] = static_cast<double>(g.occupied_count());
}
BENCHMARK(BM_VoxelMeshSmoothBlob)->Unit(benchmark::kMillisecond);

// Consolidation (accel/parallel-consolidate): baking a 193-node layer, once
// through bake_layer — which batches every lattice sample through the CPU
// backend's pool — and once through the serial std::function path bake_layer
// replaced, reconstructed here as the reference. The two are byte-identical
// by contract (regression-tested in test_consolidate.cpp); check_bench.py
// requires the batched bake to be FASTER than the serial one, which is what
// catches the one-point-at-a-time bake coming back. A ratio against a
// fixed-cost reference would drift with core count; this pair holds on any
// machine with more than one, because both sides pay the identical serial
// redistance floor and only the evaluation differs.
void BM_ConsolidateGrownDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    scene::ConsolidationParams params;
    params.cell_size = 0.05f;
    for (auto _ : state) {
        std::optional<field::FieldVolume> v =
            scene::bake_layer(doc.layers.front(), params, nullptr, eval::pooled_bake_eval());
        benchmark::DoNotOptimize(v->sample_count());
    }
}
BENCHMARK(BM_ConsolidateGrownDoc)->Unit(benchmark::kMillisecond);

void BM_ConsolidateSerialGrownDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const float cell = 0.05f, band = cell * 3.0f;
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    for (auto _ : state) {
        field::FieldVolume v = field::FieldVolume::sample(
            [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, region, cell, band);
        if (field::redistance(v)) v.compact();
        v.set_sample_lipschitz(v.measure_sample_lipschitz());
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_ConsolidateSerialGrownDoc)->Unit(benchmark::kMillisecond);

// The same layer with TWO colours in it, which is what makes the bake fill a
// colour channel — a SECOND evaluation of the tape at every surviving sample.
// check_bench.py requires the one-colour bake above to be FASTER than this one,
// which is what catches a uniform layer paying for colour it cannot show.
//
// It is a pair rather than a ceiling on purpose. That defect shipped once
// already, in the release that added colour, and nothing in CI reported it: the
// only gate on this bake compared it against the serial reference, and both
// sides of that comparison moved together, so a 1.5x regression read as a 7x
// win. A ratio between two paths that differ ONLY in whether the pass is taken
// cannot wash out that way, and unlike a millisecond floor it does not have to
// be re-tuned for whatever runner CI is on.
void BM_ConsolidateColoredGrownDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(192);
    // One dab of a second colour is the whole difference: the layer can now
    // produce more than one colour, so the channel has to be filled.
    scene::Node red;
    red.prim = scene::Prim::sphere(0.04f);
    red.xform.position = cf3(-1.0f, 0, 0);
    red.color = cf3(1, 0, 0);
    doc.layers.front().sdf->insert(red);
    scene::ConsolidationParams params;
    params.cell_size = 0.05f;
    for (auto _ : state) {
        std::optional<field::FieldVolume> v =
            scene::bake_layer(doc.layers.front(), params, nullptr, eval::pooled_bake_eval());
        benchmark::DoNotOptimize(v->sample_count());
    }
}
BENCHMARK(BM_ConsolidateColoredGrownDoc)->Unit(benchmark::kMillisecond);

// Batched brick raycast (accel/parallel-raycast): clay_brick_cache_raycast_many
// fans its rays out across the CPU backend's pool; the serial reference is the
// single-ray C-ABI call issued once per ray, which marches identically and by
// contract answers bit-identically (regression-tested in test_c_brick.cpp).
// The pair goes through the C ABI because that is where the fan-out lives.
// check_bench.py requires the batch to be FASTER than the serial loop, which
// is what catches the batch falling off the pool; it holds on any machine
// with more than one core, since both sides pay the same per-ray march and
// only the fan-out differs (8x on an M2 Max).
namespace {

struct CRaycastScene {
    clay_document* doc = nullptr;
    clay_brick_cache* cache = nullptr;
    std::vector<float> rays;  // count * 6 packed origin+direction

    explicit CRaycastScene(std::size_t ray_count) {
        doc = clay_document_create();
        clay_layer_id layer = 0;
        clay_add_sdf_layer(doc, "bench", &layer);
        auto add_sphere = [&](float r, float x, float y, float z) {
            clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
            const float pos[3] = {x, y, z};
            clay_item_set_position(it, pos);
            clay_node_id id = 0;
            clay_layer_add_item(doc, layer, it, &id);
            clay_item_destroy(it);
        };
        add_sphere(0.4f, 0.0f, 0.0f, 0.0f);
        add_sphere(0.25f, 0.5f, 0.2f, -0.1f);
        add_sphere(0.3f, -0.4f, -0.3f, 0.2f);
        clay_brick_config cfg;
        clay_brick_config_defaults(&cfg);
        cfg.dim = 8;
        cfg.voxel_size = 0.05f;
        cfg.band_voxels = 3;
        cfg.memory_budget = 0;
        cache = clay_brick_cache_create(&cfg);
        clay_brick_cache_mark_dirty_layer(cache, doc, layer);
        constexpr std::size_t kChunk = 64, kSamples = 8 * 8 * 8;
        std::vector<clay_brick_request> reqs(kChunk);
        std::vector<float> values(kChunk * kSamples);
        for (;;) {
            std::size_t count = kChunk, remaining = 0;
            clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining);
            if (count == 0) break;
            clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                           count * kSamples, nullptr, 0);
            std::size_t accepted = 0;
            clay_brick_cache_submit(cache, reqs.data(), count, values.data(), count * kSamples,
                                    nullptr, 0, nullptr, &accepted);
            if (remaining == 0) break;
        }
        // Rays from a shell of radius 3 toward jittered targets among the
        // spheres, so most hit and every march crosses real bricks.
        rays.resize(ray_count * 6);
        std::uint64_t s = 24680;
        auto rnd = [&] {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>((s >> 40) & 0xFFFFFF) / 16777216.0f - 0.5f;
        };
        for (std::size_t i = 0; i < ray_count; ++i) {
            float* r = rays.data() + i * 6;
            float o[3] = {rnd(), rnd(), rnd()};
            const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) + 1e-6f;
            for (int a = 0; a < 3; ++a) {
                r[a] = o[a] / len * 3.0f;
                r[3 + a] = rnd() * 0.8f - r[a];
            }
        }
    }
    ~CRaycastScene() {
        clay_brick_cache_destroy(cache);
        clay_document_destroy(doc);
    }
    CRaycastScene(const CRaycastScene&) = delete;
    CRaycastScene& operator=(const CRaycastScene&) = delete;
};

constexpr std::size_t kRaycastBenchRays = 2048;

}  // namespace

void BM_RaycastBricksBatch(benchmark::State& state) {
    CRaycastScene scene(kRaycastBenchRays);
    std::vector<std::int32_t> hits(kRaycastBenchRays);
    std::vector<float> t(kRaycastBenchRays);
    for (auto _ : state) {
        clay_brick_cache_raycast_many(scene.cache, scene.rays.data(), kRaycastBenchRays,
                                      hits.data(), t.data(), nullptr, nullptr);
        benchmark::DoNotOptimize(hits.data());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * kRaycastBenchRays);
}
BENCHMARK(BM_RaycastBricksBatch)->Unit(benchmark::kMillisecond);

void BM_RaycastBricksSerial(benchmark::State& state) {
    CRaycastScene scene(kRaycastBenchRays);
    std::vector<std::int32_t> hits(kRaycastBenchRays);
    std::vector<float> t(kRaycastBenchRays);
    for (auto _ : state) {
        for (std::size_t i = 0; i < kRaycastBenchRays; ++i)
            clay_brick_cache_raycast(scene.cache, scene.rays.data() + i * 6,
                                     scene.rays.data() + i * 6 + 3, &hits[i], &t[i], nullptr,
                                     nullptr);
        benchmark::DoNotOptimize(hits.data());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * kRaycastBenchRays);
}
BENCHMARK(BM_RaycastBricksSerial)->Unit(benchmark::kMillisecond);

// Undo does not scale with the document (accel/undo-removal): undoing a
// 100-stamp stroke on a 10k-stamp document is gated against the same stroke
// on a 100-stamp one. Undo removes each stamp through SdfContent::locate,
// which used to walk the root list and then every node's children — linear
// in the document, 33x between these two sizes — and now answers from the
// maintained id -> (parent, index) location map. check_bench.py holds the
// ratio to the same generous 3x style as the gates above: it catches the
// O(document) walk coming back, not runner noise.
namespace {

void undo_stamp_stroke(benchmark::State& state, int base_nodes) {
    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("sculpt");
    const scene::LayerId lid = layer.id;
    scene::UndoStack undo;
    auto stamp = [&](int i) {
        scene::Node n;
        n.id = layer.sdf->reserve_id();
        n.prim = scene::Prim::sphere(0.05f);
        n.xform.position = cf3(0.001f * static_cast<float>(i), 0, 0);
        return n;
    };
    auto add = [&](int i) {
        return undo.perform(doc,
                            scene::Command{scene::AddNodeCmd{lid, scene::kNoNode, -1, {stamp(i)}}});
    };
    for (int i = 0; i < base_nodes; ++i) add(i);
    for (auto _ : state) {
        undo.begin_group();
        for (int i = 0; i < 100; ++i) add(base_nodes + i);
        undo.end_group();
        undo.undo(doc);
        benchmark::DoNotOptimize(doc.layers.front().sdf->roots.data());
    }
    state.counters["nodes"] = static_cast<double>(base_nodes);
}

void BM_UndoStampsFreshDoc(benchmark::State& state) { undo_stamp_stroke(state, 100); }
BENCHMARK(BM_UndoStampsFreshDoc)->Unit(benchmark::kMillisecond);

void BM_UndoStampsGrownDoc(benchmark::State& state) { undo_stamp_stroke(state, 10000); }
BENCHMARK(BM_UndoStampsGrownDoc)->Unit(benchmark::kMillisecond);

}  // namespace

// Resident uploaded tapes (accel/metal-persistent): the Metal backend keeps
// the uploaded form of recent tapes resident, keyed on the process-unique
// Tape::compile_id the compiler stamps, so re-evaluating an unchanged
// document does not re-upload its tape — notably a consolidated volume's
// multi-megabyte blob — every dispatch. The pair below drives a small lattice
// against a consolidated layer once with ONE compiled tape (every call after
// the first is resident) and once alternating MORE tapes of the same document
// than the residency holds (every call re-uploads the blob). The values are
// identical by construction; check_bench.py requires resident < reupload,
// which is exactly the residency existing. Registered only when a Metal
// device is present — FASTER_THAN skips the pair when the names are absent,
// so CPU-only CI is unaffected.
namespace {

void metal_consolidated_eval(benchmark::State& state, int tape_count) {
    scene::Document doc = sculpted_sphere(193);
    scene::ConsolidationParams params;
    params.cell_size = 0.015f;
    scene::consolidate_layer(doc, doc.layers.front().id, params, nullptr, nullptr,
                             eval::pooled_bake_eval());
    std::vector<scene::Tape> tapes;
    for (int i = 0; i < tape_count; ++i) tapes.push_back(scene::compile_document(doc));
    eval::Backend* metal = eval::Registry::instance().find("metal");
    eval::GridQuery q;
    q.origin = cf3(0.9f, -0.08f, -0.08f);
    q.spacing = 0.02f;
    q.nx = q.ny = q.nz = 8;
    std::vector<float> values(static_cast<std::size_t>(q.nx) * q.ny * q.nz);
    std::size_t at = 0;
    for (auto _ : state) {
        metal->eval_grid(tapes[at % tapes.size()], q, values.data());
        ++at;
        benchmark::DoNotOptimize(values.data());
    }
    state.counters["tapes"] = static_cast<double>(tape_count);
    state.counters["blob_floats"] = static_cast<double>(tapes.front().blob.size());
}

// Called from main, not from a static initializer: probing the registry
// spins up backend runtimes (a Metal device), which has no business running
// before main.
void register_metal_benches() {
    if (!eval::Registry::instance().find("metal")) return;
    benchmark::RegisterBenchmark("BM_MetalTapeResident",
                                 [](benchmark::State& s) { metal_consolidated_eval(s, 1); })
        ->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("BM_MetalTapeReupload",
                                 [](benchmark::State& s) { metal_consolidated_eval(s, 6); })
        ->Unit(benchmark::kMillisecond);
}

}  // namespace

}  // namespace

// BENCHMARK_MAIN(), plus the conditionally registered Metal pair.
int main(int argc, char** argv) {
    register_metal_benches();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
