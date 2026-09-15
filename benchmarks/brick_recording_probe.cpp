// Exact-output and timing comparison for #531's local edge recording.
// Timings are informational: both paths run in one process, in alternating order.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/mesh/brick_recording.h"
#include "clay/eval/backend.h"
#include "clay/scene/tape.h"

using namespace clay;
namespace {
using Clock = std::chrono::steady_clock;

scene::Document fixture(int kind) {
    scene::Document doc;
    auto& layer = doc.add_sdf_layer("recording probe");
    scene::Node node;
    node.prim = kind == 1 ? scene::Prim::box(kernel::cf3(0.75f, 0.6f, 0.25f))
                          : scene::Prim::sphere(1.0f);
    node.color = kernel::cf3(0.2f, 0.6f, 0.9f);
    if (kind == 2)
        for (int i = 0; i < 48; ++i)
            node.deformers.push_back(scene::Deformer::grab(
                kernel::cf3(0.01f * static_cast<float>(i), 0, 0.9f), 0.35f,
                kernel::cf3(0, 0, 0.004f)));
    layer.sdf->insert(node);
    return doc;
}

bool fill(brick::BrickCache& cache, const scene::Document& doc) {
    auto* cpu = eval::Registry::instance().find("cpu");
    if (!cpu) return false;
    const auto tape = scene::compile_document(doc);
    cache.mark_dirty({kernel::cf3(-1.3f, -1.3f, -1.3f), kernel::cf3(1.3f, 1.3f, 1.3f)});
    for (const auto& request : cache.take_dirty()) {
        std::vector<float> values(cache.config().sample_count());
        if (cpu->eval_grid(tape, request.grid, values.data()) != eval::Status::Ok) return false;
        cache.submit(request, values.data());
    }
    return true;
}

template <class T>
bool same(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool measure(const brick::BrickCache& cache, const scene::Document& doc,
             const std::vector<brick::BrickKey>& keys, bool attributes, int kind) {
    const mesh::MeshingOptions options{attributes ? mesh::NormalMode::Gradient : mesh::NormalMode::None,
                                       attributes, 1e-4f};
    std::vector<mesh::BrickMeshRange> ranges[2];
    auto run = [&](bool local, std::vector<mesh::BrickMeshRange>* output) {
        return mesh::detail::mesh_bricks_recorded(cache, &doc, options, &keys, output,
                                                  nullptr, 0, local);
    };
    const auto reference = run(false, &ranges[0]);
    const auto local = run(true, &ranges[1]);
    if (!same(reference.positions, local.positions) || !same(reference.indices, local.indices) ||
        !same(reference.normals, local.normals) || !same(reference.colors, local.colors) ||
        !same(ranges[0], ranges[1])) return false;
    std::vector<double> elapsed[2];
    for (int repeat = 0; repeat < 5; ++repeat)
        for (int order = 0; order < 2; ++order) {
            const int path = (order + repeat) % 2;
            const auto start = Clock::now();
            const auto result = run(path != 0, nullptr);
            elapsed[path].push_back(std::chrono::duration<double, std::milli>(Clock::now()-start).count());
            if (result.positions.size() != reference.positions.size()) return false;
        }
    for (auto& values : elapsed) std::sort(values.begin(), values.end());
    std::printf("%d %d %zu %d %zu %.6f %.6f %.3f\n", kind, cache.config().dim, keys.size(),
                attributes ? 1 : 0, reference.positions.size(), elapsed[0][2], elapsed[1][2],
                elapsed[0][2] / std::max(elapsed[1][2], 1e-9));
    return true;
}

bool run_fixture(int kind, int dim) {
    const auto doc = fixture(kind);
    brick::BrickCache cache(brick::BrickConfig{dim, 0.02f, 3, 0});
    if (!fill(cache, doc)) return false;
    auto keys = cache.surface_bricks();
    std::vector<brick::BrickKey> one;
    for (auto key : keys) {
        one = {key};
        if (!mesh::mesh_bricks(cache, nullptr, {mesh::NormalMode::None, false}, &one).empty())
            break;
        one.clear();
    }
    if (one.empty()) return false;
    for (auto count : {keys.size(), std::size_t(48), std::size_t(1), std::size_t(0)}) {
        const std::vector<brick::BrickKey> subset = count == 1 ? one :
            std::vector<brick::BrickKey>(keys.begin(), keys.begin() + std::min(count, keys.size()));
        for (bool attributes : {false, true})
            if (!measure(cache, doc, subset, attributes, kind)) return false;
    }
    return true;
}
}  // namespace

int main() {
    std::puts("fixture dim keys attributes vertices reference_ms local_ms speedup");
    for (int kind : {0, 1, 2})
        for (int dim : {8, 16, 32})
            if (!run_fixture(kind, dim)) {
                std::fputs("fixture or exact-output comparison failed\n", stderr);
                return 1;
            }
}
