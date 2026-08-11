#!/usr/bin/env python3
"""Binding parity gate (c-abi spec): the C ABI reaches what pyclay reaches.

The ABI fell behind the Python bindings once already, silently, because
nothing compared them. This compares them:

1. pyclay's capability surface, by importing the module and walking its
   classes, their own members and their enumerators. When the module is not
   built, the same surface is read out of the nanobind registration DSL in
   bindings/python/pyclay_module.cpp, so the gate runs on a bare checkout;
   when both are available they must agree, which keeps the parser honest.
2. The C surface, by parsing bindings/c/clay.h for declared functions and
   enumerators, the way a bindings generator reads it.
3. A mapping between them: an explicit alias table for the names that differ,
   a per-class prefix rule for the ones that do not, and an exemption list
   that states a reason per entry.

A pyclay capability with no C counterpart and no exemption fails the gate.
An exemption whose capability is now reachable, or no longer exists, also
fails: the list cannot rot into a record of things nobody rechecked.

Run: python3 tools/check_binding_parity.py [--pyclay DIR] [--require-import]
"""

import argparse
import importlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "bindings" / "c" / "clay.h"
MODULE_SOURCE = REPO / "bindings" / "python" / "pyclay_module.cpp"

# How a class's members are spelled in C. Tried in order; a member is
# satisfied when any prefix + the member name is a declared C function.
CLASS_PREFIX = {
    "Document": ("clay_document_", "clay_"),
    "Layer": ("clay_layer_",),
    "Mesh": ("clay_mesh_",),
    "VoxelGrid": ("clay_voxel_", "clay_voxel_grid_"),
    "MaskField": ("clay_mask_",),
    "StrokePreset": ("clay_stroke_preset_",),
    # An inspection surface with no C counterpart; every member is exempt with
    # a reason. See CLASS_CTOR['MeshQuery'].
    "MeshQuery": ("clay_mesh_",),
    "Prim": ("clay_item_", "clay_item_set_", "clay_item_add_"),
    "Stroke": ("clay_item_", "clay_item_set_", "clay_item_add_"),
    "Armature": ("clay_item_", "clay_item_set_", "clay_item_add_"),
    # Volume is an item like any other, so it would take the clay_item_
    # prefixes — but nothing in C builds or inspects one yet. Every member is
    # exempt with a reason; see CLASS_CTOR['Volume'].
    "Volume": ("clay_item_", "clay_item_set_", "clay_item_add_"),
    "Blend": ("clay_item_set_",),
    "Profile": (),
    "Op": (),
    "module": (),
}

# Classes that are a versioned DESCRIPTOR in C rather than a handle with
# entry points. Their members are satisfied by a field of the named struct: a
# C caller sets preset.spacing directly, so demanding a clay_*_spacing getter
# would be inventing surface that neither binding wants. A field still has to
# exist, which is what the gate is for.
CLASS_STRUCT = {
    "StrokePreset": "clay_stroke_preset",
}

# Classes whose members name an enumerator rather than an entry point: a new
# combine op or profile shape in pyclay needs one in clay.h.
CLASS_ENUM_PREFIX = {
    "Accumulation": "CLAY_ACCUMULATION_",
    # the shape kinds are an enumerator on the cut descriptor in C
    "CutShape": "CLAY_CUT_",
    "Op": "CLAY_OP_",
    "Profile": "CLAY_PROFILE_",
    "Prim": "CLAY_DEFORM_",  # the deformer chain; the rest falls to the prefixes
}

