// THE SCALING MATRIX (add-extreme-poly-runtime 7.1 and 7.2).
//
// One sentence is what the whole change serves: a dab costs approximately what
// it TOUCHES, not what the model HOLDS. This is the measurement that says
// whether that is true, across the sizes the requirement names — 100k, 1M, 5M,
// 10M and 20M vertices — at footprints of 1k, 5k, 20k, 100k and 500k touched
// vertices, on the fixed mesh, the adaptive surface and the hierarchy.
//
// THE FIXTURE IS FIXED-SPACING WITH A GROWING EXTENT, and this is the whole
// experiment. "A bigger model" here means MORE OF THE SAME GEOMETRY AT THE SAME
// DETAIL — never a more finely subdivided one, which would grow the footprint
// along with the model and make the matrix measure nothing. A footprint is a
// world radius chosen so that pi*r^2/spacing^2 is the wanted vertex count, and
// the model is always wide enough that the largest footprint is interior; a dab
// clipped by the model boundary measures the boundary.
//
// RATIOS, NOT MILLISECONDS. A time on this machine is a time on this machine.
// The portable number is the ratio of the same footprint at two model sizes,
// which is what `tools/bench_extreme_poly.py` computes and what the locality
// gate is stated in. This program prints the absolutes so the driver can form
// them, and prints the ratio itself for a reader who is running it by hand.
//
// P50, P95, P99 AND MAX — never an average alone. An average over a stroke
// hides the dab that dropped the frame, and a dropped frame is the only thing
// an artist can feel.
//
// WHAT THE STAGE BREAKDOWN IS. Requirement 7.2 names fourteen stages. This
// program times the ones separable FROM OUTSIDE the library — seed, chunk
// query, index update, remesh, detail write and readback, each its own call —
// and the ones INSIDE `MeshSculptor::stamp` are now timed by the library and
// printed under `stage breakdown`: seed, query, weight, alpha, automask,
// snapshot, neighbors, kernel, writeback, normals and chunkmark.
//
// They were one bucket called `stamp*` until `mesh::StageTelemetry` existed,
// on the argument that adding timers would perturb what was being measured.
// What resolved that is that NO CLOCK IS READ when the telemetry pointer is
// null, so a stamp outside this program pays one predictable branch per stage
// and nothing else. The two views are kept side by side rather than one
// replacing the other: the wall-clock rows are a distribution and the stage
// rows are a mean, and a mean that disagreed with the p50 above it is itself
// worth seeing.
//
// NOT A GATED BENCHMARK. The gate at sizes CI can afford is
// `tests/unit/test_extreme_poly_scaling.cpp`; this is the evidence behind it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/layered_sculpt.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/surface_view.h"

#include "chunk_tree.h"

using namespace clay;
using namespace clay::kernel;
using mesh::ChunkOptions;
using mesh::ChunkTable;
using mesh::Mesh;

