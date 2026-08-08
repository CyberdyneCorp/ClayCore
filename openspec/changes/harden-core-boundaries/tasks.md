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

## 8. Degenerate kernels

- [x] 8.1 `sd_round_cone` returns the enclosing sphere when one end sphere
      contains the other, instead of csqrt of a negative radicand — one such
      item returned NaN, and NaN propagates through every combine op, so it
      turned the whole document into NaN
- [x] 8.2 `sd_round_cone_ab` gets the same case, which also covers coincident
      endpoints; it is the stroke-segment kernel, so a tapered two-point stroke
      reaches it

## 9. Edits that escaped the vocabulary

- [x] 9.1 `SetLayerMirrorCmd`: mirroring went straight into the layer, so it
      neither honoured the lock every other layer edit checks nor reached the
      undo stack. Append-only tag; the document format serializes layers, not
      commands, so nothing versioned moves.
- [x] 9.2 `clay_document_move_layer` groups its remove and insert — ungrouped,
      one undo applied only the remove and the layer vanished
- [x] 9.3 `mesh_tape` prices its own dense lattice, not only the C ABI above it
- [x] 9.4 `repair_close_holes` clamps `passes` to the grid's longest side: the
      window grew cubically in it and the padded corner overflowed int first
- [x] 9.5 PLY refuses an element it does not read rather than leaving its
      payload in front of the vertices and returning a silently wrong mesh

## 10. Tests that asserted nothing

- [x] 10.1 The C ABI sculpt-verb parity fixture ran verbs at size 7 inside a
      size-9 solid stamp, entirely in the interior, so smooth, inflate and
      pinch changed ZERO cells and the comparison held for any implementation.
      Verb size raised past the stamp, a spur added for smooth to dissolve, and
      each verb now has to change the grid.
- [x] 10.2 Proven by mutation: stubbing `sculpt_smooth` to a no-op fails the
      test. The parity comparison alone does NOT catch it — both sides call the
      same engine, so a broken verb stays in agreement with itself.
- [x] 10.3 `clay_voxel_sculpt_magnify` and `clay_voxel_sculpt_grab` had no test
      at all; both now have parity coverage and refusal cases
- [x] 10.4 The "did something" assertion compares the whole grid, not the
      occupied COUNT — magnify and grab move material without adding any

## 11. Regressions this change introduced, found by review

Every one of these was a guard added above that was too tight. They are listed
because "the fix broke a working case" is the failure mode a hardening change
is most prone to, and each now has a test.

- [x] 11.1 `FieldVolume::from_blob` refused any volume above 2^24 samples.
      Index entries are stored as float, and past 2^24 a float cannot hold
      consecutive integers, so a large volume reads its own offsets back
      rounded — the exact bound then fired on the LAST brick and a document
      this library had written would not reopen. Entries are snapped to their
      kBrickSamples boundary, which is where they always are.
- [x] 11.2 `clay_document_mesh` capped the grid at CLAY_MAX_BATCH, which caps
      `resolution` at about 253 — the docs advertise 512. CLAY_MAX_BATCH bounds
      how many items cross the boundary in one call, a different quantity. The
      ceiling is now the mesher's own, set from what the API promises.
- [x] 11.3 The ascii PLY payload floor was exactly the cost of a
      newline-terminated line, so a well-formed file whose last line has no
      trailing newline — which the PLY spec does not require — was refused.
- [x] 11.4 `load_clayspace_file` gained a 2 GiB ceiling no caller could raise,
      while `save_clayspace_file` had no matching cap: claycore would write a
      document it then refused to read. It takes an `ImportBudget` now.
- [x] 11.5 `load_obj_file` read into a byte vector and then copied it into a
      string, holding both live — twice the file at peak, for the one loader
      whose input is text. `read_whole_file` reads into either directly.

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
