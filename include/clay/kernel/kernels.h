#pragma once

// The whole kernel dialect behind one include.
//
// This is the entry point for a HOST that wants to evaluate ClayCore fields
// itself — typically an analytic GPU preview drawn while ClayCore bakes,
// meshes or exports the same document. From Metal shader source:
//
//     #include "clay/kernel/kernels.h"
//     using namespace clay::kernels;   // MSL reserves `kernel`; see shim.h
//
// No build settings are needed: shim.h reads __METAL_VERSION__ and selects
// the MSL branch itself. Point the Metal header search path at the packaged
// headers (`dist/claycore-kernels/include`, or the xcframework slice's
// `Headers`) and there is exactly ONE implementation of every distance
// function, blend and deformer in the product — which is the point. A
// hand-mirrored preview drifts; `docs/06-host-gpu-previews.md` records what
// that cost the last time, and how to gate against it with the parity
// fixture.
//
// The list below is the dependency order; every header is also self-sufficient
// if a consumer prefers to include just one.

#include "clay/kernel/shim.h"

#include "clay/kernel/ease.h"
#include "clay/kernel/ops.h"
#include "clay/kernel/prim2d.h"
#include "clay/kernel/prim3d.h"
#include "clay/kernel/xform.h"
#include "clay/kernel/repeat.h"
#include "clay/kernel/lift.h"
#include "clay/kernel/deform.h"
#include "clay/kernel/tape.h"

// OpenCL C is C99: no templates, no bare struct tags as type names. field.h
// is templated over the field functor, and exactness.h/stroke.h name their
// structs directly, so the OpenCL subset stops at the tape interpreter —
// which is what its backend compiles today (evaluation-backends, tier 3).
#if !defined(CLAY_KERNEL_OPENCL)
#include "clay/kernel/exactness.h"
#include "clay/kernel/stroke.h"
#include "clay/kernel/field.h"
#endif
