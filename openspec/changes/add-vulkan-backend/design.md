# Design: a Vulkan compute backend

## The decision the proposal asked for: the shader route

**A GLSL profile in the shim, not clspv.** The proposal recommended clspv —
compile the existing OpenCL amalgamation to Vulkan SPIR-V, changing no kernel
line — and asked for the decision to be made "on a real compile of the current
kernel set rather than on reputation". That compile was attempted first, and
the route is not available:

- clspv is not packaged (no apt, no Homebrew formula worth pinning), publishes
  **no release binaries at all**, and is not in the Android NDK. Using it means
  building it against LLVM and vendoring the result into CI.
- A dependency that every contributor and every CI runner must build from LLVM
  source, for a tier-3 backend, is a worse trade than the alternative below.

The alternative was measured rather than assumed. GLSL is missing three things
the tape interpreter uses, and the exact site counts are:

| What GLSL lacks | Sites |
|---|---|
| Pointers (`CLAY_DEVICE const float*`, with arithmetic like `blob + (int)q[0]`) | 33 declarations, ~290 index sites |
| Prefix casts (`(int)x` is invalid; `int(x)` is required) | 86 |
| int-to-bool conversion (`(i & 1u)` as a condition) | 6 |
| `typedef struct T {...} N;` and `enum` | 5 + 7 blocks |

So the choice was: build clspv from LLVM, or teach the shim a vocabulary and
spend ~400 mechanical edits in the shared headers. The second was chosen, on
the condition that the four working backends could be PROVEN not to move.

## What makes a 400-site rewrite of correctness-critical code sane

The C-family definitions of every new macro expand to **exactly the text they
replaced**:

```c
#define CLAY_FPTR      CLAY_DEVICE const float*
#define CLAY_AT(p, i)  p[i]
#define CLAY_OFF(p, n) p + n
#define CLAY_INT(x)    (int)x
```

so the preprocessed token stream for cpu, cuda, metal and opencl is unchanged.
That is checkable, and it was checked: preprocess `clay/kernel/kernels.h` under
each dialect before and after, normalise whitespace, compare. Over 400 rewrites
produced **six token differences in total** — the `!= 0u` in `noise.h`, written
by hand, because that one cannot be macro'd without obscuring the bit test for
every reader.

`CLAY_OFF` is deliberately unparenthesised, which is the one place the macros
are not defensive. Parenthesising it would break token identity, and the gate is
worth more than the defence: every use is an argument or an initialiser, never
a subexpression that could rebind. Noted so a later change that uses it
differently knows to look.

## What is generated rather than macro'd

Two constructs have no macro spelling, because the C form is not a macro
expansion at all:

- `enum E { a = 0, b = 1 };` → `const int a = 0; const int b = 1;`
- `typedef struct XT {...} X;` → `struct X {...};`

`tools/amalgamate_glsl.py` rewrites both when it emits the shader. This is the
narrowest possible transpiler: two local, syntactic rules, on constructs whose
extent is unambiguous. Anything it mangles fails the glslang compile, which
runs on every push — so a mistake is a build error, never a wrong field.

The alternative considered and rejected was an X-macro list, which keeps the
enums in the source for both dialects. It would have destroyed the enumerator
comments — `tape.h` documents nearly every one of its ~90 enumerators inline,
and a `\`-continued macro list cannot carry `//` comments.

## The data model: indices, not device addresses

GLSL can express pointers through `GL_EXT_buffer_reference2` and
`VK_KHR_buffer_device_address`. That was tested and works, and was still not
chosen: it raises the floor to devices with buffer-device-address, and it makes
a cursor a 64-bit handle that needs explicit byte arithmetic.

Instead a cursor is a `uint` INDEX, and the tape's `params` and `blob` are
uploaded into **one** storage buffer, in that order, with each base passed in
the push constants. Consequences:

- The floor is Vulkan 1.1 with no optional features.
- One binding and one upload instead of two.
- `CLAY_AT(p, i)` is `clay_floats_[(p) + uint(i)]`, and `CLAY_OFF` is integer
  addition — cheaper than address arithmetic, not more expensive.

The instruction stream is a flat `uint` array read as `(op, param_offset)`
pairs rather than an SSBO of structs, so std430 padding rules cannot disagree
with the host's `CTapeInstr`.

## Two modules, not one with a mode

A SPIR-V module has one entry point. The build compiles the same generated
source twice with `-DCLAY_ENTRY_POINTS` / `-DCLAY_ENTRY_GRID`, rather than
shipping one module that branches on a push constant, because that branch would
cost every invocation for the life of the backend to save one pipeline object.

## What is excluded, and why it costs nothing

`sd_stroke` and `sd_polygon2` take arrays of structs, which a flat-buffer cursor
cannot express. Both are excluded from the Vulkan profile. Neither is reachable
from a tape: a stroke item stores raw floats and evaluates through
`ctape_stroke_dist`, and a polygon profile goes through `sd_polygon2_raw`. This
was verified by finding their callers (there are none) rather than assumed.

## Residency: an exact compare, not a hash

The spec delta requires a residency key that cannot collide. A 64-bit content
hash can, and a collision here means silently evaluating the wrong field, so
the backend keeps a copy of the resident tape and compares it exactly.

The honest accounting: the compare touches the same bytes the upload would, so
against host-visible coherent memory the saving is a memcpy and a queue submit,
not an order of magnitude. It is still the right default — a dab dispatches ~24
times against one document — and the test that pins it counts uploads rather
than trusting the claim. A cheaper key needs a tape IDENTITY in the backend
interface, which is `add-tape-abi-export` territory, not this change's.

The test for it is the case that would break a naive implementation: two
documents with the same shape at different positions produce tapes of identical
LENGTH and different contents, so a residency check keyed on sizes evaluates the
stale field.

## Gradients stay on the host

`eval_points` runs on the device; a gradient request falls back to the scalar
reference for the whole batch, because the tetrahedron tap is in `field.h`,
templated C++ that no compute dialect compiles. Same choice the OpenCL backend
makes.

This is the pattern `speed-the-metal-path` criticises, and the difference is
worth stating rather than glossing: there it is the tier-1 production path and
silent; here it is tier 3, and it is recorded in the backend's own comment, in
`docs/RELEASE.md`, and in the delta spec. Doing it on the device would need a
second copy of the tap pattern or a refactor of the reference evaluator, and
neither belongs in the change that introduces the backend.

## What a software runtime does and does not prove

The parity suite passes identically on an RTX 5060 and on lavapipe. Those runs
are not equivalent evidence: lavapipe executes on the CPU, so agreement with the
CPU reference is close to guaranteed by construction. It gates SPIR-V validity,
descriptor and buffer layout, dispatch and readback — real things, and not
arithmetic. This is the pocl trap the OpenCL CI job fell into, and the delta
spec requires the distinction to be in the job's name.

## Deferred

- The CPU/GPU crossover for this backend. The 16³ figure on record is Metal on
  an M2 Max; Vulkan's dispatch cost is its own.
- Device meshing. `caps().device_meshing` is false and `mesh()` returns
  `Unsupported`, which agree.
- Retiring OpenCL. If this backend holds its tier, that becomes a proposal with
  evidence behind it; deciding it here would be deciding it without any.
