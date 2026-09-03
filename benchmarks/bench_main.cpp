// Performance benchmarks with CI regression gates (build-packaging spec):
// points/sec, bricks/sec, mesh time on fixed scenes. tools/check_bench.py
// enforces generous floor thresholds in CI — they catch order-of-magnitude
// regressions; tight deltas need dedicated hardware.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <optional>
#include <thread>
#include <vector>

#include "clay.h"
#include "clay_internal.h"
#include "clay/brick/cache.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/brush/move.h"
#include "clay/eval/backend.h"
#include "clay/eval/bake_points.h"
#include "clay/eval/bake_volume.h"
#include "clay/field/move_topological.h"
#include "clay/field/redistance.h"
#include "clay/field/relax.h"
#include "clay/kernel/field.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/voxel_remesh.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/tape.h"
#include "clay/session/sdf_sculpt.h"
#include "clay/voxel/grid.h"
#include "scatter_spread.h"

using namespace clay;
using kernel::cf3;

// The backends' upload and patch counters: a backend-private test hook,
// declared here rather than published in a header, the same way the backends'
// own tests reach them. Guarded because the symbols exist only where the
// backend was built — the resident/reupload Metal pair below needs no guard,
// since it reaches nothing beyond the registry, but the stroke pair does.
#if defined(CLAY_HAS_VULKAN)
namespace clay {
namespace eval {
std::uint64_t vulkan_tape_uploads(const Backend& backend);
std::uint64_t vulkan_tape_patches(const Backend& backend);
}  // namespace eval
}  // namespace clay
#endif
#if defined(CLAY_HAS_METAL)
namespace clay {
namespace eval {
std::uint64_t metal_tape_uploads(const Backend& backend);
std::uint64_t metal_tape_patches(const Backend& backend);
}  // namespace eval
}  // namespace clay
#endif

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

scene::Document deep_sphere(int nodes, float k = 0.03f) {
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
        dab.blend = scene::Blend{scene::BlendProfile::Quadratic, k};
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

// -- the whole-document tape after an append (#197 phase 1) ------------------
//
// A host that raycasts to place the next dab reads the WHOLE-document tape,
// which is cached on the document revision and so thrown away by every edit.
// The pair below is the cost of that rebuild, with and without reusing the
// compiled prefix.
//
// What the ratio is bounded by is worth stating, because it is not the
// instruction count. Reuse turns an O(N) COMPILE into an O(N) MEMCPY: it
// removes the per-item work — influence bounds, transform inversion, curve
// tessellation, info folding — but not the byte movement, and params are ~90%
// of the bytes. Measured on this machine at 50k items: 7.58 ms to compile,
// 0.83 ms to copy 7.82 MiB and append, so roughly 9x and no more. A result
// far below that means something is copying twice.
//
// The append rows measure ONE rebuild on a document of the size in their
// name: the dab is appended once and re-compiled onto the same prefix every
// iteration.
//
// AND IT LEAVES THE GPU RE-UPLOAD UNTOUCHED. The reused tape has different
// bytes and so a different compile_id, which is a guaranteed miss in the
// Metal backend's resident-tape cache and a full re-emit for Vulkan's
// memcmp. #197 is not closed by this pair improving; that needs the tape
// identity to carry a generation and a dirty range.
void deep_doc_whole_compile(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    for (auto _ : state) {
        scene::Tape tape = scene::compile_document(doc);
        benchmark::DoNotOptimize(tape.instrs.size());
        state.counters["instrs"] = static_cast<double>(tape.instrs.size());
    }
    state.counters["nodes"] = static_cast<double>(nodes);
}

void deep_doc_whole_append(benchmark::State& state, int nodes) {
    scene::Document doc = deep_sphere(nodes);
    scene::TapeCheckpoint base_cp;
    const scene::Tape base = scene::compile_document_resumable(doc, &base_cp);

    // The dab is appended ONCE, and every iteration rebuilds the same tape
    // from the same prefix. Appending per iteration instead would grow the
    // document as the benchmark ran — at 1 000 nodes it reached 11 000 — and
    // the row would report an average over sizes rather than the size in its
    // name, which is not comparable with the compile row above it.
    scene::Node dab;
    dab.prim = scene::Prim::sphere(0.05f);
    dab.xform.position = cf3(0.0f, 1.0f, 0.0f);
    dab.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.03f};
    const scene::NodeId id = doc.layers[0].sdf->insert(dab);

    for (auto _ : state) {
        scene::Tape grown;
        if (!scene::compile_document_append(base, base_cp, doc, {id}, &grown, nullptr)) {
            state.SkipWithError("the append was refused; the fast path is not being measured");
            return;
        }
        benchmark::DoNotOptimize(grown.instrs.size());
        state.counters["instrs"] = static_cast<double>(grown.instrs.size());
    }
    state.counters["nodes"] = static_cast<double>(doc.layers[0].sdf->roots.size());
}

// NAMED, not Arg()-parameterised: check_bench.py keys its gate on the part of
// the name before "/", so BM_X/50000 and BM_X/1000 would collapse to one key.
void BM_WholeDocCompile1000(benchmark::State& state) { deep_doc_whole_compile(state, 1000); }
BENCHMARK(BM_WholeDocCompile1000)->Unit(benchmark::kMillisecond);
void BM_WholeDocCompile10000(benchmark::State& state) { deep_doc_whole_compile(state, 10000); }
BENCHMARK(BM_WholeDocCompile10000)->Unit(benchmark::kMillisecond);
void BM_WholeDocCompile50000(benchmark::State& state) { deep_doc_whole_compile(state, 50000); }
BENCHMARK(BM_WholeDocCompile50000)->Unit(benchmark::kMillisecond);

void BM_WholeDocAppend1000(benchmark::State& state) { deep_doc_whole_append(state, 1000); }
BENCHMARK(BM_WholeDocAppend1000)->Unit(benchmark::kMillisecond);
void BM_WholeDocAppend10000(benchmark::State& state) { deep_doc_whole_append(state, 10000); }
BENCHMARK(BM_WholeDocAppend10000)->Unit(benchmark::kMillisecond);
void BM_WholeDocAppend50000(benchmark::State& state) { deep_doc_whole_append(state, 50000); }
BENCHMARK(BM_WholeDocAppend50000)->Unit(benchmark::kMillisecond);

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
void deep_doc_cull_planned(benchmark::State& state, int nodes, float k = 0.03f) {
    scene::Document doc = deep_sphere(nodes, k);
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
        std::size_t survived = 0;
        for (const brick::BrickKey& key : dab) {
            scene::CullRegion cull{cache.cull_region(key)};
            scene::Tape tape = scene::compile_document(doc, &cull, &index, &plan);
            survived += tape.instrs.size();
            benchmark::DoNotOptimize(tape.instrs.size());
        }
        // What the cull actually left, which is the deterministic half of this
        // measurement: the time is a machine's, the survivor count is the
        // pad's, and a change to either shows up here first.
        state.counters["instrs"] = static_cast<double>(survived);
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

// THE SAME CULL AT A SCULPT'S BLEND RADIUS, and the pair is the point (#335).
//
// Everything above blends at k = 0.03. #282's chain pad is the largest
// single-item reach in the layer, which for a quadratic profile is 4k, and it
// is added to a cull region that is a fixed brick plus band — so the survivor
// count grows superlinearly in k while every gate here sat at one value of it.
// The pad's cost was recorded as "20-35% on the DeepDocCull benchmarks" and
// measured 1.87x on a document that blends at 0.06, which is an ordinary
// sculpt and is what ClaySpaceDesktop reported as a 1.76x frame-path
// regression it could not find in any fixture we had.
//
// Held as a RATIO against the k = 0.03 row rather than a ceiling: the absolute
// number is a machine's, the spread between the two blend radii is the pad's,
// and a change that widens the pad again moves this and nothing else.
void BM_DeepDocCullPlanned2000K06(benchmark::State& state) {
    deep_doc_cull_planned(state, 2000, 0.06f);
}
BENCHMARK(BM_DeepDocCullPlanned2000K06)->Unit(benchmark::kMillisecond);

// THE CULL PLAN ALONE, ON A DOCUMENT THAT GROWS IN EXTENT.
//
// Every SDF fixture above -- sculpted_sphere, pole_dense_sphere, deep_sphere,
// spread_sculpt -- is a UNIT SPHERE with a growing node COUNT. A dab's cull
// region therefore keeps covering the same FRACTION of the model at every
// size, and the survivor count grows with the document: measured at a flat
// 28.3% of the items at 2 000, 10 000 and 50 000 alike. On such a document
// `plan` is ~3% of a dab's cull and the per-brick compiles over its survivors
// are the other 97%, so a broad phase that answered instantly would win 3%.
//
// That is not a property of the engine, it is a property of the fixture, and it
// is how `add-item-spatial-index` came to be measured as a 590x faster query
// inside a 2.4x slower operation (ROADMAP P1). A sculpt does not get denser
// forever at a fixed size; it gets BIGGER. This fixture is the other axis --
// dabs at a fixed spacing over a growing sheet, so a fixed region keeps a flat
// number of survivors (36 here at every size) and `plan` is what grows.
//
// Timed ALONE, with the index built outside the loop, because that is how a
// stroke pays it: the C ABI keeps one index per revision and extends it with
// `CullIndex::append`, so a stamp pays one append and one plan, not a build.
// The same reason `check_bench.py` gates this on a ratio -- what a broad phase
// changes is the SLOPE, and an absolute ceiling on a 0.02 ms row is noise.
namespace {

scene::Document spread_grid(int nodes) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("sculpt");
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(nodes))));
    int made = 0;
    for (int i = 0; i < side && made < nodes; ++i)
        for (int j = 0; j < side && made < nodes; ++j, ++made) {
            scene::Node dab;
            dab.prim = scene::Prim::sphere(0.05f);
            dab.xform.position = cf3(static_cast<float>(i) * 0.15f, 0.0f,
                                     static_cast<float>(j) * 0.15f);
            dab.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.03f};
            l.sdf->insert(dab);
        }
    return doc;
}

void cull_plan_local(benchmark::State& state, int nodes) {
    scene::Document doc = spread_grid(nodes);
    brick::BrickCache cache(brick::BrickConfig{8, 0.05f, 3, 0});
    // A dab's worth of bricks at the sheet's corner, which is where the grid
    // starts however far it runs -- so the region is the same box at every size
    // and the survivor count is the thing held flat.
    math::Aabb batch;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                math::Aabb brick;
                brick.expand(cf3(0.4f * i, 0.4f * j, 0.4f * k));
                brick.expand(cf3(0.4f * (i + 1), 0.4f * (j + 1), 0.4f * (k + 1)));
                batch.expand(brick.dilated(cache.config().band()));
            }
    const scene::CullIndex index(doc);
    for (auto _ : state) {
        const scene::CullPlan plan = index.plan(batch);
        benchmark::DoNotOptimize(&plan);
    }
    const scene::CullPlan plan = index.plan(batch);
    const scene::Layer& layer = doc.layers[0];
    const std::vector<scene::CullIndex::Entry>* kept = plan.chain(layer, layer.sdf->roots);
    // The deterministic half, and the premise the whole row rests on: this must
    // NOT grow with `nodes`. If it ever does, the fixture stopped being local
    // and the ratio gate below is measuring something else.
    state.counters["survivors"] = static_cast<double>(kept ? kept->size() : 0);
    state.counters["nodes"] = static_cast<double>(nodes);
}

}  // namespace

void BM_CullPlanLocal10000(benchmark::State& state) { cull_plan_local(state, 10000); }
BENCHMARK(BM_CullPlanLocal10000)->Unit(benchmark::kMillisecond);
void BM_CullPlanLocal50000(benchmark::State& state) { cull_plan_local(state, 50000); }
BENCHMARK(BM_CullPlanLocal50000)->Unit(benchmark::kMillisecond);

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

// Continuing the fold against replaying it (#306). A dab's cost follows
// everything already sculpted, because a dirty brick re-evaluates every
// surviving item over its samples even though almost none of them changed;
// `compile_layer_suffix` + `eval_points_seeded` run only what the dab adds,
// onto the value the rest produced.
//
// The pair holds the scaling law rather than a percentage: the suffix is two
// instructions whatever the document holds, so the gap widens with the sculpt.
// At 10,000 dabs spread over a sphere the full walk compiles 16,000-odd
// instructions for 12 bricks and the suffix compiles none.
namespace {
scene::Document spread_sculpt(int nodes) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("s");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    for (int i = 1; i < nodes; ++i) {
        scene::Node d;
        d.prim = scene::Prim::sphere(0.04f);
        const double z = 1.0 - 2.0 * (i + 0.5) / nodes;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double th = 2.399963 * i;
        const double a = r * std::cos(th), b = r * std::sin(th);
        d.xform.position = cf3(static_cast<float>(std::sqrt(std::max(0.0, 1.0 - a * a - b * b))),
                               static_cast<float>(a), static_cast<float>(b));
        l.sdf->insert(d);
    }
    return doc;
}

// The dab's dirty bricks, and the lattice points of each.
struct DabWork {
    brick::BrickCache cache{brick::BrickConfig{8, 0.05f, 3, 0}};
    std::vector<brick::BrickRequest> reqs;
    std::vector<std::vector<float>> points;
    DabWork() {
        cache.mark_dirty(math::Aabb{cf3(0.90f, -0.08f, -0.08f), cf3(1.06f, 0.08f, 0.08f)});
        reqs = cache.take_dirty();
        points.resize(reqs.size());
        for (std::size_t s = 0; s < reqs.size(); ++s) {
            const auto& g = reqs[s].grid;
            points[s].resize(static_cast<std::size_t>(g.nx) * g.ny * g.nz * 3);
            std::size_t at = 0;
            for (int k = 0; k < g.nz; ++k)
                for (int j = 0; j < g.ny; ++j)
                    for (int i = 0; i < g.nx; ++i) {
                        const kernel::cfloat3 p = g.origin + cf3(static_cast<float>(i) * g.spacing,
                                                                 static_cast<float>(j) * g.spacing,
                                                                 static_cast<float>(k) * g.spacing);
                        points[s][at * 3] = p.x;
                        points[s][at * 3 + 1] = p.y;
                        points[s][at * 3 + 2] = p.z;
                        ++at;
                    }
        }
    }
};
constexpr int kSuffixDabNodes = 10000;
}  // namespace

void BM_DabFullWalk(benchmark::State& state) {
    const scene::Document doc = spread_sculpt(kSuffixDabNodes);
    DabWork w;
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    std::int64_t instrs = 0;
    for (auto _ : state) {
        std::int64_t per_iter = 0;
        const scene::CullIndex index(doc);
        math::Aabb batch;
        for (const auto& q : w.reqs) batch.expand(w.cache.cull_region(q.key));
        const scene::CullPlan plan = index.plan(batch);
        for (const auto& q : w.reqs) {
            scene::CullRegion cr{w.cache.cull_region(q.key)};
            const scene::Tape tp = scene::compile_document(doc, &cr, &index, &plan);
            per_iter += static_cast<std::int64_t>(tp.instrs.size());
            std::vector<float> v(static_cast<std::size_t>(q.grid.nx) * q.grid.ny * q.grid.nz);
            cpu->eval_grid(tp, q.grid, v.data());
        }
        instrs = per_iter;  // every brick's culled tape, this dab
    }
    state.counters["instrs"] = static_cast<double>(instrs);
}
BENCHMARK(BM_DabFullWalk)->Unit(benchmark::kMillisecond);

void BM_DabSuffixSeeded(benchmark::State& state) {
    const scene::Document doc = spread_sculpt(kSuffixDabNodes);
    const scene::Document before = spread_sculpt(kSuffixDabNodes - 1);
    DabWork w;
    scene::TapeCheckpoint cp;
    scene::compile_document_resumable(before, &cp);
    const std::vector<scene::NodeId>& roots = doc.layers[0].sdf->roots;
    scene::Tape suffix;
    if (!scene::compile_layer_suffix(cp, doc, {roots.back()}, &suffix, nullptr)) {
        state.SkipWithError("the checkpoint was refused");
        return;
    }
    // The seed a cache would hold: what everything before the dab says at these
    // lattice points. Paid once here, as it would be paid once per stroke --
    // during a stroke each dab's own result is the next dab's seed.
    std::vector<std::vector<float>> seeds(w.reqs.size());
    for (std::size_t s = 0; s < w.reqs.size(); ++s) {
        const std::size_t n = w.points[s].size() / 3;
        seeds[s].assign(n, 0.0f);
        eval::PointQuery q;
        q.points_xyz = w.points[s].data();
        q.count = n;
        eval::PointResults r;
        r.distances = seeds[s].data();
        scene::CullRegion cr{w.cache.cull_region(w.reqs[s].key)};
        eval::eval_points_blocked(scene::compile_document(before, &cr), q, r);
    }
    for (auto _ : state) {
        for (std::size_t s = 0; s < w.reqs.size(); ++s) {
            const std::size_t n = seeds[s].size();
            std::vector<float> v(n);
            eval::PointQuery q;
            q.points_xyz = w.points[s].data();
            q.count = n;
            eval::PointResults r;
            r.distances = v.data();
            eval::eval_points_seeded(suffix, q, seeds[s].data(), nullptr, r);
        }
    }
    state.counters["instrs"] = static_cast<double>(suffix.instrs.size());
}
BENCHMARK(BM_DabSuffixSeeded)->Unit(benchmark::kMillisecond);

// Rebuilding the cull index per dab against extending it (#306 follow-up, and
// the other half of the pair above: seeding a suffix removes the per-brick
// compile and the evaluation, and then this IS the dab). A stroke appends one
// item per stamp and every stamp bumps the revision, so the index is rebuilt
// from scratch -- walking every node recomputing bounds that did not move.
// 2.42 ms at 50,000 items, of which 2.29 ms is bounds and 0.15 ms the pad.
//
// Same fixture as the pair above, deliberately: the two measure the two costs
// of one dab over one document.
namespace {
constexpr int kIndexAppendNodes = 20000;
// A tenth of the document, for the SLOPE: extending an index costs what the
// dab adds, so the two sizes must measure the same. See tools/check_bench.py.
constexpr int kIndexAppendSmallNodes = 2000;
// A FIXED iteration count, and a fixture rebuilt every kIndexAppendReset of
// them. Every append GROWS the document it appends to, so left to the clock
// these measure a document whose size is a property of the MACHINE -- the
// faster side runs more iterations and so grows more -- which is exactly what
// a slope gate must not depend on. Rebuilt, both sizes sweep the same
// [nodes, nodes + 256] on every machine.
constexpr int kIndexAppendIters = 1024;
constexpr int kIndexAppendReset = 256;
}  // namespace

// A brick refill that RESUMES against one that replays (#306). The refill's own
// float32 output is the accumulator the edit list reached at that brick's
// lattice, so keeping it is all a dab needs to cost what the dab adds. Both
// sides are bit-identical by contract (test_c_tape_cache.cpp).
namespace {
constexpr int kRefillBricks = 12;

std::vector<clay_brick_request> pole_requests() {
    std::vector<clay_brick_request> reqs(kRefillBricks);
    for (int i = 0; i < kRefillBricks; ++i) {
        std::memset(&reqs[i], 0, sizeof(reqs[i]));
        reqs[i].key[0] = 2;
        reqs[i].key[1] = (i % 4) - 2;
        reqs[i].key[2] = (i / 4) - 1;
        for (int a = 0; a < 3; ++a)
            reqs[i].origin[a] = static_cast<float>(reqs[i].key[a]) * 8 * 0.05f;
        reqs[i].spacing = 0.05f;
        reqs[i].dims[0] = reqs[i].dims[1] = reqs[i].dims[2] = 8;
        reqs[i].band = 0.15f;
    }
    return reqs;
}

// A document of `nodes` dabs over a sphere, through the C ABI so the caches
// under test are the ones a host actually drives.
clay_document* abi_sculpt(int nodes) {
    clay_document* d = clay_document_create();
    clay_layer_id l = 0;
    clay_add_sdf_layer(d, "s", &l);
    auto add = [&](float r, float x, float y, float z) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float p[3] = {x, y, z};
        clay_item_set_position(it, p);
        clay_layer_add_item(d, l, it, nullptr);
        clay_item_destroy(it);
    };
    add(1.0f, 0, 0, 0);
    for (int i = 1; i < nodes; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / nodes;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double th = 2.399963 * i;
        const double a = r * std::cos(th), b = r * std::sin(th);
        add(0.05f, static_cast<float>(std::sqrt(std::max(0.0, 1.0 - a * a - b * b))),
            static_cast<float>(a), static_cast<float>(b));
    }
    return d;
}

// A MOVE DRAG, through the C ABI, because that is where its cost is.
//
// `clay_layer_move_surface` issues one SetDeformersCmd per node the drag
// reaches -- 257 of them over a 1,000-item document -- and each one used to pay
// two `command_influence_bound` calls and a seed-store walk inside apply_edit.
// That is how the path lost 1.34x when region invalidation landed and kept it
// for four releases (#358): no CPU benchmark covered the drag, and the only
// gate that did was the device suite, which cannot run in CI.
//
// The document is rebuilt per iteration, untimed, because a drag PREPENDS a
// warp to every node it catches: measured without the rebuild the chains grow
// as the benchmark runs and the row reports an average over depths rather than
// the depth in its name.
void abi_move_drag(benchmark::State& state, int nodes) {
    clay_move_params params{};
    params.struct_size = sizeof(params);
    params.radius = 0.4f;
    params.ease = 0;
    params.front_only = 0;
    // ON the shell the dabs sit on, not at the origin: `abi_sculpt` puts its
    // dabs over the unit sphere, so a drag at the centre reaches exactly one
    // item and measures a loop that never runs.
    const float centre[3] = {1.0f, 0.0f, 0.0f};
    const float disp[3] = {0.05f, 0.0f, 0.0f};

    std::size_t applied = 0;
    for (auto _ : state) {
        state.PauseTiming();
        clay_document* d = abi_sculpt(nodes);
        state.ResumeTiming();
        clay_layer_move_surface(d, 1, centre, disp, &params, &applied);
        state.PauseTiming();
        clay_document_destroy(d);
        state.ResumeTiming();
    }
    // A drag that reached nothing would be an empty loop reporting a fast row.
    if (applied == 0) state.SkipWithError("the drag warped no node; nothing is being measured");
    state.counters["warped"] = static_cast<double>(applied);
    state.counters["nodes"] = static_cast<double>(nodes);
}

