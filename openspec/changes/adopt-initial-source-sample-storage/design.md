# Design

Fill the temporary vector before changing the volume, as today. If the sample store is empty, swap it with that filled vector and use the adopted storage while recording brick metadata. Otherwise append samples as before. Compute brick offsets from the sample-store size before adoption/appending. The stored samples never move during metadata processing in the adopted path.

This preserves callback observations: each fill sees exactly the materialized bricks from earlier runs, and no samples from its current run are published before it returns. Existing samples and edits are never refilled. The moved block is private to the volume, and temporary scratch remains local to the call. No cache invalidation or cross-thread ownership mechanism is introduced.

For full initial priming, total allocation requests must be below two sample payloads, including bookkeeping, so an implementation retaining both the complete scratch block and a copied final payload fails. Compare complete serialized output and callback observations against the previous implementation. Verify local correctness under contention, but exclude contended timings from performance claims.