# The names that do not derive. Kept explicit so the difference is reviewable.
ALIASES = {
    "Document.eval": "clay_eval_points",
    "Document.colors": "clay_eval_points",  # the colors ride the same call
    "Document.gradients": "clay_eval_gradients",
    "Document.raycast": "clay_raycast_attributed",  # pyclay's reports layer+item too
    "Volume.from_mesh": "clay_item_volume_from_mesh",
    "Volume.from_document": "clay_item_volume_from_document",
    "Volume.relaxed": "clay_item_volume_relax",
    "Volume.flattened": "clay_item_volume_flatten",
    "Volume.flattened_from": "clay_item_volume_flatten_from",
    "Prim.magnify": "CLAY_DEFORM_MAGNIFY",
    "Prim.noise": "CLAY_DEFORM_NOISE",
    "Mesh.from_triangles": "clay_mesh_from_triangles",
    "Layer.add": "clay_layer_add_item",
    "Layer.set_points": "clay_layer_set_stroke_points",
    # closed and the tolerance are one call in C: they only mean anything
    # together, and a UI that exposes one exposes the other
    "Stroke.closed": "clay_item_set_curve",
    "Stroke.tolerance": "clay_item_set_curve",
    # per-point types ride the point arrays rather than being set separately
    "Stroke.types": "clay_item_set_curve_points",
    "CutShape.curve": "clay_cut_polygon_from_curve",
    # the preset is an argument in C, not the receiver, so the name has no
    # _preset_ in it
    "StrokePreset.resolve": "clay_stroke_resolve",
    "Layer.eval": "clay_layer_eval_points",
    "Layer.colors": "clay_layer_eval_points",
    "Layer.gradients": "clay_layer_eval_gradients",
    "Document.undo_enabled": "clay_document_undo_state",
    "Document.undo_depth": "clay_document_undo_state",   # one C query reports both
    "Document.redo_depth": "clay_document_undo_state",
    "Layer.mirror": "clay_set_layer_mirror",
    "Layer.remove": "clay_remove_node",  # named for the node, not the layer
    "Mesh.triangle_count": "clay_mesh_index_count",
    "Mesh.is_watertight": "clay_mesh_validate",
    "Mesh.is_manifold": "clay_mesh_validate",
    "Prim.at": "clay_item_set_position",
    "Stroke.add_point": "clay_item_add_stroke_point",
    "VoxelGrid.voxel_size": "clay_voxel_size",
    "Blend.k": "clay_item_set_blend",
    "module.load": "clay_document_load",
    "module.load_mesh": "clay_mesh_load",
    # A resolver, not a capability: it produces an ordinary stroke item, and the
    # C ABI builds those with clay_item_create + clay_item_set_curve_points.
    # Adding a C entry point would be a second way to say the same thing.
    "module.snakehook": "clay_item_set_curve_points",
    "CutShape.trim": "clay_cut_polygon_from_open_curve",
    "Volume.moved_topologically_from": "clay_item_volume_move_topological",
    "module.tube": "clay_tube_create",
    "module.backends": "clay_list_backends",
}

# What each class constructs, since a constructor is a capability of its own.
# None marks an abstract base or an enumeration: nothing to build.
CLASS_CTOR = {
    "Prim": None,
    "Blend": None,
    "Profile": None,
    "Transition": None,
    "Op": None,
    "Document": "clay_document_create",
    "Layer": "clay_add_sdf_layer",
    "Mesh": "clay_document_mesh",
    "VoxelGrid": "clay_voxel_grid_create",
    "MaskField": "clay_mask_create",
    "Accumulation": None,
    "StrokePreset": "clay_stroke_preset_defaults",
    "Cut": "clay_cut_create",
    # Not clay_item_create: a volume needs samples, and that entry point has
    # none to give. Its producer takes a mesh.
    "Volume": "clay_item_volume_from_mesh",
    # A BVH exposed so a SCRIPT can check that the sign behaves — that a hole
    # does not flip a half-space, that summarizing distant nodes does not move
    # anything across the surface. The capability an app needs, importing a
    # mesh as a field, is clay_item_volume_from_mesh; this is the microscope
    # pointed at it, not a second way to do it.
    "MeshQuery": None,
    "CutShape": None,
    "Smooth": "CLAY_BLEND_QUADRATIC",
    "Cubic": "CLAY_BLEND_CUBIC",
    "Circular": "CLAY_BLEND_CIRCULAR",
    "Chamfer": "CLAY_BLEND_CHAMFER",
    "TransitionLinear": "clay_item_set_transition_linear",
    "TransitionRadial": "clay_item_set_transition_radial",
    # primitives: the Python class name is not the enumerator's name
    "Sphere": "CLAY_PRIM_SPHERE",
    "Box": "CLAY_PRIM_BOX",
    "RoundBox": "CLAY_PRIM_ROUND_BOX",
    "Torus": "CLAY_PRIM_TORUS",
    "Capsule": "CLAY_PRIM_CAPSULE",
    "Cylinder": "CLAY_PRIM_CAPPED_CYLINDER",
    "Cone": "CLAY_PRIM_CAPPED_CONE",
    "RoundCone": "CLAY_PRIM_ROUND_CONE",
    "Ellipsoid": "CLAY_PRIM_ELLIPSOID",
    "Octahedron": "CLAY_PRIM_OCTAHEDRON",
    "HexPrism": "CLAY_PRIM_HEX_PRISM",
    "Pyramid": "CLAY_PRIM_PYRAMID",
    "CappedTorus": "CLAY_PRIM_CAPPED_TORUS",
    "Link": "CLAY_PRIM_LINK",
    "CylinderInfinite": "CLAY_PRIM_CYLINDER_INFINITE",
    "ExactCone": "CLAY_PRIM_CONE",
    "Plane": "CLAY_PRIM_PLANE",
    "CutSphere": "CLAY_PRIM_CUT_SPHERE",
    "CutHollowSphere": "CLAY_PRIM_CUT_HOLLOW_SPHERE",
    "SolidAngle": "CLAY_PRIM_SOLID_ANGLE",
    "Tetrahedron": "CLAY_PRIM_TETRAHEDRON",
    "Dodecahedron": "CLAY_PRIM_DODECAHEDRON",
    "Icosahedron": "CLAY_PRIM_ICOSAHEDRON",
    "TriPrism": "CLAY_PRIM_TRI_PRISM",
    "OctahedronCheap": "CLAY_PRIM_OCTAHEDRON_CHEAP",
    "LNormSphere": "CLAY_PRIM_LNORM_SPHERE",
    "Extrude": "CLAY_PRIM_EXTRUDE",
    "Revolve": "CLAY_PRIM_REVOLVE",
    "Loft": "CLAY_PRIM_LOFT",
    "Swept": "CLAY_PRIM_SWEPT",
    "Stroke": "CLAY_PRIM_STROKE",
    "Armature": "CLAY_PRIM_ARMATURE",
}

