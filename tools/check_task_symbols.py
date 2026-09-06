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

# Overridable so `--self-test` can drive this gate end to end over a synthetic
# tree. Nothing in the repository sets it; the self-test sets it and nobody else.
ROOT = (os.environ.get("CLAY_TASK_SYMBOLS_ROOT")
        or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHANGES = os.path.join(ROOT, "openspec", "changes")

# Where a symbol may live. openspec/ is deliberately absent: a name that appears
# only in the proposal that promises it has not been built.
SEARCH_DIRS = ["include", "src", "tests", "bindings", "examples", "benchmarks", "tools",
               "docs", "backends"]
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
    # THE BASELINE IS NOT PART OF THE TREE IT SEARCHES. It lives under `tools/`,
    # which is a search directory, so without this exclusion every name written
    # into the baseline resolves by its own record: the row would be redundant,
    # and — the part that matters — any OTHER change could then cite the same
    # name and pass. A debt list that grants what it records is not a debt list.
    args.append(":(exclude)" + os.path.relpath(BASELINE, ROOT))
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


def resolves(span: str, dirs: list[str]) -> bool:
    """Does this span name something that is actually in the tree right now?"""
    if is_path(span):
        return os.path.exists(os.path.join(ROOT, span))
    if is_ident(span):
        return find_symbol(span, dirs)
    # Neither shape: the gate never had an opinion on it, so it cannot be debt.
    return True


def check(change: str, dirs: list[str], baseline: set[tuple[str, str]],
          used: set[tuple[str, str]], paid: set[tuple[str, str]]) -> list[str]:
    path = os.path.join(CHANGES, change, "tasks.md")
    if not os.path.isfile(path):
        return []
    failures = []
    with open(path, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            for span in BACKTICK.findall(line):
                span = span.strip()
                key = (change, span)
                if key in baseline:
                    # A BASELINED ROW IS STILL MEASURED, it is just not fatal.
                    # Recording that the row was reached, and whether the name
                    # has since appeared, is what lets `stale_rows` retire it --
                    # without this the row exempts the name forever and the
                    # debt list stops describing the tree.
                    used.add(key)
                    if resolves(span, dirs):
                        paid.add(key)
                    continue
                if is_path(span):
                    if not os.path.exists(os.path.join(ROOT, span)):
                        failures.append(f"{change}/tasks.md:{lineno}: no such file `{span}`")
                elif is_ident(span):
                    if not find_symbol(span, dirs):
                        failures.append(f"{change}/tasks.md:{lineno}: no such symbol `{span}`")
    return failures


def stale_rows(baseline: set[tuple[str, str]], used: set[tuple[str, str]],
               paid: set[tuple[str, str]], checked: list[str]) -> list[str]:
    """Baseline rows that no longer record anything, and must be deleted.

    A DEBT LIST NOBODY RETIRES STOPS BEING A DEBT LIST. Each row exempts one
    name in one change; once the name is built, or the change stops citing it,
    the row goes on exempting it silently -- so if that name is later renamed or
    deleted, the gate stays quiet about the very thing it exists to catch. This
    is the same shape as the defect that put the baseline inside the tree it
    searched: a record that grants what it records, and that nothing re-reads.

    The file used to carry "whoever rebases past that merge should delete its
    row" as a note to a human. This is that sentence, enforced.
    """
    out = []
    for change, span in sorted(baseline):
        # A row for a change that is not in this run was never consulted, so it
        # cannot be judged. Only a change that WAS read can retire its rows.
        if change not in checked:
            continue
        key = (change, span)
        if key not in used:
            out.append(f"task_symbols_baseline.txt: {change} no longer cites `{span}`"
                       " -- delete the row")
        elif key in paid:
            out.append(f"task_symbols_baseline.txt: `{span}` now resolves for {change}"
                       " -- delete the row")
    return out


def self_test() -> int:
    """Drive the real gate over a synthetic tree, both directions.

    A gate that is only ever run against a passing tree has never been shown to
    fail, and the stale-row rule in particular is invisible until a debt is
    paid -- which on this branch has not happened yet. So it is exercised here
    against a tree built to make it fire, and this runs as a ctest so the claim
    does not decay into a comment.
    """
    import shutil
    import tempfile

    root = tempfile.mkdtemp(prefix="task-symbols-selftest-")
    try:
        os.makedirs(os.path.join(root, "src"))
        os.makedirs(os.path.join(root, "tools"))
        tasks_dir = os.path.join(root, "openspec", "changes", "demo")
        os.makedirs(tasks_dir)
        # THE GATE IS NOT COPIED INTO THE TREE IT SEARCHES. `tools/` is a
        # search directory, so a copy here would contain the very names this
        # function writes, and every one of them would resolve against the
        # self-test's own source -- the same self-reference the baseline
        # exclusion exists to stop. The real gate runs, pointed at this tree.
        gate = os.path.abspath(__file__)

        src = os.path.join(root, "src", "thing.cpp")
        tasks = os.path.join(tasks_dir, "tasks.md")
        baseline = os.path.join(root, "tools", "task_symbols_baseline.txt")

        def write(path: str, text: str) -> None:
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)

        def run_gate() -> tuple[int, str]:
            env = dict(os.environ, CLAY_TASK_SYMBOLS_ROOT=root)
            proc = subprocess.run([sys.executable, gate], cwd=root, env=env,
                                  capture_output=True, text=True)
            return proc.returncode, proc.stdout + proc.stderr

        for cmd in (["git", "init", "-q"],
                    ["git", "config", "user.email", "t@t"],
                    ["git", "config", "user.name", "t"]):
            subprocess.run(cmd, cwd=root, check=True, stdout=subprocess.DEVNULL)

        failures = []

        def expect(label: str, want_code: int, want_text: str) -> None:
            subprocess.run(["git", "add", "-A"], cwd=root, check=True,
                           stdout=subprocess.DEVNULL)
            code, out = run_gate()
            if code != want_code or (want_text and want_text not in out):
                failures.append(f"{label}: exit {code} (wanted {want_code}), output:\n{out}")

        # THE FIXTURE NAME MUST BE ONE NOTHING ELSE CITES. This file lives in
        # `tools/`, which the gate searches, so every symbol named literally
        # below is a real name in the real tree. Using a name that a real
        # baseline row records would make that row look paid off and retire it
        # -- which is exactly what happened when this fixture was first written
        # with a name a change had genuinely baselined.
        #
        # 1. The ORIGINAL rule: a cited name that is nowhere in the tree fails.
        write(src, "int unrelated() { return 0; }\n")
        write(tasks, "- [ ] build `ClaySelfTestOnlyMarker`\n")
        write(baseline, "# empty\n")
        expect("an unresolvable cited name must fail", 1, "no such symbol")

        # 2. Baselined, and still unbuilt: the row is doing its job, gate passes.
        write(baseline, "demo\tClaySelfTestOnlyMarker\n")
        expect("a baselined name that is still unbuilt must pass", 0, "1 baselined")

        # 3. THE FIX. The name gets built, so the row is now paid off and must
        #    be retired rather than left to exempt the name forever.
        write(src, "struct ClaySelfTestOnlyMarker { int x; };\n")
        expect("a baselined name that now resolves must fail", 1, "now resolves")

        # 4. THE OTHER HALF. The change stops citing the name, so the row
        #    records nothing at all.
        write(src, "int unrelated() { return 0; }\n")
        write(tasks, "- [ ] build something else entirely\n")
        expect("a baselined row nothing cites must fail", 1, "no longer cites")

        for failure in failures:
            print(failure)
        if failures:
            print(f"\ntask-symbols self-test: {len(failures)} of 4 checks failed")
            return 1
        print("task-symbols self-test: OK (4 checks, both directions)")
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
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
    used: set[tuple[str, str]] = set()
    paid: set[tuple[str, str]] = set()
    failures = []
    for change in changes:
        failures += check(change, dirs, baseline, used, paid)
    stale = stale_rows(baseline, used, paid, changes)
    for failure in failures + stale:
        print(failure)
    checked = len(changes)
    if failures or stale:
        if failures:
            print(f"\n{len(failures)} unresolved name(s) across {checked} change(s)")
        if stale:
            print(f"{len(stale)} stale baseline row(s): the debt is paid, so the record"
                  " must not keep granting it")
        return 1
    print(f"task symbols resolve in {checked} change(s)"
          + (f", {len(baseline)} baselined" if baseline else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
