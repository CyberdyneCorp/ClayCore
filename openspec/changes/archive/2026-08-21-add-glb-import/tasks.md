# Tasks

## 1. A JSON reader

- [x] 1.1 Dependency-free and error-code based, because the core builds with
      `-fno-exceptions` and the GLB writer already assembles its JSON by hand
- [x] 1.2 Flat arena of nodes rather than an owning tree, so freeing a document
      does not walk a deep structure
- [x] 1.3 Bounded nesting — a file of ten thousand `[` must not recurse until
      the stack ends
- [x] 1.4 Numbers validated against JSON's grammar BEFORE `strtod`, which also
      accepts `nan`, `inf` and hex — none of which JSON has, and all of which
      would otherwise enter a mesh as a coordinate
- [x] 1.5 `\u` escapes including surrogate pairs; lone surrogates refused

## 2. The GLB reader

- [x] 2.1 Container: magic, version, chunk walk, 4-byte chunk alignment
- [x] 2.2 The declared total length is never trusted over the real one
- [x] 2.3 Accessors resolved and bounds-checked against bufferView and BIN
      once, so the readers can index without re-checking
- [x] 2.4 `uint8` / `uint16` / `uint32` indices, and non-indexed primitives
- [x] 2.5 Interleaved bufferViews with a `byteStride`
- [x] 2.6 `COLOR_0` as VEC3 or VEC4, float / `uint8` / `uint16`, normalized per
      the spec, alpha dropped
- [x] 2.7 Node hierarchy walked with world transforms; normals by the inverse
      transpose
- [x] 2.8 The walk is iterative with a visited set, so a node cycle terminates
- [x] 2.9 Non-triangle modes refused rather than dropped
- [x] 2.10 Import budget enforced before allocating

## 3. Reach it from both bindings

- [x] 3.1 `clay_mesh_load` gains `.glb`; no new entry point
- [x] 3.2 `clay.load_mesh` gains `.glb`, and its "supported" message updated —
      it named the gap out loud
- [x] 3.3 `.gltf` refused with a reason rather than a generic failure
- [x] 3.4 ABI 0.37.0 in all three places the release checklist names

## 4. Prove it on files we did not write

- [x] 4.1 Round trip through our own writer, bit-identical
- [x] 4.2 A GLB assembled BY HAND using conventions our writer never emits —
      node transform, `uint16` indices, interleaved stride, normalized `uint8`
      colours — because round-tripping our own output only proves the reader
      agrees with our writer
- [x] 4.3 Hostile input: bad magic, wrong version, truncation, a chunk longer
      than the file, malformed JSON, an accessor past its bufferView, an index
      past the vertices, a node cycle
- [x] 4.4 A deterministic single-byte mutation sweep over a valid file, with
      every accepted result required to be self-consistent
- [x] 4.5 The whole reader clean under ASan
- [x] 4.6 Round trip through the C ABI and through pyclay, including the budget
