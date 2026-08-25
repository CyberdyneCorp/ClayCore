// Performance benchmarks with CI regression gates (build-packaging spec):
// points/sec, bricks/sec, mesh time on fixed scenes. tools/check_bench.py
// enforces generous floor thresholds in CI — they catch order-of-magnitude
// regressions; tight deltas need dedicated hardware.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>
#include <vector>

#include "clay.h"
#include "clay/brick/cache.h"
#include "clay/eval/backend.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/surface_nets.h"
#include "clay/field/redistance.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/eval/bake_points.h"
#include "clay/eval/bake_volume.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "clay/mesh/bvh.h"
#include "scatter_spread.h"

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

// -- the deep edit list (#118 workstream C) ---------------------------------
//
// "A 2,000-item document driven through the brick cache — the honest next
// measurement, and the gate the rows below are judged by." The existing dense
// fixture is 193 nodes; this is an order of magnitude past it, which is where
// the epic says the per-brick cull's O(items) walk stops being affordable.
//
// TWO SETS, because the first version of this measured the wrong path and
// concluded the wrong thing. `clay_brick_cache_eval_requests` builds one
// CullIndex per revision and one CullPlan per batch, then compiles each brick
// against the plan's survivor lists — so a bare `compile_document(doc, &cull)`
// is NOT what a host pays. It is 5.5x slower.
//
//   *Planned  — index + plan, as the C ABI does it. THIS is the cost of a dab.
//   the rest  — the unaccelerated compile, kept as the contrast that shows
//               what CullIndex is already worth
//
// Both are kept rather than the wrong one deleted, because the pair is the
// evidence for what the index buys and for what it does not: a 5.5x constant,
// and a slope that is still linear in item count.
//
// A sculpt, not a cloud: dabs are placed ON the surface of a base sphere the
// way a stroke leaves them, so the culled tapes are realistically dense rather
// than trivially empty.
namespace {

scene::Document deep_sphere(int nodes) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("sculpt");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    // A low-discrepancy walk over the sphere, so the dabs cover it evenly
    // rather than clumping the way a naive sin/cos pair does.
    const double golden = 0.6180339887;
    for (int i = 1; i < nodes; ++i) {
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.05f);
        const double u = std::fmod(static_cast<double>(i) * golden, 1.0);
        const double v = (static_cast<double>(i) + 0.5) / static_cast<double>(nodes);
        const double phi = std::acos(1.0 - 2.0 * v);
        const double th = 6.283185307 * u;
        dab.xform.position = cf3(static_cast<float>(std::sin(phi) * std::cos(th)),
                                 static_cast<float>(std::cos(phi)),
                                 static_cast<float>(std::sin(phi) * std::sin(th)));
        dab.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.03f};
        l.sdf->insert(dab);
    }
    return doc;
}

}  // namespace

// NAMED rather than Arg()-parameterised: check_bench.py keys a gate on the
// part of the name before "/", so `BM_X/2000` and `BM_X/193` collapse to the
// same key and the ceiling would gate whichever ran last. A trap worth not
// leaving for the next person.
void deep_doc_compile(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    const std::vector<brick::BrickKey> all = cache.surface_bricks();
    const scene::CullRegion cull{cache.cull_region(all.front())};
    for (auto _ : state) {
        scene::Tape tape = scene::compile_document(doc, &cull);
        benchmark::DoNotOptimize(tape.instrs.size());
        state.counters["instrs"] = static_cast<double>(tape.instrs.size());
    }
    state.counters["nodes"] = static_cast<double>(nodes);
}
void BM_DeepDocCompile193(benchmark::State& state) { deep_doc_compile(state, 193); }
BENCHMARK(BM_DeepDocCompile193)->Unit(benchmark::kMillisecond);
void BM_DeepDocCompile2000(benchmark::State& state) { deep_doc_compile(state, 2000); }
BENCHMARK(BM_DeepDocCompile2000)->Unit(benchmark::kMillisecond);

