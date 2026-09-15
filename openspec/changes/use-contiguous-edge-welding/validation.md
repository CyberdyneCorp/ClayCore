# Validation in progress

## Exact lookup regressions

Two cases pass 24,616 assertions. A standard unordered-map reference checks first-encounter indices through repeated growth. A constant hash forces all 2,048 keys through collisions and bucket wrapping. Additional inputs cover zero and maximum endpoint/index bits and independent builders.

Clang-tidy cognitive complexity is 3 for interning, 2 for probing and 4 for growth. Test helpers score 10 and 2; test cases score 2 and 1. Layering and all 66 strict OpenSpec items pass.

## Exact mesh comparison

The original builder from the pre-change revision is compiled as a separate object and linked ahead of the otherwise-current library. Twelve cases cover sphere, box and a 48-Grab sphere; full/48-key subsets; with/without gradient normals and color. All 60 serialized vectors (positions, indices, normals, colors, brick ranges) match exactly: 36,019,240 bytes, SHA-256 `2a7cfac38a13c1f4dfff484c694c733e373cc1b87237d462d276c27cf26e449c`. The parser verifies vector counts and the exact end of each file.

## Isolated production timing

Three alternating process pairs, three timed repetitions per case, complete 216 timed runs. All paired output hashes match. One-minute load is 1.477–1.839 on 24 logical CPUs; no other build or test from this task runs concurrently. Times are informational component medians, not application latency or a per-run guarantee.

| Fixture | Keys | Attributes | Before ms | Contiguous ms |
|---|---:|---|---:|---:|
| Sphere | 1043 | None | 32.355 | 24.806 |
| Sphere | 1043 | Gradient/color | 36.603 | 27.508 |
| Box | 336 | None | 11.090 | 7.524 |
| Box | 336 | Gradient/color | 12.016 | 9.365 |
| 48-Grab sphere | 1044 | None | 30.382 | 23.627 |
| 48-Grab sphere | 1044 | Gradient/color | 39.351 | 30.877 |

The six 48-key cases improve by approximately 0.08–0.50 ms in median. Complete application measurements remain pending.

## Memory tradeoff

A standalone allocation counter measures lookup storage alone, excluding mesh output and fixture construction. At 141,000 distinct edges, the standard map performs 141,014 allocations, retains/peaks at 7,023,464 bytes and cumulatively allocates 8,366,480 bytes. The selected table performs 15 allocations, retains 6,291,456 bytes, peaks at 9,437,184 bytes during rehash and cumulatively allocates 12,582,528 bytes. These are allocator payload bytes, not process resident memory.

The half-full prototype peaked at 18,874,368 bytes and retained 12,582,912; it is not adopted. The selected table grows at 75% occupancy and starts with 16 buckets. At 10,000 edges it retains 393,216 bytes versus the standard map's 482,184, but peaks at 589,824 during growth. Peak scratch memory can therefore increase even though node allocations disappear. The table is private to a mesh builder and is released at its end; no persistent cache or document storage changes.

## Remaining gates

- Full CPU suite and targeted sanitizer results.
- Combined application correctness and alternating live latency measurement.
- Platform CI and final PR evidence.
- The broader #531 target remains 16 ms across all reported brushes and is not yet met.

## CPU verification

All 11 CPU CTest targets pass in 85.82 seconds: 2,759 C++ cases / 16,672,231 assertions, plus 756 Python tests and one intentional skip. A final declaration-only change prevents copying/moving the private lookup; the complete build is repeated afterward and focused meshing tests are rerun. Sanitizer and combined application verification remain pending.
