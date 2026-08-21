# Proposal: read the format we already write

## Why

`save_glb` exists and no `load_glb` does. OBJ, PLY and FBX all round trip;
glTF/GLB was the only format this library could write and not read, which the
roadmap recorded as "the only real gap in the importer".

It is the gap that matters most in practice. GLB is what Blender exports, what
a mobile app ships, and what a host is most likely to hand a sculpting library
first — importing a base mesh is step one of most sculpting workflows, and
until now `clay_mesh_save(m, "x.glb")` followed by `clay_mesh_load("x.glb")`
failed on the second call.

## What changes

`io::load_glb` / `load_glb_file`, reached from both bindings through the
existing extension dispatch, so `clay_mesh_load("model.glb")` and
`clay.load_mesh("model.glb")` simply work rather than needing a new entry
point.

It reads the WHOLE file: every mesh, every `TRIANGLES` primitive, concatenated
into one mesh with each node's world transform applied. An exported scene
otherwise arrives as a pile of pieces at the origin, which reads as a broken
importer rather than a missing feature. Normals go through the inverse
transpose, so a non-uniform scale does not tilt them off the surface.

Accessors are read in the formats real exporters emit rather than only the ones
this library writes: `uint8` / `uint16` / `uint32` indices, interleaved
bufferViews with a `byteStride`, and `COLOR_0` as `VEC3` or `VEC4` in float,
`uint8` or `uint16` with the spec's normalization.

A JSON reader comes with it, because the writer assembles its JSON by hand and
there was nothing to parse one back. It is deliberately strict — no comments,
no trailing commas, no `NaN` or `Infinity` literals — since glTF forbids all of
them and accepting them would mean reading files the format does not describe.

### What it does not do, and why that is stated

Materials, textures, animation, skinning, cameras and morph targets are
IGNORED: `mesh::Mesh` has nowhere to put them, and an asset carrying them
should still import its geometry. A non-triangle primitive mode is REFUSED
rather than skipped — importing a line set as an empty mesh looks like a broken
reader, and the difference between "this file has no triangles" and "this
reader dropped them" is exactly what a user cannot diagnose.

`.gltf` is not accepted. Its buffers live in separate files beside it, and a
loader handed one path that then read whatever the JSON named would be reading
files the caller never gave it.

## Impact

Additive. No signature changes and no new C entry points — `clay_mesh_load`
gains an extension. ABI 0.37.0, since `clay_mesh_load` accepts a format it did
not before.

The reader parses UNTRUSTED input, which is the part worth reviewing: every
accessor is bounds-checked against its bufferView and the BIN chunk before a
byte is read, the declared GLB length is never trusted over the real one, JSON
nesting is bounded, and the node walk is iterative with a visited set so a
self-referencing node list terminates instead of recursing. A deterministic
mutation sweep over a valid file — 1 161 single-byte flips — runs clean under
ASan.
