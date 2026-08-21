// The benchmark spread must be a VOLUME, not a surface (#196).
//
// Every scattered-material benchmark is built from `clay_bench::scatter_cell`,
// and what those cases measure is entirely a question of how many distinct
// chunks the material lands in. A spread that collapses onto a plane piles
// blobs on top of each other, so the case measures a fraction of the work it
// claims and its ceiling is calibrated against that fraction. That is not a
// hypothetical: it is what the golden-ratio walk this replaced actually did.
//
// The last case here is the important one. It runs the SAME checks against the
// old multipliers and requires them to fail, so a test that stopped being able
// to detect the defect would itself fail rather than passing quietly.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

#include "scatter_spread.h"

namespace {

constexpr int kPoints = 4000;
constexpr int kBins = 4;

// How many cells of a kBins^3 partition of [0,1)^3 the spread reaches.
int cells_reached(double mx, double my, double mz) {
    std::set<int> seen;
    for (int i = 0; i < kPoints; ++i) {
        const double n = static_cast<double>(i);
        auto fr = [](double v) { return v - static_cast<double>(static_cast<long long>(v)); };
        const std::array<double, 3> p = {fr(n * mx), fr(n * my), fr(n * mz)};
        int key = 0;
        for (double v : p) {
            int b = static_cast<int>(v * kBins);
            if (b >= kBins) b = kBins - 1;
            if (b < 0) b = 0;
            key = key * kBins + b;
        }
        seen.insert(key);
    }
    return static_cast<int>(seen.size());
}

// The largest absolute Pearson correlation between any two of the three axes.
double worst_correlation(double mx, double my, double mz) {
    auto fr = [](double v) { return v - static_cast<double>(static_cast<long long>(v)); };
    const std::array<double, 3> mult = {mx, my, mz};
    std::array<std::vector<double>, 3> axis;
    for (int a = 0; a < 3; ++a) {
        axis[a].reserve(kPoints);
        for (int i = 0; i < kPoints; ++i)
            axis[a].push_back(fr(static_cast<double>(i) * mult[a]));
    }
    double worst = 0.0;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            double ma = 0.0, mb = 0.0;
            for (int i = 0; i < kPoints; ++i) { ma += axis[a][i]; mb += axis[b][i]; }
            ma /= kPoints;
            mb /= kPoints;
            double num = 0.0, da = 0.0, db = 0.0;
            for (int i = 0; i < kPoints; ++i) {
                num += (axis[a][i] - ma) * (axis[b][i] - mb);
                da += (axis[a][i] - ma) * (axis[a][i] - ma);
                db += (axis[b][i] - mb) * (axis[b][i] - mb);
            }
            worst = std::max(worst, std::abs(num / (std::sqrt(da) * std::sqrt(db))));
        }
    return worst;
}

}  // namespace

TEST_CASE("bench spread: the scatter fills the volume") {
    const int reached =
        cells_reached(clay_bench::kScatterX, clay_bench::kScatterY, clay_bench::kScatterZ);
    CHECK(reached == kBins * kBins * kBins);
}

TEST_CASE("bench spread: no two axes are correlated") {
    // Algebraic dependence between the multipliers shows up here as a
    // correlation near 1, whatever plane it puts the points on.
    const double worst =
        worst_correlation(clay_bench::kScatterX, clay_bench::kScatterY, clay_bench::kScatterZ);
    CHECK(worst < 0.05);
    MESSAGE("worst pairwise correlation: " << worst);
}

TEST_CASE("bench spread: the scatter reaches distinct cells on an integer lattice") {
    // What the benchmarks actually consume. 400 blobs on a 40-cell lattice is
    // BM_VoxelAddLevelWhole's own workload; the collapsed spread put them in
    // far fewer places than there are blobs.
    std::set<std::tuple<int, int, int>> seen;
    for (int i = 0; i < 400; ++i) {
        int x = 0, y = 0, z = 0;
        clay_bench::scatter_cell(i, 40, &x, &y, &z);
        seen.insert({x, y, z});
    }
    CHECK(seen.size() >= 390);
    MESSAGE("400 blobs land in " << seen.size() << " distinct cells");
}

TEST_CASE("bench spread: the checks above can still detect the defect they exist for") {
    // The golden-ratio walk this replaced. g^2 == 1-g, so frac(i*g^2) is
    // exactly 1 - frac(i*g) and y == -x for every point; g^3 == 2g-1 ties z to
    // x as well. If these assertions ever stop failing, the two above have
    // stopped meaning anything.
    const double g = 0.6180339887;
    CHECK(cells_reached(g, g * g, g * g * g) < kBins * kBins * kBins);
    CHECK(worst_correlation(g, g * g, g * g * g) > 0.9);

    // And the obvious replacement, which fails the same way on a different
    // pair: the plastic number's powers satisfy a^2 + a^3 == 1 exactly.
    const double a = 0.7548776662;
    CHECK(worst_correlation(a, a * a, a * a * a) > 0.9);
}
