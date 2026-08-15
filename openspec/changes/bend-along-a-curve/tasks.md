# Tasks — bend along a curve

## 1. Kernel

- [x] 1.1 Thread the blob pointer through `ctape_deform_point`, and through the
      one call site in `ctape_prim_local`
- [x] 1.2 `cbend_curve_point(guide, count, p, t0, t1)` in `deform.h`, built on
      `csweep_nearest` and the sweep's own frame construction
- [x] 1.3 `cdeform_bend_curve` in the tape's deformer dispatch, reading the
      guide offset and count from the record
- [x] 1.4 `cfi_bend_curve` in `exactness.h`: curvature headroom and axial rescale
- [x] 1.5 Kernel dialect gate green — all five backends compile the new signature

## 2. Scene

- [x] 2.1 `Deformer::bend_curve(guide, t0, t1)` carrying the guide
- [x] 2.2 Factor `emit_guide` out of `emit_swept`; the sweep uses the extracted
      one so there is one parallel-transport implementation, not two
- [x] 2.3 Factor the circumradius bend-radius scan out of `swept_field_info`
- [x] 2.4 Emit the deformer's guide into the blob at compile time
- [x] 2.5 Bounds: guide AABB grown by the cross-section extent; Lipschitz via
      `cfi_bend_curve`; mark the item inexact
- [x] 2.6 `.clayspace` codec writes and reads the guide, length-prefixed, after
      the extension floats — old files unaffected

## 3. Bindings

- [x] 3.1 `CLAY_DEFORM_BEND_CURVE` and the guide-carrying C ABI entry point
- [x] 3.2 `pyclay` `.bend_curve(...)`
- [x] 3.3 Refusals: fewer than two points, zero-length guide
- [x] 3.4 Binding parity gate green

## 4. Evidence

- [x] 4.1 A straight guide is the identity, asserted pointwise
- [x] 4.2 A point ON a curved guide reads the item's axis at the matching arc
      length. NOT agreement with the constant-rate `bend`, which is the cheap
      bend and is not arc-length-preserving — the two are not meant to agree
- [x] 4.3 Material follows a turning guide, and the influence bound contains it
- [x] 4.4 A cross-section wider than the tightest bend reports a tiny step scale
- [x] 4.5 `check_conservative_steps` covers a turning guide
- [x] 4.6 Every existing deformer is unchanged by the signature threading
- [x] 4.7 Refusal cases
- [x] 4.8 Parity-corpus scene whose guide TURNS
- [x] 4.9 Round trip through the `.clayspace` codec
- [x] 4.10 `examples/03_deformers.py` gains a case; render inspected, not just run

## 5. Docs

- [x] 5.1 `docs/07` deformer table, ZBrush-equivalent row, reachability
- [x] 5.2 Deformer counts recounted from source wherever they are stated