# Capabilities pyclay exposes that the C ABI deliberately does not, each with
# the reason. The gate prints these, so a Python-only capability stays visible
# instead of disappearing, and it fails when one becomes reachable in C or
# vanishes from pyclay.
EXEMPT = {
    "MeshQuery.distance": "an inspection surface, not a capability: C imports a "
                          "mesh with clay_item_volume_from_mesh and evaluates "
                          "through the document",
    "MeshQuery.winding_number": "lets a script check the sign behaves — the "
                                "property the import rests on; not something an "
                                "app asks for directly",
    "MeshQuery.signed_distance": "as above",
    "MeshQuery.contains": "as above",
    "MeshQuery.triangle_count": "reads back what was handed in, as elsewhere",

    "Volume.eval": "reads the sampled field back before it is placed, so a test "
                   "can tell a sampling error from a placement one; C evaluates "
                   "through the document",
    "Volume.has_samples_at": "inspects the sparse index, as above",
    "Volume.cell_size": "reads a builder's own state back, as above",
    "Volume.band": "reads a builder's own state back, as above",
    "Volume.brick_count": "reads a builder's own state back, as above",
    "Volume.sample_count": "reads a builder's own state back, as above",
    "Volume.megabytes": "reads a builder's own state back, as above",
    "Volume.bounds": "reads a builder's own state back, as above",
    "Volume.sample_lipschitz": "reads back what an operator declared about its own "
                               "result; in C the step scale it feeds is already "
                               "reachable through the document",
    "Prim.repeat": "reads a builder's own state back; clay_item is write-only by "
                   "design, the caller keeps what it set",
    "Prim.deformers": "reads a builder's own state back, as above",
    "CutShape.vertex_count":
        "reads a shape's own outline back; in C the caller owns the buffer it passed",
    "Stroke.points": "reads a builder's own state back, as above",
    "Stroke.point_count": "reads a builder's own state back, as above",
    "Armature.nodes": "reads a builder's own state back, as above",
    "Armature.parents": "reads a builder's own state back, as above",
    "Armature.node_count": "reads a builder's own state back, as above",
    "Profile.point_count": "reads a builder's own state back, as above",
    "Layer.name": "reads back the name the caller passed to clay_add_sdf_layer",
    "Layer.resolution": "per-layer meshing hint the C ABI does not author; "
                        "clay_mesh_params carries the resolution a mesh is built at",
    "Layer.id": "in C the id IS the handle — clay_layer_id is the type every "
                "layer entry point already takes, so there is nothing to read back",
}

