// CPU backend: the always-available reference implementation. Scalar
// reference path defines correctness; the batch path processes contiguous
// chunks per thread (block-friendly inner loops the compiler can vectorize;
// hand-tuned SIMD lanes are a benchmark-driven follow-up in task 10.6).

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>
#include <string>

#include "clay/eval/backend.h"
#include "clay/kernel/field.h"
#include "clay/math/geom.h"

#include "../common/grid_mesh.h"
#include "clay/parallel/thread_pool.h"

namespace clay {
namespace eval {

using kernel::cf3;
using kernel::cfloat3;
using kernel::CTapeValue;

void eval_points_reference(const scene::Tape& tape, const PointQuery& q,
                           const PointResults& out) {
    for (std::size_t i = 0; i < q.count; ++i) {
        cfloat3 p = cf3(q.points_xyz[i * 3], q.points_xyz[i * 3 + 1], q.points_xyz[i * 3 + 2]);
        CTapeValue v = tape.eval(p);
        if (out.distances) out.distances[i] = v.d;
        if (out.colors_rgb) {
            out.colors_rgb[i * 3] = v.color.x;
            out.colors_rgb[i * 3 + 1] = v.color.y;
            out.colors_rgb[i * 3 + 2] = v.color.z;
        }
        if (out.gradients_xyz) {
            cfloat3 n = kernel::cnormal([&](cfloat3 s) { return tape.eval(s).d; }, p,
                                        q.gradient_eps);
            out.gradients_xyz[i * 3] = n.x;
            out.gradients_xyz[i * 3 + 1] = n.y;
            out.gradients_xyz[i * 3 + 2] = n.z;
        }
    }
}

namespace {

class CpuBackend final : public Backend {
  public:
    const char* name() const override { return "cpu"; }
    BackendCaps caps() const override { return BackendCaps{false, false, 0}; }

    Status eval_points(const scene::Tape& tape, const PointQuery& q,
                       const PointResults& out) override {
        // Not `!out.distances`: a caller may want only the gradient, or only
        // the colour, and requiring a distance buffer would make it pay for a
        // walk it does not read.
        if (!q.points_xyz) return Status::InvalidInput;
        if (!out.distances && !out.gradients_xyz && !out.colors_rgb) return Status::InvalidInput;
        parallel::ThreadPool::instance().parallel_for(
            q.count, 256, [&](std::size_t b, std::size_t e) {
                PointQuery sub = q;
                sub.points_xyz = q.points_xyz + b * 3;
                sub.count = e - b;
                PointResults sub_out;
                sub_out.distances = out.distances ? out.distances + b : nullptr;
                sub_out.gradients_xyz = out.gradients_xyz ? out.gradients_xyz + b * 3 : nullptr;
                sub_out.colors_rgb = out.colors_rgb ? out.colors_rgb + b * 3 : nullptr;
                eval_points_blocked(tape, sub, sub_out);
            });
        return Status::Ok;
    }

    // The flattened batch goes to the pool as ONE parallel_for over every
    // point of every run: a chunk that spans a run boundary walks the runs it
    // covers. The default's per-run dispatch barriers between runs, and an
    // attribute pass hands over dozens of runs of a few hundred points each —
    // too few per run to occupy the pool, plenty across the batch. Each point
    // is evaluated by exactly one chunk with the same reference arithmetic,
    // so the results match the default bit for bit.
    Status eval_points_batch(const PointBatchQuery& q, const PointResults& out) override {
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.offsets || !q.points_xyz) return Status::InvalidInput;
        if (!out.distances && !out.gradients_xyz && !out.colors_rgb) return Status::InvalidInput;
        for (std::size_t i = 0; i < q.count; ++i)
            if (!q.tapes[i] || q.offsets[i] > q.offsets[i + 1]) return Status::InvalidInput;
        const std::size_t first = q.offsets[0];
        parallel::ThreadPool::instance().parallel_for(
            q.offsets[q.count] - first, 64, [&](std::size_t b, std::size_t e) {
                std::size_t at = first + b;
                const std::size_t last = first + e;
                // the first run whose end is past this chunk's start
                std::size_t run = static_cast<std::size_t>(
                    std::upper_bound(q.offsets + 1, q.offsets + q.count + 1, at) -
                    (q.offsets + 1));
                while (at < last) {
                    while (q.offsets[run + 1] <= at) ++run;  // skip empty runs
                    const std::size_t stop = std::min(q.offsets[run + 1], last);
                    PointQuery sub;
                    sub.points_xyz = q.points_xyz + at * 3;
                    sub.count = stop - at;
                    sub.gradient_eps = q.gradient_eps;
                    PointResults slice;
                    slice.distances = out.distances ? out.distances + at : nullptr;
                    slice.gradients_xyz =
                        out.gradients_xyz ? out.gradients_xyz + at * 3 : nullptr;
                    slice.colors_rgb = out.colors_rgb ? out.colors_rgb + at * 3 : nullptr;
                    eval_points_blocked(*q.tapes[run], sub, slice);
                    at = stop;
                }
            });
        return Status::Ok;
    }

