// Performance benchmarks with CI regression gates (build-packaging spec):
// points/sec, bricks/sec, mesh time on fixed scenes. tools/check_bench.py
// enforces generous floor thresholds in CI — they catch order-of-magnitude
// regressions; tight deltas need dedicated hardware.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/eval/backend.h"
#include "clay/mesh/marching.h"
#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"

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

void BM_MeshTape(benchmark::State& state) {
    scene::Document doc = bench_document();
    scene::Tape tape = scene::compile_document(doc);
    for (auto _ : state) {
        mesh::Mesh m = mesh::mesh_tape(tape, tape.bounds, 0.02f);
        benchmark::DoNotOptimize(m.triangle_count());
    }
}
BENCHMARK(BM_MeshTape)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
