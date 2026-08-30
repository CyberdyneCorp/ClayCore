#pragma once

// Backend interface + runtime registry (evaluation-backends spec).
// One interface, implemented identically everywhere; requests are plain
// flat buffers, the caller owns threading policy and lifetime. The CPU
// backend is always registered; GPU backends register only when their
// runtime is available — availability changes speed, never results.

#include <cstddef>
#include <memory>
#include <string>
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

    // What this backend can actually run. Default true, so a backend that
    // provides everything says nothing: these exist for the backend that
    // provides MOST of it.
    //
    // A GPU runtime builds one pipeline per kernel and they can fail
    // independently — on Apple Paravirtual GPUs `clay_raycast` would not
    // compile while the evaluation kernels did (#63). Requiring all of them to
    // register threw away a backend that could accelerate everything the C ABI
    // actually routes to it, and left `clay_list_backends` answering `cpu`,
    // which is what a build with no Metal at all answers.
    //
    // An operation reported false here returns Status::Unsupported when
    // called, which is the refusal a caller already handles for mesh().
    // Batched operations are deliberately absent: the base class loops over
    // the single-item form with identical results, so an unavailable batch
    // kernel costs submission overhead rather than capability.
    bool eval_points = true;
    bool eval_grid = true;
    bool raycast = true;

    // Whether this backend can move bytes between host memory and a slice of a
    // buffer the CALLER owns (`write_device_buffer` / `read_device_buffer`).
    //
    // FALSE by default, unlike the three above: it is meaningful only on a
    // backend bound to a caller-supplied device, and the backends that create
    // their own have no caller buffer to address. Reported rather than probed
    // because the device-destination brick refill has to decide BEFORE it
    // writes anything whether it can keep seeds at all — a decision taken half
    // way through would leave part of the batch resumed and part not, with no
    // way back (issue #345).
    bool device_copy = false;
};

// Why a compiled-in backend is not in the registry, or is in it without all of
// its operations. Empty for a backend that registered with everything working.
//
// This is the half of the registry a host cannot otherwise see. `find("metal")`
// returning nullptr means "no Metal here" and says nothing about whether this
// build HAS Metal, so a host on a machine whose GPU was rejected cannot tell
// that from a CPU-only build, and cannot report it to anyone who could fix it.
//
// Recorded by the backend itself while it initializes, so the runtime's own
// words survive — a pipeline that failed to compile contributes the compiler's
// log, which is the only text that identifies the cause.
void report_backend_unavailable(std::string_view name, std::string why);

// The recorded reason, or an empty string when there is none. Forces registry
// construction first: before anything has tried to register, every backend has
// an empty reason for the uninteresting reason that nobody has looked.
std::string backend_diagnostic(std::string_view name);

// Whether this build contains the named backend at all, however it fared at
// runtime. "Not compiled in" and "compiled in and rejected" are different
// answers to a host deciding whether to file a bug or buy a machine.
bool backend_compiled_in(std::string_view name);

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

// A batch of same-shape lattices, each with its OWN tape — the brick refill
// shape: per-brick culled tapes over per-brick origins, one dims/spacing for
// the whole batch. Grid i fills out_values[i * nx*ny*nz ...] and, when colours
// are wanted, out_colors_rgb[i * nx*ny*nz * 3 ...].
struct GridBatchQuery {
    const scene::Tape* const* tapes = nullptr;  // count tapes, none null
    const kernel::cfloat3* origins = nullptr;   // count lattice origins
    float spacing = 1.0f;
    int nx = 0, ny = 0, nz = 0;
    std::size_t count = 0;
};

// A batch of point runs, each with its OWN tape — the brick-mesh attribute
// shape: per-brick culled tapes over the vertices each brick owns. Run i
// covers the points at indices [offsets[i], offsets[i+1]) of points_xyz and
// writes the same slice of the outputs, so offsets has count+1 entries and
// the outputs are indexed exactly like the points. Runs may be empty.
struct PointBatchQuery {
    const scene::Tape* const* tapes = nullptr;  // count tapes, none null
    const std::size_t* offsets = nullptr;       // count+1, non-decreasing
    const float* points_xyz = nullptr;          // packed xyz, offsets[count]*3 floats
    std::size_t count = 0;                      // number of runs
    float gradient_eps = 1e-4f;                 // tetrahedron-tap half-width
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