void BM_MoveDrag1000(benchmark::State& state) { abi_move_drag(state, 1000); }
BENCHMARK(BM_MoveDrag1000)->Unit(benchmark::kMillisecond);
void BM_MoveDrag10000(benchmark::State& state) { abi_move_drag(state, 10000); }
BENCHMARK(BM_MoveDrag10000)->Unit(benchmark::kMillisecond);

// The same drag under a LAYER MIRROR (#363). abi_sculpt puts every dab on the
// +x hemisphere, so the mirror puts their copies at x < 0 and the ball at
// (1, 0, 0) touches no copy: a drag that selects by what the ball or its
// reflection touches warps exactly the items the unmirrored drag does, and
// one that selects on the mirror-expanded bound warps every item whose
// expanded bound spans the plane -- 46 items against 22 on the ridge fixture
// of #363, each extra one a grab that does nothing. `warped_ratio` is the
// mirrored count over the unmirrored one, in
// ITEMS (a straddler counts once), resolved untimed through the preview on a
// fresh unmirrored document; the gate on it is machine-independent where a
// time would not be.
void BM_MoveDragMirrored1000(benchmark::State& state) {
    clay_move_params params{};
    params.struct_size = sizeof(params);
    params.radius = 0.4f;
    const float centre[3] = {1.0f, 0.0f, 0.0f};
    const float disp[3] = {0.05f, 0.0f, 0.0f};

    std::size_t unmirrored = 0;
    {
        clay_document* d = abi_sculpt(1000);
        clay_layer_move_surface_preview(d, 1, centre, disp, &params, nullptr, 0, &unmirrored);
        clay_document_destroy(d);
    }
    std::size_t applied = 0;
    for (auto _ : state) {
        state.PauseTiming();
        clay_document* d = abi_sculpt(1000);
        clay_set_layer_mirror(d, 1, 1, 0, 0, 0.05f);
        state.ResumeTiming();
        clay_layer_move_surface(d, 1, centre, disp, &params, &applied);
        state.PauseTiming();
        clay_document_destroy(d);
        state.ResumeTiming();
    }
    if (applied == 0 || unmirrored == 0)
        state.SkipWithError("the drag warped no node; nothing is being measured");
    state.counters["warped"] = static_cast<double>(applied);
    state.counters["warped_ratio"] =
        static_cast<double>(applied) / static_cast<double>(unmirrored);
}
BENCHMARK(BM_MoveDragMirrored1000)->Unit(benchmark::kMillisecond);


constexpr int kRefillHistory = 5000;

void refill_stroke(benchmark::State& state, bool prime) {
    clay_document* d = abi_sculpt(kRefillHistory);
    const std::vector<clay_brick_request> reqs = pole_requests();
    const std::size_t per = 8 * 8 * 8;
    std::vector<float> out(static_cast<std::size_t>(kRefillBricks) * per);
    // Priming stores the seeds the resumed form continues from. Without it
    // every call is the full walk, which is the control.
    if (prime)
        clay_brick_cache_eval_requests(d, nullptr, reqs.data(), kRefillBricks, out.data(),
                                       out.size(), nullptr, 0);
    float y = 0.0f;
    for (auto _ : state) {
        state.PauseTiming();
        clay_item* it = nullptr;
        const float r = 0.05f;
        it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float p[3] = {0.98f, y, -0.1f};
        y += 0.001f;
        clay_item_set_position(it, p);
        clay_layer_add_item(d, 1, it, nullptr);
        clay_item_destroy(it);
        if (!prime) {
            // The control: a document that never keeps a seed. Rebuilt rather
            // than reused, so the comparison is refill against refill.
            clay_document_destroy(d);
            d = abi_sculpt(kRefillHistory);
        }
        state.ResumeTiming();
        clay_brick_cache_eval_requests(d, nullptr, reqs.data(), kRefillBricks, out.data(),
                                       out.size(), nullptr, 0);
    }
    state.counters["history"] = static_cast<double>(kRefillHistory);
    clay_document_destroy(d);
}
}  // namespace

void BM_BrickRefillResumed(benchmark::State& state) { refill_stroke(state, true); }
BENCHMARK(BM_BrickRefillResumed)->Unit(benchmark::kMillisecond);

void BM_BrickRefillFull(benchmark::State& state) { refill_stroke(state, false); }
BENCHMARK(BM_BrickRefillFull)->Unit(benchmark::kMillisecond);

// A DAB'S WORTH OF DIRTY BRICKS ON A SCULPTED SURFACE, cold, through the
// library refill: the case the uniform-brick gate exists for. A dab dirties
// the solid box its influence bound covers, and on a worked model most of
// that box is clay -- 57 of the 80 bricks here are uniformly inside -- so
// most of a cold refill used to be 512 walks per brick to discover the brick
// stores nothing. The gate proves it from one evaluation and the tape's
// Lipschitz bound (bindings/c/clay_c.cpp, prove_uniform).
//
// bench_unspent's fixture and window: a sphere r=0.5 plus `dabs` smooth-union
// dabs on a 3-turn helix, a dim-8 cache at a 0.01 voxel and a 3-voxel band,
// the box one more dab at (0.5, 0, 0) with reach 0.05 + 0.06 would touch.
// A FRESH DOCUMENT per iteration, untimed: seeds belong to the document, so
// a second refill of the same one would resume rather than walk, and this
// row is the cold walk. Measured 9.22 -> 5.58 ms at 400 dabs and 33.4 -> 14.1
// ms at 1,500 on an M2 Max (medians of 7). The warm dab is
// BM_BrickRefillResumed's shape and is held elsewhere; what this row catches
// is the gate switching off, which reads as a correct document and 2x.
namespace {
clay_document* helix_sculpt(int dabs) {
    clay_document* d = clay_document_create();
    clay_layer_id l = 0;
    clay_add_sdf_layer(d, "s", &l);
    auto add = [&](float r, float x, float y, float z, float k) {
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float p[3] = {x, y, z};
        clay_item_set_position(it, p);
        if (k > 0.0f) clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, k);
        clay_layer_add_item(d, l, it, nullptr);
        clay_item_destroy(it);
    };
    add(0.5f, 0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < dabs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(dabs);
        const float a = t * 6.2831853f * 3.0f;
        const float z = -0.9f + 1.8f * t;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z)) * 0.5f;
        add(0.05f, r * std::cos(a), r * std::sin(a), z * 0.5f, 0.06f);
    }
    return d;
}

void dab_refill_sculpted(benchmark::State& state, int dabs) {
    clay_brick_config cfg{};
    cfg.struct_size = sizeof(cfg);
    clay_brick_config_defaults(&cfg);
    cfg.dim = 8;
    cfg.voxel_size = 0.01f;
    cfg.band_voxels = 3;
    cfg.memory_budget = 0;
    cfg.colors = 0;
    const float reach = 0.05f + 0.06f;
    const float lo[3] = {0.5f - reach, -reach, -reach};
    const float hi[3] = {0.5f + reach, reach, reach};
    std::size_t bricks = 0;
    for (auto _ : state) {
        state.PauseTiming();
        clay_document* d = helix_sculpt(dabs);
        clay_brick_cache* c = clay_brick_cache_create(&cfg);
        clay_brick_cache_mark_dirty(c, lo, hi);
        std::vector<clay_brick_request> reqs(4096);
        std::size_t count = reqs.size(), remaining = 0;
        clay_brick_cache_take_dirty(c, reqs.data(), &count, &remaining);
        reqs.resize(count);
        bricks = count;
        std::vector<float> values(count * 512);
        std::vector<int32_t> results(count);
        std::size_t accepted = 0;
        state.ResumeTiming();
        clay_brick_cache_eval_requests(d, "cpu", reqs.data(), count, values.data(), values.size(),
                                       nullptr, 0);
        clay_brick_cache_submit(c, reqs.data(), count, values.data(), values.size(), nullptr, 0,
                                results.data(), &accepted);
        state.PauseTiming();
        clay_brick_cache_destroy(c);
        clay_document_destroy(d);
        state.ResumeTiming();
    }
    state.counters["bricks"] = static_cast<double>(bricks);
    state.counters["dabs"] = static_cast<double>(dabs);
}
}  // namespace

void BM_DabRefillSculpted400(benchmark::State& state) { dab_refill_sculpted(state, 400); }
BENCHMARK(BM_DabRefillSculpted400)->Unit(benchmark::kMillisecond);

void BM_DabRefillSculpted1500(benchmark::State& state) { dab_refill_sculpted(state, 1500); }
BENCHMARK(BM_DabRefillSculpted1500)->Unit(benchmark::kMillisecond);

// The pair above holds one window still. A REAL stroke drags its dirty window
// across the model, and that is the case the resumed path used to lose: a
// refill re-stamps only the bricks it filled and an append re-stamps none, so
// every dab mixed the ground the last one covered with ground it had not, and
// the batch-wide admission gate spent one un-resumable brick on all of them.
//
// The window here slides one brick every third dab, so three dabs in four ask
// for bricks that are entirely warm and the fourth brings in one new one. It
// should cost what the dab adds either way; before #342 it cost what the sculpt
// holds, every dab.
namespace {
void refill_moving(benchmark::State& state, int history) {
    clay_document* d = abi_sculpt(history);
    const std::size_t per = 8 * 8 * 8;
    constexpr int kWindow = 4;
    std::vector<float> out(static_cast<std::size_t>(kWindow) * per);
    // Along the sphere's EQUATOR (key[1] = key[2] = -1), not up its side. Every
    // brick of the swept range then either straddles the surface or lies inside
    // it, and both carry an accumulator. A window that runs off the shape holds
    // bricks whose culled prefix produced nothing at all, which are correctly
    // walked in full for ever -- a legitimate refusal that would sit in this
    // benchmark's counter pretending to be the defect it is gating.
    auto window = [&](int shift) {
        std::vector<clay_brick_request> reqs(kWindow);
        for (int i = 0; i < kWindow; ++i) {
            std::memset(&reqs[i], 0, sizeof(reqs[i]));
            reqs[i].key[0] = -3 + shift + i;
            reqs[i].key[1] = -1;
            reqs[i].key[2] = -1;
            for (int a = 0; a < 3; ++a)
                reqs[i].origin[a] = static_cast<float>(reqs[i].key[a]) * 8 * 0.05f;
            reqs[i].spacing = 0.05f;
            reqs[i].dims[0] = reqs[i].dims[1] = reqs[i].dims[2] = 8;
            reqs[i].band = 0.15f;
        }
        return reqs;
    };
    // Prime EVERY window position the loop will ask for, so no brick inside the
    // timed region is one the stroke has never reached. That is deliberate: a
    // brick with no seed is correctly walked in full, and leaving that in makes
    // the counter below a warmup transient amortised over the iteration count --
    // which is a property of how fast the runner is, not of the code. Primed,
    // what remains is purely bricks that HAVE a seed at a revision the window's
    // neighbours do not share, which is the thing under test.
    for (int shift = 0; shift < 3; ++shift)
        clay_brick_cache_eval_requests(d, nullptr, window(shift).data(), kWindow, out.data(),
                                       out.size(), nullptr, 0);
    int n = 0;
    float y = 0.0f;
    for (auto _ : state) {
        state.PauseTiming();
        const float r = 0.05f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float p[3] = {0.98f, y, -0.1f};
        y += 0.001f;
        clay_item_set_position(it, p);
        clay_layer_add_item(d, 1, it, nullptr);
        clay_item_destroy(it);
        const std::vector<clay_brick_request> reqs = window((n++ / 3) % 3);
        state.ResumeTiming();
        clay_brick_cache_eval_requests(d, nullptr, reqs.data(), kWindow, out.data(), out.size(),
                                       nullptr, 0);
    }
    clay_resume_stats rs{};
    rs.struct_size = sizeof rs;
    clay_document_resume_stats(d, &rs);
    state.counters["history"] = static_cast<double>(history);
    // The share of bricks that had to be WALKED IN FULL, which is the gate.
    // Every brick the loop asks for is primed, so resuming per brick this is 0:
    // the bricks of a window sit at whatever revision their last refill stamped,
    // and each is carried forward from its own. A gate that admits a batch only
    // when they AGREE takes it to 1 -- the whole window, every dab. A ratio of
    // counts rather than a time, so it says the same thing on any machine and
    // does not move with the iteration count; `resumed_frac` is reported beside
    // it to be read, not gated.
    const double served = static_cast<double>(rs.resumed_bricks + rs.refilled_bricks);
    state.counters["resumed_frac"] =
        served > 0 ? static_cast<double>(rs.resumed_bricks) / served : 0.0;
    state.counters["refilled_frac"] =
        served > 0 ? static_cast<double>(rs.refilled_bricks) / served : 1.0;
    clay_document_destroy(d);
}
}  // namespace

// A FIXED iteration count, because every iteration stamps another item into the
// document: left to the clock, how far the fixture grows is a property of how
// fast the timed region is. That was tolerable while a dab cost 0.17 ms and
// stopped being so when #347 took it to 0.004 -- 40x the iterations, 40x the
// growth, and minutes of wall clock per repetition. 4096 is about what the
// clock chose before, so the fixture is the one these counters were read on.
namespace {
constexpr int kRefillMovingIters = 4096;
}  // namespace

void BM_BrickRefillMoving5000(benchmark::State& state) { refill_moving(state, 5000); }
BENCHMARK(BM_BrickRefillMoving5000)->Unit(benchmark::kMillisecond)
    ->Iterations(kRefillMovingIters);

void BM_BrickRefillMoving20000(benchmark::State& state) { refill_moving(state, 20000); }
BENCHMARK(BM_BrickRefillMoving20000)->Unit(benchmark::kMillisecond)
    ->Iterations(kRefillMovingIters);

// The resumed refill at several WINDOW SIZES, which is what #348 needed
// measuring before it could pick a shape. A dab's dirty window is one brick for
// a small brush and dozens for a big one or a symmetry pass, and the resumed
// path's per-brick compile-and-evaluate used to run serially under the document
// cache mutex however many bricks the host asked for.
//
// A STILL window, primed, so every brick resumes and the timed region is the
// resumed path and nothing else: the moving pair above already covers the
// admission gate, and mixing the two would make this measure both.
namespace {
void refill_window(benchmark::State& state, int window, int history, int dabs) {
    clay_document* d = abi_sculpt(history);
    const std::size_t per = 8 * 8 * 8;
    std::vector<clay_brick_request> reqs(static_cast<std::size_t>(window));
    for (int i = 0; i < window; ++i) {
        std::memset(&reqs[i], 0, sizeof(reqs[i]));
        // A compact block starting at the equator and growing into the two
        // rows behind it, so a window of any size stays ON the sphere: a brick
        // that runs off the shape carries no accumulator and is walked in full
        // for ever, which would put the full path inside this timing. Same
        // reason the moving pair above runs along the equator.
        reqs[i].key[0] = -3 + (i % 6);
        reqs[i].key[1] = -1 + (i / 6) % 2;
        reqs[i].key[2] = -1 + (i / 12);
        for (int a = 0; a < 3; ++a)
            reqs[i].origin[a] = static_cast<float>(reqs[i].key[a]) * 8 * 0.05f;
        reqs[i].spacing = 0.05f;
        reqs[i].dims[0] = reqs[i].dims[1] = reqs[i].dims[2] = 8;
        reqs[i].band = 0.15f;
    }
    std::vector<float> out(static_cast<std::size_t>(window) * per);
    clay_brick_cache_eval_requests(d, nullptr, reqs.data(), static_cast<std::size_t>(window),
                                   out.data(), out.size(), nullptr, 0);
    const clay_resume_stats before = [&] {
        clay_resume_stats rs{};
        rs.struct_size = sizeof rs;
        clay_document_resume_stats(d, &rs);
        return rs;
    }();
    float y = 0.0f;
    for (auto _ : state) {
        state.PauseTiming();
        // `dabs` appends between refills, which is the SUFFIX LENGTH each brick
        // has to walk. One is the host that refills every dab; more is the host
        // that refills every few, and the brick a moving window left behind.
        for (int k = 0; k < dabs; ++k) {
            const float r = 0.05f;
            clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
            const float p[3] = {0.98f, y, -0.1f};
            y += 0.001f;
            clay_item_set_position(it, p);
            clay_layer_add_item(d, 1, it, nullptr);
            clay_item_destroy(it);
        }
        state.ResumeTiming();
        clay_brick_cache_eval_requests(d, nullptr, reqs.data(), static_cast<std::size_t>(window),
                                       out.data(), out.size(), nullptr, 0);
    }
    clay_resume_stats rs{};
    rs.struct_size = sizeof rs;
    clay_document_resume_stats(d, &rs);
    const double served = static_cast<double>((rs.resumed_bricks - before.resumed_bricks) +
                                              (rs.refilled_bricks - before.refilled_bricks));
    state.counters["window"] = static_cast<double>(window);
    state.counters["history"] = static_cast<double>(history);
    state.counters["dabs"] = static_cast<double>(dabs);
    // 1.0 by construction: every brick of a primed still window resumes. Read
    // as a guard on the fixture, not as the thing being measured -- a fixture
    // that quietly stopped resuming would report the full path's time here.
    state.counters["resumed_frac"] =
        served > 0 ? static_cast<double>(rs.resumed_bricks - before.resumed_bricks) / served : 0.0;
    // Its complement, and the one tools/check_bench.py GATES -- the same pair
    // and the same reason as the moving benchmarks above. A ceiling belongs on
    // the share WALKED IN FULL rather than on the share resumed, because that
    // is the direction a broken fixture moves in and a ceiling reads the same
    // way as every other entry in that table.
    state.counters["refilled_frac"] =
        served > 0 ? static_cast<double>(rs.refilled_bricks - before.refilled_bricks) / served
                   : 1.0;
    clay_document_destroy(d);
}
}  // namespace

void BM_BrickRefillWindow(benchmark::State& state) {
    refill_window(state, static_cast<int>(state.range(0)), static_cast<int>(state.range(1)),
                  static_cast<int>(state.range(2)));
}
BENCHMARK(BM_BrickRefillWindow)
    ->Args({1, 5000, 1})
    ->Args({4, 5000, 1})
    ->Args({12, 5000, 1})
    ->Args({48, 5000, 1})
    ->Args({1, 5000, 16})
    ->Args({4, 5000, 16})
    ->Args({12, 5000, 16})
    ->Args({48, 5000, 16})
    ->Args({48, 20000, 16})
    ->Unit(benchmark::kMicrosecond);

