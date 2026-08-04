// OpenCL backend (evaluation-backends spec, tier 3 / best-effort).
//
// Kernels come from the amalgamated kernel-header text (tools/amalgamate_cl.py)
// compiled at runtime with -DCLAY_KERNEL_OPENCL, so the math is the same
// single source every other backend compiles. Scope is eval_points and
// eval_grid (the brick-fill primitive); raycast and device meshing report
// Unsupported and fall back to CPU — the tier-3 contract is that a missing
// capability never changes results, only speed.
//
// Targets OpenCL 3.0 but builds with -cl-std=CL1.2, the common subset the
// kernels actually use, so 1.2-only runtimes (Apple, older ICDs) work too.

#if defined(__APPLE__)
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "clay/eval/backend.h"

extern "C" {
extern const char clay_cl_source[];
extern const unsigned int clay_cl_source_size;
}

namespace clay {
namespace eval {

namespace {

class OpenClBackend final : public Backend {
  public:
    static std::unique_ptr<OpenClBackend> create() {
        auto b = std::unique_ptr<OpenClBackend>(new OpenClBackend());
        return b->init() ? std::move(b) : nullptr;
    }

    ~OpenClBackend() override {
        if (kernel_points_) clReleaseKernel(kernel_points_);
        if (kernel_grid_) clReleaseKernel(kernel_grid_);
        if (program_) clReleaseProgram(program_);
        if (queue_) clReleaseCommandQueue(queue_);
        if (context_) clReleaseContext(context_);
    }

    const char* name() const override { return "opencl"; }
    BackendCaps caps() const override { return BackendCaps{false, false, 0}; }

