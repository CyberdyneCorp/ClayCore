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

- Alternating live latency measurement.
- Platform CI and final PR evidence.
- The broader #531 target remains 16 ms across all reported brushes and is not yet met.

## CPU verification

All 11 CPU CTest targets pass in 85.82 seconds: 2,759 C++ cases / 16,672,231 assertions, plus 756 Python tests and one intentional skip. A final declaration-only change prevents copying/moving the private lookup; the complete build is repeated afterward and focused meshing tests are rerun. The final focused rerun passes 72 cases / 1,303,476 assertions. ASan/UBSan with leak detection passes the same 72 cases and assertion count. Combined application results are recorded below.

## Combined application correctness

The combined application uses host production `4e8d1af` and Core `db802faa`. After correcting the visual Clay fixture to touch the starting radius-1 sphere at z=1, all 85 enabled cases pass: 68 library and 17 native/rendered integration cases. The informational library timing test is ignored, with no adapter skips. Windowed tests run on an isolated virtual X display; these are correctness results, not latency evidence. The application's committed engine pin is restored afterward.

The original fixture at z=0.6 placed a radius-0.25 dab inside the sphere. Its image-change assertion passed only two of three repetitions with each of the previous and new engine builds. The actual main-branch test (9a7ec9c with Core v0.113.0) passed that image assertion and later failed a different export-gate assertion, so the image failure is not claimed as reproduced on main. The fixture now requests a visible surface change and retains the image-difference assertion. A subsequent active-desktop run passed that assertion but failed its consent-gate expectation; the complete isolated run passes both.

The corrected surface-touching end-to-end test also passes all six isolated alternating repeats (three each against Core `abd87b0b` and `db802faa`, with identical application production code). Assertions for visible pixel change, measurement synchronization, history and consent remain enabled.
