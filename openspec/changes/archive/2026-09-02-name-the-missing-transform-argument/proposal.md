# Proposal: name the transform argument a caller left out

## Why

Issue #327 reports that `clay_layer_set_transform` with a NULL `rotation_axis`
"silently discards the whole transform and returns `CLAY_OK`".

**The return is not `CLAY_OK`.** The call already refuses:

```
set_transform(NULL axis) -> 1  err="null transform"
readback                 -> pos=(0,0,0)      # unchanged, as a refusal implies
```

`read_transform` has guarded both arrays since the edit vocabulary was first
exposed, and the check is byte-identical at `v0.39.0`, the version #319 was
measured against. Every prebuilt library in the tree refuses — `cpu-only`,
`release`, `cpu-check`, `bench`, `relgpu` — and there is one unversioned
`clay_layer_set_transform` symbol with no wrapper in the Swift or Python
bindings that could reach it another way.

So the ABI already does what #327 asks for as its second option, and the
refusal it says "would have caught it on the first run" is there. What is not
there is anything that tells a caller so:

- the message is `"null transform"` whether the POSITION or the AXIS was null,
  which names the pair and not the mistake — a caller who read the axis as
  optional learns that one of two arguments was missing and is no closer to
  which;
- `clay.h` documented neither the non-NULL nor the non-zero requirement on the
  node and layer transforms, so the only place the rule was written down was
  `clay_mesh_transform`, which a host editing a document never reads;
- nothing in the test suite pinned the refusal, which is why the contract could
  be argued about from measurement rather than settled by reading a test.

## What Changes

`read_transform` refuses the two arguments separately: `"null position"`, and
`"null rotation axis: name an axis and pass angle 0 for no rotation"` — the
second says what to pass instead, because the caller who hits it wanted no
rotation and the signature already has a way to say that.

`clay.h` states the rule at `clay_layer_set_transform`, where a host editing a
node reads, and at `clay_document_set_layer_transform`, which carried no
documentation at all. Both point at the convention `clay_mesh_transform`
already names, so there is one rule and not three.

`tests/unit/test_c_transform_args.cpp` pins it: the refusal, that a refused
edit leaves the node's placement exactly as it was, that the two messages
differ and each names its argument, and that every entry point taking a
transform refuses on the same terms.

## Impact

**No behaviour change and no ABI change.** No symbol moves, no signature
changes, no result code changes for any input. What changes is the text of
`clay_last_error` after a refusal that already happened, and what the header
says about it.

A host matching on the exact string `"null transform"` would stop matching.
Nothing in the tree does, and the ABI documents `clay_last_error` as a
diagnostic rather than a value to branch on.

## Non-goals

**Not the other option #327 offers.** Treating a NULL axis as "no rotation" was
considered and rejected: these calls take the WHOLE transform, so accepting
NULL would be a second way to say what `rotation_angle` 0 already says, and it
would remove exactly the refusal the issue's own "how it was found" section
wanted. The header now says this, so the decision is discoverable rather than
re-litigated from a benchmark.