    // A RUN OF LATTICE ROWS — the unit both the single-grid and the batched
    // path dispatch in, so the arithmetic and the order of writes are the same
    // whichever one a caller reaches. Rows rather than z-slices because a BRICK
    // is 8 cells across: eight slices cannot occupy more than eight threads,
    // and that was the whole of the batch path's scaling.
    //
    // A RUN of them rather than one, because the tape is now walked once per
    // block of points: a single row is 8 points, which is the worst end of the
    // block-size curve, while a run gathered across rows reaches the flat part.
    // Row `r` writes to `out_values[r * nx ...]`, so a run's outputs are
    // contiguous and no scatter is needed.
    //
    // The point positions are computed by the same expression the scalar walk
    // used, in the same order, so the values are bit-identical to what
    // `tape.eval(p)` per point produced — which the grid/batch parity test and
    // `test_tape_block.cpp` both assert rather than assume.
    static void eval_rows(const scene::Tape& tape, const GridQuery& q, std::size_t r0,
                          std::size_t r1, float* out_values, float* out_colors_rgb) {
        // Thread-local for the same reason the blocked stack is: chunks are as
        // small as one row, and a malloc per row costs more than the row.
        static thread_local std::vector<float> scratch;
        const std::size_t nx = static_cast<std::size_t>(q.nx);
        const std::size_t ny = static_cast<std::size_t>(q.ny);
        const std::size_t count = (r1 - r0) * nx;
        scratch.resize(count * 3);
        for (std::size_t r = r0; r < r1; ++r) {
            const std::size_t z = r / ny;
            const std::size_t y = r % ny;
            for (std::size_t x = 0; x < nx; ++x) {
                const cfloat3 p = q.origin + cf3(static_cast<float>(x), static_cast<float>(y),
                                                 static_cast<float>(z)) *
                                                 q.spacing;
                const std::size_t o = ((r - r0) * nx + x) * 3;
                scratch[o] = p.x;
                scratch[o + 1] = p.y;
                scratch[o + 2] = p.z;
            }
        }
        PointQuery pq{scratch.data(), count, 0.0f};
        PointResults pr;
        pr.distances = out_values + r0 * nx;
        pr.gradients_xyz = nullptr;
        pr.colors_rgb = out_colors_rgb ? out_colors_rgb + r0 * nx * 3 : nullptr;
        eval_points_blocked(tape, pq, pr);
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        const std::size_t rows =
            static_cast<std::size_t>(q.nz) * static_cast<std::size_t>(q.ny);
        parallel::ThreadPool::instance().parallel_for(
            rows, 1, [&](std::size_t r0, std::size_t r1) {
                eval_rows(tape, q, r0, r1, out_values, out_colors_rgb);
            });
        return Status::Ok;
    }

