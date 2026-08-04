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

// CPU and CUDA share one implementation: plain structs plus C++ operators,
// with libm/CUDA-device math behind the same c* names. Identical arithmetic
// on both sides is exactly what keeps CUDA close to the scalar reference.
#if defined(CLAY_KERNEL_CPU) || defined(CLAY_KERNEL_CUDA)

#include <cmath>
#include <cstdint>

#if defined(CLAY_KERNEL_CUDA)
#define CLAY_FN __host__ __device__ inline
#else
#define CLAY_FN inline
#endif
#define CLAY_THREAD  // address-space qualifier for out-params (MSL: thread)
#define CLAY_DEVICE  // address-space qualifier for buffers (MSL: device)
#define CLAY_NS_BEGIN \
    namespace clay {  \
    namespace kernel {
#define CLAY_NS_END \
    }               \
    }

CLAY_NS_BEGIN

struct cfloat2 {
    float x, y;
};
struct cfloat3 {
    float x, y, z;
};
struct cfloat4 {
    float x, y, z, w;
};

CLAY_FN cfloat2 cf2(float x, float y) { return cfloat2{x, y}; }
CLAY_FN cfloat3 cf3(float x, float y, float z) { return cfloat3{x, y, z}; }
CLAY_FN cfloat4 cf4(float x, float y, float z, float w) { return cfloat4{x, y, z, w}; }

// -- component-wise operators ------------------------------------------------

CLAY_FN cfloat2 operator+(cfloat2 a, cfloat2 b) { return cf2(a.x + b.x, a.y + b.y); }
CLAY_FN cfloat2 operator-(cfloat2 a, cfloat2 b) { return cf2(a.x - b.x, a.y - b.y); }
CLAY_FN cfloat2 operator*(cfloat2 a, cfloat2 b) { return cf2(a.x * b.x, a.y * b.y); }
CLAY_FN cfloat2 operator/(cfloat2 a, cfloat2 b) { return cf2(a.x / b.x, a.y / b.y); }
CLAY_FN cfloat2 operator*(cfloat2 a, float s) { return cf2(a.x * s, a.y * s); }
CLAY_FN cfloat2 operator*(float s, cfloat2 a) { return a * s; }
CLAY_FN cfloat2 operator/(cfloat2 a, float s) { return cf2(a.x / s, a.y / s); }
CLAY_FN cfloat2 operator-(cfloat2 a) { return cf2(-a.x, -a.y); }

CLAY_FN cfloat3 operator+(cfloat3 a, cfloat3 b) { return cf3(a.x + b.x, a.y + b.y, a.z + b.z); }
CLAY_FN cfloat3 operator-(cfloat3 a, cfloat3 b) { return cf3(a.x - b.x, a.y - b.y, a.z - b.z); }
CLAY_FN cfloat3 operator*(cfloat3 a, cfloat3 b) { return cf3(a.x * b.x, a.y * b.y, a.z * b.z); }
CLAY_FN cfloat3 operator/(cfloat3 a, cfloat3 b) { return cf3(a.x / b.x, a.y / b.y, a.z / b.z); }
CLAY_FN cfloat3 operator*(cfloat3 a, float s) { return cf3(a.x * s, a.y * s, a.z * s); }
CLAY_FN cfloat3 operator*(float s, cfloat3 a) { return a * s; }
CLAY_FN cfloat3 operator/(cfloat3 a, float s) { return cf3(a.x / s, a.y / s, a.z / s); }
CLAY_FN cfloat3 operator-(cfloat3 a) { return cf3(-a.x, -a.y, -a.z); }

CLAY_FN cfloat4 operator+(cfloat4 a, cfloat4 b) {
    return cf4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}
CLAY_FN cfloat4 operator*(cfloat4 a, float s) { return cf4(a.x * s, a.y * s, a.z * s, a.w * s); }

// -- scalar math (c-prefixed so each backend can map to its native calls) ----

CLAY_FN float cmin(float a, float b) { return a < b ? a : b; }
CLAY_FN float cmax(float a, float b) { return a > b ? a : b; }
CLAY_FN float cclamp(float v, float lo, float hi) { return cmin(cmax(v, lo), hi); }
CLAY_FN float cabs(float v) { return ::fabsf(v); }
CLAY_FN float csign(float v) { return (v > 0.0f) ? 1.0f : ((v < 0.0f) ? -1.0f : 0.0f); }
CLAY_FN float cfloor(float v) { return ::floorf(v); }
CLAY_FN float cround(float v) { return ::roundf(v); }
CLAY_FN float csqrt(float v) { return ::sqrtf(v); }
CLAY_FN float csin(float v) { return ::sinf(v); }
CLAY_FN float ccos(float v) { return ::cosf(v); }
CLAY_FN float ctan(float v) { return ::tanf(v); }
CLAY_FN float cacos(float v) { return ::acosf(v); }
CLAY_FN float catan2(float y, float x) { return ::atan2f(y, x); }
CLAY_FN float cpow(float b, float e) { return ::powf(b, e); }
CLAY_FN float cexp2(float v) { return ::exp2f(v); }
CLAY_FN float cfmod(float a, float b) { return ::fmodf(a, b); }
CLAY_FN float cmix(float a, float b, float t) { return a + (b - a) * t; }

