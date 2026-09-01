// THE CHUNK-SIZE MEASUREMENT (add-extreme-poly-runtime task 1.1, design.md D2).
//
// The task says in terms not to adopt a number from prior art, and the design
// fixes the experiment and the rule for reading it BEFORE the data exists so
// that the number cannot be rationalised afterwards:
//
//   > Minimise the P95 of (chunk query + gather + normal recompute + index
//   > update) at the 20k footprint, subject to false-positive touched vertices
//   > at or under 2x the exact footprint and upload bytes per stamp at or under
//   > 3x the bytes actually moved. Ties, and differences inside the run-to-run
//   > spread, break toward the SMALLER chunk, because mutation cost grows
//   > superlinearly in chunk size and the false-positive term grows linearly.
//
// The null hypothesis to beat is 256 target / 64 min / 512 max —
// `DynamicBvhOptions`'s current defaults, which are prior art in the strict
// sense: they are in this tree and they were never measured here.
//
// THE SIX QUANTITIES, all named by the task and each measured where it lives:
//
//   1. CHUNK QUERY        time to find the chunks a brush ball reaches.
//   2. FALSE POSITIVES    vertices admitted by the chunks minus the vertices
//                         actually inside the ball. The term that grows
//                         linearly in chunk size and is the reason a large
//                         chunk is not free.
//   3. NORMAL RECOMPUTE   time to rewrite the normals of everything a chunk
//                         admitted, which is what a host actually pays for a
//                         false positive.
//   4. UPLOAD BYTES       what the transport hands a host per stamp, against
//                         the bytes that actually moved.
//   5. LOCALITY PROXY     bytes of position data resident per chunk, against
//                         the L2 working set. A chunk whose vertices do not fit
//                         in cache costs its query twice.
//   6. MUTATION COST      split and merge, on the adaptive surface, which is
//                         the term that punishes a LARGE chunk and the reason
//                         the tie-break points down.
//
// THE FIXTURE IS FIXED-SPACING WITH A GROWING EXTENT. A footprint is a world
// radius chosen so that pi*r^2/spacing^2 is the wanted vertex count, and the
// model is always wide enough that the largest footprint is interior — a dab
// clipped by the model boundary measures the boundary.
//
// PERCENTILES AND NOT AVERAGES. An average over a stroke hides the dab that
// dropped the frame, and the decision rule is written on the P95 for exactly
// that reason.
//
// NOT A GATED BENCHMARK. It exits non-zero only if the partition it measures
// stops being a partition — every face in exactly one chunk — because a
// measurement of a broken partition is worse than no measurement.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/surface_view.h"

#include "chunk_tree.h"

using namespace clay;
using namespace clay::kernel;
using mesh::ChunkOptions;
using mesh::ChunkTable;
using mesh::Mesh;

