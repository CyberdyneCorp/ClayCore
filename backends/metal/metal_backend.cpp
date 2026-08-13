// Metal backend host — pure C++ via metal-cpp, no Objective-C. Loads the
// embedded metallib (compiled at build time from the shared kernel headers)
// and dispatches the fixed tape-interpreter kernels.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <cstdio>
#include <string>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>
#include <TargetConditionals.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "clay/eval/backend.h"
#include "metal_shared.h"

#include "../common/grid_mesh.h"

extern "C" {
extern const unsigned char clay_metallib[];
extern const unsigned int clay_metallib_size;
}

namespace clay {
namespace eval {

namespace {

class MetalBackend final : public Backend {
  public:
    static std::unique_ptr<MetalBackend> create() {
        auto b = std::unique_ptr<MetalBackend>(new MetalBackend());
        return b->init() ? std::move(b) : nullptr;
    }

    // Bound to a device the CALLER owns. The pipelines and the library are
    // still ours and are still built and released here; only the device and
    // the command queue are borrowed.
    static std::unique_ptr<MetalBackend> adopt(const DeviceHandles& d) {
        auto b = std::unique_ptr<MetalBackend>(new MetalBackend());
        return b->adopt_init(d) ? std::move(b) : nullptr;
    }

    ~MetalBackend() override {
        for (MTL::Buffer* b : scratch_)
            if (b) b->release();
        for (ResidentTape& t : resident_) release_resident(t);
        if (pso_points_) pso_points_->release();
        if (pso_grid_) pso_grid_->release();
        if (pso_grid_batch_) pso_grid_batch_->release();
        if (pso_rays_) pso_rays_->release();
        // Release nothing we did not retain. On an adopted device the caller
        // keeps its device and queue, and releasing them here would drop a
        // reference the host still holds.
        if (owns_device_) {
            if (queue_) queue_->release();
            if (device_) device_->release();
        }
    }

    const char* name() const override { return "metal"; }
    // Pipelines fail independently, so what this backend can do is a fact about
    // this device rather than about the code — on Apple Paravirtual GPUs the
    // raycast kernel would not compile while the evaluation kernels did (#63).
    // Registration needs the two core pipelines; the rest is reported here.
    BackendCaps caps() const override {
        BackendCaps c{false, true, 0};
        c.eval_points = pso_points_ != nullptr;
        c.eval_grid = pso_grid_ != nullptr;
        c.raycast = pso_rays_ != nullptr;
        return c;
    }

    Status eval_points(const scene::Tape& tape, const PointQuery& q,
                       const PointResults& out) override {
        if (!q.points_xyz || !out.distances) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;
        std::lock_guard<std::mutex> lock(mutex_);

        ClayEvalUniforms u{};
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.point_count = static_cast<unsigned int>(q.count);
        u.has_colors = out.colors_rgb ? 1u : 0u;

        MTL::Buffer* pts = copy_in(kSlotIn, q.points_xyz, q.count * 3 * sizeof(float));
        MTL::Buffer* dist = scratch(kSlotValues, q.count * sizeof(float));
        MTL::Buffer* cols =
            scratch(kSlotColors, out.colors_rgb ? q.count * 3 * sizeof(float) : 0);

        TapeBuffers tb = upload_tape(tape);
        bool ok = dispatch(pso_points_, {tb.instrs, tb.params, tb.blob, pts, dist, cols},
                           &u, sizeof(u), 6, q.count);
        if (ok) {
            std::memcpy(out.distances, dist->contents(), q.count * sizeof(float));
            if (out.colors_rgb)
                std::memcpy(out.colors_rgb, cols->contents(), q.count * 3 * sizeof(float));
            if (out.gradients_xyz) gradients_from_taps(tape, q, out);
        }
        return ok ? Status::Ok : Status::DeviceError;
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        std::size_t total = static_cast<std::size_t>(q.nx) * q.ny * q.nz;
        std::lock_guard<std::mutex> lock(mutex_);

        ClayGridUniforms u{};
        u.origin[0] = q.origin.x;
        u.origin[1] = q.origin.y;
        u.origin[2] = q.origin.z;
        u.spacing = q.spacing;
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.nx = static_cast<unsigned int>(q.nx);
        u.ny = static_cast<unsigned int>(q.ny);
        u.nz = static_cast<unsigned int>(q.nz);
        u.has_colors = out_colors_rgb ? 1u : 0u;

        MTL::Buffer* dist = scratch(kSlotValues, total * sizeof(float));
        MTL::Buffer* cols =
            scratch(kSlotColors, out_colors_rgb ? total * 3 * sizeof(float) : 0);
        TapeBuffers tb = upload_tape(tape);
        bool ok = dispatch(pso_grid_, {tb.instrs, tb.params, tb.blob, dist, cols}, &u,
                           sizeof(u), 5, total);
        if (ok) {
            std::memcpy(out_values, dist->contents(), total * sizeof(float));
            if (out_colors_rgb)
                std::memcpy(out_colors_rgb, cols->contents(), total * 3 * sizeof(float));
        }
        return ok ? Status::Ok : Status::DeviceError;
    }