// -- dirty-prefix (frontier) tracking (#360) ---------------------------------
//
// A continuing Move drag replaces the tail grab deformer every frame, so
// nothing BEFORE the dragged node ever changes -- and before #360 every frame
// still dropped every seed the drag's bound reached and replayed the whole
// chain per dirty brick. Now a seed carries a PREFIX (the active chain folded
// through the first B roots), the drag's parameter edit marks it
// dirty_from = B instead of dropping it, and the refill folds only
// roots[B..end) onto the prefix.
//
// Three claims, three shapes:
//   BM_MoveDragRefill / BM_MoveDragRefillCold -- the per-frame win: one drag
//       frame plus one window refill, seeds kept against seeds disabled.
//   BM_TouchRegionSeedStore / BM_TouchRegionFromSeedStore (and the Small
//       pair) -- the invalidation overhead: one parameter edit over a live
//       store, the frontier MARK against the legacy DROP, at the same store
//       size and the same bound magnitude.
//   BM_MoveDragRefillHistory500/5000 and BM_TouchRegionFromSeedStore against
//       BM_TouchRegionFromDeepHistory -- the same dirty brick count at short
//       and long history: tracking cost must not scale with history length.
//
// Ratios, not absolutes, and benches measure while the TESTS hold parity
// (test_c_frontier_resume.cpp memcmps every kept seed against an oracle that
// never resumed) -- the same division of labour as the volume-move pair.
namespace {

constexpr std::size_t kFrontierPer = 8u * 8u * 8u;

// One brick, addressed the way the cache addresses it (brick width 0.4,
// band 0.15 -- the frontier tests' geometry).
clay_brick_request frontier_brick(int kx, int ky, int kz) {
    clay_brick_request q;
    std::memset(&q, 0, sizeof q);
    const int k[3] = {kx, ky, kz};
    for (int a = 0; a < 3; ++a) {
        q.key[a] = k[a];
        q.origin[a] = static_cast<float>(k[a]) * 8 * 0.05f;
        q.dims[a] = 8;
    }
    q.spacing = 0.05f;
    q.band = 0.15f;
    return q;
}

clay_resume_stats frontier_stats(const clay_document* d) {
    clay_resume_stats s{};
    s.struct_size = sizeof s;
    clay_document_resume_stats(d, &s);
    return s;
}

// One drag frame: grab whatever surface sits inside `radius` of `centre` and
// pull it +x by `displacement`. Centre and radius held fixed across a
// gesture, which is what makes moved_chain REPLACE the leading grab rather
// than stack another -- the continuing drag #360 exists for.
bool frontier_drag(clay_document* d, clay_layer_id layer, const float centre[3], float radius,
                   float displacement) {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.radius = radius;
    const float disp[3] = {displacement, 0.0f, 0.0f};
    std::size_t applied = 0;
    return clay_layer_move_surface(d, layer, centre, disp, &p, &applied) == CLAY_OK &&
           applied >= 1;
}

// abi_sculpt plus a smooth-unioned drag TARGET appended LAST (its root
// ordinal is the frontier), sticking out at x 1.35 so the brush at its +x
// pole (1.6) reaches nothing else. Quadratic blend for the reason the
// frontier tests give: the dragged suffix then folds non-idempotently, so a
// broken fixture cannot hide behind min. Layer id 1 is abi_sculpt's one
// layer, as in refill_stroke above.
clay_document* frontier_sculpt(int history) {
    clay_document* d = abi_sculpt(history);
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_SPHERE;
    desc.params[0] = 0.25f;
    desc.op = CLAY_OP_ADD;
    desc.blend = CLAY_BLEND_QUADRATIC;
    desc.blend_k = 0.05f;
    desc.position[0] = 1.35f;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    clay_add_item(d, 1, &desc, nullptr);
    return d;
}

// One drag frame plus one window refill -- what a host pays per frame of a
// continuing Move drag. The window (kx -1..3 along the equator) mixes ground
// the drag dirties (kx 1..3) with ground it leaves clean, as a real re-mesh
// region does. The cold row is byte-identical but runs with the resume budget
// at zero, so every frame is the full walk -- the gated #360 ratio.
void move_drag_refill(benchmark::State& state, int history, bool keep_seeds,
                      bool mirrored = false) {
    clay_document* d = frontier_sculpt(history);
    if (!keep_seeds) clay_internal_set_resume_budget(d, 0);
    // Under a layer mirror (#363), set BEFORE the warm refill: the mirror edit
    // is a layer-wide parameter edit and takes the legacy drop itself. The
    // drag then has to state the target's ordinal as its frontier, or the
    // guard below skips this row. On THIS fixture the base sits far enough
    // from the ball that even the mirror-expanded bound never took it, so the
    // row resumed before the brush was reflected too; what it holds is that
    // the frontier path saves the same fraction under a mirror as without one.
    // The selection defect itself is pinned by BM_MoveDragMirrored1000's
    // warped_ratio, whose fixture the expanded bound did over-select.
    if (mirrored) clay_set_layer_mirror(d, 1, 1, 0, 0, 0.05f);
    const float centre[3] = {1.6f, 0.0f, 0.0f};
    std::vector<clay_brick_request> reqs;
    for (int kx = -1; kx <= 3; ++kx) reqs.push_back(frontier_brick(kx, -1, -1));
    std::vector<float> out(reqs.size() * kFrontierPer);
    auto refill = [&] {
        clay_brick_cache_eval_requests(d, nullptr, reqs.data(), reqs.size(), out.data(),
                                       out.size(), nullptr, 0);
    };
    refill();  // warm: every brick holds a seed (or none at all, in the cold row)
    // Frame one pays the pre-drag prefix recording; after it, the fast path
    // must be REAL before it is measured -- a fixture whose bricks quietly
    // fell to the full walk would report the cold row's time as the warm one.
    if (!frontier_drag(d, 1, centre, 0.2f, 0.05f)) {
        state.SkipWithError("the drag reached nothing; the fixture is not a drag");
        clay_document_destroy(d);
        return;
    }
    {
        const clay_resume_stats b = frontier_stats(d);
        refill();
        const clay_resume_stats a = frontier_stats(d);
        if (keep_seeds && (a.refilled_bricks != b.refilled_bricks ||
                           a.resumed_bricks - b.resumed_bricks != reqs.size())) {
            state.SkipWithError("a warm drag frame did not resume every brick");
            clay_document_destroy(d);
            return;
        }
    }
    const clay_resume_stats before = frontier_stats(d);
    int f = 0;
    for (auto _ : state) {
        // The displacement grows (a drag that stands still is a no-op frame)
        // but stays under 0.09, so the target's influence never crosses into
        // another brick and the dirty set stays put for the whole run.
        frontier_drag(d, 1, centre, 0.2f, 0.05f + 0.0001f * static_cast<float>(++f));
        refill();
        benchmark::DoNotOptimize(out.data());
    }
    const clay_resume_stats after = frontier_stats(d);
    const double served = static_cast<double>((after.resumed_bricks - before.resumed_bricks) +
                                              (after.refilled_bricks - before.refilled_bricks));
    state.counters["history"] = static_cast<double>(history);
    state.counters["bricks"] = static_cast<double>(reqs.size());
    // 1.0 on the warm rows by the guard above. The cold row reads 1/window
    // (0.2), not 0.0: eviction always keeps the newest entry, so one brick of
    // the window resumes even at budget zero -- which only flatters the full
    // walk the cold row stands for. A guard on the fixture, not the
    // measurement, as in the moving pair.
    state.counters["resumed_frac"] =
        served > 0 ? static_cast<double>(after.resumed_bricks - before.resumed_bricks) / served
                   : 0.0;
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(reqs.size() * kFrontierPer));
    clay_document_destroy(d);
}

// The invalidation fixture: a store of `bricks` live seeds under a two-layer
// document whose ACTIVE layer is a base that reaches every brick (the prefix
// must hold an accumulator everywhere, or frontier_seed_for correctly refuses
// per brick and the frontier row measures the drop path by accident),
// `history - 2` dabs, and a big blended ball appended LAST -- the drag target
// and the frontier edit's node. The BELOW layer holds a twin of that ball, so
// the legacy edit has the same bound magnitude: both timed edits nudge a ball
// whose influence covers the whole store, one through the frontier keep, one
// through the legacy drop.
struct TouchStore {
    clay_document* d = nullptr;
    clay_layer_id below = 0;
    clay_layer_id active = 0;
    clay_node_id below_ball = 0;
    clay_node_id ball = 0;
    std::vector<clay_brick_request> reqs;
};

TouchStore touch_store(int bricks, int history) {
    TouchStore t;
    t.d = clay_document_create();
    clay_add_sdf_layer(t.d, "below", &t.below);
    clay_add_sdf_layer(t.d, "active", &t.active);
    auto ball = [&](clay_layer_id layer, float r, float k) {
        clay_item_desc desc;
        std::memset(&desc, 0, sizeof desc);
        desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
        desc.prim = CLAY_PRIM_SPHERE;
        desc.params[0] = r;
        desc.op = CLAY_OP_ADD;
        desc.blend = k > 0.0f ? CLAY_BLEND_QUADRATIC : CLAY_BLEND_HARD;
        desc.blend_k = k;
        desc.rotation[3] = 1.0f;
        desc.scale = 1.0f;
        clay_node_id id = 0;
        clay_add_item(t.d, layer, &desc, &id);
        return id;
    };
    t.below_ball = ball(t.below, 2.2f, 0.0f);
    ball(t.active, 2.0f, 0.0f);  // the base, and the whole of the prefix
    for (int i = 1; i < history - 1; ++i) {
        // The dab spiral abi_sculpt walks, scaled onto the base's surface.
        const double z = 1.0 - 2.0 * (i + 0.5) / (history - 1);
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double th = 2.399963 * i;
        clay_item* it = nullptr;
        const float dr = 0.05f;
        it = clay_item_create(CLAY_PRIM_SPHERE, &dr, 1);
        const float p[3] = {static_cast<float>(2.0 * r * std::cos(th)),
                            static_cast<float>(2.0 * r * std::sin(th)),
                            static_cast<float>(2.0 * z)};
        clay_item_set_position(it, p);
        clay_layer_add_item(t.d, t.active, it, nullptr);
        clay_item_destroy(it);
    }
    t.ball = ball(t.active, 2.2f, 0.05f);
    // The `bricks` bricks nearest the origin out of an 8x6x6 block, so a
    // small region is the innermost one and every brick's culled PREFIX is
    // non-empty (worst corner cull box sits ~1.4 from the origin, well inside
    // the base's 2.2 influence-plus-pad reach).
    std::vector<clay_brick_request> all;
    for (int kz = -3; kz < 3; ++kz)
        for (int ky = -3; ky < 3; ++ky)
            for (int kx = -4; kx < 4; ++kx) all.push_back(frontier_brick(kx, ky, kz));
    std::sort(all.begin(), all.end(), [](const clay_brick_request& a, const clay_brick_request& b) {
        auto d2 = [](const clay_brick_request& q) {
            double s = 0.0;
            for (int i = 0; i < 3; ++i) {
                const double c = static_cast<double>(q.key[i]) + 0.5;
                s += c * c;
            }
            return s;
        };
        return d2(a) < d2(b);
    });
    all.resize(static_cast<std::size_t>(bricks));
    t.reqs = std::move(all);
    return t;
}

// One parameter edit over a store of live seeds. The frontier row nudges the
// active layer's tail ball -- touch_region_from, which MARKS every in-bound
// entry dirty_from = its ordinal and keeps the seed. The legacy row nudges
// the below twin -- a non-active-layer edit moves the below half of every
// seed, so it takes the legacy drop, exactly as every edit did before #360.
// The refill between edits is untimed: it restores what the legacy edit
// destroyed (and clears what the frontier edit marked), so every timed edit
// works over the same full store.
void touch_seed_store(benchmark::State& state, int bricks, int history, bool frontier) {
    TouchStore t = touch_store(bricks, history);
    std::vector<float> out(t.reqs.size() * kFrontierPer);
    auto refill = [&] {
        clay_brick_cache_eval_requests(t.d, nullptr, t.reqs.data(), t.reqs.size(), out.data(),
                                       out.size(), nullptr, 0);
    };
    auto nudge = [&](clay_layer_id layer, clay_node_id node, float x) {
        const float pos[3] = {x, 0.0f, 0.0f};
        const float axis[3] = {0.0f, 0.0f, 1.0f};
        clay_layer_set_transform(t.d, layer, node, pos, axis, 0.0f, 1.0f);
    };
    refill();  // warm: every brick holds a seed
    // One drag records the prefixes (the pre-drag pass runs before its
    // applies); the brush grabs the active ball's +x pole and nothing else --
    // the base's surface sits 0.25 away, past the 0.1 radius.
    const float centre[3] = {2.25f, 0.0f, 0.0f};
    if (!frontier_drag(t.d, t.active, centre, 0.1f, 0.02f)) {
        state.SkipWithError("the drag reached nothing; no prefixes were recorded");
        clay_document_destroy(t.d);
        return;
    }
    refill();
    if (frontier) {
        // The fixture guard: one nudge must MARK the store, not drop it, and
        // the next refill must resume every brick. A store that quietly fell
        // to the drop path would report the legacy row's time here.
        nudge(t.active, t.ball, 0.004f);
        const clay_resume_stats b = frontier_stats(t.d);
        refill();
        const clay_resume_stats a = frontier_stats(t.d);
        if (a.refilled_bricks != b.refilled_bricks) {
            state.SkipWithError("a nudge dropped seeds the frontier should have kept");
            clay_document_destroy(t.d);
            return;
        }
    }
    float x = 0.0f;
    for (auto _ : state) {
        state.PauseTiming();
        refill();
        state.ResumeTiming();
        x = (x == 0.0f) ? 0.004f : 0.0f;
        if (frontier)
            nudge(t.active, t.ball, x);
        else
            nudge(t.below, t.below_ball, x);
    }
    std::uint64_t entries = 0;
    clay_internal_resume_order_size(t.d, &entries);
    state.counters["bricks"] = static_cast<double>(bricks);
    state.counters["history"] = static_cast<double>(history);
    state.counters["entries"] = static_cast<double>(entries);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * bricks);
    clay_document_destroy(t.d);
}

}  // namespace

// FIXED ITERATION COUNTS throughout, for the usual reason: the drag rows grow
// their displacement as they run and the touch rows pay an untimed refill per
// edit, so left to the clock the faster side would sweep a different workload
// (and the touch rows would run for minutes of untimed wall clock). NAMED
// variants rather than ->Arg(): check_bench.py keys its gates on the part of
// the name before "/".
void BM_MoveDragRefill(benchmark::State& state) { move_drag_refill(state, 2000, true); }
BENCHMARK(BM_MoveDragRefill)->Unit(benchmark::kMillisecond)->Iterations(300);

void BM_MoveDragRefillCold(benchmark::State& state) { move_drag_refill(state, 2000, false); }
BENCHMARK(BM_MoveDragRefillCold)->Unit(benchmark::kMillisecond)->Iterations(300);

// The warm frame under a layer mirror (#363): the same drag on the same
// document must still resume every brick, held against ITS OWN cold row.
// A mirrored document carries twice the geometry -- every item's bound and
// every brick's tape pay for the copy -- so both the warm frame and the full
// walk cost more than their unmirrored twins (measured 0.63 ms against
// 0.32 ms warm), and the claim this pair holds is that the frontier path
// saves the same fraction under a mirror as it does without one.
void BM_MoveDragRefillMirrored(benchmark::State& state) {
    move_drag_refill(state, 2000, true, /*mirrored=*/true);
}
BENCHMARK(BM_MoveDragRefillMirrored)->Unit(benchmark::kMillisecond)->Iterations(300);

void BM_MoveDragRefillMirroredCold(benchmark::State& state) {
    move_drag_refill(state, 2000, false, /*mirrored=*/true);
}
BENCHMARK(BM_MoveDragRefillMirroredCold)->Unit(benchmark::kMillisecond)->Iterations(300);

// The same warm frame at two histories: the whole point of the prefix seed is
// that a frame costs what the SUFFIX costs, so these two rows must sit
// together however deep the sculpt under the drag is.
void BM_MoveDragRefillHistory500(benchmark::State& state) { move_drag_refill(state, 500, true); }
BENCHMARK(BM_MoveDragRefillHistory500)->Unit(benchmark::kMillisecond)->Iterations(300);

void BM_MoveDragRefillHistory5000(benchmark::State& state) { move_drag_refill(state, 5000, true); }
BENCHMARK(BM_MoveDragRefillHistory5000)->Unit(benchmark::kMillisecond)->Iterations(300);

// The <=5% overhead claim (#360 spec): marking a live store must cost no more
// than dropping it did. Both rows time ONE edit whose bound covers the whole
// store; the pair differs only in which path the edit takes.
void BM_TouchRegionSeedStore(benchmark::State& state) { touch_seed_store(state, 288, 2, false); }
BENCHMARK(BM_TouchRegionSeedStore)->Unit(benchmark::kMicrosecond)->Iterations(512);

void BM_TouchRegionFromSeedStore(benchmark::State& state) {
    touch_seed_store(state, 288, 2, true);
}
BENCHMARK(BM_TouchRegionFromSeedStore)->Unit(benchmark::kMicrosecond)->Iterations(512);

void BM_TouchRegionSeedStoreSmall(benchmark::State& state) {
    touch_seed_store(state, 12, 2, false);
}
BENCHMARK(BM_TouchRegionSeedStoreSmall)->Unit(benchmark::kMicrosecond)->Iterations(2048);

void BM_TouchRegionFromSeedStoreSmall(benchmark::State& state) {
    touch_seed_store(state, 12, 2, true);
}
BENCHMARK(BM_TouchRegionFromSeedStoreSmall)->Unit(benchmark::kMicrosecond)->Iterations(2048);

// The history-scaling check: the same 288-entry store, the same edit, under a
// 5000-root sculpt. Per-brick frontier metadata is three numbers whatever the
// history, and the edit's ordinal lookup is one walk of the root list -- so
// this row must sit beside BM_TouchRegionFromSeedStore, not above it.
void BM_TouchRegionFromDeepHistory(benchmark::State& state) {
    touch_seed_store(state, 288, 5000, true);
}
BENCHMARK(BM_TouchRegionFromDeepHistory)->Unit(benchmark::kMicrosecond)->Iterations(512);

void BM_CullIndexRebuild(benchmark::State& state) {
    const scene::Document doc = spread_sculpt(kIndexAppendNodes);
    for (auto _ : state) {
        const scene::CullIndex index(doc);
        benchmark::DoNotOptimize(index.cull_pad());
    }
    state.counters["nodes"] = static_cast<double>(kIndexAppendNodes);
}
BENCHMARK(BM_CullIndexRebuild)->Unit(benchmark::kMillisecond);

namespace {
// One dab per iteration, each extending the index the last one produced -- a
// stroke, which is the case this exists for. The insert is outside the timed
// region the same way the rebuild's document build is, and so is the periodic
// rebuild of the fixture.
//
// Through scene::append_cached, which is the decision the C ABI makes
// (clay_document::cull_index_locked) rather than a copy of it. `shared` holds a
// second handle across the timed call -- a reader with a plan it already made
// -- which is what makes append_cached copy; with nobody looking it extends the
// cached index in place instead, which is what the other two measure (#347).
void index_append(benchmark::State& state, int nodes, bool shared) {
    scene::Document doc;
    std::shared_ptr<scene::CullIndex> index;
    int since_reset = kIndexAppendReset;
    for (auto _ : state) {
        state.PauseTiming();
        if (since_reset == kIndexAppendReset) {
            index.reset();  // it borrows the document about to be replaced
            doc = spread_sculpt(nodes);
            index = std::make_shared<scene::CullIndex>(doc);
            since_reset = 0;
            // WARM THE FIXTURE, untimed. A fresh index reserves each chain's
            // entries exactly, so the FIRST append doubles a 20,000-entry
            // vector -- a megabyte of copy that a stroke pays once and that
            // this loop would otherwise pay every reset, amortised over the
            // 256 appends between them. That is a cost proportional to the
            // DOCUMENT masquerading as the cost of an append, which is the one
            // thing the pair of sizes below must not measure.
            scene::Node warm;
            warm.prim = scene::Prim::sphere(0.04f);
            warm.xform.position = cf3(1.0f, -0.001f, 0.0f);
            const scene::NodeId warm_id = doc.layers[0].sdf->insert(std::move(warm));
            if (!index->append({warm_id})) {
                state.SkipWithError("the append was refused");
                return;
            }
        }
        scene::Node n;
        n.prim = scene::Prim::sphere(0.04f);
        n.xform.position = cf3(1.0f, 0.001f * static_cast<float>(since_reset++), 0.0f);
        const scene::NodeId id = doc.layers[0].sdf->insert(std::move(n));
        // Taken untimed: the handle is what the reader would already be
        // holding, not work the append does.
        std::shared_ptr<const scene::CullIndex> reader;
        if (shared) reader = index;
        state.ResumeTiming();
        if (!scene::append_cached(index, {id})) {
            state.SkipWithError("the append was refused");
            return;
        }
    }
    state.counters["nodes"] = static_cast<double>(nodes);
}
}  // namespace

void BM_CullIndexAppend(benchmark::State& state) {
    index_append(state, kIndexAppendNodes, false);
}
BENCHMARK(BM_CullIndexAppend)->Unit(benchmark::kMillisecond)->Iterations(kIndexAppendIters);

void BM_CullIndexAppendSmall(benchmark::State& state) {
    index_append(state, kIndexAppendSmallNodes, false);
}
BENCHMARK(BM_CullIndexAppendSmall)->Unit(benchmark::kMillisecond)->Iterations(kIndexAppendIters);

void BM_CullIndexAppendShared(benchmark::State& state) {
    index_append(state, kIndexAppendNodes, true);
}
BENCHMARK(BM_CullIndexAppendShared)->Unit(benchmark::kMillisecond)->Iterations(kIndexAppendIters);

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

// The two meshers on ONE precomputed lattice, which is what the meshing spec's
// "cheaper preview" claim is actually about. The pair above cannot say it: both
// its sides spend about half their time in `eval_tape_grid`, evaluating the
// same field, and that shared half compresses whatever the meshers differ by.
//
// It also used to say the opposite of the truth. Until #302 both benchmarks
// meshed with the default attributes, and the attribute pass -- one tape walk
// per vertex for the colour and four for the gradient, on one thread -- was
// 80-96% of each. Surface nets emits 3.2x fewer vertices, so it paid 3.2x less
// of that and the pair passed on VERTEX COUNT while the geometry step went
// unmeasured. With the attributes batched, nets was measured 1.68x SLOWER to
// build (#304), and the fix was the mesher rather than the spec.
namespace {
struct LatticeFixture {
    std::vector<float> values;
    int nx = 0, ny = 0, nz = 0;
    float voxel = 0.02f;
    kernel::cfloat3 origin;
};

const LatticeFixture& bench_lattice() {
    static const LatticeFixture fixture = [] {
        LatticeFixture f;
        const scene::Document doc = bench_document();
        const scene::Tape tape = scene::compile_document(doc);
        const math::Aabb r = tape.bounds;
        f.origin = r.min;
        f.nx = static_cast<int>(kernel::cround((r.max.x - r.min.x) / f.voxel)) + 1;
        f.ny = static_cast<int>(kernel::cround((r.max.y - r.min.y) / f.voxel)) + 1;
        f.nz = static_cast<int>(kernel::cround((r.max.z - r.min.z) / f.voxel)) + 1;
        f.values.resize(static_cast<std::size_t>(f.nx) * f.ny * f.nz);
        eval::GridQuery q;
        q.origin = r.min;
        q.spacing = f.voxel;
        q.nx = f.nx;
        q.ny = f.ny;
        q.nz = f.nz;
        eval::Registry::instance().find("cpu")->eval_grid(tape, q, f.values.data());
        return f;
    }();
    return fixture;
}
}  // namespace

