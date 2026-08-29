# Proposal: sculpting inside a group should not cost 90x

## Why

The compiler's append fast path is `tail_append` (`bindings/c/clay_c.cpp:2886`),
and it accepts an edit only when the added node's parent is the ROOT LIST:

```cpp
if (!add || add->parent != scene::kNoNode || add->index != -1) return {};
```

A dab added into a GROUP fails that test. It is then classified structural, so
every prefix seed is retired, the compiled prefix is not reused, and the tape is
recompiled whole — per dab, for the length of the stroke. Everything
`reuse-the-tape-prefix` and `patch-the-resident-tape` bought is switched off by
where the artist put the node.

Nothing said so. The engine has no diagnostic for it, the docs do not mention
it, and until this change the gate could not see it: `sdf_stroke_bricks` adds
its dabs at the layer root, which is the only shape that reaches the path it is
named for.

Measured on the reference iPad (`iPad15,5`, iOS 26.5.2, ABI 0.60.0, CPU, p95),
a 24-dab stroke on the SAME base document, differing only in whether the dabs
are appended to the layer root or into a group at the tail of it:

| per dab | 10 items | 100 items | 1000 items |
|---|---:|---:|---:|
| `sdf_stroke_bricks` (root) | 0.0339 ms | 0.0340 ms | 0.0339 ms |
| `sdf_stroke_in_group_bricks` | 0.1395 ms | 0.3703 ms | **3.0669 ms** |
| ratio | 4.1x | 10.9x | **90.4x** |

The root row is FLAT to three decimal places across two orders of magnitude of
document — that is what the append path is for. The in-group row scales with
the document, because every dab pays for every item that came before it.

Per dab, 3.07 ms against a 4.17 ms frame share is 74% of the budget with no
headroom, still growing, at a document size an artist reaches in an afternoon.

## Verified independently, after one version of the pair was confounded

The first version of the in-group case built the WHOLE document inside a group,
which varied the base document's shape as well as the dab's parent. A C-ABI
harness sharing no code with the device harness separated them, over
10/30/100/300/1000/3000 items:

| ms/dab (M-series Mac, CPU) | 10 | 30 | 100 | 300 | 1000 | 3000 |
|---|---:|---:|---:|---:|---:|---:|
| flat base, dabs at root | 0.047 | 0.036 | 0.038 | 0.038 | 0.039 | 0.039 |
| flat base, dabs in a group | 0.140 | 0.179 | 0.332 | 0.771 | 2.371 | 6.804 |
| ratio | 3.0x | 5.0x | 8.7x | 20.1x | **61.4x** | **176x** |

Identical base documents; only the dab's parent differs. Flat against
O(document) is the signature of a per-dab whole-tape recompile, and it is what
`tail_append` says in source. Reproduces to within 2% across runs. With the
device case corrected to hold the base document fixed, its ratio was unchanged:
89.9x before, 90.4x after.

**A separate anomaly the control turned up, and it is not this.** With the dabs
at the ROOT but the base document inside a group, the same stroke measures 1.1x
at 10, 30, 1000 and 3000 items and **8.4x at 100, 19.7x at 300** — an order of
magnitude, in a band in the middle of the axis, reproducing exactly across runs.
Dabs at the root take the append path either way, so this is something else in
how a grouped document compiles or culls, it is unexplained, and nothing
measures it. Filed here because it was found here; it is not part of this
change's claim and should not be folded into it.

**This is the ordinary way to work.** Grouping a character's head and then
sculpting on it is not an exotic shape; it is what groups are for. A modelling
decision with no performance meaning switches off the engine's most valuable
optimisation, silently.

## What Changes

- `tail_append` recognises an append at the tail of a GROUP's child list, not
  only at the tail of the root list — when that group is itself in tail
  position all the way up to the root, which is the condition that makes the
  compiled prefix still a prefix.
- The compiler's resumable checkpoint follows: `compile_document_append`
  already resumes a layer's chain from a checkpoint, and a group's chain is an
  ordered list compiled the same way. What it needs is the checkpoint taken
  INSIDE the group's compile rather than only in front of the layer's union.
- The invalidation follows the same rule the root case already uses: a
  recognised tail append routes to `touch_appended` and keeps the prefix seeds;
  anything else keeps the legacy drop.
- **The refusals stay refusals.** An insert one place short of the end, a group
  that is not itself last, shared content, an append under a non-local combine
  — each moves the compiled prefix or changes what a prefix means, and each
  keeps today's behaviour. The fast path is widened by exactly one shape, not
  loosened.

## Capabilities

### New Capabilities
None.

### Modified Capabilities
- `scene-model`: what the compiled-prefix reuse must cover — a tail append at
  any depth whose ancestors are all in tail position, not only at the root
  list. Stated ADDITIVELY: the requirement it extends
  ("An appended document reuses its compiled prefix") is still in flight in
  `reuse-the-tape-prefix` and is not yet in the main spec, so this one says
  what the reuse must COVER without restating what the reuse is. **Ordering:
  land `reuse-the-tape-prefix` first, or archive them together.**
- `device-gate`: the stroke case is measured in both shapes, because one is not
  evidence for the other.

## Impact

- `bindings/c/clay_c.cpp` — `tail_append`, and the `command_frontier` sibling
  that makes the same root-list assumption.
- `src/scene/tape_build.cpp` — `run`/`resume` and where the checkpoint is
  taken; `TapeCheckpoint` gains the chain it names.
- `include/clay/scene/tape.h` — what a checkpoint identifies.
- `tests/unit/` — a stroke into a group compiles the same tape as a full
  compile, and takes the append path; every refused shape still refuses.
- `tests/device/` — `sdf_stroke_in_group_bricks`, which exists as of
  `add-device-transform-cases` and is the before.

## Non-goals

**Making an insert anywhere cheap.** Only a TAIL append leaves the compiled
prefix a prefix. An insert in the middle moves everything after it and is the
general case, correctly.

**The cull pad.** A grouped document also compiles and culls differently from a
flat one — worth about 1.06x on the reference iPad once the append path is out
of the comparison (`sdf_stamp_after_drag_bricks` against
`sdf_stamp_after_group_drag_bricks`). That is a different and much smaller
term, and `narrow-the-chain-pad` is where it lives.

**Nested groups deeper than the tail chain.** A dab into a group whose parent
is not last is refused, as above. Widening that needs the checkpoint to name a
path rather than a chain, and no measurement yet says it is worth it.