    // The whole batch in ONE command buffer: the per-grid culled tapes ride
    // concatenated in three shared buffers, a table says where each grid's
    // slices start, and there is one dispatch, one wait and one readback. The
    // per-grid loop the default implementation runs pays a command buffer and
    // a waitUntilCompleted per 8^3 lattice — roughly 250 us of round trip for
    // 512 samples of work — which is what made refill 7-10x slower on Metal
    // than on the CPU at every batch size (issue #64).
    Status eval_grid_batch(const GridBatchQuery& q, float* out_values,
                           float* out_colors_rgb) override {
        // A batch kernel that would not build costs submission overhead, not
        // capability: the base class loops over eval_grid for identical
        // values. So this falls back rather than refusing, and caps() does not
        // report a batch operation at all.
        if (!pso_grid_batch_)
            return Backend::eval_grid_batch(q, out_values, out_colors_rgb);
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.origins || !out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0)
            return Status::InvalidInput;
        const std::size_t per = static_cast<std::size_t>(q.nx) *
                                static_cast<std::size_t>(q.ny) *
                                static_cast<std::size_t>(q.nz);
        const std::size_t total = per * q.count;
        std::lock_guard<std::mutex> lock(mutex_);

        BatchUpload up = upload_batch(q);
        if (!up.ok) return Status::InvalidInput;
        MTL::Buffer* dist = scratch(kSlotValues, total * sizeof(float));
        MTL::Buffer* cols = scratch(kSlotColors, out_colors_rgb ? total * 3 * sizeof(float) : 0);

        const ClayGridBatchUniforms u = batch_uniforms(q, out_colors_rgb != nullptr);
        bool ok = dispatch(pso_grid_batch_, {up.instrs, up.params, up.blob, up.table, dist, cols},
                           &u, sizeof(u), 6, total);
        if (ok) {
            std::memcpy(out_values, dist->contents(), total * sizeof(float));
            if (out_colors_rgb)
                std::memcpy(out_colors_rgb, cols->contents(), total * 3 * sizeof(float));
        }
        return ok ? Status::Ok : Status::DeviceError;
    }