// The cull ALONE, over a dab's worth of bricks: this is the ~64 ns x item x
// brick walk #118 says is past the interactive budget at 10k items before a
// sample is evaluated.
void deep_doc_cull(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    std::vector<brick::BrickKey> all = cache.surface_bricks();
    std::vector<brick::BrickKey> dab(all.begin(),
                                     all.begin() + std::min<std::size_t>(8, all.size()));
    for (auto _ : state) {
        for (const brick::BrickKey& key : dab) {
            scene::CullRegion cull{cache.cull_region(key)};
            scene::Tape tape = scene::compile_document(doc, &cull);
            benchmark::DoNotOptimize(tape.instrs.size());
        }
    }
    state.counters["nodes"] = static_cast<double>(nodes);
    state.counters["bricks"] = static_cast<double>(dab.size());
}
// The SAME cull, through the CullIndex and CullPlan the C ABI actually uses.
//
// The pair above measures compile_document with a bare CullRegion, which is
// NOT what a host pays: clay_brick_cache_eval_requests builds one index per
// revision and one plan per batch, then compiles each brick against the
// plan's survivor lists. Measuring the unaccelerated path and calling it the
// cost of a dab overstates it — this is the correction.
void deep_doc_cull_planned(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    std::vector<brick::BrickKey> all = cache.surface_bricks();
    std::vector<brick::BrickKey> dab(all.begin(),
                                     all.begin() + std::min<std::size_t>(8, all.size()));
    for (auto _ : state) {
        // Per revision, as the document handle owns it.
        const scene::CullIndex index(doc);
        // Per batch: ONE region containing every brick's, which is what the
        // plan requires and what the C ABI builds.
        math::Aabb batch_region;
        for (const brick::BrickKey& key : dab) batch_region.expand(cache.cull_region(key));
        const scene::CullPlan plan = index.plan(batch_region);
        for (const brick::BrickKey& key : dab) {
            scene::CullRegion cull{cache.cull_region(key)};
            scene::Tape tape = scene::compile_document(doc, &cull, &index, &plan);
            benchmark::DoNotOptimize(tape.instrs.size());
        }
    }
    state.counters["nodes"] = static_cast<double>(nodes);
    state.counters["bricks"] = static_cast<double>(dab.size());
}
void BM_DeepDocCullPlanned193(benchmark::State& state) { deep_doc_cull_planned(state, 193); }
BENCHMARK(BM_DeepDocCullPlanned193)->Unit(benchmark::kMillisecond);
void BM_DeepDocCullPlanned2000(benchmark::State& state) { deep_doc_cull_planned(state, 2000); }
BENCHMARK(BM_DeepDocCullPlanned2000)->Unit(benchmark::kMillisecond);
// 10 000 items is the size add-item-spatial-index calls over budget "before it
// evaluates a single sample". Measured rather than extrapolated, because that
// proposal's arithmetic predates CullIndex and CullPlan and no longer describes
// this build.
void BM_DeepDocCullPlanned10000(benchmark::State& state) { deep_doc_cull_planned(state, 10000); }
BENCHMARK(BM_DeepDocCullPlanned10000)->Unit(benchmark::kMillisecond);

void BM_DeepDocCull193(benchmark::State& state) { deep_doc_cull(state, 193); }
BENCHMARK(BM_DeepDocCull193)->Unit(benchmark::kMillisecond);
void BM_DeepDocCull2000(benchmark::State& state) { deep_doc_cull(state, 2000); }
BENCHMARK(BM_DeepDocCull2000)->Unit(benchmark::kMillisecond);

