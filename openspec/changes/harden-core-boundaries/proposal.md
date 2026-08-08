# Proposal: harden the boundaries where untrusted bytes come in

## Why

Every capability in this library has a boundary where data it did not produce
arrives: a `.clayspace` off disk, a mesh a user exported from somewhere else, a
count a host passed across the C ABI. Inside those boundaries the code is
careful. At them it was uneven — and the unevenness was not random, it was
sibling functions disagreeing with each other.

`MaskField::deserialize` refuses a cell size that is not a positive real;
`VoxelGrid::deserialize` took whatever float the file carried, and every
world-to-cell conversion then divided by it and cast the result to `int32`.
`ctape_swept` refuses fewer than two profiles; `ctape_loft` read two records
regardless. `mesh::decimate` compares attribute sizes in its second pass and
tests for emptiness in its first. The C ABI refuses a loft through
`clay_add_item` and accepted one through `clay_layer_set_prim`.

Four loaders each had their own copy of "open the file, seek to the end, ask
how big it is, allocate that much". They had drifted, and the one guard among
them was the wrong guard: `fopen("rb")` succeeds on a DIRECTORY and glibc then
reports its length as `LONG_MAX`, which is not negative, so every loader sized a
buffer from it. The library builds `-fno-exceptions`, so the resulting
`std::bad_alloc` reached `std::terminate`: `clay validate <any-directory>` took
the process down.

None of this needed a hostile file. A two-object FBX where only one object is
vertex-painted — an ordinary export — returned a mesh whose `colors` array was
shorter than its `positions` array, and feeding that to `mesh::decimate` read
past the end of it.

## What changes

Each boundary gets the check its own sibling already had, and the duplicated
file handling collapses into one implementation that cannot drift again. Nothing
here changes what a well-formed input does; every fix turns a crash, an
out-of-bounds read or a silently-wrong result into a refusal the caller can see.

The test reference evaluator is also corrected: it built a six-float deformer
record where the tape's is twelve, so every deformer carrying extension slots —
noise, grab, pose, pose_line, magnify, bend_linear — had been compared against
whatever sat past the array. GCC had been reporting this; the project's
`-Wno-error=array-bounds` demotion, added for a libstdc++ false positive, was
hiding a real one alongside it.

## What it is not

Not a new validation layer, and not a budget system. The refusals reuse the
error vocabulary already in `IoStatus` and `clay_result`, and the limits reuse
`ImportBudget` and `CLAY_MAX_BATCH`. A caller that was passing sound data sees
no difference.

Not a rewrite of the parsers. `load_ply` is still the same function; only its
header scan and its payload-fit guard changed.