namespace {

constexpr float kSpacing = 0.01f;
// The five sizes the task names.
const std::size_t kTargets[] = {64, 128, 256, 512, 1024};
// The five footprints, in vertices.
const std::size_t kFootprints[] = {1000, 5000, 20000, 100000, 500000};
// The rule reads the 20k row.
constexpr std::size_t kDecisionFootprint = 20000;

double now_micros() {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Stats {
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
};

Stats summarise(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        const std::size_t i = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
        return v[i];
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = v.back();
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    return s;
}

// A plane at fixed spacing, measured outward from the centre so the vertex at
// the origin is the same float at every size.
Mesh plane(int n, float spacing) {
    Mesh m;
    const int centre = n / 2;
    const std::size_t side = static_cast<std::size_t>(n) + 1;
    m.positions.reserve(side * side);
    m.normals.reserve(side * side);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(spacing * static_cast<float>(x - centre),
                                      ((x + z) & 1) ? spacing * 0.5f : 0.0f,
                                      spacing * static_cast<float>(z - centre)));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    m.indices.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * 6);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

float radius_for(std::size_t vertices, float spacing) {
    return spacing * std::sqrt(static_cast<float>(vertices) / 3.14159265f);
}

math::Aabb ball_box(cfloat3 c, float r) {
    return math::Aabb{cf3(c.x - r, c.y - r, c.z - r), cf3(c.x + r, c.y + r, c.z + r)};
}


// -- one row of the matrix ---------------------------------------------------------

struct Row {
    std::size_t target = 0;
    std::size_t footprint = 0;
    std::size_t exact_vertices = 0;
    std::size_t admitted_vertices = 0;
    std::size_t chunks_admitted = 0;
    std::size_t upload_bytes = 0;   // positions of the dirty chunks
    std::size_t index_bytes = 0;    // and their indices, which a stable-topology stamp skips
    std::size_t moved_bytes = 0;
    Stats query;
    Stats normals;
    Stats index_update;
    double decision_p95 = 0;  // query + normals + index update
};

// Where a stamp of this footprint is centred. Several places, so a chunk
// boundary that happens to fall on the origin does not decide the number.
const cfloat3 kCentres[] = {cf3(0, 0, 0), cf3(0.137f, 0, -0.211f), cf3(-0.301f, 0, 0.089f),
                            cf3(0.052f, 0, 0.463f)};

Row measure_fixed(const Mesh& mesh, const ChunkTable& table, const mesh::SurfaceView& view,
                  const bench::ChunkTree& tree, std::size_t target, std::size_t footprint,
                  int repetitions) {
    Row row;
    row.target = target;
    row.footprint = footprint;
    const float radius = radius_for(footprint, kSpacing);

    std::vector<double> query, normals, index_update;
    std::vector<std::uint32_t> admitted;
    // The dedup mark and its reset list, sized once and reset over what was
    // touched — the same discipline the sculptor's own workset keeps.
    std::vector<std::uint8_t> seen(mesh.positions.size(), 0);
    std::vector<std::uint32_t> touched;
    // Somewhere for the recomputed normals to go that the optimiser cannot
    // delete. Without it the whole loop above is dead code and the stage reads
    // as free.
    cfloat3 sink = cf3(0, 0, 0);

    for (int rep = 0; rep < repetitions; ++rep) {
        const cfloat3 centre = kCentres[static_cast<std::size_t>(rep) %
                                        (sizeof(kCentres) / sizeof(kCentres[0]))];
        const math::Aabb box = ball_box(centre, radius);

        // 1. CHUNK QUERY, through a tree over the chunk bounds.
        const double t0 = now_micros();
        tree.query(box, &admitted);
        const double t1 = now_micros();
        query.push_back(t1 - t0);

        // 2. FALSE POSITIVES, and 3. NORMAL RECOMPUTE over what was admitted.
        //
        // DEDUPLICATED, and the first version of this was not. A vertex on a
        // chunk boundary is in the vertex list of every chunk that uses it, so
        // summing the lists counts it once per chunk — and a small chunk has
        // more boundary per face, so the double counting grows as the chunk
        // shrinks. It reported the 64-face partition admitting 43958 vertices
        // against the 1024-face partition's 36720, which is backwards: a finer
        // partition tiles a ball more tightly and must admit FEWER. A host
        // recomputes each vertex once, so the measurement has to as well.
        std::size_t admitted_vertices = 0, exact = 0, upload = 0, indices_bytes = 0;
        const float r2 = radius * radius;
        const double t2 = now_micros();
        for (std::uint32_t id : admitted) {
            const mesh::ChunkVertexSpan vertices = table.vertices(id);
            for (std::size_t k = 0; k < vertices.size(); ++k) {
                const std::uint32_t v = vertices[k];
                if (seen[v]) continue;
                seen[v] = 1;
                touched.push_back(v);
                ++admitted_vertices;
                if (kernel::cdot2(mesh.positions[v] - centre) <= r2) ++exact;
                // The shape of a normal recompute: read the position, sum a
                // few neighbours' cross products, normalise. Not the library's
                // own — this is about how MANY vertices a chunk size makes a
                // host touch, not about how fast one vertex is.
                const cfloat3 p = mesh.positions[v];
                cfloat3 sum = cf3(0, 0, 0);
                for (int j = 1; j <= 4; ++j) {
                    const std::size_t o =
                        (v + static_cast<std::uint32_t>(j)) % mesh.positions.size();
                    sum = sum + kernel::ccross(mesh.positions[o] - p, cf3(0, 1, 0));
                }
                const float len = clength(sum);
                sink = sink + (len > 0.0f ? sum / len : cf3(0, 1, 0));
            }
        }
        const double t3 = now_micros();
        normals.push_back(t3 - t2);
        for (std::uint32_t v : touched) seen[v] = 0;
        touched.clear();

        // 4. UPLOAD BYTES, through the transport a host actually uses.
        //
        // POSITIONS ONLY, and that is what requirement 6.3 is for rather than a
        // convenience of the harness: a stamp with stable topology advances the
        // GEOMETRY revision and leaves the TOPOLOGY revision alone, so a host
        // reading the four revisions re-uploads the vertex buffer and not the
        // index buffer. Counting the indices here would put a constant ~2x on
        // every row — a triangle mesh has about twice as many index bytes as
        // position bytes — and the decision rule would then be reading the mesh
        // rather than the chunk size. The index bytes are reported beside it.
        for (std::uint32_t id : admitted) {
            const mesh::ChunkReadback sized =
                view.copy_chunk(id, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
            upload += static_cast<std::size_t>(sized.vertex_count) * 3u * sizeof(float);
            indices_bytes += static_cast<std::size_t>(sized.index_count) * sizeof(std::uint32_t);
        }

        // 5. INDEX UPDATE: marking what the stamp touched, which is what a
        // stamp pays the table on every dab.
        ChunkTable& mutable_table = const_cast<ChunkTable&>(table);
        const double t4 = now_micros();
        for (std::uint32_t id : admitted) mutable_table.mark(id, mesh::ChunkDirty::Geometry);
        const double t5 = now_micros();
        index_update.push_back(t5 - t4);
        mutable_table.clear_dirty();

        if (rep == 0) {
            row.admitted_vertices = admitted_vertices;
            row.exact_vertices = exact;
            row.chunks_admitted = admitted.size();
            row.upload_bytes = upload;
            row.index_bytes = indices_bytes;
            row.moved_bytes = exact * 3u * sizeof(float);
        }
    }

    if (clength(sink) < -1.0f) std::printf("unreachable\n");  // keep the recompute alive
    row.query = summarise(query);
    row.normals = summarise(normals);
    row.index_update = summarise(index_update);
    row.decision_p95 = row.query.p95 + row.normals.p95 + row.index_update.p95;
    return row;
}

// -- 6. mutation cost, on the adaptive surface --------------------------------------

struct Mutation {
    std::size_t target = 0;
    Stats stamp;
    std::size_t splits = 0;
    std::size_t chunks = 0;
};

Mutation measure_mutation(std::size_t target, int stamps) {
    Mutation out;
    out.target = target;
    Mesh source = plane(220, 0.01f);
    auto surface = mesh::DynamicSurface::from_mesh(source);
    if (!surface.has_value()) return out;

    mesh::DynamicSculptOptions options;
    options.index.target_leaf_faces = target;
    options.index.max_leaf_faces = target * 2;
    options.index.min_leaf_faces = target / 4 + 1;
    mesh::DynamicSculptor sculptor(*surface, options);

    mesh::MeshBrushSettings brush;
    brush.radius = 0.08f;
    brush.strength = 0.4f;
    mesh::DynamicTopologySettings topology;
    topology.enabled = true;
    topology.detail_mode = mesh::DynamicDetailMode::BrushRelative;

    std::vector<double> times;
    for (int i = 0; i < stamps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(stamps);
        brush.center = cf3(-0.4f + 0.8f * t, 0.0f, 0.3f * std::sin(6.2f * t));
        const double a = now_micros();
        const mesh::DynamicStampResult r = sculptor.stamp(mesh::MeshBrush::Draw, brush, topology);
        const double b = now_micros();
        // The first few stamps warm the pools; they are a different measurement.
        if (i >= stamps / 4) times.push_back(b - a);
        out.splits += r.remesh.total();
    }
    out.stamp = summarise(times);
    out.chunks = sculptor.bvh().chunks().live_count();
    return out;
}

// -- the achieved chunk size, which the target does not promise ---------------------
//
// design.md names this as a risk in its own words: the multires chunk depth is
// derived from the target, and a level whose patch count is small produces
// chunks far under target. A decision rule that reads a number no chunk has is
// not reading anything.

void report_multires_distribution(std::size_t target) {
    Mesh cage;
    const int n = 8;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            cage.positions.push_back(cf3(0.25f * static_cast<float>(x - n / 2), 0.0f,
                                         0.25f * static_cast<float>(z - n / 2)));
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            cage.quads.push_back(at(x, z));
            cage.quads.push_back(at(x + 1, z));
            cage.quads.push_back(at(x + 1, z + 1));
            cage.quads.push_back(at(x, z + 1));
            cage.indices.insert(cage.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1),
                                                     at(x, z), at(x + 1, z + 1), at(x, z + 1)});
        }
    auto surface = mesh::MultiresSurface::from_mesh(cage);
    if (!surface.has_value()) return;
    mesh::MultiresError error = mesh::MultiresError::None;
    for (int i = 0; i < 4; ++i)
        if (!surface->add_level(&error)) {
            std::printf("  (stopped adding levels at %d: %s)\n", i,
                        mesh::multires_error_text(error));
            break;
        }

    for (std::uint32_t level = 0; level <= surface->max_level(); ++level) {
        const ChunkTable& table = surface->chunks_at(level);
        std::vector<std::size_t> sizes;
        for (std::uint32_t i = 0; i < table.slot_count(); ++i)
            if (const mesh::SurfaceChunk* c = table.chunk(i)) sizes.push_back(c->faces.size());
        if (sizes.empty()) continue;
        std::sort(sizes.begin(), sizes.end());
        std::size_t sum = 0;
        for (std::size_t s : sizes) sum += s;
        std::printf("  level %u  chunks %5zu  faces/chunk min %4zu  p50 %4zu  max %4zu  "
                    "mean %6.1f  (target %zu)\n",
                    level, sizes.size(), sizes.front(), sizes[sizes.size() / 2], sizes.back(),
                    static_cast<double>(sum) / static_cast<double>(sizes.size()), target);
    }
}