void BM_MeshLatticeMarch(benchmark::State& state) {
    const LatticeFixture& f = bench_lattice();
    auto sample = [&f](int i, int j, int k) -> float {
        if (i < 0 || j < 0 || k < 0 || i >= f.nx || j >= f.ny || k >= f.nz) return f.voxel;
        return f.values[(static_cast<std::size_t>(k) * f.ny + j) * f.nx + i];
    };
    int cmin[3] = {-1, -1, -1};
    int cmax[3] = {f.nx, f.ny, f.nz};
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_lattice(sample, cmin, cmax, f.origin, f.voxel);
        benchmark::DoNotOptimize(m.triangle_count());
    }
}
BENCHMARK(BM_MeshLatticeMarch)->Unit(benchmark::kMillisecond);

void BM_MeshLatticeNets(benchmark::State& state) {
    const LatticeFixture& f = bench_lattice();
    auto sample = [&f](int i, int j, int k) -> float {
        if (i < 0 || j < 0 || k < 0 || i >= f.nx || j >= f.ny || k >= f.nz) return f.voxel;
        return f.values[(static_cast<std::size_t>(k) * f.ny + j) * f.nx + i];
    };
    const int cmin[3] = {-1, -1, -1};
    const int cmax[3] = {f.nx, f.ny, f.nz};
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_lattice_nets(sample, cmin, cmax, f.origin, f.voxel);
        benchmark::DoNotOptimize(m.triangle_count());
    }
}
BENCHMARK(BM_MeshLatticeNets)->Unit(benchmark::kMillisecond);

// The attribute pass the non-brick meshers reach — `mesh_tape`, `mesh_tape_dc`
// and the dual-grid path all land in `apply_tape_attributes`. It walked the
// tape once per vertex for the colour and four more for the gradient, on ONE
// thread, which was 96% of a coloured mesh; it now hands the whole vertex set
// to the CPU backend's blocked, pooled evaluator (#302).
//
// The pair is what stops that walk coming back. Both sides mesh the identical
// geometry and evaluate the identical taps against the identical tape — they
// differ only in whether the evaluation goes through the backend — and the
// attributes are byte-identical by contract, held by `test_points_batch.cpp`.
// It holds on any machine with more than one core, and the margin is wide
// because neither side pays a floor the other does not: 53x on a twelve-core
// machine at this fixture's size.
void BM_MeshTapeAttributes(benchmark::State& state) {
    scene::Document doc = bench_document();
    scene::Tape tape = scene::compile_document(doc);
    mesh::MeshingOptions bare;
    bare.normals = mesh::NormalMode::None;
    bare.colors = false;
    const mesh::Mesh base = mesh::mesh_tape(tape, tape.bounds, 0.02f, bare);
    mesh::MeshingOptions full;
    full.normals = mesh::NormalMode::Gradient;
    full.colors = true;
    for (auto _ : state) {
        mesh::Mesh m = base;
        mesh::apply_tape_attributes(m, tape, full);
        benchmark::DoNotOptimize(m.normals.size());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(base.positions.size()));
}
BENCHMARK(BM_MeshTapeAttributes)->Unit(benchmark::kMillisecond);

// The serial reference the pass above replaced, kept as the thing it must beat.
void BM_MeshTapeAttributesSerial(benchmark::State& state) {
    scene::Document doc = bench_document();
    scene::Tape tape = scene::compile_document(doc);
    mesh::MeshingOptions bare;
    bare.normals = mesh::NormalMode::None;
    bare.colors = false;
    const mesh::Mesh base = mesh::mesh_tape(tape, tape.bounds, 0.02f, bare);
    auto field = [&tape](kernel::cfloat3 p) { return tape.eval(p).d; };
    for (auto _ : state) {
        mesh::Mesh m = base;
        m.colors.resize(m.positions.size());
        m.normals.resize(m.positions.size());
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            m.colors[i] = tape.eval(m.positions[i]).color;
            m.normals[i] = kernel::cnormal(field, m.positions[i], 1e-4f);
        }
        benchmark::DoNotOptimize(m.normals.size());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(base.positions.size()));
}
BENCHMARK(BM_MeshTapeAttributesSerial)->Unit(benchmark::kMillisecond);

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

// The document bake with its tape culled per brick, against the whole-tape
// bake it is measured to equal byte for byte. check_bench.py requires the
// culled form to be FASTER — and the pair is the only thing that would catch
// the adaptive guard silently deciding never to cull.
void BM_VolumeBakeCulledDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(600);
    const scene::Tape tape = scene::compile_document(doc);
    const float cell = 0.04f, band = cell * 3.0f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    for (auto _ : state) {
        field::FieldVolume v = field::FieldVolume::sample_blocks(
            eval::document_block_fill(doc, tape), region, cell, band);
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_VolumeBakeCulledDoc)->Unit(benchmark::kMillisecond);

void BM_VolumeBakeWholeTapeDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(600);
    const scene::Tape tape = scene::compile_document(doc);
    const float cell = 0.04f, band = cell * 3.0f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    for (auto _ : state) {
        field::FieldVolume v = field::FieldVolume::sample_blocks(
            eval::tape_block_fill(tape), region, cell, band);
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_VolumeBakeWholeTapeDoc)->Unit(benchmark::kMillisecond);

// Move Topological from a document, batched against the per-point walk it
// replaced. The third of the document-sourced verbs to get a pooled evaluator
// and the one that needed a different kind: its query positions are the
// PULLED-BACK points rather than the sample lattice, so it takes a batch of
// arbitrary points. check_bench.py requires the batched form to be FASTER.
//
// Byte-identity is held by "the pooled point batch moves the volume the
// per-point source does" in test_consolidate.cpp, not by this pair.
void BM_VolumeMoveDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const float cell = 0.05f, band = cell * 3.0f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    field::TopologicalMoveSettings s;
    s.anchor = cf3(0, 0, 1.0f);
    s.radius = 0.3f;
    s.displacement = cf3(0.0f, 0.06f, 0.0f);
    for (auto _ : state) {
        field::FieldVolume v =
            field::move_topological(eval::tape_point_batch(tape), region, cell, band, s);
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_VolumeMoveDoc)->Unit(benchmark::kMillisecond);

void BM_VolumeMoveSerialDoc(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const scene::Tape tape = scene::compile_layer(doc.layers.front());
    const float cell = 0.05f, band = cell * 3.0f;
    const kernel::cfloat3 pad = cf3(band, band, band);
    const math::Aabb region{tape.bounds.min - pad, tape.bounds.max + pad};
    field::TopologicalMoveSettings s;
    s.anchor = cf3(0, 0, 1.0f);
    s.radius = 0.3f;
    s.displacement = cf3(0.0f, 0.06f, 0.0f);
    for (auto _ : state) {
        field::FieldVolume v = field::move_topological(
            [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, region, cell, band, s);
        benchmark::DoNotOptimize(v.sample_count());
    }
}
BENCHMARK(BM_VolumeMoveSerialDoc)->Unit(benchmark::kMillisecond);

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

// -- live SDF sculpt transactions (sdf-sculpt-transaction) -------------------
//
// The two verbs an edit-list brush cannot spell. Both used to cost the MODEL
// per pointer event and both now cost the GESTURE once plus the dab, and this
// group is where that claim is measured rather than asserted.
//
// SMOOTH bakes, because averaging a field has no node that means "the average
// of what was here" (field/relax.h says so first). A host with nowhere to keep
// the baked volume between pointer events has exactly one implementation: bake
// the layer, relax, throw the volume away, per dab. That is
// BM_SdfSmoothStandalone, and it is the reason Smooth shipped without a live
// preview at all.
//
// THESE ROWS HAVE SHIFTED MEANING ONCE ALREADY, and the note says so rather
// than leaving the old reading in place. `SdfSmoothTransaction` first MOVED the
// bake to pointer-down, and BM_SdfSmoothTransactionBegin was that bake — 29.6
// ms of whole-layer sampling before an artist saw anything. It is now LAZY: it
// compiles, indexes a lattice and takes a digest, and evaluates nothing at all,
// which the `samples` counter reads as 0. The bake did not move again, it was
// broken up — a dab materializes the bricks its own relax will read and then
// relaxes them in place, and that is BM_SdfSmoothTransactionUpdate.
//
// The gesture rows exist because the pair above does not settle the trade on
// its own: a transaction that made every dab free and cost a second at
// pointer-down would be worse to sculpt with. BM_SdfSmoothTransaction100 and
// BM_SdfSmoothTransaction1000 time a WHOLE gesture — one begin and N updates —
// so whatever is paid once is amortised where an artist actually pays it, and
// the break-even against N standalone dabs can be read straight off the table.
//
// The dab is region-limited, at the pole `sculpted_sphere` piles its 192 extra
// spheres on. A relax with no region is a filter over the whole volume rather
// than a brush, and it would hide the property the update row exists to show:
// that a dab costs the ball it moves and not the model it sits on.
namespace {

constexpr float kSculptCell = 0.05f;

session::SdfSculptPolicy smooth_policy() {
    session::SdfSculptPolicy policy;
    policy.cell_size = kSculptCell;  // band and padding take their defaults
    return policy;
}

// One dab, at the -x pole where `sculpted_sphere` puts every dab it adds.
field::RelaxSettings smooth_dab() {
    field::RelaxSettings settings;
    settings.strength = 0.5f;
    settings.radius_cells = 1;
    settings.iterations = 1;
    settings.centre = cf3(-1.0f, 0.0f, 0.0f);
    settings.region_radius = 0.25f;
    return settings;
}

}  // namespace

// THE "BEFORE". One dab of Smooth with nowhere to keep a volume: sample the
// whole layer, relax the result, discard it. `field::relax` rather than
// `relax_in_place` on purpose — a caller with no working volume of its own has
// nothing to relax in place, so the copy is part of what the old path costs.
void BM_SdfSmoothStandalone(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    scene::ConsolidationParams params;
    params.cell_size = kSculptCell;
    const field::RelaxSettings dab = smooth_dab();
    std::size_t samples = 0;
    for (auto _ : state) {
        std::optional<field::FieldVolume> v =
            scene::bake_layer(doc.layers.front(), params, nullptr, eval::pooled_bake_eval());
        if (!v) {
            state.SkipWithError("the layer did not bake; nothing is being measured");
            return;
        }
        const field::FieldVolume relaxed = field::relax(*v, dab);
        samples = relaxed.sample_count();
        // A SEPARATE sink, never the variable a counter is read from after the
        // loop. DoNotOptimize takes its argument as a read-write operand, so
        // the compiler must assume the asm changed it -- reading it afterwards
        // is reading whatever the register happened to hold, and it prints as a
        // fourteen-digit number that looks like a measurement.
        std::size_t sink = samples;
        benchmark::DoNotOptimize(sink);
    }
    state.counters["nodes"] = 193;
    state.counters["samples"] = static_cast<double>(samples);
}
BENCHMARK(BM_SdfSmoothStandalone)->Unit(benchmark::kMillisecond);

// Pointer-down, alone. This row USED TO BE A WHOLE-LAYER BAKE — the cost the
// transaction moved rather than removed, benchmarked separately so that a lazy
// local checkpoint would have something to beat. The checkpoint landed and this
// is now the lazy begin: a compile, an index for the working lattice and a
// digest, and no evaluation whatsoever.
//
// `samples` IS THE MEASUREMENT, not decoration. It reads the working volume's
// stored sample count immediately after `begin`, and a lazy begin materializes
// no brick, so a correct row reports 0 where the old one reported 123,930. A
// begin that started sampling again would report the sample count of the model
// and no wall clock on a shared runner could tell that from a slow machine.
// BM_SdfSmoothTransactionBegin5000 and BM_SdfSmoothTransactionBegin20000 below
// carry the same row over two larger models.
void BM_SdfSmoothTransactionBegin(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const scene::LayerId layer = doc.layers.front().id;
    const session::SdfSculptPolicy policy = smooth_policy();
    for (auto _ : state) {
        std::optional<session::SdfSmoothTransaction> tx =
            session::SdfSmoothTransaction::begin(doc, layer, policy, eval::pooled_bake_eval());
        if (!tx) {
            state.SkipWithError("the transaction did not begin; nothing is being measured");
            return;
        }
        state.counters["samples"] = static_cast<double>(tx->preview_volume().sample_count());
        std::size_t sink = tx->preview_volume().sample_count();
        benchmark::DoNotOptimize(sink);
        tx->cancel();
    }
    state.counters["nodes"] = 193;
}
BENCHMARK(BM_SdfSmoothTransactionBegin)->Unit(benchmark::kMillisecond);

// THE "AFTER". `begin` runs ONCE, outside the timed loop, and the loop is one
// live dab: `relax_in_place` over the bricks the dab's ball reaches, and no
// bake, no compile, no command and no undo entry.
//
// FIXED ITERATION COUNT. It used to be fixed because the setup was a whole
// bake; the setup is now a lazy begin, and the reason survives in a sharper
// form: the FIRST update materializes the bricks the dab reads and every one
// after it reuses them, so THE ITERATION COUNT DECIDES how much of that
// one-time cost the average carries. Left to fill a time budget it would be a
// property of the machine.
//
// The row therefore MOVED, 0.152 -> 0.31 ms, without anything getting slower,
// and the shape is worth recording because it is the honest reading of the
// lazy path on a small model. Measured on this fixture at three counts — 20,
// 200 and 2000 iterations — the row is a fixed ~27-30 ms plus ~0.15 ms per dab:
// 1.49, 0.31 and 0.162 ms. The 0.15 is the steady-state dab, unchanged and
// identical to what this row read when `begin` baked; the ~28 ms is that bake,
// which at 193 nodes did not go away but arrived at the first dab instead,
// because one 0.25-radius dab's dependency region covers most of a unit
// sphere's band. On a model the dab does NOT cover, it is a fraction of the
// layer — which is the whole point, and which is what
// BM_SdfSmoothLazyFirstUpdateNoPrefix below measures at 5,000 roots.
//
// The volume is relaxed repeatedly and so converges towards its own average.
// That does not change what is being timed: `rewrite_region` visits the same
// bricks and blends the same stencil whatever the samples hold, and the
// `bricks` counter says so.
void BM_SdfSmoothTransactionUpdate(benchmark::State& state) {
    scene::Document doc = sculpted_sphere(193);
    const session::SdfSculptPolicy policy = smooth_policy();
    std::optional<session::SdfSmoothTransaction> tx = session::SdfSmoothTransaction::begin(
        doc, doc.layers.front().id, policy, eval::pooled_bake_eval());
    if (!tx) {
        state.SkipWithError("the transaction did not begin; nothing is being measured");
        return;
    }
    const field::RelaxSettings dab = smooth_dab();
    std::size_t bricks = 0;
    for (auto _ : state) {
        const session::SdfSculptDirty dirty = tx->update(dab);
        bricks = dirty.touched_bricks;
        std::size_t sink = bricks;  // see the note in BM_SdfSmoothStandalone
        benchmark::DoNotOptimize(sink);
    }
    // A dab that touched nothing would be an empty loop reporting a fast row.
    if (bricks == 0) state.SkipWithError("the dab touched no brick; nothing is being measured");
    state.counters["bricks"] = static_cast<double>(bricks);
    state.counters["nodes"] = 193;
}
BENCHMARK(BM_SdfSmoothTransactionUpdate)->Unit(benchmark::kMillisecond)->Iterations(200);

namespace {

// A WHOLE gesture: pointer-down, N dabs, pointer-up. Timed together, including
// the begin, because that is the trade a host is actually making — the
// one-time bake is only affordable if a stroke has enough dabs to amortise it,
// and the number of dabs at which it does is what these rows report.
void smooth_gesture(benchmark::State& state, int dabs) {
    scene::Document doc = sculpted_sphere(193);
    const scene::LayerId layer = doc.layers.front().id;
    const session::SdfSculptPolicy policy = smooth_policy();
    const field::RelaxSettings dab = smooth_dab();
    std::size_t bricks = 0;
    for (auto _ : state) {
        std::optional<session::SdfSmoothTransaction> tx =
            session::SdfSmoothTransaction::begin(doc, layer, policy, eval::pooled_bake_eval());
        if (!tx) {
            state.SkipWithError("the transaction did not begin; nothing is being measured");
            return;
        }
        for (int i = 0; i < dabs; ++i) bricks = tx->update(dab).touched_bricks;
        std::size_t sink = tx->preview_volume().sample_count();
        benchmark::DoNotOptimize(sink);
        // Cancelled, not committed: the commit is one command group and a
        // policy pass, and neither is part of the per-frame claim. The document
        // is untouched either way, which is what lets one fixture serve every
        // iteration.
        tx->cancel();
    }
    if (bricks == 0) state.SkipWithError("the dabs touched no brick; nothing is being measured");
    state.counters["dabs"] = static_cast<double>(dabs);
    state.counters["bricks"] = static_cast<double>(bricks);
    state.counters["nodes"] = 193;
}

}  // namespace

// NAMED rather than Arg()-parameterised, for the reason the deep-document rows
// give: check_bench.py keys a gate on the part of a name before "/".
void BM_SdfSmoothTransaction100(benchmark::State& state) { smooth_gesture(state, 100); }
BENCHMARK(BM_SdfSmoothTransaction100)->Unit(benchmark::kMillisecond)->Iterations(3);

void BM_SdfSmoothTransaction1000(benchmark::State& state) { smooth_gesture(state, 1000); }
BENCHMARK(BM_SdfSmoothTransaction1000)->Unit(benchmark::kMillisecond)->Iterations(1);

// -- Move: a drag that costs what it drags -----------------------------------
//
// `move_brush` prepares and resolves in one call, so a host driving it per
// pointer event re-walks the whole edit list sixty times a second to
// rediscover which items a fixed anchor and a fixed radius reach — an answer
// that cannot have changed. BM_SdfMoveResolve is that per-frame cost;
// BM_SdfMoveTransactionBegin is the same traversal paid once at pointer-down;
// BM_SdfMoveTransactionUpdate is what a frame costs afterwards, which is
// `resolve_prepared_move` and a chain rebuild per AFFECTED item and no scene
// access at all.
//
// The fixture holds the AFFECTED SET FIXED and grows the rest of the layer,
// which is the only shape in which the claim can be seen. A drag over a scene
// twice the size must cost the same per frame, so growing the scene while
// holding the drag is the experiment; growing both would measure nothing.
// `sculpted_sphere` cannot serve — its dabs all land on one pole, so a bigger
// document is also a bigger drag — hence a fixture of its own.
namespace {

// The items the drag reaches. Small and constant: 32 is enough that the
// per-item loop is real work and few enough that the row is dominated by it
// rather than by allocation.
constexpr int kMoveCluster = 32;
constexpr float kMoveRadius = 0.3f;

kernel::cfloat3 move_anchor() { return cf3(1.0f, 0.0f, 0.0f); }

// A cluster the drag reaches, plus `unrelated` items five world units clear of
// it. The far items are on a Fibonacci shell of radius 6, so they are spread
// rather than coincident — a pile of items at one point would let a bounds
// test cull them as a block and understate what a traversal costs.
scene::Document move_scene(int unrelated) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("move");
    for (int i = 0; i < kMoveCluster; ++i) {
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.05f);
        dab.xform.position = cf3(1.0f + 0.08f * std::sin(static_cast<float>(i) * 1.7f),
                                 0.08f * std::cos(static_cast<float>(i) * 2.3f),
                                 0.08f * std::sin(static_cast<float>(i) * 0.9f));
        l.sdf->insert(dab);
    }
    const double golden = 0.6180339887;
    for (int i = 0; i < unrelated; ++i) {
        // `elsewhere` rather than `far`, which older MSVC headers still define
        // as a macro.
        scene::Node elsewhere;
        elsewhere.prim = scene::Prim::sphere(0.05f);
        const double u = std::fmod(static_cast<double>(i) * golden, 1.0);
        const double v = (static_cast<double>(i) + 0.5) / static_cast<double>(unrelated);
        const double phi = std::acos(1.0 - 2.0 * v);
        const double th = 6.283185307 * u;
        elsewhere.xform.position = cf3(static_cast<float>(6.0 * std::sin(phi) * std::cos(th)),
                                       static_cast<float>(6.0 * std::cos(phi)),
                                       static_cast<float>(6.0 * std::sin(phi) * std::sin(th)));
        l.sdf->insert(elsewhere);
    }
    return doc;
}

brush::MoveSettings move_settings() {
    brush::MoveSettings settings;
    settings.radius = kMoveRadius;
    return settings;
}

// THE "BEFORE": one frame of a drag driven through `move_brush`, which
// prepares from scratch every call. Pure — the layer is read and never written
// — so the fixture is built once and no per-iteration rebuild is needed, unlike
// the ABI drag above which commits its warps.
void move_resolve(benchmark::State& state, int unrelated) {
    scene::Document doc = move_scene(unrelated);
    const brush::MoveSettings settings = move_settings();
    std::size_t warped = 0;
    for (auto _ : state) {
        const std::vector<brush::MoveWarp> warps =
            brush::move_brush(doc.layers.front(), move_anchor(), cf3(0.05f, 0.0f, 0.0f), settings);
        warped = warps.size();
        const brush::MoveWarp* sink = warps.data();
        benchmark::DoNotOptimize(sink);
    }
    if (warped == 0) state.SkipWithError("the drag warped no item; nothing is being measured");
    state.counters["warped"] = static_cast<double>(warped);
    state.counters["items"] = static_cast<double>(unrelated + kMoveCluster);
}

