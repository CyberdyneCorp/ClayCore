#include "brick_recording.h"
#include "clay/eval/backend.h"
#include "clay/scene/tape.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0}, bytes{0};
using namespace clay;
std::FILE *binary = nullptr;
bool save = false;
namespace {
scene::Document fixture(int kind) {
  scene::Document doc;
  auto &layer = doc.add_sdf_layer("recording probe");
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

bool fill(brick::BrickCache &cache, const scene::Document &doc) {
  auto *cpu = eval::Registry::instance().find("cpu");
  if (!cpu)
    return false;
  const auto tape = scene::compile_document(doc);
  cache.mark_dirty(
      {kernel::cf3(-1.3f, -1.3f, -1.3f), kernel::cf3(1.3f, 1.3f, 1.3f)});
  for (const auto &request : cache.take_dirty()) {
    std::vector<float> values(cache.config().sample_count());
    if (cpu->eval_grid(tape, request.grid, values.data()) != eval::Status::Ok)
      return false;
    cache.submit(request, values.data());
  }
  return true;
}

template <class T> void hash(std::uint64_t &h, const std::vector<T> &v) {
  if (save) {
    std::uint64_t n = v.size() * sizeof(T);
    std::fwrite(&n, sizeof(n), 1, binary);
    if (n)
      std::fwrite(v.data(), 1, n, binary);
  }
  const auto *p = reinterpret_cast<const unsigned char *>(v.data());
  for (std::size_t i = 0; i < v.size() * sizeof(T); ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
}
void run_case(int kind, const brick::BrickCache &cache,
              const scene::Document &doc,
              const std::vector<brick::BrickKey> &keys, bool attr) {
  mesh::MeshingOptions options{
      attr ? mesh::NormalMode::Gradient : mesh::NormalMode::None, attr, 1e-4f};
  for (int repeat = -1; repeat < 3; ++repeat) {
    std::vector<mesh::BrickMeshRange> ranges;
    allocations.store(0);
    bytes.store(0);
    counting.store(true);
    auto t = std::chrono::steady_clock::now();
    auto m = mesh::mesh_bricks(cache, &doc, options, &keys, &ranges);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t)
                    .count();
    counting.store(false);
    save = repeat == 0;
    std::uint64_t h = 14695981039346656037ull;
    hash(h, m.positions);
    hash(h, m.indices);
    hash(h, m.normals);
    hash(h, m.colors);
    hash(h, ranges);
    if (repeat >= 0)
      std::printf("%d,%zu,%d,%d,%.6f,%zu,%zu,%zu,%llu\n", kind, keys.size(),
                  int(attr), repeat, ms, allocations.load(), bytes.load(),
                  m.positions.size(), static_cast<unsigned long long>(h));
  }
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 3;
  binary = std::fopen(argv[1], "wb");
  if (!binary)
    return 4;
  std::puts("kind,keys,attributes,repeat,ms,allocations,bytes,vertices,hash");
  for (int kind : {0, 1, 2}) {
    auto doc = fixture(kind);
    brick::BrickCache cache(brick::BrickConfig{8, 0.02f, 3, 0});
    if (!fill(cache, doc))
      return 2;
    auto all = cache.surface_bricks();
    for (auto n : {all.size(), std::size_t(48)})
      for (bool attr : {false, true}) {
        std::vector<brick::BrickKey> keys(
            all.begin(), all.begin() + std::min(n, all.size()));
        run_case(kind, cache, doc, keys, attr);
      }
  }
  const bool failed = std::ferror(binary) != 0;
  return std::fclose(binary) != 0 || failed ? 5 : 0;
}