// What an edit actually pays: compile the culled tape and evaluate it, for the
// bricks a dab dirties.
void deep_doc_refill(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    std::vector<brick::BrickKey> all = cache.surface_bricks();
    std::vector<brick::BrickKey> dab(all.begin(),
                                     all.begin() + std::min<std::size_t>(8, all.size()));
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    // The requests a dab's worth of dirty bricks produces — taken once, since
    // take_dirty drains and the loop below re-evaluates the same work.
    brick::BrickCache probe(brick::BrickConfig{8, 0.05f, 3, 0});
    probe.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    std::vector<brick::BrickRequest> all_reqs = probe.take_dirty();
    std::vector<brick::BrickRequest> reqs(
        all_reqs.begin(), all_reqs.begin() + std::min<std::size_t>(8, all_reqs.size()));

    for (auto _ : state) {
        for (const brick::BrickRequest& req : reqs) {
            scene::CullRegion cull{cache.cull_region(req.key)};
            scene::Tape tape = scene::compile_document(doc, &cull);
            std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                      req.grid.nz);
            cpu->eval_grid(tape, req.grid, values.data());
            benchmark::DoNotOptimize(values[0]);
        }
    }
    state.counters["bricks"] = static_cast<double>(reqs.size());
    state.counters["nodes"] = static_cast<double>(nodes);
}
// A refill through the index and plan — what a dab actually costs a host.
void deep_doc_refill_planned(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    brick::BrickCache cache = filled_cache(doc);
    brick::BrickCache probe(brick::BrickConfig{8, 0.05f, 3, 0});
    probe.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    std::vector<brick::BrickRequest> all_reqs = probe.take_dirty();
    std::vector<brick::BrickRequest> reqs(
        all_reqs.begin(), all_reqs.begin() + std::min<std::size_t>(8, all_reqs.size()));
    eval::Backend* cpu = eval::Registry::instance().find("cpu");

    for (auto _ : state) {
        const scene::CullIndex index(doc);
        math::Aabb batch_region;
        for (const brick::BrickRequest& req : reqs) batch_region.expand(cache.cull_region(req.key));
        const scene::CullPlan plan = index.plan(batch_region);
        for (const brick::BrickRequest& req : reqs) {
            scene::CullRegion cull{cache.cull_region(req.key)};
            scene::Tape tape = scene::compile_document(doc, &cull, &index, &plan);
            std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                      req.grid.nz);
            cpu->eval_grid(tape, req.grid, values.data());
            benchmark::DoNotOptimize(values[0]);
        }
    }
    state.counters["nodes"] = static_cast<double>(nodes);
    state.counters["bricks"] = static_cast<double>(reqs.size());
}
void BM_DeepDocRefillPlanned193(benchmark::State& state) { deep_doc_refill_planned(state, 193); }
BENCHMARK(BM_DeepDocRefillPlanned193)->Unit(benchmark::kMillisecond);
void BM_DeepDocRefillPlanned2000(benchmark::State& state) { deep_doc_refill_planned(state, 2000); }
BENCHMARK(BM_DeepDocRefillPlanned2000)->Unit(benchmark::kMillisecond);
// A dab's WHOLE cost at 10 000 items — cull, compile and evaluate — which is
// the number the interactive budget is actually about.
void BM_DeepDocRefillPlanned10000(benchmark::State& state) { deep_doc_refill_planned(state, 10000); }
BENCHMARK(BM_DeepDocRefillPlanned10000)->Unit(benchmark::kMillisecond);

void BM_DeepDocRefill193(benchmark::State& state) { deep_doc_refill(state, 193); }
BENCHMARK(BM_DeepDocRefill193)->Unit(benchmark::kMillisecond);
void BM_DeepDocRefill2000(benchmark::State& state) { deep_doc_refill(state, 2000); }
BENCHMARK(BM_DeepDocRefill2000)->Unit(benchmark::kMillisecond);

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

// CORES ACTUALLY USED by a brick fill's evaluation, which is a different
// question from how long it takes and the one a cross-machine comparison needs.
//
// #207 reports `BM_BrickFill` at 47.5 ms on an M2 Max against 30.6 ms on an i9
// and reads the gap as evidence that brick fill is bound by something evaluation
// work does not touch. It might be — or the machines are 12 hardware threads
// against 24 and the benchmark is threaded. Wall clock alone cannot tell those
// apart, and `benchmark`'s own CPU column does not answer it either: it is the
// main thread's time, not the process's.
//
// So this times ONLY the `eval_grid` calls, with the tapes compiled outside the
// timed region, and reports process CPU time over wall time. That is the same
// measurement `batch-brick-eval` used to find the per-brick dispatch barrier,
// where it read 6.7-8.9 cores of 24.
//
// Not a gated benchmark — a diagnostic. Read `cores` against the machine's
// thread count, not against a threshold.
#if defined(_WIN32)
double process_cpu_seconds() { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }
#else
double process_cpu_seconds() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
#endif

