# Tasks: roll-up-document-memory

## 1. Establish what is actually missing

- [x] 1.1 Enumerate every byte query that exists today and what it covers.
      Expected: `clay_document_history_bytes` (whole subsystem),
      `clay_voxel_sculpt_layers_bytes` (part of one layer),
      `clay_brick_cache_stats` (not owned by the document). Nothing else
- [x] 1.2 Enumerate what a `clay_document` holds and confirm each is unreported:
      `scene::Document`, voxel layers, masks, mesh layers, `session::History`,
      thumbnail and camera passthrough
- [x] 1.3 MEASURED. One Linux box, a document built up in four steps:

      | document | total | edit list | voxel | mask |
      |---|---:|---:|---:|---:|
      | empty | 343 B | 100% | — | — |
      | + 200 SDF items | 96 KB | 100% | — | — |
      | + a 50^3 voxel sphere (65 267 cells) | 360 KB | 26.7% | **73.3%** | — |
      | + a painted mask (125 000 cells) | 623 KB | 15.5% | 42.3% | 42.2% |

      THE PREMISE HOLDS: one modest voxel layer outweighs a 200-item edit list
      by 2.7x, and it was the largest unreported term. The seams stand.

      Two things the measurement changed anyway, both found by walking the
      types rather than by the numbers — see 1.4

- [x] 1.4 The EXISTING `node_bytes` was six members behind, and it is what
      `add-history-budget` reports through. It counted stroke points, the
      deformer chain, a bend guide and the child list, and missed:
      `armature_parents`, `armature_signs`, `profile_points`, `profiles`,
      `profile_polygons` (a vector OF vectors, so wrong twice), a lattice
      deformer's `cage`, and BOTH `shared_ptr<FieldVolume>` members — which are
      typically the largest thing a node owns by two orders of magnitude.
      `FieldVolume` had no `bytes()` at all. So the history's budget was
      measuring low, and lowest on exactly the documents where a budget matters.
      Fixed here rather than filed: the rollup calls the same walk

- [x] 1.5 SHARED PAYLOADS must be charged once per report, or the figure tells
      a host to free memory that was never allocated. Two of them: an instance
      layer's `SdfContent`, and a `FieldVolume` several nodes sample. A
      `SharedSeen` set threads through `node_bytes`, `command_bytes`, both undo
      stacks and the document walk

## 2. Per-subsystem accounting

- [x] 2.1 `scene::Document::bytes()` — nodes, deformer chains, stroke points,
      layer records. The node payload is the term that matters; a per-node
      figure that ignores its chain is the same defect `add-history-budget`
      already fixed once in the command stack
- [x] 2.2 `voxel::VoxelGrid::bytes()` — chunk map, LOD levels, dirty sets,
      palette. Separate from `sculpt_layer_total_bytes()`, which already exists
      and is NOT folded in
- [x] 2.3 `voxel::MaskField::bytes()` and a separate figure for the in-flight
      step snapshot
- [x] 2.4 `mesh::Mesh::bytes()` — positions, normals, colors, uvs, indices,
      quads. Every array, since a mesh with UVs and colors is three times one
      without them
- [x] 2.5 Use `capacity()` rather than `size()` wherever a container
      over-allocates; account an `unordered_map` as buckets plus nodes

## 3. The rollup

- [x] 3.1 `clay/io/memory.h` — `MemoryReport` and `document_memory()`. In `io`
      because it is the only module the layering table lets name all five
      subsystems; verify with `tools/check_layering.py` rather than by reading
      the table
- [x] 3.2 `layer_memory()` for one layer, returning the SAME struct with the
      document-wide fields zeroed
- [x] 3.3 Assert in code that the fields sum to the total, so a field added
      later without being summed cannot pass silently

## 4. The C surface

- [x] 4.1 `clay_memory_report` — versioned descriptor, `struct_size` first,
      written with `write_desc`
- [x] 4.2 `clay_document_memory` and `clay_layer_memory`
- [x] 4.3 An unknown layer id is an error, NOT a zeroed report — a host reads a
      zeroed report as an empty layer
- [x] 4.4 Run `clang++ -Wreturn-type-c-linkage -Werror` on `clay_c.cpp` before
      pushing: GCC does not warn and the CI macOS and Windows jobs do

## 5. Python

- [x] 5.1 Expose both entry points with named fields
- [x] 5.2 `tools/check_binding_parity.py` must pass

## 6. Tests

- [x] 6.1 The report moves with the content — as a RATIO against the empty
      document, never an absolute byte count: `sizeof(Node)` and
      `bucket_count()` differ between libstdc++ and libc++ and an absolute
      assertion fails on macOS for a reason that is not a defect
- [x] 6.2 The fields sum to the total, asserted by SUMMING the fields rather
      than by restating the number
- [x] 6.3 Attribution: painting a mask moves `masks` and leaves `voxel_content`
      and `edit_list` unchanged. This is the assertion a total-only test cannot
      make, and the one that catches a sum into the wrong bucket
- [x] 6.4 The transient snapshot is non-zero mid-step and zero after
- [x] 6.5 Per-layer figures plus the document-wide figures equal the total
- [x] 6.6 An unknown layer id errors
- [x] 6.7 CHECK EVERY FIXTURE IS NON-DEGENERATE before trusting a pass: a
      rollup test on an empty document, or a mask painted where no cells exist,
      asserts a relationship between zeros. Assert the content is there FIRST

      CAUGHT ONE. The per-layer fixture filled +/-20 against +/-3 — 200x apart
      in occupancy — and both straddle the origin, so they touch EXACTLY THE
      SAME EIGHT CHUNKS and reported an identical figure. A chunk is 32^3 cells
      allocated whole, so memory follows CHUNKS, not cells: one voxel costs
      32 KiB and 32 768 voxels in that chunk cost the same 32 KiB. The rollup
      was right and the fixture was meaningless. It now differs in CHUNK SPAN,
      and a second test pins the quantization directly, because a host reading
      voxel_content beside occupied_count will otherwise call their
      independence a bug

- [x] 6.8 The `transient` field is ALWAYS ZERO through the C ABI, and shipping
      it as "non-zero during a gesture" would have been the `record_barrier`
      failure again — documented, asserted, and false. Every C mask entry point
      opens its step and closes it before returning, and calls on one document
      must be serialized, so no caller can hold a handle while a step is open.
      Two tests for the one field: the C half asserts it is zero and fails the
      day an entry point spans a step, the C++ half drives `History` directly
      and asserts it is non-zero mid-step and released after. The header says
      which is which rather than describing behaviour a caller cannot reach

## 7. Documentation

- [x] 7.1 `docs/05-claycore-library.md` — a memory section that says plainly
      what a host may release and what it may not, since that is the decision
      the number exists to inform
- [x] 7.2 State that the figure exceeds the serialized size, and why
- [x] 7.3 State what is outside it: allocator overhead, the brick cache, and
      the library's own code and static data
- [x] 7.4 An example that reads a document's memory and attributes it

## 8. Gates

- [x] 8.1 `ctest`, `pytest`, `check_c_abi`, `check_binding_parity`,
      `check_layering`, `openspec validate --all --strict`
- [x] 8.2 Bump the ABI version and every place that records it
