# Design

A complete rectangular grid contains `(8*bx+1)*(8*by+1)*(8*bz+1)` unique samples. Generate these in x-fastest order using the same integer-to-float and multiply/add arithmetic as BrickGrid. Evaluate them in one backend batch, then copy nine-float rows into the original 729-float brick layout.

Eligibility requires a full grid beginning at slot zero, at least 64 bricks, positive dimensions, and checked coordinate/allocation products. Limit unique scratch to no more than the existing point scratch. Decline ineligible calls before allocation. Composed sources retain their per-brick coverage and fallback decisions; partial windows retain their current batching. Scratch is per call, with no persistent cache or synchronization. The path uses two temporary vectors instead of one; the guard bounds their combined float payload, not allocator metadata or process RSS.

Output sample bits, brick ordering, volume blobs and sample bounds must match the existing path. A private helper accepts a fill callback so tests can assert both unique evaluation counts and reconstruction independently. Application latency, rather than a synthetic source result alone, decides the practical benefit.
