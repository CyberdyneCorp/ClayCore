## Why

`scene::Layer` carries a name, a transform, protection and visibility, and no
operator. Visible SDF layers hard-union, unconditionally, and the compiler says
so in eight places. So a layer is organisation and nothing else: an artist who
wants one shape to cut another must put both in one layer and lose the ability to
hide, reorder or transform the cutter as a thing.

Giving a layer an operator turns the stack into a procedural modelling stack —
`A - B + C` — where hiding the cutter restores the uncut geometry and reordering
is a modelling decision rather than a cosmetic one.

## What Changes

- `LayerComposition` on an SDF layer — op, blend, `blend_k`, rounding — using the
  **existing item-level enums**. A layer boolean is the same operation an item
  boolean is and SHALL NOT get a second vocabulary or a second evaluator.
- The document compile stops unioning and **folds**: the first visible SDF layer
  initialises the accumulator and every later one combines with what is below.
- **The first visible layer's operator is not applied.** `Subtract(empty, A)` and
  `Intersect(empty, A)` are the two ways a stack can open with nothing on screen
  and no error, and an artist who reorders their base to the top would hit both.
- Bounds per operator, which is the correctness half: a subtract cannot create
  material outside its left operand, an intersect is bounded by the intersection,
  and defaulting everything to union bounds means missing ray hits and incomplete
  brick plans rather than a slow frame.
- Exactness and Lipschitz folded exactly as the item-level combine folds them,
  with a parity fixture: **layer A then layer B(Subtract) must equal one layer
  holding A then B(Subtract)** in distance, colour, bounds and safe step.
- Composition changes are undoable through the existing layer-property history,
  persisted, and default to union so every existing document renders as it does.
- Non-SDF layers REFUSE the setter rather than storing state that does nothing.

## The blast radius, named up front

This is why the guide puts it last, and the audit agrees. The inter-layer union
is not only in `compile_document`:

| Site | What assumes it |
|---|---|
| `tape.h:147` | "Whole document: visible SDF layers chained by hard union" |
| `tape.h:200` | the resumable checkpoint: the trailing union an append must be emitted BEFORE |
| `tape.h:372` | `compile_document_part`: "The union to fold them with is a HARD Add ... Anything else is a different field" |
| `tape.h:390` | `compile_document_except`, for previewing one layer beside the rest |
| `tape_build.cpp:1281` | "a hard Add is exact and adds no extent, so the prefix's are the chain's" |
| `clay_c.cpp:1502,1541` | the brick refill's multi-layer split, which folds the two halves itself |

A layer fold that is not a hard Add breaks the multi-layer brick resume unless
each of these is taught the operator — and the failure is SILENT, because a
refill that folds with the wrong operator returns a field that never existed
rather than an error. `design.md` decides what each site does; the first
decision it must make is whether the resumable split stays available at all when
the layer above is not a union.

## Capabilities

### Modified Capabilities
- `scene-model`: visible SDF layers fold under a per-layer operator rather than
  unioning, with the bounds and exactness that makes it correct.
- `c-abi`: the composition setter and getter.

## Impact

- `include/clay/scene/document.h`, `src/scene/tape_build.cpp`,
  `include/clay/scene/tape.h`, `cull_index`, `io/`, `session/`, `bindings/`.
- The six sites above, each decided rather than discovered.
- ABI grows; document format gains a per-layer block that defaults to union.