CLAY_NS_END

#elif defined(CLAY_KERNEL_METAL)

#include <metal_stdlib>

#define CLAY_FN inline
#define CLAY_THREAD thread
#define CLAY_DEVICE device
// `kernel` is a reserved word in MSL, so the namespace is `clay::kernels`
// on this backend (only .metal sources ever see it).
#define CLAY_NS_BEGIN \
    namespace clay {  \
    namespace kernels {
#define CLAY_NS_END \
    }               \
    }

CLAY_NS_BEGIN

using cfloat2 = ::float2;
using cfloat3 = ::float3;
using cfloat4 = ::float4;

CLAY_FN cfloat2 cf2(float x, float y) { return cfloat2(x, y); }
CLAY_FN cfloat3 cf3(float x, float y, float z) { return cfloat3(x, y, z); }
CLAY_FN cfloat4 cf4(float x, float y, float z, float w) { return cfloat4(x, y, z, w); }

CLAY_FN float cmin(float a, float b) { return metal::fmin(a, b); }
CLAY_FN float cmax(float a, float b) { return metal::fmax(a, b); }
CLAY_FN float cclamp(float v, float lo, float hi) { return metal::clamp(v, lo, hi); }
CLAY_FN float cabs(float v) { return metal::fabs(v); }
CLAY_FN float csign(float v) { return metal::sign(v); }
CLAY_FN float cfloor(float v) { return metal::floor(v); }
CLAY_FN float cround(float v) { return metal::round(v); }
CLAY_FN float csqrt(float v) { return metal::sqrt(v); }
CLAY_FN float csin(float v) { return metal::sin(v); }
CLAY_FN float ccos(float v) { return metal::cos(v); }
CLAY_FN float ctan(float v) { return metal::tan(v); }
CLAY_FN float cacos(float v) { return metal::acos(v); }
CLAY_FN float catan2(float y, float x) { return metal::atan2(y, x); }
CLAY_FN float cpow(float b, float e) { return metal::pow(b, e); }
CLAY_FN float cexp2(float v) { return metal::exp2(v); }
CLAY_FN float cfmod(float a, float b) { return metal::fmod(a, b); }
CLAY_FN float cmix(float a, float b, float t) { return a + (b - a) * t; }

CLAY_NS_END

#elif defined(CLAY_KERNEL_OPENCL)

// OpenCL C is C99: no namespaces, no user overloading, no references. The
// language's own vector types carry arithmetic operators and its builtins
// are generically overloaded by the compiler, so the whole c* surface maps
// to macros here and the shared C++ overload layer below is skipped.
// Constraint for kernel headers: anything the OpenCL backend compiles must
// avoid templates and function overloading (field.h is therefore CPU/Metal/
// CUDA only — the OpenCL backend implements eval_points/eval_grid, per the
// tier-3 scope in the evaluation-backends spec).

#define CLAY_FN static inline
#define CLAY_THREAD          // private is the default address space
#define CLAY_DEVICE __global
#define CLAY_NS_BEGIN
#define CLAY_NS_END

typedef float2 cfloat2;
typedef float3 cfloat3;
typedef float4 cfloat4;

#define cf2(x, y) ((float2)((float)(x), (float)(y)))
#define cf3(x, y, z) ((float3)((float)(x), (float)(y), (float)(z)))
#define cf4(x, y, z, w) ((float4)((float)(x), (float)(y), (float)(z), (float)(w)))

#define cmin(a, b) min(a, b)
#define cmax(a, b) max(a, b)
#define cclamp(v, lo, hi) clamp(v, lo, hi)
#define cabs(v) fabs(v)
#define csign(v) sign(v)
#define cfloor(v) floor(v)
#define cround(v) round(v)
#define csqrt(v) sqrt(v)
#define csin(v) sin(v)
#define ccos(v) cos(v)
#define ctan(v) tan(v)
#define cacos(v) acos(v)
#define catan2(y, x) atan2(y, x)
#define cpow(b, e) pow(b, e)
#define cexp2(v) exp2(v)
#define cfmod(a, b) fmod(a, b)
#define cmix(a, b, t) mix(a, b, (float)(t))
#define cdot(a, b) dot(a, b)
#define cdot2(v) dot(v, v)
#define clength(v) length(v)
#define cnormalize(v) normalize(v)
#define ccross(a, b) cross(a, b)