void BM_BrickFillCores(benchmark::State& state) {
    scene::Document doc = bench_document();
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    brick::BrickCache cache(brick::BrickConfig{8, 0.05f, 3, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    const std::vector<brick::BrickRequest> reqs = cache.take_dirty();

    // Compiled once, outside the measurement: the question is what EVALUATION
    // does with the cores, not what compiling does.
    std::vector<scene::Tape> tapes;
    tapes.reserve(reqs.size());
    for (const brick::BrickRequest& req : reqs) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        tapes.push_back(scene::compile_document(doc, &cull));
    }
    std::vector<std::vector<float>> values(reqs.size());
    for (std::size_t i = 0; i < reqs.size(); ++i)
        values[i].resize(static_cast<std::size_t>(reqs[i].grid.nx) * reqs[i].grid.ny *
                         reqs[i].grid.nz);

    double cpu_seconds = 0.0;
    double wall_seconds = 0.0;
    for (auto _ : state) {
        const double c0 = process_cpu_seconds();
        const auto w0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < reqs.size(); ++i)
            cpu->eval_grid(tapes[i], reqs[i].grid, values[i].data());
        const auto w1 = std::chrono::steady_clock::now();
        cpu_seconds += process_cpu_seconds() - c0;
        wall_seconds += std::chrono::duration<double>(w1 - w0).count();
        benchmark::DoNotOptimize(values[0].data());
    }
    state.counters["cores"] = wall_seconds > 0.0 ? cpu_seconds / wall_seconds : 0.0;
    state.counters["bricks"] = static_cast<double>(reqs.size());
    state.counters["threads"] = static_cast<double>(std::thread::hardware_concurrency());
}
BENCHMARK(BM_BrickFillCores)->Unit(benchmark::kMillisecond);

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
// Importing a mesh as a field — `Volume.from_mesh`, and the first thing that
// happens to every model a host loads. Per sample it is a BVH signed-distance
// query with a generalized winding number, which is pure and expensive: the
// shape #119 calls "per plane/row… nothing; disjoint slices".
// Meshing a whole document over a lattice — the export path, and #119's
// "whole-document meshing per slab" with the seam welding it calls the only
// genuinely fiddly one.
//
// The FIELD evaluation already runs on the pool (eval_grid is one of the CPU
// backend's batch paths); what is serial is the marching-tets pass over the
// evaluated lattice.
void BM_MeshTapeWholeDoc(benchmark::State& state) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node ball;
    ball.prim = scene::Prim::sphere(0.55f);
    l.sdf->insert(ball);
    scene::Node cap;
    cap.prim = scene::Prim::capsule(cf3(0, 0.2f, 0), cf3(0, 0.9f, 0), 0.18f);
    cap.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.12f};
    l.sdf->insert(cap);
    scene::Node ring;
    ring.prim = scene::Prim::torus(0.5f, 0.08f);
    ring.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.06f};
    l.sdf->insert(ring);
    const scene::Tape tape = scene::compile_document(doc);
    const math::Aabb region{cf3(-0.8f, -0.8f, -0.8f), cf3(0.8f, 1.1f, 0.8f)};

    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_tape(tape, region, 0.006f, {});
        benchmark::DoNotOptimize(m.triangle_count());
        state.counters["tris"] = static_cast<double>(m.triangle_count());
    }
}
BENCHMARK(BM_MeshTapeWholeDoc)->Unit(benchmark::kMillisecond);

void BM_MeshToField(benchmark::State& state) {
    // An icosphere by hand — a real triangle soup rather than a handful of
    // quads, and built here so the benchmark does not depend on the mesher.
    mesh::Mesh m;
    const int rings = 48, seg = 96;
    for (int i = 0; i <= rings; ++i) {
        const float phi = 3.14159265f * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < seg; ++j) {
            const float th = 6.2831853f * static_cast<float>(j) / static_cast<float>(seg);
            m.positions.push_back(cf3(0.5f * std::sin(phi) * std::cos(th), 0.5f * std::cos(phi),
                                      0.5f * std::sin(phi) * std::sin(th)));
        }
    }
    auto at = [&](int i, int j) {
        return static_cast<std::uint32_t>(i * seg + (j % seg));
    };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < seg; ++j) {
            m.indices.push_back(at(i, j));
            m.indices.push_back(at(i + 1, j));
            m.indices.push_back(at(i + 1, j + 1));
            m.indices.push_back(at(i, j));
            m.indices.push_back(at(i + 1, j + 1));
            m.indices.push_back(at(i, j + 1));
        }

    mesh::ImportSettings settings;
    settings.cell_size = 0.01f;
    for (auto _ : state) {
        std::optional<field::FieldVolume> v = mesh::to_field(m, settings);
        benchmark::DoNotOptimize(v.has_value());
        state.counters["tris"] = static_cast<double>(m.triangle_count());
    }
}
BENCHMARK(BM_MeshToField)->Unit(benchmark::kMillisecond);

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

