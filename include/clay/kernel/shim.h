#pragma once

// Kernel dialect shim — the ONLY header allowed to know which backend is
// compiling. Every other kernel header uses the CLAY_* macros and c* types
// defined here and must satisfy the dialect rules enforced by
// tools/check_kernel_dialect.py: no virtuals, no exceptions, no allocation,
// no recursion, no standard headers beyond the allowlist.
//
// Backend selection macros (exactly one active):
//   CLAY_KERNEL_CPU     scalar/SIMD host C++ (the correctness reference)
//   CLAY_KERNEL_METAL   compiled as MSL
//   CLAY_KERNEL_CUDA    compiled as CUDA device code
//   CLAY_KERNEL_OPENCL  compiled as OpenCL C-compatible subset

#if !defined(CLAY_KERNEL_CPU) && !defined(CLAY_KERNEL_METAL) && \
    !defined(CLAY_KERNEL_CUDA) && !defined(CLAY_KERNEL_OPENCL)
#define CLAY_KERNEL_CPU 1
#endif

#if defined(CLAY_KERNEL_CUDA)
#define CLAY_FN __device__ inline
#elif defined(CLAY_KERNEL_METAL)
#define CLAY_FN inline
#elif defined(CLAY_KERNEL_OPENCL)
#define CLAY_FN inline
#else
#define CLAY_FN inline
#endif

// Vector/scalar types (cfloat3, cfloat4x4, ...) land with task 2.1, mapped
// per backend: Apple simd / scalar structs (CPU), MSL vectors (Metal),
// CUDA vectors, OpenCL vectors.
