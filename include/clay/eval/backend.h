#pragma once

// Backend interface + runtime registry (evaluation-backends spec).
// One interface, implemented identically everywhere; requests are plain
// flat buffers, the caller owns threading policy and lifetime. The CPU
// backend is always registered; GPU backends register only when their
// runtime is available — availability changes speed, never results.

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "clay/scene/tape.h"

namespace clay {
namespace eval {

enum class Status {
    Ok = 0,
    Unsupported,   // capability not provided by this backend
    InvalidInput,  // bad buffer/dimension in the request
    DeviceError,   // backend runtime failure
};

struct BackendCaps {
    bool device_meshing = false;
    bool fp16_storage = false;
    std::size_t max_tape_instrs = 0;  // 0 = unlimited
};

// Batch field query. points_xyz is count*3 floats (packed xyz). Outputs are
// caller-allocated; gradients/colors may be null when not wanted.
struct PointQuery {
    const float* points_xyz = nullptr;
    std::size_t count = 0;
    float gradient_eps = 1e-4f;  // tetrahedron-tap half-width
};
struct PointResults {
    float* distances = nullptr;      // count
    float* gradients_xyz = nullptr;  // count*3, optional
    float* colors_rgb = nullptr;     // count*3, optional
};

// Dense grid evaluation (the brick cache's fill primitive): nx*ny*nz lattice
// samples at origin + spacing * (i,j,k), x-fastest.
struct GridQuery {
    kernel::cfloat3 origin = kernel::cf3(0, 0, 0);
    float spacing = 1.0f;
    int nx = 0, ny = 0, nz = 0;
};

// Rays as count*6 floats (origin xyz, dir xyz, dir normalized).
struct RayQuery {
    const float* rays = nullptr;
    std::size_t count = 0;
    float tmin = 0.0f;
    float tmax = 1e9f;
    float eps = 1e-4f;   // pixel-proportional acceptance
    int max_steps = 256;
};
struct RayHit {
    std::int32_t hit = 0;
    float t = 0.0f;
    float pos[3] = {};
    float normal[3] = {};
};

class Backend {
  public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    virtual BackendCaps caps() const = 0;

    virtual Status eval_points(const scene::Tape& tape, const PointQuery& q,
                               const PointResults& out) = 0;
    virtual Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                             float* out_colors_rgb = nullptr) = 0;
    virtual Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) = 0;

    // Device meshing lands with the meshing group; Unsupported by default.
    virtual Status mesh(const scene::Tape&, const GridQuery&, std::vector<float>*,
                        std::vector<std::uint32_t>*) {
        return Status::Unsupported;
    }
};

// Registry — CPU is registered on first access, GPU backends when compiled
// in and their runtime initializes.
class Registry {
  public:
    static Registry& instance();
    void add(std::unique_ptr<Backend> backend);
    Backend* find(std::string_view name);
    std::vector<Backend*> all();

  private:
    Registry();
    std::vector<std::unique_ptr<Backend>> backends_;
};

// Single-threaded scalar reference evaluation — CPU scalar defines
// correctness; the parity suite compares every backend (including the CPU
// batch path) against this.
void eval_points_reference(const scene::Tape& tape, const PointQuery& q,
                           const PointResults& out);

}  // namespace eval
}  // namespace clay
