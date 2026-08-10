# Proposal: a host can compile our kernels but cannot get our tape

## Why

`add-host-kernel-package` shipped the hard half of host-side GPU preview. The
kernel dialect is a published artifact (`dist/claycore-kernels/`, and in the
xcframework under `Headers/clay/kernel/`), so an app's `.metal` file includes
one header and evaluates *our* distance functions rather than a copy that will
drift. `clay parity-fixture` exports tapes, probe points and reference values so
the host can assert agreement in its own CI.

What is missing is the tape itself. `ctape_eval` takes four arguments:

```cpp
kernel::ctape_eval(instrs.data(), instrs.size(), params.data(), blob.data(), p)
```

and the C ABI publishes none of the three buffers. A host has the evaluator and
no way to hand it the document the user is actually sculpting. It can evaluate
the fixture's tapes — which is a CI gate, not a product.

The consequence is a latency one. A host drawing its own frames today has to go
through the library for every one: call `clay_raycast` or the grid evaluation,
have the results computed and copied into host memory, then upload them to the
GPU it was going to draw on anyway. The tape it needs is a few kilobytes and
changes once per edit; the pixels it is round-tripping instead change every
frame. The data flowing across the boundary is three or four orders of magnitude
larger than the data that would suffice, at 60 Hz instead of at edit rate.

This is the row `openspec/ROADMAP.md` already carries as `add-tape-abi-export`
("three buffers and their lifetime across the boundary", blocking
"WYSIWYG preview-vs-bake for any host that draws its own frames"). It is being
written up now because it is on the interactive path, not beside it.

## What changes

The compiled tape crosses the C ABI: instructions, parameters, blob, plus the
field info and bounds that come with it (`safe_step_scale` is what a host's
sphere tracer needs to step safely, and the bounds are what it clips against —
a host that guesses either draws a wrong or a slow frame).

The document already caches its compiled tape keyed on a revision. Exporting
that revision alongside the buffers is what lets a host know whether the tape it
uploaded is still current, which is the difference between re-uploading per
frame and re-uploading per edit.

## What it is not

**Not a new tape format.** `CTapeInstr` is already a versioned, documented
struct that the published headers define, because the host's evaluator is
compiled from those same headers. The ABI publishes what exists.

**Not a promise of stability the kernels do not have.** The tape encoding
changes when opcodes are added. The version already carried by the kernel
package is what a host checks against, and a mismatch must be detectable rather
than reinterpreted — the same rule the stroke presets follow.

**Not a rendering interface.** No camera, no frame, no swapchain enters this
library, for the same reason the cut tool takes a frame in world units rather
than a camera. The host gets the data and draws its own picture.

**Not a live pointer into the document.** How the buffers are owned across the
boundary is the whole design question (below), and "a pointer into the cache
that a subsequent edit invalidates" is the tempting answer that hands hosts a
use-after-free.

## Open questions

- **Ownership and lifetime.** Three shapes: copy into caller-provided buffers
  (simple, costs a copy per edit, and the host must size them first); an opaque
  snapshot handle the caller releases (no copy, explicit lifetime, one more
  object); or borrowed pointers valid until the next mutation (fastest, and one
  missed invalidation is a crash in someone else's app). The C ABI's existing
  habit is a two-call size-then-fill, and the tape cache already hands out a
  `shared_ptr` snapshot internally, which makes the handle route cheap to build.
  To be decided in `design.md`.
- **Whether a culled tape is exportable too.** A host drawing a region, or
  streaming bricks, wants the same cull the brick cache uses. It is the same
  three buffers with a `CullRegion`, so it costs little to allow and should be
  decided deliberately rather than by omission.
- **What the host does about the blob's growth.** Sampled volumes ride in the
  blob, and the multi-resolution and consolidation work makes them bigger. A
  32 MB blob re-uploaded per brushstroke is exactly the problem the sampled-field
  design identified. Whether this export needs to distinguish "what changed"
  from "the whole tape" is a real question, and the honest answer may be that it
  does not yet and should say so.
- **Versioning.** Which version a host checks, and what it must do on a
  mismatch.

## Impact

`c-abi` gains the export. `build-packaging` gains the statement that the
published kernel package and the exported tape are versioned together — a host
compiling one against the other is the only way this is used. No output values
change, and nothing changes for a host that does not draw its own frames.