    // eval_grid_batch's treatment for the caller's own MTLBuffer: the same
    // concatenated tapes, the same table, the same kernel and ONE dispatch —
    // only the destination binding differs, so the values are bit-identical
    // to the host-memory batch and nothing crosses host memory on the way
    // out. The per-grid loop the default runs pays a command buffer and a
    // waitUntilCompleted per 8^3 lattice, which is what left the zero-copy
    // path 25-165x behind the host-memory one it exists to beat.
    Status eval_grid_batch_device(const GridBatchQuery& q, const DeviceBuffer& values,
                                  const DeviceBuffer& colors) override {
        // Only an ADOPTED backend can serve this: an MTLBuffer belongs to the
        // device that made it.
        if (owns_device_) return Status::Unsupported;
        if (q.count == 0) return Status::Ok;
        if (!q.tapes || !q.origins || values.empty() || q.nx <= 0 || q.ny <= 0 || q.nz <= 0)
            return Status::InvalidInput;
        const std::size_t per = static_cast<std::size_t>(q.nx) *
                                static_cast<std::size_t>(q.ny) *
                                static_cast<std::size_t>(q.nz);
        const std::size_t total = per * q.count;
        // Checked before any dispatch, so an undersized destination is refused
        // with nothing written rather than partially filled.
        if (values.size < static_cast<std::uint64_t>(total) * sizeof(float))
            return Status::InvalidInput;
        const bool want_colors = !colors.empty();
        if (want_colors && colors.size < static_cast<std::uint64_t>(total) * 3 * sizeof(float))
            return Status::InvalidInput;
        std::lock_guard<std::mutex> lock(mutex_);

        BatchUpload up = upload_batch(q);
        if (!up.ok) return Status::InvalidInput;
        const ClayGridBatchUniforms u = batch_uniforms(q, want_colors);
        const std::vector<Bound> bound = {
            Bound{up.instrs, 0}, Bound{up.params, 0}, Bound{up.blob, 0}, Bound{up.table, 0},
            Bound{static_cast<MTL::Buffer*>(values.handle),
                  static_cast<NS::UInteger>(values.offset)},
            want_colors ? Bound{static_cast<MTL::Buffer*>(colors.handle),
                                static_cast<NS::UInteger>(colors.offset)}
                        // Binding 5 must be a real buffer even when no
                        // colours were asked for.
                        : Bound{scratch(kSlotColors, 0), 0},
        };
        const bool ok = dispatch_bound(pso_grid_batch_, bound, &u, sizeof(u), 6, total);
        // dispatch waits until completed, so the work has landed here and
        // nothing is left in flight on the caller's queue.
        return ok ? Status::Ok : Status::DeviceError;
    }

