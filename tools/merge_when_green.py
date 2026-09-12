#!/usr/bin/env python3
"""Merge a pull request only when its checks have GENUINELY passed.

    tools/merge_when_green.py <pr> [--min-checks N] [--timeout-mins N] [--poll N]

Written after merging two PRs that nobody had checked.

THE BUG THIS EXISTS TO PREVENT. The obvious loop is "wait until no check is
pending, then merge". It is wrong, and it is wrong in the direction that merges
unverified code:

    until ! gh pr checks "$pr" | grep -q pending; do sleep 60; done
    gh pr merge "$pr"

After a push there is a window — seconds to minutes — where GitHub has
registered NO checks at all. `gh pr checks` prints nothing, "no pending" is
trivially true, and the PR merges having been verified by nothing. That is how
#547 and #553 reached main. The second time, the loop had been written
specifically to stop the first.

So the condition here is POSITIVE and has three terms, all required:

    at least `min_checks` have PASSED, and
    none is pending, and
    none has failed.

NEVER INFER GREEN FROM ABSENCE. An empty result is "CI has not started", which
is the opposite of "CI is happy" and looks identical to it.

Two more rules the shell version lacked:

  - A FAILURE EXITS WITHOUT MERGING and names the failing checks, rather than
    retrying forever or falling through.
  - A TIMEOUT GIVES UP rather than merging. A run that never finishes is not a
    run that passed, and an unattended script should stop rather than decide.

Exit status is 0 when the merge happened, 1 otherwise, so a caller can tell the
difference without parsing the output.
"""

import argparse
import json
import subprocess
import sys
import time

# The full matrix is 16 jobs. Requiring nearly all of it means a partially
# reported run cannot satisfy the condition: the point is to refuse anything
# that has not demonstrably finished.
DEFAULT_MIN_CHECKS = 14


def gh(args):
    """Run gh, returning (ok, stdout). A failure is never treated as green."""
    try:
        p = subprocess.run(["gh"] + args, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)
    return p.returncode == 0, p.stdout


def tally(pr):
    """(total, passed, pending, failed, failing_names) for a PR's checks.

    A gh failure returns zeros, which cannot satisfy the merge condition — the
    safe direction, since "I could not ask" must never read as "all passed".
    """
    ok, out = gh(["pr", "checks", str(pr), "--json", "name,state"])
    if not ok:
        return 0, 0, 0, 0, []
    try:
        rows = json.loads(out or "[]")
    except json.JSONDecodeError:
        return 0, 0, 0, 0, []
    passed = pending = failed = 0
    failing = []
    for r in rows:
        state = (r.get("state") or "").upper()
        if state in ("SUCCESS", "NEUTRAL", "SKIPPED"):
            passed += 1
        elif state in ("PENDING", "QUEUED", "IN_PROGRESS", "WAITING", "REQUESTED"):
            pending += 1
        else:
            failed += 1
            failing.append(r.get("name", "?"))
    return len(rows), passed, pending, failed, failing


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pr")
    ap.add_argument("--min-checks", type=int, default=DEFAULT_MIN_CHECKS)
    # 90 was too short, twice. With several PRs queued this repo's matrix has
    # taken over 90 minutes to finish its last job, and the watcher then
    # refused a PR that was green minutes later -- #555 and #559 both. It fails
    # toward NOT merging, which is the right direction, but a timeout that
    # fires on healthy runs makes the tool useless rather than safe.
    ap.add_argument("--timeout-mins", type=int, default=240)
    ap.add_argument("--poll", type=int, default=60, help="seconds between polls")
    ap.add_argument("--merge-method", default="merge",
                    choices=("merge", "squash", "rebase"))
    ap.add_argument("--dry-run", action="store_true",
                    help="report the verdict and do not merge")
    args = ap.parse_args()

    deadline = time.monotonic() + args.timeout_mins * 60
    while True:
        total, passed, pending, failed, failing = tally(args.pr)

        if failed:
            print(f"PR #{args.pr} NOT MERGED — {failed} failing:")
            for n in failing:
                print(f"  {n}")
            return 1

        if total > 0 and pending == 0 and passed >= args.min_checks:
            print(f"PR #{args.pr} green: {passed} passed, 0 pending, 0 failed")
            if args.dry_run:
                print("dry run; not merging")
                return 0
            ok, out = gh(["pr", "merge", str(args.pr),
                          f"--{args.merge_method}", "--delete-branch"])
            if not ok:
                print(f"PR #{args.pr} merge REFUSED by gh:\n{out}")
                return 1
            _, state = gh(["pr", "view", str(args.pr), "--json", "state", "-q", ".state"])
            print(f"PR #{args.pr} merged (state: {state.strip()})")
            return 0

        if time.monotonic() > deadline:
            # A run that never finished is not a run that passed.
            print(f"PR #{args.pr} NOT MERGED — timed out with {passed} passed, "
                  f"{pending} pending, {total} reported (needed {args.min_checks})")
            return 1

        time.sleep(args.poll)


if __name__ == "__main__":
    sys.exit(main())
