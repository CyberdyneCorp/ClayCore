// CPU backend: the always-available reference implementation. Scalar
// reference path defines correctness; the batch path processes contiguous
// chunks per thread (block-friendly inner loops the compiler can vectorize;
// hand-tuned SIMD lanes are a benchmark-driven follow-up in task 10.6).

#include <cstring>

#include "clay/eval/backend.h"
#include "clay/kernel/field.h"
#include "clay/math/geom.h"
#include "thread_pool.h"

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
        if (!q.points_xyz || !out.distances) return Status::InvalidInput;
        backends_cpu::ThreadPool::instance().parallel_for(
            q.count, 256, [&](std::size_t b, std::size_t e) {
                PointQuery sub = q;
                sub.points_xyz = q.points_xyz + b * 3;
                sub.count = e - b;
                PointResults sub_out;
                sub_out.distances = out.distances + b;
                sub_out.gradients_xyz = out.gradients_xyz ? out.gradients_xyz + b * 3 : nullptr;
                sub_out.colors_rgb = out.colors_rgb ? out.colors_rgb + b * 3 : nullptr;
                eval_points_reference(tape, sub, sub_out);
            });
        return Status::Ok;
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        std::size_t nxy = static_cast<std::size_t>(q.nx) * static_cast<std::size_t>(q.ny);
        backends_cpu::ThreadPool::instance().parallel_for(
            static_cast<std::size_t>(q.nz), 1, [&](std::size_t z0, std::size_t z1) {
                for (std::size_t z = z0; z < z1; ++z) {
                    for (int y = 0; y < q.ny; ++y) {
                        std::size_t row = z * nxy + static_cast<std::size_t>(y) * q.nx;
                        for (int x = 0; x < q.nx; ++x) {
                            cfloat3 p = q.origin +
                                        cf3(static_cast<float>(x), static_cast<float>(y),
                                            static_cast<float>(z)) *
                                            q.spacing;
                            CTapeValue v = tape.eval(p);
                            out_values[row + x] = v.d;
                            if (out_colors_rgb) {
                                std::size_t c = (row + x) * 3;
                                out_colors_rgb[c] = v.color.x;
                                out_colors_rgb[c + 1] = v.color.y;
                                out_colors_rgb[c + 2] = v.color.z;
                            }
                        }
                    }
                }
            });
        return Status::Ok;
    }

    Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) override {
        if (!q.rays || !hits) return Status::InvalidInput;
        float step_scale = tape.safe_step_scale();
        backends_cpu::ThreadPool::instance().parallel_for(
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

Registry::Registry() {
    backends_.push_back(std::make_unique<CpuBackend>());
#if defined(CLAY_HAS_METAL)
    if (auto metal = create_metal_backend()) backends_.push_back(std::move(metal));
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

}  // namespace eval
}  // namespace clay