namespace {

constexpr float kSpacing = 0.01f;

double now_micros() {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Resident set, so a row that is about to exhaust the machine says so before it
// does. Reported per row because the matrix's largest rows are the ones a
// reader will want to know the cost of before repeating them.
std::size_t rss_kb() {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    long total = 0, resident = 0;
    if (std::fscanf(f, "%ld %ld", &total, &resident) != 2) resident = 0;
    std::fclose(f);
    return static_cast<std::size_t>(resident) * 4u;  // pages of 4 KiB
}

struct Stats {
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
    std::size_t n = 0;
};

Stats summarise(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) {
        return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = v.back();
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    s.n = v.size();
    return s;
}

// A plane at fixed spacing, measured outward from the centre so the vertex at
// the origin is the same float at every model size — otherwise `-half +
// spacing*x` rounds differently at two sizes and the footprint is not held
// constant.
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

int side_for(std::size_t vertices) {
    return static_cast<int>(std::sqrt(static_cast<double>(vertices))) - 1;
}

float radius_for(std::size_t vertices) {
    return kSpacing * std::sqrt(static_cast<float>(vertices) / 3.14159265f);
}

const cfloat3 kCentres[] = {cf3(0, 0, 0), cf3(0.137f, 0, -0.211f), cf3(-0.301f, 0, 0.089f),
                            cf3(0.052f, 0, 0.463f), cf3(-0.088f, 0, -0.372f)};

cfloat3 centre_for(int rep) {
    return kCentres[static_cast<std::size_t>(rep) % (sizeof(kCentres) / sizeof(kCentres[0]))];
}

// The library's own stage breakdown, as a MEAN per stamp: the accumulators are
// sums over the measured reps, so there is no distribution here to quote a p95
// from. The wall-clock percentiles above stay the headline; this says where the
// stamp's own time went.
void print_stages(const mesh::StageTelemetry& stages, double reps) {
    if (reps <= 0.0) return;
    double total = 0.0;
    for (std::size_t i = 0; i < mesh::kSculptStageCount; ++i)
        total += static_cast<double>(stages.nanos[i]);
    if (total <= 0.0) return;
    std::printf("        stage breakdown (mean us/stamp, %.0f reps):\n", reps);
    for (std::size_t i = 0; i < mesh::kSculptStageCount; ++i) {
        if (stages.calls[i] == 0) continue;
        const double us = static_cast<double>(stages.nanos[i]) / reps / 1000.0;
        std::printf("          %-10s %8.3f  (%4.1f%%)\n",
                    mesh::StageTelemetry::name(static_cast<mesh::SculptStage>(i)), us,
                    100.0 * static_cast<double>(stages.nanos[i]) / total);
    }
}

void print_stage(const char* name, const Stats& s) {
    if (s.n == 0) return;
    std::printf("      %-14s p50 %9.2f  p95 %9.2f  p99 %9.2f  max %9.2f  mean %9.2f us\n", name,
                s.p50, s.p95, s.p99, s.max, s.mean);
}

// -- the fixed mesh -----------------------------------------------------------------

void run_fixed(std::size_t vertices, const std::vector<std::size_t>& footprints, int reps) {
    const double build_begin = now_micros();
    Mesh mesh = plane(side_for(vertices), kSpacing);
    const std::size_t actual = mesh.positions.size();
    ChunkOptions options;
    ChunkTable table;
    mesh::partition_mesh_chunks(mesh, options, &table);
    bench::ChunkTree tree;
    tree.build(table);
    mesh::MeshSculptor sculptor(mesh);
    // A host that places a brush has picked, and picking builds the tree. The
    // sculptor never builds one on its own behalf — 689 ms against 1.24 ms
    // saved per stamp — so the benchmark builds it the way a host does.
    (void)sculptor.bvh();
    const double build_end = now_micros();

    std::printf("  fixed mesh: %zu vertices, %zu triangles, %zu chunks, setup %.2f s, "
                "rss %zu MB\n",
                actual, mesh.indices.size() / 3, table.live_count(),
                (build_end - build_begin) / 1e6, rss_kb() / 1024);

    const float half_extent = kSpacing * static_cast<float>(side_for(vertices)) * 0.5f;
    for (std::size_t footprint : footprints) {
        const float radius = radius_for(footprint);
        // A FOOTPRINT LARGER THAN THE MODEL IS NOT A FOOTPRINT. The dab would
        // be clipped by the boundary and the row would measure the boundary,
        // which is the one thing the fixed-spacing fixture exists to avoid.
        if (radius * 1.6f > half_extent) {
            std::printf("    footprint %6zu  SKIPPED: r %.3f does not fit inside a model of "
                        "half-extent %.3f\n",
                        footprint, static_cast<double>(radius),
                        static_cast<double>(half_extent));
            continue;
        }
        mesh::MeshBrushSettings brush;
        brush.radius = radius;
        brush.strength = 0.15f;
        brush.smooth_iterations = 2;
        brush.geodesic = mesh::default_geodesic(mesh::MeshBrush::Draw);

        std::vector<double> seed, query, stamp, index, readback;
        // 7.2: the eight stages inside the stamp, timed by the library rather
        // than bucketed by this program. `set_stage_telemetry` is what closed
        // the `stamp*` bucket below.
        mesh::StageTelemetry stages;
        sculptor.set_stage_telemetry(&stages);
        // The sculptor publishes the dirty stream this row's readback measures.
        // Without it the marking that used to happen in the loop below is gone
        // and the upload column reads zero — which is how this was caught.
        sculptor.set_chunks(&table);
        std::size_t reached = 0, upload = 0, workset = 0;
        const mesh::SurfaceView view = mesh::SurfaceView::over_mesh(mesh, table);
        std::vector<std::uint32_t> admitted;

        for (int rep = -4; rep < reps; ++rep) {  // negatives are the warm-up
            brush.center = centre_for(rep < 0 ? 0 : rep);
            // Reset after the warm-up, so the stage means are over the measured
            // reps and not diluted by four cold ones.
            if (rep == 0) stages.reset();

            const double t0 = now_micros();
            const std::uint32_t anchor = sculptor.nearest_class(brush.center);
            const double t1 = now_micros();

            const math::Aabb box{cf3(brush.center.x - radius, brush.center.y - radius,
                                     brush.center.z - radius),
                                 cf3(brush.center.x + radius, brush.center.y + radius,
                                     brush.center.z + radius)};
            // THROUGH THE TREE. A walk over every chunk testing its bounds is
            // O(chunks), which is O(model) at a fixed footprint — so a harness
            // written that way fails the very claim this program measures, and
            // reports it as an engine defect.
            tree.query(box, &admitted);
            const double t2 = now_micros();

            brush.seed_class = anchor;
            sculptor.stamp(mesh::MeshBrush::Draw, brush);
            const double t3 = now_micros();

            // The sculptor marks its own chunks now. This loop used to do it
            // from OUTSIDE, over the chunks the QUERY admitted rather than the
            // ones the stamp WROTE — host-side logic every host had to copy,
            // and a superset: the query's ball reaches chunks a falloff and an
            // automask then decline to move.
            sculptor.refit_bvh();
            const double t4 = now_micros();

            std::size_t bytes = 0;
            for (std::uint32_t id : table.dirty()) {
                const mesh::ChunkReadback sized =
                    view.copy_chunk(id, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
                bytes += static_cast<std::size_t>(sized.vertex_count) * 3u * sizeof(float) +
                         static_cast<std::size_t>(sized.index_count) * sizeof(std::uint32_t);
            }
            const double t5 = now_micros();
            table.clear_dirty();

            if (rep < 0) continue;
            seed.push_back(t1 - t0);
            query.push_back(t2 - t1);
            stamp.push_back(t3 - t2);
            index.push_back(t4 - t3);
            readback.push_back(t5 - t4);
            reached = admitted.size();
            upload = bytes;
            workset = sculptor.workset().size();
        }

        std::vector<double> total(stamp.size());
        for (std::size_t i = 0; i < stamp.size(); ++i)
            total[i] = seed[i] + query[i] + stamp[i] + index[i] + readback[i];

        std::printf("    footprint %6zu (r %.4f)  workset %7zu  chunks %5zu  upload %8.1f KB\n",
                    footprint, static_cast<double>(radius), workset, reached,
                    static_cast<double>(upload) / 1024.0);
        print_stage("seed", summarise(seed));
        print_stage("chunk query", summarise(query));
        print_stage("stamp", summarise(stamp));
        print_stage("index update", summarise(index));
        print_stage("readback", summarise(readback));
        print_stage("TOTAL", summarise(total));
        print_stages(stages, static_cast<double>(stamp.size()));
    }
}

// -- the adaptive surface -----------------------------------------------------------

void run_adaptive(std::size_t vertices, const std::vector<std::size_t>& footprints, int reps) {
    const double build_begin = now_micros();
    Mesh source = plane(side_for(vertices), kSpacing);
    auto surface = mesh::DynamicSurface::from_mesh(source);
    if (!surface.has_value()) {
        std::printf("  adaptive surface: refused this fixture\n");
        return;
    }
    source = Mesh{};  // the conversion is done; give the source back
    mesh::DynamicSculptor sculptor(*surface);
    const double build_end = now_micros();
    std::printf("  adaptive surface: %zu vertices, %zu faces, %zu chunks, setup %.2f s, "
                "rss %zu MB\n",
                surface->stats().vertices, surface->stats().faces,
                sculptor.bvh().chunks().live_count(), (build_end - build_begin) / 1e6,
                rss_kb() / 1024);

    const float half_extent = kSpacing * static_cast<float>(side_for(vertices)) * 0.5f;
    for (std::size_t footprint : footprints) {
        const float radius = radius_for(footprint);
        if (radius * 1.6f > half_extent) {
            std::printf("    footprint %6zu  SKIPPED: does not fit inside this model\n",
                        footprint);
            continue;
        }
        mesh::MeshBrushSettings brush;
        brush.radius = radius;
        brush.strength = 0.15f;
        mesh::DynamicTopologySettings topology;
        topology.enabled = true;
        topology.detail_mode = mesh::DynamicDetailMode::BrushRelative;

        std::vector<double> stamp, readback;
        std::size_t dirty = 0, upload = 0, remesh_ops = 0;
        for (int rep = -4; rep < reps; ++rep) {
            brush.center = centre_for(rep < 0 ? 0 : rep);
            const double t0 = now_micros();
            const mesh::DynamicStampResult r =
                sculptor.stamp(mesh::MeshBrush::Draw, brush, topology);
            const double t1 = now_micros();

            const mesh::SurfaceView view =
                mesh::SurfaceView::over_dynamic(sculptor.surface(), sculptor.bvh().chunks());
            std::size_t bytes = 0;
            for (std::uint32_t id : view.dirty_chunks()) {
                const mesh::ChunkReadback sized =
                    view.copy_chunk(id, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
                bytes += static_cast<std::size_t>(sized.vertex_count) * 3u * sizeof(float) +
                         static_cast<std::size_t>(sized.index_count) * sizeof(std::uint32_t);
            }
            const double t2 = now_micros();
            dirty = view.dirty_chunks().size();
            sculptor.bvh().clear_dirty();
            if (rep < 0) continue;
            stamp.push_back(t1 - t0);
            readback.push_back(t2 - t1);
            upload = bytes;
            remesh_ops += r.remesh.total();
        }
        std::printf("    footprint %6zu  dirty chunks %5zu  upload %8.1f KB  topology ops %zu\n",
                    footprint, dirty, static_cast<double>(upload) / 1024.0, remesh_ops);
        print_stage("stamp+remesh*", summarise(stamp));
        print_stage("readback", summarise(readback));
    }
}

// -- the hierarchy -------------------------------------------------------------------

void run_multires(std::size_t vertices, const std::vector<std::size_t>& footprints, int reps,
                  int levels, bool layered) {
    // The cage carries the extent and the levels carry the detail, so the cage
    // side is the model side divided by 2^levels — which keeps the SPACING at
    // the finest level equal to every other representation's.
    const int fine_side = side_for(vertices);
    const int cage_side = std::max(2, fine_side >> levels);
    const float cage_spacing = kSpacing * static_cast<float>(1 << levels);

    const double build_begin = now_micros();
    Mesh cage;
    const int centre = cage_side / 2;
    for (int z = 0; z <= cage_side; ++z)
        for (int x = 0; x <= cage_side; ++x)
            cage.positions.push_back(cf3(cage_spacing * static_cast<float>(x - centre), 0.0f,
                                         cage_spacing * static_cast<float>(z - centre)));
    const auto at = [&](int x, int z) {
        return static_cast<std::uint32_t>(z * (cage_side + 1) + x);
    };
    for (int z = 0; z < cage_side; ++z)
        for (int x = 0; x < cage_side; ++x) {
            cage.quads.push_back(at(x, z));
            cage.quads.push_back(at(x + 1, z));
            cage.quads.push_back(at(x + 1, z + 1));
            cage.quads.push_back(at(x, z + 1));
            cage.indices.insert(cage.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1),
                                                     at(x, z), at(x + 1, z + 1), at(x, z + 1)});
        }
    auto surface = mesh::MultiresSurface::from_mesh(cage);
    if (!surface.has_value()) {
        std::printf("  hierarchy: refused this cage\n");
        return;
    }
    for (int i = 0; i < levels; ++i) {
        mesh::MultiresError error = mesh::MultiresError::None;
        if (!surface->add_level(&error)) {
            std::printf("  hierarchy: stopped at level %d (%s)\n", i,
                        mesh::multires_error_text(error));
            break;
        }
    }
    const std::uint32_t level = surface->max_level();
    surface->set_sculpt_level(level);
    surface->set_display_level(level);
    surface->positions_at(level);
    surface->chunks_at(level);
    mesh::MultiresSculptor sculptor(*surface);
    // The LAYERED row (7.1). A non-destructive detail pass writes the same
    // verbs into a layer's coefficients rather than into the base, through the
    // same level sculptor -- so the interesting number is not the absolute but
    // the RATIO against the row above: what a stroke pays for being undoable
    // and dialable after the fact.
    mesh::LayeredMultiresSculptor layered_sculptor(*surface);
    if (layered) {
        const mesh::SculptLayerId id = surface->add_sculpt_layer("bench");
        surface->set_active_sculpt_layer(id);
        layered_sculptor.set_write_domain(mesh::MultiresWriteDomain::Detail);
    }
    const double build_end = now_micros();

    std::printf("  %s: cage %zu, level %u with %zu vertices, %zu chunks, setup %.2f s, "
                "rss %zu MB\n",
                layered ? "hierarchy + layers" : "hierarchy",
                cage.positions.size(), level, surface->positions_at(level).size(),
                surface->chunks_at(level).live_count(), (build_end - build_begin) / 1e6,
                rss_kb() / 1024);

    const float half_extent = kSpacing * static_cast<float>(fine_side) * 0.5f;
    for (std::size_t footprint : footprints) {
        const float radius = radius_for(footprint);
        if (radius * 1.6f > half_extent) {
            std::printf("    footprint %6zu  SKIPPED: does not fit inside this model\n",
                        footprint);
            continue;
        }
        mesh::MeshBrushSettings brush;
        brush.radius = radius;
        brush.strength = 0.15f;
        brush.geodesic = mesh::default_geodesic(mesh::MeshBrush::Draw);

        std::vector<double> stamp, detail, readback;
        std::size_t dirty = 0, upload = 0;
        mesh::StageTelemetry stages;
        if (layered) {
            // ONE gesture over the whole row, which is what a layered stroke
            // is: `begin` fixes the channel and holds the composition, and
            // paying that per dab would measure the transaction rather than
            // the dab.
            if (!layered_sculptor.begin()) {
                std::printf("    footprint %6zu  SKIPPED: the layer stroke refused to begin\n",
                            footprint);
                continue;
            }
        } else {
            sculptor.begin_stroke();
            sculptor.set_stage_telemetry(&stages);
        }
        for (int rep = -4; rep < reps; ++rep) {
            brush.center = centre_for(rep < 0 ? 0 : rep);
            if (rep == 0) stages.reset();
            const double t0 = now_micros();
            if (layered)
                layered_sculptor.stamp(mesh::MeshBrush::Draw, brush);
            else
                sculptor.stamp(mesh::MeshBrush::Draw, brush);
            const double t1 = now_micros();
            // The detail write and the propagation it causes, which is the
            // stage this representation has and the other two do not.
            surface->positions_at(level);
            const double t2 = now_micros();

            const mesh::SurfaceView view = mesh::SurfaceView::over_level(*surface, level);
            std::size_t bytes = 0;
            for (std::uint32_t id : view.dirty_chunks()) {
                const mesh::ChunkReadback sized =
                    view.copy_chunk(id, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
                bytes += static_cast<std::size_t>(sized.vertex_count) * 3u * sizeof(float) +
                         static_cast<std::size_t>(sized.index_count) * sizeof(std::uint32_t);
            }
            const double t3 = now_micros();
            dirty = view.dirty_chunks().size();
            upload = bytes;
            surface->clear_dirty_chunks(level);
            if (rep < 0) continue;
            stamp.push_back(t1 - t0);
            detail.push_back(t2 - t1);
            readback.push_back(t3 - t2);
        }
        if (layered) layered_sculptor.commit();
        std::printf("    footprint %6zu  dirty chunks %5zu  upload %8.1f KB\n", footprint, dirty,
                    static_cast<double>(upload) / 1024.0);
        print_stage("stamp", summarise(stamp));
        print_stage("detail write", summarise(detail));
        print_stage("readback", summarise(readback));
        if (!layered) print_stages(stages, static_cast<double>(stamp.size()));
    }
}

// What the driver asked for. A struct rather than five out-parameters, so
// `main` reads as the matrix it runs rather than as the parsing that got there.
struct Options {
    std::vector<std::size_t> sizes = {100000, 1000000, 5000000};
    std::vector<std::size_t> footprints = {1000, 5000, 20000, 100000, 500000};
    int reps = 40;
    int levels = 3;
    std::string which = "all";
};

void parse_list(const std::string& text, std::vector<std::size_t>* out) {
    out->clear();
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t comma = text.find(',', at);
        const std::string item = text.substr(at, comma - at);
        if (!item.empty())
            out->push_back(static_cast<std::size_t>(std::strtoull(item.c_str(), nullptr, 10)));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--sizes=", 0) == 0) parse_list(arg.substr(8), &o.sizes);
        else if (arg.rfind("--footprints=", 0) == 0) parse_list(arg.substr(13), &o.footprints);
        else if (arg.rfind("--reps=", 0) == 0) o.reps = std::atoi(arg.c_str() + 7);
        else if (arg.rfind("--levels=", 0) == 0) o.levels = std::atoi(arg.c_str() + 9);
        else if (arg.rfind("--which=", 0) == 0) o.which = arg.substr(8);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);
    const std::vector<std::size_t>& sizes = options.sizes;
    const std::vector<std::size_t>& footprints = options.footprints;
    const int reps = options.reps;
    const int levels = options.levels;
    const std::string& which = options.which;

    std::printf("# bench_extreme_poly: the 7.1 matrix with 7.2's separable stages\n");
    std::printf("# `stage breakdown` is the library's own per-stage timing inside\n");
    std::printf("#   MeshSculptor::stamp (mesh::StageTelemetry). No clock is read when the\n");
    std::printf("#   telemetry pointer is null, which is what let 7.2's remaining stages be\n");
    std::printf("#   timed without perturbing a stamp outside this program.\n");
    std::printf("# spacing %.4f, %d repetitions per cell after 4 warm-up stamps\n",
                static_cast<double>(kSpacing), reps);

    for (std::size_t size : sizes) {
        std::printf("== %zu vertices ==============================================\n", size);
        if (which == "all" || which == "fixed") run_fixed(size, footprints, reps);
        if (which == "all" || which == "adaptive") run_adaptive(size, footprints, reps);
        if (which == "all" || which == "multires")
            run_multires(size, footprints, reps, levels, /*layered=*/false);
        // 7.1's fourth path. Run beside the third rather than instead of it, so
        // the pair is a RATIO and not two numbers from different sessions on a
        // box whose load moves between them.
        if (which == "all" || which == "layers")
            run_multires(size, footprints, reps, levels, /*layered=*/true);
        std::printf("\n");
    }
    std::printf("# peak rss %zu MB\n", rss_kb() / 1024);
    return 0;
}