    Status eval_points(const scene::Tape& tape, const PointQuery& q,
                       const PointResults& out) override {
        if (!q.points_xyz || !out.distances) return Status::InvalidInput;
        if (q.count == 0) return Status::Ok;

        TapeBuffers tb;
        if (!upload_tape(tape, &tb)) return Status::DeviceError;
        cl_int err = CL_SUCCESS;
        cl_mem pts = buffer_in(q.points_xyz, q.count * 3 * sizeof(float), &err);
        cl_mem dist = buffer_out(q.count * sizeof(float), &err);
        cl_mem cols = buffer_out((out.colors_rgb ? q.count * 3 : 1) * sizeof(float), &err);
        if (err != CL_SUCCESS) {
            release({tb.instrs, tb.params, tb.blob, pts, dist, cols});
            return Status::DeviceError;
        }

        unsigned int instr_count = static_cast<unsigned int>(tape.instrs.size());
        unsigned int count = static_cast<unsigned int>(q.count);
        unsigned int has_colors = out.colors_rgb ? 1u : 0u;
        int a = 0;
        err |= clSetKernelArg(kernel_points_, a++, sizeof(cl_mem), &tb.instrs);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(cl_mem), &tb.params);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(cl_mem), &tb.blob);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(cl_mem), &pts);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(cl_mem), &dist);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(cl_mem), &cols);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(unsigned int), &instr_count);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(unsigned int), &count);
        err |= clSetKernelArg(kernel_points_, a++, sizeof(unsigned int), &has_colors);

        if (err == CL_SUCCESS) err = run(kernel_points_, q.count);
        if (err == CL_SUCCESS)
            err = clEnqueueReadBuffer(queue_, dist, CL_TRUE, 0, q.count * sizeof(float),
                                      out.distances, 0, nullptr, nullptr);
        if (err == CL_SUCCESS && out.colors_rgb)
            err = clEnqueueReadBuffer(queue_, cols, CL_TRUE, 0, q.count * 3 * sizeof(float),
                                      out.colors_rgb, 0, nullptr, nullptr);
        release({tb.instrs, tb.params, tb.blob, pts, dist, cols});
        if (err != CL_SUCCESS) return Status::DeviceError;

        // gradients are a host-side tetrahedron tap on the same tape
        if (out.gradients_xyz) {
            PointResults grad = out;
            std::vector<float> tmp(q.count);
            grad.distances = tmp.data();
            grad.colors_rgb = nullptr;
            eval_points_reference(tape, q, grad);
        }
        return Status::Ok;
    }

    Status eval_grid(const scene::Tape& tape, const GridQuery& q, float* out_values,
                     float* out_colors_rgb) override {
        if (!out_values || q.nx <= 0 || q.ny <= 0 || q.nz <= 0) return Status::InvalidInput;
        std::size_t total = static_cast<std::size_t>(q.nx) * q.ny * q.nz;

        TapeBuffers tb;
        if (!upload_tape(tape, &tb)) return Status::DeviceError;
        cl_int err = CL_SUCCESS;
        cl_mem dist = buffer_out(total * sizeof(float), &err);
        cl_mem cols = buffer_out((out_colors_rgb ? total * 3 : 1) * sizeof(float), &err);
        if (err != CL_SUCCESS) {
            release({tb.instrs, tb.params, tb.blob, dist, cols});
            return Status::DeviceError;
        }

        unsigned int instr_count = static_cast<unsigned int>(tape.instrs.size());
        unsigned int nx = static_cast<unsigned int>(q.nx);
        unsigned int ny = static_cast<unsigned int>(q.ny);
        unsigned int nz = static_cast<unsigned int>(q.nz);
        unsigned int has_colors = out_colors_rgb ? 1u : 0u;
        int a = 0;
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(cl_mem), &tb.instrs);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(cl_mem), &tb.params);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(cl_mem), &tb.blob);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(cl_mem), &dist);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(cl_mem), &cols);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(float), &q.origin.x);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(float), &q.origin.y);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(float), &q.origin.z);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(float), &q.spacing);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(unsigned int), &instr_count);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(unsigned int), &nx);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(unsigned int), &ny);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(unsigned int), &nz);
        err |= clSetKernelArg(kernel_grid_, a++, sizeof(unsigned int), &has_colors);

        if (err == CL_SUCCESS) err = run(kernel_grid_, total);
        if (err == CL_SUCCESS)
            err = clEnqueueReadBuffer(queue_, dist, CL_TRUE, 0, total * sizeof(float),
                                      out_values, 0, nullptr, nullptr);
        if (err == CL_SUCCESS && out_colors_rgb)
            err = clEnqueueReadBuffer(queue_, cols, CL_TRUE, 0, total * 3 * sizeof(float),
                                      out_colors_rgb, 0, nullptr, nullptr);
        release({tb.instrs, tb.params, tb.blob, dist, cols});
        return err == CL_SUCCESS ? Status::Ok : Status::DeviceError;
    }

    // Sphere tracing lives in templated C++ (field.h) that OpenCL C cannot
    // compile; callers fall back to another backend.
    Status raycast(const scene::Tape&, const RayQuery&, RayHit*) override {
        return Status::Unsupported;
    }

  private:
    OpenClBackend() = default;

    struct TapeBuffers {
        cl_mem instrs = nullptr;
        cl_mem params = nullptr;
        cl_mem blob = nullptr;
    };

    bool init() {
        cl_uint platform_count = 0;
        if (clGetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS || platform_count == 0)
            return false;
        std::vector<cl_platform_id> platforms(platform_count);
        clGetPlatformIDs(platform_count, platforms.data(), nullptr);

        for (cl_platform_id platform : platforms) {
            cl_device_id device = nullptr;
            cl_uint device_count = 0;
            if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, &device_count) !=
                    CL_SUCCESS ||
                device_count == 0) {
                if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &device_count) !=
                        CL_SUCCESS ||
                    device_count == 0)
                    continue;
            }
            cl_int err = CL_SUCCESS;
            context_ = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
            if (err != CL_SUCCESS) continue;
            device_ = device;
#if defined(CL_VERSION_2_0)
            queue_ = clCreateCommandQueueWithProperties(context_, device_, nullptr, &err);
#else
            queue_ = clCreateCommandQueue(context_, device_, 0, &err);
