// THE SERIAL THRESHOLD (add-extreme-poly-runtime task 3.7).
//
// `kVertexParallelGrain` and `kChunkParallelGrain` are the point below which a
// footprint does not dispatch at all. They shipped as round numbers with a
// comment asserting that "at a few hundred vertices the dispatch is the
// measurement", which is exactly the claim task 3.7 says to stop making and
// start measuring — and the same failure task 1.1 was written against, where
// the chunk size was nearly adopted from prior art that had never been run
// here.
//
// THE RULE, fixed before the data exists, in the shape D2 used for 1.1:
//
//   > The grain is the SMALLEST footprint at which the parallel form's P50 is
//   > at least 15% faster than the serial form's, and stays faster at every
//   > larger footprint measured. A crossover that does not hold at the next
//   > size up is noise, not a threshold.
//
// The 15% is a margin against this box rather than a preference: it is a shared
// machine that picks up unrelated jobs, and a threshold read off a 3% win would
// move between runs. Erring high costs a little parallelism on medium dabs;
// erring low costs a dispatch on every small dab of every stroke, which is the
// case that has to stay cheap.
//
// THE WORKLOAD IS THE ONE THE GRAIN GUARDS, not a synthetic spin. A per-vertex
// sculpt pass reads a position and a normal, computes a falloff weight from a
// distance and writes a displaced position: a few flops against three streams
// of memory. A heavier body would cross over sooner and would measure a
// workload this constant does not gate.
//
// RATIOS AND PERCENTILES. Absolute microseconds off this box do not travel; the
// parallel/serial ratio does, and it is what the rule is written on. The load
// average is printed either side of the run so a row taken while the box was
// busy can be discarded rather than believed.
//
// NOT A GATED BENCHMARK. It exits non-zero only if the two forms disagree about
// the result they computed, because a measurement of two different workloads is
// worse than no measurement.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/parallel/thread_pool.h"

using namespace clay;
using namespace clay::kernel;

namespace {

// The load average, printed either side of the sweep. A row taken while the box
// was busy is not comparable with one taken while it was idle, and this is a
// SHARED machine — recording it is what makes a re-run decidable rather than a
// matter of opinion.
void print_load(const char* when) {
    std::FILE* f = std::fopen("/proc/loadavg", "r");
    if (f == nullptr) return;
    double one = 0, five = 0, fifteen = 0;
    if (std::fscanf(f, "%lf %lf %lf", &one, &five, &fifteen) == 3)
        std::printf("# load %s: %.2f %.2f %.2f\n", when, one, five, fifteen);
    std::fclose(f);
}

double now_micros() {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Stats {
    double p50 = 0, p95 = 0;
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
    return s;
}

// One per-vertex sculpt pass: a falloff weight from the distance to the brush
// centre, applied along the vertex normal. The shape of the weight pass and the
// write-back that `kVertexParallelGrain` actually gates.
void pass(const std::vector<cfloat3>& positions, const std::vector<cfloat3>& normals,
          std::vector<cfloat3>* out, std::size_t begin, std::size_t end) {
    const cfloat3 centre = cf3(0, 0, 0);
    const float radius = 1.0f;
    for (std::size_t i = begin; i < end; ++i) {
        const float d = clength(positions[i] - centre) / radius;
        const float t = d >= 1.0f ? 0.0f : 1.0f - d * d;
        (*out)[i] = positions[i] + normals[i] * (t * t * 0.05f);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int repetitions = 201;
    if (argc > 1) repetitions = std::atoi(argv[1]);

    std::printf("# bench_parallel_grain: the 3.7 serial threshold\n");
    std::printf("# %d repetitions per cell; the rule is P50 at least 15%% faster,\n",
                repetitions);
    std::printf("# holding at every larger footprint.\n");
    std::printf("# shipped: kVertexParallelGrain = 1024, kChunkParallelGrain = 4\n");
    print_load("before");

    // Doubling from far below any plausible threshold to far above it.
    const std::size_t sizes[] = {64,   128,   256,   512,   1024,  2048,
                                 4096, 8192,  16384, 32768, 65536, 131072};

    std::printf("\n%10s %12s %12s %12s %12s %9s\n", "vertices", "serial p50", "par p50",
                "serial p95", "par p95", "ratio");

    std::size_t crossover = 0;
    bool held = true;
    for (std::size_t n : sizes) {
        std::vector<cfloat3> positions(n), normals(n), out_serial(n), out_parallel(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float f = static_cast<float>(i) * 1e-4f;
            positions[i] = cf3(std::sin(f), f * 0.01f, std::cos(f));
            normals[i] = cf3(0, 1, 0);
        }

        std::vector<double> serial, par;
        serial.reserve(static_cast<std::size_t>(repetitions));
        par.reserve(static_cast<std::size_t>(repetitions));
        // Warm both forms before either is timed: the pool's threads park
        // between jobs and the first dispatch after a pause pays to wake them,
        // which is a cost no stroke pays twice.
        pass(positions, normals, &out_serial, 0, n);
        parallel::for_range(n, 1, [&](std::size_t b, std::size_t e) {
            pass(positions, normals, &out_parallel, b, e);
        });

        for (int r = 0; r < repetitions; ++r) {
            const double a = now_micros();
            pass(positions, normals, &out_serial, 0, n);
            const double b = now_micros();
            parallel::for_range(n, 1, [&](std::size_t lo, std::size_t hi) {
                pass(positions, normals, &out_parallel, lo, hi);
            });
            const double c = now_micros();
            serial.push_back(b - a);
            par.push_back(c - b);
        }

        // The two forms must have computed the same thing, or the comparison is
        // between two different workloads.
        for (std::size_t i = 0; i < n; ++i) {
            if (clength(out_serial[i] - out_parallel[i]) > 1e-6f) {
                std::fprintf(stderr, "n=%zu: the two forms disagree at %zu\n", n, i);
                return 1;
            }
        }

        const Stats s = summarise(serial), p = summarise(par);
        const double ratio = p.p50 <= 0.0 ? 0.0 : s.p50 / p.p50;
        std::printf("%10zu %12.2f %12.2f %12.2f %12.2f %8.2fx\n", n, s.p50, p.p50, s.p95, p.p95,
                    ratio);
        std::fflush(stdout);

        // The rule: the first size at which parallel is 15% ahead, and every
        // larger size has to keep it.
        if (ratio >= 1.15) {
            if (crossover == 0) crossover = n;
        } else if (crossover != 0) {
            held = false;
        }
    }

    print_load("after");
    std::printf("\n# crossover: ");
    if (crossover == 0)
        std::printf("none in range — the dispatch never pays here\n");
    else if (!held)
        std::printf("%zu, but it did NOT hold at a larger size: noise, not a threshold\n",
                    crossover);
    else
        std::printf("%zu vertices, held at every larger size\n", crossover);
    return 0;
}
