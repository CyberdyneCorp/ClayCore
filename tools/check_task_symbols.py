#!/usr/bin/env python3
"""Every identifier and file a change's tasks.md names in backticks must exist.

A tasks.md is a checklist someone else executes. When it names `foo_bar()` or
`tests/unit/test_x.cpp`, the reader goes looking for it -- so a name that is not
in the tree is either a typo or a promise dressed up as a citation, and both
waste the reader's time in the same way. The rule this enforces is: cite what
exists, and describe what does not exist in prose.

Only spans that unambiguously LOOK like a symbol or a path are checked. Prose in
backticks, version numbers, flags and expressions are skipped, because guessing
at those produces false failures and a gate nobody trusts gets disabled.

Names that predate this gate are listed in `tools/task_symbols_baseline.txt`
rather than deleted from changes this repository has in flight. The baseline is
a record of debt, not a silencer: a name may only be added to it by hand, so a
new change cannot acquire one by accident.

Usage:
    tools/check_task_symbols.py                 # every non-archived change
    tools/check_task_symbols.py <change-name>   # one change
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGES = os.path.join(ROOT, "openspec", "changes")

# Where a symbol may live. openspec/ is deliberately absent: a name that appears
# only in the proposal that promises it has not been built.
SEARCH_DIRS = ["include", "src", "tests", "bindings", "examples", "benchmarks", "tools", "docs"]
SEARCH_FILES = ["CMakeLists.txt", "pyproject.toml", "README.md"]

BASELINE = os.path.join(ROOT, "tools", "task_symbols_baseline.txt")

BACKTICK = re.compile(r"`([^`\n]+)`")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)*(?:\(\))?$")
PATHLIKE = re.compile(r"^[A-Za-z0-9_.][A-Za-z0-9_./+-]*$")

# Words that pass the identifier shape and are English, not code.
PROSE = {
    "and", "or", "not", "the", "a", "an", "is", "it", "no", "yes", "true", "false",
    "TODO", "NOT", "DONE", "ALREADY", "READ", "MEASURE", "DECISION", "x", "n", "k",
}


def is_path(span: str) -> bool:
    return "/" in span and PATHLIKE.match(span) is not None


def is_ident(span: str) -> bool:
    if not IDENT.match(span):
        return False
    bare = span.rstrip("()").split("::")[-1]
    if bare in PROSE or len(bare) < 3:
        return False
    # A single all-lowercase English word with no underscore is prose more often
    # than it is a symbol; require a shape only code has.
    return "_" in bare or "::" in span or span.endswith("()") or not bare.islower()


def haystack() -> list[str]:
    args = [d for d in SEARCH_DIRS if os.path.isdir(os.path.join(ROOT, d))]
    args += [f for f in SEARCH_FILES if os.path.isfile(os.path.join(ROOT, f))]
    return args


def find_symbol(name: str, dirs: list[str]) -> bool:
    bare = name.rstrip("()").split("::")[-1]
    proc = subprocess.run(
        ["git", "grep", "-l", "-w", "-F", "--", bare] + dirs,
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return proc.returncode == 0


def load_baseline() -> set[tuple[str, str]]:
    if not os.path.isfile(BASELINE):
        return set()
    out = set()
    with open(BASELINE, encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            change, _, span = line.partition("\t")
            out.add((change.strip(), span.strip()))
    return out


def check(change: str, dirs: list[str], baseline: set[tuple[str, str]]) -> list[str]:
    path = os.path.join(CHANGES, change, "tasks.md")
    if not os.path.isfile(path):
        return []
    failures = []
    with open(path, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            for span in BACKTICK.findall(line):
                span = span.strip()
                if (change, span) in baseline:
                    continue
                if is_path(span):
                    if not os.path.exists(os.path.join(ROOT, span)):
                        failures.append(f"{change}/tasks.md:{lineno}: no such file `{span}`")
                elif is_ident(span):
                    if not find_symbol(span, dirs):
                        failures.append(f"{change}/tasks.md:{lineno}: no such symbol `{span}`")
    return failures


def main() -> int:
    if not os.path.isdir(CHANGES):
        print("no openspec/changes directory")
        return 1
    wanted = sys.argv[1:]
    if wanted:
        changes = wanted
    else:
        changes = sorted(
            name for name in os.listdir(CHANGES)
            if name != "archive" and os.path.isdir(os.path.join(CHANGES, name))
        )
    dirs = haystack()
    baseline = load_baseline()
    failures = []
    for change in changes:
        failures += check(change, dirs, baseline)
    for failure in failures:
        print(failure)
    checked = len(changes)
    if failures:
        print(f"\n{len(failures)} unresolved name(s) across {checked} change(s)")
        return 1
    print(f"task symbols resolve in {checked} change(s)"
          + (f", {len(baseline)} baselined" if baseline else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
