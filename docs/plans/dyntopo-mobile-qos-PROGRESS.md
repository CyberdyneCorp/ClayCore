# Dyntopo + Mobile Threads/QoS — run ledger

This file is the shared memory of an unattended cloud routine that fires every
6 hours. Each run reads it, does one increment of work, and rewrites the
"State" block below before pushing. Nothing else in the repository records what
a previous run decided, so a run that skips the update strands its successor.

Design guide: `docs/plans/dyntopo-mobile-qos-guide.md` (on this branch only).

## Deliverables

Two independent PRs into `main`, in this order:

- **PR A — Dyntopo completion/hardening.** `openspec/changes/add-dynamic-topology`
  reports 68/68 tasks done; the guide's §78 definition-of-done is the real
  checklist. The work is closing gaps against §78 (operator refusal tests,
  constraint propagation, determinism/replay, randomized torture under
  sanitizers, undo identity restoration, dirty-chunk C ABI, pyclay parity,
  numbered example) — NOT rewriting the representation. Branch:
  `feat/dyntopo-completion`.

- **PR B — Mobile Threads / QoS.** `openspec/changes/add-mobile-thread-scheduling`
  is 12/19; the open tasks are 1.1, 1.2, 1.4, 1.5, 1.7, 1.8, 1.10. Guide
  Part II (§29–§79) is the design. Branch: `feat/mobile-thread-qos`.
  Depends on PR A only for merge order, not for code.

## State

```
phase:            not-started
pr_a_branch:      (none)
pr_a_number:      (none)
pr_a_status:      not-started
pr_b_branch:      (none)
pr_b_number:      (none)
pr_b_status:      not-started
last_run_utc:     (none)
last_run_summary: (none)
next_action:      Run 1 — audit Dyntopo against guide §78 and write the
                  DONE/PARTIAL/MISSING/UNTESTED matrix into this file, then
                  cut feat/dyntopo-completion with the first gap closed.
blocked_on:       (none)
```

## Audit matrix (guide §74 Phase A) — filled in by run 1

| Component | Status | Evidence | Gap to close |
| --- | --- | --- | --- |
| _(pending run 1)_ | | | |

## Run log

Append one line per run: `YYYY-MM-DDTHH:MMZ — what changed — commit/PR`.
