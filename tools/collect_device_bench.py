#!/usr/bin/env python3
"""Merge the device harness's attachments into one run record.

The harness emits one JSON attachment per test case class, because a test is
the unit XCTest gives us and there is no shared teardown that could write a
single file. This merges them into the one record `check_device_bench.py`
compares against a baseline.

Two fields are stamped HERE rather than on the device, because the device does
not know them: the claycore commit and the xcframework's identity. A baseline
that cannot say what produced it is not comparable to anything.

    tools/collect_device_bench.py <results.xcresult> <out.json>
"""

import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def export_attachments(xcresult: pathlib.Path, into: pathlib.Path) -> list[dict]:
    """Export every attachment and return the manifest."""
    subprocess.run(
        ["xcrun", "xcresulttool", "export", "attachments",
         "--path", str(xcresult), "--output-path", str(into)],
        check=True, capture_output=True)
    manifest = into / "manifest.json"
    if not manifest.exists():
        return []
    return json.loads(manifest.read_text())


def git_commit() -> str | None:
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def merge(records: list[dict]) -> dict:
    """One run record from many. Cases concatenate; the run-level fields must
    agree, because two tests in one run measured the same device."""
    if not records:
        raise SystemExit("device-bench: no device-bench.json attachments found. "
                         "Did the harness run at all?")

    merged = dict(records[0])
    merged["cases"] = []
    for record in records:
        for field in ("deviceModel", "osVersion", "abiVersion"):
            if record.get(field) != merged.get(field):
                raise SystemExit(
                    f"device-bench: attachments disagree on {field} "
                    f"({record.get(field)!r} vs {merged.get(field)!r}); "
                    "they cannot be from one run")
        merged["cases"].extend(record.get("cases", []))

    # A run is valid only if EVERY part of it was: one test that started
    # nominal and ended fair invalidates the run it belongs to, because the
    # cases after it were measured on throttled hardware.
    merged["valid"] = all(r.get("valid", False) for r in records)
    if any(r.get("thermalStateEnd") != "nominal" for r in records):
        merged["thermalStateEnd"] = next(
            r["thermalStateEnd"] for r in records
            if r.get("thermalStateEnd") != "nominal")
    return merged


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    xcresult = pathlib.Path(sys.argv[1])
    out_path = pathlib.Path(sys.argv[2])
    if not xcresult.exists():
        print(f"device-bench: no result bundle at {xcresult}", file=sys.stderr)
        return 1

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="clay-bench-"))
    try:
        manifest = export_attachments(xcresult, tmp)
        records = []
        for entry in manifest:
            for attachment in entry.get("attachments", []):
                name = attachment.get("suggestedHumanReadableName", "")
                if not name.startswith("device-bench"):
                    continue
                path = tmp / attachment["exportedFileName"]
                records.append(json.loads(path.read_text()))
        # Renders come out of the same bundle. They are the half of the result
        # that can be looked AT: a brush that got fast by doing nothing passes
        # its latency case and produces an obviously wrong picture.
        gallery = out_path.parent / "gallery"
        gallery.mkdir(parents=True, exist_ok=True)
        for old in gallery.glob("*.png"):
            old.unlink()
        for entry in manifest:
            for attachment in entry.get("attachments", []):
                name = attachment.get("suggestedHumanReadableName", "")
                if not name.startswith("gallery-"):
                    continue
                src = tmp / attachment["exportedFileName"]
                # xcresulttool appends "_<index>_<uuid>" before the extension.
                # Strip exactly that: splitting on "_" instead collapsed
                # gallery-stroke_build and gallery-stroke_carve onto one name,
                # and one silently overwrote the other.
                stem = re.sub(r"_\d+_[0-9A-Fa-f-]{36}(?=\.png$)", "", name)
                if not stem.endswith(".png"):
                    stem += ".png"
                shutil.copyfile(src, gallery / stem)
        rendered = sorted(p.name for p in gallery.glob("*.png"))
        merged = merge(records)
        merged["gallery"] = rendered
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    merged["claycoreCommit"] = git_commit()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(merged, indent=2, sort_keys=True) + "\n")

    print(f"device-bench: {len(merged['cases'])} case(s) -> {out_path}")
    if merged.get("gallery"):
        print(f"  {len(merged['gallery'])} render(s) -> "
              f"{out_path.parent / 'gallery'}: {', '.join(merged['gallery'])}")
    print(f"  device {merged['deviceModel']}, {merged['osVersion']}")
    if not merged["valid"]:
        print(f"  INVALID: thermal {merged['thermalStateStart']}"
              f" -> {merged['thermalStateEnd']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