// THE "AFTER": `begin` once, outside the loop, then one frame per iteration.
// The `visited` counter is the whole gate — it is what must stay at the
// affected count whatever the rest of the layer holds, and unlike a
// millisecond it means the same thing on every machine.
void move_transaction_update(benchmark::State& state, int unrelated) {
    scene::Document doc = move_scene(unrelated);
    std::optional<session::SdfMoveTransaction> tx = session::SdfMoveTransaction::begin(
        doc, doc.layers.front().id, move_anchor(), move_settings());
    if (!tx || tx->affected_count() == 0) {
        state.SkipWithError("the drag reached no item; nothing is being measured");
        return;
    }
    // A GROWING TOTAL, never an increment: a live drag reports how far it has
    // come from the anchor, and feeding the same displacement every frame would
    // let a future implementation notice nothing had changed.
    float travelled = 0.0f;
    for (auto _ : state) {
        travelled += 0.0001f;
        std::size_t touched = tx->update(cf3(travelled, 0.0f, 0.0f)).touched_bricks;
        benchmark::DoNotOptimize(touched);
    }
    state.counters["visited"] = static_cast<double>(tx->last_update_visited());
    state.counters["affected"] = static_cast<double>(tx->affected_count());
    // What `begin` walked, for contrast: this one DOES grow with the layer, and
    // it is paid once a gesture rather than once a frame.
    state.counters["prepared"] = static_cast<double>(tx->prepare_stats().visited);
    state.counters["items"] = static_cast<double>(unrelated + kMoveCluster);
}

}  // namespace

void BM_SdfMoveResolve(benchmark::State& state) { move_resolve(state, 1000); }
BENCHMARK(BM_SdfMoveResolve)->Unit(benchmark::kMillisecond);

// The traversal, paid once. Reports both halves of MovePrepareStats: `visited`
// is what grows with the layer and `reached` is what the frames afterwards
// cost.
void BM_SdfMoveTransactionBegin(benchmark::State& state) {
    scene::Document doc = move_scene(1000);
    const scene::LayerId layer = doc.layers.front().id;
    const brush::MoveSettings settings = move_settings();
    std::size_t reached = 0;
    for (auto _ : state) {
        std::optional<session::SdfMoveTransaction> tx =
            session::SdfMoveTransaction::begin(doc, layer, move_anchor(), settings);
        if (!tx) {
            state.SkipWithError("the transaction did not begin; nothing is being measured");
            return;
        }
        reached = tx->affected_count();
        state.counters["visited"] = static_cast<double>(tx->prepare_stats().visited);
        std::size_t sink = reached;  // see the note in BM_SdfSmoothStandalone
        benchmark::DoNotOptimize(sink);
        tx->cancel();
    }
    if (reached == 0) state.SkipWithError("the drag reached no item; nothing is being measured");
    state.counters["reached"] = static_cast<double>(reached);
    state.counters["items"] = static_cast<double>(1000 + kMoveCluster);
}
BENCHMARK(BM_SdfMoveTransactionBegin)->Unit(benchmark::kMillisecond);

void BM_SdfMoveTransactionUpdate(benchmark::State& state) { move_transaction_update(state, 1000); }
BENCHMARK(BM_SdfMoveTransactionUpdate)->Unit(benchmark::kMillisecond)->Iterations(20000);

// A whole 1000-frame drag: one begin and a thousand updates, which is about
// sixteen seconds of dragging at 60 Hz. Timed together for the same reason the
// Smooth gesture rows are — the one-time traversal is only affordable if a
// gesture amortises it, and this row is where that is read.
void BM_SdfMoveTransaction1000(benchmark::State& state) {
    scene::Document doc = move_scene(1000);
    const scene::LayerId layer = doc.layers.front().id;
    const brush::MoveSettings settings = move_settings();
    std::size_t affected = 0;
    for (auto _ : state) {
        std::optional<session::SdfMoveTransaction> tx =
            session::SdfMoveTransaction::begin(doc, layer, move_anchor(), settings);
        if (!tx) {
            state.SkipWithError("the transaction did not begin; nothing is being measured");
            return;
        }
        for (int i = 1; i <= 1000; ++i)
            tx->update(cf3(0.0001f * static_cast<float>(i), 0.0f, 0.0f));
        affected = tx->affected_count();
        std::size_t sink = affected;  // see the note in BM_SdfSmoothStandalone
        benchmark::DoNotOptimize(sink);
        tx->cancel();
    }
    if (affected == 0) state.SkipWithError("the drag reached no item; nothing is being measured");
    state.counters["frames"] = 1000;
    state.counters["affected"] = static_cast<double>(affected);
    state.counters["items"] = static_cast<double>(1000 + kMoveCluster);
}
BENCHMARK(BM_SdfMoveTransaction1000)->Unit(benchmark::kMillisecond)->Iterations(5);

// THE SCALING PAIR, and the reason the fixture was written the way it was. The
// drag is identical in every row — same anchor, same radius, same 32 items —
// and only the UNRELATED bulk of the layer grows. `move_brush` re-walks that
// bulk per frame and its row climbs with it; a prepared update never touches
// the scene again and its row must not move at all.
//
// Both rows are in MILLISECONDS even though the update row is microseconds
// wide, because check_bench.py's ratio gates divide raw `real_time` values and
// two rows in different units would divide numbers in different units.
//
// PARAMETERISED here rather than named, unlike everywhere else in this file,
// because the gate that matters is a COUNTER and not a time: check_bench.py
// keys on the name before "/", so the row it lands on is the last registered —
// 50 000 unrelated items, the largest — which is exactly the row where
// `visited` staying at 32 is worth asserting.
void BM_SdfMoveResolveScaling(benchmark::State& state) {
    move_resolve(state, static_cast<int>(state.range(0)));
}
BENCHMARK(BM_SdfMoveResolveScaling)
    ->Unit(benchmark::kMillisecond)
    ->Args({100})
    ->Args({1000})
    ->Args({10000})
    ->Args({50000})
    ->Iterations(100);

void BM_SdfMoveTransactionUpdateScaling(benchmark::State& state) {
    move_transaction_update(state, static_cast<int>(state.range(0)));
}
BENCHMARK(BM_SdfMoveTransactionUpdateScaling)
    ->Unit(benchmark::kMillisecond)
    ->Args({100})
    ->Args({1000})
    ->Args({10000})
    ->Args({50000})
    ->Iterations(20000);

// -- what repeated drags do to a layer, and what the policy does about it ----
//
// A grab does not replace the one before it unless it is the SAME drag still
// in progress — `moved_chain` identifies that by the centre and the radius —
// so N separate drags leave N deformers on every item they all reached. Each
// one raises the layer's declared Lipschitz, and `safe_step_scale` is 1 over
// that: the marcher's step shrinks with the artist's history whether or not
// the shape changed much.
//
// These rows report the two numbers `SdfSculptComplexityPolicy` is written
// against, measured after N complete transactions — begin, update, commit —
// with the policy OFF, and then the same run with it ON and consolidation
// authorised. The `consolidations` counter says how many times it actually
// fired, which is the honest reading: `settle_budget` declines to collapse a
// layer that is already a single volume item, so a run that has consolidated
// once reports over budget afterwards and acts no further.
namespace {

void move_repeated(benchmark::State& state, int drags, bool policy_on) {
    session::SdfSculptPolicy policy;
    policy.cell_size = 0.02f;
    if (policy_on) {
        policy.complexity.min_safe_step_scale = 0.5f;
        policy.complexity.allow_consolidation = true;
        policy.complexity.consolidation.cell_size = 0.02f;
    }
    const brush::MoveSettings settings = move_settings();
    double chain = 0, step = 0, consolidations = 0;
    for (auto _ : state) {
        // Rebuilt per iteration, untimed, because every drag PREPENDS a warp:
        // measured without the rebuild the chains grow as the benchmark runs
        // and the row reports an average over depths rather than the depth in
        // its name. Same reason as the ABI drag rows above.
        state.PauseTiming();
        scene::Document doc = move_scene(0);
        const scene::LayerId layer = doc.layers.front().id;
        consolidations = 0;
        state.ResumeTiming();
        for (int i = 0; i < drags; ++i) {
            // A DIFFERENT anchor each time, which is what makes these separate
            // drags rather than one drag's frames: `moved_chain` replaces a
            // leading grab only when the centre and the radius match, so an
            // unmoved anchor would measure a chain of depth one N times.
            const kernel::cfloat3 centre = cf3(1.0f, 0.002f * static_cast<float>(i), 0.0f);
            std::optional<session::SdfMoveTransaction> tx =
                session::SdfMoveTransaction::begin(doc, layer, centre, settings, policy);
            if (!tx) {
                state.SkipWithError("the transaction did not begin; nothing is being measured");
                return;
            }
            tx->update(cf3(0.0f, 0.004f, 0.0f));
            if (!tx->commit(nullptr)) {
                state.SkipWithError("the commit was refused; nothing is being measured");
                return;
            }
            if (tx->budget().consolidated) consolidations += 1;
        }
        state.PauseTiming();
        const scene::FieldReport report = scene::report_layer(doc.layers.front());
        chain = static_cast<double>(report.longest_deformer_chain);
        step = static_cast<double>(report.safe_step_scale);
        state.ResumeTiming();
    }
    state.counters["drags"] = static_cast<double>(drags);
    // The two numbers the policy is written against. With it off they are the
    // damage a session does to a layer; with it on they are what it is held to.
    state.counters["chain"] = chain;
    state.counters["safe_step_scale"] = step;
    state.counters["consolidations"] = consolidations;
}

}  // namespace

void BM_SdfMoveRepeated10(benchmark::State& state) { move_repeated(state, 10, false); }
BENCHMARK(BM_SdfMoveRepeated10)->Unit(benchmark::kMillisecond)->Iterations(20);

void BM_SdfMoveRepeated100(benchmark::State& state) { move_repeated(state, 100, false); }
BENCHMARK(BM_SdfMoveRepeated100)->Unit(benchmark::kMillisecond)->Iterations(5);

// The same hundred drags with the policy ON. It costs a bake where it fires,
// which is why this row is slower than the one above and why the gate on it is
// a counter rather than a time.
void BM_SdfMoveRepeatedPolicy100(benchmark::State& state) { move_repeated(state, 100, true); }
BENCHMARK(BM_SdfMoveRepeatedPolicy100)->Unit(benchmark::kMillisecond)->Iterations(3);
// -- the history a dab stops paying for (sdf-prefix-cache) -------------------
//
// #306 measured the shape of the problem and this group measures the cure. A
// dirty region over worked geometry re-evaluates every item that contributes
// there and almost none of them changed: one dab into 12 bricks cost 0.23 ms at
// 200 items and 18.07 ms at 50,000, so an artist's cost per stroke followed
// what they had already sculpted. `SdfPrefixCache` samples the old roots into
// an fp32 volume and keeps the nodes, so the same 12 bricks fold a 64-item
// suffix onto a stored seed instead of replaying the whole list.
//
// THE FIXED WORK IS THE 12 BRICKS, and the HISTORY is what grows. That is the
// only shape in which the claim can be read: a benchmark that grew the dirty
// region with the document would measure the region. Both sides evaluate the
// identical points through the identical entry point (`SdfSourceField`) and
// differ only in whether a prefix was built first — and they are bit-identical
// where the volume covers the window, which is the property
// test_sdf_prefix_cache.cpp asserts and this group never re-checks.
//
// TWO DISTRIBUTIONS, because they are two different documents to the cull and
// to the bake even though they are the same number of roots. SPREAD scatters
// the dabs over the whole sphere, which is what a host's own `abi_sculpt`
// fixture does and what a survey stroke looks like; PILED lands every dab in
// one patch, which is what actually happens when an artist works a detail —
// and it is the case where the dirty bricks sit under the deepest stack.
//
// THE FALLBACK COUNTERS ARE THE HONEST HALF. The far-bound rule (see
// sdf_prefix_cache.h) sends a window to the prefix TAPE wherever the volume
// does not store every sample of it, which is correct and slow, so a row whose
// `fallback_windows` is high is a row where the cache is not working rather
// than one where it is wrong. They are counts, identical on every machine, and
// that is what makes them gateable where a millisecond is not.
namespace {

constexpr float kHistoryCell = 0.05f;
// A WIDER BAND THAN THE DEFAULT THREE CELLS, and the reason is the far-bound
// rule rather than fidelity: a query brick spans eight cells, so a band only
// six cells thick cannot store every sample of a brick that straddles the
// surface and every window would fall back to the tape. The band a host picks
// for a prefix has to cover the windows it means to serve.
constexpr float kHistoryBand = 0.30f;
// The dirty region #306 measured, held FIXED while the history grows.
constexpr int kHistoryBricks = 12;
// What stays live in front of the boundary. The suffix is what still costs per
// evaluation, so this — and not the document — is what a cached dab pays for.
constexpr std::size_t kHistoryLiveSuffix = 64;

session::SdfPrefixPolicy history_policy() {
    session::SdfPrefixPolicy policy;
    policy.cell_size = kHistoryCell;
    policy.band = kHistoryBand;
    policy.padding = kHistoryBand;
    policy.min_history_roots = 256;
    policy.keep_live_suffix_roots = kHistoryLiveSuffix;
    policy.max_bytes = 512u << 20;  // 0 would DISABLE the cache, not unbound it
    return policy;
}

// SPREAD: `roots` dabs over the whole unit sphere on a Fibonacci shell, the
// same distribution the C ABI fixture above builds. Every part of the surface
// carries history, so the dirty window below sits over a full stack wherever it
// is put.
scene::Document history_spread(int roots) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("history");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    l.sdf->insert(base);
    const double golden = 0.6180339887;
    for (int i = 1; i < roots; ++i) {
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.04f);
        const double u = std::fmod(static_cast<double>(i) * golden, 1.0);
        const double v = (static_cast<double>(i) + 0.5) / static_cast<double>(roots);
        const double phi = std::acos(1.0 - 2.0 * v);
        const double th = 6.283185307 * u;
        dab.xform.position = cf3(static_cast<float>(std::sin(phi) * std::cos(th)),
                                 static_cast<float>(std::cos(phi)),
                                 static_cast<float>(std::sin(phi) * std::sin(th)));
        l.sdf->insert(dab);
    }
    return doc;
}

// PILED: `sculpted_sphere` already is this — every dab lands in one patch at
// the -x pole — so it is named rather than re-written, and the probe window
// below sits on that patch. Both fixtures therefore share a surface and a
// window and differ only in where the history is.
scene::Document history_piled(int roots) { return sculpted_sphere(roots); }

// Twelve dim-8 bricks that STRADDLE THE SURFACE at the -x pole, on the lattice
// a brick key implies (origin = key * kBrickDim * cell), which is the shape a
// refill request has. Chosen by walking a 4x3 grid of (y, z) brick columns and
// taking, in each, the brick the sphere actually passes through: a fixed block
// of keys would spend most of its twelve on empty space, where every window
// falls back and the row measures the tape rather than the cache.
std::vector<float> history_probe_points() {
    const float span = kHistoryCell * static_cast<float>(field::kBrickDim);
    std::vector<float> pts;
    pts.reserve(static_cast<std::size_t>(kHistoryBricks) * field::kBrickSamples * 3);
    for (int ky = -2; ky <= 1; ++ky) {
        for (int kz = -1; kz <= 1; ++kz) {
            const float yc = (static_cast<float>(ky) + 0.5f) * span;
            const float zc = (static_cast<float>(kz) + 0.5f) * span;
            const float r2 = yc * yc + zc * zc;
            const float xs = -std::sqrt(std::max(0.05f, 1.0f - r2));
            const int kx = static_cast<int>(std::floor(xs / span));
            const float o[3] = {static_cast<float>(kx) * span, static_cast<float>(ky) * span,
                                static_cast<float>(kz) * span};
            for (int k = 0; k <= field::kBrickDim; ++k)
                for (int j = 0; j <= field::kBrickDim; ++j)
                    for (int i = 0; i <= field::kBrickDim; ++i) {
                        pts.push_back(o[0] + static_cast<float>(i) * kHistoryCell);
                        pts.push_back(o[1] + static_cast<float>(j) * kHistoryCell);
                        pts.push_back(o[2] + static_cast<float>(k) * kHistoryCell);
                    }
        }
    }
    return pts;
}

// One iteration is the 12 bricks, ONE `fill_points` CALL PER BRICK. That is not
// a detail of the harness: `fill_points` decides the far-bound question for the
// whole call, so a single 8,748-point call would drag every brick onto the slow
// path as soon as one of them was uncovered — which is exactly why
// `SdfSourceField::block_fill` splits per brick, and this fixture drives it the
// same way a bake through it would.
void history_eval(benchmark::State& state, scene::Document doc, int roots, bool with_prefix) {
    const scene::LayerId layer = doc.layers.front().id;
    const session::SdfPrefixPolicy policy = history_policy();
    session::SdfPrefixCache cache;
    if (with_prefix && !cache.build(doc, layer, policy, eval::pooled_bake_eval())) {
        state.SkipWithError("the prefix did not build; nothing is being measured");
        return;
    }
    // A NULL cache on the control side, not an empty one: a null cache is
    // documented to be the full walk, and that is the "before" this pair is
    // about. `open` never builds either way.
    std::optional<session::SdfSourceField> src = session::SdfSourceField::open(
        doc, layer, with_prefix ? &cache : nullptr, policy, eval::pooled_bake_eval());
    if (!src) {
        state.SkipWithError("the source did not open; nothing is being measured");
        return;
    }
    if (with_prefix && !src->accelerated()) {
        // The row would still be correct and would silently be the control,
        // which is precisely the failure the cache is designed to have.
        state.SkipWithError("the prefix did not attach; the control is being measured twice");
        return;
    }
    const std::vector<float> pts = history_probe_points();
    std::vector<float> out(static_cast<std::size_t>(kHistoryBricks) * field::kBrickSamples);
    for (auto _ : state) {
        for (int b = 0; b < kHistoryBricks; ++b) {
            const std::size_t at = static_cast<std::size_t>(b) * field::kBrickSamples;
            src->fill_points(pts.data() + at * 3, field::kBrickSamples, out.data() + at);
        }
        // A SEPARATE sink, never a variable read after the loop -- see the note
        // in BM_SdfSmoothStandalone.
        float sink = out.front();
        benchmark::DoNotOptimize(sink);
    }
    const double iters = static_cast<double>(state.iterations());
    state.counters["roots"] = static_cast<double>(roots);
    state.counters["prefix_roots"] = static_cast<double>(src->prefix_roots());
    state.counters["suffix_roots"] = static_cast<double>(src->suffix_roots());
    // PER ITERATION, so the pair reads as "of the twelve bricks, how many were
    // served from the volume". The control reports zero of each because a null
    // cache has nothing to count, which is the honest reading and not a hole.
    state.counters["seeded_windows"] = static_cast<double>(cache.stats().seeded_windows) / iters;
    state.counters["fallback_windows"] =
        static_cast<double>(cache.stats().fallback_windows) / iters;
    state.counters["bricks"] = kHistoryBricks;
}

