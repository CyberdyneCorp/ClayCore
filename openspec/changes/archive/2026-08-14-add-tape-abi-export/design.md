# Design: add-tape-abi-export

The proposal left four open questions. This answers them.

## 1. Ownership: an opaque snapshot handle the caller releases

**Decision.** `clay_tape_export(doc, …)` returns an opaque `clay_tape*` the
caller frees with `clay_tape_release`. Accessors on it borrow pointers into the
buffers, valid for the handle's lifetime and for nothing else.

This is task 1.1's third option's speed with the first option's safety, and it
is nearly free to build because the machinery is already there. `clay_document`
caches its compiled tape as `std::shared_ptr<const scene::Tape>` keyed on an
atomic revision, and hands it out as a `shared_ptr` — the comment at
`bindings/c/clay_c.cpp:815` says why: "two threads". So a `clay_tape` is a
`shared_ptr<const scene::Tape>` copy. Exporting is a refcount increment. An edit
that recompiles installs a *new* `Tape` in the cache and leaves the exported one
untouched, because it is `const` and shared, which means:

**The lifetime rule is "an export is a snapshot; editing the document cannot
invalidate it."** No invalidation callback, no revision the host must check
before dereferencing, no window. Task 1.8 asks for this to be tested directly
under ASan and that test can only pass — which is the point of choosing the
shape that makes it unfalsifiable rather than the shape that makes it a race.

**Rejected: copy into caller buffers (two-call size-then-fill).** It is this
ABI's habit and it is the wrong habit here. It costs a copy of the whole tape,
blob included, per export; the blob is where sampled volumes ride, so the copy
is exactly the cost the proposal's "open question" about blob growth is worried
about, paid twice. And it needs two calls that must see the same revision, which
is a race the handle does not have.

**Rejected: borrowed pointers into the document's cache.** Fastest by a
refcount, and it hands a host a use-after-free on one missed invalidation. The
proposal already named this and asked for a sentence a reviewer can disagree
with: *the performance difference between an atomic increment and nothing is not
worth a crash class in someone else's app.*

## 2. Culled tapes are exportable

**Decision.** Yes, through the same call, with the optional `region_min` /
`region_max` pair `clay_eval_grid` already uses and the same rules — both NULL
means the whole document, one without the other is rejected, an empty or
non-finite region is rejected.

The alternative was leaving it out, and the proposal is right that omission is
not a decision. A host streaming a region wants the cull the brick cache already
uses, it is `compile_document(doc, &cull)` on a path that exists, and adding it
later would mean a second entry point rather than an argument.

**What differs from the uncached case, and must be said in the header:** a
culled tape is compiled per call and not cached — the existing comment on
`clay_eval_grid` says so, because "consecutive bricks want different regions, so
a cache keyed on the document alone would thrash". So exporting a culled tape
*compiles*, where exporting the whole document's tape is a refcount increment.
A host exporting one cull per brick per frame is doing something expensive and
the header should tell it so.

## 3. Versioning: the kernel package version, checked by the host, refused on mismatch

**Decision.** `clay_tape_encoding_version()` returns the version of the tape
encoding, and it is the version `tools/package_kernels.py` already stamps into
`dist/claycore-kernels/VERSION` — the project version. The two are the same
number because they only work together: a host evaluates an exported tape with
`ctape_eval` from the headers in that package, so an opcode added on one side
and absent on the other is a wrong answer, not a link error.

The check is the **host's** to make, because the library cannot make it: the
library does not know which package the host compiled. So the ABI's obligation
is to publish the number, and the package's obligation is to record the number
its headers evaluate — which is `build-packaging` task in the delta spec. A host
that finds a mismatch refuses; the header says so in those words.

**`CTapeInstr` crosses as itself.** Two `uint32`, the layout the published
headers define, asserted with `offsetof` in `bindings/c/clay_c.cpp` exactly as
`clay_brick_request` is against `brick::BrickRequest`. It joins
`ARRAY_ELEMENT_STRUCTS` in `tools/check_c_abi.py` with that reason: it is an
element a caller receives thousands of, and changing its layout is a break
rather than something to negotiate.

## 4. The blob's growth: not addressed, and here is why that is defensible now

**Decision.** The export publishes the whole tape. There is no delta encoding
and no "what changed since revision N".

The proposal asks whether it needs one and suggests "the honest answer may be
that it does not yet and should say so". It does not yet, for a reason that can
be checked rather than asserted: a tape's `instrs` and `params` are kilobytes,
and the export is a refcount increment, so for a document whose items are
primitives there is nothing to optimize — the host uploads a few KB per edit.
The problem is entirely the `blob`, where sampled volumes ride, and it is real:
a 32 MB blob re-uploaded per brushstroke is the cost the sampled-field design
already identified.

But a delta encoding is only useful if a host can tell *which region* of the
blob changed, and that is a property of the edit, not of the tape — it belongs
with `add-multi-resolution` and `add-consolidation-policy`, which are the changes
that decide how sampled volumes are stored and re-stored. Designing a tape delta
before those land would be designing against a representation that is about to
move.

So: the export reports `blob_size` and the revision, which is enough for a host
to notice that the blob did not change between two edits and skip the re-upload
— the common case for a stroke that touches no volume. That covers the cheap
half honestly, and the header records the expensive half as a known limit with
the changes that will address it named.

## What the export carries

```c
typedef struct clay_tape clay_tape;  /* opaque snapshot */

clay_result clay_tape_export(const clay_document* doc,
                             const float region_min[3], const float region_max[3],
                             clay_tape** out_tape);
void clay_tape_release(clay_tape* tape);

uint32_t clay_tape_encoding_version(void);

/* Borrowed, valid until clay_tape_release. */
const clay_tape_instr* clay_tape_instrs(const clay_tape* tape, size_t* out_count);
const float*           clay_tape_params(const clay_tape* tape, size_t* out_count);
const float*           clay_tape_blob  (const clay_tape* tape, size_t* out_count);

/* What the buffers cannot tell an evaluator. */
clay_result clay_tape_info(const clay_tape* tape, int32_t* out_is_exact,
                           float* out_lipschitz, float* out_safe_step_scale,
                           float out_bounds_min[3], float out_bounds_max[3],
                           uint64_t* out_revision);
```

`safe_step_scale` is published as well as `lipschitz`, though one derives from
the other by `1 / max(lipschitz, 1)`, because the proposal's point is that "a
host that guesses its step scale draws a wrong frame" and a host recomputing a
one-line formula is a host that can get it wrong. `csafe_step_scale` is in the
published headers too; publishing the value costs four bytes and removes the
question.

`out_revision` is `clay_document`'s existing atomic revision — the integer
comparison that replaces comparing buffers.
