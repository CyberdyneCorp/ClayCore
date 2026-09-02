#!/usr/bin/env python3
"""The unit suite's shards partition it exactly (issue #430).

`tests/CMakeLists.txt` runs `clay_unit_tests` as four ctest entries so the
sanitizer jobs use more than one core of the runner's four. Splitting a suite
has exactly one way to do harm, and it is silent: a file reachable from two
shards runs twice, and a file reachable from none STOPS RUNNING while ctest goes
on reporting green. Nothing in the test output would say so -- the count is
printed per shard, and nobody adds four numbers.

So the partition is asserted rather than trusted. This runs the binary once per
shard with `--list-test-cases`, once unfiltered, and checks:

  1. every case appears in exactly one shard -- no case lost, none run twice;
  2. the union is the whole suite;

The shard filters are read out of tests/CMakeLists.txt rather than duplicated
here, because a copy of them in this file is a second thing to keep in step and
the first thing anybody would forget.

Run: python3 tools/check_test_shards.py --binary path/to/clay_unit_tests
"""

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKE = REPO / "tests" / "CMakeLists.txt"


def shard_filters():
    """The -sf / -sfe arguments, read from the CMake that defines them."""
    text = CMAKE.read_text()
    groups = dict(re.findall(r'set\((CLAY_SHARD_\w+)\s+\n?\s*"([^"]*)"\)', text))
    if not groups:
        sys.exit("shards: no CLAY_SHARD_* groups in tests/CMakeLists.txt")

    # The add_test lines reference the groups by ${VAR}; expand them.
    shards = []
    for m in re.finditer(r"add_test\(NAME (clay_unit_tests_\w+)\s+COMMAND[^)]*?"
                         r"-(sfe|sf)=([^\s)]+)\)", text, re.S):
        name, kind, raw = m.group(1), m.group(2), m.group(3)
        expanded = re.sub(r"\$\{(CLAY_SHARD_\w+)\}", lambda v: groups[v.group(1)], raw)
        shards.append((name, kind, expanded.strip()))
    if not shards:
        sys.exit("shards: no clay_unit_tests_* entries found")
    return shards


def cases(binary, kind=None, expr=None):
    cmd = [binary, "--list-test-cases"]
    if kind:
        cmd.append(f"-{kind}={expr}")
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        sys.exit(f"shards: {' '.join(cmd)} exited {out.returncode}\n{out.stderr}")
    names = []
    for line in out.stdout.splitlines():
        line = line.strip()
        # doctest brackets its listing with '=' rules and ends with a summary.
        if not line or line.startswith("=") or line.startswith("[doctest]"):
            continue
        names.append(line)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    args = ap.parse_args()

    whole = cases(args.binary)
    if not whole:
        sys.exit("shards: the unfiltered suite listed no cases at all")

    # COUNTED, NOT SET-MEMBERSHIP. Two cases in two different files may share a
    # title -- `test_multires_io.cpp` and `test_detail_field.cpp` both have "the
    # decoder refuses a truncated, hostile or unknown buffer" -- and doctest's
    # listing gives titles with no file beside them. Comparing sets would call
    # that a duplicated case and fail a partition that is in fact exact, which
    # is a gate crying wolf on its first run. Comparing MULTISETS asks the
    # question that matters: does each title appear across the shards exactly as
    # many times as it appears in the whole suite.
    want = collections.Counter(whole)
    got = collections.Counter()
    where = collections.defaultdict(list)
    for name, kind, expr in shard_filters():
        for c in cases(args.binary, kind, expr):
            got[c] += 1
            where[c].append(name)

    ok = True
    for c in sorted(set(want) | set(got)):
        if got[c] == want[c]:
            continue
        ok = False
        if got[c] > want[c]:
            print(f"shards: RUNS {got[c]}x, suite has {want[c]} -- "
                  f"{c}  [{', '.join(where[c])}]")
        elif got[c] == 0:
            print(f"shards: RUNS IN NO SHARD -- {c}")
        else:
            print(f"shards: RUNS {got[c]}x, suite has {want[c]} -- {c}")

    if not ok:
        print(f"shards: FAILED -- {sum(want.values())} cases in the suite, "
              f"{sum(got.values())} across the shards")
        return 1

    print(f"shards: OK ({len(whole)} cases partitioned across "
          f"{len(shard_filters())} shards, none duplicated, none unrun)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