    // MANY SMALL GRIDS IN ONE DISPATCH, which is what a brick refill is.
    //
    // The base class default loops `eval_grid` per brick, and that is what a
    // CPU refill used to do. Two things went wrong with it, and they compound:
    // each brick is its own `parallel_for`, so a 13-brick refill paid thirteen
    // dispatch-and-join barriers; and each of those could only split a brick's
    // 8 z-slices, so at most eight threads had anything to do however many the
    // machine has.
    //
    // Measured on a 24-thread desktop before this override: a stamp on a
    // 50,000-item document spent 165.8 ms in evaluation at 6.73 cores of 16
    // physical, and the share FELL as the document grew — 8.92 at a hundred
    // items down to 6.73 at fifty thousand. Evaluation is 98% of that stamp.
    //
    // One `parallel_for` over every ROW of every brick fixes both: one barrier,
    // and 13 x 8 x 8 = 832 units for the pool to balance instead of 8. The
    // reference arithmetic is untouched — `eval_row` is the same code the
    // single-grid path runs — so results are bit-identical either way, which
    // the batch/grid parity test checks.
    Status eval_grid_batch(const GridBatchQuery& q, float* out_values,
                           float* out_colors_rgb) override {
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.origins || !out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0)
            return Status::InvalidInput;
        for (std::size_t i = 0; i < q.count; ++i)
            if (!q.tapes[i]) return Status::InvalidInput;
        const std::size_t per = static_cast<std::size_t>(q.nx) *
                                static_cast<std::size_t>(q.ny) *
                                static_cast<std::size_t>(q.nz);
        const std::size_t rows_per =
            static_cast<std::size_t>(q.nz) * static_cast<std::size_t>(q.ny);
        parallel::ThreadPool::instance().parallel_for(
            q.count * rows_per, 1, [&](std::size_t b, std::size_t e) {
                // A chunk can span bricks, and each brick has its own tape, so
                // the chunk is walked as runs of rows that share one — which is
                // also what lets each run be a single blocked walk.
                std::size_t i = b;
                while (i < e) {
                    const std::size_t brick = i / rows_per;
                    const std::size_t stop = std::min(e, (brick + 1) * rows_per);
                    GridQuery g;
                    g.origin = q.origins[brick];
                    g.spacing = q.spacing;
                    g.nx = q.nx;
                    g.ny = q.ny;
                    g.nz = q.nz;
                    eval_rows(*q.tapes[brick], g, i - brick * rows_per, stop - brick * rows_per,
                              out_values + brick * per,
                              out_colors_rgb ? out_colors_rgb + brick * per * 3 : nullptr);
                    i = stop;
                }
            });
        return Status::Ok;
    }

    Status mesh(const scene::Tape& tape, const GridQuery& q, std::vector<float>* out_verts,
                std::vector<std::uint32_t>* out_indices) override {
        return grid_mesh(*this, tape, q, out_verts, out_indices);
    }

    Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) override {
        if (!q.rays || !hits) return Status::InvalidInput;
        float step_scale = tape.safe_step_scale();
        parallel::ThreadPool::instance().parallel_for(
            q.count, 16, [&](std::size_t b, std::size_t e) {
                for (std::size_t i = b; i < e; ++i) {
                    cfloat3 ro = cf3(q.rays[i * 6], q.rays[i * 6 + 1], q.rays[i * 6 + 2]);
                    cfloat3 rd = cf3(q.rays[i * 6 + 3], q.rays[i * 6 + 4], q.rays[i * 6 + 5]);
                    hits[i] = trace_one(tape, ro, rd, q, step_scale);
                }
            });
        return Status::Ok;
    }

  private:
    static RayHit trace_one(const scene::Tape& tape, cfloat3 ro, cfloat3 rd, const RayQuery& q,
                            float step_scale) {
        RayHit hit;
        float tmin = q.tmin, tmax = q.tmax;
        // clip to the scene bound (dilated so grazing hits survive)
        if (!tape.bounds.empty() && !tape.bounds.is_infinite()) {
            float t0, t1;
            if (!math::ray_aabb({ro, rd}, tape.bounds.dilated(0.01f), &t0, &t1)) return hit;
            tmin = kernel::cmax(tmin, t0);
            tmax = kernel::cmin(tmax, t1);
        }
        auto field = [&](cfloat3 p) { return tape.eval(p).d; };
        kernel::CRayHit r =
            kernel::craycast(field, ro, rd, tmin, tmax, q.eps, step_scale, 1.4f, q.max_steps);
        if (!r.hit) return hit;
        hit.hit = 1;
        hit.t = r.t;
        cfloat3 pos = ro + rd * r.t;
        cfloat3 n = kernel::cnormal(field, pos, 1e-4f);
        hit.pos[0] = pos.x;
        hit.pos[1] = pos.y;
        hit.pos[2] = pos.z;
        hit.normal[0] = n.x;
        hit.normal[1] = n.y;
        hit.normal[2] = n.z;
        return hit;
    }
};

}  // namespace

#if defined(CLAY_HAS_METAL)
std::unique_ptr<Backend> create_metal_backend();  // backends/metal
#endif
#if defined(CLAY_HAS_CUDA)
std::unique_ptr<Backend> create_cuda_backend();  // backends/cuda
#endif
#if defined(CLAY_HAS_OPENCL)
std::unique_ptr<Backend> create_opencl_backend();  // backends/opencl
#endif
#if defined(CLAY_HAS_VULKAN)
std::unique_ptr<Backend> create_vulkan_backend();  // backends/vulkan
#endif