// -- BVH refit against rebuild (add-bvh-refit, issue #194) -------------------
//
// A mesh layer's topology is fixed, so a brush that moves vertices leaves the
// tree's SHAPE valid and only its bounds stale. The pair below is the whole
// claim of that change: the rebuild is proportional to the MESH and the refit
// to the BRUSH, and the ratio gate in tools/check_bench.py is what holds it.
//
// The rebuild here is the operation a host pays today whenever it wants an
// honest pick during a stroke — measured at 1.3 s on a 2M-vertex model, against
// 0.25 ms for the stamp that dirtied it.
namespace {

mesh::Mesh bvh_bench_mesh(int n) {
    mesh::Mesh m;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float x = 2.0f * i / (n - 1) - 1.0f, z = 2.0f * j / (n - 1) - 1.0f;
            m.positions.push_back(
                cf3(x, 0.1f * std::sin(i * 0.3f) * std::cos(j * 0.2f), z));
        }
    for (int j = 0; j < n - 1; ++j)
        for (int i = 0; i < n - 1; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j * n + i), b = a + 1;
            const std::uint32_t c = a + static_cast<std::uint32_t>(n), d = c + 1;
            for (std::uint32_t k : {a, c, b, b, c, d}) m.indices.push_back(k);
        }
    return m;
}

// The triangles a brush-sized dab at the centre would touch: a fixed fraction
// of the mesh, so the refit's work is constant while the mesh grows.
std::vector<std::uint32_t> bvh_bench_dab(const mesh::Mesh& m, float radius) {
    std::vector<std::uint32_t> out;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const kernel::cfloat3 p = m.positions[m.indices[t * 3]];
        if (std::fabs(p.x) < radius && std::fabs(p.z) < radius)
            out.push_back(static_cast<std::uint32_t>(t));
    }
    return out;
}

constexpr int kBvhBenchGrid = 256;   // ~130k triangles
constexpr int kBvhBenchBig = 512;    // ~522k triangles, 4x the above

// A brush-sized patch, scaled with the grid so it holds the SAME number of
// triangles at every mesh size. That is what makes the pair below a statement
// about the slope rather than about one machine: the refit's work is constant
// and the rebuild's is not.
std::vector<std::uint32_t> bvh_bench_fixed_dab(const mesh::Mesh& m, int grid) {
    return bvh_bench_dab(m, 20.0f / static_cast<float>(grid));
}

}  // namespace

void BM_BvhRebuild(benchmark::State& state) {
    const mesh::Mesh m = bvh_bench_mesh(kBvhBenchGrid);
    for (auto _ : state) {
        mesh::Bvh tree = mesh::Bvh::build(m);
        benchmark::DoNotOptimize(tree.triangle_count());
    }
    state.counters["triangles"] = static_cast<double>(m.triangle_count());
}
BENCHMARK(BM_BvhRebuild)->Unit(benchmark::kMillisecond);

void BM_BvhRefitDab(benchmark::State& state) {
    mesh::Mesh m = bvh_bench_mesh(kBvhBenchGrid);
    mesh::Bvh tree = mesh::Bvh::build(m);
    const std::vector<std::uint32_t> dab = bvh_bench_fixed_dab(m, kBvhBenchGrid);
    float phase = 0.0f;
    for (auto _ : state) {
        // Move the dab's vertices a little each iteration, so the refit is
        // doing real work rather than re-fitting bounds that did not change.
        phase += 0.001f;
        for (std::uint32_t t : dab) m.positions[m.indices[t * 3]].y = phase;
        tree.refit(m, dab.data(), dab.size());
        benchmark::DoNotOptimize(tree.bounds().max.y);
    }
    state.counters["triangles"] = static_cast<double>(m.triangle_count());
    state.counters["dab"] = static_cast<double>(dab.size());
}
BENCHMARK(BM_BvhRefitDab)->Unit(benchmark::kMillisecond);

