#!/usr/bin/env python3
"""Every backticked symbol a tasks.md claims must exist in the tree.

A task naming a test, an entry point or a function is a CLAIM, and nothing was
reading it. This change's stage 5 reported `clay_brick_cache_eval_requests_below`
as built; the name existed only in the change's own design.md, and every gate
passed. No gate asks whether the artefacts a change says it built exist, which
is the least sophisticated way for a gate to be missing: the claim is already in
a structured file with a stable syntax.

Deliberately crude, and deliberately quiet about what it cannot judge. It checks
identifier-shaped names only (an underscore or a `::`), skips filenames, and
splits a dotted form so `clay_document_resume_stats.resumed_bricks` is checked as
its parts — a struct field and its function both have to exist, and neither is
grep-able as one token.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP_SUFFIX = (".cpp", ".h", ".py", ".md", ".json", ".clayspace", ".swift", ".sh", ".yml", ".toml")
SEARCH_ARGS = ["--exclude-dir=.git", "--exclude-dir=build", "--exclude-dir=openspec"]


def claimed_symbols(text):
    out = set()
    for name in re.findall(r"`([A-Za-z_][A-Za-z0-9_:.]*)`", text):
        if name.endswith(SKIP_SUFFIX):
            continue
        if "_" not in name and "::" not in name:
            continue
        # A dotted or scoped form is a path to a member: check each part, since
        # neither the whole string nor the owner alone appears as one token.
        for part in re.split(r"[.:]+", name):
            if part and ("_" in part or part[:1].isupper()):
                out.add(part)
    return out


def main():
    missing = []
    checked = 0
    for tasks in sorted(ROOT.glob("openspec/changes/*/tasks.md")):
        names = claimed_symbols(tasks.read_text())
        checked += len(names)
        for name in sorted(names):
            found = subprocess.run(["grep", "-rqI", *SEARCH_ARGS, "--", name, str(ROOT)],
                                   capture_output=True)
            if found.returncode != 0:
                missing.append(f"{tasks.relative_to(ROOT)}: `{name}` is claimed and is nowhere in the tree")
    if missing:
        print("task-symbols: FAIL")
        for m in missing:
            print(f"  {m}")
        return 1
    print(f"task-symbols: OK ({checked} claimed identifiers, all present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