// Building the prefix, ON ITS OWN AND UNAMORTISED. It is a whole-layer bake of
// the roots behind the boundary and it is not free, and a group that only
// showed the hit would be hiding the half a host has to schedule. A FRESH cache
// per iteration, because `build` returns the entry it already holds and would
// otherwise measure a hash lookup.
void prefix_build(benchmark::State& state, scene::Document doc, int roots) {
    const scene::LayerId layer = doc.layers.front().id;
    const session::SdfPrefixPolicy policy = history_policy();
    std::size_t bricks = 0, bytes = 0, prefix_roots = 0;
    for (auto _ : state) {
        session::SdfPrefixCache cache;
        const session::SdfPrefixField* built =
            cache.build(doc, layer, policy, eval::pooled_bake_eval());
        if (!built) {
            state.SkipWithError("the prefix did not build; nothing is being measured");
            return;
        }
        bricks = built->volume.brick_count();
        bytes = built->bytes();
        prefix_roots = built->prefix_roots;
        std::size_t sink = bricks;  // see the note in BM_SdfSmoothStandalone
        benchmark::DoNotOptimize(sink);
    }
    state.counters["roots"] = static_cast<double>(roots);
    state.counters["prefix_roots"] = static_cast<double>(prefix_roots);
    state.counters["stored_bricks"] = static_cast<double>(bricks);
    state.counters["bytes"] = static_cast<double>(bytes);
    state.counters["MiB"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}  // namespace

void BM_SdfHistoryFullSpread5000(benchmark::State& state) {
    history_eval(state, history_spread(5000), 5000, false);
}
BENCHMARK(BM_SdfHistoryFullSpread5000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryPrefixSpread5000(benchmark::State& state) {
    history_eval(state, history_spread(5000), 5000, true);
}
BENCHMARK(BM_SdfHistoryPrefixSpread5000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryFullSpread20000(benchmark::State& state) {
    history_eval(state, history_spread(20000), 20000, false);
}
BENCHMARK(BM_SdfHistoryFullSpread20000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryPrefixSpread20000(benchmark::State& state) {
    history_eval(state, history_spread(20000), 20000, true);
}
BENCHMARK(BM_SdfHistoryPrefixSpread20000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryFullPiled5000(benchmark::State& state) {
    history_eval(state, history_piled(5000), 5000, false);
}
BENCHMARK(BM_SdfHistoryFullPiled5000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryPrefixPiled5000(benchmark::State& state) {
    history_eval(state, history_piled(5000), 5000, true);
}
BENCHMARK(BM_SdfHistoryPrefixPiled5000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryFullPiled20000(benchmark::State& state) {
    history_eval(state, history_piled(20000), 20000, false);
}
BENCHMARK(BM_SdfHistoryFullPiled20000)->Unit(benchmark::kMillisecond);

void BM_SdfHistoryPrefixPiled20000(benchmark::State& state) {
    history_eval(state, history_piled(20000), 20000, true);
}
BENCHMARK(BM_SdfHistoryPrefixPiled20000)->Unit(benchmark::kMillisecond);

// The cost that must NOT be hidden behind the four hits above. FIXED ITERATION
// COUNTS, because a whole-layer bake left to fill a time budget is minutes of
// CI for a number that does not get more accurate.
void BM_SdfPrefixBuildSpread5000(benchmark::State& state) {
    prefix_build(state, history_spread(5000), 5000);
}
BENCHMARK(BM_SdfPrefixBuildSpread5000)->Unit(benchmark::kMillisecond)->Iterations(3);

void BM_SdfPrefixBuildSpread20000(benchmark::State& state) {
    prefix_build(state, history_spread(20000), 20000);
}
BENCHMARK(BM_SdfPrefixBuildSpread20000)->Unit(benchmark::kMillisecond)->Iterations(1);

void BM_SdfPrefixBuildPiled5000(benchmark::State& state) {
    prefix_build(state, history_piled(5000), 5000);
}
BENCHMARK(BM_SdfPrefixBuildPiled5000)->Unit(benchmark::kMillisecond)->Iterations(3);

void BM_SdfPrefixBuildPiled20000(benchmark::State& state) {
    prefix_build(state, history_piled(20000), 20000);
}
BENCHMARK(BM_SdfPrefixBuildPiled20000)->Unit(benchmark::kMillisecond)->Iterations(1);

// -- a lazy begin, and the first dab that pays for it ------------------------
//
// `SdfSmoothTransaction::begin` used to sample the whole finite layer, and the
// row above (BM_SdfSmoothTransactionBegin) used to be that bake. It now
// evaluates NOTHING — a compile, an index for the working lattice, and a digest
// — so the pair of sizes here holds the property that replaced it: begin no
// longer follows the MODEL. It is not constant, and this group does not claim
// it is: a compile and a digest are both linear in the root count, and what
// went away is the sampling, which was linear in roots TIMES samples.
//
// The dab is where the work went, so the second pair measures the FIRST update
// after a lazy begin — the one that materializes, and the only one that can be
// slow. With a prefix built, that materialization folds a 64-item suffix onto a
// stored seed; without one it replays the whole edit list, which is the #306
// cost arriving at the dab instead of at pointer-down. `materialized_bricks` is
// the count that says the two rows did the same amount of work.
namespace {

// A WIDER BAND STILL THAN THE HISTORY ROWS', and the dab is the reason. A
// dab's dependency region is a BALL -- `smooth_dab`'s 0.25 region radius plus
// the stencil's reach plus a brick of outward rounding -- and a prefix volume
// is a SHELL, so any band narrower than that ball guarantees that the bricks
// the ball reaches inside and outside it are not stored and every one of them
// falls back to the prefix tape. Measured at the history rows' 0.30: 37 of the
// 112 bricks seeded and 75 fell back, and the pair read 0.69 rather than the
// 0.03 the same mechanism gives on a covered window. That is the far-bound
// rule working exactly as written, and it is a POLICY question rather than a
// defect: a host caches a band that covers the windows it means to serve.
//
// 0.60 is the brush's own reach, and it is NOT the number that reads best.
// Widening to 0.90 leaves 20 fallbacks of 159 and the pair reads 0.16 instead
// of 0.27 -- for a fatter prefix to build and hold, bought against edge bricks
// a ball reaches and a shell never stores. The residual fallbacks are that
// geometry rather than a machine, and the counters below say how many.
constexpr float kSculptBand = 0.60f;

// The same sampling the Smooth rows above use, plus a prefix the gesture is
// allowed to reach for. The three sampling numbers are copied over the prefix
// policy by `begin` itself, so a caller cannot ask for a seed off a lattice the
// gesture is not using.
session::SdfSculptPolicy lazy_policy() {
    session::SdfSculptPolicy policy;
    policy.cell_size = kHistoryCell;
    policy.band = kSculptBand;
    policy.padding = kSculptBand;
    policy.prefix.min_history_roots = 256;
    policy.prefix.keep_live_suffix_roots = kHistoryLiveSuffix;
    policy.prefix.max_bytes = 512u << 20;
    return policy;
}

// The prefix policy `begin` will compose, so a cache built out here is the one
// `find` matches inside. A key includes the resolution; a mismatch would be a
// silent miss and a row that measured the control twice.
session::SdfPrefixPolicy lazy_prefix_policy() {
    const session::SdfSculptPolicy sculpt = lazy_policy();
    session::SdfPrefixPolicy prefix = sculpt.prefix;
    prefix.cell_size = sculpt.cell_size;
    prefix.band = sculpt.band;
    prefix.padding = sculpt.padding;
    return prefix;
}

void smooth_begin(benchmark::State& state, scene::Document doc, int roots) {
    const scene::LayerId layer = doc.layers.front().id;
    const session::SdfSculptPolicy policy = lazy_policy();
    std::size_t samples = 0;
    for (auto _ : state) {
        std::optional<session::SdfSmoothTransaction> tx =
            session::SdfSmoothTransaction::begin(doc, layer, policy, eval::pooled_bake_eval());
        if (!tx) {
            state.SkipWithError("the transaction did not begin; nothing is being measured");
            return;
        }
        samples = tx->preview_volume().sample_count();
        std::size_t sink = samples;  // see the note in BM_SdfSmoothStandalone
        benchmark::DoNotOptimize(sink);
        tx->cancel();
    }
    state.counters["roots"] = static_cast<double>(roots);
    // ZERO, and that is the measurement: a lazy begin materializes no brick, so
    // the working volume it hands back stores no sample at all.
    state.counters["samples"] = static_cast<double>(samples);
}

// One dab straight after a lazy begin. The transaction is rebuilt per
// iteration, UNTIMED, because materialization is a once-per-brick event: left
// to run against one transaction every iteration after the first would reuse
// what the first materialized and the row would report the steady-state dab
// that BM_SdfSmoothTransactionUpdate already reports.
void smooth_first_update(benchmark::State& state, int roots, bool with_prefix) {
    scene::Document doc = history_piled(roots);
    const scene::LayerId layer = doc.layers.front().id;
    const session::SdfSculptPolicy policy = lazy_policy();
    session::SdfPrefixCache cache;
    if (with_prefix &&
        !cache.build(doc, layer, lazy_prefix_policy(), eval::pooled_bake_eval())) {
        state.SkipWithError("the prefix did not build; nothing is being measured");
        return;
    }
    const field::RelaxSettings dab = smooth_dab();
    session::SdfSmoothMaterializationStats stats;
    for (auto _ : state) {
        state.PauseTiming();
        std::optional<session::SdfSmoothTransaction> tx = session::SdfSmoothTransaction::begin(
            doc, layer, policy, eval::pooled_bake_eval(), nullptr, with_prefix ? &cache : nullptr);
        if (!tx) {
            state.SkipWithError("the transaction did not begin; nothing is being measured");
            return;
        }
        state.ResumeTiming();

        const session::SdfSculptDirty dirty = tx->update(dab);

        state.PauseTiming();
        stats = tx->materialization();
        tx->cancel();
        state.ResumeTiming();
        std::size_t sink = dirty.touched_bricks;  // see BM_SdfSmoothStandalone
        benchmark::DoNotOptimize(sink);
    }
    if (stats.materialized_bricks == 0) {
        state.SkipWithError("the dab materialized nothing; nothing is being measured");
        return;
    }
    const double iters = static_cast<double>(state.iterations());
    state.counters["roots"] = static_cast<double>(roots);
    // THE PAIR'S OWN CONTROL: both rows must materialize the same bricks, or
    // the ratio between them is measuring two different dabs.
    state.counters["materialized_bricks"] = static_cast<double>(stats.materialized_bricks);
    state.counters["reused_bricks"] = static_cast<double>(stats.reused_bricks);
    state.counters["updates"] = static_cast<double>(stats.updates);
    // AND WHY THE RATIO IS WHAT IT IS. A dab's dependency region is a BALL,
    // and a prefix volume is a SHELL: the bricks that straddle the surface are
    // seeded and the ones the ball reaches inside and outside the band are not,
    // so a first update is part cache and part tape by construction. These two
    // counts are that split, per iteration, and they are what a reader should
    // check before blaming the machine for a modest margin.
    state.counters["seeded_windows"] = static_cast<double>(cache.stats().seeded_windows) / iters;
    state.counters["fallback_windows"] =
        static_cast<double>(cache.stats().fallback_windows) / iters;
}

}  // namespace

// The two larger models, against BM_SdfSmoothTransactionBegin's 193 above. Same
// fixture family (`sculpted_sphere`), so the three rows differ only in how much
// history sits behind the gesture.
void BM_SdfSmoothTransactionBegin5000(benchmark::State& state) {
    smooth_begin(state, history_piled(5000), 5000);
}
BENCHMARK(BM_SdfSmoothTransactionBegin5000)->Unit(benchmark::kMillisecond);

void BM_SdfSmoothTransactionBegin20000(benchmark::State& state) {
    smooth_begin(state, history_piled(20000), 20000);
}
BENCHMARK(BM_SdfSmoothTransactionBegin20000)->Unit(benchmark::kMillisecond);

void BM_SdfSmoothLazyFirstUpdateNoPrefix(benchmark::State& state) {
    smooth_first_update(state, 5000, false);
}
BENCHMARK(BM_SdfSmoothLazyFirstUpdateNoPrefix)->Unit(benchmark::kMillisecond)->Iterations(5);

void BM_SdfSmoothLazyFirstUpdateWithPrefix(benchmark::State& state) {
    smooth_first_update(state, 5000, true);
}
BENCHMARK(BM_SdfSmoothLazyFirstUpdateWithPrefix)->Unit(benchmark::kMillisecond)->Iterations(5);

// -- the preview transport, through the C ABI a host actually draws from -----
//
// `clay_sdf_smooth_preview_item` copies the WHOLE working volume and hands it
// over as a fresh item the caller owns. That is the right call for a host
// joining mid-gesture and the wrong one for a per-frame loop: a dab moves a
// ball of bricks and the host re-uploads the model.
// `clay_sdf_smooth_preview_delta_take` hands over only the bricks whose bytes
// are new.
//
// THE HEADLINE IS BYTES, NOT MILLISECONDS, and the `bytes` counter on both rows
// is the number the pair exists to report: it is exact, deterministic and the
// same on every machine, where the wall clock of a memcpy is a property of the
// runner's memory bandwidth. `delta_frac` is the two divided, which is the
// gateable form.
//
// THE WORKING SET IS WARMED FIRST. Every dab of the path is run once, untimed,
// so the timed loop is a steady-state frame — the model already materialized,
// the dab moving samples inside it — which is the state a stroke spends almost
// all of its time in. Measured without the warm-up the delta row would carry
// the materialization of a growing model and would understate itself.
namespace {

constexpr int kPreviewHistory = 2000;
constexpr int kPreviewPathDabs = 16;

// A dab sweeping a short arc of the shell `abi_sculpt` puts its dabs on, so
// successive frames move different samples rather than re-averaging one ball
// into its own fixed point.
clay_relax_params preview_dab(int i) {
    clay_relax_params p{};
    p.struct_size = sizeof(p);
    p.strength = 0.5f;
    p.radius_cells = 1;
    p.iterations = 1;
    const float a = 0.6f * (static_cast<float>(i) / static_cast<float>(kPreviewPathDabs) - 0.5f);
    p.centre[0] = std::cos(a);
    p.centre[1] = std::sin(a);
    p.centre[2] = 0.0f;
    p.region_radius = 0.2f;
    p.falloff = 0.0f;
    p.mask = nullptr;
    return p;
}

void abi_preview(benchmark::State& state, bool delta) {
    // Layer id 1 is `abi_sculpt`'s one layer, as the refill rows above already
    // assume.
    clay_document* d = abi_sculpt(kPreviewHistory);
    clay_sculpt_policy policy{};
    policy.struct_size = sizeof(policy);
    policy.cell_size = kHistoryCell;
    policy.band = kHistoryBand;
    policy.padding = kHistoryBand;
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d, 1, &policy, nullptr);
    if (!tx) {
        state.SkipWithError("the transaction did not begin; nothing is being measured");
        clay_document_destroy(d);
        return;
    }
    for (int i = 0; i < kPreviewPathDabs; ++i) {
        const clay_relax_params p = preview_dab(i);
        clay_sdf_smooth_update(tx, &p, nullptr, nullptr);
    }
    // What the whole working volume holds now, which is what the snapshot call
    // copies every frame. Read through the delta's own `info` call BEFORE
    // anything is taken: nothing has been taken yet, and a brick enters the
    // delta exactly when it is materialized, so what is waiting is precisely
    // the set of bricks the volume stores.
    clay_sdf_preview_delta_info info{};
    info.struct_size = sizeof(info);
    if (clay_sdf_smooth_preview_delta_info(tx, &info) != CLAY_OK || info.sample_floats == 0) {
        state.SkipWithError("the preview holds nothing; nothing is being measured");
        clay_sdf_smooth_destroy(tx);
        clay_document_destroy(d);
        return;
    }
    const double snapshot_bytes = static_cast<double>(info.sample_floats) * sizeof(float);

    std::vector<clay_sdf_preview_brick> bricks;
    std::vector<float> samples;
    double taken_bytes = 0, taken_bricks = 0;
    int at = 0;
    for (auto _ : state) {
        // THE DAB IS NOT MEASURED. Both rows pay the identical update and it is
        // two orders of magnitude wider than either transport, so leaving it
        // inside made the pair read 0.154 ms against 0.151 ms -- the same
        // number twice, with the thing being compared invisible underneath it.
        state.PauseTiming();
        const clay_relax_params p = preview_dab(at++ % kPreviewPathDabs);
        clay_sdf_smooth_update(tx, &p, nullptr, nullptr);
        state.ResumeTiming();
        if (!delta) {
            // THE WHOLE-VOLUME COPY, destroy included: the item is the caller's
            // and freeing it is part of what the transport costs.
            clay_item* item = nullptr;
            if (clay_sdf_smooth_preview_item(tx, &item) != CLAY_OK) {
                state.SkipWithError("the snapshot failed; nothing is being measured");
                break;
            }
            clay_item_destroy(item);
            taken_bytes += snapshot_bytes;
            continue;
        }
        clay_sdf_preview_delta_info frame{};
        frame.struct_size = sizeof(frame);
        if (clay_sdf_smooth_preview_delta_info(tx, &frame) != CLAY_OK) {
            state.SkipWithError("the delta info failed; nothing is being measured");
            break;
        }
        // GROWN, NEVER SHRUNK, and never inside the measurement after the first
        // frames: a host owns these buffers across a stroke, and reallocating
        // one per frame would measure the allocator.
        if (bricks.size() < frame.brick_count) bricks.resize(frame.brick_count);
        if (samples.size() < frame.sample_floats) samples.resize(frame.sample_floats);
        std::uint64_t got_bricks = 0, got_samples = 0;
        if (clay_sdf_smooth_preview_delta_take(tx, bricks.data(), bricks.size(), samples.data(),
                                               samples.size(), &got_bricks,
                                               &got_samples) != CLAY_OK) {
            state.SkipWithError("the delta take failed; nothing is being measured");
            break;
        }
        taken_bytes += static_cast<double>(got_samples) * sizeof(float);
        taken_bricks += static_cast<double>(got_bricks);
    }
    const double iters = static_cast<double>(state.iterations());
    state.counters["nodes"] = kPreviewHistory;
    // The whole working volume, for scale on both rows.
    state.counters["snapshot_bytes"] = snapshot_bytes;
    // WHAT THIS ROW ACTUALLY COPIED, per frame. On the snapshot row it is the
    // whole volume by construction; on the delta row it is what the dab moved.
    state.counters["bytes"] = taken_bytes / iters;
    state.counters["bricks"] = taken_bricks / iters;
    // The two divided, which is the machine-independent form of the claim.
    state.counters["delta_frac"] = (taken_bytes / iters) / snapshot_bytes;
    clay_sdf_smooth_destroy(tx);
    clay_document_destroy(d);
}

}  // namespace

void BM_CAbiSmoothPreviewFullSnapshot(benchmark::State& state) { abi_preview(state, false); }
BENCHMARK(BM_CAbiSmoothPreviewFullSnapshot)->Unit(benchmark::kMillisecond)->Iterations(200);

void BM_CAbiSmoothPreviewDelta(benchmark::State& state) { abi_preview(state, true); }
BENCHMARK(BM_CAbiSmoothPreviewDelta)->Unit(benchmark::kMillisecond)->Iterations(200);

// -- adaptive topology: the scaling gate --------------------------------------
//
// THE CLAIM THESE MEASURE, and it is the one that decides whether this feature
// is usable at all: for a FIXED brush footprint, the stamp cost stays in one
// band as the surface grows across orders of magnitude. A 50x model must not be
// a 50x stamp.
//
// Run these at the three sizes the requirement names — 100k, 1M and 5M
// triangles — and compare the per-stamp times. The unit test alongside them
// asserts the band at smaller sizes so it can run in CI; these are where the
// real numbers come from.

namespace {

// A GRID AT FIXED SPACING. `n` grows the model's EXTENT, not its density.
//
// The obvious fixture — subdivide a unit sphere further for each size — is the
// wrong one, and measuring with it is how the O(surface) cost in the topology
// operators stayed hidden. Subdividing a fixed-radius sphere makes the surface
// finer, so a brush of fixed world radius covers quadratically more faces: the
// stamp does more work because it was ASKED to, and a rising curve says nothing
// about whether the cost is local. Here the triangles are the same size at
// every `n`, so a fixed-radius brush covers the same face count in all three
// rows and the only thing that changes is how much surface it is embedded in.
// That is the question the scaling gate asks.
clay::mesh::Mesh bench_surface_patch(int n, float spacing) {
    using namespace clay;
    using namespace clay::kernel;
    mesh::Mesh m;
    const float half = spacing * static_cast<float>(n) * 0.5f;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + spacing * static_cast<float>(x), 0.0f,
                                      -half + spacing * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

}  // namespace


// -- global voxel remesh (add-voxel-remesher) --------------------------------
//
// The claim these hold is a SCALING one, not a time one: the expensive per
// sample work follows the source's surface area and the band's thickness, and
// not the volume of the bounding box. A remesh built on `mesh::to_field`
// unchanged would evaluate every brick of the box — 24 million BVH queries at
// longest-axis 256, each carrying a generalized winding number — and the
// ratios below are what a revert to that would blow.
namespace {

// A UV sphere at a chosen radius and tessellation. Two dials, because two
// different scaling questions are asked of it: RADIUS at a fixed voxel size
// (surface grows as r^2 while the box grows as r^3), and TRIANGLE COUNT at a
// fixed shape and resolution (the sampling should barely notice).
clay::mesh::Mesh bench_sphere(float radius, int rings, int segments,
                              clay::kernel::cfloat3 centre = clay::kernel::cf3(0, 0, 0)) {
    using namespace clay;
    using kernel::cf3;
    mesh::Mesh m;
    const float pi = 3.14159265358979323846f;
    m.positions.push_back(centre + cf3(0, radius, 0));
    for (int r = 1; r < rings; ++r) {
        const float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        const float y = std::cos(phi), rr = std::sin(phi);
        for (int s = 0; s < segments; ++s) {
            const float th = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            m.positions.push_back(centre + cf3(rr * std::cos(th), y, rr * std::sin(th)) * radius);
        }
    }
    m.positions.push_back(centre + cf3(0, -radius, 0));
    const std::uint32_t bottom = static_cast<std::uint32_t>(m.positions.size() - 1);
    auto at = [&](int r, int s) {
        return static_cast<std::uint32_t>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s)
        m.indices.insert(m.indices.end(), {0u, at(1, s + 1), at(1, s)});
    for (int r = 1; r < rings - 1; ++r)
        for (int s = 0; s < segments; ++s)
            m.indices.insert(m.indices.end(), {at(r, s), at(r, s + 1), at(r + 1, s),
                                               at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)});
    for (int s = 0; s < segments; ++s)
        m.indices.insert(m.indices.end(), {bottom, at(rings - 1, s), at(rings - 1, s + 1)});
    return m;
}

clay::mesh::VoxelRemeshParams remesh_at(std::uint32_t resolution) {
    clay::mesh::VoxelRemeshParams p;
    p.longest_axis_resolution = resolution;
    return p;
}

void run_remesh(benchmark::State& state, const clay::mesh::Mesh& source,
                const clay::mesh::VoxelRemeshParams& params) {
    using namespace clay;
    std::uint64_t triangles = 0, samples = 0;
    for (auto _ : state) {
        mesh::VoxelRemeshResult r = mesh::voxel_remesh(source, params);
        benchmark::DoNotOptimize(r.mesh.triangle_count());
        triangles = r.report.result_triangles;
        samples = r.report.active_samples;
    }
    state.counters["result_tris"] = static_cast<double>(triangles);
    state.counters["band_samples"] = static_cast<double>(samples);
    state.counters["source_tris"] = static_cast<double>(source.triangle_count());
}

}  // namespace

void BM_VoxelRemeshSphere128(benchmark::State& state) {
    run_remesh(state, bench_sphere(1.0f, 32, 64), remesh_at(128));
}
BENCHMARK(BM_VoxelRemeshSphere128)->Unit(benchmark::kMillisecond);

void BM_VoxelRemeshSphere256(benchmark::State& state) {
    run_remesh(state, bench_sphere(1.0f, 32, 64), remesh_at(256));
}
BENCHMARK(BM_VoxelRemeshSphere256)->Unit(benchmark::kMillisecond);

// Two spheres crossing: the fusion path, and the case where the winding number
// is summed over a source that overlaps itself.
void BM_VoxelRemeshIntersections256(benchmark::State& state) {
    using namespace clay;
    using kernel::cf3;
    mesh::Mesh m = bench_sphere(0.6f, 24, 48, cf3(-0.35f, 0, 0));
    const mesh::Mesh other = bench_sphere(0.6f, 24, 48, cf3(0.35f, 0, 0));
    const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
    m.positions.insert(m.positions.end(), other.positions.begin(), other.positions.end());
    for (std::uint32_t i : other.indices) m.indices.push_back(base + i);
    run_remesh(state, m, remesh_at(256));
}
BENCHMARK(BM_VoxelRemeshIntersections256)->Unit(benchmark::kMillisecond);

// THE SPARSITY GATE, and the pair it is a pair for.
//
// Same voxel size, one sphere twice the radius of the other. The surface grows
// 4x and the bounding box 8x, so a domain that followed the box would cost
// about 8x and one that follows the band about 4x. Measured: 815,751 band
// samples against 3,230,930, a ratio of 3.96, and 243 ms against 1118 ms on a
// 24-core Linux desktop. The ratio gate in check_bench.py sits between 4 and 8,
// which is what makes a revert to the dense converter fail rather than merely
// look slower.
void BM_VoxelRemeshSmallBall(benchmark::State& state) {
    clay::mesh::VoxelRemeshParams p;
    p.resolution_mode = clay::mesh::VoxelRemeshResolutionMode::VoxelSize;
    p.voxel_size = 0.01f;
    run_remesh(state, bench_sphere(0.5f, 32, 64), p);
}
BENCHMARK(BM_VoxelRemeshSmallBall)->Unit(benchmark::kMillisecond);

void BM_VoxelRemeshLargeBall(benchmark::State& state) {
    clay::mesh::VoxelRemeshParams p;
    p.resolution_mode = clay::mesh::VoxelRemeshResolutionMode::VoxelSize;
    p.voxel_size = 0.01f;
    run_remesh(state, bench_sphere(1.0f, 32, 64), p);
}
BENCHMARK(BM_VoxelRemeshLargeBall)->Unit(benchmark::kMillisecond);

// SOURCE TRIANGLES AT A FIXED SHAPE AND RESOLUTION. The same sphere at the same
// voxel size, tessellated sixteen times as finely. The field is sampled at the
// same points and the BVH is deeper by four levels, so the remesh should cost
// slightly more and not proportionally more — a remesh whose cost tracked the
// input's triangle count would be doing per-triangle work it does not need to.
void BM_VoxelRemeshCoarseSource(benchmark::State& state) {
    run_remesh(state, bench_sphere(1.0f, 16, 32), remesh_at(128));
}
BENCHMARK(BM_VoxelRemeshCoarseSource)->Unit(benchmark::kMillisecond);

void BM_VoxelRemeshDenseSource(benchmark::State& state) {
    run_remesh(state, bench_sphere(1.0f, 64, 128), remesh_at(128));
}
BENCHMARK(BM_VoxelRemeshDenseSource)->Unit(benchmark::kMillisecond);

// Projection and attribute transfer, isolated: the same remesh with each off
// and on, so a regression in either is attributable rather than showing up as
// "the remesh got slower".
void BM_VoxelRemeshNoProjection(benchmark::State& state) {
    clay::mesh::VoxelRemeshParams p = remesh_at(128);
    p.project_to_source = false;
    p.preserve_colors = false;
    run_remesh(state, bench_sphere(1.0f, 32, 64), p);
}
BENCHMARK(BM_VoxelRemeshNoProjection)->Unit(benchmark::kMillisecond);

void BM_VoxelRemeshProjection(benchmark::State& state) {
    clay::mesh::VoxelRemeshParams p = remesh_at(128);
    p.project_to_source = true;
    p.preserve_colors = false;
    run_remesh(state, bench_sphere(1.0f, 32, 64), p);
}
BENCHMARK(BM_VoxelRemeshProjection)->Unit(benchmark::kMillisecond);

void BM_VoxelRemeshAttributeTransfer(benchmark::State& state) {
    clay::mesh::Mesh m = bench_sphere(1.0f, 32, 64);
    m.colors.assign(m.positions.size(), clay::kernel::cf3(0.5f, 0.4f, 0.3f));
    clay::mesh::VoxelRemeshParams p = remesh_at(128);
    p.project_to_source = true;
    p.preserve_colors = true;
    run_remesh(state, m, p);
}
BENCHMARK(BM_VoxelRemeshAttributeTransfer)->Unit(benchmark::kMillisecond);

// The preflight, which a host calls on every tick of a resolution slider. It
// must cost the triangles and the brick lattice and NOT the remesh: this is the
// row that fails if the estimate ever starts sampling anything.
void BM_VoxelRemeshEstimate256(benchmark::State& state) {
    using namespace clay;
    const mesh::Mesh source = bench_sphere(1.0f, 32, 64);
    const mesh::VoxelRemeshParams p = remesh_at(256);
    for (auto _ : state) {
        mesh::VoxelRemeshEstimate e = mesh::voxel_remesh_estimate(source, p);
        benchmark::DoNotOptimize(e);
    }
}
BENCHMARK(BM_VoxelRemeshEstimate256)->Unit(benchmark::kMillisecond);

// `state.range(0)` is the patch's side in quads: 2 * n^2 triangles, so 224 is
// ~100k, 707 is ~1M and 1581 is ~5M. The brush footprint is identical in all
// three; only the surface around it grows.
void BM_DynamicStamp(benchmark::State& state) {
    using namespace clay;
    using namespace clay::kernel;
    const int n = static_cast<int>(state.range(0));
    auto surface = mesh::DynamicSurface::from_mesh(bench_surface_patch(n, 0.02f));
    if (!surface) {
        state.SkipWithError("could not build the surface");
        return;
    }
    mesh::DynamicSculptor sculptor(*surface);

    mesh::MeshBrushSettings brush;
    brush.radius = 0.12f;  // a FIXED footprint, whatever the model's size
    brush.strength = 0.2f;
    mesh::DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = 4.0f;

    int i = 0;
    for (auto _ : state) {
        brush.center = cf3(0.02f * static_cast<float>(i % 5) - 0.04f, 0.0f, 0.0f);
        sculptor.stamp(mesh::MeshBrush::Draw, brush, topo);
        ++i;
    }
    state.counters["faces"] = static_cast<double>(surface->stats().faces);
    state.counters["bytes_per_face"] =
        static_cast<double>(surface->bytes()) / static_cast<double>(surface->stats().faces);
}
BENCHMARK(BM_DynamicStamp)
    ->Arg(224)   // ~100k triangles
    ->Arg(707)   // ~1M
    ->Arg(1581)  // ~5M
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

// The same stamp with topology OFF, so the deformation and the remesh can be
// told apart rather than reported as one number.
void BM_DynamicStampNoTopology(benchmark::State& state) {
    using namespace clay;
    using namespace clay::kernel;
    const int n = static_cast<int>(state.range(0));
    auto surface = mesh::DynamicSurface::from_mesh(bench_surface_patch(n, 0.02f));
    if (!surface) {
        state.SkipWithError("could not build the surface");
        return;
    }
    mesh::DynamicSculptor sculptor(*surface);
    mesh::MeshBrushSettings brush;
    brush.radius = 0.12f;
    brush.strength = 0.2f;
    mesh::DynamicTopologySettings topo;
    topo.enabled = false;

    int i = 0;
    for (auto _ : state) {
        brush.center = cf3(0.02f * static_cast<float>(i % 5) - 0.04f, 0.0f, 0.0f);
        sculptor.stamp(mesh::MeshBrush::Draw, brush, topo);
        ++i;
    }
    state.counters["faces"] = static_cast<double>(surface->stats().faces);
}
BENCHMARK(BM_DynamicStampNoTopology)
    ->Arg(224)   // ~100k triangles
    ->Arg(707)   // ~1M
    ->Arg(1581)  // ~5M
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

// -- what the neutral automask's virtual costs (add-shared-brush-runtime 6.8) --
//
// THE ONE PLACE THIS CHANGE ADDED AN INDIRECT CALL. `compute_automask`'s core
// is now written against `WorkItemTopology`, an abstract base with two methods,
// so the boundary fade and the connectivity flood reach a representation
// through a virtual rather than through a `Mesh` and an `Adjacency` directly.
// That is what lets one automask serve three representations instead of three
// copies of it serving one each, and it is not free.
//
// WHY IT WAS JUDGED ACCEPTABLE, and what these cases exist to check that
// judgement against. `sculpt_kernels.h` explicitly REFUSED a virtual for the
// neighbour lookup, and the reason does not carry here: that would have been a
// call in the INNERMOST loop of a smoothing pass, per neighbour, per pass, on a
// million-vertex surface. This is one call per WORKSET entry, on two of five
// factors, in a pass that already calls `acos` per entry. The alternative was a
// template on the topology, which reinstates the three instantiations the
// neutral work item exists to avoid.
//
// SO THE PAIR IS THE MEASUREMENT: the same stamp on the same fixture with the
// factor off and on. The difference is the automask ENTIRELY — its arithmetic,
// its arena scope and its virtual — which is the honest bound on the virtual
// rather than a number that flatters it. A regression that made the indirect
// call expensive (a topology object rebuilt per entry, say) shows up as the
// ratio moving, and the ratio is what to read: this box is shared, so an
// absolute time here says as much about the neighbours as about the code.
//
// NO CEILING IS ADDED TO check_bench.py. The gate's own note says a ceiling has
// to be set from the RUNNER rather than from a development machine, and these
// numbers were taken on a 24-core desktop under other load. Reporting is the
// honest width until someone reads them off CI.
//
// WHAT IT MEASURED, against a44b1f5 built from the same source with the same
// four cases. Nine repetitions of 200 iterations each, P50 in microseconds,
// load average 7-12 before and after both runs, so the RATIOS are the reading
// and the absolutes are not:
//
//                                          main      this branch
//   fixed, no automask       n=224        158.00       71.02   (0.45x)
//   fixed, no automask       n=707       1258.12      583.97   (0.46x)
//   fixed, boundary automask n=224        350.28      170.39   (0.49x)
//   fixed, boundary automask n=707       2527.86     1615.53   (0.64x)
//   adaptive, no automask    n=224        153.23      145.46   (0.95x)
//   adaptive, no automask    n=707        141.59      168.28   (1.19x)
//   adaptive, boundary       n=224        144.65      550.11   (3.80x)
//   adaptive, boundary       n=707        141.40     1143.02   (8.08x)
//
// THREE THINGS TO READ OUT OF THAT, and the third is the point of the file.
//
// The VIRTUAL DID NOT COST WHAT IT LOOKED LIKE IT WOULD. On the fixed path —
// the only one where the automask ran on main at all — the neutral rewrite is
// 2.0x and 1.6x FASTER than the direct `Mesh`/`Adjacency` implementation it
// replaced, indirect call and all, because the five per-stamp `std::vector`s it
// used to build became arena blocks. An indirect call per workset entry is
// cheaper than a malloc per stamp by a wide margin.
//
// THE ADAPTIVE ROWS ARE NOT A REGRESSION, THEY ARE THE DEFECT. On main the
// automasked adaptive stamp costs the same as the unmasked one — 144.65 against
// 153.23 — because `DynamicSculptor::gather` read none of `brush.automask` and
// the factor never ran. Paying nothing for work not done is not a baseline. The
// 3.80x and 8.08x are what an automask costs on a representation that has
// started honouring it, and the fixed column is the fair comparison for that
// number.
//
// THE PLAIN STAMP'S SCALING WITH THE SURFACE IS PRE-EXISTING AND WAS VERIFIED,
// not assumed: at an identical 114-entry workset, 10x the surface costs 7.96x
// on main and 8.22x here. Something in the fixed path is proportional to the
// model rather than to the footprint, it is older than this change, and it is
// not this change's to fix — but it is worth someone's afternoon, and these
// two cases are where it will show up next.

void mesh_stamp_automask(benchmark::State& state, std::uint32_t factors) {
    using namespace clay;
    using namespace clay::kernel;
    const int n = static_cast<int>(state.range(0));
    mesh::Mesh patch = bench_surface_patch(n, 0.02f);
    mesh::MeshSculptor sculptor(patch, 0.0f);

    mesh::MeshBrushSettings brush;
    brush.radius = 0.12f;  // a FIXED footprint, whatever the patch's size
    brush.strength = 0.05f;
    brush.geodesic = false;
    brush.direction = cf3(0.001f, 0.002f, 0.0f);
    brush.automask.factors = factors;
    brush.automask.boundary_rings = 2;

    // WARM. The arena takes its storage on the first stamp of a footprint and
    // keeps it, so charging that to the measurement would measure the warm-up.
    for (int i = 0; i < 8; ++i) {
        brush.center = cf3(0.02f * static_cast<float>(i % 5) - 0.04f, 0.0f, 0.0f);
        sculptor.stamp(mesh::MeshBrush::Grab, brush);
    }

    int i = 0;
    std::size_t moved = 0;
    for (auto _ : state) {
        brush.center = cf3(0.02f * static_cast<float>(i % 5) - 0.04f, 0.0f, 0.0f);
        moved = sculptor.stamp(mesh::MeshBrush::Grab, brush);
        ++i;
    }
    // A stamp that reached nothing would time a no-op, and the pair would
    // compare two no-ops and look wonderfully flat.
    state.counters["moved"] = static_cast<double>(moved);
    state.counters["workset"] = static_cast<double>(sculptor.workset().size());
    state.counters["arena_growths"] = static_cast<double>(sculptor.arena().growths());
    state.counters["arena_high_water"] =
        static_cast<double>(sculptor.arena().high_water_bytes());
}

void BM_MeshStampNoAutomask(benchmark::State& state) { mesh_stamp_automask(state, 0); }
BENCHMARK(BM_MeshStampNoAutomask)
    ->Arg(224)   // ~100k triangles
    ->Arg(707)   // ~1M
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

// BOUNDARY, and boundary alone, because it is the only one of the three
// input-free factors that makes the virtual do work on every entry: it asks
// `on_open_border` once per workset slot to seed the spread. NormalAngle reads
// the workset's own normals and never touches the topology at all, so a case
// built on it would report the arithmetic and none of the indirection.
void BM_MeshStampBoundaryAutomask(benchmark::State& state) {
    mesh_stamp_automask(state, static_cast<std::uint32_t>(clay::mesh::AutomaskFactor::Boundary));
}
BENCHMARK(BM_MeshStampBoundaryAutomask)
    ->Arg(224)
    ->Arg(707)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

// The same pair on the ADAPTIVE surface, where the topology object is a
// different implementation of the same two methods — `one_ring` through the
// workset's slot map, and `DynamicSurface::is_boundary_edge`. If the virtual
// were the cost, the two representations would pay it alike; if one of them
// pays much more, the cost is in that implementation rather than in the
// indirection, which is a different fix.
void dynamic_stamp_automask(benchmark::State& state, std::uint32_t factors) {
    using namespace clay;
    using namespace clay::kernel;
    const int n = static_cast<int>(state.range(0));
    auto surface = mesh::DynamicSurface::from_mesh(bench_surface_patch(n, 0.02f));
    if (!surface) {
        state.SkipWithError("could not build the surface");
        return;
    }
    mesh::DynamicSculptor sculptor(*surface);

    mesh::MeshBrushSettings brush;
    brush.radius = 0.12f;
    brush.strength = 0.05f;
    brush.geodesic = false;
    brush.direction = cf3(0.001f, 0.002f, 0.0f);
    brush.automask.factors = factors;
    brush.automask.boundary_rings = 2;
    mesh::DynamicTopologySettings topo;
    topo.enabled = false;

    for (int i = 0; i < 8; ++i) {
        brush.center = cf3(0.02f * static_cast<float>(i % 5) - 0.04f, 0.0f, 0.0f);
        sculptor.stamp(mesh::MeshBrush::Grab, brush, topo);
    }

    int i = 0;
    std::size_t moved = 0;
    for (auto _ : state) {
        brush.center = cf3(0.02f * static_cast<float>(i % 5) - 0.04f, 0.0f, 0.0f);
        moved = sculptor.stamp(mesh::MeshBrush::Grab, brush, topo).moved_vertices;
        ++i;
    }
    state.counters["moved"] = static_cast<double>(moved);
    state.counters["workset"] = static_cast<double>(sculptor.workset().size());
    state.counters["arena_growths"] = static_cast<double>(sculptor.arena().growths());
    state.counters["arena_high_water"] =
        static_cast<double>(sculptor.arena().high_water_bytes());
}

void BM_DynamicStampNoAutomask(benchmark::State& state) { dynamic_stamp_automask(state, 0); }
BENCHMARK(BM_DynamicStampNoAutomask)
    ->Arg(224)
    ->Arg(707)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

void BM_DynamicStampBoundaryAutomask(benchmark::State& state) {
    dynamic_stamp_automask(state,
                           static_cast<std::uint32_t>(clay::mesh::AutomaskFactor::Boundary));
}
BENCHMARK(BM_DynamicStampBoundaryAutomask)
    ->Arg(224)
    ->Arg(707)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);


// The two numbers `DetailField` is configured by, measured rather than asserted
// (add-mesh-multires, task 1.2).
//
// BM_MultiresDetailBlockSize sweeps the block size over a real brush footprint
// at a fine level and reports, through the counters, the BYTES each choice
// allocates to hold the same detail and the block-table bytes it pays for them.
// The trade is only visible side by side: a small block wastes little on the rim
// of a dab, a large one spends fewer table entries over the level, and the
// footprint decides where the crossover sits. Time is not the reading here —
// `allocated_KiB` and `table_KiB` are.
//
// BM_MultiresDetailAccess is the promotion threshold's evidence: the same
// coefficients read out of a sparse field and a dense one, over a stroke-sized
// index set. The block table costs four bytes per block, so the sparse form is
// smaller until coverage is almost total and memory NEVER argues for promoting;
// what argues for it is the indirection this pair prices.
namespace {

// The vertices one dab reaches at a fine level: a contiguous-ish run with the
// scatter a geodesic walk leaves, rather than a tidy block-aligned range that
// would flatter every block size equally.
std::vector<std::uint32_t> detail_footprint(std::uint32_t level_vertices, std::uint32_t reached) {
    std::vector<std::uint32_t> out;
    out.reserve(reached);
    std::uint32_t v = level_vertices / 3;
    for (std::uint32_t i = 0; i < reached; ++i) {
        out.push_back(v % level_vertices);
        v += 1 + (i % 5 == 0 ? 3 : 0);
    }
    return out;
}

}  // namespace

void BM_MultiresDetailBlockSize(benchmark::State& state) {
    const std::uint32_t block = static_cast<std::uint32_t>(state.range(0));
    const std::uint32_t vertices = 1u << 20;  // a level-4-sized surface
    // TWO FOOTPRINTS, because the crossover moves with the dab. A large block
    // amortises its table over a wide dab and wastes most of itself on a
    // narrow one, so a sweep over a single footprint picks whatever that
    // footprint happened to favour.
    const std::uint32_t reached = static_cast<std::uint32_t>(state.range(1));
    const std::vector<std::uint32_t> touched = detail_footprint(vertices, reached);
    mesh::DetailField field;
    for (auto _ : state) {
        field.reset(vertices, block);
        for (std::uint32_t v : touched) field.set(v, mesh::LocalDetail{0.0f, 0.0f, 0.01f});
        benchmark::DoNotOptimize(field);
    }
    const double allocated = static_cast<double>(field.resident_vertices() * sizeof(mesh::LocalDetail));
    const double table = static_cast<double>((vertices + block - 1) / block) * sizeof(std::uint32_t);
    state.counters["block"] = block;
    state.counters["reached"] = reached;
    state.counters["allocated_KiB"] = allocated / 1024.0;
    state.counters["table_KiB"] = table / 1024.0;
    state.counters["total_KiB"] = (allocated + table) / 1024.0;
    state.counters["bytes_per_touched"] =
        (allocated + table) / static_cast<double>(touched.size());
}
BENCHMARK(BM_MultiresDetailBlockSize)
    ->ArgsProduct({{64, 256, 1024, 4096}, {400, 4000, 40000}})
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

namespace {

void detail_access(benchmark::State& state, bool dense) {
    const std::uint32_t vertices = 1u << 18;
    mesh::DetailField field;
    field.reset(vertices);
    // COVERAGE IS COUNTED IN BLOCKS, NOT VERTICES, and getting that wrong is
    // how this pair first measured nothing: writing every OTHER vertex touches
    // every block, so the "sparse" side promoted and both rows reported the
    // dense field. The sparse side therefore fills a FRACTION OF THE BLOCKS.
    const std::uint32_t block = field.block_size();
    const std::uint32_t limit = dense ? vertices : (vertices / block) / 2 * block;
    for (std::uint32_t v = 0; v < limit; ++v)
        field.set(v, mesh::LocalDetail{0.0f, 0.0f, 0.01f});
    // Read only where the sparse field actually stores something, so the pair
    // prices the INDIRECTION rather than one side's early-out on a missing
    // block.
    const std::vector<std::uint32_t> touched = detail_footprint(limit, 20000);
    for (auto _ : state) {
        float sum = 0.0f;
        for (std::uint32_t v : touched) sum += field.get(v).normal;
        benchmark::DoNotOptimize(sum);
    }
    state.counters["dense"] = field.dense() ? 1 : 0;
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(touched.size()));
}

}  // namespace

void BM_MultiresDetailAccessSparse(benchmark::State& state) { detail_access(state, false); }
BENCHMARK(BM_MultiresDetailAccessSparse)->Unit(benchmark::kMicrosecond)->Iterations(2000);

void BM_MultiresDetailAccessDense(benchmark::State& state) { detail_access(state, true); }
BENCHMARK(BM_MultiresDetailAccessDense)->Unit(benchmark::kMicrosecond)->Iterations(2000);

// -- multiresolution (add-mesh-multires) --------------------------------------
//
// The pair that matters is COLD against LOCAL. Both reconstruct the same
// surface at the same display level; one rebuilds the hierarchy from the cage
// and the other propagates one dab's descendants. A hierarchy whose dab cost
// tracked its SIZE rather than the brush's reach would be correct and unusable,
// and the two names below are what make that visible — check_bench.py requires
// the local one to be the faster, which is exactly the propagation existing.
namespace {

mesh::Mesh multires_cage(int n) {
    mesh::Mesh m;
    const float step = 2.0f / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(kernel::cf3(-1.0f + step * static_cast<float>(x), 0.0f,
                                              -1.0f + step * static_cast<float>(z)));
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            m.quads.insert(m.quads.end(), {a, b, c, d});
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    return m;
}

mesh::MultiresSurface multires_fixture(int n, std::uint32_t levels) {
    auto surface = mesh::MultiresSurface::from_mesh(multires_cage(n));
    for (std::uint32_t i = 0; i < levels; ++i) surface->add_level();
    return std::move(*surface);
}

}  // namespace

// Adding a level: the subdivision itself, which is what `preflight_add_level`
// prices and what a host waits on when an artist presses Subdivide.
void BM_MultiresSubdivide(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        mesh::MultiresSurface s = multires_fixture(n, 2);
        state.ResumeTiming();
        s.add_level();
        std::uint32_t added = s.topology_at(s.max_level()).vertex_count;
        benchmark::DoNotOptimize(added);
    }
}
BENCHMARK(BM_MultiresSubdivide)->Arg(16)->Arg(32)->Unit(benchmark::kMillisecond)->Iterations(20);

// A COLD reconstruction of the whole hierarchy: every level rebuilt from the
// cage. The number the local path has to beat.
void BM_MultiresEvalCold(benchmark::State& state) {
    mesh::MultiresSurface s = multires_fixture(24, 3);
    for (auto _ : state) {
        s.drop_all_caches();
        std::size_t n = s.positions_at(3).size();
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_MultiresEvalCold)->Unit(benchmark::kMillisecond)->Iterations(20);

// ONE DAB at a coarse level, propagated to the same display level. The
// descendants of what it moved and nothing else.
void BM_MultiresDabLocal(benchmark::State& state) {
    mesh::MultiresSurface s = multires_fixture(24, 3);
    s.positions_at(3);
    s.set_sculpt_level(1);
    mesh::MultiresSculptor sculptor(s);
    mesh::MeshBrushSettings brush;
    brush.radius = 0.15f;
    brush.strength = 0.3f;
    int i = 0;
    for (auto _ : state) {
        brush.center = kernel::cf3(0.02f * static_cast<float>(i % 5), 0.0f, 0.0f);
        brush.strength = (i % 2) ? 0.3f : -0.3f;
        sculptor.stamp(mesh::MeshBrush::Draw, brush);
        std::size_t n = s.positions_at(3).size();
        benchmark::DoNotOptimize(n);
        ++i;
    }
}
BENCHMARK(BM_MultiresDabLocal)->Unit(benchmark::kMillisecond)->Iterations(200);

// A dab at the FINEST level, which is the interactive case a detail pass lives
// in: it writes coefficients and propagates to nothing.
void BM_MultiresDabFine(benchmark::State& state) {
    mesh::MultiresSurface s = multires_fixture(24, 3);
    s.positions_at(3);
    s.set_sculpt_level(3);
    mesh::MultiresSculptor sculptor(s);
    mesh::MeshBrushSettings brush;
    brush.radius = 0.05f;
    brush.strength = 0.3f;
    int i = 0;
    for (auto _ : state) {
        brush.center = kernel::cf3(0.01f * static_cast<float>(i % 5), 0.0f, 0.0f);
        brush.strength = (i % 2) ? 0.3f : -0.3f;
        sculptor.stamp(mesh::MeshBrush::Draw, brush);
        ++i;
    }
}
BENCHMARK(BM_MultiresDabFine)->Unit(benchmark::kMillisecond)->Iterations(400);

// Exporting a level as an ordinary mesh — what a host copies when it takes the
// whole display level rather than the changed blocks.
void BM_MultiresExportLevel(benchmark::State& state) {
    mesh::MultiresSurface s = multires_fixture(24, 3);
    s.positions_at(3);
    for (auto _ : state) {
        mesh::Mesh m = s.mesh_at_level(3);
        std::size_t tris = m.triangle_count();
        benchmark::DoNotOptimize(tris);
    }
}
BENCHMARK(BM_MultiresExportLevel)->Unit(benchmark::kMillisecond)->Iterations(20);

// -- sculpt layers (add-mesh-sculpt-layers) -----------------------------------
//
// THE STACK DEPTH IS THE VARIABLE, and the three shapes of coverage are what
// decide whether it matters. 1, 4, 16, 64 and 128 layers over the same
// hierarchy, each with:
//
//   local        every layer over the same small footprint. The deep-stack
//                worst case for one block, and the case a detail pass on one
//                cheek actually is.
//   overlapping  layers over footprints that slide across each other, so a
//                block holds some of the stack and not all of it.
//   dense        every layer over the whole level. What a host should be told
//                is expensive, because it is.
//
// WHAT THE NUMBERS ARE FOR. `BM_SculptLayerStrengthChange` is task 5.4's
// measurement: the cost of moving one slider must follow that layer's COVERAGE
// and not the level, so its time must be flat in the level's size for a local
// layer and must not grow with the layers that do not overlap it. The
// `blocks_recomposed` and `layer_blocks_visited` counters are the reading here
// rather than the clock — a correct implementation and a quadratic one produce
// the same surface, and only the counters tell them apart.
//
// `BM_SculptLayerStampOnStack` is task 5.5's: a stamp on top of a deep stack
// must not sum every layer beneath it over unrelated geometry, so its
// `layer_blocks_visited` must be bounded by the layers covering what the stamp
// touched. If these ever say prefix checkpoints are needed, the cache keys
// already admit one — a checkpoint is a synthetic layer over a contiguous
// prefix with its own composition revision.
namespace {

enum class LayerCoverage { Local, Overlapping, Dense };

// Fill `count` layers over one level in the named shape, and return the
// hierarchy holding them.
mesh::MultiresSurface layered_fixture(int n, std::uint32_t levels, std::uint32_t count,
                                      LayerCoverage coverage, std::uint32_t* out_level) {
    mesh::MultiresSurface s = multires_fixture(n, levels);
    const std::uint32_t level = levels;
    const std::uint32_t vertices = s.topology_at(level).vertex_count;
    *out_level = level;
    const std::uint32_t footprint = vertices / 64 + 1;
    for (std::uint32_t i = 0; i < count; ++i) {
        const mesh::SculptLayerId id = s.add_sculpt_layer();
        std::uint32_t begin = 0, end = 0;
        switch (coverage) {
            case LayerCoverage::Local:
                begin = vertices / 3;
                end = begin + footprint;
                break;
            case LayerCoverage::Overlapping:
                begin = (vertices / 3) + (i * footprint) / 4;
                end = begin + footprint;
                break;
            case LayerCoverage::Dense:
                begin = 0;
                end = vertices;
                break;
        }
        for (std::uint32_t v = begin; v < end && v < vertices; ++v)
            s.set_sculpt_layer_detail(id, level, v, mesh::LocalDetail{0.0f, 0.0f, 0.001f});
    }
    s.positions_at(level);
    return s;
}

void report_composition(benchmark::State& state, const mesh::MultiresSurface& s,
                        std::uint32_t layers) {
    state.counters["layers"] = static_cast<double>(layers);
    state.counters["blocks_recomposed"] =
        static_cast<double>(s.sculpt_layer_stats().blocks_recomposed);
    state.counters["layer_blocks_visited"] =
        static_cast<double>(s.sculpt_layer_stats().layer_blocks_visited);
    state.counters["layer_MiB"] =
        static_cast<double>(s.memory().sculpt_layers) / (1024.0 * 1024.0);
    state.counters["composed_MiB"] = static_cast<double>(s.memory().composed) / (1024.0 * 1024.0);
}

void sculpt_layer_compose(benchmark::State& state, LayerCoverage coverage) {
    const std::uint32_t layers = static_cast<std::uint32_t>(state.range(0));
    std::uint32_t level = 0;
    mesh::MultiresSurface s = layered_fixture(24, 3, layers, coverage, &level);
    for (auto _ : state) {
        // A COLD composition: the composed field is released with the rest of
        // the caches and rebuilt whole, which is the ceiling the incremental
        // paths below are measured against.
        s.drop_all_caches();
        std::size_t count = s.positions_at(level).size();
        benchmark::DoNotOptimize(count);
    }
    report_composition(state, s, layers);
}

void sculpt_layer_strength(benchmark::State& state, LayerCoverage coverage) {
    const std::uint32_t layers = static_cast<std::uint32_t>(state.range(0));
    std::uint32_t level = 0;
    mesh::MultiresSurface s = layered_fixture(24, 3, layers, coverage, &level);
    const mesh::SculptLayerId top = s.sculpt_layers().id_at(layers - 1);
    s.reset_sculpt_layer_stats();
    int i = 0;
    for (auto _ : state) {
        s.set_sculpt_layer_strength(top, (i % 2) ? 0.25f : 0.75f);
        std::size_t count = s.positions_at(level).size();
        benchmark::DoNotOptimize(count);
        ++i;
    }
    report_composition(state, s, layers);
}

void sculpt_layer_stamp(benchmark::State& state, LayerCoverage coverage) {
    const std::uint32_t layers = static_cast<std::uint32_t>(state.range(0));
    std::uint32_t level = 0;
    mesh::MultiresSurface s = layered_fixture(24, 3, layers, coverage, &level);
    s.set_sculpt_level(level);
    s.set_active_sculpt_layer(s.sculpt_layers().id_at(layers - 1));
    mesh::MultiresSculptor sculptor(s);
    mesh::MeshBrushSettings brush;
    brush.radius = 0.08f;
    brush.strength = 0.2f;
    s.reset_sculpt_layer_stats();
    int i = 0;
    for (auto _ : state) {
        brush.center = kernel::cf3(0.02f * static_cast<float>(i % 5), 0.0f, 0.0f);
        brush.strength = (i % 2) ? 0.2f : -0.2f;
        sculptor.stamp(mesh::MeshBrush::Draw, brush);
        std::size_t count = s.positions_at(level).size();
        benchmark::DoNotOptimize(count);
        ++i;
    }
    report_composition(state, s, layers);
}

}  // namespace

void BM_SculptLayerComposeLocal(benchmark::State& state) {
    sculpt_layer_compose(state, LayerCoverage::Local);
}
BENCHMARK(BM_SculptLayerComposeLocal)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(128)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

void BM_SculptLayerComposeOverlapping(benchmark::State& state) {
    sculpt_layer_compose(state, LayerCoverage::Overlapping);
}
BENCHMARK(BM_SculptLayerComposeOverlapping)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(128)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

void BM_SculptLayerComposeDense(benchmark::State& state) {
    sculpt_layer_compose(state, LayerCoverage::Dense);
}
BENCHMARK(BM_SculptLayerComposeDense)
    ->Arg(1)->Arg(4)->Arg(16)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(10);

void BM_SculptLayerStrengthChangeLocal(benchmark::State& state) {
    sculpt_layer_strength(state, LayerCoverage::Local);
}
BENCHMARK(BM_SculptLayerStrengthChangeLocal)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(128)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

void BM_SculptLayerStrengthChangeOverlapping(benchmark::State& state) {
    sculpt_layer_strength(state, LayerCoverage::Overlapping);
}
BENCHMARK(BM_SculptLayerStrengthChangeOverlapping)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(128)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

void BM_SculptLayerStrengthChangeDense(benchmark::State& state) {
    sculpt_layer_strength(state, LayerCoverage::Dense);
}
BENCHMARK(BM_SculptLayerStrengthChangeDense)
    ->Arg(1)->Arg(4)->Arg(16)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

void BM_SculptLayerStampOnStackLocal(benchmark::State& state) {
    sculpt_layer_stamp(state, LayerCoverage::Local);
}
BENCHMARK(BM_SculptLayerStampOnStackLocal)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(128)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

void BM_SculptLayerStampOnStackOverlapping(benchmark::State& state) {
    sculpt_layer_stamp(state, LayerCoverage::Overlapping);
}
BENCHMARK(BM_SculptLayerStampOnStackOverlapping)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(128)
    ->Unit(benchmark::kMicrosecond)
    ->Iterations(200);

void BM_SculptLayerStampOnStackDense(benchmark::State& state) {
    sculpt_layer_stamp(state, LayerCoverage::Dense);
}
BENCHMARK(BM_SculptLayerStampOnStackDense)
    ->Arg(1)->Arg(4)->Arg(16)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

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

#if defined(CLAY_HAS_METAL)
// -- a Metal stroke, patched against re-uploaded (#296) ----------------------
//
// The pair is the same stroke twice: append a dab, evaluate, repeat. One
// evaluates the tape the compiler produced, which names its ancestor and so is
// served by copying only the suffix; the other strips the lineage first, which
// is exactly what the backend saw before #296 and forces a fresh
// newBuffer(StorageModeShared) for all three sections of a tape that grew by
// one item.
//
// Stripping the lineage rather than comparing against a second backend keeps
// everything else identical — same document, same tapes, same dispatches, one
// field different — so the gap is the transfer and nothing else.
//
// READ THE REALLOCATION COUNTER, NOT ONLY THE TIME. On unified memory both
// rows evaluate the same field with the same dispatch, and a Mac has neither
// the memory pressure nor the power budget an iPad sculpts under, so the
// wall-clock gap here UNDER-reports what the change is worth on the hardware
// that ships. The allocator churn is exact and machine-independent.
void metal_stroke(benchmark::State& state, bool keep_lineage) {
    eval::Backend* mtl = eval::Registry::instance().find("metal");
    scene::Document doc = sculpted_sphere(20000);
    scene::Layer& layer = doc.layers.front();
    scene::TapeCheckpoint cp;
    scene::Tape tape = scene::compile_document_resumable(doc, &cp);

    eval::GridQuery q;
    q.origin = cf3(0.2f, -0.08f, -0.08f);
    q.spacing = 0.02f;
    q.nx = q.ny = q.nz = 8;
    std::vector<float> values(static_cast<std::size_t>(q.nx) * q.ny * q.nz);
    mtl->eval_grid(tape, q, values.data());  // the first upload is not the measurement

    const std::uint64_t uploads_before = eval::metal_tape_uploads(*mtl);
    for (auto _ : state) {
        // The APPEND IS NOT MEASURED. Phase 1 already gated what it costs, and
        // leaving it inside would put ~0.2 ms of compile in front of the
        // transfer this pair exists to compare.
        state.PauseTiming();
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.05f);
        dab.xform.position = cf3(0.0f, 1.0f, 0.0f);
        dab.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.03f};
        const scene::NodeId id = layer.sdf->insert(dab);
        scene::Tape grown;
        scene::TapeCheckpoint next;
        if (!scene::compile_document_append(tape, cp, doc, {id}, &grown, &next)) {
            state.SkipWithError("the append was refused; the fast path is not being measured");
            return;
        }
        tape = std::move(grown);
        cp = next;
        if (!keep_lineage) tape.parent_id = 0;  // what the backend saw before #296
        state.ResumeTiming();

        mtl->eval_grid(tape, q, values.data());
        benchmark::DoNotOptimize(values.data());
    }
    state.counters["instrs"] = static_cast<double>(tape.instrs.size());
    state.counters["tape_KiB"] =
        static_cast<double>(tape.instrs.size() * sizeof(kernel::CTapeInstr) +
                            (tape.params.size() + tape.blob.size()) * sizeof(float)) /
        1024.0;
    // The allocator churn, which is half of what #197 is about and the half a
    // wall-clock number on unified memory reports least honestly.
    state.counters["repacks"] =
        static_cast<double>(eval::metal_tape_uploads(*mtl) - uploads_before);
}
#endif

#if defined(CLAY_HAS_VULKAN)
// -- a Vulkan stroke, patched against re-uploaded (#197 phase 2) -------------
//
// The pair is the same stroke twice: append a dab, evaluate, repeat. One
// evaluates the tape the compiler produced, which names its ancestor and so
// is served by transferring only the suffix; the other strips the lineage
// first, which is exactly what the backend saw before this change and forces
// a whole re-upload of a tape that grew by one item.
//
// Stripping the lineage rather than comparing against a second backend keeps
// everything else identical — same document, same tapes, same dispatches, one
// field different — so the gap is the transfer and nothing else.
void vulkan_stroke(benchmark::State& state, bool keep_lineage) {
    eval::Backend* vk = eval::Registry::instance().find("vulkan");
    scene::Document doc = sculpted_sphere(20000);
    scene::Layer& layer = doc.layers.front();
    scene::TapeCheckpoint cp;
    scene::Tape tape = scene::compile_document_resumable(doc, &cp);

    eval::GridQuery q;
    q.origin = cf3(0.2f, -0.08f, -0.08f);
    q.spacing = 0.02f;
    q.nx = q.ny = q.nz = 8;
    std::vector<float> values(static_cast<std::size_t>(q.nx) * q.ny * q.nz);
    vk->eval_grid(tape, q, values.data());  // the first upload is not the measurement

    const std::uint64_t uploads_before = eval::vulkan_tape_uploads(*vk);
    for (auto _ : state) {
        // The APPEND IS NOT MEASURED. Phase 1 already gated what it costs, and
        // leaving it inside would put ~0.2 ms of compile in front of the
        // transfer this pair exists to compare.
        state.PauseTiming();
        scene::Node dab;
        dab.prim = scene::Prim::sphere(0.05f);
        dab.xform.position = cf3(0.0f, 1.0f, 0.0f);
        dab.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.03f};
        const scene::NodeId id = layer.sdf->insert(dab);
        scene::Tape grown;
        scene::TapeCheckpoint next;
        if (!scene::compile_document_append(tape, cp, doc, {id}, &grown, &next)) {
            state.SkipWithError("the append was refused; the fast path is not being measured");
            return;
        }
        tape = std::move(grown);
        cp = next;
        if (!keep_lineage) tape.parent_id = 0;  // what the backend saw before #294
        state.ResumeTiming();

        vk->eval_grid(tape, q, values.data());
        benchmark::DoNotOptimize(values.data());
    }
    state.counters["instrs"] = static_cast<double>(tape.instrs.size());
    state.counters["tape_KiB"] =
        static_cast<double>(tape.instrs.size() * sizeof(kernel::CTapeInstr) +
                            (tape.params.size() + tape.blob.size()) * sizeof(float)) /
        1024.0;
    // The allocator churn, which is half of what #197 is about and which no
    // wall-clock number on a desktop with 8 GB of VRAM reports honestly.
    state.counters["repacks"] =
        static_cast<double>(eval::vulkan_tape_uploads(*vk) - uploads_before);
}

// Called from main, not from a static initializer: probing the registry
// spins up backend runtimes (a Metal device), which has no business running
// before main.
void register_vulkan_benches() {
    if (!eval::Registry::instance().find("vulkan")) return;
    // FIXED ITERATION COUNT, deliberately. A stroke grows the document as it
    // runs, so letting the harness pick iterations to fill a time budget
    // would hand the faster row MORE dabs and a bigger document to be fast
    // on — the two rows would stop measuring the same stroke. Measured that
    // way once: 8 154 iterations and the document went from 2 000 items to
    // 10 153.
    benchmark::RegisterBenchmark("BM_VulkanStrokePatched",
                                 [](benchmark::State& s) { vulkan_stroke(s, true); })
        ->Unit(benchmark::kMillisecond)
        ->Iterations(300);
    benchmark::RegisterBenchmark("BM_VulkanStrokeReupload",
                                 [](benchmark::State& s) { vulkan_stroke(s, false); })
        ->Unit(benchmark::kMillisecond)
        ->Iterations(300);
}

#else
void register_vulkan_benches() {}
#endif

void register_metal_benches() {
    if (!eval::Registry::instance().find("metal")) return;
    benchmark::RegisterBenchmark("BM_MetalTapeResident",
                                 [](benchmark::State& s) { metal_consolidated_eval(s, 1); })
        ->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("BM_MetalTapeReupload",
                                 [](benchmark::State& s) { metal_consolidated_eval(s, 6); })
        ->Unit(benchmark::kMillisecond);
#if defined(CLAY_HAS_METAL)
    // FIXED ITERATION COUNT, deliberately. A stroke grows the document as it
    // runs, so letting the harness pick iterations to fill a time budget would
    // hand the faster row MORE dabs and a bigger document to be fast on — the
    // two rows would stop measuring the same stroke.
    benchmark::RegisterBenchmark("BM_MetalStrokePatched",
                                 [](benchmark::State& s) { metal_stroke(s, true); })
        ->Unit(benchmark::kMillisecond)
        ->Iterations(300);
    benchmark::RegisterBenchmark("BM_MetalStrokeReupload",
                                 [](benchmark::State& s) { metal_stroke(s, false); })
        ->Unit(benchmark::kMillisecond)
        ->Iterations(300);
#endif
}

}  // namespace

}  // namespace

// BENCHMARK_MAIN(), plus the conditionally registered Metal and Vulkan pairs.
int main(int argc, char** argv) {
    register_metal_benches();
    register_vulkan_benches();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
