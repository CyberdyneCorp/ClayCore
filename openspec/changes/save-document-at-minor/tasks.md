## 1. The gap, and what it was hiding

- [x] 1.1 `clay_document_writable_at_minor` shipped with its own gap recorded:
      "this ABI has no way to write at an older minor at all"
- [x] 1.2 REPRODUCED through the ABI: that call returns CLAY_OK at minor 18 for
      a document carrying a multiresolution hierarchy, which container minor 19
      added an 'MRES' chunk for, against a promise of "nothing an artist
      authored dropped"
- [x] 1.3 The reason it went unnoticed is written down too: `io/clayspace.h`
      says there is deliberately no `multires_blocking_minor` because
      `save_clayspace` takes no minor, so the query would have no caller. That
      was correct — adding the parameter is what makes it necessary

## 2. The container

- [x] 2.1 `save_clayspace` and `save_clayspace_file` take a minor
- [x] 2.2 A chunk a minor did not have is not written at that minor
- [x] 2.3 Refused before a byte is written, so a caller that ignores the return
      value cannot end up with a partial file that opens
- [x] 2.4 `multires_blocking_minor`, drawn at `multires_carries_detail` — the
      function already here for this question
- [x] 2.5 `document_blocking_minor`: one answer, so the per-field queries cannot
      get out of step

## 3. The ABI

- [x] 3.1 `clay_document_save_at_minor`
- [x] 3.2 `clay_document_save_memory_at_minor`, leaving *out_blob NULL on a
      refusal so a caller checking the pointer and one checking the result agree
- [x] 3.3 `clay_document_writable_at_minor` answers for the whole document
- [x] 3.4 One `resolve_write_minor` for both saves, so the path form and the
      memory form cannot disagree about one document
- [x] 3.5 REFUTED: no new result code. CLAY_ERROR_UNSUPPORTED already means this
      and already carries the blocking layer; two codes for one condition is two
      answers to one question
- [x] 3.6 The refusal names WHICH kind of thing blocked it, in two sentences
- [x] 3.7 A snapshot is noted only at this build's own layout: a file written at
      an older minor is not one this build's journal can be replayed onto
- [x] 3.8 pyclay: `minor=` on `save` and `to_bytes`, sharing one resolver, and
      `writable_at_minor` fixed the same way
- [x] 3.9 ABI 0.92.0 -> 0.93.0 across the three version lines

## 4. Gates

- [x] 4.1 A hierarchy holding only its cage is a plainer file: writable at 18,
      and the stream is SHORTER because it carries no 'MRES' chunk
- [x] 4.2 It reopens with the cage and no hierarchy
- [x] 4.3 A hierarchy with a level above its cage refuses, and names the layer
- [x] 4.4 The query and the save refuse for the same layer
- [x] 4.5 A refused save leaves an existing file byte-identical
- [x] 4.6 The argument rules match the query's: 0 refused, above this build's
      layout clamped
- [x] 4.7 PROVEN TO CATCH ITS REGRESSION: with the query back on
      `layer_blocking_minor` alone, three assertions fail
- [x] 4.8 pyclay covers the same two

## 5. Verification

- [x] 5.1 Full unit suite green
- [ ] 5.2 `python3 tools/release_check.py --skip-slow`
- [ ] 5.3 CI green

## 8. The fixture leaked, and only one CI job could see it

- [x] 8.1 `AddressSanitizer: 520 byte(s) leaked in 5 allocation(s)` on the ASan
      job. `HierarchyDoc` builds a `clay_multires` handle and `refine()` borrows
      one; neither was destroyed. Three constructions plus two refines is the
      five
- [x] 8.2 BOTH handles are the caller's. `clay_layer_take_multires` moves the
      HIERARCHY and says at its declaration that "the handle follows its
      hierarchy rather than being left moved-from"; `clay_layer_multires` hands
      back a borrow where "destroying the handle leaves the hierarchy in place".
      A handle is a separate allocation from the thing it names
- [x] 8.3 NOT REPRODUCIBLE LOCALLY UNDER ASan -- macOS answers
      "detect_leaks is not supported on this platform", so a clean local ASan
      run says nothing at all about leaks. TSan and the plain Linux build passed
      on the same commit; one job of sixteen could see this
- [x] 8.4 Verified with the macOS `leaks` tool instead, both directions: 0 leaks
      with the fix, `5 leaks for 560 total leaked bytes` with it reverted. The
      byte count differs from CI's 520 because the handle struct differs by
      platform; the five objects are the same five
