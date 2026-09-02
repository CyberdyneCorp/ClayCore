# Design: add-mobile-thread-scheduling

## D1 — The default class is UserInitiated, and it is not a neutral choice

Every dispatch that existed before work classes did keeps its signature and
means `UserInitiated`. That is the honest reading of the work already in the
tree: a caller was blocked on all of it, since the pool is synchronous and has
no queue to defer into.

The two alternatives were both worse. A `Default`/`Unspecified` class would let
an unclassified call site keep drifting forever with nothing to notice it, and
on Apple maps to `QOS_CLASS_DEFAULT`, which is a distinct class the scheduler
treats as *unknown intent* rather than as a priority. Defaulting to
`Interactive` would mark the whole library's existing work as latency-critical
on the strength of nobody having said otherwise, which is precisely the failure
this change exists to prevent — if everything is interactive, nothing is.

## D2 — One pool with a class per job, not a pool per class

Two pools would remove the per-job `pthread_set_qos_class_self_np` and give a
worker a fixed class for its whole life. It also doubles the resident worker
count on a device where thread stacks are already accounted, splits the
over-decomposition balance across two sets of workers that cannot help each
other, and cannot reclassify a job at all.

The pool holds ONE job slot. A second pool means a second slot, which is most of
the way to a multi-job scheduler, and that is the thing the guide says not to
build until measurement demands it. Strategy 1 — apply the class per job
generation — is what is implemented. Strategy 2 stays available and becomes
interesting only if a device profile shows the per-job apply is hot; there are
no device numbers yet, so committing to it now would be guessing.

## D3 — Interactive maps to USER_INITIATED, not USER_INTERACTIVE

Apple reserves `QOS_CLASS_USER_INTERACTIVE` for a main thread's event handling.
A pool of worker threads claiming it competes with the UI thread it exists to
feed, which inverts the goal. `USER_INITIATED` is the highest class a worker
should take, so `Interactive` and `UserInitiated` map to the same QoS today and
are still distinct in the portable vocabulary — the distinction is what lets a
later scheduling policy, or a different platform, separate them without every
call site being revisited.

## D4 — A nested dispatch inherits its caller, ignoring its own argument

A nested call runs inline on a thread already inside another job. Applying the
nested class there re-schedules the OUTER work for the duration: a `Utility`
helper called from an `Interactive` dab would drag the dab down with it. The
argument is honoured for top-level calls and deliberately dropped when nested.

## D5 — The seam is plain C++, not Objective-C++

`pthread_set_qos_class_self_np` is a C function in `<pthread/qos.h>`. One
translation unit with one `#if defined(__APPLE__)` serves both halves, so the
build does not enable `OBJCXX` and CMake needs no platform branch. Swift was
considered and is not possible here in any case: SwiftPM consumes a PREBUILT
xcframework produced from this CMake build, so Swift sits downstream of the
archive and cannot be compiled into it. The host's half of the contract is
Swift's to write, and is documented in `docs/05-claycore-library.md`.

## D6 — No per-operation QoS in the C ABI

An operation knows what it is for better than its caller does.
`clay_dynamic_sculptor_stamp` is interactive by nature and
`clay_sdf_consolidate` is not, and neither should take a class argument — a knob
there invites a host to mark everything interactive, which is the same failure
as having no classes. Only global configuration (the worker limit) is a
candidate for the ABI, and it is not in this change.

## Deferred, deliberately

- **Worker count from performance cores** (tasks 1.2, 1.5) and its **C ABI
  descriptor** (1.7, 1.8). Independent of classification, and the ABI surface
  belongs to whichever change is holding the version line.
- **The call-site classification pass** (Q10-Q14). The header and the seam land
  first so the pass has something to classify against; doing both at once would
  put a mechanical edit across many files on top of a design change and make
  the review of neither possible.
- **Device gates** (Q16-Q20, Q24). Deliberately unfrozen: the acceptance
  criterion is a latency ratio measured on reference iPads, and a number picked
  on a shared CI container would be a gate on the wrong hardware.