#else
#error "claycore kernel shim: this backend mapping is not implemented yet"
#endif

// -- shared vector/matrix layer (built on the per-backend types above) -------

#if !defined(CLAY_KERNEL_OPENCL)  // OpenCL gets these from its builtins

CLAY_NS_BEGIN

CLAY_FN float cdot(cfloat2 a, cfloat2 b) { return a.x * b.x + a.y * b.y; }
CLAY_FN float cdot(cfloat3 a, cfloat3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
CLAY_FN float cdot2(cfloat2 v) { return cdot(v, v); }
CLAY_FN float cdot2(cfloat3 v) { return cdot(v, v); }
CLAY_FN float clength(cfloat2 v) { return csqrt(cdot(v, v)); }
CLAY_FN float clength(cfloat3 v) { return csqrt(cdot(v, v)); }
CLAY_FN cfloat2 cnormalize(cfloat2 v) { return v / clength(v); }
CLAY_FN cfloat3 cnormalize(cfloat3 v) { return v / clength(v); }
CLAY_FN cfloat3 ccross(cfloat3 a, cfloat3 b) {
    return cf3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

CLAY_FN cfloat2 cabs(cfloat2 v) { return cf2(cabs(v.x), cabs(v.y)); }
CLAY_FN cfloat3 cabs(cfloat3 v) { return cf3(cabs(v.x), cabs(v.y), cabs(v.z)); }
CLAY_FN cfloat2 cmin(cfloat2 a, cfloat2 b) { return cf2(cmin(a.x, b.x), cmin(a.y, b.y)); }
CLAY_FN cfloat3 cmin(cfloat3 a, cfloat3 b) {
    return cf3(cmin(a.x, b.x), cmin(a.y, b.y), cmin(a.z, b.z));
}
CLAY_FN cfloat2 cmax(cfloat2 a, cfloat2 b) { return cf2(cmax(a.x, b.x), cmax(a.y, b.y)); }
CLAY_FN cfloat3 cmax(cfloat3 a, cfloat3 b) {
    return cf3(cmax(a.x, b.x), cmax(a.y, b.y), cmax(a.z, b.z));
}
CLAY_FN cfloat2 cmax(cfloat2 a, float s) { return cf2(cmax(a.x, s), cmax(a.y, s)); }
CLAY_FN cfloat3 cmax(cfloat3 a, float s) { return cf3(cmax(a.x, s), cmax(a.y, s), cmax(a.z, s)); }
CLAY_FN cfloat2 cmin(cfloat2 a, float s) { return cf2(cmin(a.x, s), cmin(a.y, s)); }
CLAY_FN cfloat3 cmin(cfloat3 a, float s) { return cf3(cmin(a.x, s), cmin(a.y, s), cmin(a.z, s)); }
CLAY_FN cfloat2 cclamp(cfloat2 v, cfloat2 lo, cfloat2 hi) { return cmin(cmax(v, lo), hi); }
CLAY_FN cfloat3 cclamp(cfloat3 v, cfloat3 lo, cfloat3 hi) { return cmin(cmax(v, lo), hi); }
CLAY_FN cfloat2 cround(cfloat2 v) { return cf2(cround(v.x), cround(v.y)); }
CLAY_FN cfloat3 cround(cfloat3 v) { return cf3(cround(v.x), cround(v.y), cround(v.z)); }
CLAY_FN cfloat2 cmix(cfloat2 a, cfloat2 b, float t) { return a + (b - a) * t; }
CLAY_FN cfloat3 cmix(cfloat3 a, cfloat3 b, float t) { return a + (b - a) * t; }

CLAY_NS_END

#endif  // !CLAY_KERNEL_OPENCL

// -- matrices: no overloads, so every backend shares these ------------------

CLAY_NS_BEGIN

// Column-major, like MSL/GLSL. typedef form for C99 (OpenCL) compatibility.
typedef struct cfloat3x3T {
    cfloat3 c0, c1, c2;
} cfloat3x3;
typedef struct cfloat4x4T {
    cfloat4 c0, c1, c2, c3;
} cfloat4x4;

CLAY_FN cfloat3 cmul(cfloat3x3 m, cfloat3 v) { return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z; }

// Transform a POINT by an affine matrix (kernels receive pre-inverted
// transforms; see the tape-compilation requirement in the scene-model spec).
CLAY_FN cfloat3 cmul_point(cfloat4x4 m, cfloat3 p) {
    cfloat4 r = m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3;
    return cf3(r.x, r.y, r.z);
}
// Transform a DIRECTION (no translation).
CLAY_FN cfloat3 cmul_dir(cfloat4x4 m, cfloat3 d) {
    cfloat4 r = m.c0 * d.x + m.c1 * d.y + m.c2 * d.z;
    return cf3(r.x, r.y, r.z);
}

CLAY_NS_END
