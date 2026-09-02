#!/usr/bin/env python3
"""Module-layering include check (openspec build-packaging).

Enforces the dependency rule: kernel depends on nothing; scene/brick/mesh/
voxel/pick depend only on kernel+math; backends depend on eval; io and
bindings sit on top; no module depends on a backend.

Scans #include directives for clay/<module>/ paths in include/, src/,
backends/, bindings/, and tools/ and validates each edge.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# module -> modules it may include (itself always allowed)
ALLOWED = {
    # The data-parallel primitive, below everything and depending on nothing but
    # the standard library. It was a PRIVATE HEADER OF THE CPU BACKEND, which
    # meant the rule below — no module depends on a backend — locked the core
    # library out of the only pool in the tree: every mesher, every voxel verb,
    # redistance and the per-brick cull were serial because they could not
    # legally reach it, not because they resist parallelism. Moving it here is
    # what lets a core module use it AND lets this gate see that it does.
    "parallel": set(),
    # The memory budget, the ledger, the trim vocabulary and the scratch arena,
    # below everything and depending on nothing but the standard library. It is
    # a leaf for the same reason `parallel` is one: `mesh` has to consult a
    # budget on every stamp — the scratch hard bound, what may be deferred, what
    # is resident, what an operation is allowed to peak at — and `io` is the TOP
    # of this table, so a profile living beside `io::MemoryReport` would mean
    # either a `mesh -> io` edge that makes the table cyclic or a byte count
    # threaded through every call signature from the host down, which puts
    # residency policy in the host. `scene` was the other candidate and is worse
    # in a subtler way: `scene` is what gets SERIALIZED, so a device budget
    # landing there would drift into the file format and travel with a document
    # to another machine.
    "memory": set(),
    "kernel": set(),  # the GPU dialect: no host threading, deliberately
    "math": {"parallel", "kernel"},
    "scene": {"parallel", "kernel", "math", "field"},
    # eval -> field is the batched document bake (bake_volume.h): the block
    # fill that `field::FieldVolume::sample_blocks` wants, filled by asking
    # the CPU backend for a whole window of points at once. It names a tape
    # AND a backend, so it cannot live in `scene` or below; `field` is the
    # type it fills. It adds no edge to the transitive graph — eval already
    # depends on scene, and scene depends on field — and creates no cycle,
    # because field depends on nothing above kernel and math.
    "eval": {"parallel", "kernel", "math", "scene", "field"},
    "brick": {"memory", "parallel", "kernel", "math", "scene", "eval"},
    # voxel -> field is the return trip (#90): a sculpt converting into a
    # sampled field so it can be an operand again. It adds no edge to the
    # transitive graph — voxel already depends on scene, and scene depends on
    # field — and it creates no cycle, because field depends on nothing above
    # kernel and math. The alternative homes were worse: brush and mesh can
    # both see field, but a representation conversion is neither a brush nor a
    # mesher, and putting it there would hide it from the type that owns the
    # cells.
    "voxel": {"memory", "parallel", "kernel", "math", "scene", "mesh", "field"},  # mesh_data.h is a leaf data type
    "mesh": {"memory", "parallel", "kernel", "math", "scene", "eval", "brick", "field"},
    # brush -> field is mask extrude: the join of a mask (above scene) and a
    # sampled field (below it). It cannot live in field without making
    # field -> voxel -> scene -> field a cycle, and brush already sits above
    # both. Nothing in field knows about brush.
    # brush -> mesh is apply_to_mesh, the stroke engine's fourth consumer: a
    # resolved stroke stamped onto a mesh layer's own vertices. It is the one
    # call that sees both a mesh and a mask, which is exactly why it lives
    # here — mesh may not include voxel (voxel already includes mesh), so a
    # masked mesh brush cannot live in mesh. No cycle: nothing in mesh knows
    # about brush.
    "brush": {"parallel", "kernel", "math", "scene", "voxel", "field", "mesh"},
    "cut": {"parallel", "kernel", "math", "scene"},
    "field": {"parallel", "kernel", "math"},  # a sampled field is a leaf payload, below scene
    # pick -> mesh is raycast_mesh. A mesh layer never enters a tape, so
    # raycast_scene cannot see one and never will; picking one means asking its
    # BVH directly. Nothing in mesh knows about pick.
    "pick": {"parallel", "kernel", "math", "scene", "eval", "brick", "voxel", "mesh"},
    # session -> scene + voxel + mesh is the ONE undo history (unify-the-undo-
    # history). scene may not include voxel or mesh, so the history that
    # reverses a voxel pass AND a vertex delta cannot live beside UndoStack;
    # brush is the only module that already sees all three and is the stroke
    # engine, not a history. So it gets its own module above the three, the way
    # `parallel` got one when the layering rule put the thread pool out of
    # reach. It depends on nothing above it: the object that OWNS the three
    # (io::ClaySpaceDoc) sits above, so the resolvers are passed IN.
    # session -> brush is the SDF sculpt transaction (add-sdf-sculpt-transaction):
    # a live Move drag prepares brush::PreparedMove once and resolves one warp
    # per pointer event, and it must reuse brush's resolver rather than grow a
    # second one — a preview computed by different arithmetic than the commit is
    # a preview of something else. It adds no edge to the transitive graph:
    # brush's own set is already a subset of session's, and brush knows nothing
    # about session, so there is no cycle. A transaction is not a stroke engine,
    # which is why it does not simply live in brush: its subject is the GESTURE
    # lifetime — begin, update, commit — and that is what `session` is for.
    # session -> eval is the prefix field cache (add-sdf-prefix-cache): a cached
    # prefix is only worth having if the suffix that continues it can be
    # evaluated onto it, and `eval::eval_points_seeded` is the one function that
    # does that. Free, like the brush edge beside it: eval's own set
    # {parallel, kernel, math, scene, field} is already a subset of session's,
    # and nothing in eval includes session. It is also the ordinary shape for a
    # module at this height -- mesh, pick and io all depend on eval, and
    # src/mesh reaches the backend registry exactly as this does. The INJECTION
    # pattern (scene::BakePointEval) exists for the opposite case, a module
    # BELOW eval that must not name it; session is above.
    "session": {"memory", "parallel", "kernel", "math", "scene", "voxel", "mesh", "field",
                "brush", "eval"},
    "io": {"memory", "parallel", "kernel", "math", "scene", "eval", "brick", "voxel", "mesh",
           "field", "session"},
}
CORE_MODULES = set(ALLOWED)
INCLUDE_RE = re.compile(r'#\s*include\s*[<"]clay/(\w+)/')
SCAN_ROOTS = ["include", "src", "backends", "bindings", "tools"]
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".cc", ".cu", ".mm", ".metal", ".cl"}


def module_of(path: Path) -> str | None:
    """Return the layering identity of a file, or None if unconstrained."""
    rel = path.relative_to(REPO)
    parts = rel.parts
    if parts[0] == "include" and len(parts) > 3 and parts[1] == "clay":
        return parts[2] if parts[2] in CORE_MODULES else None
    if parts[0] == "src" and len(parts) > 2:
        return parts[1] if parts[1] in CORE_MODULES else None
    if parts[0] == "backends":
        return "backend"
    if parts[0] == "bindings":
        return "binding"
    return None  # top-level src files, tools: unconstrained except backend ban


def main() -> int:
    errors = []
    for root in SCAN_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            mod = module_of(path)
            text = path.read_text(errors="replace")
            for target in INCLUDE_RE.findall(text):
                rel = path.relative_to(REPO)
                if target not in CORE_MODULES and target != "version":
                    errors.append(f"{rel}: includes unknown module 'clay/{target}/'")
                elif mod in CORE_MODULES and target in CORE_MODULES:
                    if target != mod and target not in ALLOWED[mod]:
                        errors.append(f"{rel}: module '{mod}' may not include 'clay/{target}/'")
                # backends may include any clay header; bindings sit on top: all allowed.
            if "backends/" not in str(path.relative_to(REPO)) and re.search(
                r'#\s*include\s*[<"]\.\./backends|[<"]backends/', text
            ):
                errors.append(f"{path.relative_to(REPO)}: no module may include backend headers")

    for e in errors:
        print(f"layering: {e}", file=sys.stderr)
    if not errors:
        print("layering: OK")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
