// WHAT A MEMORY WARNING COSTS THE DAB AFTER IT (add-extreme-poly-runtime 4.4,
// 4.6 and 7.2).
//
// The memory-pressure gate says a trim is CORRECT: the checksum survives it and
// every dropped cache reconstructs. It says nothing about what the next dab
// PAYS, and that is the number a host actually needs, because `trim` is called
// from an operating-system callback that can land in the middle of a drag. A
// host that knows the price can decide between answering the warning now and
// holding a `MemoryPin` until the stroke ends; a host that does not know it
// finds out as a dropped frame.
//
// WHY THIS BENCHMARK EXISTS AT ALL, and it is not a tuning exercise. The stamp
// after a critical trim used to cost NOTHING, because `MultiresSculptor::bind`
// compared a `cache_generation` that a release did not move and therefore kept
// a sculptor bound to a freed `LevelCache` — so the dab wrote into released
// storage and vanished. Making it correct made it cost a rebuild, and the size
// of that cost is what a host has to plan around. Measuring it is how "the fix
// is affordable" stops being an assertion.
//
// THREE COLUMNS, and the third is the point:
//
//   undisturbed  a dab in a stroke nobody interrupted
//   after warning  the dab following Pressure::Warning, which drops the levels
//                  nobody is looking at and leaves the sculpt level alone
//   after critical the dab following Pressure::Critical, which drops
//                  everything and is therefore a full rebuild of the levels
//                  under the one being sculpted
//
// P50, P95, P99 AND MAX rather than an average: the recovery dab IS the tail,
// and an average that mixed it with the cheap dabs around it would report a
// number no frame ever took. RATIOS against the undisturbed column, because an
// absolute microsecond count is a fact about this machine.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "clay/memory/budget.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/surface_view.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MultiresSculptor;
using mesh::MultiresSurface;

namespace {

double now_micros() {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Stats {
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
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
    s.n = v.size();
    return s;
}

// A cage at FIXED SPACING whose extent grows with `n` — the same fixture rule
// the rest of this change measures under. A more finely subdivided cage would
// grow the footprint with the model and the ratio would be measuring that.
Mesh quad_field(int n, float spacing) {
    Mesh m;
    const int centre = n / 2;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(spacing * static_cast<float>(x - centre), 0.0f,
                                      spacing * static_cast<float>(z - centre)));
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.quads.push_back(at(x, z));
            m.quads.push_back(at(x + 1, z));
            m.quads.push_back(at(x + 1, z + 1));
            m.quads.push_back(at(x, z + 1));
            m.indices.insert(m.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1), at(x, z),
                                               at(x + 1, z + 1), at(x, z + 1)});
        }
    return m;
}

enum class Warning { None, Warning, Critical };

// One stroke of `dabs` dabs, timing each one. The trim happens AFTER the timed
// dab, so what is being timed is always the recovery and never the release.
Stats stroke(int cage, std::uint32_t levels, Warning warning, int dabs, std::size_t* moved_out) {
    auto surface = MultiresSurface::from_mesh(quad_field(cage, 0.25f));
    if (!surface.has_value()) return {};
    for (std::uint32_t i = 0; i < levels; ++i)
        if (!surface->add_level()) return {};
    const std::uint32_t level = surface->max_level();
    if (!surface->set_sculpt_level(level)) return {};
    // Warm, so the first dab is not paying for the hierarchy's first
    // evaluation and reporting it as the recovery cost.
    surface->positions_at(level);

    MultiresSculptor sculptor(*surface);
    sculptor.begin_stroke();
    MeshBrushSettings settings;
    settings.radius = 0.35f;
    settings.strength = 0.2f;

    std::vector<double> samples;
    std::size_t moved = 0;
    const float span = 0.6f;
    for (int i = 0; i < dabs; ++i) {
        // A circle rather than a line, so a long stroke keeps returning to
        // surface it has already moved instead of walking off the cage.
        const float t = 6.2831853f * static_cast<float>(i) / static_cast<float>(dabs);
        settings.center = cf3(span * std::cos(t), 0.0f, span * std::sin(t));
        const double t0 = now_micros();
        moved += sculptor.stamp(MeshBrush::Draw, settings);
        samples.push_back(now_micros() - t0);
        switch (warning) {
            case Warning::None: break;
            case Warning::Warning:
                mesh::trim_surface(*surface, memory::Pressure::Warning);
                break;
            case Warning::Critical:
                mesh::trim_surface(*surface, memory::Pressure::Critical);
                break;
        }
    }
    if (moved_out != nullptr) *moved_out = moved;
    return summarise(std::move(samples));
}

void row(const char* label, const Stats& s, const Stats& base) {
    const auto ratio = [&](double a, double b) { return b > 0 ? a / b : 0.0; };
    std::printf("  %-16s %9.1f %9.1f %9.1f %9.1f   %6.2fx %6.2fx\n", label, s.p50, s.p95, s.p99,
                s.max, ratio(s.p50, base.p50), ratio(s.p95, base.p95));
}

}  // namespace

int main(int argc, char** argv) {
    int dabs = 200;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--dabs" && i + 1 < argc) dabs = std::atoi(argv[++i]);

    std::printf(
        "A MEMORY WARNING MID-STROKE, and what the dab after it costs.\n"
        "Times are microseconds per dab on THIS machine; the two ratio columns are\n"
        "p50 and p95 against the undisturbed stroke and are the portable numbers.\n");

    struct Model {
        const char* name;
        int cage;
        std::uint32_t levels;
    };
    const Model models[] = {
        {"6^2 cage, 3 levels", 6, 3},
        {"12^2 cage, 3 levels", 12, 3},
        {"24^2 cage, 3 levels", 24, 3},
    };

    for (const Model& m : models) {
        std::size_t moved_none = 0, moved_warning = 0, moved_critical = 0;
        const Stats none = stroke(m.cage, m.levels, Warning::None, dabs, &moved_none);
        const Stats warn = stroke(m.cage, m.levels, Warning::Warning, dabs, &moved_warning);
        const Stats crit = stroke(m.cage, m.levels, Warning::Critical, dabs, &moved_critical);

        std::printf("\n%s  (%d dabs each)\n", m.name, dabs);
        std::printf("  %-16s %9s %9s %9s %9s   %7s %7s\n", "", "p50", "p95", "p99", "max",
                    "p50 x", "p95 x");
        row("undisturbed", none, none);
        row("after warning", warn, none);
        row("after critical", crit, none);
        // THE SAME STROKE, WHICH IS WHAT MAKES THE RATIO MEAN ANYTHING. Three
        // runs that reached different amounts of surface would be three
        // different measurements wearing one table.
        std::printf("  weld classes moved: %zu / %zu / %zu%s\n", moved_none, moved_warning,
                    moved_critical,
                    (moved_none == moved_warning && moved_none == moved_critical)
                        ? "   (identical, as the determinism gate requires)"
                        : "   *** THE THREE STROKES DIVERGED ***");
    }
    return 0;
}