# The OTHER direction, which this gate does not enforce and deliberately does
# not: the C ABI legitimately carries plumbing pyclay has no need of, so a
# C-only entry point is not a defect. A whole SUBSYSTEM reachable only from C
# is different — it is a gap somebody owes work on — so those are named here
# and printed on every run rather than being invisible because the comparison
# runs one way. An entry is a follow-up, not an exemption: nothing fails while
# one is listed, and removing it is the point.
C_ONLY_FOLLOW_UPS = {
    "BrickCache": "the incremental sculpting path (brick::BrickCache) is reachable from C "
                  "as clay_brick_cache_* since ABI 0.24.0 — mark_dirty, take_dirty, "
                  "eval_requests, submit — and from pyclay not at all. A Python binding "
                  "wants a buffer protocol for the fp16 payloads and a numpy view of a "
                  "request array, which is its own change with its own tests; until then "
                  "the incremental path is covered in C++ and C only, and a pyclay script "
                  "cannot reproduce an app's refill. This now covers the GPU-atlas surface "
                  "too — the RGBA8 colour lattice, the apron on the readback, subset "
                  "meshing with per-key ranges and the batched brick raycast (ABI 0.25.0) — "
                  "which lands in the same binding and wants the same buffer protocol.",
    "Device": "clay_device_adopt lends claycore the GPU device the CALLER already "
              "owns, so evaluation lands in the caller's own buffer instead of "
              "crossing host memory. A pyclay script has no device to lend: it is "
              "not a renderer, and numpy arrays are host memory by definition. If "
              "pyclay ever grows a CUDA-array-interface or dlpack path, this "
              "becomes a real gap and the exemption should go.",
    "Mesh.combine": "clay_mesh_transform, clay_mesh_concat and "
                    "clay_document_mesh_combined build one export out of the meshed "
                    "field and the visible mesh layers, in C, because a C host "
                    "otherwise writes the index-rebasing loop itself and has to get "
                    "the attribute-drop rule right on its own. pyclay does not need "
                    "them: the attribute arrays support the buffer protocol, so "
                    "numpy concatenates and offsets them without crossing the "
                    "boundary — which is exactly what examples/36_mesh_layers.py "
                    "does. If pyclay ever wants the drop rule enforced rather than "
                    "hand-written, this becomes a real gap.",
    "Mesh.copy_vertices": "clay_mesh_copy_vertices interleaves a mesh into memory the "
                          "CALLER owns, which exists so a host writes one pass into a "
                          "mapped GPU buffer. Python's equivalent is already there and is "
                          "better: the existing attribute arrays support the buffer "
                          "protocol, so numpy interleaves them without crossing the "
                          "boundary at all. A pyclay wrapper would be a slower way to do "
                          "what numpy does.",
}

# String-valued choices pyclay parses out of an argument. They are capabilities
# too — a new brush shape is a new thing the C ABI must be able to name — and
# they land on C enumerators rather than entry points.
STRING_CHOICES = (
    # (pyclay function, what the strings select, C enumerator prefix)
    ("parse_brush_shape", "brush shape", "CLAY_BRUSH_SHAPE_"),
    ("parse_falloff", "brush falloff", "CLAY_BRUSH_FALLOFF_"),
    ("parse_axis", "mirror axis", "CLAY_MIRROR_"),
    ("parse_extrude_side", "mask extrude side", "CLAY_EXTRUDE_"),
    ("mesh_document", "mesher", "CLAY_MESHER_"),
)


# -- the C side ----------------------------------------------------------------


def c_surface() -> tuple[set[str], set[str], set[str]]:
    """Declared functions, enumerators and struct fields, as a bindings
    generator reads them. Struct fields arrive as "struct.field" so a
    descriptor-backed class can resolve onto them."""
    text = HEADER.read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    functions = set(re.findall(r"\b(clay_\w+)\s*\(", text))
    enumerators = set(re.findall(r"\b(CLAY_[A-Z0-9_]+)\s*=", text))
    fields = set()
    # The typedef's trailing name is the one a caller uses; the tag may be
    # empty, so key off the typedef name rather than the struct tag.
    for body, name in re.findall(r"typedef struct \w*\s*{([^}]*)}\s*(\w+)\s*;", text):
        for field in re.findall(r"\b(\w+)\s*(?:\[\s*\d+\s*\])?\s*;", body):
            fields.add(f"{name}.{field}")
    return functions, enumerators, fields


# -- the pyclay side -----------------------------------------------------------


def imported_surface(module) -> dict[str, list[str]]:
    """Classes and the members each one declares itself, from the module."""
    surface: dict[str, list[str]] = {}
    for name in sorted(dir(module)):
        if name.startswith("_"):
            continue
        obj = getattr(module, name)
        if isinstance(obj, type):
            surface[name] = sorted(m for m in vars(obj) if not m.startswith("_"))
        elif callable(obj):
            surface.setdefault("module", []).append(name)
    surface["module"] = sorted(surface.get("module", []))
    return surface


