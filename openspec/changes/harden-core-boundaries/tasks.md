# Tasks: harden-core-boundaries

## 1. One implementation of file handling

- [x] 1.1 `src/io/file_bytes.{h,cpp}`: one `read_whole_file` and one
      `write_whole_file`, replacing four copies of the read block and five of
      the write block
- [x] 1.2 The read checks `fseek`, checks `ftell`, and prices the length against
      `ImportBudget::max_file_bytes` BEFORE sizing the buffer — which is what a
      directory needs, since it opens fine and tells `LONG_MAX`
- [x] 1.3 The write reports a failed `fclose`, not only a short `fwrite`: a
      buffered write fails at the flush as often as at the call
- [x] 1.4 Test: every loader refuses a directory path instead of terminating

## 2. PLY

- [x] 2.1 Step over the newline after `end_header` only when there is one
- [x] 2.2 Refuse a vertex element declaring no properties, which made the
      payload-fit check vacuous for any count
- [x] 2.3 Apply the payload-fit check to ascii as well as binary
- [x] 2.4 Tests: the unterminated header, the zero-property element, the
      over-declaring ascii header

## 3. Meshes off a file satisfy the mesh invariant

- [x] 3.1 FBX import drops a colors array that did not come out aligned, as it
      already did for normals — colour layers are per mesh node and the loader
      accumulates every node into one mesh
- [x] 3.2 `mesh::decimate` compares attribute lengths in its first pass, as its
      second pass already did

## 4. Records that were never written

- [x] 4.1 `ctape_loft` returns far below two profiles, as `ctape_swept` does
- [x] 4.2 `clay_layer_set_prim` refuses the primitives carrying out-of-line
      data, as `clay_add_item` does
- [x] 4.3 `FieldVolume::from_blob` bounds each brick index entry against the
      sample section, not just the section offsets against the blob
- [x] 4.4 Tests: a loft at zero and one profile evaluates far; `set_prim`
      refuses each out-of-line kind; a poisoned index entry is refused

## 5. Voxel and mask payloads

- [x] 5.1 `VoxelGrid::deserialize` refuses a voxel size that is not a positive
      real, as `MaskField::deserialize` already did
- [x] 5.2 Both refuse infinity, which the existing `> 0` test let through
- [x] 5.3 Both bound the declared chunk count by what the remaining bytes could
      describe — a chunk costs 19 bytes and decodes to 32 KiB
- [x] 5.4 Tests: zero, negative, infinite and NaN sizes; an over-declared count

## 6. C ABI counts

- [x] 6.1 `clay_tube_create` runs the batch gate every other out-of-line payload
      runs
- [x] 6.2 `clay_document_mesh` prices the dense grid its resolution implies and
      refuses one above the batch ceiling

## Found while building

- [x] 7.1 The test reference evaluator built a six-float deformer record where
      the tape's is twelve, so noise, grab, pose, pose_line, magnify and
      bend_linear were compared against whatever sat past the array. GCC had
      been reporting it at `tape.h:295`; the `-Wno-error=array-bounds` demotion
      added for a libstdc++ false positive was hiding this real one next to it.
      Six of main's nine clean-build warnings were this one bug.
- [x] 7.2 An "ascii PLY reserves for its declared count" theory did not survive
      measurement: peak RSS stays at 3.4 MB because `reserve` never faults the
      pages in. The payload-fit check in 2.3 is still right — the spec says
      counts are validated before allocating — but it is a consistency fix, not
      the allocation bomb it looked like.