    // `out` says what is WANTED, and a null buffer means "not this one". A
    // caller after only the gradient leaves `distances` null and does not pay
    // for the walk that fills it -- which is a fifth of a gradient, since the
    // four taps need neither the distance nor the colour. Asking for nothing at
    // all is refused.
    virtual Status eval_points(const scene::Tape& tape, const PointQuery& q,
                               const PointResults& out) = 0;
    virtual Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                             float* out_colors_rgb = nullptr) = 0;
    virtual Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) = 0;

    // Many small grids in ONE call — what a brick refill is. The default runs
    // eval_grid per entry, so every backend answers and answers identically;
    // a GPU backend overrides it to evaluate the whole batch in one device
    // submission, because a per-brick round trip (command buffer + wait per
    // 8^3 lattice) costs more than the evaluation it carries (issue #64).
    virtual Status eval_grid_batch(const GridBatchQuery& q, float* out_values,
                                   float* out_colors_rgb = nullptr) {
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.origins || !out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0)
            return Status::InvalidInput;
        const std::size_t per = static_cast<std::size_t>(q.nx) *
                                static_cast<std::size_t>(q.ny) *
                                static_cast<std::size_t>(q.nz);
        for (std::size_t i = 0; i < q.count; ++i) {
            if (!q.tapes[i]) return Status::InvalidInput;
            GridQuery g;
            g.origin = q.origins[i];
            g.spacing = q.spacing;
            g.nx = q.nx;
            g.ny = q.ny;
            g.nz = q.nz;
            const Status s = eval_grid(*q.tapes[i], g, out_values + i * per,
                                       out_colors_rgb ? out_colors_rgb + i * per * 3 : nullptr);
            if (s != Status::Ok) return s;
        }
        return Status::Ok;
    }

    // Many point runs, each against its own tape, in ONE call — what the
    // brick-mesh attribute pass is. The default runs eval_points per run, so
    // every backend answers and answers identically; the CPU backend
    // overrides it to dispatch the flattened batch across its thread pool,
    // because a per-run barrier leaves most cores idle on runs of a few
    // hundred vertices. Per-point results are independent of how the batch
    // is split, so the override is value-identical to this default.
    virtual Status eval_points_batch(const PointBatchQuery& q, const PointResults& out) {
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.offsets || !q.points_xyz || !out.distances)
            return Status::InvalidInput;
        for (std::size_t i = 0; i < q.count; ++i) {
            if (!q.tapes[i] || q.offsets[i] > q.offsets[i + 1]) return Status::InvalidInput;
            PointQuery sub;
            sub.points_xyz = q.points_xyz + q.offsets[i] * 3;
            sub.count = q.offsets[i + 1] - q.offsets[i];
            sub.gradient_eps = q.gradient_eps;
            if (sub.count == 0) continue;
            PointResults slice;
            slice.distances = out.distances + q.offsets[i];
            slice.gradients_xyz =
                out.gradients_xyz ? out.gradients_xyz + q.offsets[i] * 3 : nullptr;
            slice.colors_rgb = out.colors_rgb ? out.colors_rgb + q.offsets[i] * 3 : nullptr;
            const Status s = eval_points(*q.tapes[i], sub, slice);
            if (s != Status::Ok) return s;
        }
        return Status::Ok;
    }

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

    // eval_grid_batch whose destination is a buffer the CALLER owns — the
    // brick-refill shape with a device destination. Grid i lands at
    // values.offset + i * nx*ny*nz * sizeof(float) and, when colours are
    // wanted, at colors.offset + i * nx*ny*nz * 3 * sizeof(float): the same
    // fixed slots the host-memory batch uses, so a whole refill lands in the
    // allocation the caller will draw from. The default runs eval_grid_device
    // per grid, so any backend that serves the per-grid device path answers
    // the batch — and answers it identically, because per-grid results are
    // independent of how the batch is split; a GPU backend overrides it to
    // evaluate the whole batch in one device submission, because a command
    // buffer + wait per 8^3 lattice costs more than the samples it carries
    // (issue #64, which the host-memory batch fixed first).
    virtual Status eval_grid_batch_device(const GridBatchQuery& q, const DeviceBuffer& values,
                                          const DeviceBuffer& colors) {
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.origins || values.empty() || q.nx <= 0 || q.ny <= 0 || q.nz <= 0)
            return Status::InvalidInput;
        const std::uint64_t per = static_cast<std::uint64_t>(q.nx) *
                                  static_cast<std::uint64_t>(q.ny) *
                                  static_cast<std::uint64_t>(q.nz);
        // Checked for the whole batch before any grid runs, so an undersized
        // destination is refused with nothing written rather than partially
        // filled.
        if (values.size < per * sizeof(float) * q.count) return Status::InvalidInput;
        const bool want_colors = !colors.empty();
        if (want_colors && colors.size < per * 3 * sizeof(float) * q.count)
            return Status::InvalidInput;
        for (std::size_t i = 0; i < q.count; ++i) {
            if (!q.tapes[i]) return Status::InvalidInput;
            GridQuery g;
            g.origin = q.origins[i];
            g.spacing = q.spacing;
            g.nx = q.nx;
            g.ny = q.ny;
            g.nz = q.nz;
            DeviceBuffer slot = values;
            slot.offset = values.offset + i * per * sizeof(float);
            slot.size = per * sizeof(float);
            DeviceBuffer color_slot;
            if (want_colors) {
                color_slot = colors;
                color_slot.offset = colors.offset + i * per * 3 * sizeof(float);
                color_slot.size = per * 3 * sizeof(float);
            }
            const Status s = eval_grid_device(*q.tapes[i], g, slot, color_slot);
            if (s != Status::Ok) return s;
        }
        return Status::Ok;
    }

    // Host memory into a slice of a buffer the CALLER owns, and back.
    //
    // What these exist for is the resumable brick refill (#306) reaching the
    // device destination (#345). A brick's seed is host-resident float32 and
    // the answer a resumed brick produces is computed on the host, so landing
    // it in the caller's allocation needs a write; a brick that could NOT be
    // resumed is evaluated on the device, and its result has to come back to
    // become the next dab's seed. Both are a few kilobytes a brick against a
    // full walk measured in milliseconds.
    //
    // NOT a general memory service, and deliberately not one: `bytes` from
    // `src`'s start, no strides, no format conversion, and the transfer has
    // COMPLETED when the call returns — the same rule every other device entry
    // point here follows. Only a backend bound to a caller-supplied device can
    // serve them, so the default is Unsupported and `caps().device_copy` says
    // which those are, before a caller commits to a plan that needs them.
    virtual Status write_device_buffer(const DeviceBuffer&, const void*, std::uint64_t) {
        return Status::Unsupported;
    }
    virtual Status read_device_buffer(void*, const DeviceBuffer&, std::uint64_t) {
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

// The same evaluation, one walk of the tape per BLOCK of points rather than one
// per point. An instruction's parameter header — the inverse transform, the
// scale, the rounding radius, a combine's mode and gate — is decoded once per
// block instead of once per point, which is where a tape evaluation's time
// actually goes: the arithmetic is around 5% of it and the primitive is
// invisible in the cost (`add-cpu-simd-path/design.md`).
//
// Results are BIT-IDENTICAL to `eval_points_reference`, not merely within
// tolerance, and the test suite asserts identity rather than a bound. This is a
// second implementation of the same walk, so identity is the only thing standing
// between it and a slow divergence from the scalar evaluator that defines
// correctness for every backend in the tree.
//
// Control flow is uniform across a block: every branch in `ctape_eval` selects
// on the OPCODE or on the stack depth, never on a point's value, so all points
// in a block take the same path and the stack depth is shared. Instructions that
// cannot hoist anything — a sampled volume gathers per point — still evaluate
// correctly here, they simply win less, so there is no per-tape fallback and no
// opcode this path declines.
//
// `block` is the number of points walked per pass; 0 selects the default. It is
// not a tuning constant to agonise over — everything from 64 up measured within
// 4% — and a brick's 8^3 = 512 points already sits in the flat region.
void eval_points_blocked(const scene::Tape& tape, const PointQuery& q, const PointResults& out,
                         std::size_t block = 0);

// The same walk, CONTINUED from a value already computed at each point.
//
// `suffix` must come from `scene::compile_layer_suffix`: it holds only the
// instructions for the appended items and expects the accumulator the items in
// front of them left, which `seed` supplies per point. That makes a dab cost
// what the dab adds rather than what the document holds -- one spread over 12
// bricks at a 0.05 voxel is 0.23 ms at 200 items and 18.07 ms at 50,000, and
// almost all of the difference is history being re-evaluated unchanged (#306).
//
// NOT AN APPROXIMATION. The instructions are the ones a full walk would have
// run, in the same order, over the same floats; only the part already folded is
// represented by the number it produced. Given an exact seed the result is
// bit-identical to evaluating the whole document, which is what
// `test_suffix_tape.cpp` asserts.
//
// `seed` holds `q.count` distances, one per point of `q`, in the same order.
//
// COLOUR is carried when `out.colors_rgb` is set AND `seed_rgb` is given --
// three floats a point, the colour the prefix reached. What the accumulator IS
// decides what a seed must carry: a coloured walk folds a CTapeValue, so a
// caller asking for colour back without supplying it would fold every combine
// against black, and this returns distances only rather than doing that.
//
// `out.gradients_xyz` is ignored either way: a gradient is four taps of the
// whole field, not something a single accumulator can be continued into.
//
// IN PLACE IS ALLOWED, and callers depend on it. `seed` MAY be `out.distances`
// and `seed_rgb` MAY be `out.colors_rgb` -- the same buffer, seeded on entry
// and holding the answer on exit. The blocked walk reads a block's seed into
// its own stack before it writes that block's result, which is what makes it
// safe, and this is a CONTRACT rather than an accident of the current code: the
// C ABI's resumed brick refill copies each seed out of the document's seed
// store into the buffer the walk will answer into, so that it can drop the
// cache lock before evaluating (#348), and the single-layer case copies into
// the host's own output slot. Hoisting the output store above the seed load, or
// marking either pointer `restrict`, would break that caller silently -- the
// values would be wrong, not the addresses -- so `test_suffix_tape.cpp` asserts
// the property directly on a buffer that is its own seed.
void eval_points_seeded(const scene::Tape& suffix, const PointQuery& q, const float* seed,
                        const float* seed_rgb, const PointResults& out, std::size_t block = 0);

// eval_points_seeded for a walk that starts holding SEVERAL values.
//
// A resume that picks up inside a group has more than one open chain:
// compile_group emits a group's combine after its children, so a prefix that
// stops there leaves one value per open group plus one on the stack, and the
// suffix's own combines pop them in order. `seeds` is `levels` planes of
// `q.count` distances, plane 0 the BOTTOM of the stack; `seeds_rgb` is the
// same in three floats a point. One seed can only continue a chain with
// nothing open above it, which is every append at a layer's root list and no
// append inside a group.
//
// Refuses (writing nothing) when `levels` is 0 or deeper than the tape stack.
// `stack_out` asks for the stack as it stood at instruction `snapshot_at`,
// which is what the NEXT append resumes from: the seed is where the checkpoint
// sits, not the answer the walk ends with, and evaluating the prefix again to
// find it would give back what the resume saved. `kTapeStackAtEnd` takes it
// after the last instruction.
void eval_points_seeded_stack(const scene::Tape& suffix, const PointQuery& q, const float* seeds,
                              const float* seeds_rgb, std::size_t levels,
                              const PointResults& out, float* stack_out = nullptr,
                              float* stack_out_rgb = nullptr,
                              std::size_t* stack_out_levels = nullptr,
                              std::size_t snapshot_at = static_cast<std::size_t>(-1),
                              std::size_t block = 0);

// The other half: evaluate `tape` and hand back the WHOLE final stack rather
// than only its top, which is what a prefix stopping inside a group is worth.
//
// `stack_out` receives `*out_levels` planes of `q.count` distances, plane 0 the
// bottom; `stack_out_rgb`, when given, the same in three floats a point. The
// caller sizes both for `tape_stack_depth(tape)` planes, which is the most
// that can be written. `*out_levels` is 0 for an empty tape, which has no
// stack and writes nothing.
//
// `out`, when given, receives the field the same walk produces. The walk
// computes it either way -- the top of the stack IS the answer -- so a caller
// that needs both SHALL ask for both here rather than walking twice.
void eval_points_stack(const scene::Tape& tape, const PointQuery& q, float* stack_out,
                       float* stack_out_rgb, std::size_t* out_levels,
                       std::size_t snapshot_at = static_cast<std::size_t>(-1),
                       std::size_t block = 0, const PointResults* out = nullptr);

// The stack depth a tape actually reaches, which is a property of its
// instruction sequence rather than of any point. The blocked path allocates
// `block * depth` values, so a tape of depth 2 — which a flat chain of stamps is
// — costs a fraction of what CLAY_TAPE_MAX_STACK would.
std::size_t tape_stack_depth(const scene::Tape& tape);

}  // namespace eval
}  // namespace clay