def parsed_surface() -> dict[str, list[str]]:
    """The same surface read out of the nanobind registration DSL.

    Each .def/.def_prop_ro/.value belongs to the most recent nb::class_ or
    nb::enum_ above it, which is exactly how nanobind attributes them.
    """
    text = MODULE_SOURCE.read_text()
    declare = re.compile(r'nb::(?:class_|enum_)<[^>]*>\(\s*\n?\s*m,\s*"(\w+)"')
    # (?<!m) so a module-level m.def is not read as a member of the class above
    member = re.compile(r'(?<!m)\.(?:def|def_prop_ro_static|def_prop_ro|def_prop_rw|def_static|def_ro|def_rw|value)'
                        r'\(\s*\n?\s*"(\w+)"')
    module_fn = re.compile(r'^\s*m\.def\(\s*\n?\s*"(\w+)"', re.M)
    surface: dict[str, list[str]] = {"module": sorted(module_fn.findall(text))}
    events = [(m.start(), "class", m.group(1)) for m in declare.finditer(text)]
    events += [(m.start(), "member", m.group(1)) for m in member.finditer(text)]
    current = None
    for _, kind, name in sorted(events):
        if kind == "class":
            current = name
            surface.setdefault(current, [])
        elif current and not name.startswith("_"):
            surface[current].append(name)
    return {k: sorted(set(v)) for k, v in surface.items()}