// The reasons compiled-in backends gave for not registering. Written during
// Registry construction and read afterwards, so a plain map behind the same
// one-time initialization the registry itself has needs no lock of its own:
// every write happens inside the `static Registry reg` construction that
// instance() serializes, and every read is after it.
namespace {
std::map<std::string, std::string, std::less<>>& diagnostics() {
    static std::map<std::string, std::string, std::less<>> map;
    return map;
}

// Compiled in, whatever happened at runtime. The list is the set of backends
// this build could POSSIBLY register, which is what makes "not compiled in"
// answerable rather than inferred from an absence.
constexpr std::string_view kCompiledIn[] = {
    "cpu",
#if defined(CLAY_HAS_METAL)
    "metal",
#endif
#if defined(CLAY_HAS_CUDA)
    "cuda",
#endif
#if defined(CLAY_HAS_OPENCL)
    "opencl",
#endif
#if defined(CLAY_HAS_VULKAN)
    "vulkan",
#endif
};
}  // namespace

void report_backend_unavailable(std::string_view name, std::string why) {
    // Appends rather than replaces. A backend reports twice on the way down —
    // the pipeline that failed, carrying the compiler's log, and then what its
    // absence cost — and the second would otherwise erase the first, which is
    // the half that identifies the cause. Registration happens once per
    // process, so this accumulates a record rather than growing without bound.
    std::string& slot = diagnostics()[std::string(name)];
    if (!slot.empty()) slot += "\n";
    slot += why;
}

std::string backend_diagnostic(std::string_view name) {
    // Before the registry exists nobody has tried, and an empty reason would
    // read as "nothing went wrong" rather than "nothing has happened yet".
    (void)Registry::instance();
    auto it = diagnostics().find(name);
    return it == diagnostics().end() ? std::string() : it->second;
}

bool backend_compiled_in(std::string_view name) {
    for (std::string_view n : kCompiledIn)
        if (n == name) return true;
    return false;
}

Registry::Registry() {
    backends_.push_back(std::make_unique<CpuBackend>());
#if defined(CLAY_HAS_METAL)
    if (auto metal = create_metal_backend()) backends_.push_back(std::move(metal));
#endif
#if defined(CLAY_HAS_CUDA)
    if (auto cuda = create_cuda_backend()) backends_.push_back(std::move(cuda));
#endif
#if defined(CLAY_HAS_OPENCL)
    if (auto opencl = create_opencl_backend()) backends_.push_back(std::move(opencl));
#endif
#if defined(CLAY_HAS_VULKAN)
    if (auto vulkan = create_vulkan_backend()) backends_.push_back(std::move(vulkan));
#endif
}

Registry& Registry::instance() {
    static Registry reg;
    return reg;
}

void Registry::add(std::unique_ptr<Backend> backend) { backends_.push_back(std::move(backend)); }

Backend* Registry::find(std::string_view name) {
    for (auto& b : backends_)
        if (name == b->name()) return b.get();
    return nullptr;
}

std::vector<Backend*> Registry::all() {
    std::vector<Backend*> out;
    for (auto& b : backends_) out.push_back(b.get());
    return out;
}

#if defined(CLAY_HAS_VULKAN)
std::unique_ptr<Backend> adopt_vulkan_backend(const DeviceHandles&);  // backends/vulkan
#endif
#if defined(CLAY_HAS_METAL)
std::unique_ptr<Backend> adopt_metal_backend(const DeviceHandles&);  // backends/metal
#endif

std::unique_ptr<Backend> make_backend(std::string_view name, const DeviceHandles& device) {
    // A device-bound backend is an INSTANCE the caller holds, never a registry
    // entry: two hosts with two devices cannot share one process-wide slot
    // under one name. Registration is untouched by anything here.
#if defined(CLAY_HAS_VULKAN)
    if (name == "vulkan" && device.api == DeviceApi::Vulkan)
        return adopt_vulkan_backend(device);
#endif
#if defined(CLAY_HAS_METAL)
    if (name == "metal" && device.api == DeviceApi::Metal) return adopt_metal_backend(device);
#endif
    // Every other backend, and every mismatch between a name and an API,
    // reports "unsupported" by returning nothing. The caller falls back to the
    // registered backend and gets identical values — adoption changes where
    // work runs, never what it computes.
    (void)name;
    (void)device;
    return nullptr;
}

}  // namespace eval
}  // namespace clay
