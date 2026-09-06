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
its parts -- a struct field and its function both have to exist, and neither is
grep-able as one token.

The match is a WHOLE WORD, which is the difference between this and the obvious
version. A substring match passes a name that is merely PART of a real symbol,
and that is the nastiest way for a claim to be wrong: a task naming
`an_unworked_session_still_exports` when the test is
`an_unworked_session_still_exports_every_phase` greps to something and reads as
correct, while a reader searching for it finds a deletion that never happened.
Whole-word matching costs one thing in exchange, measured on this tree: of 238
claimed identifiers exactly one was a SHORTHAND -- `lattice_gizmo_preview` for
`clay_layer_lattice_gizmo_preview` -- and the fix is to write the symbol in
full, which is what a claim should have said anyway.

KNOWN LIMITATION, and it is not fixable from here: a backticked name is a claim,
so you cannot DISCUSS a dead symbol in a file this tool polices. Correcting a
line means naming the wrong symbol unquoted, with a note saying why.

WHEN THIS FAILS, the cause is usually one of five, in the order they occur.
Triage before fixing: a spike is usually one refactor rather than one change's
worth of sloppiness.

  1. A FILE GREW INTO A DIRECTORY. `foo.cpp` became `foo/`, and every task
     citing the old path went stale in one commit. This is the dominant decay
     mode for a FILE claim and it has no symbol equivalent, because splitting a
     module keeps its symbols and moves its path. Nobody was careless.
  2. A SHORTHAND. The tail or head of a longer symbol, written to avoid
     repeating it. The fix is to name the symbol in full, which is what the
     claim should have said.
  3. A PREFIX. The nastiest, and the reason the match is a whole word: a name
     that is PART of a real symbol greps to something and reads as correct,
     while a reader searching for it finds a deletion that never happened.
  4. NOT A FILE IN THIS TREE. Either it lives in another repository, or it is a
     name that only exists at runtime -- a default the UI offers a person, an
     output path. Both are legitimate claims about the world and illegitimate
     claims about this checkout; name the repository, or say it is a runtime
     name, and leave it unquoted.
  5. IT WAS NEVER WRITTEN. The case this tool exists for, and the rarest.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
# A name ending in one of these is a FILE, and a file is checked as a path
# rather than as a token: nothing in the tree's contents says
# `tests/unit/test_layer_parity.cpp` exists, the file does. Skipping them, which
# this tool did first, means a task can name a test file that was never written
# and nothing says so — the claim is "this artefact exists", and an artefact is
# a file as often as it is a symbol.
FILE_SUFFIX = (".cpp", ".h", ".py", ".md", ".json", ".clayspace", ".swift", ".sh", ".yml", ".toml",
               ".rs", ".txt", ".cmake", ".metal", ".cl", ".glsl", ".hpp")
# This file is excluded from its own search. The docstring above NAMES a
# shorthand as an example, and without the exclusion that mention satisfies the
# claim it exists to describe — the quoting trap, arriving inside the tool that
# documents the quoting trap.
SEARCH_ARGS = ["--exclude-dir=.git", "--exclude-dir=build", "--exclude-dir=openspec",
               "--exclude=check_task_symbols.py"]


def claimed_files(text):
    """Backticked names that end in a source suffix, as basenames."""
    out = set()
    for name in re.findall(r"`([A-Za-z_][A-Za-z0-9_:./-]*)`", text):
        if name.endswith(FILE_SUFFIX):
            out.add(name.rsplit("/", 1)[-1])
    return out


def claimed_symbols(text):
    out = set()
    for name in re.findall(r"`([A-Za-z_][A-Za-z0-9_:.]*)`", text):
        if name.endswith(FILE_SUFFIX):
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
    files_on_disk = {p.name for p in ROOT.rglob("*")
                     if p.is_file() and ".git" not in p.parts and "build" not in p.parts}
    for tasks in sorted(ROOT.glob("openspec/changes/*/tasks.md")):
        text = tasks.read_text()
        for claimed in sorted(claimed_files(text)):
            checked += 1
            if claimed not in files_on_disk:
                missing.append(f"{tasks.relative_to(ROOT)}: `{claimed}` is claimed as a file "
                               f"and no file of that name is in the tree")
        names = claimed_symbols(text)
        checked += len(names)
        for name in sorted(names):
            found = subprocess.run(["grep", "-rqIw", *SEARCH_ARGS, "--", name, str(ROOT)],
                                   capture_output=True)
            if found.returncode != 0:
                part = subprocess.run(["grep", "-rqI", *SEARCH_ARGS, "--", name, str(ROOT)],
                                      capture_output=True)
                why = ("is a shorthand: it appears only INSIDE a longer symbol, so write "
                       "that symbol in full" if part.returncode == 0
                       else "is nowhere in the tree")
                missing.append(f"{tasks.relative_to(ROOT)}: `{name}` is claimed and {why}")
    if missing:
        print("task-symbols: FAIL")
        for m in missing:
            print(f"  {m}")
        return 1
    print(f"task-symbols: OK ({checked} claimed identifiers, all present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
