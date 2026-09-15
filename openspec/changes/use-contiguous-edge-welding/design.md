# Design

## Measured problem

Three alternating prototype/control pairs cover sphere, box and a 48-Grab sphere, full and 48-key subsets, with and without gradient/color attributes. All 12 cases have identical complete output hashes across 216 timed runs. Full gradient/color sphere median is 34.854 ms with current storage versus 25.440 ms with the prototype; the deformed sphere is 37.666 versus 28.101 ms. These are isolated timings, not application guarantees. One-minute load is 2.066–2.782 on 24 logical CPUs.

## Lookup

Keep the existing canonical pair of packed lattice endpoint identifiers. Use an avalanche hash for bucket selection, complete pair equality for matching, and linear probing. Occupancy is separate from key and vertex values, so zero and maximum values are valid. Rehash by moving existing entries into a larger power-of-two array; neither rehash order nor hash values define mesh order. The caller assigns an index only on the first encounter and emits exactly the existing coordinate arithmetic.

## Correctness and lifetime

The table belongs to one mesh builder and dies with it. No cross-call cache, shared mutable state or floating-point key is introduced. Forced hash collisions and repeated growth must agree with a standard unordered-map reference. Exercise existing dense-grid, brick, subset, seam, negative-coordinate, attribute and cancellation coverage. Compare complete serialized meshes against the original implementation.

## Memory and alternatives

Open addressing removes per-vertex nodes but spare capacity and rehash overlap may increase peak scratch memory. Measure both allocations and bytes before choosing a load factor. A standard-map node arena prototype gave smaller improvements and complicates allocator lifetime; a new third-party container dependency is disproportionate to this private table. Avoid adding a generic container framework. Do not adopt based solely on the prototype timings.