bool partition_is_a_partition(const Mesh& mesh, const ChunkTable& table) {
    std::vector<int> seen(mesh.indices.size() / 3, 0);
    for (std::uint32_t i = 0; i < table.slot_count(); ++i) {
        const mesh::SurfaceChunk* c = table.chunk(i);
        if (c == nullptr) continue;
        for (mesh::FaceId f : c->faces) {
            if (f.slot >= seen.size()) return false;
            if (++seen[f.slot] > 1) return false;
        }
    }
    for (int n : seen)
        if (n != 1) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // The model has to be wide enough that the 500k footprint is interior:
    // r(500k) is about 4.0 at this spacing, so a half-extent of 7.2 leaves
    // room for the off-centre stamps too.
    int side = 1440;
    int repetitions = 60;
    if (argc > 1) side = std::atoi(argv[1]);
    if (argc > 2) repetitions = std::atoi(argv[2]);

    std::printf("# bench_surface_chunks: the 1.1 matrix\n");
    std::printf("# fixed-spacing plane, %d quads a side at %.4f, %d repetitions per cell\n", side,
                static_cast<double>(kSpacing), repetitions);
    const Mesh mesh = plane(side, kSpacing);
    std::printf("# model: %zu vertices, %zu triangles\n\n", mesh.positions.size(),
                mesh.indices.size() / 3);

    std::vector<Row> rows;
    for (std::size_t target : kTargets) {
        ChunkOptions options;
        options.target_faces = target;
        options.max_faces = target * 2;
        options.min_faces = target / 4 + 1;
        ChunkTable table;
        const double build_begin = now_micros();
        mesh::partition_mesh_chunks(mesh, options, &table);
        const double build_end = now_micros();
        if (!partition_is_a_partition(mesh, table)) {
            std::fprintf(stderr, "target %zu: the partition is not a partition\n", target);
            return 1;
        }
        const mesh::SurfaceView view = mesh::SurfaceView::over_mesh(mesh, table);
        bench::ChunkTree tree;
        tree.build(table);

        // 5. THE LOCALITY PROXY: the position bytes one chunk holds.
        std::size_t vertex_total = 0;
        for (std::uint32_t i = 0; i < table.slot_count(); ++i)
            if (table.chunk(i) != nullptr) vertex_total += table.vertices(i).size();
        const double bytes_per_chunk = table.live_count() == 0
                                           ? 0.0
                                           : static_cast<double>(vertex_total * 3u * sizeof(float)) /
                                                 static_cast<double>(table.live_count());

        std::printf("target %4zu  chunks %6zu  tree nodes %6zu  build %8.1f ms  index %7.2f MB  "
                    "positions/chunk %7.0f B\n",
                    target, table.live_count(), tree.nodes(), (build_end - build_begin) / 1000.0,
                    static_cast<double>(table.bytes()) / 1048576.0, bytes_per_chunk);

        for (std::size_t footprint : kFootprints) {
            const Row row = measure_fixed(mesh, table, view, tree, target, footprint, repetitions);
            rows.push_back(row);
            std::printf("  fp %6zu  query p50/p95/p99/max %7.1f %7.1f %7.1f %7.1f us | "
                        "normals p95 %8.1f | index p95 %6.2f | admitted %7zu vs exact %7zu "
                        "(%.2fx) | positions %8.1f KB vs moved %7.1f KB (%.2fx) | "
                        "indices %8.1f KB\n",
                        footprint, row.query.p50, row.query.p95, row.query.p99, row.query.max,
                        row.normals.p95, row.index_update.p95, row.admitted_vertices,
                        row.exact_vertices,
                        row.exact_vertices == 0
                            ? 0.0
                            : static_cast<double>(row.admitted_vertices) /
                                  static_cast<double>(row.exact_vertices),
                        static_cast<double>(row.upload_bytes) / 1024.0,
                        static_cast<double>(row.moved_bytes) / 1024.0,
                        row.moved_bytes == 0 ? 0.0
                                             : static_cast<double>(row.upload_bytes) /
                                                   static_cast<double>(row.moved_bytes),
                        static_cast<double>(row.index_bytes) / 1024.0);
        }
        std::printf("\n");
    }

    std::printf("# 6. topology mutation cost, adaptive surface, %d stamps with splits on\n", 200);
    for (std::size_t target : kTargets) {
        const Mutation m = measure_mutation(target, 200);
        std::printf("  target %4zu  stamp p50/p95/p99/max %8.1f %8.1f %8.1f %8.1f us  "
                    "topology ops %6zu  chunks %5zu\n",
                    target, m.stamp.p50, m.stamp.p95, m.stamp.p99, m.stamp.max, m.splits,
                    m.chunks);
    }

    std::printf("\n# the achieved chunk size per multires level, which the target does not "
                "promise\n");
    report_multires_distribution(256);

    std::printf("\n# THE DECISION RULE, read at the %zu footprint\n", kDecisionFootprint);
    std::printf("# minimise P95(query + normals + index update), subject to admitted/exact <= 2\n"
                "# and upload/moved <= 3; ties break toward the SMALLER chunk.\n");
    for (const Row& row : rows) {
        if (row.footprint != kDecisionFootprint) continue;
        const double fp = row.exact_vertices == 0 ? 0.0
                                                  : static_cast<double>(row.admitted_vertices) /
                                                        static_cast<double>(row.exact_vertices);
        const double up = row.moved_bytes == 0 ? 0.0
                                               : static_cast<double>(row.upload_bytes) /
                                                     static_cast<double>(row.moved_bytes);
        std::printf("  target %4zu  P95 %9.1f us  false-positive %.2fx %s  upload %.2fx %s\n",
                    row.target, row.decision_p95, fp, fp <= 2.0 ? "ok " : "OVER",
                    up, up <= 3.0 ? "ok " : "OVER");
    }
    return 0;
}
