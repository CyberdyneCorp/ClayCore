## Implementation
- [x] Split `save_clayspace` into `encode_clayspace` (bytes) and the `snapshot_id` stamp; every save clears `snapshot_reencoded_id`.
- [x] `scene::Document::snapshot_reencoded_id`, recorded by `load_clayspace` only for a stream this build would not write byte for byte (older minor, skipped unknown chunk, dropped orphan chunk).
- [x] `io::journal_seed_for`: unchanged -> the named snapshot; edited -> the identity of what a save would write now; never serialized -> 0.
- [x] `clay_document_enable_undo` and pyclay `Document.enable_undo` seed through `io::journal_seed_for`.
- [x] `load_clayspace`'s chunk dispatch moved into `read_chunk` and the three orphan drops behind `drop_unmatched_chunks`, so tracking exactness does not push it further past the backend target (cognitive complexity 17 on main, 7 now).
- [x] No entry point added or changed: ABI stays 0.121.0.

## Tests
- [x] `tests/unit/test_c_journal.cpp`: the issue's repro through the C ABI is refused with `CLAY_ERROR_SNAPSHOT_MISMATCH` and applies nothing; a save after the enable pairs; edit-save-enable pairs; an unedited load pairs; an unedited minor-19 load pairs and an edited one is refused.
- [x] `tests/unit/test_io.cpp`: the seed equals the loaded id until a voxel or mask edit, equals the identity a following save stamps, does not stamp when asked, is 0 for a never-serialized document; an unknown-chunk stream records a re-encoded id, still seeds its own snapshot, and a save clears it.
- [x] `bindings/python/tests/test_pyclay.py`: the issue's pyclay repro raises "different snapshot"; a save after enable and an unedited load pair.
- [x] Proven failing on 0f45417d: the C repro returned `CLAY_OK` with one step applied, the pyclay repro did not raise.
- [x] Mutation: skipping the load-time re-encode fails the minor-19 C case and the unknown-chunk io case.

## Docs
- [x] `bindings/c/clay.h` enable_undo note, `docs/05` "Enabling undo mid-session", `session/history.h` note_snapshot comment.
