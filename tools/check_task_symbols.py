#!/usr/bin/env python3
"""Every identifier and file a change's tasks.md names in backticks must exist.

A tasks.md is a checklist someone else executes. When it names `foo_bar()` or
`tests/unit/test_x.cpp`, the reader goes looking for it -- so a name that is not
in the tree is either a typo or a promise dressed up as a citation, and both
waste the reader's time in the same way. The rule this enforces is: cite what
exists, and describe what does not exist in prose.

Only spans that unambiguously LOOK like a symbol, a path or a filename are
checked. Prose in backticks, version numbers, flags and expressions are skipped,
because guessing at those produces false failures and a gate nobody trusts gets
disabled.

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
FILENAME = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.+-]*\.([A-Za-z][A-Za-z0-9]*)$")

# A bare filename is only recognised by an extension this repository actually
# uses. The shape alone is too generous: `0.88.0` and `report.moved_vertices`
# are both a word, a dot and a word, and neither is a file. An unknown
# extension is left to the "no opinion" branch, which is the safe direction.
EXTENSIONS = {
    "cpp", "h", "hpp", "cc", "c", "cu", "cl", "metal", "mm", "swift", "inc",
    "py", "sh", "js", "html", "md", "txt", "toml", "yaml", "yml", "json",
    "cmake", "in", "plist", "clayspace",
}

# Words that pass the identifier shape and are English, not code.
PROSE = {
    "and", "or", "not", "the", "a", "an", "is", "it", "no", "yes", "true", "false",
    "TODO", "NOT", "DONE", "ALREADY", "READ", "MEASURE", "DECISION", "x", "n", "k",
}


def is_path(span: str) -> bool:
    return "/" in span and PATHLIKE.match(span) is not None


def is_filename(span: str) -> bool:
    """A filename cited without any directory, e.g. `cross_level.h`.

    Such a span used to satisfy NEITHER predicate -- `is_path` wants a "/" and
    IDENT forbids "." -- so the gate skipped it in silence, which is the one
    outcome a gate must never have. A reader goes looking for a cited filename
    exactly as they go looking for a cited path, so it is checked the same way,
    as a basename anywhere in the tracked tree.
    """
    match = FILENAME.match(span)
    return match is not None and match.group(1).lower() in EXTENSIONS


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
    """Every component of a qualified name must be present, in ONE file.

    Searching for the trailing identifier alone read none of the qualification
    it was given: an invented class in front of a real member resolved off the
    member, so the gate advertised qualified names and verified bare ones. The
    check is not a literal search for the span either, because a member is
    almost never written qualified where it is declared -- a class body holds
    the member name and the class name on separate lines and joins them nowhere.
    Requiring ONE file to contain every component is what a class and its
    member, or a namespace and its type, actually look like on disk.

    No fake name is written literally here. This file is under `tools/`, which
    the gate searches, so a name spelled out in this docstring would resolve
    against it -- the same self-reference the baseline exclusion exists to stop.
    """
    patterns = []
    for part in name.rstrip("()").split("::"):
        patterns += ["-e", part]
    proc = subprocess.run(
        ["git", "grep", "-l", "-w", "-F", "--all-match"] + patterns + ["--"] + dirs,
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return proc.returncode == 0


def tracked_basenames() -> set[str]:
    """Every tracked file's basename. Unlike the symbol haystack this includes
    openspec/, because a cited `design.md` IS the change's own design document
    and citing it is not a promise about built code."""
    if not hasattr(tracked_basenames, "cache"):
        proc = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True,
                              text=True)
        tracked_basenames.cache = {os.path.basename(line)
                                   for line in proc.stdout.splitlines() if line}
    return tracked_basenames.cache


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


def unresolved(span: str, dirs: list[str]) -> str | None:
    """Why this span names nothing in the tree, or None when it resolves."""
    if is_path(span):
        return None if os.path.exists(os.path.join(ROOT, span)) else f"no such file `{span}`"
    if is_filename(span):
        return None if span in tracked_basenames() else f"no such file `{span}`"
    if is_ident(span):
        return None if find_symbol(span, dirs) else f"no such symbol `{span}`"
    # No recognised shape: the gate never had an opinion on it, so it cannot
    # be debt either.
    return None


def resolves(span: str, dirs: list[str]) -> bool:
    """Does this span name something that is actually in the tree right now?"""
    return unresolved(span, dirs) is None


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
                reason = unresolved(span, dirs)
                if reason:
                    failures.append(f"{change}/tasks.md:{lineno}: {reason}")
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
        checks = 0

        def expect(label: str, want_code: int, want_text: str) -> None:
            nonlocal checks
            checks += 1
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

        # 5. THE QUALIFICATION IS READ. The member exists and the class it is
        #    hung off does not, which is precisely what a stale or invented
        #    qualified name looks like -- and what searching for the trailing
        #    identifier alone could never see.
        write(baseline, "# empty\n")
        write(src, "int self_test_member = 0;\n")
        write(tasks, "- [ ] call `ClaySelfTestOnlyHolder::self_test_member`\n")
        expect("an invented qualifier must fail", 1,
               "no such symbol `ClaySelfTestOnlyHolder::self_test_member`")

        # 6. The same span, with the class now present alongside the member:
        #    the failure above was about the qualifier and nothing else.
        write(src, "struct ClaySelfTestOnlyHolder { int self_test_member; };\n")
        expect("a qualified name whose every part is present must pass", 0,
               "task symbols resolve")

        # 7. A BARE FILENAME IS A CITATION TOO. It satisfies neither the path
        #    shape (no "/") nor the identifier shape (a "."), so it used to be
        #    skipped in silence -- a reader still goes looking for it.
        write(tasks, "- [ ] read `clay_self_test_only.cpp`\n")
        expect("an invented bare filename must fail", 1,
               "no such file `clay_self_test_only.cpp`")

        # 8. The file exists, under a directory the citation never named, and
        #    the tree is clean again.
        write(os.path.join(root, "src", "clay_self_test_only.cpp"), "// present\n")
        expect("a bare filename that is in the tree must pass", 0,
               "task symbols resolve")

        for failure in failures:
            print(failure)
        if failures:
            print(f"\ntask-symbols self-test: {len(failures)} of {checks} checks failed")
            return 1
        print(f"task-symbols self-test: OK ({checks} checks, both directions)")
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