    // Results land in the caller's own MTLBuffer, so a host that was going to
    // draw them never copies them through host memory. Same shader, same tape,
    // same uniforms as eval_grid — only the destination differs.
    Status eval_grid_device(const scene::Tape& tape, const GridQuery& q,
                            const DeviceBuffer& values, const DeviceBuffer& colors) override {
        // Only an ADOPTED backend can serve this: an MTLBuffer belongs to the
        // device that made it.
        if (owns_device_) return Status::Unsupported;
        if (values.empty() || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        const std::size_t total = static_cast<std::size_t>(q.nx) *
                                  static_cast<std::size_t>(q.ny) *
                                  static_cast<std::size_t>(q.nz);
        // Checked before any dispatch, so an undersized destination is refused
        // with nothing written rather than partially filled.
        if (values.size < static_cast<std::uint64_t>(total) * sizeof(float))
            return Status::InvalidInput;
        const bool want_colors = !colors.empty();
        if (want_colors && colors.size < static_cast<std::uint64_t>(total) * 3 * sizeof(float))
            return Status::InvalidInput;
        std::lock_guard<std::mutex> lock(mutex_);

        ClayGridUniforms u{};
        u.origin[0] = q.origin.x;
        u.origin[1] = q.origin.y;
        u.origin[2] = q.origin.z;
        u.spacing = q.spacing;
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.nx = static_cast<unsigned int>(q.nx);
        u.ny = static_cast<unsigned int>(q.ny);
        u.nz = static_cast<unsigned int>(q.nz);
        u.has_colors = want_colors ? 1u : 0u;

        TapeBuffers tb = upload_tape(tape);
        const std::vector<Bound> bound = {
            Bound{tb.instrs, 0}, Bound{tb.params, 0}, Bound{tb.blob, 0},
            Bound{static_cast<MTL::Buffer*>(values.handle),
                  static_cast<NS::UInteger>(values.offset)},
            want_colors ? Bound{static_cast<MTL::Buffer*>(colors.handle),
                                static_cast<NS::UInteger>(colors.offset)}
                        // Binding 4 must be a real buffer even when no
                        // colours were asked for.
                        : Bound{scratch(kSlotColors, 0), 0},
        };
        const bool ok = dispatch_bound(pso_grid_, bound, &u, sizeof(u), 5, total);
        // dispatch waits until completed, so the work has landed here and
        // nothing is left in flight on the caller's queue.
        return ok ? Status::Ok : Status::DeviceError;
    }

    Status mesh(const scene::Tape& tape, const GridQuery& q, std::vector<float>* out_verts,
                std::vector<std::uint32_t>* out_indices) override {
        // hybrid: field values on the device, triangulation on the host
        return grid_mesh(*this, tape, q, out_verts, out_indices);
    }

    Status raycast(const scene::Tape& tape, const RayQuery& q, RayHit* hits) override {
        // The refusal a caller already handles for mesh(), rather than a
        // backend that is absent entirely. Checked before the argument
        // validation so a device that cannot raycast says so for every call,
        // not only for well-formed ones.
        if (!pso_rays_) return Status::Unsupported;
        if (!q.rays || !hits) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;
        static_assert(sizeof(RayHit) == sizeof(ClayRayHitGpu));
        std::lock_guard<std::mutex> lock(mutex_);

        ClayRayUniforms u{};
        u.instr_count = static_cast<unsigned int>(tape.instrs.size());
        u.ray_count = static_cast<unsigned int>(q.count);
        u.max_steps = static_cast<unsigned int>(q.max_steps);
        u.tmin = q.tmin;
        u.tmax = q.tmax;
        u.eps = q.eps;
        u.step_scale = tape.safe_step_scale();
        if (!tape.bounds.empty() && !tape.bounds.is_infinite()) {
            math::Aabb b = tape.bounds.dilated(0.01f);
            u.has_bounds = 1;
            u.bounds_min[0] = b.min.x;
            u.bounds_min[1] = b.min.y;
            u.bounds_min[2] = b.min.z;
            u.bounds_max[0] = b.max.x;
            u.bounds_max[1] = b.max.y;
            u.bounds_max[2] = b.max.z;
        }

        MTL::Buffer* rays = copy_in(kSlotIn, q.rays, q.count * 6 * sizeof(float));
        MTL::Buffer* out = scratch(kSlotValues, q.count * sizeof(ClayRayHitGpu));
        TapeBuffers tb = upload_tape(tape);
        bool ok = dispatch(pso_rays_, {tb.instrs, tb.params, tb.blob, rays, out}, &u,
                           sizeof(u), 5, q.count);
        if (ok) std::memcpy(hits, out->contents(), q.count * sizeof(ClayRayHitGpu));
        return ok ? Status::Ok : Status::DeviceError;
    }

  private:
    MetalBackend() = default;

    struct TapeBuffers {
        MTL::Buffer* instrs;
        MTL::Buffer* params;
        MTL::Buffer* blob;
    };

    bool init() {
        device_ = MTL::CreateSystemDefaultDevice();
        if (!device_) return false;
        if (!build_pipelines()) return false;
        queue_ = device_->newCommandQueue();
        return queue_ != nullptr;
    }

    bool adopt_init(const DeviceHandles& d) {
        device_ = static_cast<MTL::Device*>(d.handles[0]);
        queue_ = static_cast<MTL::CommandQueue*>(d.handles[1]);
        owns_device_ = false;
        if (!device_ || !queue_) {
            device_ = nullptr;  // so the destructor touches nothing
            queue_ = nullptr;
            return false;
        }
        return build_pipelines();
    }

    // Every failure below used to discard the NS::Error it was handed, so a
    // backend that could not initialise registered nothing and said nothing —
    // indistinguishable from a machine with no GPU. That cost three release
    // attempts to diagnose: the artifact was fine and the reason was sitting
    // in an ignored out-parameter. Metal init happens once, so one line on
    // stderr is cheap and is the difference between a mystery and a message.
    static void report(const char* what, NS::Error* err) {
        const char* detail = "no error reported";
        if (err && err->localizedDescription())
            detail = err->localizedDescription()->utf8String();
        std::fprintf(stderr, "claycore: the Metal backend will not register — %s: %s\n", what,
                     detail);
        // And into the registry, so it reaches a HOST rather than only whoever
        // is watching stderr. A host on a paravirtualised machine sees
        // clay_list_backends answer "cpu" — the same answer a build without
        // Metal gives — and had no way to tell those apart or to report this
        // to anyone who could fix it. The compiler's own log goes in whole:
        // it is the text that identifies the cause, and summarising it here
        // would discard the thing worth forwarding.
        std::string full = std::string(what) + ": " + detail;
        if (err && err->description())
            full += std::string("\nfull Metal error: ") + err->description()->utf8String();
        report_backend_unavailable("metal", std::move(full));
        // localizedDescription for a pipeline failure is the useless half:
        // "Compilation failed" and nothing about WHAT failed. The compiler's
        // own log is in the error's userInfo, and description() prints the
        // whole object including it. Without this the message names a symptom
        // and leaves the cause a guess — which is how the paravirtual failure
        // survived three release attempts.
        if (err && err->description())
            std::fprintf(stderr, "claycore: full Metal error: %s\n",
                         err->description()->utf8String());
    }

    bool build_pipelines() {
        dispatch_data_t data = dispatch_data_create(clay_metallib, clay_metallib_size, nullptr,
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* err = nullptr;
        MTL::Library* lib = device_->newLibrary(data, &err);
        dispatch_release(data);
        if (!lib) {
            report("the embedded metallib did not load", err);
            return false;
        }
        pso_points_ = make_pso(lib, "clay_eval_points");
        pso_grid_ = make_pso(lib, "clay_eval_grid");
        pso_grid_batch_ = make_pso(lib, "clay_eval_grid_batch");
        pso_rays_ = make_pso(lib, "clay_raycast");
        lib->release();

        // The CORE pipelines, and only those. A device that evaluates points
        // and grids accelerates everything the C ABI routes here — both
        // clay_raycast and clay_raycast_many ask the registry for "cpu"
        // explicitly — so requiring every pipeline threw a working backend
        // away over a kernel no ABI entry point can call (#63).
        //
        // Requiring all four was also getting wider rather than narrower:
        // 0.29.1's batched refill added pso_grid_batch_ to the conjunction,
        // which is the "next kernel that fails on any device" the issue
        // predicted.
        if (!pso_points_ || !pso_grid_) return false;

        // Registered, but say what is missing. A partial backend that reported
        // nothing would be the same silence one level down.
        if (!pso_rays_)
            report_backend_unavailable(
                "metal",
                "registered WITHOUT raycast: no compute pipeline for clay_raycast. "
                "Point and grid evaluation run on the GPU; Backend::raycast returns "
                "Unsupported and clay_raycast is unaffected, since it evaluates on the "
                "CPU regardless. See the full Metal error above for the compiler's log.");
        else if (!pso_grid_batch_)
            report_backend_unavailable(
                "metal",
                "registered WITHOUT the batched grid kernel: no compute pipeline for "
                "clay_eval_grid_batch. Values are identical — the batch falls back to a "
                "loop over eval_grid — but a brick refill pays one dispatch per lattice.");
        return true;
    }

    MTL::ComputePipelineState* make_pso(MTL::Library* lib, const char* fn_name) {
        NS::String* name = NS::String::string(fn_name, NS::UTF8StringEncoding);
        MTL::Function* fn = lib->newFunction(name);
        if (!fn) {
            std::fprintf(stderr,
                         "claycore: the Metal backend will not register — the metallib "
                         "carries no function named %s\n",
                         fn_name);
            return nullptr;
        }
        NS::Error* err = nullptr;
        MTL::ComputePipelineState* pso = device_->newComputePipelineState(fn, &err);
        fn->release();
        if (!pso) {
            std::string what = std::string("no compute pipeline for ") + fn_name;
            report(what.c_str(), err);
        }
        return pso;
    }

    // -- persistent scratch pool ---------------------------------------------
    //
    // One reusable buffer per binding role, grown in place and never shrunk:
    // a steady stream of dispatches allocates on the first call (or on a new
    // high-water mark) and never again. Allocating five buffers per call was
    // most of what a small dispatch cost. Reuse is safe because every submit
    // waits until completed before returning and mutex_ serializes calls, so
    // no buffer is read by the device after this call returns. Slots are
    // never zero-sized: every kernel binding must be a real buffer, and an
    // empty tape section still gets bound.
    enum Slot : int {
        kSlotIn = 0,       // points / rays in
        kSlotValues,       // distances / ray hits out
        kSlotColors,       // colours out, or the 4-byte placeholder binding
        kSlotBatchInstrs,  // concatenated batch tape sections + table
        kSlotBatchParams,
        kSlotBatchBlob,
        kSlotBatchTable,
        kSlotTapeInstrs,  // single tape without an identity (compile_id 0)
        kSlotTapeParams,
        kSlotTapeBlob,
        kSlotCount,
    };

    MTL::Buffer* scratch(Slot slot, std::size_t bytes) {
        MTL::Buffer*& b = scratch_[slot];
        if (bytes < 4) bytes = 4;
        if (!b || b->length() < bytes) {
            if (b) b->release();
            b = device_->newBuffer(bytes, MTL::ResourceStorageModeShared);
        }
        return b;
    }

    MTL::Buffer* copy_in(Slot slot, const void* data, std::size_t bytes) {
        MTL::Buffer* b = scratch(slot, bytes);
        if (bytes) std::memcpy(b->contents(), data, bytes);
        return b;
    }

    // -- resident uploaded tapes ---------------------------------------------
    //
    // The uploaded form of the last few identified tapes, keyed on
    // Tape::compile_id — process-unique per compile, so an id hit IS a
    // content hit and no bytes need comparing or hashing (which would cost
    // what the upload costs, linear in a consolidated volume's blob). A
    // document being sculpted re-evaluates one cached whole-document tape
    // between edits; a handful of entries covers that plus a pick tape and a
    // layer tape without building a cache framework. Tapes without an
    // identity (compile_id 0, hand-assembled) go through the scratch slots
    // and are re-uploaded per call, exactly as before.
    struct ResidentTape {
        std::uint64_t id = 0;
        std::uint64_t last_use = 0;
        MTL::Buffer* instrs = nullptr;
        MTL::Buffer* params = nullptr;
        MTL::Buffer* blob = nullptr;
    };

    void release_resident(ResidentTape& t) {
        if (t.instrs) t.instrs->release();
        if (t.params) t.params->release();
        if (t.blob) t.blob->release();
        t = ResidentTape{};
    }

    // An exact-size shared buffer holding a copy of one tape section; never
    // zero-sized, because it gets bound.
    MTL::Buffer* upload_section(const void* data, std::size_t bytes) {
        if (bytes == 0) return device_->newBuffer(4, MTL::ResourceStorageModeShared);
        return device_->newBuffer(data, bytes, MTL::ResourceStorageModeShared);
    }

    TapeBuffers upload_tape(const scene::Tape& tape) {
        const std::size_t instr_bytes = tape.instrs.size() * sizeof(kernel::CTapeInstr);
        const std::size_t param_bytes = tape.params.size() * sizeof(float);
        const std::size_t blob_bytes = tape.blob.size() * sizeof(float);
        if (tape.compile_id == 0)
            return TapeBuffers{copy_in(kSlotTapeInstrs, tape.instrs.data(), instr_bytes),
                               copy_in(kSlotTapeParams, tape.params.data(), param_bytes),
                               copy_in(kSlotTapeBlob, tape.blob.data(), blob_bytes)};
        ResidentTape* entry = &resident_[0];
        for (ResidentTape& t : resident_) {
            if (t.id == tape.compile_id) {
                t.last_use = ++use_counter_;
                return TapeBuffers{t.instrs, t.params, t.blob};
            }
            if (t.last_use < entry->last_use) entry = &t;  // oldest = eviction victim
        }
        release_resident(*entry);
        entry->id = tape.compile_id;
        entry->last_use = ++use_counter_;
        entry->instrs = upload_section(tape.instrs.data(), instr_bytes);
        entry->params = upload_section(tape.params.data(), param_bytes);
        entry->blob = upload_section(tape.blob.data(), blob_bytes);
        return TapeBuffers{entry->instrs, entry->params, entry->blob};
    }

    // A batch's per-grid tapes concatenated into three shared buffers, plus
    // the table that says where each grid's slices start — the upload half of
    // the clay_eval_grid_batch kernel, shared by the host-memory and
    // device-destination batch paths so the two cannot drift. The buffers are
    // the pooled scratch slots — borrowed for the call, not owned. ok is
    // false when the batch is refused (a null tape, or tapes whose
    // concatenation overflows the table's 32-bit device offsets) and then no
    // slot was touched.
    struct BatchUpload {
        MTL::Buffer* instrs = nullptr;
        MTL::Buffer* params = nullptr;
        MTL::Buffer* blob = nullptr;
        MTL::Buffer* table = nullptr;
        bool ok = false;
    };

    BatchUpload upload_batch(const GridBatchQuery& q) {
        BatchUpload up;
        // First pass: sizes, so the concatenated buffers are allocated once.
        // The table offsets are 32-bit on the device, so a batch whose tapes
        // do not fit is refused rather than wrapped.
        std::uint64_t n_instrs = 0, n_params = 0, n_blob = 0;
        for (std::size_t i = 0; i < q.count; ++i) {
            if (!q.tapes[i]) return up;
            n_instrs += q.tapes[i]->instrs.size();
            n_params += q.tapes[i]->params.size();
            n_blob += q.tapes[i]->blob.size();
        }
        const std::uint64_t limit = 0xffffffffu;
        if (n_instrs > limit || n_params > limit || n_blob > limit) return up;

        up.instrs = scratch(kSlotBatchInstrs, n_instrs * sizeof(kernel::CTapeInstr));
        up.params = scratch(kSlotBatchParams, n_params * sizeof(float));
        up.blob = scratch(kSlotBatchBlob, n_blob * sizeof(float));
        up.table = scratch(kSlotBatchTable, q.count * sizeof(ClayBatchGrid));

        // Second pass: each tape's slices land at their offsets, and the table
        // records where.
        auto* out_instrs = static_cast<kernel::CTapeInstr*>(up.instrs->contents());
        auto* out_params = static_cast<float*>(up.params->contents());
        auto* out_blob = static_cast<float*>(up.blob->contents());
        auto* out_table = static_cast<ClayBatchGrid*>(up.table->contents());
        std::uint32_t at_instr = 0, at_param = 0, at_blob = 0;
        for (std::size_t i = 0; i < q.count; ++i) {
            const scene::Tape& t = *q.tapes[i];
            if (!t.instrs.empty())
                std::memcpy(out_instrs + at_instr, t.instrs.data(),
                            t.instrs.size() * sizeof(kernel::CTapeInstr));
            if (!t.params.empty())
                std::memcpy(out_params + at_param, t.params.data(),
                            t.params.size() * sizeof(float));
            if (!t.blob.empty())
                std::memcpy(out_blob + at_blob, t.blob.data(), t.blob.size() * sizeof(float));
            ClayBatchGrid g{};
            g.origin[0] = q.origins[i].x;
            g.origin[1] = q.origins[i].y;
            g.origin[2] = q.origins[i].z;
            g.instr_offset = at_instr;
            g.instr_count = static_cast<unsigned int>(t.instrs.size());
            g.param_offset = at_param;
            g.blob_offset = at_blob;
            out_table[i] = g;
            at_instr += static_cast<std::uint32_t>(t.instrs.size());
            at_param += static_cast<std::uint32_t>(t.params.size());
            at_blob += static_cast<std::uint32_t>(t.blob.size());
        }
        up.ok = true;
        return up;
    }

    static ClayGridBatchUniforms batch_uniforms(const GridBatchQuery& q, bool has_colors) {
        ClayGridBatchUniforms u{};
        u.spacing = q.spacing;
        u.nx = static_cast<unsigned int>(q.nx);
        u.ny = static_cast<unsigned int>(q.ny);
        u.nz = static_cast<unsigned int>(q.nz);
        u.grid_count = static_cast<unsigned int>(q.count);
        u.has_colors = has_colors ? 1u : 0u;
        return u;
    }

    // A buffer and where in it to start. Offsets matter only on the device
    // path, where a host packs many results into one allocation.
    struct Bound {
        MTL::Buffer* buffer = nullptr;
        NS::UInteger offset = 0;
    };

    bool dispatch(MTL::ComputePipelineState* pso, std::initializer_list<MTL::Buffer*> buffers,
                  const void* uniforms, std::size_t uniform_bytes, unsigned uniform_index,
                  std::size_t thread_count) {
        std::vector<Bound> bound;
        bound.reserve(buffers.size());
        for (MTL::Buffer* b : buffers) bound.push_back(Bound{b, 0});
        return dispatch_bound(pso, bound, uniforms, uniform_bytes, uniform_index, thread_count);
    }

    bool dispatch_bound(MTL::ComputePipelineState* pso, const std::vector<Bound>& buffers,
                        const void* uniforms, std::size_t uniform_bytes,
                        unsigned uniform_index, std::size_t thread_count) {
        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        if (!cmd) return false;
        MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
        enc->setComputePipelineState(pso);
        unsigned idx = 0;
        for (const Bound& b : buffers) enc->setBuffer(b.buffer, b.offset, idx++);
        enc->setBytes(uniforms, uniform_bytes, uniform_index);
        MTL::Size grid(thread_count, 1, 1);
        NS::UInteger tg = pso->maxTotalThreadsPerThreadgroup();
        if (tg > 64) tg = 64;
        enc->dispatchThreads(grid, MTL::Size(tg, 1, 1));
        enc->endEncoding();
        cmd->commit();
        // A dab-sized dispatch finishes in tens of microseconds, but parking
        // this thread on waitUntilCompleted's semaphore notices that hundreds
        // of microseconds later — the wakeup is most of what an interactive
        // 1-brick call pays (measured ~150 us polled vs ~520 us parked on an
        // M2 Max). So on macOS, for small dispatches, poll the status for up
        // to ~2 ms of wall time before parking. Large dispatches run for
        // milliseconds and park immediately: spinning there burns a core for
        // no latency anyone can see.
        //
        // macOS ONLY. An iPad's CPU and GPU share one power budget, so the
        // spinning core starves the kernel it is waiting on: the same small
        // dispatch that polls in 150 us on an M2 Max ran 3.1x SLOWER on an
        // iPad Air M3 (sdf_stamp_metal 1.77 -> 5.54 ms p95), caught by the
        // device gate. On iOS the thread parks immediately, as it always
        // did. The call is synchronous either way; only how completion is
        // noticed changes.
#if TARGET_OS_OSX
        constexpr std::size_t kSpinThreads = 64 * 512;  // ~a refill chunk's worth
        if (thread_count <= kSpinThreads) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            do {
                for (int poll = 0; poll < 64; ++poll) {
                    const MTL::CommandBufferStatus s = cmd->status();
                    if (s == MTL::CommandBufferStatusCompleted) return true;
                    if (s == MTL::CommandBufferStatusError) return false;
                }
            } while (std::chrono::steady_clock::now() < deadline);
        }
#endif
        cmd->waitUntilCompleted();
        return cmd->status() == MTL::CommandBufferStatusCompleted;
    }

    // Gradients on the host from 4 extra device evaluations would need a
    // second dispatch; for now derive them CPU-side from the tape (rarely
    // requested on the GPU path — brick fills don't need them).
    void gradients_from_taps(const scene::Tape& tape, const PointQuery& q,
                             const PointResults& out) {
        PointResults grad_only;
        grad_only.gradients_xyz = out.gradients_xyz;
        PointQuery sub = q;
        std::vector<float> tmp(q.count);
        grad_only.distances = tmp.data();
        eval_points_reference(tape, sub, grad_only);
    }

    // Serializes the public entry points: the scratch pool and the resident
    // tapes are shared mutable state, and the device work is synchronous and
    // one queue deep anyway, so serializing costs concurrent callers nothing
    // they were not already paying at the queue.
    std::mutex mutex_;
    MTL::Buffer* scratch_[kSlotCount] = {};
    static constexpr int kResidentTapes = 4;
    ResidentTape resident_[kResidentTapes];
    std::uint64_t use_counter_ = 0;

    MTL::Device* device_ = nullptr;
    MTL::CommandQueue* queue_ = nullptr;
    // False on an adopted device: the destructor must not release what the
    // caller lent us, and eval_grid_device is only meaningful when the buffers
    // and the device came from the same place.
    bool owns_device_ = true;
    MTL::ComputePipelineState* pso_points_ = nullptr;
    MTL::ComputePipelineState* pso_grid_ = nullptr;
    MTL::ComputePipelineState* pso_grid_batch_ = nullptr;
    MTL::ComputePipelineState* pso_rays_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> create_metal_backend() { return MetalBackend::create(); }

std::unique_ptr<Backend> adopt_metal_backend(const DeviceHandles& device) {
    return MetalBackend::adopt(device);
}

}  // namespace eval
}  // namespace clay
