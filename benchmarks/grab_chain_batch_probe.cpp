// Exact grab-chain throughput, separated from meshing and thread-pool noise.
// Link this unchanged source against main and the changed library to compare
// the same point/gradient work. Scalar bit identity is the correctness gate;
// timings are measurements, not portable pass/fail thresholds (#531).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/field/volume.h"
#include "clay/scene/document.h"

using namespace clay;
using kernel::cf3;

namespace {
scene::Tape fixture(const std::string& shape, int depth) {
    scene::Document doc;
    auto& layer = doc.add_sdf_layer("grab throughput");
    scene::Node n;
    n.prim = shape == "hard" ? scene::Prim::box(cf3(0.5f, 0.6f, 0.8f)) : scene::Prim::sphere(0.8f);
    if (shape == "volume") {
        n.prim = scene::Prim::volume();
        n.volume = std::make_shared<const field::FieldVolume>(field::FieldVolume::sample_colored(
            [](kernel::cfloat3 p) { return kernel::clength(p) - 0.8f; },
            [](kernel::cfloat3 p) { return cf3(0.5f + p.x * 0.2f, 0.3f, 0.7f); },
            math::Aabb{cf3(-1.2f, -1.2f, -1.2f), cf3(1.2f, 1.2f, 1.2f)}, 0.1f, 0.3f));
    }
    for (int i = 0; i < depth; ++i)
        n.deformers.push_back(scene::Deformer::grab(
            cf3(0.15f * std::cos(float(i)), 0.12f * std::sin(float(i)), 0.8f),
            0.35f, cf3(0.001f, -0.002f, 0.012f), 0, true));
    if (shape == "mixed") n.deformers.push_back(scene::Deformer::twist(0.2f));
    layer.sdf->insert(n);
    if (shape == "hard") {
        scene::Node plate;
        plate.prim = scene::Prim::box(cf3(0.65f, 0.04f, 0.85f));
        plate.color = cf3(0.2f, 0.7f, 0.3f);
        layer.sdf->insert(plate);
        scene::Node ball;
        ball.prim = scene::Prim::sphere(0.18f);
        ball.xform.position = cf3(0.5f, 0.2f, 0.75f);
        layer.sdf->insert(ball);
    }
    return scene::compile_document(doc);
}

std::vector<float> points(std::size_t count) {
    std::vector<float> xyz(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        xyz[i * 3] = (static_cast<int>(i % 8) - 4) * 0.055f;
        xyz[i * 3 + 1] = (static_cast<int>(i / 8 % 8) - 4) * 0.055f;
        xyz[i * 3 + 2] = 0.72f + static_cast<float>(i / 64) * 0.02f;
    }
    return xyz;
}

bool row(const std::string& shape, int depth, std::size_t count) {
    const auto tape = fixture(shape, depth);
    const auto xyz = points(count);
    const eval::PointQuery query{xyz.data(), count, 1e-4f};
    // Each point has one distance, three gradient components and three colours.
    std::vector<float> reference(count * 7), result(count * 7);
    const eval::PointResults expected{reference.data(), reference.data() + count,
                                      reference.data() + count * 4};
    const eval::PointResults output{result.data(), result.data() + count, result.data() + count * 4};
    eval::eval_points_reference(tape, query, expected);
    eval::eval_points_blocked(tape, query, output, count);
    if (std::memcmp(reference.data(), result.data(), reference.size() * sizeof(float)) != 0) {
        std::printf("FAIL: %s depth %d count %zu differs from the scalar field\n", shape.c_str(), depth, count);
        return false;
    }
    const int repeats = std::max(100, 8192 / static_cast<int>(count));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repeats; ++i) eval::eval_points_blocked(tape, query, output, count);
    const double micros = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count() / repeats;
    std::printf("%-6s %2d %3zu %10.3f us/batch\n", shape.c_str(), depth, count, micros);
    return true;
}
}  // namespace

int main() {
    std::printf("shape depth points: distance + exact field gradient + colour; one CPU thread\n");
    for (const std::string shape : {"sphere", "hard", "volume", "mixed"})
        for (int depth : {0, 1, 12, 48})
            for (std::size_t count : {std::size_t(1), std::size_t(4), std::size_t(8),
                                      std::size_t(16), std::size_t(64), std::size_t(512)})
                if (!row(shape, depth, count)) return 1;
    return 0;
}
