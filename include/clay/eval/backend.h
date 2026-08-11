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

// -- device interop -----------------------------------------------------------
//
// Backends create and own their devices, which is right for a headless library
// and wrong for a host that was going to draw on a GPU anyway: every brick and
// every mesh crosses host memory on its way from our device to theirs. These
// types let a caller lend us the device it already has.
//
// The ownership rule, which is what keeps this from becoming a source of
// crashes inside someone else's driver:
//
//   * The library retains nothing it did not create and destroys nothing it did
//     not make. A Device holds BORROWED handles.
//   * The library creates, destroys and waits on no synchronization primitive
//     belonging to the caller, and submits to a supplied queue only inside a
//     call the caller made.
//   * Work issued during a call has COMPLETED when that call returns. Nothing
//     is left in flight with no way for the caller to know when it lands.
//   * Calls on one Device are the CALLER's to serialize, exactly as they are
//     for BrickCache. A GPU queue is not free-threaded and pretending otherwise
//     here would be a threading policy the consumer did not ask for.

enum class DeviceApi { Metal, Vulkan, Cuda };

// Borrowed native handles, positional per API so no vendor header reaches this
// one. The C boundary documents the positions; they are:
//   Metal:  0 id<MTLDevice>, 1 id<MTLCommandQueue>
//   Vulkan: 0 VkInstance, 1 VkPhysicalDevice, 2 VkDevice, 3 VkQueue (+ family)
//   Cuda:   0 CUcontext, 1 CUstream
struct DeviceHandles {
    DeviceApi api = DeviceApi::Vulkan;
    void* handles[6] = {};
    std::uint32_t queue_family = 0;  // Vulkan only
};

// A slice of a buffer the CALLER owns and keeps owning. `handle` is a VkBuffer
// / MTLBuffer / CUdeviceptr per the device's API.
struct DeviceBuffer {
    void* handle = nullptr;
    std::uint64_t offset = 0;  // bytes
    std::uint64_t size = 0;    // bytes available from offset
    bool empty() const { return handle == nullptr; }
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

    // eval_grid whose destination is a buffer the CALLER owns, so results a
    // consumer intends to draw from are produced in the memory it will draw
    // from. Only a backend bound to a caller-supplied device can serve this —
    // a buffer from one device is meaningless on another — so the default is
    // Unsupported and no backend is forced to implement it.
    //
    // Values are 32-bit floats and are NOT quantized on the device, even though
    // the brick cache stores fp16 and a host uploading an r16float atlas wants
    // fp16: quantization and band classification are BrickCache::submit's, and
    // a device path that did them would be a second implementation of the step
    // most able to drift. A host taking this path owns the conversion.
    virtual Status eval_grid_device(const scene::Tape&, const GridQuery&, const DeviceBuffer&,
                                    const DeviceBuffer&) {
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

// A backend bound to a device the CALLER owns, as an instance the caller holds
// — NOT a registry entry, because two hosts with two devices cannot share one
// process-wide slot under one name. Registration keeps meaning exactly what it
// means today and is unaffected by any device a caller supplies.
//
// Returns nullptr when the named backend has no adoption path, when its runtime
// is not compiled in, or when the handles are not usable (a Vulkan queue family
// without compute, an incomplete handle set). A caller whose adoption fails
// falls back to Registry::find(name) and gets IDENTICAL values: adoption
// changes where work runs, never what it computes.
std::unique_ptr<Backend> make_backend(std::string_view name, const DeviceHandles& device);

// Single-threaded scalar reference evaluation — CPU scalar defines
// correctness; the parity suite compares every backend (including the CPU
// batch path) against this.
void eval_points_reference(const scene::Tape& tape, const PointQuery& q,
                           const PointResults& out);

}  // namespace eval
}  // namespace clay