// The SAME dab on a mesh four times the size. The claim this change rests on is
// that a refit is proportional to the BRUSH, so these two must cost about the
// same while the rebuild beside them does not — which is a slope, and a single
// size cannot show one.
void BM_BvhRefitDabBig(benchmark::State& state) {
    mesh::Mesh m = bvh_bench_mesh(kBvhBenchBig);
    mesh::Bvh tree = mesh::Bvh::build(m);
    const std::vector<std::uint32_t> dab = bvh_bench_fixed_dab(m, kBvhBenchBig);
    float phase = 0.0f;
    for (auto _ : state) {
        phase += 0.001f;
        for (std::uint32_t t : dab) m.positions[m.indices[t * 3]].y = phase;
        tree.refit(m, dab.data(), dab.size());
        benchmark::DoNotOptimize(tree.bounds().max.y);
    }
    state.counters["triangles"] = static_cast<double>(m.triangle_count());
    state.counters["dab"] = static_cast<double>(dab.size());
}
BENCHMARK(BM_BvhRefitDabBig)->Unit(benchmark::kMillisecond);

void BM_VoxelAddLevelWhole(benchmark::State& state) {
    voxel::VoxelGrid g(0.02f);
    std::uint8_t c = g.palette_add(cf3(0.8f, 0.4f, 0.2f));
    // SCATTERED material, not one solid block. The shape matters more than the
    // amount: the cost this gates is a per-child chunk_key(), and a solid block
    // lives in a handful of chunks where write_cell's own bookkeeping dominates
    // it — measured at 1.14x there against 2.47x here, which would have let the
    // regression back in. This is the device case's own spread: the same
    // low-discrepancy walk over a 40-cell scale, a small blob at each stop.
    for (int i = 0; i < 400; ++i) {
        int bx = 0, by = 0, bz = 0;
        clay_bench::scatter_cell(i, 40, &bx, &by, &bz);
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
    for (int i = 0; i < 200; ++i) {
        int bx = 0, by = 0, bz = 0;
        clay_bench::scatter_cell(i, 40, &bx, &by, &bz);
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
        int bx = 0, by = 0, bz = 0;
        clay_bench::scatter_cell(n, 60, &bx, &by, &bz);
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

// The volume bake the document-sourced verbs reach — clay_item_volume_from_document
// and the _relax_from / _flatten_from pair — once through the pooled block fill
// and once through the per-point tape callable it replaced. Same pairing, same
// reason, as the consolidate pair above: check_bench.py requires the pooled bake
// to be FASTER, which is what catches the one-point-at-a-time bake coming back
// on THIS path. bake_layer was already gated; these three entry points were not,
// which is how they kept the serial walk for as long as they did.
//
// Byte-identical by contract, and held by
// "the pooled tape fill bakes the volume the per-point tape callable does" in
// test_consolidate.cpp rather than by this benchmark.
void BM_VolumeBakeDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const float cell = 0.05f, band = cell * 3.0f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    for (auto _ : state) {
        field::FieldVolume v = field::FieldVolume::sample_blocks(eval::tape_block_fill(tape),
                                                                 region, cell, band);
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_VolumeBakeDoc)->Unit(benchmark::kMillisecond);

void BM_VolumeBakeSerialDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const float cell = 0.05f, band = cell * 3.0f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    for (auto _ : state) {
        field::FieldVolume v = field::FieldVolume::sample(
            [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, region, cell, band);
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_VolumeBakeSerialDoc)->Unit(benchmark::kMillisecond);

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
        cfg.struct_size = sizeof(cfg);
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

// The coloured add combine against a reference that computes it ONCE (#225).
//
// This pair exists because of a regression that nothing in the tree could see.
// `split-the-combine` (#223) routed every mode's distance through
// `ctape_combine_dist` and let `ctape_combine_values` obtain the add case's
// blend weight from a SECOND `ctape_smin_m`, on the reasoning that both calls
// are pure with identical arguments so common-subexpression elimination makes
// the second one free. That holds on x86-64 and does NOT hold on AppleClang for
// arm64, which does not eliminate it: 3.40 -> 5.54 ns/call on an add-only mix,
// and 4055.9 -> 4992.3 ms end-to-end on the device gate's `mask_extrude`, the
// one device case that is a pure scalar coloured tape walk.
//
// Every existing instrument missed it, and for reasons worth writing down
// because they say what this guard has to be:
//
//   - the gated benchmarks here are threaded document workloads whose evaluation
//     is distance-only, so they take the blocked evaluator's specialisation and
//     never call the coloured combine at all. They got FASTER across #223.
//   - the device gate saw the 1.23x and passed it: `mask_extrude` read 1.13x
//     against its committed baseline, inside a 1.40 tolerance and under budget.
//     A tighter device baseline is worth having and is its own change; it would
//     not have made this visible from the CPU side, which is where the defect is.
//   - and an absolute ns/call threshold is not portable. This workload measures
//     ~2.8 ns/call here and CI runners are ~3x slower, so a number loose enough
//     not to flake there is far too loose to see 1.6x here.
//
// THE REFERENCE IS THE WHOLE DESIGN. The obvious control — the same combine
// asked for a distance only — is the wrong shape: it does structurally less
// work (no cmix, two loads instead of eight), so its healthy ratio is whatever
// the machine happens to charge for that difference. It reads 1.19x here and
// there is no way to know what it reads on a runner with a cheaper divide, so no
// single ceiling over it is defensible.
//
// `BM_TapeCombineAddColoredRef` instead does exactly the work a correct coloured
// add does — one `ctape_combine_dist` (which for add IS one `ctape_smin_m`), one
// `cmix` of the two colours, the same eight loads and the same four accumulator
// adds — differing only in where the mix weight comes from. So the healthy ratio
// is ~1.0 BY CONSTRUCTION rather than by measurement, on any machine, and the
// only thing that can move it is the kernel doing more work than the reference.
// Measured on an M-series Mac, medians of three at CI settings:
//
//   fixed     0.96 - 0.98x   (2.81 ns/call against 2.89)
//   post-#223 1.55 - 1.56x   (4.55 ns/call against 2.92)
//
// The reference does not move between those two builds (2.86-2.93 ns either
// way), which is the control this needs: the defect lands entirely in the ratio.
// On a toolchain that DOES eliminate the second call, both sides are unchanged
// and this reads its healthy value — the gate charges for the duplication where
// the duplication is actually paid, which is the machine it runs on.
//
// WHY ADD, AND WHY QUADRATIC. Add is the only mode whose colour couples to its
// distance — the weight that mixes the colours is the same smin that produces
// the distance — so it is the only mode where a duplicated evaluation is even
// possible, and it is most of what a sculpting tape is made of. The profile is
// quadratic because `cblend_hard`'s smin is a compare and a select: under it the
// duplicate costs almost nothing and this would go blind.
//
// THROUGHPUT, NOT LATENCY, and the first version of this got that wrong. Chained
// as an accumulator — each call's `a` carrying the previous call's distance —
// both sides read 9.96 ns/call and the ratio was 1.02x with the duplicate still
// in place: the chain runs through a divide, and a second smin computed from the
// same two operands is independent of it and hides in its shadow completely.
// The scalar evaluator is not in that regime. `ctape_eval` is called once per
// point and the points are independent, so the machine keeps as many combines in
// flight as it has room for and the duplicate costs real throughput — which is
// why the device saw 1.23x and a serial probe sees nothing. The loops below keep
// the calls INDEPENDENT, exactly as the per-point loop does.
//
// The operand set is small enough to stay in L1, so this measures the combine
// and not the memory system; the per-pass offset is what stops the compiler
// hoisting a repeated pass over an unchanging set.
namespace {

constexpr std::size_t kCombineOperands = 512;
constexpr int kCombinePasses = 1024;
constexpr std::size_t kCombineCalls = kCombineOperands * kCombinePasses;

struct CombineOperands {
    std::vector<kernel::CTapeValue> a;
    std::vector<kernel::CTapeValue> b;
    std::vector<float> k;
    std::vector<int> mode;
    std::vector<int> profile;
};

CombineOperands combine_operands() {
    CombineOperands ops;
    std::vector<float> r = random_points(kCombineOperands * 3, 1.0f);
    for (std::size_t i = 0; i < kCombineOperands; ++i) {
        const float* v = &r[i * 9];
        kernel::CTapeValue a;
        a.d = v[0] * 0.4f;
        a.color = cf3(v[1] * 0.5f + 0.5f, v[2] * 0.5f + 0.5f, v[3] * 0.5f + 0.5f);
        kernel::CTapeValue b;
        b.d = v[4] * 0.4f;
        b.color = cf3(v[5] * 0.5f + 0.5f, v[6] * 0.5f + 0.5f, v[7] * 0.5f + 0.5f);
        ops.a.push_back(a);
        ops.b.push_back(b);
        // Blend radii spanning the band, wide enough that most pairs land inside
        // the smin's support, which is where the mix weight is live.
        ops.k.push_back(0.05f + std::fabs(v[8]) * 0.25f);
        ops.mode.push_back(kernel::ccombine_add);
        ops.profile.push_back(kernel::cblend_quadratic);
    }
    return ops;
}

// The mode and the profile are read from memory rather than passed as literals,
// because the tape reads them from an instruction and the dispatch chain is part
// of what is being compared. The clobbers stop the compiler tracing the vectors'
// contents back to the loop that filled them and specialising the chain away on
// one side of the pair but not the other.
void clobber_operands(CombineOperands& ops) {
    benchmark::DoNotOptimize(ops.a.data());
    benchmark::DoNotOptimize(ops.b.data());
    benchmark::DoNotOptimize(ops.k.data());
    benchmark::DoNotOptimize(ops.mode.data());
    benchmark::DoNotOptimize(ops.profile.data());
}

void BM_TapeCombineAddColored(benchmark::State& state) {
    CombineOperands ops = combine_operands();
    clobber_operands(ops);
    float dsum = 0.0f;
    kernel::cfloat3 csum = cf3(0, 0, 0);
    for (auto _ : state) {
        for (int pass = 0; pass < kCombinePasses; ++pass) {
            const float po = 1e-7f * static_cast<float>(pass);
            for (std::size_t i = 0; i < kCombineOperands; ++i) {
                kernel::CTapeValue a = ops.a[i];
                a.d += po;
                kernel::CTapeValue r = kernel::ctape_combine_values(
                    a, ops.b[i], ops.mode[i], ops.profile[i], ops.k[i], 0.0f);
                // Both halves have to be CONSUMED. Drop the colour and the cmix
                // is dead and this stops being the coloured path at all.
                dsum += r.d;
                csum.x += r.color.x;
                csum.y += r.color.y;
                csum.z += r.color.z;
            }
        }
    }
    benchmark::DoNotOptimize(dsum);
    benchmark::DoNotOptimize(csum);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kCombineCalls));
}
BENCHMARK(BM_TapeCombineAddColored)->Unit(benchmark::kMillisecond);

// One smin and one cmix, in the same loop, over the same operands, consuming the
// same two results. The mix weight is taken from the distance rather than from
// the smin's second half — a correct coloured add gets it for free out of the
// call it already made, so this is if anything the SLOWER of the two ways to
// spend it, and the gate is the safer for that.
void BM_TapeCombineAddColoredRef(benchmark::State& state) {
    CombineOperands ops = combine_operands();
    clobber_operands(ops);
    float dsum = 0.0f;
    kernel::cfloat3 csum = cf3(0, 0, 0);
    for (auto _ : state) {
        for (int pass = 0; pass < kCombinePasses; ++pass) {
            const float po = 1e-7f * static_cast<float>(pass);
            for (std::size_t i = 0; i < kCombineOperands; ++i) {
                float d = kernel::ctape_combine_dist(ops.a[i].d + po, ops.b[i].d, ops.mode[i],
                                                     ops.profile[i], ops.k[i], 0.0f);
                kernel::cfloat3 c = kernel::cmix(ops.a[i].color, ops.b[i].color, d);
                dsum += d;
                csum.x += c.x;
                csum.y += c.y;
                csum.z += c.z;
            }
        }
    }
    benchmark::DoNotOptimize(dsum);
    benchmark::DoNotOptimize(csum);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kCombineCalls));
}
BENCHMARK(BM_TapeCombineAddColoredRef)->Unit(benchmark::kMillisecond);

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