def load_module(explicit: Path | None):
    """Import pyclay from an explicit directory, the path, or a build tree.

    Several build trees can hold a pyclay, and an older one answering first
    would compare the C ABI against a surface nobody ships; the newest
    artifact wins.
    """
    found = sorted((REPO / "build").glob("*/bindings/python/pyclay*"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    ordered = ([explicit] if explicit else []) + [p.parent for p in found]
    for path in reversed(ordered):  # inserting at 0 reverses, so undo that here
        if path and path.is_dir():
            sys.path.insert(0, str(path))
    try:
        return importlib.import_module("pyclay")
    except ImportError:
        return None


def compare_surfaces(imported: dict, parsed: dict) -> list[str]:
    """The parser and the module must see the same thing."""
    errors = []
    for name in sorted(set(imported) | set(parsed)):
        got, want = set(imported.get(name, [])), set(parsed.get(name, []))
        if got != want:
            errors.append(f"the pyclay_module.cpp parser and the built module disagree on "
                          f"{name}: parsed-only {sorted(want - got)}, imported-only "
                          f"{sorted(got - want)}")
    return errors


# -- the mapping ---------------------------------------------------------------


def candidates(cls: str, member: str) -> list[str]:
    """Every C name that would satisfy this member, in the order tried."""
    out = []
    if cls in CLASS_ENUM_PREFIX:
        out.append(CLASS_ENUM_PREFIX[cls] + member.upper())
    if cls in CLASS_STRUCT:
        out.append(f"{CLASS_STRUCT[cls]}.{member}")
    out += [prefix + member for prefix in CLASS_PREFIX.get(cls, ())]
    return out


def resolve(capability: str, cls: str, member: str, declared: set[str]) -> str | None:
    """The C token that satisfies a capability, or None when nothing does."""
    alias = ALIASES.get(capability)
    if alias:
        return alias if alias in declared else None
    return next((c for c in candidates(cls, member) if c in declared), None)


def classify_member(cls: str, member: str, declared: set[str]) -> tuple[str | None, str | None]:
    """One member's verdict: (error, exemption it used), at most one set."""
    capability = f"{cls}.{member}"
    found = resolve(capability, cls, member, declared)
    if capability in EXEMPT:
        if found:
            return (f"the exemption for {capability} is stale: {found} exists now, so drop "
                    f"the entry from EXEMPT", None)
        return (None, capability)
    if found:
        return (None, None)
    tried = ALIASES.get(capability) or ", ".join(candidates(cls, member)) or "nothing"
    return (f"pyclay exposes {capability} with no C entry point (tried {tried}); add one or "
            f"record an exemption in tools/check_binding_parity.py", None)


def check_members(surface: dict, declared: set[str]) -> tuple[list[str], set[str]]:
    """Every member of every class, against the C surface."""
    errors, used_exemptions = [], set()
    for cls in sorted(surface):
        if cls not in CLASS_PREFIX and cls not in CLASS_ENUM_PREFIX and surface[cls]:
            errors.append(f"pyclay class {cls} declares {surface[cls]} and no rule says how "
                          f"its members are spelled in C; add it to CLASS_PREFIX in "
                          f"tools/check_binding_parity.py")
            continue
        for member in surface[cls]:
            error, exemption = classify_member(cls, member, declared)
            if error:
                errors.append(error)
            if exemption:
                used_exemptions.add(exemption)
    return errors, used_exemptions


def check_constructors(surface: dict, declared: set[str]) -> list[str]:
    """Every class is something a C consumer can build, or is declared abstract."""
    errors = []
    for cls in sorted(surface):
        if cls == "module":
            continue
        if cls not in CLASS_CTOR:
            errors.append(f"pyclay exposes class {cls} and nothing records what builds it in "
                          f"C; add it to CLASS_CTOR in tools/check_binding_parity.py")
        elif CLASS_CTOR[cls] and CLASS_CTOR[cls] not in declared:
            errors.append(f"pyclay's {cls} is built in C by {CLASS_CTOR[cls]}, which clay.h "
                          f"does not declare")
    return errors


def check_string_choices(declared: set[str]) -> list[str]:
    """The string-valued choices pyclay accepts all name a C enumerator."""
    text = MODULE_SOURCE.read_text()
    errors = []
    for function, what, prefix in STRING_CHOICES:
        body = re.search(r"\b%s\(.*?\n}" % function, text, re.S)
        if not body:
            errors.append(f"cannot find {function} in pyclay_module.cpp to read its "
                          f"{what} choices")
            continue
        choices = {c.lower() for c in re.findall(r'[!=]=\s*"(\w+)"', body.group(0))}
        for choice in sorted(choices):
            if prefix + choice.upper() not in declared:
                errors.append(f"pyclay accepts {what} '{choice}' and clay.h declares no "
                              f"{prefix}{choice.upper()}")
    return errors


def unreached(surface: dict, functions: set[str]) -> list[str]:
    """C entry points no pyclay capability maps onto — reported, never fatal.

    The C ABI legitimately carries plumbing pyclay has no need of (error
    details, explicit destroys, the item builder's chained setters), so this
    is a note, not a rule.
    """
    reached = {CLASS_CTOR.get(cls) for cls in surface}
    for cls, members in surface.items():
        for member in members:
            reached.add(resolve(f"{cls}.{member}", cls, member, functions))
    return sorted(functions - {r for r in reached if r})


# -- driver --------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pyclay", type=Path, help="directory holding the built pyclay module")
    parser.add_argument("--require-import", action="store_true",
                        help="fail instead of falling back to parsing pyclay_module.cpp")
    parser.add_argument("--show-unreached", action="store_true",
                        help="list C entry points no pyclay capability maps onto")
    args = parser.parse_args()

    functions, enumerators, struct_fields = c_surface()
    declared = functions | enumerators | struct_fields
    parsed = parsed_surface()
    module = load_module(args.pyclay)
    errors = []
    if module:
        surface, source = imported_surface(module), f"imported {module.__file__}"
        errors += compare_surfaces(surface, parsed)
    elif args.require_import:
        print("parity: pyclay could not be imported and --require-import was given",
              file=sys.stderr)
        return 1
    else:
        surface, source = parsed, f"parsed {MODULE_SOURCE.relative_to(REPO)}"

    member_errors, used = check_members(surface, declared)
    errors += member_errors
    errors += check_constructors(surface, declared)
    errors += check_string_choices(declared)
    for stale in sorted(set(EXEMPT) - used):
        errors.append(f"EXEMPT lists {stale}, which pyclay no longer exposes: drop the entry "
                      f"from tools/check_binding_parity.py")

    for e in errors:
        print(f"parity: {e}", file=sys.stderr)
    for capability in sorted(used):
        print(f"parity: exempt {capability} — {EXEMPT[capability]}")
    for capability in sorted(C_ONLY_FOLLOW_UPS):
        print(f"parity: C-only, pyclay follow-up — {capability}: "
              f"{C_ONLY_FOLLOW_UPS[capability]}")
    if args.show_unreached:
        for name in unreached(surface, functions):
            print(f"parity: C-only {name}")
    if errors:
        return 1
    total = sum(len(v) for v in surface.values()) + len([c for c in surface if c != "module"])
    print(f"parity: OK ({total} pyclay capabilities, {len(used)} exempt, {source})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