#endif
            if (err != CL_SUCCESS || !build_program()) {
                cleanup_partial();
                continue;
            }
            return true;
        }
        return false;
    }

    void cleanup_partial() {
        if (program_) clReleaseProgram(program_);
        if (queue_) clReleaseCommandQueue(queue_);
        if (context_) clReleaseContext(context_);
        program_ = nullptr;
        queue_ = nullptr;
        context_ = nullptr;
    }

    bool build_program() {
        const char* src = clay_cl_source;
        std::size_t len = clay_cl_source_size;
        cl_int err = CL_SUCCESS;
        program_ = clCreateProgramWithSource(context_, 1, &src, &len, &err);
        if (err != CL_SUCCESS) return false;
        // CL1.2 is the subset the kernels use; -cl-fp32-correctly-rounded is
        // deliberately NOT requested (parity tolerance covers fp differences)
        const char* options = "-cl-std=CL1.2 -DCLAY_KERNEL_OPENCL=1";
        if (clBuildProgram(program_, 1, &device_, options, nullptr, nullptr) != CL_SUCCESS) {
            // Registration failing silently once cost real debugging time:
            // surface the device compiler's log so the cause is visible.
            std::size_t log_size = 0;
            clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
            std::vector<char> log(log_size + 1, '\0');
            clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, log_size, log.data(),
                                  nullptr);
            std::fprintf(stderr, "claycore: OpenCL kernel build failed:\n%s\n", log.data());
            return false;
        }
        kernel_points_ = clCreateKernel(program_, "clay_eval_points", &err);
        if (err != CL_SUCCESS) return false;
        kernel_grid_ = clCreateKernel(program_, "clay_eval_grid", &err);
        return err == CL_SUCCESS;
    }

    cl_mem buffer_in(const void* data, std::size_t bytes, cl_int* err) {
        cl_int e = CL_SUCCESS;
        cl_mem m = clCreateBuffer(context_, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  bytes ? bytes : 4, bytes ? const_cast<void*>(data) : &pad_,
                                  &e);
        if (e != CL_SUCCESS) *err = e;
        return m;
    }

    cl_mem buffer_out(std::size_t bytes, cl_int* err) {
        cl_int e = CL_SUCCESS;
        cl_mem m = clCreateBuffer(context_, CL_MEM_WRITE_ONLY, bytes ? bytes : 4, nullptr, &e);
        if (e != CL_SUCCESS) *err = e;
        return m;
    }

    bool upload_tape(const scene::Tape& tape, TapeBuffers* out) {
        cl_int err = CL_SUCCESS;
        out->instrs = buffer_in(tape.instrs.data(),
                                tape.instrs.size() * sizeof(kernel::CTapeInstr), &err);
        out->params = buffer_in(tape.params.data(), tape.params.size() * sizeof(float), &err);
        out->blob = buffer_in(tape.blob.data(), tape.blob.size() * sizeof(float), &err);
        return err == CL_SUCCESS;
    }

    cl_int run(cl_kernel k, std::size_t work_items) {
        std::size_t local = 64;
        std::size_t max_group = 0;
        clGetKernelWorkGroupInfo(k, device_, CL_KERNEL_WORK_GROUP_SIZE, sizeof(max_group),
                                 &max_group, nullptr);
        if (max_group && local > max_group) local = max_group;
        std::size_t global = ((work_items + local - 1) / local) * local;
        cl_int err = clEnqueueNDRangeKernel(queue_, k, 1, nullptr, &global, &local, 0, nullptr,
                                           nullptr);
        if (err != CL_SUCCESS) return err;
        return clFinish(queue_);
    }

    void release(std::initializer_list<cl_mem> buffers) {
        for (cl_mem m : buffers)
            if (m) clReleaseMemObject(m);
    }

    cl_context context_ = nullptr;
    cl_device_id device_ = nullptr;
    cl_command_queue queue_ = nullptr;
    cl_program program_ = nullptr;
    cl_kernel kernel_points_ = nullptr;
    cl_kernel kernel_grid_ = nullptr;
    float pad_ = 0.0f;
};

}  // namespace

std::unique_ptr<Backend> create_opencl_backend() { return OpenClBackend::create(); }

}  // namespace eval
}  // namespace clay
