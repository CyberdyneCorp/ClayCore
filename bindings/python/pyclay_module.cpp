// pyclay — nanobind extension module (python-bindings spec): numpy-native
// document authoring, field evaluation, meshing, and file I/O over the
// public claycore C++ API. This target compiles WITH exceptions (Python
// error protocol); the core library itself never throws.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "clay/brush/gate_bake.h"
#include "clay/brush/mask_extrude.h"
#include "clay/brush/lattice_gizmo.h"
#include "clay/brush/magnify.h"
#include "clay/brush/move.h"
#include "clay/brush/preset.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/multires_sculpt.h"
#include "clay/mesh/project.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/brush/stroke.h"
#include "clay/brush/tube.h"
#include "clay/cut/cut.h"
#include "clay/eval/backend.h"
#include "clay/eval/bake_points.h"
#include "clay/eval/bake_volume.h"
#include "clay/field/flatten.h"
#include "clay/field/move_topological.h"
#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/brush/surface_measure.h"
#include "clay/brush/procedural_mask.h"
#include "clay/kernel/field.h"
#include "clay/io/clayspace.h"
#include "clay/io/handoff.h"
#include "clay/io/memory.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/preflight.h"
#include "clay/mesh/surface_view.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/lattice.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/deform.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/transfer.h"
#include "clay/mesh/voxel_remesh.h"
#include "clay/mesh/weld.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/validate.h"
#include "clay/pick/pick.h"
#include "clay/scene/armature.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/session/history.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/curve.h"
#include "clay/scene/tape.h"
#include "clay/version.h"
#include "clay/voxel/grab.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/hide.h"
#include "clay/voxel/mask.h"

namespace nb = nanobind;
using namespace nb::literals;
using namespace clay;

namespace {

// One conversion, so the document-wide and per-layer paths cannot present the
// report differently.
nb::dict memory_dict(const io::MemoryReport& r) {
    nb::dict out;
    out["edit_list"] = r.edit_list;
    out["voxel_content"] = r.voxel_content;
    out["mesh_layers"] = r.mesh_layers;
    out["masks"] = r.masks;
    out["voxel_sculpt_layers"] = r.voxel_sculpt_layers;
    out["history"] = r.history;
    out["passthrough"] = r.passthrough;
    out["transient"] = r.transient;
    out["total"] = r.total;
    out["voxel_layers"] = r.voxel_layers;
    out["mesh_layer_count"] = r.mesh_layer_count;
    out["mask_count"] = r.mask_count;
    // The surface tier: zero unless the caller handed in the ledgers of the
    // surfaces it holds BESIDE the document, because a hierarchy and an
    // adaptive surface are owned by the host and a document cannot walk them.
    out["surface_content"] = r.surface_content;
    out["multires_detail"] = r.multires_detail;
    out["sculpt_layers"] = r.sculpt_layers;
    out["surface_caches"] = r.surface_caches;
    out["surface_scratch"] = r.surface_scratch;
    out["surface_undo"] = r.surface_undo;
    // What a memory warning actually asks: not how big the document is, but
    // WHICH PART — that is what decides what a host may release.
    out["essential"] = r.essential();
    out["rebuildable"] = r.rebuildable();
    out["undoable"] = r.undoable();
    return out;
}

// -- argument parsing helpers -------------------------------------------------

kernel::cfloat3 to_f3(nb::handle h, const char* what) {
    try {
        nb::sequence s = nb::cast<nb::sequence>(h);
        if (nb::len(s) == 3)
            return kernel::cf3(nb::cast<float>(s[0]), nb::cast<float>(s[1]),
                               nb::cast<float>(s[2]));
    } catch (const std::exception&) {
    }
    throw std::invalid_argument(std::string(what) + " must be a sequence of 3 numbers");
}

// "#rrggbb" or a sequence of 3 floats in [0, 1].
kernel::cfloat3 parse_color(nb::handle h) {
    if (!nb::isinstance<nb::str>(h)) return to_f3(h, "color");
    std::string s = nb::cast<std::string>(h);
    auto nibble = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        throw std::invalid_argument("color string must be '#rrggbb'");
    };
    if (s.size() != 7 || s[0] != '#')
        throw std::invalid_argument("color string must be '#rrggbb'");
    auto channel = [&](int i) {
        return static_cast<float>(nibble(s[i]) * 16 + nibble(s[i + 1])) / 255.0f;
    };
    return kernel::cf3(channel(1), channel(3), channel(5));
}

int parse_axis(const std::string& axis) {
    if (axis == "x" || axis == "X") return 0;
    if (axis == "y" || axis == "Y") return 1;
    if (axis == "z" || axis == "Z") return 2;
    throw std::invalid_argument("axis must be 'x', 'y' or 'z'");
}

voxel::BrushShape parse_brush_shape(const std::string& shape) {
    if (shape == "cube") return voxel::BrushShape::Cube;
    if (shape == "sphere") return voxel::BrushShape::Sphere;
    throw std::invalid_argument("shape must be 'cube' or 'sphere', got '" + shape + "'");
}

brush::ExtrudeSide parse_extrude_side(const std::string& side) {
    if (side == "outward") return brush::ExtrudeSide::Outward;
    if (side == "inward") return brush::ExtrudeSide::Inward;
    // One spelling per side, deliberately. Every string pyclay accepts is a
    // capability the C ABI has to be able to name, and an alias would be a
    // second enumerator meaning exactly what the first one does.
    if (side == "centred") return brush::ExtrudeSide::Centred;
    throw std::invalid_argument("side must be 'outward', 'inward' or 'centred', got '" + side +
                                "'");
}

// Shared by every extrude entry point so the two representations cannot end up
// reading their arguments differently.
brush::MaskExtrudeSettings extrude_settings(float thickness, const std::string& side,
                                            float threshold, float border_round,
                                            int border_smooth, nb::handle cell_size,
                                            nb::handle band) {
    if (!(thickness > 0.0f)) throw std::invalid_argument("thickness must be > 0");
    brush::MaskExtrudeSettings s;
    s.thickness = thickness;
    s.side = parse_extrude_side(side);
    s.threshold = threshold;
    s.border_round = border_round;
    s.border_smooth = border_smooth;
    if (!cell_size.is_none()) s.cell_size = nb::cast<float>(cell_size);
    if (!band.is_none()) s.band = nb::cast<float>(band);
    return s;
}

// The inverse of parse_mesh_brush, so a model reads its verb back in the same
// spelling a caller passed it. One table would be better than two and is not
// possible here: the parser has to reject unknown strings by name, which a
// bidirectional map makes clumsier than the duplication saves.
const char* mesh_brush_name(mesh::MeshBrush verb);

const char* degradation_name(scene::Degradation d) {
    switch (d) {
        case scene::Degradation::Volumes: return "volumes";
        case scene::Degradation::Deformers: return "deformers";
        case scene::Degradation::Both: return "both";
        case scene::Degradation::None: break;
    }
    return "none";
}

mesh::MeshBrush parse_mesh_brush(const std::string& verb) {
    // One spelling per verb, as parse_extrude_side already argues: every string
    // pyclay accepts is a capability the C ABI has to be able to name, and an
    // alias would be a second enumerator meaning what the first one does.
    if (verb == "grab") return mesh::MeshBrush::Grab;
    if (verb == "draw") return mesh::MeshBrush::Draw;
    if (verb == "inflate") return mesh::MeshBrush::Inflate;
    if (verb == "smooth") return mesh::MeshBrush::Smooth;
    if (verb == "pinch") return mesh::MeshBrush::Pinch;
    if (verb == "flatten") return mesh::MeshBrush::Flatten;
    if (verb == "clay") return mesh::MeshBrush::Clay;
    if (verb == "crease") return mesh::MeshBrush::Crease;
    if (verb == "scrape") return mesh::MeshBrush::Scrape;
    if (verb == "polish") return mesh::MeshBrush::Polish;
    if (verb == "snakehook") return mesh::MeshBrush::Snakehook;
    if (verb == "relax") return mesh::MeshBrush::Relax;
    if (verb == "layer") return mesh::MeshBrush::Layer;
    if (verb == "nudge") return mesh::MeshBrush::Nudge;
    if (verb == "paint") return mesh::MeshBrush::Paint;
    if (verb == "smear") return mesh::MeshBrush::Smear;
    throw std::invalid_argument(
        "verb must be one of 'grab', 'draw', 'inflate', 'smooth', 'pinch', 'flatten', 'clay', "
        "'crease', 'scrape', 'polish', 'snakehook', 'relax', 'layer', 'nudge', 'paint', "
        "'smear', got '" +
        verb + "'");
}

const char* mesh_brush_name(mesh::MeshBrush verb) {
    switch (verb) {
        case mesh::MeshBrush::Grab: return "grab";
        case mesh::MeshBrush::Draw: return "draw";
        case mesh::MeshBrush::Inflate: return "inflate";
        case mesh::MeshBrush::Smooth: return "smooth";
        case mesh::MeshBrush::Pinch: return "pinch";
        case mesh::MeshBrush::Flatten: return "flatten";
        case mesh::MeshBrush::Clay: return "clay";
        case mesh::MeshBrush::Crease: return "crease";
        case mesh::MeshBrush::Scrape: return "scrape";
        case mesh::MeshBrush::Polish: return "polish";
        case mesh::MeshBrush::Snakehook: return "snakehook";
        case mesh::MeshBrush::Relax: return "relax";
        case mesh::MeshBrush::Layer: return "layer";
        case mesh::MeshBrush::Nudge: return "nudge";
        case mesh::MeshBrush::Paint: return "paint";
        case mesh::MeshBrush::Smear: return "smear";
    }
    return "draw";
}

mesh::MeshFalloff parse_mesh_falloff(const std::string& falloff) {
    if (falloff == "constant") return mesh::MeshFalloff::Constant;
    if (falloff == "linear") return mesh::MeshFalloff::Linear;
    if (falloff == "smooth") return mesh::MeshFalloff::Smooth;
    if (falloff == "gaussian") return mesh::MeshFalloff::Gaussian;
    throw std::invalid_argument(
        "falloff must be 'constant', 'linear', 'smooth' or 'gaussian', got '" + falloff + "'");
}

field::FlattenMode parse_flatten_mode(const std::string& mode) {
    if (mode == "two_sided") return field::FlattenMode::TwoSided;
    if (mode == "cut") return field::FlattenMode::CutOnly;
    if (mode == "fill") return field::FlattenMode::FillOnly;
    throw std::invalid_argument("mode must be 'two_sided', 'cut' or 'fill', got '" + mode + "'");
}

voxel::BrushFalloff parse_falloff(const std::string& falloff) {
    if (falloff == "constant") return voxel::BrushFalloff::Constant;
    if (falloff == "linear") return voxel::BrushFalloff::Linear;
    if (falloff == "smooth") return voxel::BrushFalloff::Smooth;
    if (falloff == "gaussian") return voxel::BrushFalloff::Gaussian;
    throw std::invalid_argument(
        "falloff must be 'constant', 'linear', 'smooth' or 'gaussian', got '" + falloff + "'");
}

// Forward declaration: the mask wrapper is defined below with the other
// voxel types, but every brush entry point resolves a mask through here.
struct PyMaskField;
const voxel::MaskField* borrow_mask(nb::handle mask);
// The field verbs take a callable rather than a mask type, so a sampled field
// stays a leaf module. Empty when no mask was given, which costs nothing.
field::MaskGate mask_gate_of(nb::handle mask);

voxel::BrushParams make_brush(int size, const std::string& shape, const std::string& falloff,
                              float strength, std::uint32_t seed, nb::handle mask = nb::handle()) {
    if (size <= 0) throw std::invalid_argument("brush size must be > 0");
    voxel::BrushParams p;
    p.size = size;
    p.shape = parse_brush_shape(shape);
    p.falloff = parse_falloff(falloff);
    p.strength = strength;
    p.seed = seed;
    p.mask = borrow_mask(mask);
    return p;
}

eval::Backend* find_backend(const std::string& name) {
    eval::Registry& reg = eval::Registry::instance();
    if (eval::Backend* b = reg.find(name)) return b;
    std::string msg = "backend '" + name + "' is not registered; available backends: ";
    std::vector<eval::Backend*> all = reg.all();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i) msg += ", ";
        msg += all[i]->name();
    }
    throw std::invalid_argument(msg);  // -> Python ValueError
}

void check_io(const io::IoStatus& s) {
    if (!s.ok()) throw std::runtime_error(s.detail.empty() ? "io error" : s.detail);
}

// -- scene wrappers -----------------------------------------------------------

struct PyProfile {
    scene::Profile profile;
    std::vector<kernel::cfloat2> points;  // polygon only
};

struct PyTransition {
    scene::Transition t;
};
struct PyTransitionLinear : PyTransition {};
struct PyTransitionRadial : PyTransition {};

struct PyBlend {
    scene::Blend b;
    explicit PyBlend(scene::BlendProfile profile, float k) : b{profile, k} {
        if (k < 0.0f) throw std::invalid_argument("blend k must be >= 0");
    }
};
struct PySmooth : PyBlend {
    explicit PySmooth(float k) : PyBlend(scene::BlendProfile::Quadratic, k) {}
};
struct PyCubic : PyBlend {
    explicit PyCubic(float k) : PyBlend(scene::BlendProfile::Cubic, k) {}
};
struct PyCircular : PyBlend {
    explicit PyCircular(float k) : PyBlend(scene::BlendProfile::Circular, k) {}
};
struct PyChamfer : PyBlend {
    explicit PyChamfer(float k) : PyBlend(scene::BlendProfile::Chamfer, k) {}
};

// A primitive plus its placement, ready to be added to a layer. Strokes
// additionally carry their point chain (the tape stores it out-of-line).
struct PyPrim {
    scene::Prim prim;
    math::Transform xform;
    // The per-axis half of the placement (#320). scale= on every constructor
    // takes one number or three; three land here and leave xform.scale at 1.
    kernel::cfloat3 scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);
    std::vector<scene::StrokePoint> stroke;
    float stroke_blend_k = 0.0f;
    bool stroke_closed = false;
    float curve_tolerance = 0.01f;
    std::vector<std::uint32_t> armature_parents;
    std::vector<std::int8_t> armature_signs;
    std::vector<scene::Deformer> deformers;
    scene::Profile profile;
    std::vector<kernel::cfloat2> profile_points;
    std::vector<scene::Profile> profiles;                       // Loft only
    std::vector<std::vector<kernel::cfloat2>> profile_polygons;  // Loft only
    std::shared_ptr<const field::FieldVolume> volume;            // Volume only
    // The mask that gates this item's participation, measured to a signed
    // distance by Prim.gate(). Null for the overwhelmingly common ungated item.
    std::shared_ptr<const field::FieldVolume> gate;
    float gate_width = 0.1f;
    scene::Repeat repeat;
    // Only a cut sets this today: it derives its own bevel, and losing it
    // between constructing the prim and placing it would be a trap. Layer.add
    // uses it when the caller does not override it.
    float rounding = 0.0f;
};

math::Quat to_axis_angle(nb::handle rotation_axis_angle) {
    nb::sequence s;
    try {
        s = nb::cast<nb::sequence>(rotation_axis_angle);
    } catch (const std::exception&) {
        throw std::invalid_argument("rotation_axis_angle must be ((x, y, z), radians)");
    }
    if (nb::len(s) != 2)
        throw std::invalid_argument("rotation_axis_angle must be ((x, y, z), radians)");
    return math::Quat::from_axis_angle(to_f3(s[0], "rotation axis"), nb::cast<float>(s[1]));
}

// scale= takes ONE number or THREE. Python can carry both in one argument where
// C cannot, so pyclay spells the per-axis scale as the scale it already had
// rather than as a second entry point (issue #320): `scale=2` and
// `scale=(2, 1, 1)` are the same argument, and the second is a squash.
//
// The uniform factor and the per-axis one MULTIPLY, so writing one leaves the
// other where it was — which is what makes this safe from a partial update.
void apply_scale(float& uniform, kernel::cfloat3& axes, nb::handle scale) {
    if (nb::isinstance<nb::sequence>(scale) && !nb::isinstance<nb::str>(scale)) {
        nb::sequence s = nb::cast<nb::sequence>(scale);
        if (nb::len(s) != 3)
            throw std::invalid_argument("scale must be one number or (sx, sy, sz)");
        const kernel::cfloat3 v = to_f3(scale, "scale");
        if (!(v.x > 0.0f && v.y > 0.0f && v.z > 0.0f))
            throw std::invalid_argument("every scale component must be > 0");
        uniform = 1.0f;
        axes = v;
        return;
    }
    const float f = nb::cast<float>(scale);
    if (!(f > 0.0f)) throw std::invalid_argument("scale must be > 0");
    uniform = f;
    axes = kernel::cf3(1.0f, 1.0f, 1.0f);
}

void place(PyPrim& p, nb::handle position, nb::handle rotation_axis_angle, nb::handle scale) {
    if (!position.is_none()) p.xform.position = to_f3(position, "position");
    if (!rotation_axis_angle.is_none()) p.xform.rotation = to_axis_angle(rotation_axis_angle);
    if (!scale.is_none()) apply_scale(p.xform.scale, p.scale_axes, scale);
}

// One concrete Python class per primitive (Sphere, Box, ...); all share the
// PyPrim representation, so Layer.add takes any of them.
struct PySphere : PyPrim {};
struct PyBox : PyPrim {};
struct PyRoundBox : PyPrim {};
struct PyTorus : PyPrim {};
struct PyCapsule : PyPrim {};
struct PyCylinder : PyPrim {};
struct PyCone : PyPrim {};
struct PyRoundCone : PyPrim {};
struct PyEllipsoid : PyPrim {};
struct PyOctahedron : PyPrim {};
struct PyHexPrism : PyPrim {};
struct PyPyramid : PyPrim {};
struct PyStroke : PyPrim {};
struct PyArmature : PyPrim {};
struct PyExtrude : PyPrim {};
struct PyCappedTorus : PyPrim {};
struct PyLink : PyPrim {};
struct PyCylinderInfinite : PyPrim {};
struct PyExactCone : PyPrim {};
struct PyPlane : PyPrim {};
struct PyCutSphere : PyPrim {};
struct PyCutHollowSphere : PyPrim {};
struct PySolidAngle : PyPrim {};
struct PyTetrahedron : PyPrim {};
struct PyDodecahedron : PyPrim {};
struct PyIcosahedron : PyPrim {};
struct PyTriPrism : PyPrim {};
struct PyOctahedronCheap : PyPrim {};
struct PyLNormSphere : PyPrim {};
struct PyRevolve : PyPrim {};
struct PyLoft : PyPrim {};
struct PySwept : PyPrim {};
struct PyVolume : PyPrim {};
struct PyCut : PyPrim {};

// -- mesh wrapper ---------------------------------------------------------------

// A BVH over a mesh, exposed as its own object rather than hidden behind the
// mesh: building it is the expensive part of an import, and an API that hid
// that would rebuild it per call.
struct PyMeshQuery {
    mesh::Bvh bvh;
};

// Owns a mesh, or borrows the one a document holds for a mesh layer, mirroring
// PyVoxelGrid so a layer's triangles are read straight out of the document
// rather than copied per access.
// Per mesh layer, bumped only when its triangles are REPLACED wholesale — the
// same counter the C ABI keeps, and for the same reason. A sculpt moves
// vertices and leaves the topology alone, which is exactly the change an
// adjacency, a BVH or a live sculptor SURVIVES; a rebuild swaps every vertex
// and every index, and they do not.
//
// Shared rather than held by value because a `PyMesh` and a `PyMeshSculptor`
// both need to read it and neither owns the document.
using MeshRevisions = std::map<scene::LayerId, std::uint64_t>;

std::uint64_t revision_of(const MeshRevisions& revisions, scene::LayerId layer) {
    auto it = revisions.find(layer);
    return it == revisions.end() ? 1u : it->second;
}

struct PyMesh {
    mesh::Mesh m;                           // the owned mesh; empty on a borrow
    std::shared_ptr<io::ClaySpaceDoc> doc;  // non-null: borrowed from a mesh layer
    // The document's revision map, when this is a borrow. Held so a sculptor
    // built over this mesh can tell a REBUILD from a sculpt; null on an owned
    // mesh, which belongs to no layer and cannot be replaced under anyone.
    std::shared_ptr<MeshRevisions> revisions;
    scene::LayerId layer = 0;

    // How this mesh was quad-meshed, when it was. On the wrapper rather than
    // on mesh::Mesh because it describes a CALL: a mesh loaded from a file or
    // read back out of a document was produced by no meshing call, and
    // quad_report raises for it rather than answering with zeroes.
    std::optional<mesh::QuadFit> fit;
    std::size_t fit_target = 0;

    const mesh::Mesh& data() const {
        if (!doc) return m;
        auto it = doc->mesh_layers.find(layer);
        if (it == doc->mesh_layers.end())
            throw std::runtime_error("mesh layer was removed from its document");
        return it->second;
    }

    // The same triangles, for a caller that is going to MOVE them: the mesh
    // brushes, and nothing else. A borrowed mesh resolves to the document's own
    // storage, so sculpting a layer edits the layer rather than a copy.
    //
    // Ghost and lock both mean "never edited", and a vertex displacement is an
    // edit, so both refuse here — the rule every other edit in this library
    // follows, applied at the one place a mesh layer can now be changed.
    mesh::Mesh& editable() {
        if (!doc) return m;
        const scene::Layer* l = doc->document.find_layer(layer);
        if (!l) throw std::runtime_error("mesh layer was removed from its document");
        if (l->protected_from_edits())
            throw std::runtime_error(std::string("layer '") + l->name + "' is " +
                                     (l->ghost ? "ghosted" : "locked") + " and takes no edits");
        auto it = doc->mesh_layers.find(layer);
        if (it == doc->mesh_layers.end())
            throw std::runtime_error("mesh layer was removed from its document");
        return it->second;
    }
};

// A sculpting session over one Mesh. The engine's MeshSculptor holds its mesh
// BY REFERENCE, and a Python Mesh is an ordinary object that can be collected,
// so the session keeps the Python object alive and re-resolves the triangles on
// every call: a mesh layer removed mid-session becomes an exception rather than
// a read of freed storage.
// A lattice cage. Held by value: it is a small offset array, and a copy is
// cheaper than the indirection a shared handle would add.
struct PyLattice {
    mesh::Lattice cage{math::Aabb{}, 3, 3, 3};
};

// The two spellings of a remesh resolution, and the refusal a caller gets for
// naming both or neither.
//
// Exactly one, rather than a mode enum plus a value: a Python caller who set
// the mode and forgot the value would otherwise get whatever the other field
// happened to hold, and the mistake would look like a resolution that did
// nothing.
mesh::VoxelRemeshParams py_remesh_params(nb::handle resolution, nb::handle voxel_size,
                                         nb::handle memory_budget) {
    mesh::VoxelRemeshParams p;
    const bool has_resolution = !resolution.is_none();
    const bool has_voxel = !voxel_size.is_none();
    if (has_resolution == has_voxel)
        throw std::invalid_argument("give exactly one of resolution= or voxel_size=");
    if (has_resolution) {
        const long long n = nb::cast<long long>(resolution);
        if (n <= 0) throw std::invalid_argument("resolution must be a positive integer");
        p.resolution_mode = mesh::VoxelRemeshResolutionMode::LongestAxisResolution;
        p.longest_axis_resolution = static_cast<std::uint32_t>(n);
    } else {
        p.resolution_mode = mesh::VoxelRemeshResolutionMode::VoxelSize;
        p.voxel_size = nb::cast<float>(voxel_size);
    }
    if (!memory_budget.is_none()) {
        const long long bytes = nb::cast<long long>(memory_budget);
        if (bytes < 0) throw std::invalid_argument("memory_budget must be >= 0");
        p.memory_budget_bytes = static_cast<std::uint64_t>(bytes);
    }
    return p;
}

mesh::VoxelRemeshOpenSurfacePolicy py_open_policy(const std::string& name) {
    if (name == "close") return mesh::VoxelRemeshOpenSurfacePolicy::Close;
    if (name == "reject") return mesh::VoxelRemeshOpenSurfacePolicy::Reject;
    if (name == "best_effort") return mesh::VoxelRemeshOpenSurfacePolicy::BestEffort;
    throw std::invalid_argument("open_surface must be 'close', 'reject' or 'best_effort'");
}

mesh::VoxelRemeshSurfaceMode py_surface_mode(const std::string& name) {
    if (name == "smooth") return mesh::VoxelRemeshSurfaceMode::Smooth;
    if (name == "sharp") return mesh::VoxelRemeshSurfaceMode::Sharp;
    throw std::invalid_argument("surface_mode must be 'smooth' or 'sharp'");
}

// A refusal names WHICH contract refused it. An empty mesh with no explanation
// would make a caller diagnose the engine's decision from the geometry, which
// is exactly what the typed status exists to prevent.
[[noreturn]] void raise_remesh(mesh::VoxelRemeshStatus status) {
    switch (status) {
        case mesh::VoxelRemeshStatus::EmptySource:
            throw std::invalid_argument("a mesh with no triangles has no surface to rebuild");
        case mesh::VoxelRemeshStatus::InvalidResolution:
            throw std::invalid_argument(
                "the resolution must be a finite positive voxel size, or a non-zero "
                "longest-axis resolution");
        case mesh::VoxelRemeshStatus::InvalidParameters:
            throw std::invalid_argument(
                "a projection strength, projection distance or component volume is out of "
                "range");
        case mesh::VoxelRemeshStatus::Unsupported:
            throw std::runtime_error("this voxel remesh option is not supported yet");
        case mesh::VoxelRemeshStatus::ExceedsBudget:
            throw std::runtime_error(
                "the voxel remesh exceeds the memory budget; choose a coarser resolution");
        case mesh::VoxelRemeshStatus::OpenSurfaceRejected:
            throw std::runtime_error(
                "the source has open boundaries and open_surface='reject' refuses them");
        case mesh::VoxelRemeshStatus::ExtractionFailed:
            throw std::runtime_error("no surface was found at this resolution");
        case mesh::VoxelRemeshStatus::ResultNotWatertight:
            throw std::runtime_error(
                "the voxel remesh result failed the watertight validation this surface mode "
                "promises");
        case mesh::VoxelRemeshStatus::Cancelled:
            throw std::runtime_error("the voxel remesh was cancelled");
        case mesh::VoxelRemeshStatus::Ok:
            break;
    }
    throw std::runtime_error("voxel remesh failed");
}

// The report as a dict, in one place: `Mesh.voxel_remesh` and
// `Document.voxel_remesh_layer` return the same fields, and two copies of this
// list would drift the first time one gained a field.
struct PyMeshSculptor {
    nb::object owner;  // the Python Mesh, kept alive for the session's lifetime
    PyMesh* mesh = nullptr;
    mesh::Mesh* bound = nullptr;
    // The layer's geometry revision when this was built. See MeshRevisions:
    // it is the only one of the three checks below that can see a rebuild
    // landing on the same vertex and index counts.
    std::uint64_t geometry_revision = 0;
    std::shared_ptr<mesh::MeshSculptor> sculptor;

    mesh::MeshSculptor& live(bool for_edit) const {
        if (!sculptor) throw std::runtime_error("this sculptor was never built");
        mesh::Mesh& now = for_edit ? mesh->editable() : const_cast<mesh::Mesh&>(mesh->data());
        if (&now != bound)
            throw std::runtime_error("the mesh this sculptor was built over has been replaced");
        if (!sculptor->valid())
            throw std::runtime_error(
                "the mesh changed its vertex or index count under this sculptor");
        // The pointer comparison above catches a layer that was REMOVED — a
        // std::map node's address is stable, so it does not see the contents
        // replaced — and `valid()` catches a changed COUNT. A rebuild that
        // lands on the same counts passes both and leaves this sculptor's
        // adjacency and BVH describing triangles that no longer exist.
        if (mesh->revisions &&
            revision_of(*mesh->revisions, mesh->layer) != geometry_revision)
            throw std::runtime_error(
                "the mesh layer was rebuilt under this sculptor; build a new one");
        return *sculptor;
    }
};


// -- the surface tier: budgets, ledgers and the chunk transport ---------------
//
// (add-extreme-poly-runtime.) A dab costs approximately what it TOUCHES, and a
// host at twenty million vertices needs three things across this boundary to
// keep it that way: what changed, those bytes and nothing else, and what the
// whole thing costs.

nb::dict ledger_dict(const memory::MemoryLedger& ledger) {
    nb::dict out;
    // KEYED BY THE CATEGORY'S OWN NAME rather than by an integer, so a script
    // reading this does not have to hold the enumeration's order — and so a
    // category added later appears in the dict instead of shifting an index
    // somebody wrote down.
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i)
        out[memory::memory_category_name(static_cast<memory::MemoryCategory>(i))] =
            ledger.bytes[i];
    out["essential"] = ledger.essential();
    out["rebuildable"] = ledger.rebuildable();
    out["undoable"] = ledger.undoable();
    out["total"] = ledger.total();
    return out;
}

nb::dict trim_dict(const memory::TrimReport& report) {
    nb::dict out;
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i)
        out[memory::memory_category_name(static_cast<memory::MemoryCategory>(i))] =
            report.released[i];
    out["pressure"] = memory::pressure_name(report.pressure);
    out["total_released"] = report.total_released;
    // Nothing was released and the figures above are what the call WOULD have
    // released: a memory warning arriving mid-save gets an honest answer rather
    // than a document mutating under the writer.
    out["pinned"] = report.pinned;
    return out;
}

nb::dict preflight_dict(const mesh::SurfacePreflight& p) {
    nb::dict out;
    out["authoritative_bytes"] = p.authoritative_bytes;
    out["runtime_bytes"] = p.runtime_bytes;
    out["persistent_bytes"] = p.persistent_bytes;
    // The high-water mark DURING the call, and the number that matters: the
    // peak is what kills an application on a memory-constrained device, and a
    // steady-state figure that "fits" is what makes it happen half way through.
    out["peak_bytes"] = p.peak_bytes;
    out["allowed"] = p.allowed;
    out["error"] = memory::budget_error_text(p.error);
    return out;
}

// A ledger dict, back into the ledger it came out of. Keyed by the category's
// own NAME, which is what `ledger_dict` emitted — the roll-ups it also carries
// are derived and are ignored here rather than double-counted.
void merge_ledger_dict(nb::handle h, memory::MemoryLedger* out) {
    nb::dict d;
    if (!nb::try_cast(h, d))
        throw std::invalid_argument(
            "a surface ledger must be a dict as memory_ledger() returns, or a list of them");
    for (std::size_t i = 0; i < memory::kMemoryCategoryCount; ++i) {
        const char* key = memory::memory_category_name(static_cast<memory::MemoryCategory>(i));
        if (d.contains(key))
            out->add(static_cast<memory::MemoryCategory>(i),
                     nb::cast<std::size_t>(d[key]));
    }
}

nb::dict revisions_dict(const mesh::ChunkRevisions& r) {
    nb::dict out;
    out["topology"] = r.topology;
    out["geometry"] = r.geometry;
    out["normals"] = r.normals;
    out["attributes"] = r.attributes;
    return out;
}

// The four counters a host echoes back when it acknowledges a chunk. A dict
// rather than a class, because it is what `copy_chunk` handed the caller a
// moment ago and a round trip through a bound type would buy nothing.
mesh::ChunkRevisions to_revisions(nb::handle h, const char* what) {
    mesh::ChunkRevisions r;
    if (h.is_none()) return r;
    nb::dict d;
    if (!nb::try_cast(h, d))
        throw std::invalid_argument(std::string(what) +
                                    " must be a dict of topology/geometry/normals/attributes, "
                                    "as copy_chunk returns");
    const auto field = [&](const char* key, std::uint64_t* out) {
        if (d.contains(key)) *out = nb::cast<std::uint64_t>(d[key]);
    };
    field("topology", &r.topology);
    field("geometry", &r.geometry);
    field("normals", &r.normals);
    field("attributes", &r.attributes);
    return r;
}

// The read seam, as a Python object.
//
// IT STORES THE SOURCE AND REBUILDS THE VIEW PER CALL. `mesh::SurfaceView` is a
// call-site convenience by construction — valid only while the surface it names
// is unchanged — and a Python object outlives a stamp by definition. Caching
// the view would hand back spans into a table the next stamp has moved.
struct PySurfaceView {
    nb::object owner;  // the Python surface this names, kept alive for our life
    mesh::SurfaceKind kind = mesh::SurfaceKind::Fixed;
    PyMesh* mesh_handle = nullptr;
    // Fixed only, and OURS: the fixed sculptor tracks dirty weld classes and
    // has no chunk table of its own yet, so this view partitions the mesh and
    // reports a static partition with an empty dirty set.
    mesh::ChunkTable table;
    mesh::DynamicSculptor* dynamic_sculptor = nullptr;
    mesh::MultiresSurface* multires = nullptr;
    std::uint32_t level = 0;

    mesh::SurfaceView view() {
        switch (kind) {
            case mesh::SurfaceKind::Fixed:
                return mesh::SurfaceView::over_mesh(mesh_handle->data(), table);
            case mesh::SurfaceKind::Adaptive:
                return mesh::SurfaceView::over_dynamic(dynamic_sculptor->surface(),
                                                       dynamic_sculptor->bvh().chunks());
            case mesh::SurfaceKind::Multires:
                if (level >= multires->level_count())
                    throw std::runtime_error(
                        "that level is gone from this hierarchy; take a new view");
                return mesh::SurfaceView::over_level(*multires, level);
        }
        throw std::runtime_error("corrupt surface view");
    }

    // The acknowledgement is a WRITE, which is why the read seam does not carry
    // one. Null for a hierarchy, whose dirty set is retired through the surface.
    mesh::ChunkTable* writable_table() {
        switch (kind) {
            case mesh::SurfaceKind::Fixed: return &table;
            case mesh::SurfaceKind::Adaptive: return &dynamic_sculptor->bvh().chunks_mutable();
            case mesh::SurfaceKind::Multires: return nullptr;
        }
        return nullptr;
    }
};

// The gate a serializer or a readback holds. A CONTEXT MANAGER, and that is the
// point of binding it rather than exposing a counter: a `with` block is the only
// form that cannot leave a document pinned when the body raises, and a document
// pinned forever is one no trim can ever help.
struct PyMemoryPin {
    PyMemoryPin() = default;
    // NOT COPYABLE, said out loud rather than left to the member below. A
    // vector of unique_ptr is copy-constructible as a DECLARATION and fails
    // only when the copy is instantiated, so a binding generator that probes
    // `is_copy_constructible` gets True and then a hard error inside the
    // standard library. Copying a pin is also meaningless: the balance belongs
    // to the scope that took it.
    PyMemoryPin(const PyMemoryPin&) = delete;
    PyMemoryPin& operator=(const PyMemoryPin&) = delete;

    memory::TrimGate gate;
    // One live scope per acquire. The count is private to memory::MemoryPin on
    // purpose — RAII is the mechanism rather than a convenience over it — so a
    // balance kept by hand is a stack of scopes and never a second counter.
    std::vector<std::unique_ptr<memory::MemoryPin>> held;
};

struct PyVertexDeltas {
    std::shared_ptr<mesh::VertexDeltas> deltas = std::make_shared<mesh::VertexDeltas>();
};

// Zero-copy (owner-tracked) view over a vector of cfloat3 (plain x,y,z floats).
nb::object f3_view(nb::object owner, const std::vector<kernel::cfloat3>& v) {
    if (v.empty()) {
        nb::module_ np = nb::module_::import_("numpy");
        return np.attr("empty")(nb::make_tuple(0, 3), "dtype"_a = "float32");
    }
    return nb::cast(nb::ndarray<nb::numpy, const float>(&v.front().x, {v.size(), 3}, owner));
}

nb::object f2_view(nb::object owner, const std::vector<kernel::cfloat2>& v) {
    if (v.empty()) {
        nb::module_ np = nb::module_::import_("numpy");
        return np.attr("empty")(nb::make_tuple(0, 2), "dtype"_a = "float32");
    }
    return nb::cast(nb::ndarray<nb::numpy, const float>(&v.front().x, {v.size(), 2}, owner));
}

void save_mesh_any(const mesh::Mesh& m, const std::string& path) {
    std::size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    if (ext == "obj") return check_io(io::save_obj_file(m, path));
    if (ext == "ply") return check_io(io::save_ply_file(m, path));
    if (ext == "fbx") return check_io(io::save_fbx_file(m, path));
    if (ext == "glb") return check_io(io::save_glb_file(m, path));
    throw std::invalid_argument("unsupported mesh extension '." + ext +
                                "' (supported: .obj, .ply, .fbx, .glb)");
}

// The same two dispatches against BYTES rather than a path. A buffer has no
// extension, so the format is named — the extension without the dot, matched
// the same way.
std::vector<std::uint8_t> save_mesh_bytes_any(const mesh::Mesh& m, const std::string& format) {
    std::string name = format;
    for (char& c : name) c = static_cast<char>(std::tolower(c));
    if (name == "obj") {
        // No mtl NAME, so no mtllib line: a buffer has no companion file.
        const std::string text = io::save_obj(m, "claycore", {});
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    if (name == "ply") return io::save_ply(m);
    if (name == "fbx") return io::save_fbx(m);
    if (name == "glb") return io::save_glb(m);
    throw std::invalid_argument("unsupported mesh format '" + name +
                                "' (supported: obj, ply, fbx, glb)");
}

mesh::Mesh load_mesh_bytes_any(const std::uint8_t* data, std::size_t size,
                               const std::string& format, const io::ImportBudget& limits) {
    std::string name = format;
    for (char& c : name) c = static_cast<char>(std::tolower(c));
    mesh::Mesh out;
    if (name == "obj") {
        check_io(io::load_obj(std::string(reinterpret_cast<const char*>(data), size), &out,
                              limits));
    } else if (name == "ply") {
        check_io(io::load_ply(data, size, &out, limits));
    } else if (name == "fbx") {
        check_io(io::load_fbx(data, size, &out, limits));
    } else if (name == "glb") {
        check_io(io::load_glb(data, size, &out, limits));
    } else {
        throw std::invalid_argument("unsupported mesh format '" + name +
                                    "' for loading (supported: obj, ply, fbx, glb)");
    }
    return out;
}

mesh::Mesh load_mesh_any(const std::string& path, const io::ImportBudget& limits) {
    std::size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    mesh::Mesh out;
    if (ext == "obj") {
        check_io(io::load_obj_file(path, &out, limits));
    } else if (ext == "ply") {
        check_io(io::load_ply_file(path, &out, limits));
    } else if (ext == "fbx") {
        check_io(io::load_fbx_file(path, &out, limits));
    } else if (ext == "glb") {
        check_io(io::load_glb_file(path, &out, limits));
    } else {
        // .gltf lands here on purpose: its buffers live in separate files
        // beside it, and reading whatever the JSON names would mean reading
        // files the caller never handed us.
        throw std::invalid_argument("unsupported mesh extension '." + ext +
                                    "' for loading (supported: .obj, .ply, .fbx, .glb)");
    }
    return out;
}

// A cut's swept region: the document being cut, which is the answer in every
// real use, or an explicit ((lo), (hi)) pair for a caller that wants to bound
// it by hand.
math::Aabb to_aabb(nb::handle obj);

// -- mesh brush helpers --------------------------------------------------------

// One place that turns a Python call into a MeshBrushSettings, shared by `stamp`
// and `apply_stroke`, so the two cannot drift about what an argument means.
// Every refusal here mirrors a refusal in the C ABI, which is the standing rule
// for these two bindings: they must not disagree about what a value is.
mesh::MeshBrushSettings mesh_brush_settings(
    const std::string& verb, nb::handle center, float radius, float strength,
    const std::string& falloff, nb::handle direction, nb::handle deposit_normal,
    nb::handle geodesic, nb::handle seed_class, const std::string& flatten_mode,
    nb::handle plane_point, nb::handle plane_normal, float polish_angle, int smooth_iterations,
    float layer_height, nb::handle alpha, nb::handle alpha_direction, nb::handle alpha_tangent,
    float alpha_extent, nb::handle color, mesh::MeshBrush* out_verb) {
    *out_verb = parse_mesh_brush(verb);
    if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
    if (smooth_iterations < 1 || smooth_iterations > mesh::kMaxSmoothIterations)
        throw std::invalid_argument("smooth_iterations must be in 1.." +
                                    std::to_string(mesh::kMaxSmoothIterations));
    mesh::MeshBrushSettings settings;
    if (!center.is_none()) settings.center = to_f3(center, "center");
    settings.radius = radius;
    settings.strength = strength;
    settings.falloff = parse_mesh_falloff(falloff);
    if (!direction.is_none()) settings.direction = to_f3(direction, "direction");
    if (!deposit_normal.is_none())
        settings.deposit_normal = to_f3(deposit_normal, "deposit_normal");
    // PAINT's target. None keeps the engine default rather than meaning black,
    // because here — unlike across the C boundary, where an absent field is
    // indistinguishable from a zeroed one — None and (0, 0, 0) are different
    // things a caller can say.
    if (!color.is_none()) settings.color = to_f3(color, "color");
    // None means "whatever this verb should do", which is off for the two whose
    // meaning is "everything under this disc".
    settings.geodesic =
        geodesic.is_none() ? mesh::default_geodesic(*out_verb) : nb::cast<bool>(geodesic);
    settings.seed_class =
        seed_class.is_none() ? mesh::kNoClass : nb::cast<std::uint32_t>(seed_class);
    settings.flatten_mode = parse_flatten_mode(flatten_mode);
    // A plane is given when EITHER half of it is, so a caller who names only the
    // normal gets the plane they meant rather than a silently ignored argument.
    settings.use_given_plane = !plane_point.is_none() || !plane_normal.is_none();
    if (!plane_point.is_none()) settings.plane_point = to_f3(plane_point, "plane_point");
    if (!plane_normal.is_none()) {
        settings.plane_normal = to_f3(plane_normal, "plane_normal");
        if (!(kernel::clength(settings.plane_normal) > 1e-6f))
            throw std::invalid_argument("plane_normal must not be zero length");
    }
    settings.polish_angle = polish_angle;
    settings.smooth_iterations = smooth_iterations;
    settings.layer_height = layer_height;
    if (!alpha.is_none()) {
        // A 2D array, because a stamp IS two-dimensional and letting a caller
        // pass a flat one with separate dimensions is the multiply they would
        // get wrong. NOT copied — nanobind keeps the array alive for the call,
        // which is exactly as long as the samples are read.
        auto arr = nb::cast<nb::ndarray<const float, nb::ndim<2>, nb::c_contig>>(alpha);
        if (arr.shape(0) < 2 || arr.shape(1) < 2)
            throw std::invalid_argument(
                "an alpha needs at least 2x2 samples; there is nothing to interpolate below "
                "that");
        settings.alpha = arr.data();
        settings.alpha_height = static_cast<int>(arr.shape(0));
        settings.alpha_width = static_cast<int>(arr.shape(1));
        if (!alpha_direction.is_none())
            settings.alpha_direction = to_f3(alpha_direction, "alpha_direction");
        if (!alpha_tangent.is_none())
            settings.alpha_tangent = to_f3(alpha_tangent, "alpha_tangent");
        settings.alpha_extent = alpha_extent;
    }
    return settings;
}

// -- stroke engine helpers -----------------------------------------------------

// (N, 3) positions, or (N, 4) with pressure, or (N, 5) with pressure and
// tilt. Anything a stylus reports, without three separate array arguments.
std::vector<brush::StrokeSample> to_stroke_samples(nb::handle obj) {
    nb::module_ np = nb::module_::import_("numpy");
    nb::object arr = np.attr("ascontiguousarray")(obj, "dtype"_a = "float32");
    nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
    try {
        view = nb::cast<decltype(view)>(arr);
    } catch (const std::exception&) {
        throw std::invalid_argument("samples must be an (N, 3), (N, 4) or (N, 5) array");
    }
    std::size_t width = view.shape(1);
    // 3 to 8: position, pressure, tilt, azimuth, velocity, timestamp.
    //
    // Widening this is free where the C ABI's flat count*5 packing could not
    // be: a numpy array carries its own shape, so an (N, 5) array keeps
    // meaning exactly what it did and an (N, 8) one gains the channels. That
    // is the interface negotiating a layout, which is what the C side needed a
    // second entry point to do.
    if (width < 3 || width > 8)
        throw std::invalid_argument(
            "samples must be an (N, K) array with K from 3 to 8: position, then optional "
            "pressure, tilt, azimuth, velocity, timestamp");
    std::vector<brush::StrokeSample> out(view.shape(0));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float* row = view.data() + i * width;
        out[i].position = kernel::cf3(row[0], row[1], row[2]);
        if (width > 3) out[i].pressure = row[3];
        if (width > 4) out[i].tilt = row[4];
        if (width > 5) out[i].azimuth = row[5];
        if (width > 6) out[i].velocity = row[6];
        // float32 here, so pass seconds since the STROKE began rather than an
        // epoch time — a float carries about seven digits, which an absolute
        // timestamp exhausts before the fractional part a stroke needs.
        if (width > 7) out[i].timestamp = static_cast<double>(row[7]);
    }
    return out;
}

// -- voxel wrapper -------------------------------------------------------------

// Owns a grid, or borrows the one stored in a document's voxel layer so edits
// are visible to save()/mesh() without a copy.
// A session::History since unify-the-undo-history: the same opt-in and the
// same methods, now spanning voxel grids and mesh layers as well as the edit
// list. It WRAPS an UndoStack, so every command path below is unchanged.
using UndoRef = std::shared_ptr<session::History>;

struct PyVoxelGrid {
    std::shared_ptr<io::ClaySpaceDoc> doc;  // null for standalone grids
    // The document's history, shared. A shared_ptr TO the document's UndoRef
    // rather than a copy of it, so a grid handle taken BEFORE enable_undo still
    // sees the history once it is switched on — which is the ordinary order a
    // host does those two things in.
    std::shared_ptr<UndoRef> undo;
    scene::LayerId layer = 0;
    std::shared_ptr<voxel::VoxelGrid> owned;

    voxel::VoxelGrid& grid() const {
        if (owned) return *owned;
        auto it = doc->voxel_layers.find(layer);
        if (it == doc->voxel_layers.end())
            throw std::runtime_error("voxel layer was removed from its document");
        return it->second;
    }
};

// `with grid.sculpt_layer("pass"):` — the shape a Python caller reaches for,
// and the one that cannot leave a grid recording when the block raises. It
// holds the grid by the same shared handle PyVoxelGrid does, so a scope over a
// document's layer stays valid exactly as long as the grid does.
struct PySculptLayerScope;

// A sculpt layer index that does not exist is an IndexError rather than a
// silent no-op — the C++ side returns false, which Python callers would not
// see through a void binding.
void check_sculpt_layer(const PyVoxelGrid& g, std::size_t layer) {
    if (layer >= g.grid().sculpt_layer_count())
        throw nb::index_error(("no sculpt layer " + std::to_string(layer)).c_str());
}

// Owns a mask, or borrows the one a document holds for a layer, mirroring
// PyVoxelGrid so a mask edited through a document is the one that gets saved.
struct PyMaskField {
    std::shared_ptr<io::ClaySpaceDoc> doc;  // null for standalone masks
    // The document's history, shared — see PyVoxelGrid. A mask edit is a
    // BARRIER, and this is how the handle reaches the history to record one.
    std::shared_ptr<UndoRef> undo;
    scene::LayerId layer = 0;
    std::shared_ptr<voxel::MaskField> owned;
    // One memoised gate bake per mask OBJECT, so gating N items by one painted
    // mask pays for one measurement rather than N. Held by shared_ptr so a
    // copied handle shares the memo rather than starting a cold one.
    std::shared_ptr<brush::GateBake> gate_bake = std::make_shared<brush::GateBake>();

    voxel::MaskField& field() const {
        if (owned) return *owned;
        auto it = doc->masks.find(layer);
        if (it == doc->masks.end())
            throw std::runtime_error("mask was removed from its document");
        return it->second;
    }
};

// The document's surface groups. Always a BORROW — there is no standalone
// form, unlike a mask, because a group names a region of a MODEL and a
// standalone lattice would name a region of nothing.
struct PyGroupField {
    std::shared_ptr<io::ClaySpaceDoc> doc;
    std::shared_ptr<UndoRef> undo;

    voxel::GroupField& field() const {
        if (!doc->groups) throw std::runtime_error("this document has no surface groups");
        return *doc->groups;
    }
};

// Brackets a group edit so it becomes one undo step, on the same RAII shape the
// C binding's GroupStep uses. Every mutator below takes one, which is what
// keeps eleven call sites from each having to remember.
struct PyGroupStep {
    session::History* history = nullptr;
    voxel::GroupField* field = nullptr;

    explicit PyGroupStep(const PyGroupField& g) {
        session::History* h = g.undo ? g.undo->get() : nullptr;
        if (!h) return;
        voxel::GroupField& f = g.field();
        if (!h->begin_group_step(f)) return;
        history = h;
        field = &f;
    }
    ~PyGroupStep() {
        if (history) history->end_group_step(*field);
    }
    PyGroupStep(const PyGroupStep&) = delete;
    PyGroupStep& operator=(const PyGroupStep&) = delete;
};

struct PySculptLayerScope {
    PyVoxelGrid grid;
    std::string name;
    std::size_t index = 0;
};

// A voxel drag as a gesture (issue #393). Holds the grid handle as well as the
// transaction, because every write has to raise the same undo step a stateless
// verb does; the transaction itself knows nothing about a document.
struct PyVoxelGrab {
    PyVoxelGrid grid;
    std::optional<voxel::GrabTransaction> tx;
};

field::MaskGate mask_gate_of(nb::handle mask) {
    const voxel::MaskField* m = borrow_mask(mask);
    if (!m) return {};
    return [m](kernel::cfloat3 p) { return m->sample(p); };
}

const voxel::MaskField* borrow_mask(nb::handle mask) {
    if (!mask.is_valid() || mask.is_none()) return nullptr;
    // Borrowed for the duration of the call only, which is all a BrushParams
    // built at the call site needs.
    return &nb::cast<PyMaskField&>(mask).field();
}

// The gate memo belonging to this mask OBJECT. A caller that writes
// `m = doc.mask(id)` once and gates fifty items off `m` pays for one bake; one
// that rebuilds the handle inside the loop gets today's behaviour, correct but
// uncached, because there is no object for the memo to live on.
const std::shared_ptr<brush::GateBake>& gate_bake_of(nb::handle mask) {
    return nb::cast<PyMaskField&>(mask).gate_bake;
}


brush::SurfaceMeasure measure_from_name(const std::string& name) {
    if (name == "curvature") return brush::SurfaceMeasure::Curvature;
    if (name == "cavity") return brush::SurfaceMeasure::Cavity;
    if (name == "convexity") return brush::SurfaceMeasure::Convexity;
    if (name == "normal_direction") return brush::SurfaceMeasure::NormalDirection;
    if (name == "occlusion") return brush::SurfaceMeasure::AmbientOcclusion;
    if (name == "thickness") return brush::SurfaceMeasure::Thickness;
    throw std::invalid_argument(
        "measure must be curvature/cavity/convexity/normal_direction/occlusion/thickness, got '" +
        name + "'");
}

// None means "the defaults", which is what a caller measuring curvature with no
// opinion about the stencil wants.
brush::MeasureSettings measure_settings_from(nb::handle params) {
    brush::MeasureSettings s;
    if (!params.is_valid() || params.is_none()) return s;
    nb::dict d = nb::cast<nb::dict>(params);
    auto get = [&](const char* k, float& into) {
        if (d.contains(k)) into = nb::cast<float>(d[k]);
    };
    get("h", s.h);
    get("scale", s.scale);
    get("threshold", s.threshold);
    get("ray_length", s.ray_length);
    get("falloff", s.falloff);
    if (d.contains("direction")) s.direction = to_f3(d["direction"], "direction");
    if (d.contains("ray_count")) s.ray_count = nb::cast<int>(d["ray_count"]);
    if (d.contains("seed")) s.seed = nb::cast<std::uint32_t>(d["seed"]);
    return s;
}

// -- document / layer wrappers ------------------------------------------------

// The undo stack is opt-in and shared by a document and every Layer handle
// onto it, so an edit made through a layer records into the same history as
// one made through the document. Null means undo is off, and a document with
// no stack behaves exactly as it did before the feature existed.

// Every editing entry point goes through the command vocabulary rather than
// mutating the document, so a binding edit means exactly what the document
// format records for it — and, once the undo stack is exposed, is undoable
// for free. apply() returns nullopt when the target does not exist and leaves
// the document untouched.
void apply_or_throw(scene::Document& doc, const scene::Command& cmd, const char* what,
                   const UndoRef* undo = nullptr) {
    // apply() says no for two different reasons. A protected layer is a state
    // the artist chose and deserves its own message; a missing id is a bug.
    scene::LayerId target = scene::edited_layer(cmd);
    if (target != 0) {
        const scene::Layer* l = doc.find_layer(target);
        if (l && l->protected_from_edits())
            throw std::invalid_argument(std::string(what) + ": layer " +
                                        std::to_string(target) + " is " +
                                        (l->ghost ? "ghosted" : "locked") +
                                        " and takes no edits");
    }
    // With a stack attached the edit is applied AND its inverse recorded, so
    // no reachable edit can escape undo. Without one it is a plain apply.
    bool ok = (undo && *undo) ? (*undo)->perform(doc, cmd) : static_cast<bool>(scene::apply(doc, cmd));
    if (!ok)
        throw std::invalid_argument(std::string(what) +
                                    ": no node or layer with that id in this document");
}

struct PyDocument {
    std::shared_ptr<io::ClaySpaceDoc> doc = std::make_shared<io::ClaySpaceDoc>();
    std::shared_ptr<UndoRef> undo = std::make_shared<UndoRef>();
    std::shared_ptr<MeshRevisions> mesh_revisions = std::make_shared<MeshRevisions>();
};


// The one place a mesh layer's triangles are swapped from Python, so both
// callers get the same guards, the same undo record and the same invalidation.
// Mirrors replace_mesh_layer_geometry in the C ABI exactly.
void py_replace_mesh_layer(PyDocument& d, scene::LayerId layer, mesh::Mesh replacement,
                           nb::handle expected_revision) {
    const scene::Layer* l = d.doc->document.find_layer(layer);
    if (!l || l->kind != scene::LayerKind::Mesh)
        throw std::invalid_argument("no mesh layer with that id");
    if (l->protected_from_edits())
        throw std::runtime_error(std::string("layer '") + l->name + "' is " +
                                 (l->ghost ? "ghosted" : "locked") + " and takes no edits");
    auto it = d.doc->mesh_layers.find(layer);
    if (it == d.doc->mesh_layers.end())
        throw std::invalid_argument("the mesh layer holds no triangles");
    if (replacement.triangle_count() == 0)
        throw std::invalid_argument("the replacement has no triangles");
    // A quad list describing triangles that no longer exist is a lie that
    // survives into a saved document — mesh_data.h states the rule, and the
    // save path drops such a list silently, so this refuses rather than
    // repairs.
    if (replacement.has_quads() && !mesh::quads_consistent(replacement))
        throw std::invalid_argument("the replacement's quads do not describe its triangles");

    const std::uint64_t now = revision_of(*d.mesh_revisions, layer);
    if (!expected_revision.is_none() &&
        nb::cast<std::uint64_t>(expected_revision) != now)
        throw std::runtime_error(
            "the mesh layer was rebuilt while this result was being prepared");

    // ONE UNDO STEP holding both meshes. A VertexDeltas cannot express this and
    // must not be asked to: deltas already on the stack for this layer were
    // recorded against the OLD vertex count.
    if (d.undo && *d.undo) (*d.undo)->record_mesh_replace(layer, it->second, replacement);
    it->second = std::move(replacement);
    (*d.mesh_revisions)[layer] = now + 1;
}

nb::dict voxel_remesh_report_dict(const mesh::VoxelRemeshReport& r) {
    nb::dict out;
    out["voxel_size"] = r.voxel_size;
    out["source_vertices"] = r.source_vertices;
    out["source_triangles"] = r.source_triangles;
    out["result_vertices"] = r.result_vertices;
    out["result_triangles"] = r.result_triangles;
    out["source_volume"] = r.source_volume;
    out["result_volume"] = r.result_volume;
    out["relative_volume_error"] = r.relative_volume_error;
    out["source_boundary_edges"] = r.source_boundary_edges;
    out["result_boundary_edges"] = r.result_boundary_edges;
    out["source_components"] = r.source_components;
    out["result_components"] = r.result_components;
    out["removed_components"] = r.removed_components;
    out["active_samples"] = r.active_samples;
    out["source_was_open"] = r.source_was_open;
    out["result_watertight"] = r.result_watertight;
    out["result_manifold"] = r.result_manifold;
    out["result_oriented"] = r.result_oriented;
    out["projected_to_source"] = r.projected_to_source;
    out["projected_vertices"] = r.projected_vertices;
    out["volume_corrected"] = r.volume_corrected;
    out["colors_transferred"] = r.colors_transferred;
    out["uvs_dropped"] = r.uvs_dropped;
    out["result_to_source_rms"] = r.result_to_source_rms;
    out["result_to_source_p95"] = r.result_to_source_p95;
    out["result_to_source_max"] = r.result_to_source_max;
    out["estimated_memory_bytes"] = r.estimated_memory_bytes;
    nb::dict stages;
    static const char* kStageNames[] = {"preflight",  "source_acceleration", "sampling",
                                        "extraction", "projection",          "attribute_transfer",
                                        "validation"};
    for (std::size_t i = 0; i < mesh::kVoxelRemeshStageCount; ++i)
        stages[kStageNames[i]] = r.stage_ms[i];
    out["stage_ms"] = stages;
    return out;
}

session::History::GridFor grid_for(const PyDocument& d) {
    std::shared_ptr<io::ClaySpaceDoc> doc = d.doc;
    return [doc](scene::LayerId id) -> voxel::VoxelGrid* {
        auto it = doc->voxel_layers.find(id);
        return it == doc->voxel_layers.end() ? nullptr : &it->second;
    };
}

session::History::MaskFor mask_for(const PyDocument& d) {
    std::shared_ptr<io::ClaySpaceDoc> doc = d.doc;
    return [doc](scene::LayerId id) -> voxel::MaskField* {
        auto it = doc->masks.find(id);
        return it == doc->masks.end() ? nullptr : &it->second;
    };
}

session::History::MeshFor mesh_for(const PyDocument& d) {
    std::shared_ptr<io::ClaySpaceDoc> doc = d.doc;
    return [doc](scene::LayerId id) -> mesh::Mesh* {
        auto it = doc->mesh_layers.find(id);
        return it == doc->mesh_layers.end() ? nullptr : &it->second;
    };
}

// Bracket a mask edit so it becomes ONE undo step, matching the C binding's
// MaskStep. This used to record a BARRIER, because a mask had no history
// mechanism at all; it has one now, so a mask edit is an ordinary step.
struct PyMaskStep {
    session::History* history = nullptr;
    voxel::MaskField* mask = nullptr;

    PyMaskStep(const PyMaskField& handle, voxel::MaskField& m) {
        if (!handle.doc || !handle.undo || !*handle.undo) return;
        session::History* h = handle.undo->get();
        if (!h || !h->begin_mask_step(handle.layer, m)) return;
        history = h;
        mask = &m;
    }
    ~PyMaskStep() {
        if (history) history->end_mask_step(*mask);
    }
    PyMaskStep(const PyMaskStep&) = delete;
    PyMaskStep& operator=(const PyMaskStep&) = delete;
};

// Bracket a voxel edit so it becomes ONE undo step, matching the C binding's
// VoxelStep. A standalone grid has no document and therefore no history: undo
// is a document concept. RAII because the verbs below throw.
struct PyVoxelStep {
    session::History* history = nullptr;
    voxel::VoxelGrid* grid = nullptr;

    PyVoxelStep(const PyVoxelGrid& handle, voxel::VoxelGrid& g) {
        if (!handle.doc || !handle.undo || !*handle.undo) return;
        session::History* h = handle.undo->get();
        if (!h || !h->begin_voxel_step(handle.layer, g)) return;
        history = h;
        grid = &g;
    }
    ~PyVoxelStep() {
        if (history) history->end_voxel_step(*grid);
    }
    PyVoxelStep(const PyVoxelStep&) = delete;
    PyVoxelStep& operator=(const PyVoxelStep&) = delete;
};

math::Aabb to_aabb(nb::handle obj) {
    if (nb::isinstance<PyDocument>(obj)) {
        math::Aabb b = scene::compile_document(nb::cast<PyDocument&>(obj).doc->document).bounds;
        if (b.empty())
            throw std::invalid_argument(
                "the document is empty, so there is nothing to size the cut against — pass "
                "an explicit ((lo), (hi)) region");
        return b;
    }
    nb::sequence s;
    try {
        s = nb::cast<nb::sequence>(obj);
    } catch (const std::exception&) {
        throw std::invalid_argument("region must be a Document or a ((lo), (hi)) pair");
    }
    if (nb::len(s) != 2)
        throw std::invalid_argument("region must be a Document or a ((lo), (hi)) pair");
    return math::Aabb{to_f3(s[0], "region lo"), to_f3(s[1], "region hi")};
}

// -- consolidation helpers -----------------------------------------------------

scene::ConsolidationParams to_consolidation(float cell, nb::handle band, nb::handle padding,
                                            nb::handle region, bool redistance) {
    if (!(cell > 0.0f)) throw std::invalid_argument("cell must be > 0");
    scene::ConsolidationParams p;
    p.cell_size = cell;
    p.band = band.is_none() ? 0.0f : nb::cast<float>(band);
    p.padding = padding.is_none() ? 0.0f : nb::cast<float>(padding);
    p.skip_redistance = !redistance;
    if (!region.is_none()) p.region = to_aabb(region);
    return p;
}

nb::dict cost_dict(const scene::ConsolidationCost& c) {
    nb::dict out;
    out["cell_size"] = c.cell_size;
    out["band"] = c.band;
    out["brick_count"] = c.brick_count;
    out["sample_count"] = c.sample_count;
    out["megabytes"] = static_cast<double>(c.bytes) / (1024.0 * 1024.0);
    out["sample_lipschitz"] = c.sample_lipschitz;
    out["lipschitz"] = c.lipschitz;
    out["safe_step_scale"] = c.safe_step_scale;
    out["bounds"] = nb::make_tuple(
        nb::make_tuple(c.bounds.min.x, c.bounds.min.y, c.bounds.min.z),
        nb::make_tuple(c.bounds.max.x, c.bounds.max.y, c.bounds.max.z));
    return out;
}

struct PyLayer {
    std::shared_ptr<io::ClaySpaceDoc> doc;
    std::shared_ptr<UndoRef> undo;
    scene::LayerId id = 0;

    scene::Layer& layer() const {
        scene::Layer* l = doc->document.find_layer(id);
        if (!l) throw std::runtime_error("layer was removed from its document");
        return *l;
    }
};

// The one insertion path for Layer.add and Layer.add_group alike: an
// AddNodeCmd with a reserved id (replay preserves ids) so an enabled undo stack
// records the add like every other edit. Regression: a direct insert let adds
// escape undo. A parent that is not a group is refused here, where the id the
// caller passed can still be named in the message.
scene::NodeId insert_node(PyLayer& l, scene::Node n, nb::handle parent, int index) {
    scene::NodeId parent_id = scene::kNoNode;
    if (!parent.is_none()) {
        parent_id = nb::cast<scene::NodeId>(parent);
        const scene::Node* g = l.layer().sdf->find(parent_id);
        if (!g) throw std::invalid_argument("no node with that id in this layer");
        if (!g->is_group) throw std::invalid_argument("parent must be a group");
    }
    n.id = l.layer().sdf->reserve_id();
    scene::NodeId id = n.id;
    std::vector<scene::Node> subtree;
    subtree.push_back(std::move(n));
    apply_or_throw(l.doc->document,
                   scene::Command{scene::AddNodeCmd{l.id, parent_id, index, std::move(subtree)}},
                   "add", l.undo.get());
    return id;
}

// The group rules the C ABI states (clay.h, "-- groups --"): the inline op is
// groups only and reads no blend, rounding or colour off the group at all, and
// the transitions are items only — compile_group emits no transition
// parameters, so a group carrying one would morph on defaults nobody wrote.
void check_group_op_blend(scene::Op op, const scene::Blend& blend, float rounding) {
    if (scene::op_is_transition(op))
        throw std::invalid_argument("a group cannot carry a transition op");
    if (op == scene::Op::None &&
        (blend.profile != scene::BlendProfile::Hard || blend.k != 0.0f || rounding != 0.0f))
        throw std::invalid_argument(
            "an inline group reads no blend or rounding: its children combine into the "
            "outer chain with their own");
}

// -- numpy point evaluation -----------------------------------------------------

struct PointsView {
    nb::object array;  // keeps the (possibly converted) buffer alive
    const float* data = nullptr;
    std::size_t count = 0;
};

// (N,3) float32 C-contiguous is used zero-copy; float64 / non-contiguous
// inputs are converted (numpy-native data exchange requirement).
PointsView to_points(nb::handle obj) {
    nb::module_ np = nb::module_::import_("numpy");
    nb::object arr = np.attr("ascontiguousarray")(obj, "dtype"_a = "float32");
    nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
    try {
        view = nb::cast<decltype(view)>(arr);
    } catch (const std::exception&) {
        throw std::invalid_argument("points must be an (N, 3) array");
    }
    if (view.shape(1) != 3) throw std::invalid_argument("points must be an (N, 3) array");
    return PointsView{arr, view.data(), view.shape(0)};
}

// (N,2) float32 polygon vertices. Sequences of pairs also work.
std::vector<kernel::cfloat2> to_polygon(nb::handle obj) {
    nb::module_ np = nb::module_::import_("numpy");
    nb::object arr = np.attr("ascontiguousarray")(obj, "dtype"_a = "float32");
    nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
    try {
        view = nb::cast<decltype(view)>(arr);
    } catch (const std::exception&) {
        throw std::invalid_argument("polygon points must be an (N, 2) array");
    }
    if (view.shape(1) != 2)
        throw std::invalid_argument("polygon points must be an (N, 2) array");
    if (view.shape(0) < 3)
        throw std::invalid_argument("a polygon needs at least 3 vertices");
    std::vector<kernel::cfloat2> out;
    out.reserve(view.shape(0));
    for (std::size_t i = 0; i < view.shape(0); ++i)
        out.push_back(kernel::cf2(view.data()[i * 2], view.data()[i * 2 + 1]));
    return out;
}

// (N,4) float32 stroke points: xyz + radius. Sequences of 4-tuples also work.
scene::StrokePointType parse_point_type(const std::string& t) {
    if (t == "hard") return scene::StrokePointType::Hard;
    if (t == "spline") return scene::StrokePointType::Spline;
    if (t == "bspline") return scene::StrokePointType::BSpline;
    if (t == "bezier") return scene::StrokePointType::Bezier;
    throw std::invalid_argument(
        "point type must be 'hard', 'spline', 'bspline' or 'bezier', got '" + t + "'");
}

// One name for every point, or one per point. A single string is the common
// case — a whole curve is usually smooth — and a list is what a per-point
// editor produces.
void apply_point_types(std::vector<scene::StrokePoint>& points, nb::handle types) {
    if (types.is_none()) return;
    if (nb::isinstance<nb::str>(types)) {
        scene::StrokePointType t = parse_point_type(nb::cast<std::string>(types));
        for (scene::StrokePoint& p : points) p.type = t;
        return;
    }
    nb::sequence seq;
    try {
        seq = nb::cast<nb::sequence>(types);
    } catch (const std::exception&) {
        throw std::invalid_argument("types must be a string or a sequence of strings");
    }
    if (nb::len(seq) != points.size())
        throw std::invalid_argument("types must have one entry per point (" +
                                    std::to_string(points.size()) + ")");
    for (std::size_t i = 0; i < points.size(); ++i)
        points[i].type = parse_point_type(nb::cast<std::string>(seq[i]));
}

// (N, 3) handles, or None. Only Bezier points read them, but accepting them
// for every point keeps the arrays parallel to the point list.
void apply_handles(std::vector<scene::StrokePoint>& points, nb::handle in_handles,
                   nb::handle out_handles) {
    auto load = [&](nb::handle h, bool incoming) {
        if (h.is_none()) return;
        PointsView v = to_points(h);
        if (v.count != points.size())
            throw std::invalid_argument("handles must have one entry per point (" +
                                        std::to_string(points.size()) + ")");
        for (std::size_t i = 0; i < points.size(); ++i) {
            kernel::cfloat3 value =
                kernel::cf3(v.data[i * 3 + 0], v.data[i * 3 + 1], v.data[i * 3 + 2]);
            if (incoming)
                points[i].in_handle = value;
            else
                points[i].out_handle = value;
        }
    };
    load(in_handles, true);
    load(out_handles, false);
}

// Parent index per armature node. None means a plain chain — node i's parent
// is i-1 — which is the degenerate armature that equals a stroke, and the
// friendliest default for someone reaching for this the first time.
std::vector<std::uint32_t> to_parents(nb::handle obj, std::size_t count) {
    std::vector<std::uint32_t> parents;
    parents.reserve(count);
    if (obj.is_none()) {
        for (std::size_t i = 0; i < count; ++i)
            parents.push_back(static_cast<std::uint32_t>(i == 0 ? 0 : i - 1));
        return parents;
    }
    for (nb::handle h : nb::cast<nb::iterable>(obj)) {
        long long v = nb::cast<long long>(h);
        if (v < 0 || static_cast<std::size_t>(v) >= count)
            throw std::invalid_argument("armature parent index out of range");
        parents.push_back(static_cast<std::uint32_t>(v));
    }
    if (parents.size() != count)
        throw std::invalid_argument("armature needs one parent per node");
    // A cycle would make the field depend on traversal order rather than on
    // the tree, so it is refused here where the message can say so.
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t walk = i, steps = 0;
        while (parents[walk] != walk && steps++ <= count)
            walk = parents[walk];
        if (steps > count) throw std::invalid_argument("armature parents form a cycle");
    }
    return parents;
}

// Sign per armature node, +1 or -1. None means all positive — the rig every
// armature was before signs existed. Anything other than ±1 is refused rather
// than coerced, the same reading the C setter makes.
std::vector<std::int8_t> to_signs(nb::handle obj, std::size_t count) {
    std::vector<std::int8_t> signs;
    if (obj.is_none()) return signs;
    signs.reserve(count);
    for (nb::handle h : nb::cast<nb::iterable>(obj)) {
        long long v = nb::cast<long long>(h);
        if (v != 1 && v != -1)
            throw std::invalid_argument("an armature sign must be +1 or -1");
        signs.push_back(static_cast<std::int8_t>(v));
    }
    if (signs.size() != count)
        throw std::invalid_argument("armature needs one sign per node");
    return signs;
}

std::vector<scene::StrokePoint> to_stroke_points(nb::handle obj) {
    nb::module_ np = nb::module_::import_("numpy");
    nb::object arr = np.attr("ascontiguousarray")(obj, "dtype"_a = "float32");
    nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
    try {
        view = nb::cast<decltype(view)>(arr);
    } catch (const std::exception&) {
        throw std::invalid_argument("stroke points must be an (N, 4) array of x, y, z, radius");
    }
    if (view.shape(1) != 4)
        throw std::invalid_argument("stroke points must be an (N, 4) array of x, y, z, radius");
    std::vector<scene::StrokePoint> out;
    out.reserve(view.shape(0));
    for (std::size_t i = 0; i < view.shape(0); ++i) {
        const float* p = view.data() + i * 4;
        if (p[3] < 0.0f) throw std::invalid_argument("stroke radius must be >= 0");
        out.push_back(scene::StrokePoint{kernel::cf3(p[0], p[1], p[2]), p[3]});
    }
    return out;
}

// (N,6) float32 rays: origin xyz + direction xyz (normalized on the way in).
struct RaysView {
    nb::object array;
    std::vector<float> data;  // normalized copy
    std::size_t count = 0;
};

RaysView to_rays(nb::handle obj) {
    nb::module_ np = nb::module_::import_("numpy");
    nb::object arr = np.attr("ascontiguousarray")(obj, "dtype"_a = "float32");
    nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
    try {
        view = nb::cast<decltype(view)>(arr);
    } catch (const std::exception&) {
        throw std::invalid_argument("rays must be an (N, 6) array of origin + direction");
    }
    if (view.shape(1) != 6)
        throw std::invalid_argument("rays must be an (N, 6) array of origin + direction");
    RaysView out;
    out.array = arr;
    out.count = view.shape(0);
    out.data.assign(view.data(), view.data() + out.count * 6);
    for (std::size_t i = 0; i < out.count; ++i) {
        kernel::cfloat3 d = kernel::cf3(out.data[i * 6 + 3], out.data[i * 6 + 4],
                                        out.data[i * 6 + 5]);
        float len = kernel::clength(d);
        if (len < 1e-12f) throw std::invalid_argument("ray direction must be non-zero");
        out.data[i * 6 + 3] = d.x / len;
        out.data[i * 6 + 4] = d.y / len;
        out.data[i * 6 + 5] = d.z / len;
    }
    return out;
}

// (N,3) int32 voxel coordinates.
std::vector<voxel::VoxelCoord> to_coords(nb::handle obj) {
    nb::module_ np = nb::module_::import_("numpy");
    nb::object arr = np.attr("ascontiguousarray")(obj, "dtype"_a = "int32");
    nb::ndarray<const std::int32_t, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
    try {
        view = nb::cast<decltype(view)>(arr);
    } catch (const std::exception&) {
        throw std::invalid_argument("coordinates must be an (N, 3) integer array");
    }
    if (view.shape(1) != 3)
        throw std::invalid_argument("coordinates must be an (N, 3) integer array");
    std::vector<voxel::VoxelCoord> out;
    out.reserve(view.shape(0));
    for (std::size_t i = 0; i < view.shape(0); ++i) {
        const std::int32_t* c = view.data() + i * 3;
        out.push_back(voxel::VoxelCoord{c[0], c[1], c[2]});
    }
    return out;
}

voxel::VoxelCoord to_coord(nb::handle h) {
    nb::sequence s;
    try {
        s = nb::cast<nb::sequence>(h);
    } catch (const std::exception&) {
        throw std::invalid_argument("cell must be a sequence of 3 integers");
    }
    if (nb::len(s) != 3) throw std::invalid_argument("cell must be a sequence of 3 integers");
    return voxel::VoxelCoord{nb::cast<std::int32_t>(s[0]), nb::cast<std::int32_t>(s[1]),
                             nb::cast<std::int32_t>(s[2])};
}

enum class Want { Distances, Gradients, Colors };

// (N, 3) points in, (N,) floats out. Shared by everything that answers one
// number per point without going through a backend.
template <typename Fn>
nb::object map_points(nb::handle points, Fn&& fn) {
    PointsView pts = to_points(points);
    const std::size_t n = pts.count;
    float* out = new float[n ? n : 1];
    nb::capsule owner(out, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    {
        nb::gil_scoped_release release;
        for (std::size_t i = 0; i < n; ++i)
            out[i] = fn(kernel::cf3(pts.data[i * 3], pts.data[i * 3 + 1], pts.data[i * 3 + 2]));
    }
    return nb::cast(nb::ndarray<nb::numpy, float>(out, {n}, owner));
}

nb::object eval_field(const scene::Tape& tape, nb::handle points,
                      const std::string& backend_name, Want want) {
    eval::Backend* backend = find_backend(backend_name);
    PointsView pts = to_points(points);
    const std::size_t n = pts.count;

    float* dist = new float[n ? n : 1];
    nb::capsule dist_owner(dist, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    float* aux = nullptr;
    nb::capsule aux_owner;
    if (want != Want::Distances) {
        aux = new float[n ? n * 3 : 1];
        aux_owner = nb::capsule(aux, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    }

    eval::PointQuery q{pts.data, n, 1e-4f};
    eval::PointResults out{dist, want == Want::Gradients ? aux : nullptr,
                           want == Want::Colors ? aux : nullptr};
    eval::Status status;
    {
        nb::gil_scoped_release release;  // heavy path runs without the GIL
        status = backend->eval_points(tape, q, out);
    }
    if (status != eval::Status::Ok) throw std::runtime_error("point evaluation failed");
    if (want == Want::Distances)
        return nb::cast(nb::ndarray<nb::numpy, float>(dist, {n}, dist_owner));
    return nb::cast(nb::ndarray<nb::numpy, float>(aux, {n, 3}, aux_owner));
}

PyMesh mesh_document(const PyDocument& d, int resolution, nb::handle voxel_size,
                     nb::handle decimate_ratio, const std::string& backend_name,
                     const std::string& mesher, bool experimental) {
    find_backend(backend_name);  // availability changes speed, never results
    scene::Tape tape = scene::compile_document(d.doc->document);
    if (tape.empty() || tape.bounds.empty() || tape.bounds.is_infinite())
        throw std::invalid_argument("document has no bounded SDF content to mesh");
    if (resolution <= 0) throw std::invalid_argument("resolution must be > 0");
    float voxel = voxel_size.is_none() ? 0.0f : nb::cast<float>(voxel_size);
    if (voxel <= 0.0f) {
        kernel::cfloat3 ext = tape.bounds.extent();
        voxel = kernel::cmax(ext.x, kernel::cmax(ext.y, ext.z)) / static_cast<float>(resolution);
    }
    float ratio = decimate_ratio.is_none() ? 0.0f : nb::cast<float>(decimate_ratio);
    if (mesher != "marching" && mesher != "nets" && mesher != "dual_contouring")
        throw std::invalid_argument(
            "mesher must be 'marching', 'nets' or 'dual_contouring', got '" + mesher + "'");
    // Dual contouring ships flagged/experimental per the meshing spec.
    if (mesher == "dual_contouring" && !experimental)
        throw std::invalid_argument(
            "the dual_contouring mesher is experimental: pass experimental=True to opt in");
    PyMesh out;
    {
        nb::gil_scoped_release release;
        if (mesher == "nets") {
            out.m = mesh::mesh_tape_nets(tape, tape.bounds, voxel);
        } else if (mesher == "dual_contouring") {
            mesh::DualContouringOptions dc;
            dc.enable_experimental = true;
            out.m = mesh::mesh_tape_dc(tape, tape.bounds, voxel, dc);
        } else {
            out.m = mesh::mesh_tape(tape, tape.bounds, voxel);
        }
        // What the artist put away does not come back in the export. BEFORE
        // decimation, so the decimator spends its triangle budget on surface
        // that will actually be seen rather than on surface about to be
        // dropped. A no-op when nothing is hidden.
        if (d.doc->groups) voxel::drop_hidden(out.m, *d.doc->groups);
        if (ratio > 0.0f) {
            mesh::DecimateOptions opts;
            opts.target_ratio = ratio;
            out.m = mesh::decimate(out.m, opts);
        }
    }
    return out;
}

// -- quad meshing -------------------------------------------------------------
//
// A LATTICE-DERIVED QUAD GRID, not field-aligned retopology. The docstrings
// say so at every entry point, because a user reaching for this expecting
// ZRemesher's output must learn it from the documentation and not from the
// mesh.

// clay.h's CLAY_MAX_BATCH, restated because pyclay builds against the C++ API
// and never sees the C ABI's header. The two must hold the same number: it is
// what turns a mistyped target — a byte count, a shift that went one place too
// far — into an error instead of an hour of meshing.
constexpr long long kMaxQuadTarget = 16777216;  // 1 << 24

// The count controls, shared by the document and the voxel entry points so
// there is one set of rules for what a target means. The rules are the C
// ABI's, knob for knob: clay_quad_params documents the same three fields, and
// a value that binding accepts must not be refused here — one input, one
// answer, in both bindings.
mesh::QuadTarget quad_target_from(nb::handle target, float tolerance, int max_iterations) {
    mesh::QuadTarget want;
    if (!target.is_none()) {
        long long asked = 0;
        // try_cast, not cast: an integer too large for a long long escapes
        // nb::cast as a bare std::bad_cast, which is not one of this API's
        // errors and tells a caller nothing. 2**63 is out of range for the
        // same reason 2**40 is, and should say so in the same words.
        if (!nb::try_cast<long long>(target, asked))
            throw std::invalid_argument("target must be a whole number of quads, 0.." +
                                        std::to_string(kMaxQuadTarget));
        if (asked < 0) throw std::invalid_argument("target must be >= 0");
        if (asked > kMaxQuadTarget)
            throw std::invalid_argument("target above the ceiling of " +
                                        std::to_string(kMaxQuadTarget) + " quads: " +
                                        std::to_string(asked));
        want.target = static_cast<std::size_t>(asked);
    }
    // <= 0 means the default, which is what clay_quad_params says and what a C
    // caller who declared only the original struct layout sends. A NaN or an
    // infinity is refused rather than folded into that default: it fails every
    // comparison on the way in, so it would silently become 0.10 here while
    // read_quad_params rejects it with "tolerance must be finite".
    if (!std::isfinite(tolerance))
        throw std::invalid_argument("tolerance must be finite; <= 0 means the default");
    if (tolerance >= 1.0f)
        throw std::invalid_argument(
            "tolerance is a fraction of the target, below 1; <= 0 means the default");
    // Every iteration is a whole mesh, so this is a cost knob and an absurd
    // value buys that many dense field evaluations rather than a better answer.
    // 0 is the default, as in the C struct; a NEGATIVE is a mistake and is
    // refused rather than read as one.
    if (max_iterations < 0 || max_iterations > 64)
        throw std::invalid_argument("max_iterations must be 0..64; 0 means the default");
    want.tolerance = tolerance;
    want.max_iterations = max_iterations;
    return want;
}

// The lattice, read the way the C ABI reads it. A NaN or an infinity is
// refused rather than passed through: it fails every `> 0` test on the way in,
// so it would silently become "no cell size given" and be answered with an
// estimated seed, while clay_document_mesh_quads refuses the same value with
// "cell size must be finite". One input, one answer, in both bindings.
float quad_cell_from(nb::handle cell_size) {
    if (cell_size.is_none()) return 0.0f;
    const float cell = nb::cast<float>(cell_size);
    if (cell != 0.0f && !std::isfinite(cell))
        throw std::invalid_argument("cell_size must be finite");
    return cell;
}

nb::object quad_report_dict(const mesh::QuadFit& fit, std::size_t target) {
    nb::dict out;
    out["cell_size"] = fit.cell_size;
    out["target"] = target;
    out["quad_count"] = fit.quad_count;
    out["iterations"] = fit.iterations;
    out["within_tolerance"] = fit.within_tolerance;
    out["clamped"] = fit.clamped;
    return out;
}

PyMesh mesh_document_quads(const PyDocument& d, nb::handle cell_size, nb::handle target,
                           float tolerance, int max_iterations, const std::string& mode) {
    if (mode == "faces")
        throw std::invalid_argument(
            "the 'faces' mode meshes exposed VOXEL faces, and a document has none — use "
            "VoxelGrid.mesh_quads(mode='faces'), or 'dual' here");
    if (mode != "dual") throw std::invalid_argument("mode must be 'dual', got '" + mode + "'");

    scene::Tape tape = scene::compile_document(d.doc->document);
    if (tape.empty() || tape.bounds.empty() || tape.bounds.is_infinite())
        throw std::invalid_argument("document has no bounded SDF content to mesh");
    const float cell = quad_cell_from(cell_size);
    const mesh::QuadTarget want = quad_target_from(target, tolerance, max_iterations);
    if (!(cell > 0.0f) && want.target == 0)
        throw std::invalid_argument(
            "give a cell_size, a target, or both — neither names a lattice");

    PyMesh out;
    mesh::QuadFit fit;
    {
        nb::gil_scoped_release release;
        out.m = mesh::mesh_tape_quads_fit(tape, tape.bounds, cell, want, {}, &fit);
        // Filtered BY QUAD, so the export keeps its quads — see
        // voxel::drop_hidden, which exists in that shape for this path.
        if (d.doc->groups) voxel::drop_hidden(out.m, *d.doc->groups);
    }
    out.fit = fit;
    out.fit_target = want.target;
    return out;
}

PyMesh mesh_voxel_quads(const voxel::VoxelGrid& grid, const std::string& mode,
                        nb::handle cell_size, nb::handle target, float tolerance,
                        int max_iterations, int blur, std::size_t level) {
    voxel::VoxelGrid::QuadOptions options;
    if (mode == "faces") {
        options.mode = voxel::VoxelGrid::QuadOptions::Mode::Faces;
    } else if (mode != "dual") {
        throw std::invalid_argument("mode must be 'dual' or 'faces', got '" + mode + "'");
    }
    if (blur < 0 || blur > 8) throw std::invalid_argument("blur must be 0..8 passes");
    if (level >= grid.level_count())
        throw std::invalid_argument("no such resolution level: " + std::to_string(level));
    options.cell_size = quad_cell_from(cell_size);
    options.blur = blur;
    options.level = level;
    const mesh::QuadTarget want = quad_target_from(target, tolerance, max_iterations);

    PyMesh out;
    mesh::QuadFit fit;
    {
        nb::gil_scoped_release release;
        out.m = grid.mesh_quads_fit(options, want, &fit);
    }
    out.fit = fit;
    out.fit_target = want.target;
    return out;
}

}  // namespace

NB_MODULE(pyclay, m) {
    m.doc() = "claycore Python bindings: SDF document authoring, evaluation, meshing, file I/O";
    Version v = version();
    m.attr("__version__") = (std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
                             std::to_string(v.patch));

    nb::enum_<scene::Op>(m, "Op", "Combine operator applied when adding an edit to a layer")
        .value("ADD", scene::Op::Add)
        .value("SUBTRACT", scene::Op::Subtract)
        .value("INTERSECT", scene::Op::Intersect)
        .value("PAINT", scene::Op::Paint)
        // extended vocabulary (kernel/ops.h); blend k is the mode radius and
        // groove/tongue additionally read the item's rounding as half-width
        .value("GROOVE", scene::Op::Groove)
        .value("TONGUE", scene::Op::Tongue)
        .value("PIPE", scene::Op::Pipe)
        .value("ENGRAVE", scene::Op::Engrave)
        .value("EMBOSS", scene::Op::Emboss)
        .value("INSET", scene::Op::Inset)
        .value("SHELL", scene::Op::Shell)
        .value("REPLACE", scene::Op::Replace)
        // Surface relief: the item is a REGION, not a shape. blend_k is the
        // amplitude by which the surface accumulated BEFORE it moves along its
        // own normal, and the item's rounding is the falloff width. A PAIR
        // rather than one signed amplitude, because blend_k cannot be negative.
        .value("RELIEF", scene::Op::Relief)
        .value("INCISE", scene::Op::Incise)
        // spatial morphs: need a transition= argument, and are NON-LOCAL
        // (never culled) because their weight reaches arbitrarily far
        .value("TRANSITION_LINEAR", scene::Op::TransitionLinear)
        .value("TRANSITION_RADIAL", scene::Op::TransitionRadial)
        // GROUPS ONLY (Layer.add_group): the group's children apply inline to
        // the chain outside it, as if they had been added there. Every other
        // op makes the group a sub-expression combining as a unit.
        .value("INLINE", scene::Op::None);

    // -- blends ----------------------------------------------------------------
    nb::class_<PyTransition>(m, "Transition",
                             "Base class of TransitionLinear / TransitionRadial");
    nb::class_<PyTransitionLinear, PyTransition>(
        m, "TransitionLinear",
        "Morph weight from the eased projection onto the segment a -> b")
        .def("__init__",
             [](PyTransitionLinear* self, nb::handle a, nb::handle b, int ease) {
                 new (self) PyTransitionLinear();
                 self->t.a = to_f3(a, "a");
                 self->t.b = to_f3(b, "b");
                 if (ease < 0 || ease >= kernel::ease_count)
                     throw std::invalid_argument("ease must be a valid easing curve index");
                 self->t.ease = static_cast<std::uint8_t>(ease);
             },
             "a"_a, "b"_a, "ease"_a = 0);
    nb::class_<PyTransitionRadial, PyTransition>(
        m, "TransitionRadial", "Morph weight from the eased XZ radius between r0 and r1")
        .def("__init__",
             [](PyTransitionRadial* self, float r0, float r1, int ease) {
                 new (self) PyTransitionRadial();
                 if (r0 == r1) throw std::invalid_argument("transition needs r0 != r1");
                 self->t.r0 = r0;
                 self->t.r1 = r1;
                 if (ease < 0 || ease >= kernel::ease_count)
                     throw std::invalid_argument("ease must be a valid easing curve index");
                 self->t.ease = static_cast<std::uint8_t>(ease);
             },
             "r0"_a, "r1"_a, "ease"_a = 0);

    nb::class_<PyBlend>(m, "Blend", "Base class of Smooth/Cubic/Circular/Chamfer blend kinds")
        .def_prop_ro("k", [](const PyBlend& b) { return b.b.k; });
    nb::class_<PySmooth, PyBlend>(m, "Smooth", "Quadratic smooth-min blend of radius k")
        .def(nb::init<float>(), "k"_a);
    nb::class_<PyCubic, PyBlend>(m, "Cubic", "Cubic smooth-min blend of radius k")
        .def(nb::init<float>(), "k"_a);
    nb::class_<PyCircular, PyBlend>(m, "Circular", "Circular smooth-min blend of radius k")
        .def(nb::init<float>(), "k"_a);
    nb::class_<PyChamfer, PyBlend>(m, "Chamfer", "45-degree chamfer blend of width k")
        .def(nb::init<float>(), "k"_a);

    // -- primitives --------------------------------------------------------------
    nb::class_<PyPrim>(m, "Prim",
                       "A primitive plus placement (position, rotation_axis_angle, scale)")
        .def(
            "at",
            [](nb::object self, nb::handle position) {
                nb::cast<PyPrim&>(self).xform.position = to_f3(position, "position");
                return self;
            },
            "position"_a, "Set the primitive position; returns self for chaining")
        // -- deformer modifiers (add-tape-deformers). Chainable and ordered:
        // the point is warped by the first-called deformer first.
        .def(
            "twist",
            [](nb::object self, float k) {
                nb::cast<PyPrim&>(self).deformers.push_back(scene::Deformer::twist(k));
                return self;
            },
            "k"_a, "Twist about Y at k radians per unit of height")
        .def(
            "bend",
            [](nb::object self, float k) {
                nb::cast<PyPrim&>(self).deformers.push_back(scene::Deformer::bend(k));
                return self;
            },
            "k"_a, "Bend along X at k radians per unit")
        .def(
            "taper",
            [](nb::object self, float y0, float y1, float s0, float s1, int ease) {
                if (y1 == y0) throw std::invalid_argument("taper needs y1 != y0");
                if (s0 <= 0.0f || s1 <= 0.0f)
                    throw std::invalid_argument("taper scales must be > 0");
                if (ease < 0 || ease >= kernel::ease_count)
                    throw std::invalid_argument("ease must be a valid easing curve index");
                nb::cast<PyPrim&>(self).deformers.push_back(
                    scene::Deformer::taper(y0, y1, s0, s1, static_cast<std::uint8_t>(ease)));
                return self;
            },
            "y0"_a, "y1"_a, "s0"_a, "s1"_a, "ease"_a = 0,
            "Scale the cross-section from s0 at y0 to s1 at y1 along an easing curve")
        .def(
            "noise",
            [](nb::object self, float amplitude, float frequency, int octaves, float gain,
               std::uint32_t seed) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (octaves < 1) throw std::invalid_argument("noise needs at least one octave");
                if (!(frequency > 0.0f)) throw std::invalid_argument("noise frequency must be > 0");
                p.deformers.push_back(
                    scene::Deformer::noise(amplitude, frequency, octaves, gain, seed));
                return self;
            },
            "amplitude"_a, "frequency"_a, "octaves"_a = 4, "gain"_a = 0.5f, "seed"_a = 0u,
            nb::rv_policy::reference_internal,
            "Fractal gradient noise, offsetting the distance — the irregular\n"
            "sibling of `displace`, whose sine is regular by construction and\n"
            "gives an even corrugation instead.\n\n"
            "The hash is INTEGER, and that is not an implementation detail: a\n"
            "float hash amplifies the units-in-the-last-place difference between\n"
            "each backend's `sin` into an O(1) difference, so it could not agree\n"
            "across CPU, Metal, CUDA and OpenCL. Integer operations give the same\n"
            "bits everywhere.\n\n"
            "The SEED is an ordinary parameter, not global state: two items with\n"
            "the same seed look the same, and an item's appearance never depends\n"
            "on the order it was compiled in.\n\n"
            "The fractal is normalized, so raising `octaves` adds finer detail\n"
            "without growing the overall deviation — `amplitude` stays the one\n"
            "control for how far the surface moves. Offsetting the distance\n"
            "costs the marcher: the step scale drops with amplitude x frequency.")
        .def(
            "magnify",
            [](nb::object self, nb::handle center, float radius, float strength, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (!(radius > 0.0f)) throw std::invalid_argument("magnify radius must be > 0");
                p.deformers.push_back(scene::Deformer::magnify(
                    to_f3(center, "center"), radius, strength, static_cast<std::uint8_t>(ease)));
                return self;
            },
            "center"_a, "radius"_a, "strength"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "Magnify and pinch, which are the same deformation: a radial scale\n"
            "about `center` with finite support. ONE SIGNED STRENGTH covers both\n"
            "— positive swells the surface away from the centre, negative gathers\n"
            "it toward — so there is no separate pinch to go looking for.\n\n"
            "Scaling space is not distance preserving, so the field stops being\n"
            "exact and the document's safe step scale drops with the strength.\n"
            "Support is finite: outside the radius nothing changes, which is what\n"
            "keeps item influence bounds tight.")
        .def(
            "grab",
            [](nb::object self, nb::handle center, float radius, nb::handle displacement, int ease,
               bool front_only) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (!(radius > 0.0f)) throw std::invalid_argument("grab radius must be > 0");
                p.deformers.push_back(scene::Deformer::grab(
                    to_f3(center, "center"), radius, to_f3(displacement, "displacement"),
                    static_cast<std::uint8_t>(ease), front_only));
                return self;
            },
            "center"_a, "radius"_a, "displacement"_a, "ease"_a = 0, "front_only"_a = false,
            nb::rv_policy::reference_internal,
            "Pull a region of surface: displacement weighted from the centre out, "
            "identity past the radius. front_only leaves the far side of a form "
            "where it was. The surface travels less than the full displacement.")
        .def(
            "pose",
            [](nb::object self, nb::handle center, float radius, nb::handle axis, float angle,
               int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (!(radius > 0.0f)) throw std::invalid_argument("pose radius must be > 0");
                p.deformers.push_back(scene::Deformer::pose(to_f3(center, "center"), radius,
                                                            to_f3(axis, "axis"), angle,
                                                            static_cast<std::uint8_t>(ease)));
                return self;
            },
            "center"_a, "radius"_a, "axis"_a, "angle"_a, "ease"_a = 0,
            nb::rv_policy::reference_internal,
            "Rotate a region about the centre, weighted the same way as grab")
        .def(
            "pose_line",
            [](nb::object self, nb::handle a, nb::handle b, nb::handle axis, float angle,
               int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                kernel::cfloat3 pa = to_f3(a, "a"), pb = to_f3(b, "b");
                if (kernel::cdot2(pb - pa) <= 0.0f)
                    throw std::invalid_argument("pose_line needs a != b: the segment is the ramp");
                p.deformers.push_back(scene::Deformer::pose_line(pa, pb, to_f3(axis, "axis"), angle,
                                                                 static_cast<std::uint8_t>(ease)));
                return self;
            },
            "a"_a, "b"_a, "axis"_a, "angle"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "Rotate about the axis through a, ramping from nothing at a to the full "
            "angle at b and beyond. Unlike pose() this does not stop at a radius: "
            "everything past b turns with the tip.")
        .def(
            "bend_linear",
            [](nb::object self, nb::handle a, nb::handle b, nb::handle v, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                kernel::cfloat3 pa = to_f3(a, "a"), pb = to_f3(b, "b");
                if (kernel::cdot2(pb - pa) <= 0.0f)
                    throw std::invalid_argument(
                        "bend_linear needs a != b: the segment is the ramp span");
                p.deformers.push_back(scene::Deformer::bend_linear(
                    pa, pb, to_f3(v, "v"), static_cast<std::uint8_t>(ease)));
                return self;
            },
            "a"_a, "b"_a, "v"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "Displace by v, eased along the segment a -> b")
        .def(
            "blob",
            [](nb::object self, nb::handle center, float radius, float amplitude, float frequency,
               int octaves, float gain, std::uint32_t seed, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (!(radius > 0.0f)) throw std::invalid_argument("blob needs a radius > 0");
                p.deformers.push_back(scene::Deformer::blob(to_f3(center, "center"), radius,
                                                            amplitude, frequency, octaves, gain,
                                                            seed, static_cast<std::uint8_t>(ease)));
                return self;
            },
            "center"_a, "radius"_a, "amplitude"_a, "frequency"_a = 6.0f, "octaves"_a = 3,
            "gain"_a = 0.5f, "seed"_a = 0u, "ease"_a = 0, nb::rv_policy::reference_internal,
            "ZBrush's BLOB: an irregular swelling under the brush, rather than\n"
            "the smooth one `draw` gives.\n\n"
            "`noise` with the finite support `grab` and `magnify` have — outside\n"
            "`radius` the field is untouched, which is what makes it a brush\n"
            "rather than a modifier.\n\n"
            "The amplitude is signed and so is the noise, so ONE dab both swells\n"
            "and eats in — which is what reads as blobby rather than as a\n"
            "uniform bulge. A negative amplitude is not a second verb.\n\n"
            "The seed is an ordinary parameter rather than global state, so two\n"
            "items with the same seed look the same and an item's appearance\n"
            "never depends on the order it was compiled in.")
        .def(
            "alpha",
            [](nb::object self, nb::handle samples, nb::handle centre, nb::handle direction,
               nb::handle tangent, float extent, float radius, float amplitude, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                // A 2D array, because a stamp IS two-dimensional and letting a
                // caller pass a flat one with separate dimensions is the
                // multiply they would get wrong.
                auto arr = nb::cast<nb::ndarray<const float, nb::ndim<2>, nb::c_contig>>(samples);
                const int h = static_cast<int>(arr.shape(0));
                const int w = static_cast<int>(arr.shape(1));
                if (w < 2 || h < 2)
                    throw std::invalid_argument(
                        "an alpha needs at least 2x2 samples; there is nothing to "
                        "interpolate below that");
                if (!(extent > 0.0f))
                    throw std::invalid_argument("an alpha's extent must be positive");
                // Mirrors clay_item_add_alpha exactly; the two doors already
                // share the width and extent refusals and must not drift. A
                // zero direction is not a plane and a non-positive radius
                // reaches nothing — both were accepted and silently inert.
                const kernel::cfloat3 dir = to_f3(direction, "direction");
                if (!(kernel::clength(dir) > 1e-9f))
                    throw std::invalid_argument(
                        "an alpha's direction must have length; it is the normal of the "
                        "stamp's plane, and all-zeroes is the mesh brush's convention, not "
                        "this one");
                if (!(radius > 0.0f))
                    throw std::invalid_argument("an alpha's radius must be positive");
                if (ease < 0 || ease >= kernel::ease_count)
                    throw std::invalid_argument("ease must be a valid easing curve index");
                p.deformers.push_back(
                    scene::Deformer::alpha(to_f3(centre, "centre"), dir,
                                           to_f3(tangent, "tangent"), arr.data(), w, h, extent,
                                           radius, amplitude, static_cast<std::uint8_t>(ease)));
                return self;
            },
            "samples"_a, "centre"_a, "direction"_a, "tangent"_a, "extent"_a, "radius"_a,
            "amplitude"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "An ALPHA stamp: a scalar image as a distance offset, under the same\n"
            "radial falloff `blob`, `grab` and `magnify` use. Pores, fabric,\n"
            "scales, stitching — how detail work is actually done.\n\n"
            "`samples` is a 2D (height, width) float array in [0, 1], row-major.\n"
            "It is COPIED. **The engine decodes no images** — load the PNG with\n"
            "whatever you like and hand over the array.\n\n"
            "A DEFORMER, not a primitive: an item shaped like the stamp would ADD\n"
            "material in the stamp's shape, where an alpha modulates a surface\n"
            "already there — pores in existing skin.\n\n"
            "The stamp covers a square of side `extent` in the plane through\n"
            "`centre` with normal `direction`; `tangent` orients it there and any\n"
            "rough up will do, since it is re-orthogonalised. Outside `radius`\n"
            "the field is untouched EXACTLY.\n\n"
            "`amplitude` is how far the surface moves OUTWARD at a sample of 1, so\n"
            "white is raised as in every sculpting package; negative carves.\n\n"
            "The Lipschitz bound is DERIVED from the samples — the largest\n"
            "difference between adjacent ones over the world distance between\n"
            "them — so a flat stamp costs nothing for having large values, and a\n"
            "high-frequency one costs step scale honestly. Read it back with\n"
            "`Document.safe_step_scale()`.")
        .def(
            "gate",
            [](nb::object self, nb::handle mask, float width, float threshold) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                const voxel::MaskField* m = borrow_mask(mask);
                if (!m) throw std::invalid_argument("gate needs a MaskField");
                if (!(width > 0.0f))
                    throw std::invalid_argument(
                        "a gate's width must be positive: a step in the field has no finite "
                        "Lipschitz bound and nothing could march it");
                // Memoised against the mask's own change token: repainting
                // rebakes, gating another item by the same unchanged mask does
                // not. The band rule lives in GateBake so the two bindings
                // cannot drift on it.
                std::shared_ptr<const field::FieldVolume> measured =
                    gate_bake_of(mask)->gate_for(*m, threshold > 0.0f ? threshold : 0.5f, width);
                if (!measured)
                    throw std::invalid_argument(
                        "the mask is empty or nothing reaches the threshold, so the gate "
                        "would protect nothing");
                p.gate = std::move(measured);
                p.gate_width = width;
                return self;
            },
            "mask"_a, "width"_a = 0.1f, "threshold"_a = 0.5f, nb::rv_policy::reference_internal,
            "Gate this item by a painted MASK: it does not act where the mask\n"
            "protects. This is what makes masking protect a surface from ANY\n"
            "operation — a boolean included — rather than only from a brush.\n\n"
            "The mask is MEASURED, not stored: the item carries the signed\n"
            "distance to `mask >= threshold`. That is what gives the gate a\n"
            "Lipschitz bound worth having, since a distance is 1-Lipschitz and\n"
            "the falloff's cost is set by `width` — which you choose — rather\n"
            "than by however hard the brush edge that painted the mask was.\n"
            "Painted softness is re-derived rather than preserved.\n\n"
            "`width` is how far protection fades across, in world units. A WIDE\n"
            "gate costs almost no step scale and a narrow one costs honestly;\n"
            "read it back with `Document.safe_step_scale()`.\n\n"
            "THE GATE IS IN WORLD SPACE and does not travel with the item. A\n"
            "mask is painted in world units on its own lattice, so the region\n"
            "it protects is where you painted it and stays there whatever the\n"
            "item's own position, rotation and scale then do. Gate a cut over\n"
            "an ear and the ear stays; move the cut and the ear still stays.")
        .def(
            "lattice",
            [](nb::object self, nb::handle box, nb::handle offsets, int nx, int ny, int nz) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                const int cap = scene::Deformer::kMaxLatticeDivisions;
                if (nx < 2 || ny < 2 || nz < 2 || nx > cap || ny > cap || nz > cap)
                    throw std::invalid_argument(
                        "lattice divisions must be in [2, " + std::to_string(cap) +
                        "] per axis: the cage is evaluated per sample, at nx*ny*nz "
                        "multiply-adds each time");
                const math::Aabb b = to_aabb(box);
                if (b.empty())
                    throw std::invalid_argument(
                        "the cage's box is empty; there is nothing to span");
                scene::Deformer d = scene::Deformer::lattice(b.min, b.max, nx, ny, nz);
                if (!offsets.is_none()) {
                    PointsView v = to_points(offsets);
                    if (v.count != d.cage.size())
                        throw std::invalid_argument(
                            "offsets must have one entry per control point (" +
                            std::to_string(d.cage.size()) + ")");
                    for (std::size_t n = 0; n < d.cage.size(); ++n)
                        d.cage[n] =
                            kernel::cf3(v.data[n * 3], v.data[n * 3 + 1], v.data[n * 3 + 2]);
                }
                p.deformers.push_back(std::move(d));
                return self;
            },
            "box"_a, "offsets"_a = nb::none(), "nx"_a = 3, "ny"_a = 3, "nz"_a = 3,
            nb::rv_policy::reference_internal,
            "A LATTICE cage over the item's local `box` — ZBrush's Gizmo Lattice.\n\n"
            "`offsets` is an (nx*ny*nz, 3) array of control-point offsets in\n"
            "x-fastest order — index (i, j, k) at (k*ny + j)*nx + i — or None for\n"
            "an untouched cage, which is exactly the identity.\n\n"
            "The offsets are what you DRAGGED, and the cage is applied as the\n"
            "INVERSE warp — which is the design decision rather than a detail:\n"
            "forward FFD has no closed-form inverse, and a claycore deformer runs\n"
            "backwards. Material travels WITH the drag, as on the mesh lattice.\n\n"
            "It is not the EXACT inverse. It differs from forward FFD by a term\n"
            "proportional to how the basis varies along the displacement, so it\n"
            "over-travels a drag toward rising weight and under-travels one\n"
            "pointing away — under 1.5% of the drag, measured against the forward\n"
            "cage. (`grab` always under-travels, because its weight always falls\n"
            "off along the drag; a lattice does not inherit that sign.)\n\n"
            "For FORWARD FFD with no approximation, use the mesh-layer lattice\n"
            "(`Lattice` with `MeshSculptor.lattice`) — which is what ZBrush and\n"
            "Blender actually do, because a mesh knows where its vertices are.\n\n"
            "Divisions are capped at 4 per axis, far below the mesh lattice's 32,\n"
            "because this is evaluated PER SAMPLE inside the raymarcher.\n\n"
            "Two per axis is exactly trilinear and the corners are interpolated.\n"
            "A point outside the box travels rigidly with the nearest part of the\n"
            "cage rather than being drawn onto it.")
        .def(
            "bend_curve",
            [](nb::object self, nb::handle guide, float t0, float t1,
               const std::string& point_type) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                PointsView v = to_points(guide);
                if (v.count < 2)
                    throw std::invalid_argument("bend_curve needs at least two guide points");
                if (t0 == t1)
                    throw std::invalid_argument(
                        "bend_curve needs t0 != t1: the span is what gets laid on the guide");
                std::vector<scene::StrokePoint> points;
                points.reserve(v.count);
                for (std::size_t i = 0; i < v.count; ++i) {
                    scene::StrokePoint sp;
                    sp.pos = kernel::cf3(v.data[i * 3], v.data[i * 3 + 1], v.data[i * 3 + 2]);
                    sp.type = parse_point_type(point_type);
                    points.push_back(sp);
                }
                if (!(scene::guide_arc_length(points) > 0.0f))
                    throw std::invalid_argument(
                        "bend_curve guide has zero length: there is no arc to lay the span on");
                p.deformers.push_back(scene::Deformer::bend_curve(std::move(points), t0, t1));
                return self;
            },
            "guide"_a, "t0"_a, "t1"_a, "point_type"_a = "bspline",
            nb::rv_policy::reference_internal,
            "Bend along a DRAWN guide instead of at a constant rate.\n\n"
            "`bend` turns about a fixed axis at a fixed rate, so every bend it can\n"
            "express is a circular arc. This lays the item's local X span [t0, t1]\n"
            "onto the guide's ARC LENGTH and carries the material on the guide's\n"
            "parallel-transported frames — ZBrush's Gizmo Bend Curve.\n\n"
            "It is the inverse of a swept primitive rather than a second kind of\n"
            "bend, and it shares the sweep's machinery: `guide` is the same kind of\n"
            "curve every other item takes, so `point_type` gives it B-spline\n"
            "smoothing and it tessellates to the document's curve tolerance.\n\n"
            "A guide running straight along X is exactly the undeformed item.")
        .def(
            "twist_range",
            [](nb::object self, float radians_per_unit, float y0, float y1, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (y0 == y1)
                    throw std::invalid_argument("twist_range needs y0 != y1: the span is the ramp");
                p.deformers.push_back(scene::Deformer::twist_range(
                    radians_per_unit, y0, y1, static_cast<std::uint8_t>(ease)));
                return self;
            },
            "radians_per_unit"_a, "y0"_a, "y1"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "Twist about Y at k rad/unit, RAMPED across y0 -> y1 and held beyond.\n\n"
            "`twist` winds the whole item; a gizmo's twist acts inside its box, and\n"
            "this is that. Material past the range travels rigidly rather than\n"
            "continuing to wind.\n\n"
            "With a linear ease and a range covering the content it is exactly\n"
            "`twist` — the same rotation with the angle ramped, not a second\n"
            "deformation to keep in step.")
        .def(
            "bend_range",
            [](nb::object self, float radians_per_unit, float x0, float x1, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (x0 == x1)
                    throw std::invalid_argument("bend_range needs x0 != x1: the span is the ramp");
                p.deformers.push_back(scene::Deformer::bend_range(radians_per_unit, x0, x1,
                                                                  static_cast<std::uint8_t>(ease)));
                return self;
            },
            "radians_per_unit"_a, "x0"_a, "x1"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "Bend along X at k rad/unit, ramped across x0 -> x1 and held beyond —\n"
            "ZBrush's Bend Arc is angle-limited this way, where `bend` is not.")
        .def(
            "bend_radial",
            [](nb::object self, float r0, float r1, float dz, int ease) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (r0 == r1)
                    throw std::invalid_argument(
                        "bend_radial needs r0 != r1: the band is the ramp span");
                p.deformers.push_back(
                    scene::Deformer::bend_radial(r0, r1, dz, static_cast<std::uint8_t>(ease)));
                return self;
            },
            "r0"_a, "r1"_a, "dz"_a, "ease"_a = 0, nb::rv_policy::reference_internal,
            "Displace along Y by dz, eased across the radial band r0 -> r1")
        .def(
            "elongate_axis",
            [](nb::object self, nb::handle h) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                kernel::cfloat3 e = to_f3(h, "elongate_axis half-extents");
                if (e.x < 0.0f || e.y < 0.0f || e.z < 0.0f)
                    throw std::invalid_argument("elongate_axis half-extents must be >= 0");
                p.deformers.push_back(scene::Deformer::elongate_axis(e));
                return self;
            },
            "h"_a, nb::rv_policy::reference_internal,
            "Per-axis elongation: works on any primitive, symmetric or not, but "
            "the flat interior plateau makes the field a bound rather than exact.")
        .def(
            "elongate",
            [](nb::object self, nb::handle h) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                kernel::cfloat3 e = to_f3(h, "elongate half-extents");
                if (e.x < 0.0f || e.y < 0.0f || e.z < 0.0f)
                    throw std::invalid_argument("elongate half-extents must be >= 0");
                p.deformers.push_back(scene::Deformer::elongate(e));
                return self;
            },
            "h"_a, nb::rv_policy::reference_internal,
            "Insert flat sections of half-extent h along each axis: the shape "
            "stretches without its ends distorting. Exact on an origin-symmetric "
            "primitive, a bound otherwise.")
        .def(
            "displace",
            [](nb::object self, float amplitude, float frequency) {
                nb::cast<PyPrim&>(self).deformers.push_back(
                    scene::Deformer::displace(amplitude, frequency));
                return self;
            },
            "amplitude"_a, "frequency"_a,
            "Procedural sine displacement of the field (amplitude in world units)")
        .def(
            "wrap_around",
            [](nb::object self, float x0, float x1) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                if (x0 == x1)
                    throw std::invalid_argument(
                        "wrap_around needs x0 != x1: the interval fixes the cylinder radius");
                p.deformers.push_back(scene::Deformer::wrap_around(x0, x1));
                return self;
            },
            "x0"_a, "x1"_a, nb::rv_policy::reference_internal,
            "Bend the local X interval [x0, x1] around a cylinder about Z, so a "
            "flat relief wraps around a column. Radius is (x1 - x0) / 2pi.")
        .def(
            "repeat_grid",
            [](nb::object self, nb::handle spacing, nb::handle counts) {
                PyPrim& p = nb::cast<PyPrim&>(self);
                kernel::cfloat3 s3;
                if (nb::isinstance<nb::float_>(spacing) || nb::isinstance<nb::int_>(spacing)) {
                    float v = nb::cast<float>(spacing);
                    s3 = kernel::cf3(v, v, v);
                } else {
                    s3 = to_f3(spacing, "spacing");
                }
                if (s3.x <= 0 || s3.y <= 0 || s3.z <= 0)
                    throw std::invalid_argument("spacing must be > 0 on every axis");
                if (counts.is_none()) {
                    p.repeat = scene::Repeat::grid_infinite(s3);
                } else {
                    kernel::cfloat3 c = to_f3(counts, "counts");
                    if (c.x < 0 || c.y < 0 || c.z < 0)
                        throw std::invalid_argument("counts must be >= 0 (max cell index)");
                    if (s3.x != s3.y || s3.y != s3.z)
                        throw std::invalid_argument("finite grids use one spacing for all axes");
                    p.repeat = scene::Repeat::grid_finite(s3.x, c);
                }
                return self;
            },
            "spacing"_a, "counts"_a = nb::none(),
            "Repeat on a grid: infinite without counts, otherwise the max cell "
            "index per axis. An infinite grid is never culled (unbounded influence).")
        .def(
            "repeat_radial",
            [](nb::object self, int count, float offset) {
                if (count < 2) throw std::invalid_argument("radial count must be >= 2");
                nb::cast<PyPrim&>(self).repeat = scene::Repeat::radial(count, offset);
                return self;
            },
            "count"_a, "offset"_a = 0.0f,
            "Circular array of `count` copies about Y at the given radius")
        .def_prop_ro(
            "repeat",
            [](const PyPrim& p) -> nb::object {
                if (!p.repeat.active()) return nb::none();
                nb::dict d;
                const char* names[] = {"none", "grid_infinite", "grid_finite", "radial"};
                d["type"] = p.repeat.type < 4 ? names[p.repeat.type] : "unknown";
                d["spacing"] =
                    nb::make_tuple(p.repeat.spacing.x, p.repeat.spacing.y, p.repeat.spacing.z);
                d["counts"] =
                    nb::make_tuple(p.repeat.counts.x, p.repeat.counts.y, p.repeat.counts.z);
                return d;
            },
            "The repetition applied to this primitive, or None")
        .def_prop_ro(
            "deformers",
            [](const PyPrim& p) {
                nb::list out;
                for (const scene::Deformer& d : p.deformers) {
                    nb::dict e;
                    const char* names[] = {"twist", "bend", "taper", "displace"};
                    e["type"] = d.type < 4 ? names[d.type] : "unknown";
                    e["k"] = d.k;
                    e["a"] = d.a;
                    e["b"] = d.b;
                    e["c"] = d.c;
                    e["ease"] = d.ease;
                    out.append(e);
                }
                return out;
            },
            "The deformer chain, in application order");

    // Every primitive constructor accepts position=(x, y, z),
    // rotation_axis_angle=((x, y, z), radians) and scale=<one number, or
    // (sx, sy, sz) for a per-axis squash — issue #320>. The default is None
    // rather than 1.0 because "say nothing" and "say uniform 1" now differ:
    // the first leaves both halves of the scale alone.
#define CLAY_PLACE_ARGS                                            \
    nb::arg("position") = nb::none(),                              \
    nb::arg("rotation_axis_angle") = nb::none(), nb::arg("scale") = nb::none()

    nb::class_<PySphere, PyPrim>(m, "Sphere", "Sphere of radius r")
        .def("__init__",
             [](PySphere* self, float r, nb::handle pos, nb::handle rot, nb::handle scale) {
                 new (self) PySphere();
                 self->prim = scene::Prim::sphere(r);
                 place(*self, pos, rot, scale);
             },
             "r"_a = 1.0f, CLAY_PLACE_ARGS);
    nb::class_<PyBox, PyPrim>(m, "Box", "Axis-aligned box; size = full side lengths")
        .def("__init__",
             [](PyBox* self, nb::handle size, nb::handle pos, nb::handle rot, nb::handle scale) {
                 new (self) PyBox();
                 kernel::cfloat3 s = to_f3(size, "size");
                 self->prim = scene::Prim::box(s * 0.5f);
                 place(*self, pos, rot, scale);
             },
             "size"_a, CLAY_PLACE_ARGS);
    nb::class_<PyRoundBox, PyPrim>(m, "RoundBox",
                                   "Box with rounded edges; size = full side lengths, r = radius")
        .def("__init__",
             [](PyRoundBox* self, nb::handle size, float r, nb::handle pos, nb::handle rot,
                nb::handle scale) {
                 new (self) PyRoundBox();
                 kernel::cfloat3 s = to_f3(size, "size");
                 self->prim = scene::Prim::round_box(s * 0.5f, r);
                 place(*self, pos, rot, scale);
             },
             "size"_a, "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyTorus, PyPrim>(m, "Torus", "Torus: R = ring radius, r = tube radius")
        .def("__init__",
             [](PyTorus* self, float R, float r, nb::handle pos, nb::handle rot, nb::handle scale) {
                 new (self) PyTorus();
                 self->prim = scene::Prim::torus(R, r);
                 place(*self, pos, rot, scale);
             },
             "R"_a, "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyCapsule, PyPrim>(m, "Capsule", "Capsule between local endpoints a and b")
        .def("__init__",
             [](PyCapsule* self, nb::handle a, nb::handle b, float r, nb::handle pos,
                nb::handle rot, nb::handle scale) {
                 new (self) PyCapsule();
                 self->prim = scene::Prim::capsule(to_f3(a, "a"), to_f3(b, "b"), r);
                 place(*self, pos, rot, scale);
             },
             "a"_a, "b"_a, "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyCylinder, PyPrim>(m, "Cylinder",
                                   "Vertical capped cylinder: radius r, half-height h")
        .def("__init__",
             [](PyCylinder* self, float r, float h, nb::handle pos, nb::handle rot,
                nb::handle scale) {
                 new (self) PyCylinder();
                 self->prim = scene::Prim::capped_cylinder(r, h);
                 place(*self, pos, rot, scale);
             },
             "r"_a, "h"_a, CLAY_PLACE_ARGS);
    nb::class_<PyCone, PyPrim>(m, "Cone",
                               "Capped cone: half-height h, base radius r1, top radius r2")
        .def("__init__",
             [](PyCone* self, float h, float r1, float r2, nb::handle pos, nb::handle rot,
                nb::handle scale) {
                 new (self) PyCone();
                 self->prim = scene::Prim::capped_cone(h, r1, r2);
                 place(*self, pos, rot, scale);
             },
             "h"_a, "r1"_a, "r2"_a, CLAY_PLACE_ARGS);
    nb::class_<PyRoundCone, PyPrim>(
        m, "RoundCone", "Sphere-swept cone: radius r1 at origin, r2 at height h up the y axis")
        .def("__init__",
             [](PyRoundCone* self, float r1, float r2, float h, nb::handle pos, nb::handle rot,
                nb::handle scale) {
                 new (self) PyRoundCone();
                 self->prim = scene::Prim::round_cone(r1, r2, h);
                 place(*self, pos, rot, scale);
             },
             "r1"_a, "r2"_a, "h"_a, CLAY_PLACE_ARGS);
    nb::class_<PyEllipsoid, PyPrim>(m, "Ellipsoid",
                                    "Ellipsoid with per-axis radii r=(rx, ry, rz) (bound field)")
        .def("__init__",
             [](PyEllipsoid* self, nb::handle r, nb::handle pos, nb::handle rot, nb::handle scale) {
                 new (self) PyEllipsoid();
                 self->prim = scene::Prim::ellipsoid(to_f3(r, "r"));
                 place(*self, pos, rot, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyOctahedron, PyPrim>(m, "Octahedron", "Octahedron of size s")
        .def("__init__",
             [](PyOctahedron* self, float s, nb::handle pos, nb::handle rot, nb::handle scale) {
                 new (self) PyOctahedron();
                 self->prim = scene::Prim::octahedron(s);
                 place(*self, pos, rot, scale);
             },
             "s"_a, CLAY_PLACE_ARGS);
    nb::class_<PyHexPrism, PyPrim>(m, "HexPrism",
                                   "Hexagonal prism: hx = flat-to-flat half-width, hy = half-height")
        .def("__init__",
             [](PyHexPrism* self, float hx, float hy, nb::handle pos, nb::handle rot,
                nb::handle scale) {
                 new (self) PyHexPrism();
                 self->prim = scene::Prim::hex_prism(hx, hy);
                 place(*self, pos, rot, scale);
             },
             "hx"_a, "hy"_a, CLAY_PLACE_ARGS);
    nb::class_<PyPyramid, PyPrim>(m, "Pyramid", "Unit-base pyramid of height h")
        .def("__init__",
             [](PyPyramid* self, float h, nb::handle pos, nb::handle rot, nb::handle scale) {
                 new (self) PyPyramid();
                 self->prim = scene::Prim::pyramid(h);
                 place(*self, pos, rot, scale);
             },
             "h"_a, CLAY_PLACE_ARGS);

    // -- mesh ---------------------------------------------------------------------
    nb::class_<PyCappedTorus, PyPrim>(m, "CappedTorus",
                           "Torus arc: aperture half-angle, ring radius ra, tube radius rb")
        .def("__init__",
             [](PyCappedTorus* self, float aperture, float ra, float rb, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyCappedTorus();
                 self->prim = scene::Prim::capped_torus(aperture, ra, rb);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "aperture"_a, "ra"_a, "rb"_a, CLAY_PLACE_ARGS);

    nb::class_<PyLink, PyPrim>(m, "Link",
                           "Chain link: straight length, ring radius r1, tube radius r2")
        .def("__init__",
             [](PyLink* self, float length, float r1, float r2, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyLink();
                 self->prim = scene::Prim::link(length, r1, r2);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "length"_a, "r1"_a, "r2"_a, CLAY_PLACE_ARGS);

    nb::class_<PyCylinderInfinite, PyPrim>(m, "CylinderInfinite",
                           "Infinite cylinder along Y (UNBOUNDED: never culled)")
        .def("__init__",
             [](PyCylinderInfinite* self, float cx, float cz, float r, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyCylinderInfinite();
                 self->prim = scene::Prim::cylinder_infinite(cx, cz, r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "cx"_a = 0.0f, "cz"_a = 0.0f, "r"_a = 1.0f, CLAY_PLACE_ARGS);

    nb::class_<PyExactCone, PyPrim>(m, "ExactCone",
                           "Exact cone: half-angle at the apex, height h")
        .def("__init__",
             [](PyExactCone* self, float half_angle, float h, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyExactCone();
                 self->prim = scene::Prim::cone(half_angle, h);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "half_angle"_a, "h"_a, CLAY_PLACE_ARGS);

    nb::class_<PyPlane, PyPrim>(m, "Plane",
                           "Half-space with the given normal and offset (UNBOUNDED: never culled)")
        .def("__init__",
             [](PyPlane* self, nb::handle normal, float offset, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyPlane();
                 self->prim = scene::Prim::plane(to_f3(normal, "normal"), offset);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "normal"_a, "offset"_a = 0.0f, CLAY_PLACE_ARGS);

    nb::class_<PyCutSphere, PyPrim>(m, "CutSphere",
                           "Sphere of radius r cut by the plane y = h")
        .def("__init__",
             [](PyCutSphere* self, float r, float h, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyCutSphere();
                 self->prim = scene::Prim::cut_sphere(r, h);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, "h"_a, CLAY_PLACE_ARGS);

    nb::class_<PyCutHollowSphere, PyPrim>(m, "CutHollowSphere",
                           "Hollow cut sphere of wall thickness t")
        .def("__init__",
             [](PyCutHollowSphere* self, float r, float h, float t, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyCutHollowSphere();
                 self->prim = scene::Prim::cut_hollow_sphere(r, h, t);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, "h"_a, "t"_a, CLAY_PLACE_ARGS);

    nb::class_<PySolidAngle, PyPrim>(m, "SolidAngle",
                           "Spherical wedge: cone half-angle and radius")
        .def("__init__",
             [](PySolidAngle* self, float angle, float ra, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PySolidAngle();
                 self->prim = scene::Prim::solid_angle(angle, ra);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "angle"_a, "ra"_a, CLAY_PLACE_ARGS);

    nb::class_<PyTetrahedron, PyPrim>(m, "Tetrahedron",
                           "Regular tetrahedron of size r")
        .def("__init__",
             [](PyTetrahedron* self, float r, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyTetrahedron();
                 self->prim = scene::Prim::tetrahedron(r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);

    nb::class_<PyDodecahedron, PyPrim>(m, "Dodecahedron",
                           "Regular dodecahedron (plane folds)")
        .def("__init__",
             [](PyDodecahedron* self, float r, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyDodecahedron();
                 self->prim = scene::Prim::dodecahedron(r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);

    nb::class_<PyIcosahedron, PyPrim>(m, "Icosahedron",
                           "Regular icosahedron (plane folds)")
        .def("__init__",
             [](PyIcosahedron* self, float r, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyIcosahedron();
                 self->prim = scene::Prim::icosahedron(r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);

    nb::class_<PyTriPrism, PyPrim>(m, "TriPrism",
                           "Triangular prism (BOUND field, not exact)")
        .def("__init__",
             [](PyTriPrism* self, float hx, float hy, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyTriPrism();
                 self->prim = scene::Prim::tri_prism(hx, hy);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "hx"_a, "hy"_a, CLAY_PLACE_ARGS);

    nb::class_<PyOctahedronCheap, PyPrim>(m, "OctahedronCheap",
                           "Cheap octahedron (BOUND field, not exact)")
        .def("__init__",
             [](PyOctahedronCheap* self, float s, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyOctahedronCheap();
                 self->prim = scene::Prim::octahedron_cheap(s);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "s"_a, CLAY_PLACE_ARGS);

    nb::class_<PyLNormSphere, PyPrim>(m, "LNormSphere",
                           "Superellipsoid / L-norm sphere, n >= 2 (BOUND field)")
        .def("__init__",
             [](PyLNormSphere* self, float r, float n, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyLNormSphere();
                 self->prim = scene::Prim::lnorm_sphere(r, n);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, "n"_a = 4.0f, CLAY_PLACE_ARGS);

    nb::class_<PyProfile>(m, "Profile", "A closed 2D profile for Extrude / Revolve")
        .def_static("circle", [](float r) {
            PyProfile p;
            p.profile = scene::Profile::circle(r);
            return p;
        }, "r"_a)
        .def_static("box", [](float half_x, float half_y) {
            PyProfile p;
            p.profile = scene::Profile::box(half_x, half_y);
            return p;
        }, "half_x"_a, "half_y"_a)
        .def_static("hexagon", [](float r) {
            PyProfile p;
            p.profile = scene::Profile::hexagon(r);
            return p;
        }, "r"_a)
        .def_static("triangle", [](float r) {
            PyProfile p;
            p.profile = scene::Profile::triangle(r);
            return p;
        }, "r"_a)
        .def_static("trapezoid", [](float bottom, float top, float half_height) {
            PyProfile p;
            p.profile = scene::Profile::trapezoid(bottom, top, half_height);
            return p;
        }, "bottom"_a, "top"_a, "half_height"_a)
        .def_static("vesica", [](float r, float d) {
            PyProfile p;
            p.profile = scene::Profile::vesica(r, d);
            return p;
        }, "r"_a, "d"_a)
        .def_static("polygon", [](nb::handle points) {
            PyProfile p;
            p.profile = scene::Profile::polygon();
            p.points = to_polygon(points);
            return p;
        }, "points"_a,
           "Arbitrary closed polygon from an (N, 2) array; concave and "
           "self-intersecting outlines follow the even-odd rule. Flatten curves "
           "host-side (open curves are unsigned distances, not regions).")
        .def_prop_ro("point_count", [](const PyProfile& p) { return p.points.size(); });

    // ------------------------------------------------------------------
    nb::class_<PyArmature, PyPrim>(
        m, "Armature",
        "A TREE of spheres, skinned by one sphere-swept cone per node-parent\n"
        "pair — ZBrush's ZSpheres. ONE edit item, not one per link.\n\n"
        "This is Stroke with the chain generalised. A stroke joins point i to\n"
        "point i+1, so its topology is a line; an armature gives each node a\n"
        "parent, so it can branch. The link, the smooth union between links and\n"
        "the blend parameter are literally the same code, which is why an\n"
        "armature whose parents form a line evaluates identically to the stroke\n"
        "with the same points.\n\n"
        "`nodes` is (N, 4) — x, y, z, radius, as a stroke takes them. `parents`\n"
        "is (N,) indices; a node whose parent is itself is a root, and node 0\n"
        "defaults to one. `signs` is (N,) of +1/-1, all positive by default —\n"
        "ZBrush's negative ZSphere: the field is the armature of the positive\n"
        "nodes minus the armature of the negative nodes, so a negative node\n"
        "carves and its links carry no skin. Moving a node is the caller's\n"
        "business here; the document-side edits that carry a subtree are on\n"
        "Layer.\n\n"
        "There is deliberately no per-node ROTATION. A sphere is isotropic, so\n"
        "rotating one changes no distance and no surface — it earns its place in\n"
        "ZBrush because the adaptive skin lays out quads whose edge flow follows\n"
        "the node frames, and marching cubes, surface nets and dual contouring\n"
        "do not consult one.")
        .def("__init__",
             [](PyArmature* self, nb::handle nodes, nb::handle parents, nb::handle signs,
                float blend_k, nb::handle position, nb::handle rotation_axis_angle,
                nb::handle scale) {
                 new (self) PyArmature();
                 self->prim = scene::Prim::armature();
                 if (!nodes.is_none()) self->stroke = to_stroke_points(nodes);
                 if (blend_k < 0.0f) throw std::invalid_argument("blend_k must be >= 0");
                 self->stroke_blend_k = blend_k;
                 self->armature_parents = to_parents(parents, self->stroke.size());
                 self->armature_signs = to_signs(signs, self->stroke.size());
                 place(*self, position, rotation_axis_angle, scale);
             },
             "nodes"_a = nb::none(), "parents"_a = nb::none(), "signs"_a = nb::none(),
             "blend_k"_a = 0.0f, "position"_a = nb::none(),
             "rotation_axis_angle"_a = nb::none(), "scale"_a = nb::none())
        .def("add_child",
             [](PyArmature& self, nb::handle position, float radius, int parent) {
                 if (radius < 0.0f) throw std::invalid_argument("radius must be >= 0");
                 const int n = static_cast<int>(self.stroke.size());
                 if (parent < -1 || parent >= n)
                     throw std::invalid_argument("parent index out of range");
                 scene::StrokePoint p;
                 p.pos = to_f3(position, "armature node");
                 p.radius = radius;
                 self.stroke.push_back(p);
                 // -1 means "the node before this one", which is what a drag
                 // out from the last sphere does and the common case by far.
                 int chosen = parent >= 0 ? parent : (n > 0 ? n - 1 : 0);
                 self.armature_parents.push_back(static_cast<std::uint32_t>(chosen));
                 return &self;
             },
             "position"_a, "radius"_a, "parent"_a = -1, nb::rv_policy::reference_internal,
             "Append a node under `parent` (chainable). parent=-1 continues from "
             "the last node, which is what dragging a new sphere out does")
        .def_prop_ro("node_count",
                     [](const PyArmature& self) { return self.stroke.size(); })
        .def_prop_ro("parents",
                     [](const PyArmature& self) {
                         nb::list out;
                         for (std::uint32_t v : self.armature_parents) out.append(v);
                         return out;
                     })
        .def_prop_rw("signs",
                     [](const PyArmature& self) {
                         nb::list out;
                         for (std::size_t i = 0; i < self.stroke.size(); ++i)
                             out.append(int(i < self.armature_signs.size()
                                                ? self.armature_signs[i]
                                                : std::int8_t{1}));
                         return out;
                     },
                     [](PyArmature& self, nb::handle value) {
                         self.armature_signs = to_signs(value, self.stroke.size());
                     },
                     "+1 or -1 per node, positive by default — ZBrush's negative "
                     "ZSphere. The field is the armature of the positive nodes "
                     "minus the armature of the negative nodes, so skin along a "
                     "negative node's links is never drawn and a hollow is a "
                     "continuous scoop rather than a carved ball with a bridge "
                     "across its opening. Reads padded positive when set shorter "
                     "than the nodes, exactly as the document evaluates it.")
        .def_prop_ro("nodes", [](const PyArmature& self) {
            nb::list out;
            for (const scene::StrokePoint& p : self.stroke) {
                nb::list row;
                row.append(p.pos.x); row.append(p.pos.y); row.append(p.pos.z);
                row.append(p.radius);
                out.append(row);
            }
            return out;
        });

    nb::class_<PyExtrude, PyPrim>(m, "Extrude",
                                  "Exact extrusion of a profile along Z (half_depth)")
        .def("__init__",
             [](PyExtrude* self, const PyProfile& profile, float half_depth,
                nb::handle position, nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyExtrude();
                 if (half_depth <= 0.0f)
                     throw std::invalid_argument("half_depth must be > 0");
                 self->prim = scene::Prim::extrude(half_depth);
                 self->profile = profile.profile;
                 self->profile_points = profile.points;
                 place(*self, position, rotation_axis_angle, scale);
             },
             "profile"_a, "half_depth"_a, CLAY_PLACE_ARGS);

    nb::class_<PyRevolve, PyPrim>(m, "Revolve",
                                  "Exact revolution of a profile about Y at a given offset")
        .def("__init__",
             [](PyRevolve* self, const PyProfile& profile, float offset, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyRevolve();
                 self->prim = scene::Prim::revolve(offset);
                 self->profile = profile.profile;
                 self->profile_points = profile.points;
                 place(*self, position, rotation_axis_angle, scale);
             },
             "profile"_a, "offset"_a = 0.0f, CLAY_PLACE_ARGS);

    nb::class_<PyLoft, PyPrim>(
        m, "Loft",
        "Two or more profiles interpolated along Z, evenly spaced over the\n"
        "half-depth. Two is the classic case; more are bracketed, so a\n"
        "wide-narrow-wide list gives a waist rather than a straight taper.\n\n"
        "BOUND, not exact: a lerp of two distance fields is not a distance\n"
        "field. It also raises the Lipschitz factor — interpolating very\n"
        "different profiles over a shallow depth makes the field change fast\n"
        "along Z — so the document's safe step scale drops accordingly, which\n"
        "is what keeps the raymarcher from stepping through the surface.")
        .def("__init__",
             [](PyLoft* self, nb::sequence profiles, float half_depth, std::uint8_t ease,
                nb::handle position, nb::handle rotation_axis_angle, nb::handle scale) {
                 if (nb::len(profiles) < 2)
                     throw std::invalid_argument("a loft needs two or more profiles");
                 if (!(half_depth > 0.0f))
                     throw std::invalid_argument("half_depth must be > 0");
                 if (ease >= kernel::ease_count)
                     throw std::invalid_argument("unknown easing curve");
                 new (self) PyLoft();
                 self->prim = scene::Prim::loft(half_depth, ease);
                 for (std::size_t i = 0; i < nb::len(profiles); ++i) {
                     // The element is held in a named object first: binding a
                     // reference straight to profiles[i] binds it to the
                     // sequence proxy's temporary, which GCC rightly refuses.
                     nb::object element = profiles[i];
                     const PyProfile& p = nb::cast<const PyProfile&>(element);
                     self->profiles.push_back(p.profile);
                     self->profile_polygons.push_back(p.points);
                 }
                 place(*self, position, rotation_axis_angle, scale);
             },
             "profiles"_a, "half_depth"_a = 1.0f, "ease"_a = 0, CLAY_PLACE_ARGS);

    nb::class_<PySwept, PyPrim>(
        m, "Swept",
        "The same profiles as a Loft, carried along a GUIDE curve instead of\n"
        "the Z axis. The guide is an ordinary control-point curve — the same\n"
        "point types, handles and tolerance a Stroke takes — because a guide is\n"
        "not a new kind of curve.\n\n"
        "The frame is PARALLEL-TRANSPORTED along the guide when the item is\n"
        "compiled, not derived per sample: a Frenet frame flips at an\n"
        "inflection and is undefined where the guide is straight, which would\n"
        "twist the sweep exactly where it should be calmest.\n\n"
        "Profiles are distributed by ARC LENGTH, so a guide whose points bunch\n"
        "does not bunch the profiles. The ends are the profile itself — a flat\n"
        "cap, since a profile need not be a circle.\n\n"
        "BOUND, and its Lipschitz comes from curvature: a sweep compresses\n"
        "space on the inside of a bend by R / (R - r). A profile wider than the\n"
        "guide's tightest bend folds through itself; that is not refused, since\n"
        "a guide is editable afterwards, but the safe step scale collapses so\n"
        "the raymarcher crawls instead of stepping through the surface.")
        .def("__init__",
             [](PySwept* self, nb::handle guide, nb::sequence profiles, nb::handle types,
                float tolerance, std::uint8_t ease, nb::handle in_handles,
                nb::handle out_handles, nb::handle position, nb::handle rotation_axis_angle,
                nb::handle scale) {
                 if (nb::len(profiles) < 2)
                     throw std::invalid_argument("a sweep needs two or more profiles");
                 if (!(tolerance > 0.0f))
                     throw std::invalid_argument("tolerance must be > 0");
                 if (ease >= kernel::ease_count)
                     throw std::invalid_argument("unknown easing curve");
                 new (self) PySwept();
                 self->prim = scene::Prim::swept(ease);
                 self->stroke = to_stroke_points(guide);
                 if (self->stroke.size() < 2)
                     throw std::invalid_argument("a sweep needs a guide of two or more points");
                 apply_point_types(self->stroke, types);
                 apply_handles(self->stroke, in_handles, out_handles);
                 self->curve_tolerance = tolerance;
                 for (std::size_t i = 0; i < nb::len(profiles); ++i) {
                     nb::object element = profiles[i];
                     const PyProfile& p = nb::cast<const PyProfile&>(element);
                     self->profiles.push_back(p.profile);
                     self->profile_polygons.push_back(p.points);
                 }
                 place(*self, position, rotation_axis_angle, scale);
             },
             "guide"_a, "profiles"_a, "types"_a = nb::none(), "tolerance"_a = 0.01f,
             "ease"_a = 0, "in_handles"_a = nb::none(), "out_handles"_a = nb::none(),
             CLAY_PLACE_ARGS);
    nb::class_<PyVolume, PyPrim>(
        m, "Volume",
        "A field SAMPLED onto a sparse narrow-band grid, and then usable as an\n"
        "ordinary item: combined, subtracted, transformed, saved.\n\n"
        "Storage follows the SURFACE, not the region. Only bricks that straddle\n"
        "the band store samples; the rest record which side they are on, in one\n"
        "integer each. That is what makes the volume O(area) rather than\n"
        "O(volume), and it is why the whole thing rides in the tape's blob\n"
        "instead of needing a resource handle.\n\n"
        "BOUND, not exact, and the two halves of that are different:\n"
        "  * where the volume HAS samples, the value is a trilinear\n"
        "    interpolation. Interpolating a convex field OVERSHOOTS, so it is\n"
        "    accurate to the sampling but is not a lower bound there.\n"
        "  * where it has none, the value is a genuine lower bound, so the\n"
        "    raymarcher cannot overstep across the empty majority.\n"
        "The error inside the band shrinks with `cell`, so `cell` is a real\n"
        "control over accuracy rather than a hope. `band` should be at least a\n"
        "couple of cells; a thinner one is widened rather than obeyed.")
        .def_static(
            "from_document",
            [](nb::handle source, float cell, nb::handle band, nb::handle bounds,
               nb::handle position, nb::handle rotation_axis_angle, nb::handle scale) {
                if (!(cell > 0.0f)) throw std::invalid_argument("cell must be > 0");
                float width = band.is_none() ? cell * 3.0f : nb::cast<float>(band);
                if (!(width > 0.0f)) throw std::invalid_argument("band must be > 0");

                const scene::Document& src = nb::cast<PyDocument&>(source).doc->document;
                scene::Tape tape = scene::compile_document(src);
                if (tape.empty())
                    throw std::invalid_argument("cannot sample an empty document");

                math::Aabb region;
                if (bounds.is_none()) {
                    // Padded by the band: sampling exactly to the bounds would
                    // clip the band at the surface where it is needed most.
                    region = tape.bounds;
                    if (region.empty())
                        throw std::invalid_argument(
                            "the document has no bounds to sample; pass bounds=");
                    kernel::cfloat3 pad = kernel::cf3(width, width, width);
                    region = math::Aabb{region.min - pad, region.max + pad};
                } else {
                    region = to_aabb(bounds);
                }

                PyVolume out;
                out.prim = scene::Prim::volume();
                out.volume = std::make_shared<const field::FieldVolume>(
                    field::FieldVolume::sample_blocks(eval::tape_block_fill(tape), region, cell,
                                                      width));
                place(out, position, rotation_axis_angle, scale);
                return out;
            },
            "document"_a, "cell"_a = 0.05f, "band"_a = nb::none(), "bounds"_a = nb::none(),
            CLAY_PLACE_ARGS,
            "Sample a document's field. The default region is the document's\n"
            "bounds padded by the band.")
        .def_prop_ro("cell_size",
                     [](const PyVolume& v) { return v.volume ? v.volume->cell_size() : 0.0f; })
        .def_prop_ro("band", [](const PyVolume& v) { return v.volume ? v.volume->band() : 0.0f; })
        .def_prop_ro("brick_count",
                     [](const PyVolume& v) { return v.volume ? v.volume->brick_count() : 0u; })
        .def_prop_ro("sample_count",
                     [](const PyVolume& v) { return v.volume ? v.volume->sample_count() : 0u; })
        .def_prop_ro("megabytes",
                     [](const PyVolume& v) {
                         if (!v.volume) return 0.0;
                         return static_cast<double>(v.volume->blob_floats() * sizeof(float)) /
                                (1024.0 * 1024.0);
                     },
                     "What this volume costs in the tape's blob.")
        .def_prop_ro("bounds",
                     [](const PyVolume& v) {
                         math::Aabb b = v.volume ? v.volume->bounds() : math::Aabb();
                         return nb::make_tuple(nb::make_tuple(b.min.x, b.min.y, b.min.z),
                                               nb::make_tuple(b.max.x, b.max.y, b.max.z));
                     })
        .def("eval",
             [](const PyVolume& v, nb::handle points) {
                 PointsView pts = to_points(points);
                 const std::size_t n = pts.count;
                 float* out = new float[n ? n : 1];
                 nb::capsule owner(out, [](void* p) noexcept { delete[] static_cast<float*>(p); });
                 for (std::size_t i = 0; i < n; ++i)
                     out[i] = v.volume ? v.volume->eval(kernel::cf3(pts.data[i * 3],
                                                                    pts.data[i * 3 + 1],
                                                                    pts.data[i * 3 + 2]))
                                       : 0.0f;
                 return nb::cast(nb::ndarray<nb::numpy, float>(out, {n}, owner));
             },
             "points"_a,
             "The sampled field itself, before it is placed in a document —\n"
             "so a test can tell a sampling error from a placement one.")
        .def_static(
            "from_mesh",
            [](const PyMesh& mesh, float cell, nb::handle band, float beta, nb::handle position,
               nb::handle rotation_axis_angle, nb::handle scale) {
                if (mesh.data().triangle_count() == 0)
                    throw std::invalid_argument("cannot sample a mesh with no triangles");
                if (cell < 0.0f) throw std::invalid_argument("cell must be >= 0");
                if (!(beta >= 0.0f)) throw std::invalid_argument("beta must be >= 0");
                mesh::ImportSettings settings;
                settings.cell_size = cell;
                settings.band = band.is_none() ? 0.0f : nb::cast<float>(band);
                settings.beta = beta;

                std::optional<field::FieldVolume> volume;
                {
                    nb::gil_scoped_release release;  // the BVH build is the slow part
                    volume = mesh::to_field(mesh.data(), settings);
                }
                if (!volume) throw std::invalid_argument("the mesh could not be sampled");

                PyVolume out;
                out.prim = scene::Prim::volume();
                out.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
                place(out, position, rotation_axis_angle, scale);
                return out;
            },
            "mesh"_a, "cell"_a = 0.0f, "band"_a = nb::none(), "beta"_a = 2.0f, CLAY_PLACE_ARGS,
            "Sample a mesh into a field, which is what makes an imported model\n"
            "something you can WORK on rather than only display.\n\n"
            "The sign comes from the GENERALIZED WINDING NUMBER, not a ray cast\n"
            "or the nearest triangle's normal. That matters because real assets\n"
            "are not watertight: a single hole flips a ray-parity test for a\n"
            "whole half-space, and the nearest triangle to a point inside a model\n"
            "may face away when the wall it should have hit is missing. A winding\n"
            "number degrades continuously instead, passing smoothly through a\n"
            "half across an opening.\n\n"
            "`cell` defaults to a fraction of the mesh's longest side, since a\n"
            "default in world units would be far too fine for a building and far\n"
            "too coarse for a bolt. `beta` is how far a BVH node must be before\n"
            "it is summarized by one term rather than descended: larger is more\n"
            "accurate and slower, and 0 sums every triangle exactly.")
        .def_static(
            "from_voxels",
            [](const PyVoxelGrid& grid, int blur, int index, nb::handle band,
               nb::handle position, nb::handle rotation_axis_angle, nb::handle scale) {
                if (blur < 0 || blur > 8) throw std::invalid_argument("blur must be 0..8");
                if (index < 0 || index > 255) throw std::invalid_argument("index must be 0..255");
                voxel::VoxelGrid::FieldOptions options{
                    blur, band.is_none() ? 0.0f : nb::cast<float>(band),
                    static_cast<std::uint8_t>(index)};
                std::optional<field::FieldVolume> volume;
                {
                    nb::gil_scoped_release release;
                    volume = grid.grid().to_field(options);
                }
                if (!volume)
                    throw std::invalid_argument(
                        "the grid holds nothing to convert at that index or level");

                PyVolume out;
                out.prim = scene::Prim::volume();
                out.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
                place(out, position, rotation_axis_angle, scale);
                return out;
            },
            "grid"_a, "blur"_a = 0, "index"_a = 0, "band"_a = nb::none(), CLAY_PLACE_ARGS,
            "A voxel sculpt back as a FIELD, so it is an operand again rather\n"
            "than something you can only display or export.\n\n"
            "Direct, without a mesh in between. The grid knows where its surface\n"
            "is; it does not know the DISTANCE to it. Occupancy is read by\n"
            "trilinear interpolation between cell CENTRES — which is what puts\n"
            "the isosurface on a surface rather than on a cell boundary — and\n"
            "the result is redistanced, so it carries a Lipschitz bound a\n"
            "marcher and a blend can trust.\n\n"
            "`index` converts ONE palette entry, which is how colour survives a\n"
            "trip a single field has nowhere to store it on: convert once per\n"
            "entry and place each with that entry's colour. The union of the\n"
            "parts is the whole solid. 0 converts every occupied cell.\n\n"
            "`blur` smooths the occupancy first: 0 keeps thin features, 1 is\n"
            "smoother and can erase an isolated voxel.\n\n"
            "LOSSY IN BOTH DIRECTIONS. Going to voxels quantised to the lattice\n"
            "and nothing here recovers a boolean's sharp edge; coming back turns\n"
            "occupancy into a distance. Preserved: the surface within about a\n"
            "cell, and the colour. Not preserved: exactness, and the procedural\n"
            "history — the items are gone and their parameters with them.")
        .def(
            "relaxed",
            [](const PyVolume& self, float strength, int radius_cells, int iterations,
               nb::handle centre, float region_radius, float falloff, nb::handle mask) {
                if (!self.volume) throw std::invalid_argument("nothing to relax");
                if (!(strength >= 0.0f && strength <= 1.0f))
                    throw std::invalid_argument("strength must be between 0 and 1");
                if (radius_cells < 1) throw std::invalid_argument("radius_cells must be >= 1");
                field::RelaxSettings settings;
                settings.strength = strength;
                settings.radius_cells = radius_cells;
                settings.iterations = iterations;
                if (!centre.is_none()) settings.centre = to_f3(centre, "centre");
                settings.region_radius = region_radius;
                settings.falloff = falloff;
                settings.mask = mask_gate_of(mask);

                PyVolume out;
                out.prim = self.prim;
                out.xform = self.xform;
                {
                    nb::gil_scoped_release release;
                    out.volume = std::make_shared<const field::FieldVolume>(
                        field::relax(*self.volume, settings));
                }
                return out;
            },
            "strength"_a = 1.0f, "radius_cells"_a = 1, "iterations"_a = 1,
            "centre"_a = nb::none(), "region_radius"_a = 0.0f, "falloff"_a = 0.0f,
            "mask"_a = nb::none(),
            "Smooth this volume, returning a new one. The last of the core\n"
            "sculpting brushes: voxels had smoothing, SDF layers had none.\n\n"
            "RELAX BAKES, and that is worth knowing before you pick a cell\n"
            "size. What comes back is a sampled volume, not the edit list that\n"
            "went in — the items are gone, and with them the ability to go back\n"
            "and change a radius. That is inherent to smoothing a field rather\n"
            "than a shortcut: a general relax has to be able to smooth a bump in\n"
            "the middle of ONE item, which no reweighting of an edit list can\n"
            "express. The resolution the shape now has is the one you chose.\n\n"
            "Smoothing destroys EXACTNESS — the result no longer reports the\n"
            "true distance to its own surface — but it cannot break the\n"
            "Lipschitz bound, because an average cannot vary faster than the\n"
            "thing it averages. And a field whose slope is bounded by one is\n"
            "automatically a conservative bound on the distance to its own zero\n"
            "set, so the raymarcher stays correct.\n\n"
            "`region_radius` of 0 relaxes everywhere, which is a filter; give it\n"
            "a centre and a radius and it is a brush. The falloff is widened if\n"
            "it is too narrow to hide the seam the kernel makes.\n\n"
            "`mask` freezes: a fully masked sample keeps its value verbatim, so\n"
            "the region a mask covers is left exactly as it was rather than\n"
            "nearly so. Sampled in world units, which is what lets one mask\n"
            "freeze a voxel layer and an SDF layer at the same time.")
        .def_static(
            "moved_topologically_from",
            [](nb::handle source, nb::handle anchor, nb::handle displacement, float radius,
               int ease, float cell, nb::handle band, nb::handle bounds, nb::handle position,
               nb::handle rotation_axis_angle, nb::handle scale) {
                if (!(cell > 0.0f)) throw std::invalid_argument("cell must be > 0");
                if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
                field::TopologicalMoveSettings settings;
                settings.anchor = to_f3(anchor, "anchor");
                settings.displacement = to_f3(displacement, "displacement");
                settings.radius = radius;
                settings.ease = static_cast<std::uint8_t>(ease);

                const scene::Document& src = nb::cast<PyDocument&>(source).doc->document;
                scene::Tape tape = scene::compile_document(src);
                if (tape.empty())
                    throw std::invalid_argument("cannot move an empty document");
                const float width = band.is_none() ? cell * 3.0f : nb::cast<float>(band);

                math::Aabb where;
                if (bounds.is_none()) {
                    where = tape.bounds;
                    if (where.empty())
                        throw std::invalid_argument("the document has no bounds; pass bounds=");
                    const float pad = width + radius + kernel::clength(settings.displacement);
                    kernel::cfloat3 p3 = kernel::cf3(pad, pad, pad);
                    where = math::Aabb{where.min - p3, where.max + p3};
                } else {
                    where = to_aabb(bounds);
                }

                PyVolume out;
                out.prim = scene::Prim::volume();
                {
                    nb::gil_scoped_release release;
                    out.volume = std::make_shared<const field::FieldVolume>(
                        field::move_topological(eval::tape_point_batch(tape), where, cell, width,
                                                settings));
                }
                place(out, position, rotation_axis_angle, scale);
                return out;
            },
            "document"_a, "anchor"_a, "displacement"_a, "radius"_a = 0.3f, "ease"_a = 0,
            "cell"_a = 0.02f, "band"_a = nb::none(), "bounds"_a = nb::none(),
            CLAY_PLACE_ARGS,
            "ZBrush's Move Topological: a drag whose falloff is weighted by\n"
            "distance ALONG THE MATERIAL rather than through space.\n\n"
            "`radius` is therefore a distance of travel across the surface, not a\n"
            "straight line — so it cannot step over a gap however narrow. On two\n"
            "fingers 0.32 apart joined only through a palm, a Euclidean drag at\n"
            "radius 0.5 pulls the neighbour; this does not, because along the\n"
            "material the neighbour is about 1.5 away.\n\n"
            "It BAKES, for the reason relax and flatten do: the weight is a solved\n"
            "grid rather than a closed form. Reach for `Layer.move_surface` when\n"
            "the form has no parts close in space and far along the surface — it\n"
            "is cheaper, and it does not bake.")
        .def_static(
            "flattened_from",
            [](nb::handle source, nb::handle plane_point, nb::handle plane_normal, float cell,
               nb::handle band, nb::handle bounds, float strength, nb::handle centre,
               float region_radius, float falloff, const std::string& mode,
               nb::handle position, nb::handle rotation_axis_angle, nb::handle scale,
               nb::handle mask) {
                if (!(cell > 0.0f)) throw std::invalid_argument("cell must be > 0");
                if (!(strength >= 0.0f && strength <= 1.0f))
                    throw std::invalid_argument("strength must be between 0 and 1");
                field::FlattenSettings settings;
                settings.plane_point = to_f3(plane_point, "plane_point");
                settings.plane_normal = to_f3(plane_normal, "plane_normal");
                if (!(kernel::clength(settings.plane_normal) > 1e-6f))
                    throw std::invalid_argument("plane_normal must not be zero length");
                settings.strength = strength;
                settings.mode = parse_flatten_mode(mode);
                if (!centre.is_none()) settings.centre = to_f3(centre, "centre");
                if (!(region_radius > 0.0f))
                    throw std::invalid_argument(
                        "region_radius must be > 0: flatten is local, and with no region it "
                        "replaces the shape with a half-space rather than flattening it");
                settings.region_radius = region_radius;
                settings.falloff = falloff;
                settings.mask = mask_gate_of(mask);

                const scene::Document& src = nb::cast<PyDocument&>(source).doc->document;
                scene::Tape tape = scene::compile_document(src);
                if (tape.empty())
                    throw std::invalid_argument("cannot flatten an empty document");
                const float width = band.is_none() ? cell * 3.0f : nb::cast<float>(band);

                math::Aabb where;
                if (bounds.is_none()) {
                    where = tape.bounds;
                    if (where.empty())
                        throw std::invalid_argument("the document has no bounds; pass bounds=");
                    kernel::cfloat3 pad = kernel::cf3(width, width, width);
                    where = math::Aabb{where.min - pad, where.max + pad};
                } else {
                    where = to_aabb(bounds);
                }

                PyVolume out;
                out.prim = scene::Prim::volume();
                {
                    nb::gil_scoped_release release;
                    out.volume = std::make_shared<const field::FieldVolume>(field::flatten(
                        eval::tape_block_fill(tape), where, cell, width, settings));
                }
                place(out, position, rotation_axis_angle, scale);
                return out;
            },
            "document"_a, "plane_point"_a, "plane_normal"_a, "cell"_a = 0.05f,
            "band"_a = nb::none(), "bounds"_a = nb::none(), "strength"_a = 1.0f,
            "centre"_a = nb::none(), "region_radius"_a = 0.0f, "falloff"_a = 0.0f,
            "mode"_a = "two_sided", CLAY_PLACE_ARGS, "mask"_a = nb::none(),
            "Sample a document with a flatten applied, in one pass — the verb SDF\n"
            "layers were missing, since voxels have had sculpt_flatten all along.\n\n"
            "TWO-SIDED by default, matching the voxel verb: material on the\n"
            "normal's side goes AND hollows on the other side fill. It is not a\n"
            "subtract, and\n"
            "\n"
            "`mode` picks which side it acts on: 'two_sided' (ZBrush Flatten),\n"
            "'cut' (only removes — hPolish, Planar, the Trim family, where\n"
            "cutting WITHOUT filling is what leaves a crisp facet against\n"
            "untouched surface) or 'fill' (only deposits, which fills a scanned\n"
            "hole flat without touching the surface around it).\n"
            "it is not ZBrush's Clip — as a solid, Clip is exactly Trim, which\n"
            "Cut already does.\n\n"
            "This SAMPLES rather than editing an existing volume, and the\n"
            "difference matters. Flatten moves the surface by many band widths,\n"
            "and a band cannot follow one that walks out of it — there would be\n"
            "no samples left where the surface ended up. Sampling builds the band\n"
            "around the flattened surface instead. Sampling from the DOCUMENT\n"
            "rather than from a volume also keeps the source exact: a volume\n"
            "reports a bound rather than a distance outside its own band.\n\n"
            "A region blends under a weight that varies across it, which can make\n"
            "the field steeper than a plain sampling — so the result declares the\n"
            "Lipschitz its samples actually have, measured, and the document's\n"
            "safe step scale drops to match. FLATTEN BAKES, as relax does.\n\n"
            "`mask` freezes: where it is one the source comes back untouched, so\n"
            "the surface there stays where the source put it rather than being\n"
            "drawn onto the plane.")
        .def(
            "flattened",
            [](const PyVolume& self, nb::handle plane_point, nb::handle plane_normal,
               float strength, nb::handle centre, float region_radius, float falloff,
               nb::handle mask) {
                if (!self.volume) throw std::invalid_argument("nothing to flatten");
                if (!(strength >= 0.0f && strength <= 1.0f))
                    throw std::invalid_argument("strength must be between 0 and 1");
                field::FlattenSettings settings;
                settings.plane_point = to_f3(plane_point, "plane_point");
                settings.plane_normal = to_f3(plane_normal, "plane_normal");
                if (!(kernel::clength(settings.plane_normal) > 1e-6f))
                    throw std::invalid_argument("plane_normal must not be zero length");
                settings.strength = strength;
                if (!centre.is_none()) settings.centre = to_f3(centre, "centre");
                if (!(region_radius > 0.0f))
                    throw std::invalid_argument(
                        "region_radius must be > 0: flatten is local, and with no region it "
                        "replaces the shape with a half-space rather than flattening it");
                settings.region_radius = region_radius;
                settings.falloff = falloff;
                settings.mask = mask_gate_of(mask);

                PyVolume out;
                out.prim = self.prim;
                out.xform = self.xform;
                {
                    nb::gil_scoped_release release;
                    out.volume = std::make_shared<const field::FieldVolume>(
                        field::flatten(*self.volume, settings));
                }
                return out;
            },
            "plane_point"_a, "plane_normal"_a, "strength"_a = 1.0f, "centre"_a = nb::none(),
            "region_radius"_a = 0.0f, "falloff"_a = 0.0f, "mask"_a = nb::none(),
            "Pull the surface onto a plane, returning a new volume. The verb SDF\n"
            "layers were missing: voxels have had sculpt_flatten all along.\n\n"
            "TWO-SIDED, matching the voxel verb: material on the normal's side\n"
            "goes AND hollows on the other side fill. It is not a subtract.\n\n"
            "The volume is RE-SAMPLED with the flatten applied, so the new band\n"
            "brackets the flattened surface rather than the original one — a\n"
            "band cannot follow a surface that walks out of it. Accurate while\n"
            "the surface stays near the band it came from; past that a volume\n"
            "reports a bound rather than a distance. Where a document exists,\n"
            "sample from that instead.\n\n"
            "This is NOT ZBrush's Clip. Clip moves vertices onto the plane rather\n"
            "than deleting them, and the solid that bounds is exactly Trim — the\n"
            "projected part lies ON the plane and has no volume. Clip's look is a\n"
            "zero-thickness fin, which a field cannot represent. Use a Cut for\n"
            "that; this is the brush that leaves surrounding form intact.\n\n"
            "A region blends under a weight that varies across it, which CAN make\n"
            "the field steeper than a plain volume — so unlike relax, the result\n"
            "declares a raised Lipschitz and the document's safe step scale drops.\n"
            "The plane is yours to supply: no camera and no picking enters the\n"
            "engine, the same rule Cut follows. FLATTEN BAKES, as relax does.\n\n"
            "`mask` freezes, exactly as it does on relax.")
        .def_prop_ro("sample_lipschitz",
                     [](const PyVolume& v) {
                         return v.volume ? v.volume->sample_lipschitz() : 1.0f;
                     },
                     "How fast the stored samples may vary: 1 for a sampled\n"
                     "distance field, more for one a region-limited brush has\n"
                     "steepened. The document's step scale follows it.")
        .def("has_samples_at",
             [](const PyVolume& v, nb::handle point) {
                 return v.volume && v.volume->has_samples_at(to_f3(point, "point"));
             },
             "point"_a,
             "Whether this point lands in a brick that stores samples. Not the\n"
             "same as being within the band: a brick is kept whole, so a stored\n"
             "brick holds samples well beyond it.");

#undef CLAY_PLACE_ARGS

    nb::class_<cut::CutShape>(
        m, "CutShape",
        "The outline of a cut, in world units on the frame it was drawn on.")
        .def_static("rect",
                    [](float half_width, float half_height) {
                        return cut::CutShape::rect(half_width, half_height);
                    },
                    "half_width"_a, "half_height"_a)
        .def_static("circle", [](float radius) { return cut::CutShape::circle(radius); },
                    "radius"_a)
        .def_static("polygon",
                    [](nb::handle vertices) {
                        return cut::CutShape::from_polygon(to_polygon(vertices));
                    },
                    "vertices"_a, "An (N, 2) outline; it closes implicitly")
        .def_static("trim",
                    [](nb::handle points, const std::string& side, nb::handle extent,
                       nb::handle types, float tolerance) {
                        std::vector<scene::StrokePoint> control = to_stroke_points(points);
                        apply_point_types(control, types);
                        cut::CutShape::Side which;
                        if (side == "below") which = cut::CutShape::Side::Below;
                        else if (side == "above") which = cut::CutShape::Side::Above;
                        else if (side == "left") which = cut::CutShape::Side::Left;
                        else if (side == "right") which = cut::CutShape::Side::Right;
                        else
                            throw std::invalid_argument(
                                "side must be 'below', 'above', 'left' or 'right', got '" +
                                side + "'");
                        kernel::cfloat3 e = to_f3(extent, "extent");
                        cut::CutShape shape = cut::CutShape::from_open_curve(
                            control, which, kernel::cf2(e.x, e.y), tolerance);
                        if (shape.polygon.empty())
                            throw std::invalid_argument(
                                "a trim needs at least two control points");
                        return shape;
                    },
                    "points"_a, "side"_a = "below", "extent"_a = nb::make_tuple(4.0f, 4.0f, 0.0f),
                    "types"_a = "spline", "tolerance"_a = 0.01f,
                    "ZBrush's Trim Curve: an OPEN stroke drawn across the form,\n"
                    "closed against the frame's bounds on the side it covers.\n\n"
                    "NOT `curve`, which tessellates CLOSED and is a spline lasso —\n"
                    "joining a trim stroke's endpoints cuts a sliver between them\n"
                    "instead of dividing the frame. Different shapes from the same\n"
                    "points, so different constructors.\n\n"
                    "`side` names which half the outline COVERS; the op still\n"
                    "decides its fate, as it does for every cut: SUBTRACT removes\n"
                    "that half, INTERSECT keeps only it. `extent` is how far the\n"
                    "closing edge reaches, in the frame's units.")
        .def_static("curve",
                    [](nb::handle points, nb::handle types, float tolerance) {
                        std::vector<scene::StrokePoint> control = to_stroke_points(points);
                        apply_point_types(control, types);
                        return cut::CutShape::from_curve(control, tolerance);
                    },
                    "points"_a, "types"_a = "spline", "tolerance"_a = 0.01f,
                    "A closed control-point curve drawn in the cut plane, flattened "
                    "through the same tessellator curves use — so a spline lasso "
                    "follows the same curve a spline item would.")
        .def_prop_ro("vertex_count",
                     [](const cut::CutShape& s) { return s.polygon.size(); });

    nb::class_<PyCut, PyPrim>(
        m, "Cut",
        "A shape drawn over the model, resolved into an ordinary edit item.\n\n"
        "Give the frame the shape was drawn on — an origin and an orthonormal\n"
        "basis, which a viewport already has because it needed one to draw the\n"
        "overlay — and the shape in WORLD units on that frame. Not pixels and\n"
        "not normalized device coordinates: the engine has no viewport.\n\n"
        "The cut is a PRISM, not a frustum. A shape drawn under a perspective\n"
        "camera sweeps a converging wedge, and cutting with one would give a\n"
        "cut face that is not flat and a solid that depends on where the camera\n"
        "stood. A trim is a straight cut, as it is in ZBrush and 3DCoat.\n\n"
        "Which side survives is the OP you place this with: SUBTRACT removes\n"
        "what the shape covers, INTERSECT keeps only that.\n\n"
        "The sweep is sized to `region` — the document's own bounds by default —\n"
        "so a cut goes all the way through. Passing near/far explicitly is how a\n"
        "deliberate partial cut is expressed.")
        .def("__init__",
             [](PyCut* self, nb::handle origin, nb::handle right, nb::handle up,
                nb::handle forward, nb::handle shape, nb::handle region, float rounding,
                nb::handle near_extent, nb::handle far_extent) {
                 cut::CutFrame frame;
                 frame.origin = to_f3(origin, "origin");
                 frame.right = to_f3(right, "right");
                 frame.up = to_f3(up, "up");
                 frame.forward = to_f3(forward, "forward");
                 if (!frame.is_orthonormal())
                     throw std::invalid_argument(
                         "right, up and forward must be orthonormal — the shape was drawn in "
                         "a frame, and silently squaring it up would cut somewhere else");

                 cut::CutOptions options;
                 options.rounding = rounding;
                 if (!near_extent.is_none()) options.near_extent = nb::cast<float>(near_extent);
                 if (!far_extent.is_none()) options.far_extent = nb::cast<float>(far_extent);

                 math::Aabb box = to_aabb(region);
                 std::optional<scene::Node> item =
                     cut::cut_item(frame, nb::cast<cut::CutShape&>(shape), box, options);
                 if (!item)
                     throw std::invalid_argument("the cut is degenerate: a shape with no area");
                 new (self) PyCut();
                 self->prim = item->prim;
                 self->profile = item->profile;
                 self->profile_points = item->profile_points;
                 self->xform = item->xform;
                 self->rounding = item->rounding;
             },
             "origin"_a, "right"_a, "up"_a, "forward"_a, "shape"_a, "region"_a,
             "rounding"_a = 0.0f, "near"_a = nb::none(), "far"_a = nb::none());

    nb::class_<PyStroke, PyPrim>(
        m, "Stroke",
        "A chain of sphere-swept cones with per-point radius — ONE edit item,\n"
        "not one per segment.\n\n"
        "This is also the curve object. Each point carries a type saying how it\n"
        "joins the next: 'hard' (a straight segment, the default and what a\n"
        "finger drag produces), 'spline' (Catmull-Rom, through the points),\n"
        "'bspline' (approximating, so it rounds corners off) or 'bezier'\n"
        "(shaped by in_handles/out_handles). A stroke is a curve whose points\n"
        "are all hard corners, so an all-hard chain means exactly what it\n"
        "always did.\n\n"
        "Typed points are tessellated into the same segment chain at compile\n"
        "time, to `tolerance` — the largest distance a span's midpoint may sit\n"
        "from its chord. Tolerance is a property of the document, not of the\n"
        "viewer: two builds have to agree on what a document means.\n\n"
        "Handles are in the item's LOCAL space, relative to their point.")
        .def("__init__",
             [](PyStroke* self, nb::handle points, float blend_k, nb::handle types,
                bool closed, float tolerance, nb::handle in_handles, nb::handle out_handles,
                nb::handle position, nb::handle rotation_axis_angle, nb::handle scale) {
                 new (self) PyStroke();
                 self->prim = scene::Prim::stroke();
                 if (!points.is_none()) self->stroke = to_stroke_points(points);
                 apply_point_types(self->stroke, types);
                 apply_handles(self->stroke, in_handles, out_handles);
                 if (blend_k < 0.0f) throw std::invalid_argument("blend_k must be >= 0");
                 if (!(tolerance > 0.0f)) throw std::invalid_argument("tolerance must be > 0");
                 self->stroke_blend_k = blend_k;
                 self->stroke_closed = closed;
                 self->curve_tolerance = tolerance;
                 place(*self, position, rotation_axis_angle, scale);
             },
             "points"_a = nb::none(), "blend_k"_a = 0.0f, "types"_a = nb::none(),
             "closed"_a = false, "tolerance"_a = 0.01f, "in_handles"_a = nb::none(),
             "out_handles"_a = nb::none(), "position"_a = nb::none(),
             "rotation_axis_angle"_a = nb::none(), "scale"_a = nb::none())
        .def("add_point",
             [](PyStroke& self, nb::handle position, float radius, const std::string& type,
                nb::handle in_handle, nb::handle out_handle) {
                 if (radius < 0.0f) throw std::invalid_argument("radius must be >= 0");
                 scene::StrokePoint p;
                 p.pos = to_f3(position, "stroke point");
                 p.radius = radius;
                 p.type = parse_point_type(type);
                 if (!in_handle.is_none()) p.in_handle = to_f3(in_handle, "in_handle");
                 if (!out_handle.is_none()) p.out_handle = to_f3(out_handle, "out_handle");
                 self.stroke.push_back(p);
                 return &self;
             },
             "position"_a, "radius"_a, "type"_a = "hard", "in_handle"_a = nb::none(),
             "out_handle"_a = nb::none(), nb::rv_policy::reference_internal,
             "Append one point (chainable) — the incremental authoring path")
        .def_prop_ro("closed", [](const PyStroke& self) { return self.stroke_closed; })
        .def_prop_ro("tolerance", [](const PyStroke& self) { return self.curve_tolerance; })
        .def_prop_ro("types",
                     [](const PyStroke& self) {
                         nb::list out;
                         for (const scene::StrokePoint& p : self.stroke) {
                             switch (p.type) {
                                 case scene::StrokePointType::Spline: out.append("spline"); break;
                                 case scene::StrokePointType::BSpline: out.append("bspline"); break;
                                 case scene::StrokePointType::Bezier: out.append("bezier"); break;
                                 default: out.append("hard"); break;
                             }
                         }
                         return out;
                     })
        .def_prop_ro("point_count",
                     [](const PyStroke& self) { return self.stroke.size(); })
        .def_prop_ro("points", [](const PyStroke& self) {
            nb::module_ np = nb::module_::import_("numpy");
            nb::object arr = np.attr("empty")(nb::make_tuple(self.stroke.size(), 4),
                                              "dtype"_a = "float32");
            auto view = nb::cast<nb::ndarray<float, nb::ndim<2>, nb::c_contig>>(arr);
            for (std::size_t i = 0; i < self.stroke.size(); ++i) {
                float* dst = view.data() + i * 4;
                dst[0] = self.stroke[i].pos.x;
                dst[1] = self.stroke[i].pos.y;
                dst[2] = self.stroke[i].pos.z;
                dst[3] = self.stroke[i].radius;
            }
            return arr;
        });

    m.def("tube",
          [](nb::handle path, const std::string& point_type, float radius, nb::handle radius_mid,
             nb::handle radius_end, bool closed, nb::handle profile, float tolerance,
             float blend_k) -> nb::object {
              PointsView pts = to_points(path);
              std::vector<kernel::cfloat3> points;
              points.reserve(pts.count);
              for (std::size_t i = 0; i < pts.count; ++i)
                  points.push_back(kernel::cf3(pts.data[i * 3], pts.data[i * 3 + 1],
                                               pts.data[i * 3 + 2]));

              brush::TubeSettings settings;
              settings.point_type = parse_point_type(point_type);
              settings.radius_start = radius;
              settings.radius_mid = radius_mid.is_none() ? radius : nb::cast<float>(radius_mid);
              settings.radius_end = radius_end.is_none() ? radius : nb::cast<float>(radius_end);
              settings.closed = closed;
              settings.tolerance = tolerance;
              settings.blend_k = blend_k;

              if (!profile.is_none()) {
                  // Profiles cross as PyProfile, which carries its polygon points
                  // alongside the engine's Profile — a polygon profile is the two
                  // together, so unwrapping only the first would lose the shape.
                  std::vector<scene::Profile> profiles;
                  std::vector<std::vector<kernel::cfloat2>> polygons;
                  auto take = [&](nb::handle h) {
                      const PyProfile& p = nb::cast<const PyProfile&>(h);
                      profiles.push_back(p.profile);
                      polygons.push_back(p.points);
                  };
                  if (nb::isinstance<PyProfile>(profile)) take(profile);
                  else
                      for (nb::handle h : nb::cast<nb::sequence>(profile)) take(h);
                  std::optional<scene::Node> node =
                      brush::tube_with_profile(points, profiles, settings);
                  if (!node)
                      throw std::invalid_argument(
                          "a profiled tube needs at least two points and a profile");
                  PySwept out;
                  out.prim = node->prim;
                  out.stroke = node->stroke;
                  out.stroke_closed = node->stroke_closed;
                  out.curve_tolerance = node->curve_tolerance;
                  out.profiles = node->profiles;
                  out.profile_polygons = node->profile_polygons;
                  // The resolver duplicates a lone profile so a sweep has two to
                  // interpolate between; the polygons have to follow it.
                  while (polygons.size() < out.profiles.size())
                      polygons.push_back(polygons.empty() ? std::vector<kernel::cfloat2>{}
                                                          : polygons.back());
                  out.profile_polygons = polygons;
                  return nb::cast(out);
              }

              std::optional<scene::Node> node = brush::tube(points, settings);
              if (!node)
                  throw std::invalid_argument(
                      "a tube needs at least two points and a radius > 0 somewhere");
              PyStroke out;
              out.prim = node->prim;
              out.stroke = node->stroke;
              out.stroke_closed = node->stroke_closed;
              out.stroke_blend_k = node->stroke_blend_k;
              out.curve_tolerance = node->curve_tolerance;
              return nb::cast(out);
          },
          "path"_a, "point_type"_a = "bspline", "radius"_a = 0.08f,
          "radius_mid"_a = nb::none(), "radius_end"_a = nb::none(), "closed"_a = false,
          "profile"_a = nb::none(), "tolerance"_a = 0.01f, "blend_k"_a = 0.0f,
          "Nomad Sculpt's Tubes: a drawn path becomes a rope, pipe, tentacle or\n"
          "hair strand along it.\n\n"
          "`point_type` is the smooth/sharp toggle — 'hard' turns at each control\n"
          "point, 'bspline' rounds through them — because a tube's path is the\n"
          "same kind of curve every other item takes, not a new one.\n\n"
          "`radius`, `radius_mid` and `radius_end` are Nomad's three handles, and\n"
          "are interpolated by ARC LENGTH so a path whose points bunch does not\n"
          "bunch the taper. Omitting the last two gives a uniform tube.\n\n"
          "With no `profile` this is a swept SPHERE, which is an exact distance\n"
          "field: the safe step scale stays 1. A `profile` (or a list of them)\n"
          "makes it a swept item instead — a square or custom cross-section, at\n"
          "the cost of a bound field and a step scale below 1. That choice is the\n"
          "profile itself rather than a separate flag.");

    m.def("snakehook",
          [](nb::handle anchor, nb::handle inward, nb::handle path, float base_radius,
             float tip_fraction, float min_tip_radius, float taper_curve, float tolerance) {
              brush::SnakehookSettings settings;
              settings.base_radius = base_radius;
              settings.tip_fraction = tip_fraction;
              settings.min_tip_radius = min_tip_radius;
              settings.taper_curve = taper_curve;
              settings.tolerance = tolerance;

              PointsView pts = to_points(path);
              std::vector<kernel::cfloat3> drag;
              drag.reserve(pts.count);
              for (std::size_t i = 0; i < pts.count; ++i)
                  drag.push_back(kernel::cf3(pts.data[i * 3], pts.data[i * 3 + 1],
                                             pts.data[i * 3 + 2]));

              std::optional<scene::Node> node =
                  brush::snakehook(to_f3(anchor, "anchor"), to_f3(inward, "inward"), drag,
                                   settings);
              if (!node)
                  throw std::invalid_argument(
                      "a snakehook needs a drag with points, a non-zero inward normal and a "
                      "base radius > 0");

              PyStroke out;
              out.prim = node->prim;
              out.stroke = node->stroke;
              out.curve_tolerance = node->curve_tolerance;
              return out;
          },
          "anchor"_a, "inward"_a, "path"_a, "base_radius"_a = 0.2f, "tip_fraction"_a = 0.15f,
          "min_tip_radius"_a = 0.01f, "taper_curve"_a = 1.0f, "tolerance"_a = 0.01f,
          "Pull a horn, a tendril or a spike out of a form: the brush that makes\n"
          "a sphere into a creature. Returns an ordinary Stroke, so undo,\n"
          "coalescing, saving, picking and masks all apply to it unchanged.\n\n"
          "NOT a new kind of geometry. The stroke opcode already sweeps a sphere\n"
          "along a chain with a radius per point, and that IS a tendril once the\n"
          "radii taper — the field even stays EXACT, so unlike a loft or a sweep\n"
          "a tendril costs the raymarcher nothing. What this adds is the step\n"
          "that turns a drag into that item, so a tendril does not detach from\n"
          "the surface or bead along its length.\n\n"
          "It ADDS material rather than moving it. ZBrush pulls existing surface,\n"
          "so the body dimples slightly where the tendril came from; this grows a\n"
          "tendril and leaves the body alone. The difference shows only at the\n"
          "base.\n\n"
          "`anchor` is the surface point the drag started from and `inward` points\n"
          "into the body — you have both from the pick you already did, and no\n"
          "camera enters here. The anchor is PREPENDED to the path, so the\n"
          "tendril begins where the user touched rather than where the first drag\n"
          "sample landed a frame later. The taper follows ARC LENGTH, so how fast the\n"
          "gesture was does not decide how thick the tendril is. `taper_curve`\n"
          "above 1 thins away quickly (a whip); below 1 holds the thickness and\n"
          "then drops (a horn).");

    m.def("load_mesh",
          [](const std::string& path, std::size_t max_vertices, std::size_t max_triangles) {
              io::ImportBudget limits;
              // 0 means the library's default rather than "allow nothing".
              if (max_vertices) limits.max_vertices = max_vertices;
              if (max_triangles) limits.max_triangles = max_triangles;
              PyMesh out;
              {
                  nb::gil_scoped_release release;
                  out.m = load_mesh_any(path, limits);
              }
              return out;
          },
          "path"_a, "max_vertices"_a = 0, "max_triangles"_a = 0,
          "Load a mesh by extension: .obj, .ply, .fbx or .glb, matched\n"
          "case-insensitively. The counterpart to Mesh.save, and what gives\n"
          "Volume.from_mesh something to sample. (.gltf is not read: its\n"
          "buffers live in separate files beside it, which would mean\n"
          "reading files the caller never handed us.)\n\n"
          "The budget is checked against the file's DECLARED counts before\n"
          "anything is allocated, which is the point: a malformed or hostile\n"
          "file can claim a billion triangles. 0 means the library's default.");

    nb::class_<PyMeshQuery>(
        m, "MeshQuery",
        "Distance and insideness against a mesh's triangles.\n\n"
        "The tree is built when this is constructed, not per call, because\n"
        "building it is the expensive part of an import — an interface that hid\n"
        "that would rebuild it every query.\n\n"
        "Insideness is the GENERALIZED WINDING NUMBER, not a ray cast or the\n"
        "nearest triangle's normal. Both of those are exact on a clean closed\n"
        "mesh and wrong on the meshes people import: one hole flips a parity\n"
        "count for a whole half-space, and the nearest triangle to a point\n"
        "inside a model may face away when the wall it should have hit is\n"
        "missing. A winding number passes smoothly through a half across an\n"
        "opening instead.")
        .def("__init__",
             [](PyMeshQuery* self, const PyMesh& mesh) {
                 if (mesh.data().triangle_count() == 0)
                     throw std::invalid_argument("a mesh with no triangles has no surface");
                 new (self) PyMeshQuery{mesh::Bvh::build(mesh.data())};
             },
             "mesh"_a)
        .def_prop_ro("triangle_count",
                     [](const PyMeshQuery& q) { return q.bvh.triangle_count(); })
        .def("distance",
             [](const PyMeshQuery& q, nb::handle points) {
                 return map_points(points, [&](kernel::cfloat3 p) {
                     return q.bvh.unsigned_distance(p);
                 });
             },
             "points"_a, "Unsigned distance to the surface at (N, 3) points -> (N,)")
        .def("winding_number",
             [](const PyMeshQuery& q, nb::handle points, float beta) {
                 return map_points(points,
                                   [&](kernel::cfloat3 p) { return q.bvh.winding_number(p, beta); });
             },
             "points"_a, "beta"_a = 2.0f,
             "~1 inside a closed surface, ~0 outside, continuous across a hole.\n"
             "`beta` is how far a node must be before it stands in for its\n"
             "triangles; 0 sums every triangle exactly, which is slow and is\n"
             "what the approximation is checked against.")
        .def("signed_distance",
             [](const PyMeshQuery& q, nb::handle points, float beta) {
                 return map_points(points, [&](kernel::cfloat3 p) {
                     return q.bvh.signed_distance(p, beta);
                 });
             },
             "points"_a, "beta"_a = 2.0f, "Distance, negated inside -> (N,)")
        .def("contains",
             [](const PyMeshQuery& q, nb::handle points, float beta) {
                 return map_points(points, [&](kernel::cfloat3 p) {
                     return q.bvh.is_inside(p, beta) ? 1.0f : 0.0f;
                 });
             },
             "points"_a, "beta"_a = 2.0f, "1 where inside, 0 where outside -> (N,)");

    nb::class_<PyMesh>(m, "Mesh", "Triangle mesh with numpy buffer views")
        .def(
            "transfer_attributes",
            [](PyMesh& self, const PyMesh& source, bool colors, bool uvs, bool normals,
               float max_distance) {
                if (max_distance < 0.0f)
                    throw std::invalid_argument(
                        "max_distance must be >= 0; zero derives it from the source's size");
                mesh::TransferOptions o;
                o.colors = colors;
                o.uvs = uvs;
                o.normals = normals;
                o.max_distance = max_distance;
                const mesh::Mesh& src = source.data();
                mesh::Mesh& dst = self.editable();
                mesh::TransferReport r;
                {
                    nb::gil_scoped_release release;
                    r = mesh::transfer_attributes(src, &dst, o);
                }
                nb::dict out;
                out["transferred"] = r.transferred;
                out["fell_back"] = r.fell_back;
                out["colors"] = r.colors;
                out["uvs"] = r.uvs;
                out["normals"] = r.normals;
                out["max_distance"] = r.max_distance;
                return out;
            },
            "source"_a, "colors"_a = true, "uvs"_a = true, "normals"_a = false,
            "max_distance"_a = 0.0f,
            "Take `source`'s per-vertex attributes by closest point; returns what\n"
            "happened as a dict.\n\n"
            "Everything that leaves a mesh layer loses what the layer was holding:\n"
            "sampling a model into a field and meshing it back gives new geometry\n"
            "with no colours and no uvs. The nearest point on the ORIGINAL surface\n"
            "knows what belonged there, and this reads it.\n\n"
            "IT DOES NOT GIVE BACK TOPOLOGY. The target is still the mesher's\n"
            "geometry. This refunds the paint and most of the uvs, not the mesh.\n\n"
            "`normals` is OFF by default and the default is the point: a resampled\n"
            "mesh should shade like ITSELF, and taking the source's normals makes\n"
            "new geometry shade like the old shape.\n\n"
            "Positions and indices are never modified — this is a transfer, not a\n"
            "projection.\n\n"
            "`max_distance` 0 derives a threshold from the source's size. Past it a\n"
            "vertex takes the fallback instead of the attribute of whatever was\n"
            "nearest, because geometry can exist where the source never was; the\n"
            "returned `fell_back` count is how you tell a good result from one that\n"
            "fell back everywhere.\n\n"
            "UV SEAMS: uvs are per VERTEX, which is how a seam exists — the source\n"
            "duplicates a position into two vertices with different uvs. A target\n"
            "vertex on a seam has one slot and two right answers. Colour is\n"
            "unaffected, being continuous across a seam.")
        .def(
            "weld",
            [](PyMesh& self, float epsilon, bool preserve_attribute_splits,
               float attribute_epsilon) {
                if (!(epsilon >= 0.0f))
                    throw std::invalid_argument(
                        "epsilon must be >= 0; zero welds only bit-identical positions");
                if (!(attribute_epsilon >= 0.0f))
                    throw std::invalid_argument("attribute_epsilon must be >= 0");
                mesh::WeldOptions o;
                o.epsilon = epsilon;
                o.preserve_attribute_splits = preserve_attribute_splits;
                o.attribute_epsilon = attribute_epsilon;
                mesh::Mesh& data = self.editable();
                mesh::WeldReport r;
                {
                    nb::gil_scoped_release release;
                    r = mesh::weld(&data, o);
                }
                // A weld REPLACES the triangles, so anything cached over this
                // layer is as stale as it would be after a rebuild. Bumped only
                // when something moved: a weld that changed nothing must not
                // invalidate a live sculptor.
                if (self.revisions &&
                    r.vertices_merged + r.triangles_collapsed + r.triangles_invalid > 0)
                    (*self.revisions)[self.layer] =
                        revision_of(*self.revisions, self.layer) + 1;
                nb::dict out;
                out["vertices_before"] = r.vertices_before;
                out["vertices_after"] = r.vertices_after;
                out["triangles_before"] = r.triangles_before;
                out["triangles_after"] = r.triangles_after;
                out["vertices_merged"] = r.vertices_merged;
                out["triangles_collapsed"] = r.triangles_collapsed;
                out["triangles_invalid"] = r.triangles_invalid;
                out["vertices_unreferenced"] = r.vertices_unreferenced;
                out["epsilon"] = r.epsilon;
                out["quads_dropped"] = r.quads_dropped;
                return out;
            },
            "epsilon"_a = mesh::kDefaultWeldEpsilon, "preserve_attribute_splits"_a = true,
            "attribute_epsilon"_a = 1e-6f,
            "Merge coincident vertices and remove the triangles that collapses.\n"
            "Returns what it did as a dict.\n\n"
            "WHAT THIS IS FOR. The default mesher emits ZERO-AREA TRIANGLES —\n"
            "1458 of 70,140 on a plain sphere, two per cent, with two corners at\n"
            "bit-identical positions. Almost everything downstream tolerates\n"
            "them, which is why nobody had noticed; a half-edge surface cannot,\n"
            "so no mesh this library marched could become a DynamicSurface until\n"
            "this existed.\n\n"
            "Not the same verb as a sculptor's weld epsilon. That one groups\n"
            "vertices into classes and leaves the mesh alone, so a seam's\n"
            "duplicates move together; this MERGES them and rewrites the\n"
            "triangles.\n\n"
            "`epsilon` is a fraction of the bounding-box diagonal. Weld at AT\n"
            "LEAST what the consumer will weld at — welding below it only moves\n"
            "the problem.\n\n"
            "`preserve_attribute_splits` refuses to merge vertices whose uvs or\n"
            "colours disagree, and the default is the safety of this verb: a UV\n"
            "SEAM IS DUPLICATED POSITIONS WITH DIFFERENT UVS, and merging across\n"
            "one destroys the layout.\n\n"
            "Watertightness survives — a triangle whose corners coincide bounds\n"
            "nothing, so removing it cannot open a hole. Quads are dropped when\n"
            "anything changes. A mesh with nothing to merge comes back\n"
            "byte-identical.")
        .def(
            "voxel_remesh_estimate",
            [](const PyMesh& self, nb::handle resolution, nb::handle voxel_size,
               nb::handle memory_budget) {
                const mesh::VoxelRemeshParams p =
                    py_remesh_params(resolution, voxel_size, memory_budget);
                mesh::VoxelRemeshEstimate e;
                {
                    const mesh::Mesh& src = self.data();
                    nb::gil_scoped_release release;
                    e = mesh::voxel_remesh_estimate(src, p);
                }
                if (e.status != mesh::VoxelRemeshStatus::Ok &&
                    e.status != mesh::VoxelRemeshStatus::ExceedsBudget)
                    raise_remesh(e.status);
                nb::dict out;
                out["voxel_size"] = e.resolved_voxel_size;
                out["grid_dimensions"] = nb::make_tuple(
                    e.grid_dimensions[0], e.grid_dimensions[1], e.grid_dimensions[2]);
                out["active_samples"] = e.estimated_active_samples;
                out["memory_bytes"] = e.estimated_memory_bytes;
                out["triangle_min"] = e.estimated_triangle_min;
                out["triangle_max"] = e.estimated_triangle_max;
                out["boundary_edges"] = e.boundary_edge_count;
                out["components"] = e.component_count;
                out["open_boundaries"] = e.has_open_boundaries;
                out["thin_feature_warning"] = e.thin_feature_warning;
                out["exceeds_memory_budget"] = e.exceeds_memory_budget;
                return out;
            },
            "resolution"_a = nb::none(), "voxel_size"_a = nb::none(),
            "memory_budget"_a = nb::none(),
            "What a voxel remesh would cost, without performing it.\n\n"
            "Cheap enough to call on every tick of a resolution slider: it walks\n"
            "the triangles and marks a brick lattice, and allocates nothing\n"
            "proportional to the samples it is predicting.\n\n"
            "`active_samples` is an UPPER BOUND on the narrow band, not a\n"
            "prediction -- the marking keeps bricks that turn out to hold nothing\n"
            "near enough to store. The remesh report's `active_samples` is what\n"
            "the run actually held.\n\n"
            "Give exactly one of `resolution` (longest axis, an integer) or\n"
            "`voxel_size` (world units).")
        .def(
            "voxel_remesh",
            [](const PyMesh& self, nb::handle resolution, nb::handle voxel_size,
               const std::string& open_surface, const std::string& surface_mode,
               bool preserve_volume, bool project_to_source, float projection_strength,
               bool preserve_colors, nb::handle minimum_component_volume,
               nb::handle memory_budget) {
                mesh::VoxelRemeshParams p =
                    py_remesh_params(resolution, voxel_size, memory_budget);
                p.open_surface_policy = py_open_policy(open_surface);
                p.surface_mode = py_surface_mode(surface_mode);
                p.preserve_volume = preserve_volume;
                p.project_to_source = project_to_source;
                p.projection_strength = projection_strength;
                p.preserve_colors = preserve_colors;
                if (!minimum_component_volume.is_none()) {
                    p.small_component_policy =
                        mesh::VoxelRemeshSmallComponentPolicy::RemoveBelowVolume;
                    p.minimum_component_volume = nb::cast<float>(minimum_component_volume);
                }

                mesh::VoxelRemeshResult r;
                {
                    const mesh::Mesh& src = self.data();
                    nb::gil_scoped_release release;
                    r = mesh::voxel_remesh(src, p);
                }
                if (r.status != mesh::VoxelRemeshStatus::Ok) raise_remesh(r.status);

                PyMesh out;
                out.m = std::move(r.mesh);
                nb::dict report = voxel_remesh_report_dict(r.report);
                return nb::make_tuple(std::move(out), std::move(report));
            },
            "resolution"_a = nb::none(), "voxel_size"_a = nb::none(),
            "open_surface"_a = "close", "surface_mode"_a = "smooth",
            "preserve_volume"_a = true, "project_to_source"_a = true,
            "projection_strength"_a = 0.75f, "preserve_colors"_a = true,
            "minimum_component_volume"_a = nb::none(), "memory_budget"_a = nb::none(),
            "Rebuild the whole surface through a signed volumetric representation\n"
            "at an explicit spatial resolution -- the operation an artist calls\n"
            "DynaMesh. Returns (mesh, report).\n\n"
            "Overlapping shells fuse, self-intersections resolve, stretched\n"
            "triangles disappear, and the topology is REPLACED: no source vertex\n"
            "or polygon index means anything about the result. Details finer than\n"
            "the voxel size may be lost, and UVs are DROPPED rather than\n"
            "reprojected -- the report says both.\n\n"
            "Give exactly one of `resolution` (longest axis) or `voxel_size`.\n"
            "surface_mode='sharp' selects dual contouring and is EXPERIMENTAL:\n"
            "the watertight guarantee is not claimed for it.\n\n"
            "A refusal raises, and the message names which contract refused it.")
        .def(
            "transfer_vertex_scalar",
            [](const PyMesh& self, const PyMesh& source, nb::handle values,
               float max_distance, float fallback) {
                if (max_distance < 0.0f)
                    throw std::invalid_argument(
                        "max_distance must be >= 0; zero derives it from the source's size");
                nb::object flat = nb::module_::import_("numpy")
                                      .attr("asarray")(values, "dtype"_a = "float32")
                                      .attr("reshape")(-1);
                std::vector<float> in;
                for (nb::handle v : flat) in.push_back(nb::cast<float>(v));
                const mesh::Mesh& src = source.data();
                if (in.size() != src.positions.size())
                    throw std::invalid_argument("values must hold one entry per source vertex");
                std::vector<float> out;
                {
                    const mesh::Mesh& dst = self.data();
                    nb::gil_scoped_release release;
                    out = mesh::transfer_vertex_scalar(src, in, dst, max_distance, fallback);
                }
                return nb::module_::import_("numpy").attr("asarray")(nb::cast(out),
                                                                     "dtype"_a = "float32");
            },
            "source"_a, "values"_a, "max_distance"_a = 0.0f, "fallback"_a = 0.0f,
            "Resample a per-vertex scalar -- a mask, a weight -- from `source` onto\n"
            "this mesh by closest point. Returns one float per vertex of THIS mesh.\n\n"
            "What carries a mask across a voxel remesh: the remesh replaces the\n"
            "topology the mask was indexed by, so anything that survives has to be\n"
            "resampled from geometry. A vertex further than `max_distance` from the\n"
            "source takes `fallback`; zero derives the threshold.")
        .def_static(
            "from_triangles",
            [](nb::handle positions, nb::handle indices) {
                PointsView pts = to_points(positions);
                PyMesh out;
                out.m.positions.reserve(pts.count);
                for (std::size_t i = 0; i < pts.count; ++i)
                    out.m.positions.push_back(
                        kernel::cf3(pts.data[i * 3], pts.data[i * 3 + 1], pts.data[i * 3 + 2]));
                nb::object flat = nb::module_::import_("numpy")
                                      .attr("asarray")(indices, "dtype"_a = "uint32")
                                      .attr("reshape")(-1);
                for (nb::handle v : flat) {
                    std::uint32_t index = nb::cast<std::uint32_t>(v);
                    if (index >= pts.count)
                        throw std::invalid_argument("an index points past the vertices");
                    out.m.indices.push_back(index);
                }
                if (out.m.indices.empty() || out.m.indices.size() % 3 != 0)
                    throw std::invalid_argument("need a whole number of triangles");
                return out;
            },
            "positions"_a, "indices"_a,
            "Build a mesh from an (N, 3) vertex array and a flat triangle index\n"
            "array — which is how you hand back a mesh you have edited.")
        .def_prop_ro("positions",
                     [](nb::object self) {
                         return f3_view(self, nb::cast<PyMesh&>(self).data().positions);
                     })
        .def_prop_ro("normals",
                     [](nb::object self) {
                         return f3_view(self, nb::cast<PyMesh&>(self).data().normals);
                     })
        .def_prop_ro("colors",
                     [](nb::object self) {
                         return f3_view(self, nb::cast<PyMesh&>(self).data().colors);
                     })
        .def_prop_ro("uvs",
                     [](nb::object self) {
                         return f2_view(self, nb::cast<PyMesh&>(self).data().uvs);
                     })
        .def_prop_ro("indices",
                     [](nb::object self) -> nb::object {
                         const mesh::Mesh& mm = nb::cast<const PyMesh&>(self).data();
                         if (mm.indices.empty()) {
                             nb::module_ np = nb::module_::import_("numpy");
                             return np.attr("empty")(nb::make_tuple(0, 3), "dtype"_a = "uint32");
                         }
                         return nb::cast(nb::ndarray<nb::numpy, const std::uint32_t>(
                             mm.indices.data(), {mm.indices.size() / 3, 3}, self));
                     })
        .def_prop_ro("quads",
                     [](nb::object self) -> nb::object {
                         const mesh::Mesh& mm = nb::cast<const PyMesh&>(self).data();
                         // An empty (0, 4) rather than None, matching how
                         // `indices` presents an empty mesh: a caller can shape
                         // code against it without a null check.
                         if (mm.quads.empty()) {
                             nb::module_ np = nb::module_::import_("numpy");
                             return np.attr("empty")(nb::make_tuple(0, 4), "dtype"_a = "uint32");
                         }
                         return nb::cast(nb::ndarray<nb::numpy, const std::uint32_t>(
                             mm.quads.data(), {mm.quads.size() / 4, 4}, self));
                     },
                     "The quads, (Q, 4) uint32, zero-copy — empty (0, 4) on a mesh that has\n"
                     "none, which is every mesh from an ordinary mesher or a file.\n\n"
                     "These sit BESIDE the triangles, never instead of them: `indices` still\n"
                     "holds exactly this list's triangulation, quad q = (a, b, c, d) being\n"
                     "triangles (a, b, c) and (a, c, d), so nothing an existing script reads\n"
                     "changes value.")
        .def_prop_ro("quad_count", [](const PyMesh& pm) { return pm.data().quad_count(); })
        .def_prop_ro("quad_report",
                     [](const PyMesh& pm) {
                         if (!pm.fit)
                             throw std::invalid_argument(
                                 "this mesh was not produced by a quad mesher, so there is no "
                                 "search to report — a mesh loaded from a file or read back "
                                 "out of a document has no cell size or target behind it");
                         return quad_report_dict(*pm.fit, pm.fit_target);
                     },
                     "How this mesh was quad-meshed: cell_size, target, quad_count,\n"
                     "iterations, within_tolerance, clamped. Raises on a mesh that was not\n"
                     "quad-meshed, rather than reporting zeroes you cannot tell from a\n"
                     "search that found nothing.\n\n"
                     "A TARGET IS APPROACHED, NEVER HIT — read Document.mesh_quads.")
        .def_prop_ro("triangle_count", [](const PyMesh& pm) { return pm.data().triangle_count(); })
        .def("is_watertight",
             [](const PyMesh& pm) { return mesh::validate(pm.data()).watertight; },
             "True when every edge is shared by exactly two triangles.\n\n"
             "Runs a full validation. Asking this AND is_manifold runs two of\n"
             "them — validation_report() answers both from one pass, and nine\n"
             "more things besides.")
        .def("is_manifold",
             [](const PyMesh& pm) { return mesh::validate(pm.data()).manifold; },
             "True when no edge has more than two incident triangles.\n\n"
             "See is_watertight on running one validation instead of two.")
        .def(
            "validation_report",
            [](const PyMesh& pm, std::size_t max_intersection_pairs) {
                const mesh::ValidationReport r =
                    mesh::validate(pm.data(), max_intersection_pairs);
                nb::dict out;
                out["vertices"] = r.vertices;
                out["triangles"] = r.triangles;
                out["watertight"] = r.watertight;
                out["manifold"] = r.manifold;
                out["oriented"] = r.oriented;
                out["clean"] = r.clean();
                out["boundary_edges"] = r.boundary_edges;
                out["non_manifold_edges"] = r.non_manifold_edges;
                out["degenerate_triangles"] = r.degenerate_triangles;
                out["sliver_triangles"] = r.sliver_triangles;
                out["intersecting_pairs"] = r.intersecting_pairs;
                out["intersection_budget"] = max_intersection_pairs;
                out["euler_characteristic"] = r.euler_characteristic;
                return out;
            },
            "max_intersection_pairs"_a = 0,
            "Everything the validator measures, in one pass: vertices,\n"
            "triangles, watertight, manifold, oriented, clean, boundary_edges,\n"
            "non_manifold_edges, degenerate_triangles, sliver_triangles,\n"
            "intersecting_pairs, intersection_budget, euler_characteristic.\n\n"
            "`max_intersection_pairs` caps the SAMPLED self-intersection check\n"
            "and is reported back as `intersection_budget`, because that is the\n"
            "only thing separating 'nothing intersects' from 'nothing looked':\n"
            "both leave intersecting_pairs at 0, and `clean` requires it to be\n"
            "0. The default of 0 skips the pass, which is the engine's own\n"
            "default — so a `clean` from a default call has not been checked\n"
            "for self-intersection.\n\n"
            "`sliver_triangles` is informational and is not one of `clean`'s\n"
            "terms: a near-zero-area triangle is legal geometry.")
        .def_prop_ro(
            "signed_volume", [](const PyMesh& pm) { return mesh::signed_volume(pm.data()); },
            "Signed volume by the divergence theorem, POSITIVE when triangle\n"
            "normals point outward — so its sign is an orientation check.\n"
            "Answered for any mesh, watertight or not: an open mesh still has\n"
            "the sum, and it is the number that tells you the mesh is open.")
        .def_prop_ro("surface_area",
                     [](const PyMesh& pm) { return mesh::surface_area(pm.data()); },
                     "Total area of the triangles.")
        .def_prop_ro("layer",
                     [](const PyMesh& pm) -> nb::object {
                         if (!pm.doc) return nb::none();
                         return nb::cast(pm.layer);
                     },
                     "The layer id a BORROWED mesh belongs to — what the document-level\n"
                     "layer edits take — or None for a mesh you own.")
        .def_prop_ro("bounds",
                     [](const PyMesh& pm) {
                         const mesh::Mesh& mm = pm.data();
                         if (mm.positions.empty())
                             throw std::invalid_argument("an empty mesh has no bounds");
                         math::Aabb box;
                         for (const kernel::cfloat3& p : mm.positions) box.expand(p);
                         return nb::make_tuple(nb::make_tuple(box.min.x, box.min.y, box.min.z),
                                               nb::make_tuple(box.max.x, box.max.y, box.max.z));
                     },
                     "((lo), (hi)) enclosing the positions — how you frame an imported\n"
                     "model. Document.layer_bounds is derived from SDF shapes and reports\n"
                     "nothing for a mesh layer, which is why this lives here.")
        .def(
            "save_handoff",
            [](const PyMesh& mesh, const std::string& path, const std::string& producer,
               nb::handle material_mask, bool binary) {
                io::HandoffOptions o;
                o.producer = producer;
                o.material_mask = borrow_mask(material_mask);
                o.binary = binary;
                io::IoStatus s = io::save_handoff_ply_file(mesh.data(), path, o);
                if (!s.ok()) throw std::runtime_error("handoff write failed: " + s.detail);
            },
            "path"_a, "producer"_a = "claycore", "material_mask"_a = nb::none(),
            "binary"_a = true,
            "Write the SCULPT HANDOFF that CyberRemesherAndUV's reader accepts —\n"
            "the sculpt -> retopo -> UV -> bake seam, with neither engine\n"
            "linking the other.\n\n"
            "THE FORMAT IS NOT DEFINED HERE. It is that repository's\n"
            "docs/sculpt-handoff-format.md v1.0, which ships the READING half.\n\n"
            "TWO GUARANTEES MADE FOR YOU, because their reader enforces both:\n"
            "the faces written are always TRIANGLES (save() writes a mesh's\n"
            "QUADS as its faces, and their reader rejects any other arity), and\n"
            "NORMALS are always present (computed if the mesh has none; your\n"
            "mesh is not modified).\n\n"
            "`material_mask` fills their required `material_mix` channel: a mask\n"
            "is already a painted scalar in [0,1] answerable at any point, which\n"
            "is the shape their channel asks for. None writes zeros — ClayCore\n"
            "has no material slots and does not invent them.")
        .def(
            "handoff_material_mix",
            [](const PyMesh& mesh, nb::handle material_mask) {
                const std::vector<float> mix =
                    io::handoff_material_mix(mesh.data(), borrow_mask(material_mask));
                nb::module_ np = nb::module_::import_("numpy");
                nb::object out =
                    np.attr("empty")(nb::make_tuple(mix.size()), "dtype"_a = "float32");
                auto v = nb::cast<nb::ndarray<float, nb::ndim<1>, nb::c_contig>>(out);
                for (std::size_t i = 0; i < mix.size(); ++i) v.data()[i] = mix[i];
                return out;
            },
            "material_mask"_a = nb::none(),
            "The `material_mix` column for the IN-MEMORY buffer profile.\n\n"
            "The other four arrays their Mesh.load_handoff_buffers wants —\n"
            "positions, normals, colors, indices — are already numpy views on\n"
            "this mesh, so this produces the one column you cannot get from\n"
            "them. ClayCore does not duplicate the struct they own.")
        .def("save",
             [](const PyMesh& pm, const std::string& path) { save_mesh_any(pm.data(), path); },
             "path"_a, "Save by extension: .obj, .ply, .fbx or .glb")
        .def(
            "to_bytes",
            [](const PyMesh& pm, const std::string& format) {
                const std::vector<std::uint8_t> bytes = save_mesh_bytes_any(pm.data(), format);
                return nb::bytes(bytes.data(), bytes.size());
            },
            "format"_a,
            "The same bytes `save` would write, without a path: 'obj', 'ply',\n"
            "'fbx' or 'glb', matched case-insensitively.\n\n"
            "ONE stated difference from `save`: an in-memory OBJ carries no\n"
            "`mtllib` line. The path form writes a companion .mtl beside the\n"
            "object file and names it; a buffer has no companion, and naming a\n"
            "file that was never written is worse than naming none.")
        .def(
            "preflight_to_dynamic",
            [](const PyMesh& pm, std::uint64_t budget) {
                return preflight_dict(mesh::preflight_to_dynamic(pm.data(), budget));
            },
            "budget"_a = 0,
            "What converting this mesh into an adaptive surface WOULD cost,\n"
            "asked before any of it is paid. Allocates nothing and has no side\n"
            "effects.\n\n"
            "Read `peak_bytes`, not `persistent_bytes`: the conversion holds the\n"
            "source mesh, the half-edge structure and the weld map at once, and\n"
            "the peak is what kills an application on a constrained device.\n"
            "A budget of 0 means no budget and always allows — except on an\n"
            "arithmetic overflow, which refuses at any budget, because an\n"
            "estimate nobody can compute is not one anybody may rely on.")
        .def(
            "preflight_global_remesh",
            [](const PyMesh& pm, std::uint64_t target_triangles, std::uint64_t budget) {
                return preflight_dict(
                    mesh::preflight_global_remesh(pm.data(), target_triangles, budget));
            },
            "target_triangles"_a, "budget"_a = 0,
            "The same question for a global remesh, where source and target are\n"
            "live at the same time — which is the whole reason it is asked.");

    // -- fixed-topology mesh brushes -------------------------------------------------
    nb::class_<parallel::CancelToken>(
        m, "CancelToken",
        "Stop a long operation, and see how far it got.\n\n"
        "This library has three budget classes and the third had no exit: on\n"
        "the reference iPad mask_extrude measures 4403 ms and consolidate\n"
        "661 ms, and every one was a call you entered and could not leave.\n\n"
        "`cancel()` is the ONE thing that is safe to call on a running\n"
        "operation from another thread — everything else in this library\n"
        "requires you to serialize. A cancelled operation leaves everything it\n"
        "was given exactly as it found it, so you never have to undo one.\n\n"
        "Reusable: reset() clears the flag, so one token per document does not\n"
        "mean an allocation per cancel.")
        .def(nb::init<>())
        .def("cancel", [](parallel::CancelToken& t) { t.cancel(); },
             "Stop the operation using this token. Safe before it starts,\n"
             "during it, after it, and twice.")
        .def_prop_ro("cancelled",
                     [](const parallel::CancelToken& t) { return t.cancelled(); })
        .def("reset", [](parallel::CancelToken& t) { t.reset(); },
             "Clear the cancelled flag and the progress, so the token can be\n"
             "used again.")
        .def_prop_ro(
            "progress",
            [](const parallel::CancelToken& t) {
                const parallel::Progress p = t.progress();
                nb::dict out;
                out["running"] = p.running;
                out["phase"] = p.phase;
                out["phase_count"] = p.phase_count;
                out["fraction"] = p.fraction;
                out["done"] = p.done;
                out["total"] = p.total;
                return out;
            },
            "What the operation is doing: running, phase, phase_count,\n"
            "fraction, done, total.\n\n"
            "NO TIME ESTIMATE, deliberately — a multi-phase operation's phases\n"
            "differ in per-unit cost by more than an order, so a figure derived\n"
            "from the fraction would be wrong in the direction that annoys\n"
            "users most, and you have the wall clock.\n\n"
            "Safe to read from another thread, and when nothing is running it\n"
            "says so rather than reporting a stale fraction.");

    nb::class_<PyVertexDeltas>(
        m, "VertexDeltas",
        "What one sculpting gesture moved, and how to put it back.\n\n"
        "A mesh stroke is destructive vertex displacement, not an edit item, so\n"
        "it cannot undo through the document's command stack the way an SDF\n"
        "stroke does. This is the undo it gets instead: a SPARSE record of the\n"
        "vertices a stroke actually reached, with their positions and normals\n"
        "before and after.\n\n"
        "COALESCED. A vertex passed over forty times by one stroke appears once,\n"
        "keeping the first `before` and the last `after`, so the record's size is\n"
        "bounded by the vertices reached rather than by the stamps taken — and\n"
        "reverting one is one undo step.\n\n"
        "`revert` restores positions AND normals bit-exactly, and never touches\n"
        "the index or quad buffers, because nothing here can change them.")
        .def("__init__", [](PyVertexDeltas* self) { new (self) PyVertexDeltas(); })
        .def_prop_ro(
            "vertex_count", [](const PyVertexDeltas& d) { return d.deltas->size(); },
            "How many vertices this gesture reached")
        .def(
            "revert",
            [](const PyVertexDeltas& d, PyMeshSculptor& s) {
                if (!d.deltas->revert(s.live(true).mesh()))
                    throw std::invalid_argument("this record does not belong to this mesh");
            },
            "sculptor"_a, "Put the mesh back as it was. Idempotent.")
        .def(
            "apply",
            [](const PyVertexDeltas& d, PyMeshSculptor& s) {
                if (!d.deltas->apply(s.live(true).mesh()))
                    throw std::invalid_argument("this record does not belong to this mesh");
            },
            "sculptor"_a, "Redo: put the mesh back as the gesture left it. Idempotent.")
        .def(
            "clear", [](PyVertexDeltas& d) { d.deltas->clear(); },
            "Start a new gesture in the same record");

    nb::class_<PyLattice>(
        m, "Lattice",
        "A lattice (free-form deformation) cage — ZBrush's Gizmo Lattice,\n"
        "Blender's Lattice modifier.\n\n"
        "This runs FORWARD, which is why it works on a mesh layer and not on an\n"
        "SDF item: a claycore SDF deformer is an inverse point map, and forward\n"
        "FFD has no closed-form inverse. A mesh already knows where its vertices\n"
        "are, so nothing here inverts, iterates or approximates.\n\n"
        "The cage holds control-point OFFSETS, so a cage nobody has touched is\n"
        "exactly the identity. Evaluation is trivariate Bernstein: 2 points per\n"
        "axis is exactly trilinear, and the corner control points are\n"
        "interpolated, so dragging a corner moves that corner of the box exactly.\n\n"
        "A vertex OUTSIDE the box travels rigidly with the nearest point of the\n"
        "cage rather than being drawn onto it. An axis on which the box is flat\n"
        "— what a cage over a plane's own bounds gives — reads as the middle, so\n"
        "none of its control points are dead.")
        .def(
            "__init__",
            [](PyLattice* self, nb::handle box, int nx, int ny, int nz) {
                new (self) PyLattice{mesh::Lattice(to_aabb(box), nx, ny, nz)};
            },
            "box"_a, "nx"_a = 3, "ny"_a = 3, "nz"_a = 3,
            "A cage over `box` ((min, max) in the mesh's own space) with nx*ny*nz\n"
            "control points, every offset zero. Divisions are clamped into [2, 32].")
        .def_prop_ro("divisions",
                     [](const PyLattice& l) {
                         return nb::make_tuple(l.cage.nx(), l.cage.ny(), l.cage.nz());
                     })
        .def_prop_ro("is_identity", [](const PyLattice& l) { return l.cage.is_identity(); },
                     "True while no control point has been dragged.")
        .def(
            "set_offset",
            [](PyLattice& l, int i, int j, int k, nb::handle offset) {
                l.cage.set_offset(i, j, k, to_f3(offset, "offset"));
            },
            "i"_a, "j"_a, "k"_a, "offset"_a,
            "Drag one control point. Out-of-range indices write nowhere.")
        .def(
            "offset",
            [](const PyLattice& l, int i, int j, int k) {
                const kernel::cfloat3 o = l.cage.offset(i, j, k);
                return nb::make_tuple(o.x, o.y, o.z);
            },
            "i"_a, "j"_a, "k"_a)
        .def(
            "rest",
            [](const PyLattice& l, int i, int j, int k) {
                const kernel::cfloat3 o = l.cage.rest(i, j, k);
                return nb::make_tuple(o.x, o.y, o.z);
            },
            "i"_a, "j"_a, "k"_a, "Where the control point started — what a UI draws.")
        .def(
            "position",
            [](const PyLattice& l, int i, int j, int k) {
                const kernel::cfloat3 o = l.cage.position(i, j, k);
                return nb::make_tuple(o.x, o.y, o.z);
            },
            "i"_a, "j"_a, "k"_a, "Where the control point is now.")
        .def(
            "displacement",
            [](const PyLattice& l, nb::handle p) {
                const kernel::cfloat3 d = l.cage.displacement(to_f3(p, "p"));
                return nb::make_tuple(d.x, d.y, d.z);
            },
            "p"_a, "What this cage moves a point at `p` by. Exactly zero everywhere\n"
            "for an untouched cage.");

    nb::class_<PyMeshSculptor>(
        m, "MeshSculptor",
        "The classical sculpting mode, on a mesh layer's own triangles.\n\n"
        "TOPOLOGY NEVER CHANGES. No verb here creates, splits, deletes or\n"
        "reorders a polygon or a vertex — `indices` and `quads` come out byte\n"
        "for byte as they went in, so a quad mesh sculpted here is still the\n"
        "same quad mesh. That is the whole point: the only other way to edit a\n"
        "mesh layer is `Volume.from_mesh`, which resamples the model onto a\n"
        "lattice and throws away the retopology somebody paid for.\n\n"
        "Said plainly rather than discovered: a large grab STRETCHES triangles,\n"
        "and 'snakehook' stretches them to the extreme. That is the signal the\n"
        "mesh wants retopo, exactly as Blender behaves with Dyntopo off.\n\n"
        "Built over a Mesh — including the one a mesh LAYER hands back, in which\n"
        "case sculpting edits the document's own triangles rather than a copy.\n"
        "A locked or ghosted layer refuses, because both mean 'never edited'.\n\n"
        "The adjacency it builds never goes stale, since no verb can change the\n"
        "topology it describes. The ray tree does — positions move under it — so\n"
        "`refresh` is how you decide when to pay for rebuilding it.")
        .def(
            "__init__",
            [](PyMeshSculptor* self, nb::object mesh, float weld_epsilon) {
                PyMesh* pm = nb::cast<PyMesh*>(mesh);
                mesh::Mesh& data = pm->editable();
                if (data.triangle_count() == 0)
                    throw std::invalid_argument("a mesh with no triangles has no surface");
                if (weld_epsilon < 0.0f) throw std::invalid_argument("weld_epsilon must be >= 0");
                new (self) PyMeshSculptor();
                self->owner = mesh;
                self->mesh = pm;
                self->bound = &data;
                self->geometry_revision =
                    pm->revisions ? revision_of(*pm->revisions, pm->layer) : 0;
                self->sculptor = std::make_shared<mesh::MeshSculptor>(data, weld_epsilon);
            },
            "mesh"_a, "weld_epsilon"_a = mesh::kDefaultWeldEpsilon,
            "`weld_epsilon` is relative to the bounding-box diagonal: vertices\n"
            "closer than that are one point of the surface, which is what lets a\n"
            "brush cross a UV seam without opening a crack. 0 welds on exact bits.")
        .def_prop_ro(
            "vertex_count",
            [](const PyMeshSculptor& s) { return s.live(false).adjacency().vertex_count(); })
        .def_prop_ro(
            "class_count",
            [](const PyMeshSculptor& s) { return s.live(false).adjacency().class_count(); },
            "Welded classes — fewer than `vertex_count` exactly where the mesh\n"
            "has seams, which is how you can tell you imported a split model.")
        .def(
            "stamp",
            [](PyMeshSculptor& s, const std::string& verb, nb::handle center, float radius,
               float strength, const std::string& falloff, nb::handle direction,
               nb::handle deposit_normal, nb::handle geodesic, nb::handle seed_class,
               const std::string& flatten_mode, nb::handle plane_point, nb::handle plane_normal,
               float polish_angle, int smooth_iterations, float layer_height, nb::handle alpha,
               nb::handle alpha_direction, nb::handle alpha_tangent, float alpha_extent,
               nb::handle color, nb::handle mask, nb::handle deltas) {
                mesh::MeshBrush chosen = mesh::MeshBrush::Draw;
                mesh::MeshBrushSettings settings = mesh_brush_settings(
                    verb, center, radius, strength, falloff, direction, deposit_normal, geodesic,
                    seed_class, flatten_mode, plane_point, plane_normal, polish_angle,
                    smooth_iterations, layer_height, alpha, alpha_direction, alpha_tangent,
                    alpha_extent, color, &chosen);
                mesh::VertexDeltas* record =
                    deltas.is_none() ? nullptr : nb::cast<PyVertexDeltas*>(deltas)->deltas.get();
                field::MaskGate gate = mask_gate_of(mask);
                mesh::MeshSculptor& live = s.live(true);
                nb::gil_scoped_release release;
                return live.stamp(chosen, settings, gate, record);
            },
            "verb"_a, "center"_a, "radius"_a, "strength"_a = 0.5f, "falloff"_a = "smooth",
            "direction"_a = nb::none(), "deposit_normal"_a = nb::none(), "geodesic"_a = nb::none(),
            "seed_class"_a = nb::none(), "flatten_mode"_a = "two_sided",
            "plane_point"_a = nb::none(), "plane_normal"_a = nb::none(), "polish_angle"_a = 0.20f,
            "smooth_iterations"_a = 1, "layer_height"_a = 0.05f, "alpha"_a = nb::none(),
            "alpha_direction"_a = nb::none(), "alpha_tangent"_a = nb::none(),
            "alpha_extent"_a = 0.0f, "color"_a = nb::none(), "mask"_a = nb::none(),
            "deltas"_a = nb::none(),
            "One stamp; returns how many welded classes moved.\n\n"
            "`verb` is one of:\n"
            "  'grab'      drag the region by `direction`\n"
            "  'draw'      displace along the REGION's averaged normal\n"
            "  'inflate'   displace along EACH VERTEX's own normal (signed)\n"
            "  'smooth'    Laplacian average over the one-ring\n"
            "  'pinch'     signed: + gathers tangentially, - spreads (magnify)\n"
            "  'flatten'   project onto a plane, per `flatten_mode`\n"
            "  'clay'      draw's deposit CLAMPED to a plane: flat-topped strips\n"
            "  'crease'    a tight negative draw and a pinch, in ONE stamp\n"
            "  'scrape'    flatten cut-only and smooth, from ONE snapshot\n"
            "  'polish'    smooth gated by dihedral angle: noise goes, edges stay\n"
            "  'snakehook' grab re-anchored along the drag (see `apply_stroke`)\n"
            "  'relax'     slide vertices ALONG the surface to even their spacing.\n"
            "              Smooth reshapes; this redistributes. It matters here\n"
            "              because topology is fixed: a big grab stretches the\n"
            "              triangles it has, and this recovers them\n"
            "  'layer'     deposit to a CEILING `layer_height` above the surface\n"
            "              as the STROKE found it, so a slow stroke and a fast\n"
            "              one over the same path agree. Needs `deltas`\n"
            "  'nudge'     push material along the surface; grab carries it off\n"
            "  'paint'     blend vertex COLOUR toward `color`; moves no vertex\n"
            "  'smear'     drag existing colour along `direction`; moves no vertex.\n"
            "              A zero direction is no smear rather than a smooth\n\n"
            "The colour pair needs the mesh to HAVE colours: they refuse rather\n"
            "than creating the attribute, because twelve bytes per vertex is a\n"
            "real cost to hide behind a brush stroke, and a silent creation makes\n"
            "'I painted and nothing happened' indistinguishable from 'this mesh\n"
            "had no colours'. Call `ensure_colors()` first.\n\n"
            "`alpha` is a 2D (height, width) float array in [0, 1] scaling the\n"
            "brush's weight — how detail work is done on voxels and SDF items,\n"
            "now here. **The engine decodes no images.** It multiplies the\n"
            "WEIGHT, so it composes with every verb and falloff at once, and it\n"
            "is sampled by the same kernel the SDF alpha uses, so one stamp\n"
            "reads identically on a mesh and on a field.\n\n"
            "`geodesic` measures the falloff ALONG THE SURFACE, so a brush on the\n"
            "upper lip does not drag the chin through a closed mouth. None takes\n"
            "the verb's default: off for 'flatten' and 'scrape', which mean\n"
            "'everything under this disc'.\n\n"
            "`mask` freezes: each vertex's weight is scaled by (1 - mask) at its\n"
            "world position, for every verb, with no per-verb code.")
        .def(
            "deform",
            [](PyMeshSculptor& s, const std::string& verb, nb::handle origin, nb::handle axis,
               float span, float scale_start, float scale_end, float angle, int ease,
               nb::handle mask, nb::handle deltas) {
                mesh::MeshDeformSettings d;
                if (verb == "taper") {
                    d.verb = mesh::MeshDeform::Taper;
                } else if (verb == "twist") {
                    d.verb = mesh::MeshDeform::Twist;
                } else {
                    // `bend` is named in the message on purpose: it is the one
                    // a caller will reach for next, and "unknown verb" would
                    // not tell them it is absent by measurement rather than by
                    // oversight.
                    throw std::invalid_argument(
                        "verb must be 'taper' or 'twist', got '" + verb +
                        "'. There is no 'bend': the SDF bend takes its angle from a "
                        "coordinate it then moves, so it has no forward map, and past a "
                        "gentle angle it folds distinct points onto the same place");
                }
                if (!(span > 0.0f))
                    throw std::invalid_argument(
                        "span must be > 0: there is nothing to ramp across");
                if (!origin.is_none()) d.origin = to_f3(origin, "origin");
                if (!axis.is_none()) d.axis = to_f3(axis, "axis");
                if (!(kernel::clength(d.axis) > 0.0f))
                    throw std::invalid_argument("axis has no length");
                d.span = span;
                d.scale_start = scale_start;
                d.scale_end = scale_end;
                d.angle = angle;
                if (ease < 0 || ease >= kernel::ease_count)
                    throw std::invalid_argument("ease index out of range");
                d.ease = ease;

                mesh::VertexDeltas* record =
                    deltas.is_none() ? nullptr : nb::cast<PyVertexDeltas*>(deltas)->deltas.get();
                field::MaskGate gate = mask_gate_of(mask);
                mesh::MeshSculptor& live = s.live(true);
                nb::gil_scoped_release release;
                return live.apply_deformer(d, gate, record);
            },
            "verb"_a, "origin"_a = nb::none(), "axis"_a = nb::none(), "span"_a = 1.0f,
            "scale_start"_a = 1.0f, "scale_end"_a = 1.0f, "angle"_a = 0.0f, "ease"_a = 0,
            "mask"_a = nb::none(), "deltas"_a = nb::none(),
            "A whole-form deformer over every vertex; returns how many moved.\n\n"
            "`verb` is 'taper' (cross-section scale ramps across the span) or\n"
            "'twist' (rotation about the axis ramps across it).\n\n"
            "NOT a brush: it acts on the whole mesh, because a deformer states\n"
            "something about the FORM and a brush states something about a dab.\n"
            "`mask` is what holds part of the form still, and a fully masked\n"
            "vertex is bit-identical to where it started.\n\n"
            "`origin` and `axis` are the gizmo: the span starts at `origin` and\n"
            "runs `span` units along `axis`. Material before the span is\n"
            "untouched and material past it travels rigidly with the end.\n\n"
            "An identity deformer moves nothing and adds nothing to `deltas`.\n\n"
            "There is no 'bend'. The SDF bend takes its angle from a coordinate\n"
            "it then moves, so it has no closed-form forward map, and past a\n"
            "gentle angle it has none at all: the deformation folds distinct\n"
            "points onto the same place.")
        .def(
            "lattice",
            [](PyMeshSculptor& s, const PyLattice& cage, nb::handle deltas) {
                mesh::VertexDeltas* record =
                    deltas.is_none() ? nullptr : nb::cast<PyVertexDeltas*>(deltas)->deltas.get();
                mesh::MeshSculptor& live = s.live(true);
                nb::gil_scoped_release release;
                return live.apply_lattice(cage.cage, record);
            },
            "cage"_a, "deltas"_a = nb::none(),
            "Apply a Lattice to every vertex; returns how many moved.\n\n"
            "Not a brush: no centre, no radius, no falloff, because the cage IS\n"
            "the falloff. An untouched cage moves nothing and is skipped rather\n"
            "than written back over itself, so it also adds nothing to `deltas`.\n\n"
            "Topology never changes, as with every verb here, and the whole\n"
            "lattice is ONE undo step.")
        .def(
            "apply_preset",
            [](PyMeshSculptor& s, nb::handle samples, const brush::BrushPreset& preset,
               nb::handle alpha, float alpha_extent, nb::handle mask, nb::handle deltas,
               bool defer_normals, bool orient_alpha_by_stamp) {
                std::vector<brush::StrokeSample> in = to_stroke_samples(samples);
                // THE ALPHA IS PASSED HERE, NOT CARRIED BY THE PRESET. Image
                // content stays the caller's — a preset library has to cost
                // kilobytes — and this is also the only thing that makes a
                // stamp's ORIENTATION observable, since a round brush has
                // nothing to orient.
                mesh::MeshBrushSettings settings = preset.settings;
                if (!alpha.is_none()) {
                    auto arr =
                        nb::cast<nb::ndarray<const float, nb::ndim<2>, nb::c_contig>>(alpha);
                    if (arr.shape(0) < 2 || arr.shape(1) < 2)
                        throw std::invalid_argument(
                            "an alpha needs at least 2x2 samples; there is nothing to "
                            "interpolate below that");
                    settings.alpha = arr.data();
                    settings.alpha_height = static_cast<int>(arr.shape(0));
                    settings.alpha_width = static_cast<int>(arr.shape(1));
                    settings.alpha_extent = alpha_extent;
                }
                mesh::VertexDeltas* record =
                    deltas.is_none() ? nullptr : nb::cast<PyVertexDeltas*>(deltas)->deltas.get();
                const voxel::MaskField* field_mask = borrow_mask(mask);
                brush::MeshStrokeOptions options;
                options.defer_normals = defer_normals;
                options.orient_alpha_by_stamp = orient_alpha_by_stamp;
                mesh::MeshSculptor& live = s.live(true);
                nb::gil_scoped_release release;
                return brush::apply_to_mesh(live, brush::resolve_stroke(in, preset.stroke),
                                            preset.model.verb, settings, field_mask, record,
                                            options);
            },
            "samples"_a, "preset"_a, "alpha"_a = nb::none(), "alpha_extent"_a = 0.0f,
            "mask"_a = nb::none(), "deltas"_a = nb::none(), "defer_normals"_a = false,
            "orient_alpha_by_stamp"_a = false,
            "Apply a stroke driven by a BRUSH preset, which already carries the\n"
            "stroke preset, the verb and the brush's own settings.\n\n"
            "This is the call a host makes once it has a brush library rather\n"
            "than a set of sliders. `orient_alpha_by_stamp` lets each stamp's\n"
            "rotation turn the alpha, which is what makes a rake or a chisel\n"
            "expressible; the preset's stroke half decides whether that rotation\n"
            "follows the path or the stylus barrel.")
        .def(
            "apply_stroke",
            [](PyMeshSculptor& s, nb::handle samples, const brush::StrokePreset& preset,
               const std::string& verb, const std::string& falloff, nb::handle deposit_normal,
               nb::handle geodesic, nb::handle seed_class, const std::string& flatten_mode,
               nb::handle plane_point, nb::handle plane_normal, float polish_angle,
               int smooth_iterations, float layer_height, float strength, nb::handle color,
               nb::handle mask, nb::handle deltas, bool defer_normals) {
                mesh::MeshBrush chosen = mesh::MeshBrush::Draw;
                // The radius is the STAMP's, so a placeholder goes in here.
                // No alpha on the stroke path: a stamp-oriented alpha along a
                // stroke wants the frame to follow the stroke direction, which
                // is its own decision — see the proposal's open question.
                mesh::MeshBrushSettings settings = mesh_brush_settings(
                    verb, nb::none(), 1.0f, strength, falloff, nb::none(), deposit_normal, geodesic,
                    seed_class, flatten_mode, plane_point, plane_normal, polish_angle,
                    smooth_iterations, layer_height, nb::none(), nb::none(), nb::none(), 0.0f,
                    color, &chosen);
                std::vector<brush::StrokeSample> in = to_stroke_samples(samples);
                mesh::VertexDeltas* record =
                    deltas.is_none() ? nullptr : nb::cast<PyVertexDeltas*>(deltas)->deltas.get();
                const voxel::MaskField* field_mask = borrow_mask(mask);
                brush::MeshStrokeOptions options;
                options.defer_normals = defer_normals;
                mesh::MeshSculptor& live = s.live(true);
                nb::gil_scoped_release release;
                return brush::apply_to_mesh(live, brush::resolve_stroke(in, preset), chosen,
                                            settings, field_mask, record, options);
            },
            "samples"_a, "preset"_a, "verb"_a, "falloff"_a = "smooth",
            "deposit_normal"_a = nb::none(), "geodesic"_a = nb::none(), "seed_class"_a = nb::none(),
            "flatten_mode"_a = "two_sided", "plane_point"_a = nb::none(),
            "plane_normal"_a = nb::none(), "polish_angle"_a = 0.20f, "smooth_iterations"_a = 1,
            "layer_height"_a = 0.05f, "strength"_a = 1.0f, "color"_a = nb::none(),
            "mask"_a = nb::none(), "deltas"_a = nb::none(), "defer_normals"_a = false,
            "Resolve a stroke and apply it — the stroke engine's FOURTH consumer,\n"
            "after the voxel grid, the mask and the edit list. Spacing, pressure\n"
            "response, deterministic jitter, taper, steady stroke and\n"
            "buildup-versus-clamped accumulation all reach mesh sculpting with no\n"
            "new machinery. Buildup is what turns one 'clay' stamp into\n"
            "ClayBuildup.\n\n"
            "Each stamp brings its own radius and strength from the preset;\n"
            "`strength` here multiplies them, it does not replace them.\n\n"
            "'grab' anchors on the first stamp and drags by the motion between\n"
            "stamps; 'snakehook' re-anchors on every stamp, so its region walks\n"
            "with the pull. Returns how many stamps moved a vertex.")
        .def(
            "raycast",
            [](PyMeshSculptor& s, nb::handle origin, nb::handle direction, nb::handle position,
               nb::handle rotation_axis, float rotation_angle, float scale) {
                math::Ray ray;
                ray.origin = to_f3(origin, "origin");
                ray.dir = to_f3(direction, "direction");
                if (!(kernel::clength(ray.dir) > 0.0f))
                    throw std::invalid_argument("direction has no length");
                ray.dir = kernel::cnormalize(ray.dir);
                if (!(scale > 0.0f)) throw std::invalid_argument("scale must be > 0");
                math::Transform xform;
                if (!position.is_none()) xform.position = to_f3(position, "position");
                if (!rotation_axis.is_none())
                    xform.rotation = math::Quat::from_axis_angle(
                        kernel::cnormalize(to_f3(rotation_axis, "rotation_axis")), rotation_angle);
                xform.scale = scale;

                mesh::MeshSculptor& live = s.live(false);
                const mesh::Mesh& mm = live.mesh();
                const pick::MeshHit hit = pick::raycast_mesh(mm, live.bvh(), ray, xform);
                if (!hit.hit) return nb::object(nb::none());
                nb::dict out;
                out["t"] = hit.t;
                out["position"] = nb::make_tuple(hit.position.x, hit.position.y, hit.position.z);
                out["normal"] = nb::make_tuple(hit.normal.x, hit.normal.y, hit.normal.z);
                out["triangle"] = hit.triangle;
                out["u"] = hit.u;
                out["v"] = hit.v;
                out["seed_class"] = live.adjacency().class_of(mm.indices[hit.triangle * 3]);
                return nb::object(out);
            },
            "origin"_a, "direction"_a, "position"_a = nb::none(), "rotation_axis"_a = nb::none(),
            "rotation_angle"_a = 0.0f, "scale"_a = 1.0f,
            "Where a ray meets the mesh, or None. The layer transform is applied\n"
            "HERE — the ray goes into layer space and the hit comes back out —\n"
            "because a caller doing that by hand gets a brush whose radius\n"
            "changes when the layer is scaled, and gets it wrong silently.\n\n"
            "Back faces are NOT culled: a sculptor working on the inside of a\n"
            "shell means it.\n\n"
            "The dict carries `seed_class`, which is what `stamp`'s `seed_class`\n"
            "wants: it starts the surface walk where the finger did, instead of\n"
            "scanning the whole mesh for the nearest vertex.")
        .def_prop_ro(
            "has_colors", [](PyMeshSculptor& s) { return s.live(false).has_colors(); },
            "Whether the mesh carries a vertex colour attribute, which 'paint'\n"
            "and 'smear' require.")
        .def(
            "ensure_colors",
            [](PyMeshSculptor& s, nb::handle color) {
                const kernel::cfloat3 fill =
                    color.is_none() ? kernel::cf3(1, 1, 1) : to_f3(color, "color");
                return s.live(true).ensure_colors(fill);
            },
            "color"_a = nb::none(),
            "Give every vertex `color` if the mesh has no colour attribute, and\n"
            "return whether one was created. A mesh that already has colours is\n"
            "left exactly as it is, so this is safe to call before every stroke.")
        .def(
            "refit", [](PyMeshSculptor& s) { s.live(false).refit_bvh(); },
            "Update the ray tree for the triangles the last stamp moved.\n\n"
            "THE PER-STAMP CALL. A mesh layer's topology is fixed, so a stamp\n"
            "leaves the tree's shape a valid partition of the same triangles and\n"
            "only its bounds stale. Refitting what the brush touched, and its\n"
            "ancestors, is proportional to the BRUSH: 0.021 ms on 130k triangles\n"
            "with an 800-triangle dab, against 34.9 ms to rebuild.")
        .def(
            "refresh", [](PyMeshSculptor& s) { s.live(false).refresh_bvh(); },
            "REBUILD the ray tree, which is proportional to the MESH — 1.3 s on a\n"
            "2M-vertex model against 0.25 ms for the stamp that dirtied it. Use it\n"
            "after something that moved most of the mesh, and `refit` per stamp.\n\n"
            "What a stale tree reports is worth stating, because the obvious guess\n"
            "is wrong and this docstring used to make it: it does NOT report the\n"
            "surface as it was when the tree was built. The hit follows the moved\n"
            "triangle but is found through stale bounds, so the reported position\n"
            "drifts OFF the ray — 4.4e-2 before an update, 1.5e-8 after. Invisible\n"
            "to a brush, which wants a depth; the whole error budget of a gizmo.")
        .def_prop_ro(
            "quality",
            [](PyMeshSculptor& s) {
                // Never builds. A property that silently paid for a full tree
                // build — 1.3 s on a 2M-vertex mesh, with the GIL held — would
                // be a trap dressed as a getter. No tree reads as 0.0.
                auto& sc = s.live(false);
                return sc.has_bvh() ? sc.bvh().quality() : 0.0f;
            },
            "What the ray tree's queries cost: the expected number of triangle\n"
            "tests a random ray must make. Lower is better, and it is normalised\n"
            "by the root box so it is comparable across meshes.\n\n"
            "A rise means queries are getting slower. It does NOT mean rebuild:\n"
            "measured over five deformations, a rebuild produced a better tree in\n"
            "exactly one of them, and was dramatically worse in two.")
        .def(
            "memory_ledger",
            [](const PyMeshSculptor& s) {
                memory::MemoryLedger ledger;
                mesh::report_surface_memory(s.live(false), &ledger);
                return ledger_dict(ledger);
            },
            "What this sculptor's surface costs, by category, in the same\n"
            "vocabulary the hierarchy and the adaptive surface answer in — so a\n"
            "host holding one of each gets one set of three roll-ups rather than\n"
            "three reports it has to reconcile.\n\n"
            "THE MESH AND NOTHING ELSE. The ray tree is built lazily, so asking\n"
            "a const report for its size would mean building one — 1.3 s on a 2M\n"
            "vertex mesh — which is a report nobody would call twice.");

    // -- layer -----------------------------------------------------------------------
    nb::class_<PyLayer>(m, "Layer", "SDF layer: an ordered edit list inside a Document")
        .def_prop_ro("id", [](const PyLayer& l) { return l.id; },
                     "The layer's id, which the document-level layer edits take")
        .def_prop_ro("name", [](const PyLayer& l) { return l.layer().name; })
        .def_prop_ro("resolution", [](const PyLayer& l) { return l.layer().resolution; })
        .def("add",
             [](PyLayer& l, const PyPrim& prim, scene::Op op, nb::handle blend, nb::handle color,
                nb::handle rounding, bool mirror, nb::handle transition, nb::handle parent,
                int index) {
                 if (op == scene::Op::None)
                     throw std::invalid_argument(
                         "op must be a combine operator, not Op.INLINE — that one is for "
                         "add_group");
                 scene::Node n;
                 n.prim = prim.prim;
                 n.xform = prim.xform;
                 n.scale_axes = prim.scale_axes;
                 n.stroke = prim.stroke;
                 n.stroke_blend_k = prim.stroke_blend_k;
                 n.armature_parents = prim.armature_parents;
                 n.armature_signs = prim.armature_signs;
                 n.stroke_closed = prim.stroke_closed;
                 n.curve_tolerance = prim.curve_tolerance;
                 n.deformers = prim.deformers;
                 n.profile = prim.profile;
                 n.profile_points = prim.profile_points;
                 n.profiles = prim.profiles;
                 n.profile_polygons = prim.profile_polygons;
                 n.volume = prim.volume;
                 n.gate = prim.gate;
                 n.gate_width = prim.gate_width;
                 n.repeat = prim.repeat;
                 n.op = op;
                 if (!blend.is_none()) {
                     try {
                         n.blend = nb::cast<const PyBlend&>(blend).b;
                     } catch (const std::exception&) {
                         throw std::invalid_argument(
                             "blend must be Smooth(k)/Cubic(k)/Circular(k)/Chamfer(k) or None");
                     }
                 }
                 if (!color.is_none()) n.color = parse_color(color);
                 // None means "whatever the prim decided", which is 0 for
                 // every prim that does not decide one.
                 n.rounding = rounding.is_none() ? prim.rounding : nb::cast<float>(rounding);
                 if (n.rounding < 0.0f) throw std::invalid_argument("rounding must be >= 0");
                 n.mirror = mirror;
                 if (scene::op_is_transition(op)) {
                     if (transition.is_none())
                         throw std::invalid_argument(
                             "transition ops need transition=clay.TransitionLinear(a, b) or "
                             "clay.TransitionRadial(r0, r1)");
                     const PyTransition& pt = nb::cast<const PyTransition&>(transition);
                     bool linear_params = op == scene::Op::TransitionLinear;
                     bool linear_object = nb::isinstance<PyTransitionLinear>(transition);
                     if (linear_params != linear_object)
                         throw std::invalid_argument(
                             "TRANSITION_LINEAR needs TransitionLinear parameters and "
                             "TRANSITION_RADIAL needs TransitionRadial parameters");
                     n.transition = pt.t;
                 } else if (!transition.is_none()) {
                     throw std::invalid_argument("transition= only applies to transition ops");
                 }
                 return insert_node(l, std::move(n), parent, index);
             },
             "prim"_a, "op"_a = scene::Op::Add, "blend"_a = nb::none(), "color"_a = nb::none(),
             "rounding"_a = nb::none(), "mirror"_a = true, "transition"_a = nb::none(),
             "parent"_a = nb::none(), "index"_a = -1,
             "Append an edit to the layer; returns the node id. parent=<group id> "
             "puts it inside that group instead of at the layer root, and index<0 "
             "appends. mirror=False keeps the item out of the layer's mirror "
             "(items follow it by default).")
        .def("add_group",
             [](PyLayer& l, scene::Op op, nb::handle blend, nb::handle color, float rounding,
                nb::handle parent, int index) {
                 scene::Node g;
                 g.is_group = true;
                 g.op = op;
                 if (!blend.is_none()) g.blend = nb::cast<const PyBlend&>(blend).b;
                 if (rounding < 0.0f) throw std::invalid_argument("rounding must be >= 0");
                 g.rounding = rounding;
                 if (!color.is_none()) g.color = parse_color(color);
                 check_group_op_blend(op, g.blend, g.rounding);
                 return insert_node(l, std::move(g), parent, index);
             },
             "op"_a = scene::Op::Add, "blend"_a = nb::none(), "color"_a = nb::none(),
             "rounding"_a = 0.0f, "parent"_a = nb::none(), "index"_a = -1,
             "Create an empty group and return its node id. Its children compile as "
             "ONE sub-expression, so an intersect inside it trims the group alone "
             "rather than everything the layer already holds — which is what makes "
             "(A & B) | C sayable. parent=<group id> nests it. Op.INLINE makes the "
             "children apply to the outer chain instead, and then reads no blend, "
             "rounding or colour off the group. color= is the seed a SHELL or "
             "REPLACE group paints when it starts a chain against empty space; in C "
             "it is a separate clay_layer_set_color.")
        .def("children",
             [](const PyLayer& l, scene::NodeId node) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
                 // Not a group is a caller asking the wrong question — and the
                 // answer a script that RELOADED a document reads to tell a
                 // group from an item.
                 if (!n->is_group) throw std::invalid_argument("node is not a group");
                 return n->children;
             },
             "node"_a, "A group's child node ids, in order")
        .def("set_points",
             [](PyLayer& l, scene::NodeId node, nb::handle points, nb::handle types, bool closed,
                float tolerance, nb::handle in_handles, nb::handle out_handles) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
                 if (!(tolerance > 0.0f)) throw std::invalid_argument("tolerance must be > 0");
                 std::vector<scene::StrokePoint> pts = to_stroke_points(points);
                 apply_point_types(pts, types);
                 apply_handles(pts, in_handles, out_handles);
                 // A sweep's guide is the same point list, so this edits one —
                 // but the two things Swept refuses at construction stay
                 // refused here. Closed: transporting a frame around a loop
                 // does not close the seam, and the leftover twist is real.
                 // Under two points: the sweep emits no tape record and simply
                 // vanishes, which no caller asked for.
                 if (scene::prim_is_swept(n->prim.type)) {
                     if (closed)
                         throw std::invalid_argument(
                             "a swept guide cannot be closed: transporting a frame around a loop "
                             "does not close the seam");
                     if (pts.size() < 2)
                         throw std::invalid_argument(
                             "a sweep needs a guide of two or more points");
                 }
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::SetStrokePointsCmd{
                                    l.id, node, std::move(pts), closed, tolerance}},
                                "set_points", l.undo.get());
             },
             "node"_a, "points"_a, "types"_a = nb::none(), "closed"_a = false,
             "tolerance"_a = 0.01f, "in_handles"_a = nb::none(), "out_handles"_a = nb::none(),
             "Replace a placed stroke or curve's whole point list. A curve is "
             "tens of points, so a whole-list replace costs less than granular "
             "commands would and its undo is exact by construction.")
        .def("armature_edit",
             [](PyLayer& l, scene::NodeId node, const std::string& op, nb::handle target,
                nb::handle value, float radius, bool mirrored, nb::handle sign) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
                 if (!scene::prim_is_armature(n->prim.type))
                     throw std::invalid_argument("that node is not an armature");
                 std::vector<scene::StrokePoint> nodes = n->stroke;
                 std::vector<std::uint32_t> parents = n->armature_parents;
                 std::vector<std::int8_t> signs = n->armature_signs;
                 const std::uint32_t which =
                     target.is_none() ? 0u : nb::cast<std::uint32_t>(target);
                 bool ok = false;
                 if (op == "add_child") {
                     kernel::cfloat3 pos = to_f3(value, "position");
                     ok = mirrored ? scene::armature_add_child_mirrored(nodes, parents, which,
                                                                       pos, radius) > 0
                                   : scene::armature_add_child(nodes, parents, which, pos, radius);
                 } else if (op == "move") {
                     ok = scene::armature_move(nodes, parents, which, to_f3(value, "delta"));
                 } else if (op == "set_radius") {
                     ok = scene::armature_set_radius(nodes, which, radius);
                 } else if (op == "delete") {
                     ok = scene::armature_delete_subtree(nodes, parents, signs, which);
                 } else if (op == "set_sign") {
                     const int s = sign.is_none() ? 1 : nb::cast<int>(sign);
                     if (s != 1 && s != -1)
                         throw std::invalid_argument("an armature sign must be +1 or -1");
                     ok = scene::armature_set_sign(signs, nodes.size(), which,
                                                   static_cast<std::int8_t>(s));
                 } else {
                     throw std::invalid_argument(
                         "op must be add_child, move, set_radius, set_sign or delete");
                 }
                 if (!ok) throw std::invalid_argument("that armature node does not exist");
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::SetArmatureCmd{
                                    l.id, node, std::move(nodes), std::move(parents),
                                    std::move(signs), n->stroke_blend_k}},
                                "armature_edit", l.undo.get());
             },
             "node"_a, "op"_a, "target"_a = nb::none(), "value"_a = nb::none(),
             "radius"_a = 0.1f, "mirrored"_a = false, "sign"_a = nb::none(),
             "Edit a placed armature's tree: add_child, move (which carries the "
             "target's whole subtree), set_radius, set_sign (+1 or -1 — a "
             "negative node carves instead of skinning), or delete (which takes "
             "the subtree with it). ONE undo step whatever the edit, because the "
             "command is a whole-tree replace — an armature is tens of nodes, so "
             "that costs less than granular bookkeeping and its inverse is the "
             "tree that was there. mirrored=True on add_child adds the "
             "reflection too, in the same step.")
        .def("apply_stroke",
             [](PyLayer& l, nb::handle samples, const brush::StrokePreset& preset,
                const PyPrim& prim, scene::Op op, nb::handle blend, nb::handle color,
                float rounding, nb::handle mask) {
                 scene::Node templ;
                 templ.prim = prim.prim;
                 templ.stroke = prim.stroke;
                 templ.stroke_blend_k = prim.stroke_blend_k;
                 templ.stroke_closed = prim.stroke_closed;
                 templ.curve_tolerance = prim.curve_tolerance;
                 templ.deformers = prim.deformers;
                 templ.repeat = prim.repeat;
                 templ.profile = prim.profile;
                 templ.profile_points = prim.profile_points;
                 templ.profiles = prim.profiles;
                 templ.profile_polygons = prim.profile_polygons;
                 templ.volume = prim.volume;
                 templ.gate = prim.gate;
                 templ.gate_width = prim.gate_width;
                 templ.op = op;
                 // Rounding is not decoration for every op: groove and tongue read
                 // it as the channel half-width, and relief and incise as the
                 // FALLOFF WIDTH. Dropping it left those strokes declaring an
                 // amplitude over ~1e-6, so the step scale collapsed to zero and
                 // the geometry could not be marched at all.
                 templ.rounding = rounding > 0.0f ? rounding : prim.rounding;
                 if (!blend.is_none()) templ.blend = nb::cast<PyBlend&>(blend).b;
                 if (!color.is_none()) templ.color = parse_color(color);

                 std::vector<brush::Stamp> stamps =
                     brush::resolve_stroke(to_stroke_samples(samples), preset);
                 std::vector<scene::Node> nodes = brush::stamps_to_nodes(
                     *l.layer().sdf, stamps, templ, borrow_mask(mask));

                 // One AddNodeCmd per stamp, bundled into a single undo group:
                 // a stroke is one step to undo, and every node in it is an
                 // ordinary edit that serialization and picking already know.
                 UndoRef undo = l.undo ? *l.undo : UndoRef();
                 if (undo) undo->begin_group();
                 std::vector<scene::NodeId> ids;
                 ids.reserve(nodes.size());
                 for (scene::Node& n : nodes) {
                     scene::NodeId id = n.id;
                     std::vector<scene::Node> subtree;
                     subtree.push_back(std::move(n));
                     apply_or_throw(l.doc->document,
                                    scene::Command{scene::AddNodeCmd{l.id, scene::kNoNode, -1,
                                                                     std::move(subtree)}},
                                    "apply_stroke", l.undo.get());
                     ids.push_back(id);
                 }
                 if (undo) undo->end_group();
                 return ids;
             },
             "samples"_a, "preset"_a, "prim"_a, "op"_a = scene::Op::Add, "blend"_a = nb::none(),
             "color"_a = nb::none(), "rounding"_a = 0.0f, "mask"_a = nb::none(),
             "Resolve a stroke and append one edit per stamp; returns their node "
             "ids. The prim is the stamp template, scaled to each stamp's radius. "
             "The whole stroke is one undo step, and a masked stamp emits nothing.")
        .def("lattice_gizmo",
             [](PyLayer& l, nb::handle placement_position, nb::handle placement_axis,
                float placement_angle, float placement_scale, nb::handle box,
                nb::handle offsets, int nx, int ny, int nz, bool preview) {
                 brush::GizmoCage cage;
                 // None is "not placed" rather than an error: a cage at the
                 // origin with no rotation is the common case, and making a
                 // caller spell it out would be noise.
                 if (!placement_position.is_none())
                     cage.placement.position = to_f3(placement_position, "position");
                 if (!placement_axis.is_none()) {
                     const kernel::cfloat3 axis = to_f3(placement_axis, "axis");
                     if (kernel::clength(axis) > 1e-9f)
                         cage.placement.rotation =
                             math::Quat::from_axis_angle(axis, placement_angle);
                 }
                 if (!(placement_scale > 0.0f))
                     throw std::invalid_argument("the cage's scale must be > 0");
                 cage.placement.scale = placement_scale;
                 const math::Aabb b = to_aabb(box);
                 if (b.empty())
                     throw std::invalid_argument("the cage's box is empty; there is nothing to span");
                 cage.box_min = b.min;
                 cage.box_max = b.max;
                 const int cap = scene::Deformer::kMaxLatticeDivisions;
                 if (nx < 2 || ny < 2 || nz < 2 || nx > cap || ny > cap || nz > cap)
                     throw std::invalid_argument(
                         "lattice divisions must be in [2, " + std::to_string(cap) +
                         "] per axis: the cage is evaluated per sample");
                 cage.nx = nx;
                 cage.ny = ny;
                 cage.nz = nz;
                 cage.offsets.assign(cage.point_count(), kernel::cf3(0, 0, 0));
                 if (!offsets.is_none()) {
                     PointsView v = to_points(offsets);
                     if (v.count != cage.offsets.size())
                         throw std::invalid_argument(
                             "offsets must have one entry per control point (" +
                             std::to_string(cage.offsets.size()) + ")");
                     for (std::size_t i = 0; i < cage.offsets.size(); ++i)
                         cage.offsets[i] = kernel::cf3(v.data[i * 3], v.data[i * 3 + 1],
                                                       v.data[i * 3 + 2]);
                 }

                 const std::vector<brush::LatticeWarp> warps =
                     brush::lattice_gizmo(l.layer(), cage);
                 std::vector<scene::NodeId> touched;
                 if (preview) {
                     for (const brush::LatticeWarp& w : warps) touched.push_back(w.node);
                     return touched;
                 }
                 // One undo group for the whole cage: it is one gesture.
                 UndoRef undo = l.undo ? *l.undo : UndoRef();
                 if (undo) undo->begin_group();
                 for (const brush::LatticeWarp& w : warps) {
                     const scene::Node* n = l.layer().sdf->find(w.node);
                     if (!n) continue;
                     apply_or_throw(l.doc->document,
                                    scene::Command{scene::SetDeformersCmd{
                                        l.id, w.node, brush::caged_chain(*n, w)}},
                                    "lattice_gizmo", l.undo.get());
                     touched.push_back(w.node);
                 }
                 if (undo) undo->end_group();
                 return touched;
             },
             "position"_a = nb::none(), "axis"_a = nb::none(), "angle"_a = 0.0f,
             "scale"_a = 1.0f, "box"_a = nb::none(), "offsets"_a = nb::none(), "nx"_a = 3,
             "ny"_a = 3, "nz"_a = 3, "preview"_a = false,
             "One CAGE over this layer — ZBrush's Gizmo Lattice, which acts on the\n"
             "whole subtool rather than on one item in its own frame.\n\n"
             "The cage is placed in the WORLD by position/axis/angle/scale and\n"
             "spans `box` in its own space. `offsets` is an (nx*ny*nz, 3) array of\n"
             "control-point drags in that space, x-fastest — index (i, j, k) at\n"
             "(k*ny + j)*nx + i.\n\n"
             "Resolved into one lattice deformer per item, each carrying the\n"
             "transform that takes that item's frame into the cage's. That is what\n"
             "makes it exact for a ROTATED item: a lattice box is axis-aligned by\n"
             "construction, so no per-item box reproduces a world-placed cage.\n\n"
             "Reaches EVERY item, unlike `move_surface`. A lattice's displacement\n"
             "outside its box is CLAMPED rather than zero, so material out there\n"
             "travels rigidly — skipping distant items would tear the form.\n\n"
             "`preview=True` reports which nodes it WOULD warp without touching the\n"
             "document. Returns the nodes that took a warp; the whole cage is one\n"
             "undo step. An untouched cage does nothing and returns nothing.")
        .def("move_surface_preview",
             [](PyLayer& l, nb::handle centre, nb::handle displacement, float radius,
                int ease, bool front_only) {
                 if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
                 brush::MoveSettings settings;
                 settings.radius = radius;
                 settings.ease = static_cast<std::uint8_t>(ease);
                 settings.front_only = front_only;
                 std::vector<scene::NodeId> nodes;
                 for (const brush::MoveWarp& w :
                      brush::move_brush(l.layer(), to_f3(centre, "centre"),
                                        to_f3(displacement, "displacement"), settings))
                     nodes.push_back(w.node);
                 return nodes;
             },
             "centre"_a, "displacement"_a, "radius"_a = 0.3f, "ease"_a = 0,
             "front_only"_a = false,
             "Which nodes a move WOULD warp, without touching the document — so a\n"
             "host can preview a drag, or show what it is about to affect, before\n"
             "committing it. Resolving is pure; applying is what changes things.")
        .def("move_surface",
             [](PyLayer& l, nb::handle centre, nb::handle displacement, float radius,
                int ease, bool front_only) {
                 if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
                 brush::MoveSettings settings;
                 settings.radius = radius;
                 settings.ease = static_cast<std::uint8_t>(ease);
                 settings.front_only = front_only;

                 const std::vector<brush::MoveWarp> warps =
                     brush::move_brush(l.layer(), to_f3(centre, "centre"),
                                       to_f3(displacement, "displacement"), settings);

                 // One undo group for the whole drag: it is one gesture.
                 UndoRef undo = l.undo ? *l.undo : UndoRef();
                 if (undo) undo->begin_group();
                 std::vector<scene::NodeId> touched;
                 for (const brush::MoveWarp& w : warps) {
                     const scene::Node* n = l.layer().sdf->find(w.node);
                     if (!n) continue;
                     apply_or_throw(l.doc->document,
                                    scene::Command{scene::SetDeformersCmd{
                                        l.id, w.node, brush::moved_chain(*n, w)}},
                                    "move_surface", l.undo.get());
                     touched.push_back(w.node);
                 }
                 if (undo) undo->end_group();
                 return touched;
             },
             "centre"_a, "displacement"_a, "radius"_a = 0.3f, "ease"_a = 0,
             "front_only"_a = false,
             "Drag this layer's assembled SURFACE — ZBrush's Move. Returns the\n"
             "nodes that took a warp.\n\n"
             "NOT the same as `prim.grab(...)`, and the difference is the reason\n"
             "this exists. A deformer is per ITEM and its centre is in that\n"
             "item's LOCAL frame, so a grab drags one item's own field: on a form\n"
             "blended from several, it pulls that item's share and leaves the\n"
             "rest behind. Nothing errors — it just looks wrong. This resolves\n"
             "the drag against every item the region reaches, maps it into each\n"
             "one's frame, and puts it at the FRONT of each chain, which is where\n"
             "a warp has to go to act on the assembled shape.\n\n"
             "The whole drag is ONE undo step however many items it touched.\n\n"
             "The surface moves LESS than the displacement you ask for: the\n"
             "region weight is taken at the sample point rather than at its\n"
             "preimage, so a drag of 0.5 over a radius of 0.8 moves a tip about\n"
             "0.31. That is `grab`'s deliberate behaviour — the true preimage\n"
             "costs an iteration per sample and buys nothing a sculptor can feel\n"
             "— and the pull is monotonic, so a UI can calibrate against it.")
        .def("magnify_surface_preview",
             [](PyLayer& l, nb::handle centre, float strength, float radius, int ease) {
                 if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
                 brush::MagnifySettings settings;
                 settings.radius = radius;
                 settings.ease = static_cast<std::uint8_t>(ease);
                 std::vector<scene::NodeId> nodes;
                 for (const brush::MoveWarp& w : brush::magnify_brush(
                          l.layer(), to_f3(centre, "centre"), strength, settings))
                     nodes.push_back(w.node);
                 return nodes;
             },
             "centre"_a, "strength"_a, "radius"_a = 0.3f, "ease"_a = 0,
             "Which nodes a magnify WOULD warp, without touching the document —\n"
             "so a host can preview the gesture, or show what it is about to\n"
             "affect, before committing it. Resolving is pure; applying is what\n"
             "changes things.")
        .def("magnify_surface",
             [](PyLayer& l, nb::handle centre, float strength, float radius, int ease) {
                 if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
                 if (strength == 0.0f)
                     throw std::invalid_argument(
                         "strength must be non-zero; positive swells, negative gathers");
                 brush::MagnifySettings settings;
                 settings.radius = radius;
                 settings.ease = static_cast<std::uint8_t>(ease);

                 const std::vector<brush::MoveWarp> warps =
                     brush::magnify_brush(l.layer(), to_f3(centre, "centre"), strength, settings);

                 // One undo group for the whole gesture, as a drag takes.
                 UndoRef undo = l.undo ? *l.undo : UndoRef();
                 if (undo) undo->begin_group();
                 std::vector<scene::NodeId> touched;
                 for (const brush::MoveWarp& w : warps) {
                     const scene::Node* n = l.layer().sdf->find(w.node);
                     if (!n) continue;
                     apply_or_throw(l.doc->document,
                                    scene::Command{scene::SetDeformersCmd{
                                        l.id, w.node, brush::moved_chain(*n, w)}},
                                    "magnify_surface", l.undo.get());
                     touched.push_back(w.node);
                 }
                 if (undo) undo->end_group();
                 return touched;
             },
             "centre"_a, "strength"_a, "radius"_a = 0.3f, "ease"_a = 0,
             "Magnify or PINCH this layer's assembled SURFACE — the radial-scale\n"
             "counterpart to `move_surface`. Returns the nodes that took a warp.\n\n"
             "NOT the same as `prim.magnify(...)`, and the difference is the\n"
             "reason this exists. A deformer is per ITEM and its centre is in\n"
             "that item's LOCAL frame, so a magnify scales one item's own field:\n"
             "on a form blended from several, it gathers that item's share and\n"
             "leaves the rest behind. Nothing errors — it just looks wrong. This\n"
             "resolves the gesture against every item the region reaches, maps it\n"
             "into each one's frame, and puts it at the FRONT of each chain.\n\n"
             "`strength` is SIGNED and one parameter covers both verbs: POSITIVE\n"
             "swells the surface away from the centre (Magnify), NEGATIVE gathers\n"
             "it toward (Pinch). They are one deformation.\n\n"
             "TOTAL, from the start of the gesture, never an increment on the\n"
             "last frame — a live gesture calls this every frame with a growing\n"
             "strength and the engine replaces its own last frame rather than\n"
             "stacking a deformer per frame.\n\n"
             "The whole gesture is ONE undo step however many items it touched.")
        .def("set_transform",
             [](PyLayer& l, scene::NodeId node, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
                 // A group's transform never reaches its children — the
                 // compiler composes layer * item and nothing else — so this
                 // would be an undoable, saved edit that changes nothing.
                 if (n->is_group)
                     throw std::invalid_argument(
                         "a group has no transform of its own: transform its children");
                 // The per-axis scale is seeded from the node, not defaulted:
                 // these bindings take PARTIAL updates, so a call that says
                 // nothing about scale must leave a squash alone. The C ABI
                 // takes the whole transform and collapses it instead, which
                 // is the difference between the two surfaces, not an accident.
                 scene::SetTransformCmd cmd{l.id, node, n->xform, n->scale_axes};
                 if (!position.is_none()) cmd.xform.position = to_f3(position, "position");
                 if (!rotation_axis_angle.is_none())
                     cmd.xform.rotation = to_axis_angle(rotation_axis_angle);
                 if (!scale.is_none()) apply_scale(cmd.xform.scale, cmd.scale_axes, scale);
                 apply_or_throw(l.doc->document, scene::Command{cmd}, "set_transform", l.undo.get());
             },
             "node"_a, "position"_a = nb::none(), "rotation_axis_angle"_a = nb::none(),
             "scale"_a = nb::none(),
             "Retransform a placed node; omitted arguments keep their current "
             "value. scale= takes one number for a uniform scale or "
             "(sx, sy, sz) for a per-axis one — which is how a slot, an oval "
             "hole or a stretched chamfer is said. A per-axis scale costs no "
             "step size (the field stays 1-Lipschitz, so safe_step_scale does "
             "not move); what it costs is exactness — the value becomes a "
             "BOUND on the distance rather than the distance.")
        .def("set_prim",
             [](PyLayer& l, scene::NodeId node, const PyPrim& prim) {
                 // Only the primitive is replaced: the node's deformers,
                 // repetition, profile and stroke are its own, not the
                 // builder's, and survive the edit.
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::SetPrimCmd{l.id, node, prim.prim}},
                                "set_prim", l.undo.get());
             },
             "node"_a, "prim"_a,
             "Replace a node's primitive, keeping its deformers, repetition and profile")
        .def("set_color",
             [](PyLayer& l, scene::NodeId node, nb::handle color) {
                 apply_or_throw(
                     l.doc->document,
                     scene::Command{scene::SetColorCmd{l.id, node, parse_color(color)}},
                     "set_color", l.undo.get());
             },
             "node"_a, "color"_a)
        .def("set_op_blend",
             [](PyLayer& l, scene::NodeId node, nb::handle op, nb::handle blend,
                nb::handle rounding) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
                 scene::SetOpBlendCmd cmd{l.id, node, n->op, n->blend, n->rounding};
                 if (!op.is_none()) {
                     cmd.op = nb::cast<scene::Op>(op);
                     // Which rules apply is a property of the node: a group
                     // takes the inline op and refuses the transitions, an item
                     // is the other way round.
                     if (!n->is_group && cmd.op == scene::Op::None)
                         throw std::invalid_argument(
                             "op must be a combine operator, not Op.INLINE — that one is for "
                             "add_group");
                 }
                 if (!blend.is_none()) cmd.blend = nb::cast<const PyBlend&>(blend).b;
                 if (!rounding.is_none()) {
                     cmd.rounding = nb::cast<float>(rounding);
                     if (cmd.rounding < 0.0f)
                         throw std::invalid_argument("rounding must be >= 0");
                 }
                 if (n->is_group) check_group_op_blend(cmd.op, cmd.blend, cmd.rounding);
                 apply_or_throw(l.doc->document, scene::Command{cmd}, "set_op_blend", l.undo.get());
             },
             "node"_a, "op"_a = nb::none(), "blend"_a = nb::none(), "rounding"_a = nb::none(),
             "Change how a placed node combines; omitted arguments keep their value")
        .def("move",
             [](PyLayer& l, scene::NodeId node, nb::handle parent, int index) {
                 scene::NodeId new_parent =
                     parent.is_none() ? scene::kNoNode : nb::cast<scene::NodeId>(parent);
                 // SdfContent::move refuses this too — it is the engine's
                 // invariant, not the binding's — but apply() could only report
                 // it as a missing id, which is not what went wrong.
                 if (new_parent != scene::kNoNode && l.layer().sdf->contains(node, new_parent))
                     throw std::invalid_argument("a node cannot move into its own subtree");
                 apply_or_throw(
                     l.doc->document,
                     scene::Command{scene::MoveNodeCmd{l.id, node, new_parent, index}}, "move",
                     l.undo.get());
             },
             "node"_a, "parent"_a = nb::none(), "index"_a = -1,
             "Reparent or reorder a node; parent=None moves it to the layer root")
        .def("remove",
             [](PyLayer& l, scene::NodeId node) {
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::RemoveNodeCmd{l.id, node}}, "remove",
                                l.undo.get());
             },
             "node"_a, "Remove a node and its subtree")
        .def("append_stroke",
             [](PyLayer& l, scene::NodeId node, nb::handle points) {
                 scene::AppendStrokeCmd cmd{l.id, node, to_stroke_points(points)};
                 if (cmd.points.empty())
                     throw std::invalid_argument("append_stroke needs at least one point");
                 apply_or_throw(l.doc->document, scene::Command{cmd}, "append_stroke", l.undo.get());
             },
             "node"_a, "points"_a,
             "Append (x, y, z, radius) points to a placed stroke")
        .def("trim_stroke",
             [](PyLayer& l, scene::NodeId node, std::uint32_t count) {
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::TrimStrokeCmd{l.id, node, count}},
                                "trim_stroke", l.undo.get());
             },
             "node"_a, "count"_a, "Remove the last count points from a placed stroke")
        .def("mirror",
             [](PyLayer& l, const std::string& axis, float blend) {
                 if (blend < 0.0f) throw std::invalid_argument("blend must be >= 0");
                 scene::Layer& layer = l.layer();
                 layer.mirror_axes |= static_cast<std::uint8_t>(1u << parse_axis(axis));
                 layer.mirror_k = blend;
             },
             "axis"_a = "x", "blend"_a = 0.0f,
             "Enable the layer mirror across an axis: every item reflects, before or "
             "after it was added, except those added with mirror=False")
        .def("radial",
             [](PyLayer& l, int count, const std::string& axis, float blend) {
                 if (blend < 0.0f) throw std::invalid_argument("blend must be >= 0");
                 if (count < 0 || count > 65535)
                     throw std::invalid_argument("radial count out of range");
                 // Through the command vocabulary, so it respects the lock and
                 // lands on the undo stack. `mirror` above still writes the
                 // fields directly, which is the defect clay_set_layer_mirror
                 // was changed to fix on the C side.
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::SetLayerRadialCmd{
                                    l.id, static_cast<std::uint16_t>(count),
                                    static_cast<std::uint8_t>(parse_axis(axis)), blend}},
                                "radial", l.undo.get());
             },
             "count"_a, "axis"_a = "y", "blend"_a = 0.0f,
             "Array every participating item `count` times about the layer-local axis. "
             "0 or 1 turns it off. Participation follows the same per-item flag as the "
             "mirror, so a stroke arrays and an item added with mirror=False does not.")
        .def("eval",
             [](const PyLayer& l, nb::handle points, const std::string& backend) {
                 return eval_field(scene::compile_layer(l.layer()), points, backend,
                                   Want::Distances);
             },
             "points"_a, "backend"_a = "cpu",
             "Evaluate signed distances at (N, 3) points -> (N,) float32")
        .def("gradients",
             [](const PyLayer& l, nb::handle points, const std::string& backend) {
                 return eval_field(scene::compile_layer(l.layer()), points, backend,
                                   Want::Gradients);
             },
             "points"_a, "backend"_a = "cpu",
             "Normalized field gradients (tetrahedron trick) at (N, 3) points -> (N, 3)")
        .def("colors",
             [](const PyLayer& l, nb::handle points, const std::string& backend) {
                 return eval_field(scene::compile_layer(l.layer()), points, backend,
                                   Want::Colors);
             },
             "points"_a, "backend"_a = "cpu", "Field colors at (N, 3) points -> (N, 3)")
        .def("bounds",
             [](const PyLayer& l) -> nb::object {
                 math::Aabb b = pick::layer_bounds(l.layer());
                 if (b.empty()) return nb::none();
                 return nb::make_tuple(nb::make_tuple(b.min.x, b.min.y, b.min.z),
                                       nb::make_tuple(b.max.x, b.max.y, b.max.z));
             },
             "Tight world-space bounds of the layer's shapes (no blend dilation)")
        .def("selection_bounds",
             [](const PyLayer& l, const std::vector<scene::NodeId>& nodes) -> nb::object {
                 math::Aabb b = pick::selection_bounds(l.doc->document, l.id, nodes);
                 if (b.empty()) return nb::none();
                 return nb::make_tuple(nb::make_tuple(b.min.x, b.min.y, b.min.z),
                                       nb::make_tuple(b.max.x, b.max.y, b.max.z));
             },
             "nodes"_a, "Tight bounds of the given node ids — for zoom-to-selection")
        .def("safe_step_scale", [](const PyLayer& l) {
            return scene::compile_layer(l.layer()).safe_step_scale();
        })
        .def("field_report",
             [](const PyLayer& l, float advise_below_step_scale) {
                 const scene::FieldReport r =
                     scene::report_layer(l.layer(), advise_below_step_scale);
                 nb::dict out;
                 out["lipschitz"] = r.lipschitz;
                 out["safe_step_scale"] = r.safe_step_scale;
                 out["steepest_volume"] = r.steepest_volume;
                 out["longest_deformer_chain"] = r.longest_deformer_chain;
                 out["steepest_deformer_chain"] = r.steepest_deformer_chain;
                 out["item_count"] = r.item_count;
                 out["drawable_count"] = r.drawable_count;
                 out["advises_consolidation"] = r.advises_consolidation;
                 out["degradation"] = degradation_name(r.degradation);
                 return out;
             },
             "advise_below_step_scale"_a = 0.0f,
             "What this layer's chain costs the marcher, and what is causing it.\n\n"
             "The region verbs each work once and none of them chain, for TWO\n"
             "different reasons. A polish samples a document and hands back a\n"
             "volume, so the second pass samples a VOLUME — `steepest_volume`\n"
             "is that. A move stroke never touches a volume at all: each drag\n"
             "appends a grab to the deformer chain and those multiply —\n"
             "`longest_deformer_chain` is that — a COUNT, so weigh the\n"
             "mechanisms by `steepest_volume` against `steepest_deformer_chain`,\n"
             "which are both factors. The aggregate step scale says something is\n"
             "wrong; those two say which thing.\n\n"
             "`degradation` names the mechanism outright: \"none\", \"volumes\",\n"
             "\"deformers\" or \"both\". Read it before acting, because the two\n"
             "cures are not interchangeable — `advises_consolidation` is now\n"
             "keyed on it and is FALSE for a layer degraded only by its brush\n"
             "chain, where a bake swaps a cheap analytic item for a dense volume\n"
             "and measured 6x WORSE on a real gesture.\n\n"
             "`advise_below_step_scale` is YOUR tolerance for marching cost, and\n"
             "it is an argument rather than document state because that\n"
             "tolerance belongs to a viewport and a frame budget rather than to\n"
             "the artwork. Nothing here bakes: consolidating discards the\n"
             "parameters of what it absorbs, so it is never done unasked.")
        .def("consolidation_cost",
             [](const PyLayer& l, float cell, nb::handle band, nb::handle padding,
                nb::handle region, bool redistance) {
                 scene::ConsolidationCost cost;
                 scene::ConsolidationParams p =
                     to_consolidation(cell, band, padding, region, redistance);
                 if (!scene::bake_layer(l.layer(), p, &cost, eval::pooled_bake_eval()))
                     throw std::invalid_argument(
                         "nothing to consolidate: the layer is empty, unbounded, or the region "
                         "contains no surface");
                 return cost_dict(cost);
             },
             "cell"_a, "band"_a = nb::none(), "padding"_a = nb::none(), "region"_a = nb::none(),
             "redistance"_a = true,
             "What consolidating this layer WOULD cost, without changing it.\n\n"
             "The numbers are the ones the real thing produces, because this IS\n"
             "the real thing with the result thrown away — an estimate that\n"
             "skipped the sampling could not report a brick count, and the brick\n"
             "count is where the memory is. If you mean to go ahead, call\n"
             "`consolidate` and read the same report out of it rather than\n"
             "paying for two bakes.")
        .def("plan_region_merge",
             [](const PyLayer& l, nb::handle region) {
                 const scene::RegionMerge plan =
                     scene::plan_region_merge(l.layer(), to_aabb(region));
                 nb::dict out;
                 out["absorbed"] = plan.absorb.size();
                 out["whole_layer"] = plan.whole_layer;
                 if (plan.box.empty()) {
                     out["box"] = nb::none();
                 } else {
                     out["box"] = nb::make_tuple(
                         nb::make_tuple(plan.box.min.x, plan.box.min.y, plan.box.min.z),
                         nb::make_tuple(plan.box.max.x, plan.box.max.y, plan.box.max.z));
                 }
                 return out;
             },
             "region"_a,
             "What `consolidate_region` WOULD absorb, and over what box, without\n"
             "baking anything — so you can show the region whose parameters are\n"
             "about to be lost before committing to it.\n\n"
             "`box` is the INFLUENCE CLOSURE of your region, not the region: it\n"
             "grows until every item that can reach inside it is wholly inside\n"
             "it. `whole_layer` says the closure took everything, in which case\n"
             "this is `consolidate()` and the promise to leave items outside\n"
             "parametric is vacuous.")
        .def("consolidate_region",
             [](PyLayer& l, nb::handle region, float cell, nb::handle band, nb::handle padding,
                bool redistance) {
                 if (l.layer().protected_from_edits())
                     throw std::invalid_argument("layer is protected (ghosted or locked)");
                 scene::ConsolidationParams p =
                     to_consolidation(cell, band, padding, nb::none(), redistance);
                 scene::ConsolidationCost cost;
                 scene::RegionMerge plan;
                 session::History* hist = (l.undo && *l.undo) ? l.undo->get() : nullptr;
                 if (!scene::consolidate_region(l.doc->document, l.id, to_aabb(region), p,
                                                hist ? hist->commands() : nullptr, &cost,
                                                eval::pooled_bake_eval(), nullptr, nullptr,
                                                &plan)) {
                     throw std::invalid_argument(
                         "nothing to merge: the layer is empty or protected, or the region "
                         "reaches no item");
                 }
                 if (hist) hist->sync_scene_steps();
                 nb::dict out = cost_dict(cost);
                 out["absorbed"] = plan.absorb.size();
                 out["whole_layer"] = plan.whole_layer;
                 return out;
             },
             "region"_a, "cell"_a, "band"_a = nb::none(), "padding"_a = nb::none(),
             "redistance"_a = true,
             "Bake a REGION of this layer into one volume and put it back where\n"
             "the items it absorbed were, leaving everything outside parametric.\n\n"
             "`consolidate()` collapses the whole layer, which is right for a\n"
             "chain that has genuinely degraded and wrong for what a sculptor\n"
             "does, which is work a PATCH. Applying a region bake per gesture,\n"
             "you could only append a volume each time — so every later bake\n"
             "sampled all the earlier ones — or collapse the whole subtool and\n"
             "lose the parameters of items nowhere near the stroke.\n\n"
             "`region` says where you worked. What gets SAMPLED is its INFLUENCE\n"
             "CLOSURE: the region grown until every item that can reach inside it\n"
             "is wholly inside it. That is not the same as the items overlapping\n"
             "the region, and the difference is not cosmetic — absorb only those\n"
             "and a subtract straddling the edge stays behind, the material it\n"
             "carved comes back, and the volume cannot take it away again.\n\n"
             "The second gesture on a patch has the first gesture's volume in its\n"
             "closure, so it is absorbed rather than stacked on: a patch stays at\n"
             "ONE baked item however many times it is worked.\n\n"
             "ONE undo step, as `consolidate()` is. Use `plan_region_merge()`\n"
             "first to see what it would take.")
        .def("consolidate",
             [](PyLayer& l, float cell, nb::handle band, nb::handle padding, nb::handle region,
                bool redistance, parallel::CancelToken* token) {
                 scene::ConsolidationCost cost;
                 scene::ConsolidationParams p =
                     to_consolidation(cell, band, padding, region, redistance);
                 if (l.layer().protected_from_edits())
                     throw std::invalid_argument("layer is protected (ghosted or locked)");
                 // Consolidate performs commands through the stack directly, so the
                 // session is told afterwards how many entries appeared. It IS
                 // undoable — worth saying, because it is the operation most
                 // often assumed not to be.
                 session::History* hist = (l.undo && *l.undo) ? l.undo->get() : nullptr;
                 bool cancelled = false;
                 if (!scene::consolidate_layer(l.doc->document, l.id, p,
                                               hist ? hist->commands() : nullptr, &cost,
                                               eval::pooled_bake_eval(), token, &cancelled)) {
                     // A cancel and "nothing to consolidate" both fail, and a
                     // caller must not be shown the second when they did the
                     // first — so they are different exceptions.
                     if (cancelled) throw std::runtime_error("the consolidate was cancelled");
                     throw std::invalid_argument(
                         "nothing to consolidate: the layer is empty, unbounded, or the region "
                         "contains no surface");
                 }
                 if (hist) hist->sync_scene_steps();
                 return cost_dict(cost);
             },
             "cell"_a, "band"_a = nb::none(), "padding"_a = nb::none(), "region"_a = nb::none(),
             "redistance"_a = true, "token"_a = nb::none(),
             "Collapse this layer's edit list into one item carrying samples,\n"
             "and report what it cost.\n\n"
             "ONE undo step, whose inverse restores what it absorbed with ids,\n"
             "parameters, colours and deformers intact — the undo record carries\n"
             "the removed subtrees by value, so no new command was needed for\n"
             "this. What survives is the surface at `cell`; what does not is\n"
             "every parameter of every item absorbed, and every colour but the\n"
             "first one's. Hidden items are left alone: they contribute nothing\n"
             "to the field, so absorbing them would spend their parameters on\n"
             "nothing.\n\n"
             "`redistance=True` is what actually bounds the Lipschitz. Baking\n"
             "alone does NOT — resampling a steep field reproduces the\n"
             "steepness, and a finer cell makes it worse rather than better.\n"
             "Turn it off only to measure that.\n\n"
             "Pin `region` when consolidating the same area repeatedly: a\n"
             "volume's geometric bound is its whole sampled box, so each bake\n"
             "would otherwise pad the previous padding.")
        .def_prop_ro("consolidation_state",
                     [](const PyLayer& l) -> nb::object {
                         scene::ConsolidationCost cost;
                         if (!scene::consolidation_state(l.layer(), &cost)) return nb::none();
                         return cost_dict(cost);
                     },
                     "The resolution this layer is baked at, or None if it is still\n"
                     "parametric — so a host can stop offering parameter edits there\n"
                     "rather than failing them.\n\n"
                     "Answered from the CONTENT rather than a stored provenance flag: a\n"
                     "mesh imported as a volume is exactly as unparametric as a bake, so\n"
                     "a flag marking one of them would split two cases an app has to\n"
                     "treat alike — and it would have to be serialised to survive a save.");

    // -- document ----------------------------------------------------------------------
    nb::class_<PyDocument>(m, "Document", "A claycore document: a stack of layers")
        .def(nb::init<>())
        .def("add_sdf_layer",
             [](PyDocument& d, const std::string& name, int resolution) {
                 if (resolution <= 0) throw std::invalid_argument("resolution must be > 0");
                 // Through AddLayerCmd so the add is undoable; see Layer.add.
                 scene::Layer l;
                 l.id = d.doc->document.reserve_layer_id();
                 l.name = name;
                 l.sdf = std::make_shared<scene::SdfContent>();
                 l.resolution = resolution;
                 scene::LayerId id = l.id;
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::AddLayerCmd{std::move(l), -1}},
                                "add_sdf_layer", d.undo.get());
                 return PyLayer{d.doc, d.undo, id};
             },
             "name"_a, "resolution"_a = 256)
        .def("eval",
             [](const PyDocument& d, nb::handle points, const std::string& backend) {
                 return eval_field(scene::compile_document(d.doc->document), points, backend,
                                   Want::Distances);
             },
             "points"_a, "backend"_a = "cpu",
             "Evaluate signed distances at (N, 3) points -> (N,) float32")
        .def("gradients",
             [](const PyDocument& d, nb::handle points, const std::string& backend) {
                 return eval_field(scene::compile_document(d.doc->document), points, backend,
                                   Want::Gradients);
             },
             "points"_a, "backend"_a = "cpu",
             "Normalized field gradients (tetrahedron trick) at (N, 3) points -> (N, 3)")
        .def("colors",
             [](const PyDocument& d, nb::handle points, const std::string& backend) {
                 return eval_field(scene::compile_document(d.doc->document), points, backend,
                                   Want::Colors);
             },
             "points"_a, "backend"_a = "cpu", "Field colors at (N, 3) points -> (N, 3)")
        .def("safe_step_scale",
             [](const PyDocument& d) {
                 return scene::compile_document(d.doc->document).safe_step_scale();
             },
             "Multiply distances by this before sphere-trace stepping (Lipschitz safety)")
        .def("mesh", &mesh_document, "resolution"_a = 128, "voxel_size"_a = nb::none(),
             "decimate"_a = nb::none(), "backend"_a = "cpu", "mesher"_a = "marching",
             "experimental"_a = false,
             "Mesh the document (marching over its influence bounds) -> Mesh")
        .def("mesh_quads", &mesh_document_quads, "cell_size"_a = nb::none(),
             "target"_a = nb::none(), "tolerance"_a = 0.10f, "max_iterations"_a = 4,
             "mode"_a = "dual",
             "Mesh the document as QUADS (lattice dual) -> Mesh.\n\n"
             "WHAT THIS IS NOT: a REGULAR QUAD GRID DERIVED FROM A SAMPLING LATTICE,\n"
             "which is NOT field-aligned retopology. The quads follow the lattice and\n"
             "not the form — no edge loops around a limb or a mouth, no poles at\n"
             "features, density does not follow curvature, and the result is NOT\n"
             "animation-ready. This is the input a retopology pass REPLACES, not the\n"
             "output one produces. For ZRemesher-style topology, use a quad remesher.\n\n"
             "What it IS good for: quads in a DCC that prefers them, subdividing a\n"
             "sculpt, and exporting as .obj/.ply/.fbx with four-corner faces (.glb\n"
             "stays triangles — glTF 2.0 has no quad primitive mode).\n\n"
             "`cell_size` is the lattice. `target` asks for a COUNT instead, which is a\n"
             "search over cell size: THE TARGET IS APPROACHED, NEVER HIT. The count\n"
             "goes as cell^-2, so landing inside 5-10% is the expectation; read\n"
             "Mesh.quad_report for what actually came out. Each iteration is a WHOLE\n"
             "mesh, so max_iterations is a cost knob.\n\n"
             "The knobs take the C ABI's rules: tolerance <= 0 and max_iterations 0\n"
             "mean the defaults (0.10, 4), a negative max_iterations is refused, and\n"
             "target is capped at 16777216 quads.\n\n"
             "mode is 'dual'; 'faces' is voxels only and raises here.")
        .def("add_voxel_layer",
             [](PyDocument& d, const std::string& name, float voxel_size) {
                 if (voxel_size <= 0.0f) throw std::invalid_argument("voxel_size must be > 0");
                 // Through AddLayerCmd so the creation is undoable; see
                 // add_sdf_layer and add_mesh_layer, which both already were.
                 // This one mutated the document directly, so "no reachable
                 // edit escapes undo" was false here (#341). A voxel layer
                 // carries no SDF content.
                 scene::Layer l;
                 l.id = d.doc->document.reserve_layer_id();
                 l.name = name;
                 l.kind = scene::LayerKind::Voxel;
                 const scene::LayerId id = l.id;
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::AddLayerCmd{std::move(l), -1}},
                                "add_voxel_layer", d.undo.get());
                 d.doc->voxel_layers.insert_or_assign(id, voxel::VoxelGrid(voxel_size));
                 PyVoxelGrid g;
                 g.doc = d.doc;
                 g.undo = d.undo;
                 g.layer = id;
                 return g;
             },
             "name"_a, "voxel_size"_a = 0.1f,
             "Add a voxel layer and return its grid (edits are stored in the document)")
        .def(
            "mesh_layer_revision",
            [](const PyDocument& d, scene::LayerId layer) {
                const scene::Layer* l = d.doc->document.find_layer(layer);
                if (!l || l->kind != scene::LayerKind::Mesh)
                    throw std::invalid_argument("no mesh layer with that id");
                return revision_of(*d.mesh_revisions, layer);
            },
            "layer"_a,
            "A mesh layer's geometry revision: bumped every time its triangles\n"
            "are REPLACED wholesale, and never by a sculpt.\n\n"
            "A brush moves vertices and leaves the topology alone, which is the\n"
            "fixed-topology contract and is exactly the change a cache over that\n"
            "mesh survives. A rebuild swaps every vertex and every index, and an\n"
            "adjacency, a BVH or a live sculptor built over the old ones is wrong\n"
            "in a way nothing else detects.\n\n"
            "Read it, do the slow work, then hand it to replace_mesh_layer: if\n"
            "the layer was rebuilt in between, the commit is refused rather than\n"
            "overwriting work done while you were waiting.")
        .def(
            "replace_mesh_layer",
            [](PyDocument& d, scene::LayerId layer, const PyMesh& replacement,
               nb::handle expected_revision) {
                const mesh::Mesh& src = replacement.data();
                py_replace_mesh_layer(d, layer, mesh::Mesh(src), expected_revision);
            },
            "layer"_a, "mesh"_a, "expected_revision"_a = nb::none(),
            "Replace a mesh layer's triangles wholesale, as ONE undo step.\n\n"
            "For a caller that ran Mesh.voxel_remesh itself and now wants to\n"
            "commit it. `expected_revision` is what mesh_layer_revision returned\n"
            "before the work started; None skips the check, which is only right\n"
            "when nothing could have touched the layer in between.\n\n"
            "Raises if the layer moved under you, is ghosted or locked, or the\n"
            "replacement has no triangles. A refusal leaves the layer untouched.")
        .def(
            "voxel_remesh_layer",
            [](PyDocument& d, scene::LayerId layer, nb::handle resolution,
               nb::handle voxel_size, const std::string& open_surface,
               bool preserve_volume, bool project_to_source, bool preserve_colors,
               nb::handle memory_budget) {
                const scene::Layer* l = d.doc->document.find_layer(layer);
                if (!l || l->kind != scene::LayerKind::Mesh)
                    throw std::invalid_argument("no mesh layer with that id");
                // Refused BEFORE the rebuild: remeshing a locked layer for
                // several seconds and then declining to commit is a worse
                // answer than declining now.
                if (l->protected_from_edits())
                    throw std::runtime_error(std::string("layer '") + l->name + "' is " +
                                             (l->ghost ? "ghosted" : "locked") +
                                             " and takes no edits");
                auto it = d.doc->mesh_layers.find(layer);
                if (it == d.doc->mesh_layers.end())
                    throw std::invalid_argument("the mesh layer holds no triangles");

                mesh::VoxelRemeshParams p =
                    py_remesh_params(resolution, voxel_size, memory_budget);
                p.open_surface_policy = py_open_policy(open_surface);
                p.preserve_volume = preserve_volume;
                p.project_to_source = project_to_source;
                p.preserve_colors = preserve_colors;

                // A COPY, and the revision it was taken at. The copy is what
                // makes this transactional: the rebuild reads it while the
                // layer still holds the original, so a refusal is a discard.
                const mesh::Mesh source = it->second;
                const std::uint64_t at = revision_of(*d.mesh_revisions, layer);

                mesh::VoxelRemeshResult r;
                {
                    nb::gil_scoped_release release;
                    r = mesh::voxel_remesh(source, p);
                }
                if (r.status != mesh::VoxelRemeshStatus::Ok) raise_remesh(r.status);

                nb::dict report = voxel_remesh_report_dict(r.report);
                py_replace_mesh_layer(d, layer, std::move(r.mesh), nb::cast(at));
                return report;
            },
            "layer"_a, "resolution"_a = nb::none(), "voxel_size"_a = nb::none(),
            "open_surface"_a = "close", "preserve_volume"_a = true,
            "project_to_source"_a = true, "preserve_colors"_a = true,
            "memory_budget"_a = nb::none(),
            "Rebuild a mesh layer through a signed volumetric representation and\n"
            "put the result back on the layer, as ONE undo step. Returns the\n"
            "report.\n\n"
            "The whole operation is transactional: the rebuild reads a copy while\n"
            "the layer still holds the original, so a refusal or a failure leaves\n"
            "the layer byte-identical and adds no undo step.\n\n"
            "Mesh.voxel_remesh is the same rebuild without a document, for a\n"
            "caller that wants to run it on a worker thread — pair it with\n"
            "mesh_layer_revision and replace_mesh_layer to commit safely.")
        .def("add_mesh_layer",
             [](PyDocument& d, const PyMesh& source, const std::string& name, float scale) {
                 const mesh::Mesh& src = source.data();
                 if (src.triangle_count() == 0)
                     throw std::invalid_argument("cannot carry a mesh with no triangles");
                 mesh::Mesh stored = src;
                 if (scale > 0.0f && scale != 1.0f)
                     for (kernel::cfloat3& p : stored.positions) p = p * scale;
                 // Through AddLayerCmd so the attach is undoable; see
                 // add_sdf_layer. A mesh layer carries no SDF content.
                 scene::Layer l;
                 l.id = d.doc->document.reserve_layer_id();
                 l.name = name;
                 l.kind = scene::LayerKind::Mesh;
                 scene::LayerId id = l.id;
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::AddLayerCmd{std::move(l), -1}},
                                "add_mesh_layer", d.undo.get());
                 d.doc->mesh_layers.insert_or_assign(id, std::move(stored));
                 PyMesh borrowed;
                 borrowed.doc = d.doc;
                 borrowed.revisions = d.mesh_revisions;
                 borrowed.layer = id;
                 return borrowed;
             },
             "mesh"_a, "name"_a, "scale"_a = 1.0f,
             "Carry an imported mesh in the document: the triangles are kept as the\n"
             "importer produced them, for display and re-export, and are returned\n"
             "as a BORROWED Mesh whose buffers are the document's own memory.\n\n"
             "This is the opposite of Volume.from_mesh, which resamples a mesh into\n"
             "a field so it can be sculpted. A mesh layer is never evaluated: no\n"
             "tape, no blend, no influence bound, not pickable. Its geometry lives\n"
             "beside the document rather than in it, so that is structural.\n\n"
             "`scale` is baked into the stored vertices, so unit conversion is\n"
             "resolved once at import rather than approximated by a layer\n"
             "transform. The layer's own transform is applied by whatever exports\n"
             "it, and moving the layer does not move the stored vertices.")
        .def(
            "measure",
            [](const PyDocument& d, const std::string& measure, nb::handle points,
               nb::handle params) {
                brush::SurfaceMeasure m = measure_from_name(measure);
                brush::MeasureSettings s = measure_settings_from(params);
                auto arr = nb::cast<nb::ndarray<const float, nb::ndim<2>, nb::c_contig>>(points);
                if (arr.shape(1) != 3) throw std::invalid_argument("points must be (N, 3)");
                const std::size_t n = arr.shape(0);
                std::shared_ptr<const scene::Tape> tape_ref =
                    std::make_shared<scene::Tape>(scene::compile_document(d.doc->document));
                if (tape_ref->empty()) throw std::runtime_error("the document is empty");
                std::vector<kernel::cfloat3> pts(n);
                for (std::size_t i = 0; i < n; ++i)
                    pts[i] = kernel::cf3(arr.data()[i * 3], arr.data()[i * 3 + 1],
                                         arr.data()[i * 3 + 2]);
                nb::module_ np = nb::module_::import_("numpy");
                nb::object out = np.attr("empty")(nb::make_tuple(n), "dtype"_a = "float32");
                auto ov = nb::cast<nb::ndarray<float, nb::ndim<1>, nb::c_contig>>(out);
                {
                    nb::gil_scoped_release release;
                    auto field = [&](kernel::cfloat3 p) { return tape_ref->eval(p).d; };
                    brush::measure_points(field, m, pts.data(), n, s, ov.data());
                }
                return out;
            },
            "measure"_a, "points"_a, "params"_a = nb::none(),
            "Measure the surface at points: 'curvature', 'cavity', 'convexity',\n"
            "'normal_direction', 'occlusion' or 'thickness'.\n\n"
            "WHY THIS IS CHEAP ON A FIELD. Curvature is the LAPLACIAN and its\n"
            "sign is unambiguous — for f = |p| - R it is 2/R at the surface,\n"
            "POSITIVE for convex — so cavity and convexity are one subtraction\n"
            "apart. A mesh has to estimate curvature from a vertex ring, which\n"
            "is a discrete approximation with a valence-dependent error. The\n"
            "same runs for occlusion: a field is marched directly with nothing\n"
            "to build and nothing to invalidate, and it measures the ACTUAL\n"
            "surface rather than a tessellation of it.\n\n"
            "OCCLUSION is occlusion, not lighting: 0 is open sky and 1 is fully\n"
            "enclosed. THICKNESS is how much material is behind the point.\n\n"
            "Points are taken AS GIVEN and not projected onto the surface —\n"
            "use project() when you have a cage rather than surface points.\n\n"
            "DETERMINISTIC: the hemisphere pattern is a fixed low-discrepancy\n"
            "sequence rotated by a hash of the point and `seed`, so the same\n"
            "seed gives the same bits on every backend and every run.")
        .def(
            "mask_from_surface",
            [](const PyDocument& d, const std::string& measure, nb::handle region,
               float cell_size, float band, nb::handle params) {
                brush::ProceduralMaskSettings ps;
                ps.cell_size = cell_size;
                ps.band = band;
                ps.region = to_aabb(region);
                ps.measure = measure_settings_from(params);
                if (ps.region.empty() || ps.region.is_infinite())
                    throw std::invalid_argument("region must be bounded and non-empty");
                const brush::SurfaceMeasure m = measure_from_name(measure);
                scene::Tape tape = scene::compile_document(d.doc->document);
                if (tape.empty()) throw std::runtime_error("the document is empty");
                PyMaskField out;
                {
                    nb::gil_scoped_release release;
                    auto field = [&](kernel::cfloat3 p) { return tape.eval(p).d; };
                    out.owned = std::make_shared<voxel::MaskField>(
                        brush::mask_from_surface(field, m, ps));
                }
                return out;
            },
            "measure"_a, "region"_a, "cell_size"_a = 0.0f, "band"_a = 0.0f,
            "params"_a = nb::none(),
            "The LATTICE form of the same measures — a mask, banded to the\n"
            "surface. One implementation behind both, so a mask and a measured\n"
            "point cannot disagree about the same surface.\n\n"
            "Cells outside the band stay at zero: a measure taken deep inside a\n"
            "solid describes nothing an artist can see. The banding is this\n"
            "call's job and not measure()'s.\n\n"
            "'occlusion' and 'thickness' work here and are far more expensive\n"
            "than the stencil measures — a million cells is a million\n"
            "hemisphere samples. Prefer a coarse cell_size.")
        .def(
            "project",
            [](const PyDocument& d, nb::handle point, nb::handle direction,
               float max_distance) -> nb::object {
                scene::Tape tape = scene::compile_document(d.doc->document);
                const pick::Projection p = pick::project_to_surface(
                    tape, to_f3(point, "point"), to_f3(direction, "direction"), max_distance);
                if (!p.hit) return nb::none();
                nb::dict out;
                out["distance"] = p.distance;
                out["position"] = nb::make_tuple(p.position.x, p.position.y, p.position.z);
                out["normal"] = nb::make_tuple(p.normal.x, p.normal.y, p.normal.z);
                return out;
            },
            "point"_a, "direction"_a, "max_distance"_a,
            "Project a point onto the surface, searching BOTH ways within\n"
            "max_distance. Returns None if nothing is within the bound.\n\n"
            "BOTH WAYS is the part a first implementation gets wrong: a cage\n"
            "point built from a low-poly mesh may sit inside the high-poly\n"
            "surface or outside it, and you cannot know which. Searching only\n"
            "outward silently misses every point where the low-poly sits\n"
            "inside — most of a concave region.\n\n"
            "`distance` is SIGNED along `direction`, and it is the height-map\n"
            "value; it comes back from this call rather than being recomputed\n"
            "from the position, which would be a second chance to disagree\n"
            "about the sign.")
        .def(
            "groups",
            [](PyDocument& d, float cell_size) {
                if (!d.doc->groups) {
                    if (cell_size <= 0.0f) cell_size = 0.05f;
                    d.doc->groups.emplace(cell_size);
                }
                // An EXISTING lattice is not re-scaled whatever cell_size says:
                // re-scaling would move every boundary the artist placed, and
                // silently.
                PyGroupField g;
                g.doc = d.doc;
                g.undo = d.undo;
                return g;
            },
            "cell_size"_a = 0.05f,
            "The document's surface groups — ZBrush's PolyGroups, Blender's\n"
            "Face Sets. Creates the lattice on first call and returns the same\n"
            "one after; `cell_size` is used only when creating, since\n"
            "re-scaling would move every boundary you placed.\n\n"
            "ONE WORLD-SPACE LATTICE, asked 'which group is this surface point\n"
            "in' identically whatever the surface is made of — so groups\n"
            "survive rasterizing, meshing and converting BY CONSTRUCTION: they\n"
            "were never in the SDF, the voxels or the mesh.\n\n"
            "PER DOCUMENT, not per layer: a mask gates edits to its layer,\n"
            "while a group names a region of the MODEL, and 'isolate the head'\n"
            "when the head spans two layers is what per-layer storage makes\n"
            "impossible.\n\n"
            "The cost: a boundary is quantised to this lattice rather than to\n"
            "the representation, so a mesh that could have carried an exact\n"
            "per-face boundary does not.")
        .def_prop_ro(
            "has_groups", [](const PyDocument& d) { return d.doc->groups && !d.doc->groups->empty(); },
            "Whether any region has been named, without creating a lattice.")
        .def("add_mask",
             [](PyDocument& d, const std::string& name, float cell_size) {
                 if (cell_size <= 0.0f) throw std::invalid_argument("cell_size must be > 0");
                 const scene::Layer* target = nullptr;
                 for (const scene::Layer& l : d.doc->document.layers)
                     if (l.name == name) target = &l;
                 if (!target) throw std::invalid_argument("no layer named '" + name + "'");
                 d.doc->masks.insert_or_assign(target->id, voxel::MaskField(cell_size));
                 PyMaskField m;
                 m.doc = d.doc;
                 m.undo = d.undo;
                 m.layer = target->id;
                 return m;
             },
             "name"_a, "cell_size"_a = 0.1f,
             "Attach a mask to a layer and return it (edits are stored in the document)")
        .def("mask",
             [](const PyDocument& d, const std::string& name) -> nb::object {
                 for (const scene::Layer& l : d.doc->document.layers) {
                     if (l.name != name || !d.doc->masks.count(l.id)) continue;
                     PyMaskField m;
                     m.doc = d.doc;
                     m.undo = d.undo;
                     m.layer = l.id;
                     return nb::cast(m);
                 }
                 return nb::none();
             },
             "name"_a, "Look up a layer's mask by name, or None")
        .def("remove_mask",
             [](PyDocument& d, const std::string& name) {
                 for (const scene::Layer& l : d.doc->document.layers)
                     if (l.name == name) return d.doc->masks.erase(l.id) > 0;
                 return false;
             },
             "name"_a, "Drop a layer's mask; returns whether there was one")
        .def("mask_extrude",
             [](const PyDocument& d, nb::handle mask, float thickness, const std::string& side,
                float threshold, float border_round, int border_smooth, nb::handle cell_size,
                nb::handle band, nb::handle layer,
                parallel::CancelToken* token) {
                 const voxel::MaskField* m = borrow_mask(mask);
                 if (!m) throw std::invalid_argument("mask must be a MaskField");
                 brush::MaskExtrudeSettings settings = extrude_settings(
                     thickness, side, threshold, border_round, border_smooth, cell_size, band);

                 // Sampled from a TAPE rather than from a volume, so the source
                 // stays exact: a volume reports a bound outside its own band,
                 // and sampling one would record the seam between bound and
                 // distance as part of the extracted shape.
                 scene::Tape tape;
                 if (layer.is_none()) {
                     tape = scene::compile_document(d.doc->document);
                 } else {
                     const std::string name = nb::cast<std::string>(layer);
                     const scene::Layer* found = nullptr;
                     for (const scene::Layer& l : d.doc->document.layers)
                         if (l.name == name) found = &l;
                     if (!found) throw std::invalid_argument("no layer named '" + name + "'");
                     tape = scene::compile_layer(*found);
                 }
                 if (tape.empty())
                     throw std::invalid_argument("there is no field here to extrude from");

                 std::optional<field::FieldVolume> volume;
                 {
                     nb::gil_scoped_release release;
                     volume = brush::mask_extrude(
                         [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, *m, settings,
                         token);
                 }
                 if (!volume) {
                     // A cancel and "the mask never reached the surface" both
                     // come back as nullopt. A caller must not be shown the
                     // geometric message when they pressed Stop — the same
                     // distinction the C entry point makes.
                     if (token && token->cancelled())
                         throw std::runtime_error("the mask extrude was cancelled");
                     throw std::invalid_argument(
                         "nothing to extrude: the mask is empty, does not reach the surface, or "
                         "the wall is thinner than a cell");
                 }
                 PyVolume out;
                 out.prim = scene::Prim::volume();
                 out.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
                 return out;
             },
             "mask"_a, "thickness"_a, "side"_a = "outward", "threshold"_a = 0.5f,
             "border_round"_a = 0.0f, "border_smooth"_a = 0, "cell_size"_a = nb::none(),
             "band"_a = nb::none(), "layer"_a = nb::none(), "token"_a = nb::none(),
             "Mask a patch of the surface and pull it off as a solid — ZBrush's\n"
             "Extract, 3DCoat's extrude from a frozen area. Returns a Volume you\n"
             "add to a layer like any other item.\n\n"
             "THE MASK IS THE REGION. There is no region_radius here, unlike\n"
             "relax and flatten: the painted region bounds itself, which is also\n"
             "why this samples a smaller volume than either of them.\n\n"
             "`side` is 'outward' (the plate sits on the surface), 'inward' (the\n"
             "pocket) or 'centred'. `border_round` softens the rim and\n"
             "`border_smooth` blurs a COPY of the mask first — your mask is never\n"
             "modified.\n\n"
             "It BAKES, as relax and flatten do: what comes back is a sampled\n"
             "volume with no link back to the source. Raises rather than\n"
             "returning something empty when the mask misses the surface, which\n"
             "is the common mistake and the one an empty result would disguise.")
        .def("voxel_layer",
             [](const PyDocument& d, const std::string& name) -> nb::object {
                 for (const scene::Layer& l : d.doc->document.layers) {
                     if (l.name != name || l.kind != scene::LayerKind::Voxel) continue;
                     if (!d.doc->voxel_layers.count(l.id)) continue;
                     PyVoxelGrid g;
                     g.doc = d.doc;
                 g.undo = d.undo;
                     g.layer = l.id;
                     return nb::cast(g);
                 }
                 return nb::none();
             },
             "name"_a, "Look up a voxel layer's grid by name")
        .def("mesh_layer",
             [](const PyDocument& d, const std::string& name) -> nb::object {
                 for (const scene::Layer& l : d.doc->document.layers) {
                     if (l.name != name || l.kind != scene::LayerKind::Mesh) continue;
                     if (!d.doc->mesh_layers.count(l.id)) continue;
                     PyMesh borrowed;
                     borrowed.doc = d.doc;
                     borrowed.revisions = d.mesh_revisions;
                     borrowed.layer = l.id;
                     return nb::cast(borrowed);
                 }
                 return nb::none();
             },
             "name"_a, "Look up a mesh layer's triangles by name; None when there is none")
        .def("raycast",
             [](const PyDocument& d, nb::handle origin, nb::handle direction) -> nb::object {
                 math::Ray ray{to_f3(origin, "origin"),
                               kernel::cnormalize(to_f3(direction, "direction"))};
                 pick::SceneHit hit;
                 pick::RaycastOptions opts;
                 // Hidden surface is stepped over, not turned into a miss:
                 // hiding the front of a head is how you reach the inside.
                 if (d.doc->groups) opts.groups = &*d.doc->groups;
                 {
                     nb::gil_scoped_release release;
                     hit = pick::raycast_scene(d.doc->document, ray, opts);
                 }
                 if (!hit.hit) return nb::none();
                 nb::dict out;
                 out["t"] = hit.t;
                 out["position"] =
                     nb::make_tuple(hit.position.x, hit.position.y, hit.position.z);
                 out["normal"] = nb::make_tuple(hit.normal.x, hit.normal.y, hit.normal.z);
                 out["layer"] = hit.layer;
                 out["item"] = hit.item;
                 return out;
             },
             "origin"_a, "direction"_a,
             "Raycast the document; returns hit position/normal and layer+item attribution")
        .def("raycast_many",
             [](const PyDocument& d, nb::handle rays_in) {
                 RaysView rays = to_rays(rays_in);
                 scene::Tape tape = pick::pickable_tape(d.doc->document);
                 eval::Backend* cpu = find_backend("cpu");
                 std::vector<eval::RayHit> hits(rays.count ? rays.count : 1);
                 eval::RayQuery q{rays.data.data(), rays.count, 0.0f, 1e6f, 1e-4f, 256};
                 const voxel::GroupField* groups =
                     d.doc->groups ? &*d.doc->groups : nullptr;
                 {
                     nb::gil_scoped_release release;
                     if (cpu->raycast(tape, q, hits.data()) != eval::Status::Ok)
                         throw std::runtime_error("batch raycast failed");
                     // The batch stays a batch: only rays that actually landed
                     // on hidden surface are re-resolved, so a document with
                     // nothing hidden pays exactly what it always did. This is
                     // the path _render.py draws through, so without it a
                     // rendered isolate looks identical to the whole model —
                     // which is how the omission was found.
                     if (groups && groups->any_hidden()) {
                         auto field = [&](kernel::cfloat3 p) { return tape.eval(p).d; };
                         for (std::size_t i = 0; i < rays.count; ++i) {
                             if (!hits[i].hit) continue;
                             const kernel::cfloat3 p = kernel::cf3(
                                 hits[i].pos[0], hits[i].pos[1], hits[i].pos[2]);
                             if (!groups->point_hidden(p)) continue;
                             const math::Ray r{
                                 kernel::cf3(rays.data[i * 6], rays.data[i * 6 + 1],
                                             rays.data[i * 6 + 2]),
                                 kernel::cf3(rays.data[i * 6 + 3], rays.data[i * 6 + 4],
                                             rays.data[i * 6 + 5])};
                             const float step =
                                 kernel::cmax(groups->cell_size(), q.eps * 4.0f);
                             const float t = pick::next_visible_crossing(
                                 field, r, hits[i].t + step * 0.5f, q.tmax, step, *groups);
                             if (t < 0.0f) {
                                 hits[i].hit = 0;
                                 continue;
                             }
                             const kernel::cfloat3 hp = r.at(t);
                             hits[i].t = t;
                             hits[i].pos[0] = hp.x;
                             hits[i].pos[1] = hp.y;
                             hits[i].pos[2] = hp.z;
                             const kernel::cfloat3 nn = kernel::cnormal(field, hp, 1e-4f);
                             hits[i].normal[0] = nn.x;
                             hits[i].normal[1] = nn.y;
                             hits[i].normal[2] = nn.z;
                         }
                     }
                 }
                 const std::size_t n = rays.count;
                 nb::module_ np = nb::module_::import_("numpy");
                 nb::object hit_arr = np.attr("empty")(nb::make_tuple(n), "dtype"_a = "bool");
                 nb::object t_arr = np.attr("empty")(nb::make_tuple(n), "dtype"_a = "float32");
                 nb::object pos_arr =
                     np.attr("empty")(nb::make_tuple(n, 3), "dtype"_a = "float32");
                 nb::object nor_arr =
                     np.attr("empty")(nb::make_tuple(n, 3), "dtype"_a = "float32");
                 auto hv = nb::cast<nb::ndarray<bool, nb::ndim<1>, nb::c_contig>>(hit_arr);
                 auto tv = nb::cast<nb::ndarray<float, nb::ndim<1>, nb::c_contig>>(t_arr);
                 auto pv = nb::cast<nb::ndarray<float, nb::ndim<2>, nb::c_contig>>(pos_arr);
                 auto nv = nb::cast<nb::ndarray<float, nb::ndim<2>, nb::c_contig>>(nor_arr);
                 for (std::size_t i = 0; i < n; ++i) {
                     hv.data()[i] = hits[i].hit != 0;
                     tv.data()[i] = hits[i].t;
                     for (int k = 0; k < 3; ++k) {
                         pv.data()[i * 3 + k] = hits[i].pos[k];
                         nv.data()[i * 3 + k] = hits[i].normal[k];
                     }
                 }
                 nb::dict out;
                 out["hit"] = hit_arr;
                 out["t"] = t_arr;
                 out["position"] = pos_arr;
                 out["normal"] = nor_arr;
                 return out;
             },
             "rays"_a, "Batch raycast from an (N, 6) float32 array of origin + direction")
        .def("snap_to_surface",
             [](const PyDocument& d, nb::handle points) {
                 PointsView pts = to_points(points);
                 scene::Tape tape = pick::pickable_tape(d.doc->document);
                 const std::size_t n = pts.count;
                 nb::module_ np = nb::module_::import_("numpy");
                 nb::object pos_arr =
                     np.attr("empty")(nb::make_tuple(n, 3), "dtype"_a = "float32");
                 nb::object nor_arr =
                     np.attr("empty")(nb::make_tuple(n, 3), "dtype"_a = "float32");
                 auto pv = nb::cast<nb::ndarray<float, nb::ndim<2>, nb::c_contig>>(pos_arr);
                 auto nv = nb::cast<nb::ndarray<float, nb::ndim<2>, nb::c_contig>>(nor_arr);
                 {
                     nb::gil_scoped_release release;
                     for (std::size_t i = 0; i < n; ++i) {
                         pick::SnapResult r = pick::snap_to_surface(
                             tape, kernel::cf3(pts.data[i * 3], pts.data[i * 3 + 1],
                                               pts.data[i * 3 + 2]));
                         pv.data()[i * 3 + 0] = r.position.x;
                         pv.data()[i * 3 + 1] = r.position.y;
                         pv.data()[i * 3 + 2] = r.position.z;
                         nv.data()[i * 3 + 0] = r.normal.x;
                         nv.data()[i * 3 + 1] = r.normal.y;
                         nv.data()[i * 3 + 2] = r.normal.z;
                     }
                 }
                 nb::dict out;
                 out["position"] = pos_arr;
                 out["normal"] = nor_arr;
                 return out;
             },
             "points"_a,
             "Snap (N, 3) points onto the surface; returns positions and outward normals")
        .def("save",
             [](const PyDocument& d, const std::string& path) {
                 check_io(io::save_clayspace_file(*d.doc, path));
             },
             "path"_a, "Save the document as .clayspace")
        .def(
            "to_bytes",
            [](const PyDocument& d) {
                const std::vector<std::uint8_t> bytes = io::save_clayspace(*d.doc);
                return nb::bytes(bytes.data(), bytes.size());
            },
            "The same bytes `save` would write, without a path — for a host\n"
            "whose documents live in a container, a database or a network\n"
            "request rather than on a filesystem. Read them back with\n"
            "clay.load_bytes.")
        .def("remove_layer",
             [](PyDocument& d, scene::LayerId layer) {
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::RemoveLayerCmd{layer}}, "remove_layer", d.undo.get());
             },
             "layer"_a, "Remove a layer and its content")
        .def("move_layer",
             [](PyDocument& d, scene::LayerId layer, int index) {
                 // The vocabulary expresses reorder as remove-then-add, so this
                 // is the one edit that is a pair of commands rather than one.
                 const scene::Layer* found = d.doc->document.find_layer(layer);
                 if (!found)
                     throw std::invalid_argument("move_layer: no layer with that id");
                 scene::Layer copy = *found;
                 // Taken before the remove, while the sharer is still
                 // findable: a reinsertion of a layer whose edit list is
                 // shared must NAME a sharer, or the serialized form of the
                 // add carries the content inline and a journal replay comes
                 // back with the instances unlinked (scene::content_sharer_of).
                 const scene::LayerId sharer =
                     scene::content_sharer_of(d.doc->document, layer);
                 apply_or_throw(d.doc->document, scene::Command{scene::RemoveLayerCmd{layer}},
                                "move_layer", d.undo.get());
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::AddLayerCmd{std::move(copy), index, sharer}},
                                "move_layer", d.undo.get());
             },
             "layer"_a, "index"_a, "Reorder a layer (applied as remove then add)")
        .def("set_layer_visible",
             [](PyDocument& d, scene::LayerId layer, bool visible) {
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::SetLayerVisibleCmd{layer, visible}},
                                "set_layer_visible", d.undo.get());
             },
             "layer"_a, "visible"_a,
             "Show or hide a layer; a hidden layer contributes nothing to the field")
        .def("set_layer_protection",
             [](PyDocument& d, scene::LayerId layer, bool ghost, bool locked) {
                 apply_or_throw(
                     d.doc->document,
                     scene::Command{scene::SetLayerProtectionCmd{layer, ghost, locked}},
                     "set_layer_protection", d.undo.get());
             },
             "layer"_a, "ghost"_a = false, "locked"_a = false,
             "Protect a layer. A ghosted layer is still evaluated but is never "
             "picked and never edited; a locked one is still picked but never "
             "edited. Neither changes what the document evaluates to, and an "
             "edit to a protected layer raises rather than being dropped.")
        .def("layer_protection",
             [](const PyDocument& d, scene::LayerId layer) {
                 const scene::Layer* l = d.doc->document.find_layer(layer);
                 if (!l) throw std::invalid_argument("no layer with that id in this document");
                 return nb::make_tuple(l->ghost, l->locked);
             },
             "layer"_a, "A layer's (ghost, locked) flags")
        .def("set_layer_transform",
             [](PyDocument& d, scene::LayerId layer, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 const scene::Layer* found = d.doc->document.find_layer(layer);
                 if (!found)
                     throw std::invalid_argument("set_layer_transform: no layer with that id");
                 scene::SetLayerTransformCmd cmd{layer, found->xform};
                 if (!position.is_none()) cmd.xform.position = to_f3(position, "position");
                 if (!rotation_axis_angle.is_none())
                     cmd.xform.rotation = to_axis_angle(rotation_axis_angle);
                 if (!scale.is_none()) cmd.xform.scale = nb::cast<float>(scale);
                 apply_or_throw(d.doc->document, scene::Command{cmd}, "set_layer_transform", d.undo.get());
             },
             "layer"_a, "position"_a = nb::none(), "rotation_axis_angle"_a = nb::none(),
             "scale"_a = nb::none(),
             "Retransform a whole layer; omitted arguments keep their current value")
        .def("enable_undo",
             [](PyDocument& d) {
                 if (!*d.undo) {
                     *d.undo = std::make_shared<session::History>();
                     (*d.undo)->set_enabled(true);
                     // Set once rather than passed to undo/redo like the three
                     // per-layer resolvers: the group lattice is per DOCUMENT,
                     // so there is no map lookup that can miss. Captures the
                     // ClaySpaceDoc by shared_ptr and is consulted at the
                     // moment of use, so a lattice created LATER is still
                     // found.
                     std::shared_ptr<io::ClaySpaceDoc> doc = d.doc;
                     (*d.undo)->set_groups_resolver([doc]() -> voxel::GroupField* {
                         return doc->groups ? &*doc->groups : nullptr;
                     });
                 }
             },
             "Start recording edits. Off by default, so a document that never "
             "calls this pays nothing; edits made before it are not undoable.")
        .def_prop_ro("undo_enabled", [](const PyDocument& d) { return bool(*d.undo); })
        .def("undo",
             [](PyDocument& d) {
                 if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                 return (*d.undo)->undo(d.doc->document, grid_for(d), mesh_for(d), nullptr, mask_for(d));
             },
             "Reverse the last recorded step; returns False when there is nothing to undo")
        .def("redo",
             [](PyDocument& d) {
                 if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                 return (*d.undo)->redo(d.doc->document, grid_for(d), mesh_for(d), nullptr, mask_for(d));
             },
             "Reapply the last undone step; returns False when there is nothing to redo")
        .def_prop_ro(
            "history_bytes",
            [](const PyDocument& d) {
                nb::dict out;
                session::History::Bytes b;
                if (*d.undo) b = (*d.undo)->bytes();
                out["undo"] = b.undo;
                out["redo"] = b.redo;
                out["journal"] = b.journal;
                out["total"] = b.total;
                out["undo_steps"] = b.undo_steps;
                out["redo_steps"] = b.redo_steps;
                out["journal_events"] = b.journal_events;
                out["dropped_steps"] = b.dropped_steps;
                return out;
            },
            "What the history costs, in bytes and steps.\n\n"
            "What is expensive is not what you expect. The command stack stores\n"
            "INVERSES, so REMOVING an item records a whole node while ADDING one\n"
            "records an id — a session of deletes and a session of adds cost\n"
            "very differently. A voxel or mask step is proportional to the cells\n"
            "it CHANGED, so one big fill can outweigh a thousand dabs. And the\n"
            "journal keeps its own copy, so crash recovery roughly doubles it.\n\n"
            "`dropped_steps` is how far the horizon has moved: show it rather\n"
            "than letting a user hunt for a step that is gone.")
        .def_prop_ro(
            "memory",
            [](const PyDocument& d) {
                return memory_dict(io::document_memory(*d.doc, d.undo ? d.undo->get() : nullptr));
            },
            "What this whole document costs, broken down by subsystem.\n\n"
            "THE BREAKDOWN IS THE POINT, and a total is not. Under memory\n"
            "pressure you do not need to know how big the document is, you need\n"
            "to know WHICH PART, because that decides what you may release:\n\n"
            "  history               -> costs undo depth (set_history_budget)\n"
            "  voxel_sculpt_layers   -> costs voxel undo depth\n"
            "  passthrough           -> a thumbnail; regenerable\n"
            "  edit_list / voxel_content / mesh_layers / masks -> the user's\n"
            "    work. Releasing any of it destroys something unrecoverable.\n\n"
            "A FLOOR, NOT AN EQUALITY: these are container walks, so allocator\n"
            "overhead and the library's own static data are outside them, and\n"
            "the OS will charge the process more. It is also LARGER than the\n"
            "same document's file, because the file is compressed and live\n"
            "storage is not.\n\n"
            "`voxel_content` FOLLOWS CHUNKS, NOT CELLS: a chunk is 32^3 cells\n"
            "allocated whole, so one voxel costs 32 KiB and 32768 voxels in that\n"
            "same chunk cost the same. Expect it to move independently of\n"
            "occupied_count.")
        .def(
            "memory_with_surfaces",
            [](const PyDocument& d, nb::handle surfaces) {
                // THE SURFACE SEAM, and the reason `memory` did not simply gain
                // an argument: a hierarchy and an adaptive surface are owned by
                // the HOST and held BESIDE a document rather than inside one, so
                // a document cannot walk them and a guess would be worse than
                // the honest zero `memory` reports. A caller that holds them
                // passes their ledgers — one, or a list of them — here.
                memory::MemoryLedger held;
                if (!surfaces.is_none()) {
                    nb::list items;
                    if (nb::try_cast(surfaces, items)) {
                        for (nb::handle h : items) merge_ledger_dict(h, &held);
                    } else {
                        merge_ledger_dict(surfaces, &held);
                    }
                }
                return memory_dict(io::document_memory(
                    *d.doc, d.undo ? d.undo->get() : nullptr,
                    surfaces.is_none() ? nullptr : &held));
            },
            "surfaces"_a.none(),
            "The same report, with the surfaces the host holds beside this\n"
            "document folded into the surface-tier lines.\n\n"
            "`surfaces` is a ledger `memory_ledger()` returned, or a list of\n"
            "them — only the caller knows which surfaces belong to this\n"
            "document. Passing None is exactly `memory`, which is why that one\n"
            "stays rather than gaining an argument.")
        .def(
            "layer_memory",
            [](const PyDocument& d, const std::string& name) {
                // BY NAME, because every other layer lookup in this module is
                // by name and a host should not have to learn where ids come
                // from for one query.
                for (const scene::Layer& l : d.doc->document.layers) {
                    if (l.name != name) continue;
                    io::MemoryReport r;
                    if (io::layer_memory(*d.doc, l.id, &r)) return memory_dict(r);
                    break;
                }
                throw std::runtime_error("no layer named '" + name + "'");
            },
            "name"_a,
            "The same breakdown for ONE layer, so a large document can be\n"
            "attributed to the layer responsible rather than merely called\n"
            "large.\n\n"
            "`history` and `passthrough` are document-wide and are always 0\n"
            "here. The CONTENT figures sum exactly across layers; the edit list\n"
            "does NOT, because the document-wide figure includes overhead owned\n"
            "by no layer and INSTANCE layers share one edit list, which the\n"
            "document counts once and each instance reports in full. A layer's\n"
            "edit_list is a ceiling on its contribution, not a partition.\n\n"
            "Raises for a layer that does not exist, rather than returning\n"
            "zeros — which you would read as an empty layer.")
        .def(
            "set_history_budget",
            [](PyDocument& d, std::size_t bytes) {
                if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                (*d.undo)->set_budget(bytes);
            },
            "bytes"_a,
            "Bound the history. 0 is UNBOUNDED, which is what you get without\n"
            "calling this.\n\n"
            "Bounds undo and redo ONLY. It deliberately does not evict from the\n"
            "journal: those bytes are your crash recovery, and dropping them\n"
            "silently would lose exactly what that feature exists to keep — use\n"
            "journal_trim once they are durable.\n\n"
            "Redo is spent first, because the next edit discards it anyway. The\n"
            "newest undo step is never dropped: a budget that could make the\n"
            "next undo fail would be worse than none, because you could not tell\n"
            "it from a bug.")
        .def(
            "trim_history",
            [](PyDocument& d, std::size_t bytes) {
                if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                (*d.undo)->trim_to(bytes);
            },
            "bytes"_a,
            "Drop the oldest steps until the history fits, for a platform that\n"
            "just reported memory pressure and wants an answer now rather than\n"
            "at the next edit. Does not set a budget.")
        .def(
            "journal_since",
            [](const PyDocument& d, std::size_t from) {
                if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                std::size_t now_at = 0;
                const std::vector<std::uint8_t> bytes = (*d.undo)->journal_since(from, &now_at);
                return nb::make_tuple(nb::bytes(bytes.data(), bytes.size()), now_at);
            },
            "from"_a = 0,
            "Everything recorded since `from`, as (bytes, now_at) — append the\n"
            "bytes wherever you keep them and pass `now_at` next time.\n\n"
            "A recovery is a SNAPSHOT plus the steps since it: pair this with\n"
            "Document.to_bytes(). Why not just autosave the document — saving is\n"
            "whole-document and synchronous, so a timer-driven autosave stalls\n"
            "for however long the whole sculpt takes, and that grows. A journal\n"
            "is proportional to what changed.\n\n"
            "PEEK, not drain: the log is untouched, so a failed write is retried\n"
            "by asking again. Indices are absolute and do not shift on trim.")
        .def(
            "journal_range",
            [](const PyDocument& d) {
                if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                return nb::make_tuple((*d.undo)->journal_first(), (*d.undo)->journal_next());
            },
            "The window the log still holds, as (first, next). Asking\n"
            "journal_since for something below `first` yields nothing — this is\n"
            "how you find out, rather than by replaying a short history.")
        .def(
            "journal_trim",
            [](PyDocument& d, std::size_t upto) {
                if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                (*d.undo)->trim_journal(upto);
            },
            "upto"_a, "Drop events below `upto`, once those bytes are durable.")
        .def(
            "replay_journal",
            [](PyDocument& d, nb::bytes data) {
                if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                session::History::ReplayResult r;
                const bool ok = (*d.undo)->replay(
                    reinterpret_cast<const std::uint8_t*>(data.c_str()), data.size(),
                    d.doc->document, grid_for(d), mesh_for(d), &r, mask_for(d));
                if (!ok)
                    throw std::invalid_argument(
                        "the journal could not be replayed: a version this build does not "
                        "understand, a truncated buffer, or a step naming a layer that is gone");
                nb::dict out;
                out["applied"] = r.applied;
                out["stopped_at_barrier"] = r.stopped_at_barrier;
                out["barrier"] = r.barrier;
                return out;
            },
            "data"_a,
            "Replay a journal onto a document that IS the snapshot it was taken\n"
            "against. Returns applied / stopped_at_barrier / barrier.\n\n"
            "Replay STOPS at a barrier rather than skipping it — every mask edit\n"
            "is one today, because a mask is a fourth representation with no\n"
            "history mechanism. A recovery that silently skipped would hand back\n"
            "a document quietly missing that operation's effect, and you could\n"
            "not see the loss. Seeing the flag means you need a fresher\n"
            "snapshot, not a longer journal.\n\n"
            "A journal this build does not understand, or a truncated one, is\n"
            "REFUSED. Events applied before the bad one stand, so replay onto a\n"
            "copy if you want all-or-nothing.")
        .def_prop_ro("undo_depth",
                     [](const PyDocument& d) {
                         return *d.undo ? (*d.undo)->undo_depth() : std::size_t{0};
                     })
        .def_prop_ro("redo_depth",
                     [](const PyDocument& d) {
                         return *d.undo ? (*d.undo)->redo_depth() : std::size_t{0};
                     })
        .def("begin_undo_group",
             [](PyDocument& d) {
                 if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                 (*d.undo)->begin_group();
             },
             "Bracket a burst of edits so they undo as one step")
        .def("end_undo_group",
             [](PyDocument& d) {
                 if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                 (*d.undo)->end_group();
             });

    // -- module functions -----------------------------------------------------------
    nb::enum_<brush::Accumulation>(m, "Accumulation",
                                   "How overlapping stamps in one stroke combine")
        .value("BUILDUP", brush::Accumulation::Buildup,
               "each stamp applies at its own strength; passing twice acts twice")
        .value("CLAMPED", brush::Accumulation::Clamped,
               "the stroke reaches its strength once, however many stamps overlap");

    // -- adaptive topology ----------------------------------------------------
    nb::enum_<mesh::DynamicDetailMode>(
        m, "DetailMode",
        "Where a remesher's target edge length comes from.")
        .value("WORLD", mesh::DynamicDetailMode::World, "target_edge_length in world units")
        .value("BRUSH_RELATIVE", mesh::DynamicDetailMode::BrushRelative,
               "radius / detail_resolution — a smaller brush makes finer geometry")
        .value("CONSTANT", mesh::DynamicDetailMode::Constant, "no adaptation; deformation only");

    nb::class_<mesh::DynamicTopologySettings>(
        m, "TopologySettings",
        "How a stamp adapts the surface under it.\n\n"
        "The gap between split_factor and collapse_factor is HYSTERESIS: with\n"
        "one threshold an edge just above it splits into two just below it and\n"
        "they collapse back, for as long as the brush is held still.")
        .def(nb::init<>())
        .def_rw("enabled", &mesh::DynamicTopologySettings::enabled)
        .def_rw("detail_mode", &mesh::DynamicTopologySettings::detail_mode)
        .def_rw("target_edge_length", &mesh::DynamicTopologySettings::target_edge_length)
        .def_rw("detail_resolution", &mesh::DynamicTopologySettings::detail_resolution)
        .def_rw("split_factor", &mesh::DynamicTopologySettings::split_factor)
        .def_rw("collapse_factor", &mesh::DynamicTopologySettings::collapse_factor)
        .def_rw("max_passes", &mesh::DynamicTopologySettings::max_passes)
        .def_rw("max_ops_per_stamp", &mesh::DynamicTopologySettings::max_ops_per_stamp,
                "a bound a host sets, so detail can be traded for latency")
        .def_rw("allow_split", &mesh::DynamicTopologySettings::allow_split)
        .def_rw("allow_collapse", &mesh::DynamicTopologySettings::allow_collapse)
        .def_rw("allow_flip", &mesh::DynamicTopologySettings::allow_flip)
        .def_rw("relax_after_remesh", &mesh::DynamicTopologySettings::relax_after_remesh)
        .def_rw("relax_strength", &mesh::DynamicTopologySettings::relax_strength)
        .def_rw("preserve_boundaries", &mesh::DynamicTopologySettings::preserve_boundaries)
        .def_rw("preserve_uv_seams", &mesh::DynamicTopologySettings::preserve_uv_seams)
        .def_rw("preserve_sharp_edges", &mesh::DynamicTopologySettings::preserve_sharp_edges);

    nb::class_<mesh::DynamicSurface>(
        m, "DynamicSurface",
        "A surface whose CONNECTIVITY changes under the brush: geometry is\n"
        "created where a stroke needs it and removed where it does not.\n\n"
        "A DIFFERENT REPRESENTATION, chosen deliberately, never a mode the\n"
        "fixed sculptor slips into — MeshSculptor's contract that indices and\n"
        "quads come out byte-identical is unchanged.\n\n"
        "A dynamic surface is TRIANGLES. to_mesh() writes no quads and derives\n"
        "none: a quad workflow does not pass through this representation.")
        .def_static(
            "from_mesh",
            [](const PyMesh& handle, float weld_epsilon, float uv_seam_epsilon) {
                const mesh::Mesh& src = handle.data();
                mesh::DynamicSurfaceBuildOptions options;
                if (weld_epsilon > 0.0f) options.weld_epsilon = weld_epsilon;
                if (uv_seam_epsilon > 0.0f) options.uv_seam_epsilon = uv_seam_epsilon;
                mesh::DynamicBuildError err = mesh::DynamicBuildError::None;
                auto built = mesh::DynamicSurface::from_mesh(src, options, &err);
                if (!built) {
                    const char* what = "a dynamic surface cannot be built from this mesh";
                    switch (err) {
                        case mesh::DynamicBuildError::EmptyMesh:
                            what = "empty mesh, or an index count not a multiple of three";
                            break;
                        case mesh::DynamicBuildError::IndexOutOfRange:
                            what = "a triangle index is past the end of the positions";
                            break;
                        case mesh::DynamicBuildError::DegenerateTriangle:
                            what = "a triangle whose corners weld together has no area";
                            break;
                        case mesh::DynamicBuildError::NonManifoldEdge:
                            what =
                                "three or more faces on one edge; a half-edge surface cannot "
                                "express it";
                            break;
                        default:
                            break;
                    }
                    throw std::invalid_argument(what);
                }
                return std::move(*built);
            },
            "mesh"_a, "weld_epsilon"_a = 0.0f, "uv_seam_epsilon"_a = 0.0f,
            "Convert a mesh. Refuses rather than repairs: silently dropping a\n"
            "third face on an edge would change the model without saying so.")
        .def("to_mesh",
             [](const mesh::DynamicSurface& s) {
                 PyMesh out;
                 out.m = s.to_mesh();
                 return out;
             },
             "The surface as a flat mesh. Triangles, with quads empty.")
        .def_prop_ro("vertex_count",
                     [](const mesh::DynamicSurface& s) { return s.stats().vertices; })
        .def_prop_ro("edge_count", [](const mesh::DynamicSurface& s) { return s.stats().edges; })
        .def_prop_ro("face_count", [](const mesh::DynamicSurface& s) { return s.stats().faces; })
        .def_prop_ro("boundary_edge_count",
                     [](const mesh::DynamicSurface& s) { return s.stats().boundary_edges; })
        .def_prop_ro("dead_slots",
                     [](const mesh::DynamicSurface& s) { return s.stats().dead_slots; })
        .def_prop_ro("bytes", [](const mesh::DynamicSurface& s) { return s.bytes(); })
        .def_prop_ro("topology_revision", &mesh::DynamicSurface::topology_revision,
                     "advances when connectivity changed")
        .def_prop_ro("geometry_revision", &mesh::DynamicSurface::geometry_revision,
                     "advances when a vertex moved")
        .def_prop_ro("attribute_revision", &mesh::DynamicSurface::attribute_revision)
        .def("validate",
             [](const mesh::DynamicSurface& s) {
                 const mesh::DynamicValidationReport r = mesh::validate_dynamic_surface(s);
                 nb::dict out;
                 out["ok"] = r.ok;
                 out["summary"] = r.summary();
                 return out;
             },
             "Every invariant of the half-edge structure. The failure mode here\n"
             "is a surface that still renders and is quietly wrong in one fan,\n"
             "so this is a call rather than something only a test does.")
        .def("serialize",
             [](const mesh::DynamicSurface& s) {
                 std::vector<std::uint8_t> bytes = s.encode();
                 return nb::bytes(bytes.data(), bytes.size());
             },
             "A versioned format of its own; generations are preserved.")
        .def_static("deserialize",
                    [](nb::bytes data) {
                        mesh::DynamicSurface out;
                        if (!mesh::DynamicSurface::decode(
                                reinterpret_cast<const std::uint8_t*>(data.c_str()), data.size(),
                                &out))
                            throw std::invalid_argument(
                                "not a dynamic surface this build can read: malformed, "
                                "truncated, or written by a newer schema version");
                        return out;
                    },
                    "data"_a)
        .def(
            "preflight_to_mesh",
            [](const mesh::DynamicSurface& s, std::uint64_t budget) {
                return preflight_dict(mesh::preflight_to_mesh(s, budget));
            },
            "budget"_a = 0,
            "What exporting this surface as a flat mesh WOULD cost. The export\n"
            "SPLITS a geometric vertex into as many export vertices as it has\n"
            "distinct corner attributes, so the result is bounded by CORNERS\n"
            "rather than by vertices — the term that makes this bigger than it\n"
            "looks on a seam-heavy model.")
        .def(
            "preflight_encode",
            [](const mesh::DynamicSurface& s, std::uint64_t budget) {
                return preflight_dict(mesh::preflight_encode(s, budget));
            },
            "budget"_a = 0,
            "The same for serialization, where the blob is a second copy of\n"
            "everything and exists while the surface still does.");

    nb::class_<mesh::DynamicSculptor>(
        m, "DynamicSculptor",
        "The brush engine over an adaptive surface: the same verbs, the same\n"
        "falloffs, the same mask, and the same deformation math — the shared\n"
        "kernels called rather than copied, so a brush means one thing on both\n"
        "representations.\n\n"
        "The surface must outlive the sculptor.")
        .def("__init__",
             [](mesh::DynamicSculptor* self, mesh::DynamicSurface& surface) {
                 new (self) mesh::DynamicSculptor(surface);
             },
             "surface"_a, nb::keep_alive<1, 2>())
        .def(
            "stamp",
            [](mesh::DynamicSculptor& self, const std::string& verb, nb::handle center,
               float radius, float strength, const std::string& falloff,
               const mesh::DynamicTopologySettings& topology, nb::handle direction,
               nb::handle mask, bool geodesic, int smooth_iterations) {
                mesh::MeshBrush chosen = mesh::MeshBrush::Draw;
                mesh::MeshBrushSettings settings = mesh_brush_settings(
                    verb, center, radius, strength, falloff, direction, nb::none(),
                    nb::cast(geodesic), nb::none(), "two_sided", nb::none(), nb::none(), 0.2f,
                    smooth_iterations, 0.0f, nb::none(), nb::none(), nb::none(), 0.0f, nb::none(),
                    &chosen);
                if (!mesh::dynamic_offers(chosen))
                    throw std::invalid_argument(
                        "an adaptive surface does not offer '" + verb +
                        "': its reference is the surface as the STROKE found it, and half the "
                        "vertices under the brush at the end of an adaptive stroke did not "
                        "exist at the start");
                const voxel::MaskField* field_mask = borrow_mask(mask);
                field::MaskGate gate;
                if (field_mask)
                    gate = [field_mask](kernel::cfloat3 p) { return field_mask->sample(p); };
                mesh::DynamicStampResult r;
                {
                    nb::gil_scoped_release release;
                    r = self.stamp(chosen, settings, topology, gate, nullptr);
                }
                nb::dict out;
                out["moved"] = r.moved_vertices;
                out["split"] = r.remesh.split;
                out["collapsed"] = r.remesh.collapsed;
                out["flipped"] = r.remesh.flipped;
                out["relaxed"] = r.remesh.relaxed;
                out["hit_budget"] = r.remesh.hit_budget;
                out["topology_revision"] = r.topology_revision;
                out["geometry_revision"] = r.geometry_revision;
                return out;
            },
            "verb"_a, "center"_a, "radius"_a, "strength"_a = 0.5f, "falloff"_a = "smooth",
            "topology"_a = mesh::DynamicTopologySettings{}, "direction"_a = nb::none(),
            "mask"_a = nb::none(), "geodesic"_a = true, "smooth_iterations"_a = 1,
            "One stamp: remesh where the verb's timing says, deform through the\n"
            "shared kernels, recompute the normals of what moved, and keep the\n"
            "chunked index in step.")
        .def("rebuild_index", &mesh::DynamicSculptor::rebuild_index,
             "Rebuild the chunked index. BETWEEN strokes, never mid-drag: a refit\n"
             "stays correct and does not stay fast, and a rebuild is not\n"
             "automatically an improvement.")
        .def_prop_ro("chunk_count",
                     [](const mesh::DynamicSculptor& s) { return s.bvh().leaf_count(); })
        .def_prop_ro("dirty_chunks",
                     [](const mesh::DynamicSculptor& s) {
                         const std::vector<std::uint32_t>& d = s.bvh().dirty_leaves();
                         return std::vector<std::uint32_t>(d.begin(), d.end());
                     },
                     "The chunks the stamps since the last clear touched.")
        .def("clear_dirty", [](mesh::DynamicSculptor& s) { s.bvh().clear_dirty(); })
        .def(
            "memory_ledger",
            [](const mesh::DynamicSculptor& s) {
                memory::MemoryLedger ledger;
                mesh::report_surface_memory(s, &ledger);
                return ledger_dict(ledger);
            },
            "What this surface costs, by category, with the three roll-ups a\n"
            "host under pressure can act on.")
        .def(
            "trim",
            [](mesh::DynamicSculptor& s, memory::Pressure pressure, PyMemoryPin* pin) {
                return trim_dict(
                    mesh::trim_surface(s, pressure, pin ? &pin->gate : nullptr));
            },
            "pressure"_a, "pin"_a.none() = nb::none(),
            "Release rebuildable caches at a stated pressure and report what\n"
            "went.\n\n"
            "NEVER THE SURFACE ITSELF, and never the tree: an adaptive surface's\n"
            "index is its only means of asking a local question, and releasing\n"
            "it would not save a host memory so much as make the next dab a scan\n"
            "over every face. What goes is the chunk arena's SLACK. Nor is the\n"
            "slot pool's slack released: rebuilding it renumbers slots, so the\n"
            "surface would come back identical as a surface and different as a\n"
            "partition — and what a host had already uploaded would be addressed\n"
            "by ids that no longer mean the same chunks.\n\n"
            "A held pin makes this a no-op that reports what it WOULD have\n"
            "released.");

    // -- multiresolution surfaces (add-mesh-multires) -------------------------
    nb::class_<mesh::MultiresSurface>(
        m, "MultiresSurface",
        "A base cage, a deterministic Catmull-Clark hierarchy over it, and\n"
        "per-level detail that survives an edit to the form beneath it — so an\n"
        "artist can add wrinkles at a fine level, change the skull underneath\n"
        "them, and come back to find the wrinkles still there and attached.\n\n"
        "    P(0) = the cage\n"
        "    S(n) = Subdivide(P(n-1))\n"
        "    P(n) = S(n) + Frame(n) * Detail(n)\n\n"
        "A THIRD REPRESENTATION beside the fixed mesh and the adaptive surface,\n"
        "never a mode either of them slips into. What may change is what tells\n"
        "them apart: fixed topology never changes, an adaptive surface's changes\n"
        "locally, and this one's changes only by subdivision.")
        .def_static(
            "from_mesh",
            [](const PyMesh& handle, float weld_epsilon, std::uint64_t memory_budget) {
                mesh::MultiresOptions options;
                if (weld_epsilon > 0.0f) options.weld_epsilon = weld_epsilon;
                options.memory_budget = memory_budget;
                mesh::MultiresError err = mesh::MultiresError::None;
                auto built = mesh::MultiresSurface::from_mesh(handle.data(), options, &err);
                if (!built) throw std::invalid_argument(mesh::multires_error_text(err));
                return std::move(*built);
            },
            "mesh"_a, "weld_epsilon"_a = 0.0f, "memory_budget"_a = 0,
            "Build a hierarchy of ONE level — the cage itself. Refuses rather\n"
            "than repairs: a conversion that quietly welds a face changes the\n"
            "retopology somebody paid for without saying so.")
        .def_prop_ro("level_count", &mesh::MultiresSurface::level_count)
        .def_prop_ro("max_level", &mesh::MultiresSurface::max_level)
        .def_prop_ro("base_vertex_count", &mesh::MultiresSurface::base_vertex_count)
        .def_prop_rw(
            "sculpt_level", &mesh::MultiresSurface::sculpt_level,
            [](mesh::MultiresSurface& s, std::uint32_t level) {
                if (!s.set_sculpt_level(level)) throw std::invalid_argument("no such level");
            },
            "Where the brush writes. Independent of `display_level`, which is\n"
            "the workflow the feature exists for: move the broad form while\n"
            "watching the fine surface.")
        .def_prop_rw(
            "display_level", &mesh::MultiresSurface::display_level,
            [](mesh::MultiresSurface& s, std::uint32_t level) {
                if (!s.set_display_level(level)) throw std::invalid_argument("no such level");
            },
            "What a host draws.")
        .def(
            "level_counts",
            [](const mesh::MultiresSurface& s, std::uint32_t level) {
                if (level >= s.level_count()) throw std::invalid_argument("no such level");
                nb::dict out;
                out["vertices"] = s.topology_at(level).vertex_count;
                out["faces"] = s.topology_at(level).face_count;
                return out;
            },
            "level"_a)
        .def(
            "preflight_add_level",
            [](const mesh::MultiresSurface& s) {
                const mesh::MultiresPreflight p = s.preflight_add_level();
                nb::dict out;
                out["level"] = p.level;
                out["vertices"] = p.vertices;
                out["faces"] = p.faces;
                out["topology_bytes"] = p.topology_bytes;
                out["detail_bytes"] = p.detail_bytes;
                out["evaluated_bytes"] = p.evaluated_bytes;
                out["runtime_bytes"] = p.runtime_bytes;
                out["persistent_bytes"] = p.persistent_bytes;
                out["peak_bytes"] = p.peak_bytes;
                out["allowed"] = p.allowed;
                out["error"] = std::string(mesh::multires_error_text(p.error));
                return out;
            },
            "What adding a level WOULD cost, asked before any of it is paid.\n"
            "Catmull-Clark multiplies faces by four, and on a memory-constrained\n"
            "device it is the peak allocation that kills an app rather than the\n"
            "steady state. Allocates nothing and has no side effects.")
        .def(
            "add_level",
            [](mesh::MultiresSurface& s) {
                mesh::MultiresError err = mesh::MultiresError::None;
                nb::gil_scoped_release release;
                if (!s.add_level(&err)) {
                    nb::gil_scoped_acquire acquire;
                    throw std::invalid_argument(mesh::multires_error_text(err));
                }
            },
            "Add one level. BUILD-THEN-PUBLISH: a refusal leaves the surface\n"
            "exactly as it was. Sets the sculpt and display levels to the new\n"
            "one, which is what an artist means by 'subdivide'.")
        .def(
            "remove_highest_level",
            [](mesh::MultiresSurface& s) {
                mesh::MultiresError err = mesh::MultiresError::None;
                if (!s.remove_highest_level(&err))
                    throw std::invalid_argument(mesh::multires_error_text(err));
            },
            "Drop the highest level and the detail on it. DESTRUCTIVE.")
        .def(
            "mesh_at_level",
            [](mesh::MultiresSurface& s, std::uint32_t level) {
                if (level >= s.level_count()) throw std::invalid_argument("no such level");
                PyMesh out;
                {
                    nb::gil_scoped_release release;
                    out.m = s.mesh_at_level(level);
                }
                return out;
            },
            "level"_a,
            "The level as an ordinary mesh, with the cage's attributes\n"
            "subdivided over their own connectivity so a UV seam is\n"
            "interpolated along itself and never across itself.")
        .def(
            "positions_at",
            [](nb::object self, std::uint32_t level) {
                mesh::MultiresSurface& s = nb::cast<mesh::MultiresSurface&>(self);
                if (level >= s.level_count()) throw std::invalid_argument("no such level");
                return f3_view(self, s.positions_at(level));
            },
            "level"_a, "P(n): the level as the artist sees it.")
        .def(
            "subdivided_at",
            [](nb::object self, std::uint32_t level) {
                mesh::MultiresSurface& s = nb::cast<mesh::MultiresSurface&>(self);
                if (level >= s.level_count()) throw std::invalid_argument("no such level");
                return f3_view(self, s.subdivided_at(level));
            },
            "level"_a,
            "S(n): the pure subdivision, with no detail applied. What a\n"
            "coefficient is measured FROM, and why it survives an edit below it.")
        .def(
            "normals_at",
            [](nb::object self, std::uint32_t level) {
                mesh::MultiresSurface& s = nb::cast<mesh::MultiresSurface&>(self);
                if (level >= s.level_count()) throw std::invalid_argument("no such level");
                return f3_view(self, s.normals_at(level));
            },
            "level"_a)
        .def(
            "detail_at",
            [](mesh::MultiresSurface& s, std::uint32_t level) {
                if (level >= s.level_count()) throw std::invalid_argument("no such level");
                const mesh::DetailField& field = s.detail_at(level);
                const std::size_t n = field.vertex_count();
                auto* values = new std::vector<float>(n * 3);
                for (std::size_t v = 0; v < n; ++v) {
                    const mesh::LocalDetail d = field.get(static_cast<std::uint32_t>(v));
                    (*values)[v * 3 + 0] = d.tangent;
                    (*values)[v * 3 + 1] = d.bitangent;
                    (*values)[v * 3 + 2] = d.normal;
                }
                nb::capsule owner(values,
                                  [](void* p) noexcept { delete static_cast<std::vector<float>*>(p); });
                return nb::cast(
                    nb::ndarray<nb::numpy, float>(values->data(), {n, std::size_t(3)}, owner));
            },
            "level"_a,
            "The level's coefficients, as an (N, 3) array of tangent,\n"
            "bitangent and normal. Zero where the artist has not been.")
        .def(
            "set_detail",
            [](mesh::MultiresSurface& s, std::uint32_t level, std::uint32_t vertex,
               float tangent, float bitangent, float normal) {
                if (level == 0 || level >= s.level_count())
                    throw std::invalid_argument(
                        "the cage stores positions rather than coefficients; use level >= 1");
                s.set_detail(level, vertex, mesh::LocalDetail{tangent, bitangent, normal});
            },
            "level"_a, "vertex"_a, "tangent"_a, "bitangent"_a, "normal"_a)
        .def_prop_ro("detail_checksum", &mesh::MultiresSurface::detail_checksum,
                     "A hash of every level's authoritative detail, so a caller\n"
                     "can prove that releasing the caches changed nothing.")
        .def_prop_ro("base_revision", &mesh::MultiresSurface::base_revision)
        .def_prop_ro("detail_revision", &mesh::MultiresSurface::detail_revision)
        .def_prop_ro("evaluated_revision", &mesh::MultiresSurface::evaluated_revision)
        .def_prop_ro("dirty_patches",
                     [](const mesh::MultiresSurface& s) {
                         const std::vector<std::uint32_t>& d = s.dirty_patches();
                         return std::vector<std::uint32_t>(d.begin(), d.end());
                     },
                     "The BASE PATCHES the stamps since the last clear touched —\n"
                     "the unit a host uploads by, because a base face owns a\n"
                     "subtree that never moves between faces.")
        .def("clear_dirty", &mesh::MultiresSurface::clear_dirty)
        .def(
            "memory",
            [](const mesh::MultiresSurface& s) {
                const mesh::MultiresMemory mem = s.memory();
                nb::dict out;
                out["base"] = mem.base;
                out["topology"] = mem.topology;
                out["detail"] = mem.detail;
                out["authoritative"] = mem.authoritative;
                out["evaluated"] = mem.evaluated;
                out["runtime_index"] = mem.runtime_index;
                out["rebuildable"] = mem.rebuildable;
                out["total"] = mem.total;
                out["resident_levels"] = mem.resident_levels;
                return out;
            },
            "What the hierarchy costs, split by what a host under pressure may\n"
            "act on. Authoritative detail is NEVER reported as rebuildable.")
        .def("drop_inactive_caches", &mesh::MultiresSurface::drop_inactive_caches,
             "Release the rebuildable caches of the levels nothing is using.\n"
             "Rebuilding them reproduces the surface bit-identically.")
        .def(
            "eval_stats",
            [](const mesh::MultiresSurface& s) {
                const mesh::MultiresEvalStats st = s.eval_stats();
                nb::dict out;
                out["vertices_evaluated"] = st.vertices_evaluated;
                out["normals_recomputed"] = st.normals_recomputed;
                out["full_level_rebuilds"] = st.full_level_rebuilds;
                out["partial_level_updates"] = st.partial_level_updates;
                return out;
            },
            "What the last evaluations actually did, so 'propagation is local'\n"
            "is a measurement rather than a claim.")
        .def("reset_eval_stats", &mesh::MultiresSurface::reset_eval_stats)
        .def(
            "project_from",
            [](mesh::MultiresSurface& s, const PyMesh& reference, float max_distance,
               bool normal_ray_first, float strength) {
                mesh::ProjectOptions options;
                options.max_distance = max_distance;
                options.normal_ray_first = normal_ray_first;
                if (strength > 0.0f) options.strength = strength;
                mesh::ProjectReport report;
                bool ok = false;
                {
                    nb::gil_scoped_release release;
                    ok = s.project_from(reference.data(), options, &report);
                }
                if (!ok)
                    throw std::invalid_argument(
                        "the hierarchy has no level above its cage to project");
                nb::dict out;
                out["moved"] = report.moved;
                out["missed"] = report.missed;
                out["by_ray"] = report.by_ray;
                out["by_closest"] = report.by_closest;
                out["max_offset"] = report.max_offset;
                out["mean_offset"] = report.mean_offset;
                return out;
            },
            "reference"_a, "max_distance"_a = 0.0f, "normal_ray_first"_a = true,
            "strength"_a = 1.0f,
            "Fit every level to a sculpt made somewhere else, coarse first —\n"
            "the supported route by which a hierarchy accepts a new cage.")
        .def("serialize",
             [](const mesh::MultiresSurface& s) {
                 const std::vector<std::uint8_t> bytes = s.encode();
                 return nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
             },
             "The hierarchy as bytes: the cage, the rule, the level count, the\n"
             "active levels and each level's detail. The face lists and every\n"
             "evaluated position follow from those and are not written.")
        .def_static("deserialize",
                    [](nb::bytes data) {
                        mesh::MultiresSurface out;
                        if (!mesh::MultiresSurface::decode(
                                reinterpret_cast<const std::uint8_t*>(data.c_str()), data.size(),
                                &out))
                            throw std::invalid_argument(
                                "not a multiresolution surface, or from a newer writer");
                        return out;
                    },
                    "data"_a)
        .def_prop_rw(
            "memory_profile",
            [](const mesh::MultiresSurface& s) { return s.memory_profile(); },
            [](mesh::MultiresSurface& s, const memory::SculptMemoryProfile& p) {
                s.set_memory_profile(p);
            },
            "What the HOST is willing to spend. Filled by the host and never\n"
            "detected: the portable core makes no platform call and branches on\n"
            "no device model, so constrained behaviour is exercised by a desktop\n"
            "test in three lines.\n\n"
            "Setting it takes effect immediately and then at every residency\n"
            "change the host itself causes, which is the only moment this class\n"
            "releases anything on its own. On a constrained profile the sculpt\n"
            "and display levels stay resident and every other level keeps its\n"
            "authoritative detail alone.")
        .def(
            "memory_ledger",
            [](const mesh::MultiresSurface& s) {
                memory::MemoryLedger ledger;
                mesh::report_surface_memory(s, &ledger);
                return ledger_dict(ledger);
            },
            "The same figures `memory()` reports, in the vocabulary the other\n"
            "two representations answer in, with the three roll-ups a host under\n"
            "pressure can act on.")
        .def(
            "trim",
            [](mesh::MultiresSurface& s, memory::Pressure pressure, PyMemoryPin* pin) {
                return trim_dict(
                    mesh::trim_surface(s, pressure, pin ? &pin->gate : nullptr));
            },
            "pressure"_a, "pin"_a.none() = nb::none(),
            "Release rebuildable caches at a stated pressure, in a fixed order,\n"
            "and report what went.\n\n"
            "NEVER AUTHORITATIVE CONTENT: not the cage, not a level's topology,\n"
            "not the detail. `detail_checksum` is unchanged across any trim, and\n"
            "that is how a host proves it rather than taking this sentence on\n"
            "trust. A held pin makes this a no-op that reports what it WOULD\n"
            "have released.")
        .def(
            "preflight_encode",
            [](const mesh::MultiresSurface& s, std::uint64_t budget) {
                return preflight_dict(mesh::preflight_encode(s, budget));
            },
            "budget"_a = 0,
            "What serializing this hierarchy WOULD cost. The blob is a second\n"
            "copy of everything and it exists while the hierarchy still does, so\n"
            "read `peak_bytes`.");

    nb::class_<mesh::MultiresSculptor>(
        m, "MultiresSculptor",
        "The brush engine over a hierarchy: the same verbs, the same falloffs,\n"
        "the same mask and the same deformation math — the fixed sculptor run\n"
        "over the active level's own mesh, so a brush means one thing on every\n"
        "representation.\n\n"
        "What this owns is the step that turns the moved positions back into\n"
        "what the hierarchy stores: the cage's geometry at level 0, detail\n"
        "coefficients in the transported frame above it.\n\n"
        "The surface must outlive the sculptor.")
        .def("__init__",
             [](mesh::MultiresSculptor* self, mesh::MultiresSurface& surface) {
                 new (self) mesh::MultiresSculptor(surface);
             },
             "surface"_a, nb::keep_alive<1, 2>())
        .def(
            "stamp",
            [](mesh::MultiresSculptor& self, const std::string& verb, nb::handle center,
               float radius, float strength, const std::string& falloff, nb::handle direction,
               nb::handle mask, bool geodesic, int smooth_iterations, nb::handle alpha,
               nb::handle alpha_direction, nb::handle alpha_tangent, float alpha_extent) {
                mesh::MeshBrush chosen = mesh::MeshBrush::Draw;
                mesh::MeshBrushSettings settings = mesh_brush_settings(
                    verb, center, radius, strength, falloff, direction, nb::none(),
                    nb::cast(geodesic), nb::none(), "two_sided", nb::none(), nb::none(), 0.2f,
                    smooth_iterations, 0.0f, alpha, alpha_direction, alpha_tangent, alpha_extent,
                    nb::none(), &chosen);
                field::MaskGate gate = mask_gate_of(mask);
                nb::gil_scoped_release release;
                return self.stamp(chosen, settings, gate, nullptr);
            },
            "verb"_a, "center"_a, "radius"_a, "strength"_a = 0.5f, "falloff"_a = "smooth",
            "direction"_a = nb::none(), "mask"_a = nb::none(), "geodesic"_a = true,
            "smooth_iterations"_a = 1, "alpha"_a = nb::none(),
            "alpha_direction"_a = nb::none(), "alpha_tangent"_a = nb::none(),
            "alpha_extent"_a = 0.0f,
            "One stamp at the surface's current sculpt level. Returns how many\n"
            "weld classes moved.")
        .def("begin_stroke", &mesh::MultiresSculptor::begin_stroke,
             "Start a gesture. Clears the record the Layer verb measures its\n"
             "ceiling against.")
        .def(
            "apply_stroke",
            [](mesh::MultiresSculptor& self, nb::handle samples, const brush::StrokePreset& preset,
               const std::string& verb, const std::string& falloff, float strength,
               nb::handle geodesic, int smooth_iterations, float layer_height, nb::handle mask,
               bool defer_normals) {
                mesh::MeshBrush chosen = mesh::MeshBrush::Draw;
                // The radius is the STAMP's, so a placeholder goes in here —
                // the same reading the fixed sculptor's stroke path takes.
                mesh::MeshBrushSettings settings = mesh_brush_settings(
                    verb, nb::none(), 1.0f, strength, falloff, nb::none(), nb::none(), geodesic,
                    nb::none(), "two_sided", nb::none(), nb::none(), 0.2f, smooth_iterations,
                    layer_height, nb::none(), nb::none(), nb::none(), 0.0f, nb::none(), &chosen);
                const std::vector<brush::StrokeSample> in = to_stroke_samples(samples);
                const voxel::MaskField* field_mask = borrow_mask(mask);
                brush::MeshStrokeOptions options;
                options.defer_normals = defer_normals;
                nb::gil_scoped_release release;
                return brush::apply_to_multires(self, brush::resolve_stroke(in, preset), chosen,
                                                settings, field_mask, nullptr, options);
            },
            "samples"_a, "preset"_a, "verb"_a, "falloff"_a = "smooth", "strength"_a = 1.0f,
            "geodesic"_a = nb::none(), "smooth_iterations"_a = 1, "layer_height"_a = 0.05f,
            "mask"_a = nb::none(), "defer_normals"_a = false,
            "A whole stroke at the active sculpt level, resolved into spaced\n"
            "stamps by the same engine that drives a mesh layer — so a stamp\n"
            "lands in the same place with the same radius and the same\n"
            "pressure-scaled strength on either representation.")
        .def_prop_ro("bound_level", &mesh::MultiresSculptor::bound_level)
        .def_prop_ro("last_write_vertices",
                     [](const mesh::MultiresSculptor& s) {
                         const std::vector<std::uint32_t>& v = s.last_write_vertices();
                         return std::vector<std::uint32_t>(v.begin(), v.end());
                     },
                     "The level vertices the last stamp actually moved.");

    // -- the surface tier: budgets, the pin and one transport -----------------
    //
    // (add-extreme-poly-runtime.) The principle the whole thing serves is that
    // a dab costs approximately what it TOUCHES rather than what the model
    // HOLDS, and these are the three things a host needs across this boundary
    // to keep it that way at twenty million vertices.

    nb::enum_<memory::MemoryClass>(
        m, "MemoryClass",
        "How much room the host is working in. A LABEL RATHER THAN A DEVICE:\n"
        "nothing here detects anything, so a desktop test sets `constrained`\n"
        "and observes constrained behaviour where the tests actually run.")
        .value("full", memory::MemoryClass::Full,
               "No budget. Every byte field is advisory and the runtime keeps\n"
               "what it builds — what a desktop host and most tests want.")
        .value("constrained", memory::MemoryClass::Constrained,
               "Budgets are real, inactive levels hold compact detail only, and\n"
               "maintenance runs between interactions.")
        .value("minimal", memory::MemoryClass::Minimal,
               "What a host sets when the operating system has already warned\n"
               "it once: everything rebuildable is a candidate the moment it is\n"
               "not being read.");

    nb::enum_<memory::Pressure>(
        m, "Pressure",
        "How hard a host is asking for memory back. Passed to `trim`, and\n"
        "never inferred by the engine — the host owns the moment, the engine\n"
        "owns the order.")
        .value("none", memory::Pressure::None,
               "Give back what is free anyway: scratch above its steady\n"
               "capacity, and preview staging already drained.")
        .value("warning", memory::Pressure::Warning)
        .value("urgent", memory::Pressure::Urgent)
        .value("critical", memory::Pressure::Critical,
               "The last stop before the operating system kills the process.\n"
               "Everything rebuildable goes and the next edit pays to rebuild\n"
               "what it needs.");

    nb::enum_<mesh::SurfaceKind>(
        m, "SurfaceKind",
        "Which representation a SurfaceView is over. It changes exactly one\n"
        "thing a caller can see, and copy_chunk says which.")
        .value("fixed", mesh::SurfaceKind::Fixed)
        .value("adaptive", mesh::SurfaceKind::Adaptive)
        .value("multires", mesh::SurfaceKind::Multires);

    nb::class_<memory::SculptMemoryProfile>(
        m, "SculptMemoryProfile",
        "What a host is willing to spend, filled by the HOST.\n\n"
        "NO DEVICE DETECTION ANYWHERE. Not one platform call, not one\n"
        "model-name comparison. A host knows what its operating system is\n"
        "telling it; an engine guessing from a model string is both wrong and,\n"
        "worse, untestable — because the tests run on a desktop.\n\n"
        "EVERY FIELD IS A HINT, AND THE TYPE IS WHAT SAYS SO. Each one names\n"
        "something that can be recomputed EXACTLY from what was committed:\n"
        "normals during a drag, index quality, cache residency, the rate a\n"
        "preview drains. There is deliberately no field for anything that IS\n"
        "the committed result — the deformation, split and collapse\n"
        "thresholds, remesh targets, detail coefficients, layer content,\n"
        "masks, brush strength and falloff. A deferred split would make the\n"
        "committed mesh a function of machine speed, and this library spends\n"
        "real effort on determinism that would throw away. So 'a memory-saving\n"
        "mode changed my sculpt' is unrepresentable here rather than merely\n"
        "forbidden.\n\n"
        "A byte field of 0 means no budget for that thing, which is what\n"
        "`full` means field by field.")
        .def(nb::init<>())
        .def_rw("memory_class", &memory::SculptMemoryProfile::memory_class)
        .def_rw("cache_budget", &memory::SculptMemoryProfile::cache_budget,
                "Rebuildable caches: chunk indices, per-level runtime caches,\n"
                "evaluated layer caches, derived positions. What a trim reaches\n"
                "for first.")
        .def_rw("undo_budget", &memory::SculptMemoryProfile::undo_budget,
                "The engine never trims this on its own; the figure exists so a\n"
                "host can set its own history budget from the same struct.")
        .def_rw("scratch_budget", &memory::SculptMemoryProfile::scratch_budget,
                "The per-stamp working set, and a HARD bound: a footprint\n"
                "larger than it is processed in blocks rather than allocated,\n"
                "which is what stops a 500k-vertex footprint on a constrained\n"
                "profile from becoming the peak that kills the app.")
        .def_rw("preview_budget", &memory::SculptMemoryProfile::preview_budget)
        .def_rw("max_resident_levels", &memory::SculptMemoryProfile::max_resident_levels,
                "How many multires levels keep their rebuildable caches; 0 is\n"
                "no limit.")
        .def_rw("defer_normals_in_stroke",
                &memory::SculptMemoryProfile::defer_normals_in_stroke,
                "Recompute exact normals at stroke end rather than per stamp.\n"
                "The final state is exact either way — that is the gate — and\n"
                "this only decides when the work happens.")
        .def_rw("allow_index_rebuild", &memory::SculptMemoryProfile::allow_index_rebuild,
                "Whether a spatial index may be REBUILT, never whether it may\n"
                "be refitted: a refit is correctness. Defaults to True and\n"
                "stays advisory, because a rebuild helped one of five measured\n"
                "deformations and hurt two.")
        .def_rw("preview_chunks_per_frame",
                &memory::SculptMemoryProfile::preview_chunks_per_frame,
                "How many dirty chunks a host expects to drain per frame; 0\n"
                "means 'as many as there are'. Lossless at any value, because\n"
                "the transport acknowledges per chunk.");

    nb::class_<PyMemoryPin>(
        m, "MemoryPin",
        "Hold this across a save or a readback and a trim becomes a no-op that\n"
        "reports what it WOULD have released.\n\n"
        "A CONTEXT MANAGER, and that is the point of it:\n\n"
        "    with MemoryPin() as pin:\n"
        "        surface.trim(Pressure.critical, pin)   # releases nothing\n"
        "        blob = surface.serialize()\n\n"
        "`with` is the only form that cannot leave a document pinned when the\n"
        "body raises, and a document pinned forever is one no trim can ever\n"
        "help. REENTRANT, because a readback inside a save must not un-pin the\n"
        "save when it returns.")
        .def(nb::init<>())
        .def("acquire",
             [](PyMemoryPin& p) {
                 p.held.push_back(std::make_unique<memory::MemoryPin>(p.gate));
             },
             "Hold it by hand. Prefer the `with` block, which cannot be\n"
             "unbalanced by an exception.")
        .def("release",
             [](PyMemoryPin& p) {
                 if (!p.held.empty()) p.held.pop_back();
             },
             "Releasing one nobody acquired does nothing: an unbalanced release\n"
             "is a caller's bug, and leaving the count at zero is the harmless\n"
             "reading of it. Underflowing to 'pinned forever' is not.")
        .def_prop_ro("held", [](const PyMemoryPin& p) { return p.gate.pinned(); },
                     "Whether a trim would do nothing right now.")
        .def("__enter__",
             [](nb::object self) {
                 PyMemoryPin& p = nb::cast<PyMemoryPin&>(self);
                 p.held.push_back(std::make_unique<memory::MemoryPin>(p.gate));
                 return self;
             })
        .def("__exit__",
             // Variadic: the three arguments are None on a clean exit, and a
             // typed signature would refuse them.
             [](PyMemoryPin& p, nb::args) {
                 // Releases whether the block finished or threw, which is the
                 // whole reason to bind this as a context manager.
                 if (!p.held.empty()) p.held.pop_back();
                 return false;  // an exception in the body propagates
             });

    nb::class_<PySurfaceView>(
        m, "SurfaceView",
        "What changed, and those bytes and no others — for whichever of the\n"
        "three representations you are holding.\n\n"
        "A host at twenty million vertices asks the same three questions\n"
        "whatever the surface is, and answering them per representation is\n"
        "three code paths whose dirty sets mean different things: a weld class,\n"
        "a face chunk and a base patch are not interchangeable, and a host that\n"
        "treated them as such would upload the wrong thing. So there is ONE\n"
        "chunk unit underneath and this is the seam over it.\n\n"
        "FOUR REVISIONS, NOT ONE — topology, geometry, normals, attributes — so\n"
        "an index buffer is re-uploaded only when connectivity actually\n"
        "changed, and a deferred normal flush is distinguishable from a move.\n\n"
        "AN ACKNOWLEDGEMENT RATHER THAN A CLEAR. `clear_dirty` is\n"
        "all-or-nothing: a host that drains half a set and drops a frame must\n"
        "either re-upload everything or lose a change. `acknowledge` retires a\n"
        "chunk only if it has not changed since you copied it, so draining\n"
        "across frames is lossless at any rate.\n\n"
        "A CALL-SITE CONVENIENCE, NOT A HANDLE TO STORE. It names a surface it\n"
        "does not own; the surface must outlive it, and everything it reports\n"
        "is read at the moment it is asked rather than cached.")
        .def_static(
            "over_mesh",
            [](nb::object mesh_obj, std::size_t target_faces) {
                PyMesh& handle = nb::cast<PyMesh&>(mesh_obj);
                PySurfaceView v;
                v.owner = mesh_obj;
                v.kind = mesh::SurfaceKind::Fixed;
                v.mesh_handle = &handle;
                mesh::ChunkOptions options;
                if (target_faces > 0) {
                    options.target_faces = target_faces;
                    options.min_faces = target_faces / 4 ? target_faces / 4 : 1;
                    options.max_faces = target_faces * 2;
                }
                mesh::partition_mesh_chunks(handle.data(), options, &v.table);
                return v;
            },
            "mesh"_a, "target_faces"_a = 0,
            "A flat mesh, partitioned on the spot. The partition is this\n"
            "view's, because the fixed sculptor tracks dirty WELD CLASSES and\n"
            "whether that list is retired in favour of a chunk dirty set\n"
            "depends on a measurement this change has not made yet — so this\n"
            "view reports one partition of a static mesh and an empty dirty\n"
            "set. The other two carry live dirty sets.\n\n"
            "`target_faces` of 0 takes the library's defaults, which are\n"
            "explicitly the UNMEASURED null hypothesis and not yet an answer.")
        .def_static(
            "over_dynamic",
            [](nb::object sculptor_obj) {
                mesh::DynamicSculptor& s = nb::cast<mesh::DynamicSculptor&>(sculptor_obj);
                PySurfaceView v;
                v.owner = sculptor_obj;
                v.kind = mesh::SurfaceKind::Adaptive;
                v.dynamic_sculptor = &s;
                return v;
            },
            "sculptor"_a, "The adaptive surface, whose table is its chunked index.")
        .def_static(
            "over_level",
            [](nb::object surface_obj, std::uint32_t level) {
                mesh::MultiresSurface& s = nb::cast<mesh::MultiresSurface&>(surface_obj);
                if (level >= s.level_count())
                    throw std::invalid_argument("no such level in this hierarchy");
                PySurfaceView v;
                v.owner = surface_obj;
                v.kind = mesh::SurfaceKind::Multires;
                v.multires = &s;
                v.level = level;
                return v;
            },
            "surface"_a, "level"_a,
            "One level of a hierarchy. Asking for its chunks EVALUATES the\n"
            "level, exactly as reading its positions does.")
        .def_prop_ro("kind", [](PySurfaceView& v) { return v.kind; })
        .def_prop_ro("chunk_count", [](PySurfaceView& v) { return v.view().chunk_count(); },
                     "Chunk ids run from 0 to this, and a slot in that range may\n"
                     "be dead — read `live` from chunk_info.")
        .def_prop_ro(
            "dirty_chunks",
            [](PySurfaceView& v) {
                // The view is a temporary and the dirty set is a reference INTO
                // it, so the view has to outlive the copy.
                mesh::SurfaceView s = v.view();
                const std::vector<std::uint32_t>& d = s.dirty_chunks();
                return std::vector<std::uint32_t>(d.begin(), d.end());
            },
            "The chunks the stamps since the last drain touched, in the order\n"
            "they were first marked.\n\n"
            "MAY NAME A CHUNK THAT HAS SINCE BEEN RELEASED, whose chunk_info\n"
            "reports live=False. Retiring a merged chunk from this list would\n"
            "be an erase on a path that runs during a stroke, to save a check\n"
            "that does not — so skip it rather than treating it as an error.")
        .def(
            "chunk_info",
            [](PySurfaceView& v, std::uint32_t chunk) {
                mesh::SurfaceView s = v.view();
                nb::dict out;
                const mesh::SurfaceChunk* record = s.chunks().chunk(chunk);
                out["chunk"] = chunk;
                out["live"] = record != nullptr;
                if (record == nullptr) return out;
                // The capacity query answers the counts rather than arithmetic
                // repeated here: welded and unwelded chunks count differently,
                // and a second copy of that rule is how the two come to
                // disagree about how much a caller should allocate.
                const mesh::ChunkReadback r =
                    s.copy_chunk(chunk, nullptr, nullptr, 0, nullptr, 0, nullptr, 0);
                out["vertex_count"] = r.vertex_count;
                out["index_count"] = r.index_count;
                out["revision"] = record->revision;
                out["revisions"] = revisions_dict(record->revisions);
                out["geometry_dirty"] = record->geometry_dirty;
                out["topology_dirty"] = record->topology_dirty;
                if (!record->bounds.empty()) {
                    out["bounds_min"] = nb::make_tuple(record->bounds.min.x,
                                                       record->bounds.min.y,
                                                       record->bounds.min.z);
                    out["bounds_max"] = nb::make_tuple(record->bounds.max.x,
                                                       record->bounds.max.y,
                                                       record->bounds.max.z);
                }
                return out;
            },
            "chunk"_a,
            "What one chunk holds and what it costs to copy, without copying\n"
            "it.")
        .def(
            "copy_chunk",
            [](PySurfaceView& v, std::uint32_t chunk, nb::handle expected, bool normals) {
                mesh::SurfaceView s = v.view();
                const bool has_expected = !expected.is_none();
                const mesh::ChunkRevisions want = to_revisions(expected, "expected");
                const mesh::ChunkRevisions* want_p = has_expected ? &want : nullptr;

                const mesh::ChunkReadback need =
                    s.copy_chunk(chunk, want_p, nullptr, 0, nullptr, 0, nullptr, 0);
                if (!need.ok) throw std::invalid_argument("no live chunk with that id");
                const std::size_t nv = need.vertex_count, ni = need.index_count;

                auto own_f = [](void* q) noexcept { delete[] static_cast<float*>(q); };
                auto own_u = [](void* q) noexcept {
                    delete[] static_cast<std::uint32_t*>(q);
                };
                // Each buffer is handed to its capsule the moment it exists, so
                // a throw between here and the return frees it rather than
                // leaking it into the interpreter.
                float* pos = new float[nv ? nv * 3 : 1];
                nb::capsule pos_owner(pos, own_f);
                std::uint32_t* idx = new std::uint32_t[ni ? ni : 1];
                nb::capsule idx_owner(idx, own_u);

                nb::dict out;
                float* nor = nullptr;
                if (normals) {
                    nor = new float[nv ? nv * 3 : 1];
                    nb::capsule nor_owner(nor, own_f);
                    out["normals"] =
                        nb::ndarray<nb::numpy, float>(nor, {nv, std::size_t(3)}, nor_owner);
                }

                const mesh::ChunkReadback got =
                    s.copy_chunk(chunk, want_p, pos, nv * 3, nor, nor ? nv * 3 : 0, idx, ni);
                if (got.truncated)
                    throw std::runtime_error(
                        "the chunk changed size between the query and the copy; ask again");

                out["positions"] = nb::ndarray<nb::numpy, float>(pos, {nv, std::size_t(3)},
                                                                 pos_owner);
                out["indices"] = nb::ndarray<nb::numpy, std::uint32_t>(idx, {ni}, idx_owner);
                out["revisions"] = revisions_dict(got.current);
                out["requested"] = revisions_dict(got.requested);
                // The engine moved on after you took your snapshot. The data is
                // CURRENT — this is not a failure — but a caller applying an
                // older frame's plan can tell that its plan is out of date.
                out["stale"] = got.stale;
                return out;
            },
            "chunk"_a, "expected"_a.none() = nb::none(), "normals"_a = true,
            "One chunk's geometry as numpy arrays: positions (N, 3), normals\n"
            "(N, 3) and triangle indices (M,) LOCAL to the chunk, so it draws\n"
            "as a standalone mesh.\n\n"
            "`expected` is a revisions dict a previous copy returned; pass it\n"
            "to learn whether what you get back has already been superseded.\n\n"
            "WELDED WHERE THE REPRESENTATION ALLOWS IT. A fixed mesh and a\n"
            "multires level have a stable per-chunk vertex list, so a chunk\n"
            "copies as its own vertices. An adaptive surface's topology changes\n"
            "under the stamp being uploaded, so its chunks copy as UNWELDED\n"
            "triangles — read the array shapes rather than assuming either.")
        .def(
            "acknowledge",
            [](PySurfaceView& v, std::uint32_t chunk, nb::handle seen) {
                const mesh::ChunkRevisions r = to_revisions(seen, "seen");
                mesh::ChunkTable* table = v.writable_table();
                if (table != nullptr) return table->acknowledge(chunk, r);
                return v.multires->acknowledge_chunk(v.level, chunk, r);
            },
            "chunk"_a, "seen"_a,
            "Retire one chunk from the dirty set, and ONLY if it has not\n"
            "changed since you copied it. `seen` is the revisions dict that\n"
            "copy returned.\n\n"
            "Returns whether the chunk is clean afterwards: True when it was\n"
            "retired and True when it was never dirty, False only when it moved\n"
            "on after you read it — in which case it is still waiting and you\n"
            "have lost nothing.")
        .def(
            "clear_dirty",
            [](PySurfaceView& v) {
                mesh::ChunkTable* table = v.writable_table();
                if (table != nullptr) {
                    table->clear_dirty();
                    return;
                }
                v.multires->clear_dirty_chunks(v.level);
            },
            "Drop the whole dirty set. The all-or-nothing form, for a caller\n"
            "that uploads everything it was told about in one pass; prefer\n"
            "`acknowledge` if you drain incrementally.");

    // -- the brush model, and brushes as data ---------------------------------
    nb::class_<mesh::AutomaskSettings>(
        m, "AutomaskSettings",
        "Which gates a brush applies to itself, and how hard. Composed into\n"
        "the per-vertex weight by multiplication, and applied LAST — so a\n"
        "brush with no factors set is bit-identical to one from before\n"
        "automasking existed.")
        .def(nb::init<>())
        .def_rw("factors", &mesh::AutomaskSettings::factors,
                "AutomaskFactor values, OR-ed together")
        .def_rw("normal_angle", &mesh::AutomaskSettings::normal_angle,
                "radians; full strength up to this angle, zero at twice it")
        .def_rw("boundary_rings", &mesh::AutomaskSettings::boundary_rings)
        .def_rw("cavity_strength", &mesh::AutomaskSettings::cavity_strength);

    nb::class_<mesh::MeshBrushSettings>(
        m, "MeshBrushSettings",
        "A brush's own settings — the fields that are its identity rather\n"
        "than where a stamp landed. The alpha is deliberately absent: image\n"
        "content stays caller-owned and is passed to a stamp, never stored.")
        .def(nb::init<>())
        .def_rw("radius", &mesh::MeshBrushSettings::radius)
        .def_rw("strength", &mesh::MeshBrushSettings::strength)
        .def_rw("falloff", &mesh::MeshBrushSettings::falloff)
        .def_rw("geodesic", &mesh::MeshBrushSettings::geodesic,
                "measure the falloff along the surface rather than in a straight line")
        .def_rw("flatten_mode", &mesh::MeshBrushSettings::flatten_mode)
        .def_rw("polish_angle", &mesh::MeshBrushSettings::polish_angle)
        .def_rw("smooth_iterations", &mesh::MeshBrushSettings::smooth_iterations)
        .def_rw("layer_height", &mesh::MeshBrushSettings::layer_height)
        .def_rw("automask", &mesh::MeshBrushSettings::automask);

    nb::enum_<mesh::BrushFootprint>(m, "BrushFootprint",
                                    "How the region under the brush is REACHED.")
        .value("BALL", mesh::BrushFootprint::Ball, "everything under this disc")
        .value("SURFACE_WALK", mesh::BrushFootprint::SurfaceWalk,
               "everything reachable along the surface");

    nb::enum_<mesh::BrushFrame>(m, "BrushFrame",
                                "The direction a kernel displaces along, named rather than\n"
                                "implied. This axis is why draw and inflate are one kernel.")
        .value("NONE", mesh::BrushFrame::None)
        .value("REGION_NORMAL", mesh::BrushFrame::RegionNormal,
               "one direction for the whole stamp — draw")
        .value("VERTEX_NORMAL", mesh::BrushFrame::VertexNormal,
               "each vertex along its own — inflate")
        .value("STROKE_DIRECTION", mesh::BrushFrame::StrokeDirection)
        .value("REGION_PLANE", mesh::BrushFrame::RegionPlane);

    nb::enum_<mesh::BrushKernelId>(m, "BrushKernel",
                                   "The deformation itself: a shape of arithmetic rather than a\n"
                                   "verb. Displace serves draw and inflate under two frames.")
        .value("TRANSLATE", mesh::BrushKernelId::Translate)
        .value("DISPLACE", mesh::BrushKernelId::Displace)
        .value("GATHER", mesh::BrushKernelId::Gather)
        .value("TANGENTIAL", mesh::BrushKernelId::Tangential)
        .value("PLANE", mesh::BrushKernelId::Plane)
        .value("PLANE_DEPOSIT", mesh::BrushKernelId::PlaneDeposit)
        .value("CUT_AND_GATHER", mesh::BrushKernelId::CutAndGather)
        .value("LAPLACIAN", mesh::BrushKernelId::Laplacian)
        .value("DEPOSIT_CEILING", mesh::BrushKernelId::DepositCeiling)
        .value("COLOR_BLEND", mesh::BrushKernelId::ColorBlend)
        .value("COLOR_ADVECT", mesh::BrushKernelId::ColorAdvect);

    nb::enum_<mesh::BrushWriteTarget>(m, "BrushWriteTarget",
                                      "Which buffer a kernel writes. Exclusive on purpose.")
        .value("POSITION", mesh::BrushWriteTarget::Position)
        .value("COLOR", mesh::BrushWriteTarget::Color);

    nb::enum_<mesh::BrushPostPolicy>(m, "BrushPostPolicy")
        .value("NONE", mesh::BrushPostPolicy::None)
        .value("RECOMPUTE_NORMALS", mesh::BrushPostPolicy::RecomputeNormals);

    nb::enum_<mesh::AutomaskFactor>(m, "AutomaskFactor",
                                    "The gates a brush applies to ITSELF, composed into the\n"
                                    "per-vertex weight by multiplication rather than branched\n"
                                    "into each verb.")
        .value("NORMAL_ANGLE", mesh::AutomaskFactor::NormalAngle)
        .value("TOPOLOGY_CONNECTED", mesh::AutomaskFactor::TopologyConnected)
        .value("BOUNDARY", mesh::AutomaskFactor::Boundary)
        .value("CAVITY", mesh::AutomaskFactor::Cavity)
        .value("SURFACE_GROUP", mesh::AutomaskFactor::SurfaceGroup);

    nb::class_<mesh::BrushModel>(
        m, "BrushModel",
        "One brush, as axis values: footprint, falloff, frame, kernel, write\n"
        "target and post policy. Every verb in the vocabulary is a value of\n"
        "this type, and so is every named artist family — which is what makes\n"
        "a family a preset rather than a code path.")
        .def(nb::init<>())
        .def_prop_ro("verb",
                     [](const mesh::BrushModel& m) { return mesh_brush_name(m.verb); },
                     "the verb this model decomposes, by the name pyclay uses for it")
        .def_rw("footprint", &mesh::BrushModel::footprint)
        .def_rw("falloff", &mesh::BrushModel::falloff)
        .def_rw("frame", &mesh::BrushModel::frame)
        .def_rw("kernel", &mesh::BrushModel::kernel)
        .def_rw("target", &mesh::BrushModel::target)
        .def_rw("post", &mesh::BrushModel::post)
        .def_static("of",
                    [](const std::string& verb) { return mesh::model_of(parse_mesh_brush(verb)); },
                    "verb"_a,
                    "The axis decomposition of one verb — the table a preset chooses among.\n"
                    "Takes the verb by name, as every other mesh brush call here does.");

    nb::class_<brush::BrushPreset>(
        m, "BrushPreset",
        "A brush, as data: a name, a stroke preset and a brush model.\n\n"
        "The artist-facing families — Clay Buildup, Dam Standard, hPolish,\n"
        "Trim Dynamic, Snake Hook, Rake — are not new deformations. Each is a\n"
        "kernel plus a falloff plus a frame plus a spacing, and none has an\n"
        "engine path of its own.\n\n"
        "NO IMAGE BYTES. An alpha stays caller-owned and borrowed for a call,\n"
        "so a preset library costs kilobytes rather than megabytes.")
        .def(nb::init<>())
        .def_rw("name", &brush::BrushPreset::name)
        .def_rw("stroke", &brush::BrushPreset::stroke)
        .def_rw("model", &brush::BrushPreset::model)
        .def_rw("settings", &brush::BrushPreset::settings)
        .def_prop_ro_static("version", [](nb::handle) { return brush::kBrushPresetVersion; },
                            "the schema version serialize() writes")
        .def("serialize",
             [](const brush::BrushPreset& p) {
                 std::vector<std::uint8_t> bytes = p.serialize();
                 return nb::bytes(bytes.data(), bytes.size());
             },
             "Preset as bytes, tagged with its schema version")
        .def_static("deserialize",
                    [](nb::bytes data) {
                        auto p = brush::BrushPreset::deserialize(
                            reinterpret_cast<const std::uint8_t*>(data.c_str()), data.size());
                        if (!p)
                            throw std::invalid_argument(
                                "not a brush preset this build can read: either malformed, or "
                                "written by a newer schema version than " +
                                std::to_string(brush::kBrushPresetVersion));
                        return *p;
                    },
                    "data"_a,
                    "Load a preset. An older schema loads with defaults; a newer one "
                    "raises rather than being read as a prefix.")
        .def_static("library", &brush::reference_presets,
                    "The named families, as data. Every one is axis values over existing "
                    "kernels; none has a code path.")
        .def_static("by_name",
                    [](const std::string& name) {
                        auto p = brush::reference_preset(name);
                        if (!p) throw std::invalid_argument("no preset named " + name);
                        return *p;
                    },
                    "name"_a, "One reference preset by name.");

    nb::class_<brush::StrokePreset>(
        m, "StrokePreset",
        "How a drag becomes stamps: spacing, pressure response, jitter, taper,\n"
        "steady-stroke smoothing and accumulation.\n\n"
        "Presets carry a schema version and serialize with it. An older preset\n"
        "loads with defaults for whatever it did not carry; a newer one is\n"
        "refused rather than read as a prefix. Preset libraries outlive engine\n"
        "versions, and that is the whole reason the version is there from the\n"
        "first release rather than added later.")
        .def("__init__",
             [](brush::StrokePreset* self, float radius, float spacing, float strength,
                float pressure_size, float pressure_strength, float pressure_curve,
                float jitter_position, float jitter_size, float jitter_rotation,
                std::uint32_t seed, bool rotate_along_stroke, float taper_start, float taper_end,
                float steady, brush::Accumulation accumulation) {
                 if (!(radius > 0.0f)) throw std::invalid_argument("radius must be > 0");
                 if (!(spacing > 0.0f)) throw std::invalid_argument("spacing must be > 0");
                 new (self) brush::StrokePreset();
                 self->radius = radius;
                 self->spacing = spacing;
                 self->strength = strength;
                 self->pressure.size = pressure_size;
                 self->pressure.strength = pressure_strength;
                 self->pressure.curve = pressure_curve;
                 self->jitter_position = jitter_position;
                 self->jitter_size = jitter_size;
                 self->jitter_rotation = jitter_rotation;
                 self->seed = seed;
                 self->rotate_along_stroke = rotate_along_stroke;
                 self->taper_start = taper_start;
                 self->taper_end = taper_end;
                 self->steady = steady;
                 self->accumulation = accumulation;
             },
             "radius"_a = 0.25f, "spacing"_a = 0.25f, "strength"_a = 1.0f,
             "pressure_size"_a = 0.0f, "pressure_strength"_a = 1.0f, "pressure_curve"_a = 1.0f,
             "jitter_position"_a = 0.0f, "jitter_size"_a = 0.0f, "jitter_rotation"_a = 0.0f,
             "seed"_a = 0u, "rotate_along_stroke"_a = false, "taper_start"_a = 0.0f,
             "taper_end"_a = 0.0f, "steady"_a = 0.0f,
             "accumulation"_a = brush::Accumulation::Buildup)
        .def_rw("radius", &brush::StrokePreset::radius)
        .def_rw("spacing", &brush::StrokePreset::spacing,
                "distance between stamps as a fraction of the brush diameter")
        .def_rw("strength", &brush::StrokePreset::strength)
        .def_prop_rw("pressure_size",
                     [](const brush::StrokePreset& p) { return p.pressure.size; },
                     [](brush::StrokePreset& p, float v) { p.pressure.size = v; },
                     "how much pressure drives the radius; 0 disables the channel")
        .def_prop_rw("pressure_strength",
                     [](const brush::StrokePreset& p) { return p.pressure.strength; },
                     [](brush::StrokePreset& p, float v) { p.pressure.strength = v; })
        .def_prop_rw("pressure_curve",
                     [](const brush::StrokePreset& p) { return p.pressure.curve; },
                     [](brush::StrokePreset& p, float v) { p.pressure.curve = v; },
                     "exponent applied to pressure before either channel")
        .def_rw("jitter_position", &brush::StrokePreset::jitter_position)
        .def_rw("jitter_size", &brush::StrokePreset::jitter_size)
        .def_rw("jitter_rotation", &brush::StrokePreset::jitter_rotation)
        .def_rw("seed", &brush::StrokePreset::seed,
                "jitter is a hash of the stamp index and this, never a random source")
        .def_rw("rotate_along_stroke", &brush::StrokePreset::rotate_along_stroke)
        .def_rw("rotate_to_azimuth", &brush::StrokePreset::rotate_to_azimuth,
                "Turn each stamp to follow the STYLUS BARREL rather than the\n"
                "path — the rake and chisel brushes, where the tool's own angle\n"
                "is the point and the direction of travel is not.\n\n"
                "Wins over rotate_along_stroke where both are set: they are two\n"
                "answers to one question and a stamp cannot face two ways.")
        .def_prop_rw(
            "velocity_size",
            [](const brush::StrokePreset& p) { return p.velocity_response.size; },
            [](brush::StrokePreset& p, float v) { p.velocity_response.size = v; },
            "How speed changes the radius. SIGNED on purpose: positive means a\n"
            "fast stroke is WIDER (a dry-brush sweep), negative means thinner\n"
            "(an ink pen). 0 means speed changes nothing.")
        .def_prop_rw(
            "velocity_strength",
            [](const brush::StrokePreset& p) { return p.velocity_response.strength; },
            [](brush::StrokePreset& p, float v) { p.velocity_response.strength = v; },
            "How speed changes the strength, on the same signed scale.")
        .def_prop_rw(
            "velocity_reference",
            [](const brush::StrokePreset& p) { return p.velocity_response.reference; },
            [](brush::StrokePreset& p, float v) { p.velocity_response.reference = v; },
            "The speed, in world units per second, that reads as \"fast\" — at it\n"
            "the response is fully applied, below it proportionally less.")
        .def_rw("taper_start", &brush::StrokePreset::taper_start)
        .def_rw("taper_end", &brush::StrokePreset::taper_end)
        .def_rw("steady", &brush::StrokePreset::steady,
                "lazy-mouse lag: 0 follows exactly, approaching 1 lags more")
        .def_rw("accumulation", &brush::StrokePreset::accumulation)
        .def_prop_ro_static("version", [](nb::handle) { return brush::kPresetVersion; },
                            "the schema version serialize() writes")
        .def("serialize",
             [](const brush::StrokePreset& p) {
                 std::vector<std::uint8_t> bytes = p.serialize();
                 return nb::bytes(bytes.data(), bytes.size());
             },
             "Preset as bytes, tagged with its schema version")
        .def_static("deserialize",
                    [](nb::bytes data) {
                        auto p = brush::StrokePreset::deserialize(
                            reinterpret_cast<const std::uint8_t*>(data.c_str()), data.size());
                        if (!p)
                            throw std::invalid_argument(
                                "not a preset this build can read: either malformed, or written "
                                "by a newer schema version than " +
                                std::to_string(brush::kPresetVersion));
                        return *p;
                    },
                    "data"_a,
                    "Load a preset. An older schema loads with defaults for what it "
                    "lacked; a newer one raises rather than being read as a prefix.")
        .def("resolve",
             [](const brush::StrokePreset& p, nb::handle samples) {
                 std::vector<brush::StrokeSample> in = to_stroke_samples(samples);
                 std::vector<brush::Stamp> stamps;
                 {
                     nb::gil_scoped_release release;
                     stamps = brush::resolve_stroke(in, p);
                 }
                 const std::size_t n = stamps.size();
                 float* pos = new float[n ? n * 3 : 1];
                 float* radius = new float[n ? n : 1];
                 float* strength = new float[n ? n : 1];
                 float* along = new float[n ? n : 1];
                 auto own = [](void* q) noexcept { delete[] static_cast<float*>(q); };
                 nb::capsule pos_owner(pos, own), radius_owner(radius, own);
                 nb::capsule strength_owner(strength, own), along_owner(along, own);
                 for (std::size_t i = 0; i < n; ++i) {
                     pos[i * 3 + 0] = stamps[i].position.x;
                     pos[i * 3 + 1] = stamps[i].position.y;
                     pos[i * 3 + 2] = stamps[i].position.z;
                     radius[i] = stamps[i].radius;
                     strength[i] = stamps[i].strength;
                     along[i] = stamps[i].along;
                 }
                 nb::dict out;
                 out["positions"] = nb::ndarray<nb::numpy, float>(pos, {n, 3}, pos_owner);
                 out["radii"] = nb::ndarray<nb::numpy, float>(radius, {n}, radius_owner);
                 out["strengths"] = nb::ndarray<nb::numpy, float>(strength, {n}, strength_owner);
                 out["along"] = nb::ndarray<nb::numpy, float>(along, {n}, along_owner);
                 return out;
             },
             "samples"_a,
             "Resolve stroke samples into stamps. Pure: no document is read or "
             "touched. samples is (N, 3), (N, 4) with pressure, or (N, 5) with "
             "pressure and tilt. Returns positions/radii/strengths/along arrays.");

    nb::class_<PyGroupField>(
        m, "GroupField",
        "Named regions of the model — PolyGroups / Face Sets, on one\n"
        "world-space lattice shared by every representation.\n\n"
        "Group 0 is NO_GROUP and means 'not in a group'. It is never hidden,\n"
        "and isolate() leaves it visible: ungrouped surface is not something\n"
        "you put away, and isolating a group must not make the rest of the\n"
        "model vanish because it was never named.")
        .def_prop_ro("cell_size", [](const PyGroupField& g) { return g.field().cell_size(); })
        .def_prop_ro("empty", [](const PyGroupField& g) { return g.field().empty(); })
        .def("at",
             [](const PyGroupField& g, nb::handle p) {
                 return g.field().at(to_f3(p, "point"));
             },
             "point"_a,
             "Which group a surface POINT is in — the one query every\n"
             "representation asks, and the reason this is not three mechanisms.")
        .def("fill",
             [](PyGroupField& g, nb::handle region, voxel::GroupId id) {
                 PyGroupStep step(g);
                 g.field().fill(to_aabb(region), id);
             },
             "region"_a, "group"_a,
             "Name a box-shaped region. Membership is decided at the cell\n"
             "CENTRE, so two adjacent fills do not overlap by a cell.")
        .def("fill_from_mask",
             [](PyGroupField& g, nb::handle mask, voxel::GroupId id, float threshold) {
                 const voxel::MaskField* mf = borrow_mask(mask);
                 if (!mf) throw std::invalid_argument("expected a Mask");
                 PyGroupStep step(g);
                 return g.field().fill_from_mask(*mf, id, threshold);
             },
             "mask"_a, "group"_a, "threshold"_a = 0.5f,
             "Name the region a MASK covers — how 'addressed by group or by\n"
             "mask' becomes one mechanism rather than two. Paint a mask any\n"
             "way you like (a brush, a cavity measure, an extrude) and name\n"
             "the result. The two lattices need not share a cell size.")
        .def("reassign",
             [](PyGroupField& g, voxel::GroupId from, voxel::GroupId to) {
                 PyGroupStep step(g);
                 return g.field().reassign(from, to);
             },
             "from_group"_a, "to_group"_a,
             "Everything in `from_group` becomes `to_group`. Merging into 0\n"
             "deletes a group without walking the lattice, and takes its\n"
             "hidden flag with it.")
        .def("grow",
             [](PyGroupField& g, voxel::GroupId id, int steps) {
                 PyGroupStep step(g);
                 return g.field().grow(id, steps);
             },
             "group"_a, "steps"_a = 1,
             "Dilate the region. VOLUMETRIC, NOT GEODESIC, and this is where\n"
             "it differs from a mesh tool: ZBrush grows a face set ALONG the\n"
             "surface, this dilates in 3D. Where a surface folds back within\n"
             "`steps` cells of itself — a tight crease, a thin wall — growth\n"
             "crosses the gap and claims the other side.\n\n"
             "Claims only UNGROUPED cells: growing one group into another\n"
             "would silently destroy a region you named, and 'grow' is not a\n"
             "verb anyone expects to delete.")
        .def("shrink",
             [](PyGroupField& g, voxel::GroupId id, int steps) {
                 PyGroupStep step(g);
                 return g.field().shrink(id, steps);
             },
             "group"_a, "steps"_a = 1, "Erode the region from its boundary.")
        .def("border",
             [](const PyGroupField& g, voxel::GroupId id) {
                 std::vector<voxel::VoxelCoord> rim = g.field().border(id);
                 nb::list out;
                 for (const voxel::VoxelCoord& c : rim)
                     out.append(nb::make_tuple(c.x, c.y, c.z));
                 return out;
             },
             "group"_a,
             "The cells of `group` that touch a cell not in it — the seam to\n"
             "mask, crease or polish.")
        .def("set_visible",
             [](PyGroupField& g, voxel::GroupId id, bool visible) {
                 PyGroupStep step(g);
                 g.field().set_visible(id, visible);
             },
             "group"_a, "visible"_a)
        .def("visible",
             [](const PyGroupField& g, voxel::GroupId id) { return g.field().visible(id); },
             "group"_a)
        .def("isolate",
             [](PyGroupField& g, voxel::GroupId id) {
                 PyGroupStep step(g);
                 g.field().isolate(id);
             },
             "group"_a,
             "Show this one, hide every other group that exists. Identical to\n"
             "hiding everything not in it — group 0 stays visible.")
        .def("show_all",
             [](PyGroupField& g) {
                 PyGroupStep step(g);
                 g.field().show_all();
             })
        .def("invert_visibility",
             [](PyGroupField& g) {
                 PyGroupStep step(g);
                 g.field().invert_visibility();
             },
             "Every hidden group shows, every shown group hides.")
        .def_prop_ro("any_hidden", [](const PyGroupField& g) { return g.field().any_hidden(); })
        .def("point_hidden",
             [](const PyGroupField& g, nb::handle p) {
                 return g.field().point_hidden(to_f3(p, "point"));
             },
             "point"_a, "Whether a surface point is hidden by the group it is in.")
        .def_prop_ro("ids",
                     [](const PyGroupField& g) {
                         nb::list out;
                         for (voxel::GroupId id : g.field().ids()) out.append(id);
                         return out;
                     },
                     "Present groups, ascending, never including 0.")
        .def("cell_count",
             [](const PyGroupField& g, voxel::GroupId id) {
                 return id == voxel::kNoGroup ? g.field().cell_count() : g.field().cell_count(id);
             },
             "group"_a = voxel::kNoGroup,
             "Cells carrying this group, or every grouped cell for 0.");

    nb::class_<PyMaskField>(
        m, "MaskField",
        "Paintable scalar mask in [0, 1] gating how strongly an edit may act.\n\n"
        "Addressed in world units on its own lattice, not in a layer's voxel\n"
        "cells, so a mask survives a resolution change and a move between the\n"
        "SDF and voxel representations. Pass one as the `mask` argument of any\n"
        "VoxelGrid brush: the effective strength at a cell becomes\n"
        "strength * (1 - mask), so a fully masked cell is frozen.\n\n"
        "Masking gates edits where they are authored. A mask painted now does\n"
        "not retroactively protect a region from SDF items already placed.")
        .def("__init__",
             [](PyMaskField* self, float cell_size) {
                 if (cell_size <= 0.0f) throw std::invalid_argument("cell_size must be > 0");
                 new (self) PyMaskField();
                 self->owned = std::make_shared<voxel::MaskField>(cell_size);
             },
             "cell_size"_a = 0.1f)
        .def_prop_ro("cell_size", [](const PyMaskField& m) { return m.field().cell_size(); })
        .def_prop_ro("painted_count", [](const PyMaskField& m) { return m.field().painted_count(); })
        .def_prop_ro("empty", [](const PyMaskField& m) { return m.field().empty(); })
        .def("get", [](const PyMaskField& m,
                       nb::handle cell) { return m.field().get(to_coord(cell)); }, "cell"_a)
        .def("set",
             [](PyMaskField& m, nb::handle cell, float value) {
                 voxel::MaskField& mf_ = m.field();
                 PyMaskStep step_(m, mf_);
                 mf_.set(to_coord(cell), value);
             },
             "cell"_a, "value"_a)
        .def("sample",
             [](const PyMaskField& m, nb::handle p) {
                 return m.field().sample(to_f3(p, "point"));
             },
             "point"_a, "Mask value at a world position")
        .def("sample_many",
             [](const PyMaskField& m, nb::handle points) {
                 PointsView pts = to_points(points);
                 const voxel::MaskField& f = m.field();
                 const std::size_t n = pts.count;
                 float* out = new float[n ? n : 1];
                 nb::capsule owner(out, [](void* q) noexcept { delete[] static_cast<float*>(q); });
                 {
                     nb::gil_scoped_release release;
                     for (std::size_t i = 0; i < n; ++i)
                         out[i] = f.sample(kernel::cf3(pts.data[i * 3 + 0], pts.data[i * 3 + 1],
                                                       pts.data[i * 3 + 2]));
                 }
                 return nb::cast(nb::ndarray<nb::numpy, float>(out, {n}, owner));
             },
             "points"_a, "Sample an (N, 3) array of world positions -> (N,) float32")
        .def("paint",
             [](PyMaskField& m, nb::handle point, int n, float target, const std::string& shape,
                const std::string& falloff, float strength) {
                 voxel::MaskField& mf_ = m.field();
                 PyMaskStep step_(m, mf_);
                 mf_.paint(to_f3(point, "point"),
                                 make_brush(n, shape, falloff, strength, 0u), target);
             },
             "point"_a, "size"_a, "target"_a = 1.0f, "shape"_a = "sphere",
             "falloff"_a = "smooth", "strength"_a = 1.0f,
             "Brush the mask toward `target` at a world position. target=1 masks, "
             "target=0 erases. Size is in mask cells.")
        .def("paint_cell",
             [](PyMaskField& m, nb::handle cell, int n, float target, const std::string& shape,
                const std::string& falloff, float strength) {
                 voxel::MaskField& mf_ = m.field();
                 PyMaskStep step_(m, mf_);
                 mf_.paint(to_coord(cell), make_brush(n, shape, falloff, strength, 0u),
                                 target);
             },
             "cell"_a, "size"_a, "target"_a = 1.0f, "shape"_a = "sphere",
             "falloff"_a = "smooth", "strength"_a = 1.0f,
             "As paint(), centred on a mask cell rather than a world position")
        .def("invert", [](PyMaskField& m) { voxel::MaskField& mf_ = m.field(); PyMaskStep step_(m, mf_); mf_.invert(); },
             "Flip the painted region (a sparse field has no finite complement)")
        .def("clear", [](PyMaskField& m) { voxel::MaskField& mf_ = m.field(); PyMaskStep step_(m, mf_); mf_.clear(); })
        .def("expand",
             [](PyMaskField& m, int steps) { voxel::MaskField& mf_ = m.field(); PyMaskStep step_(m, mf_); mf_.expand(steps); },
             "steps"_a = 1,
             "Grow the mask by grey dilation")
        .def("contract",
             [](PyMaskField& m, int steps) { voxel::MaskField& mf_ = m.field(); PyMaskStep step_(m, mf_); mf_.contract(steps); },
             "steps"_a = 1, "Shrink the mask by grey erosion")
        .def("smooth",
             [](PyMaskField& m, int iterations) {
                 voxel::MaskField& mf_ = m.field();
                 PyMaskStep step_(m, mf_);
                 mf_.smooth(iterations);
             },
             "iterations"_a = 1, "Blur the mask, softening its boundary")
        .def("bounds",
             [](const PyMaskField& m) -> nb::object {
                 auto lo = m.field().bounds_min();
                 auto hi = m.field().bounds_max();
                 if (!lo || !hi) return nb::none();
                 return nb::make_tuple(nb::make_tuple(lo->x, lo->y, lo->z),
                                       nb::make_tuple(hi->x, hi->y, hi->z));
             },
             "Inclusive cell bounds of the painted region, or None")
        .def("fill",
             [](PyMaskField& m, nb::handle region, float value) {
                 voxel::MaskField& mf_ = m.field();
                 PyMaskStep step_(m, mf_);
                 mf_.fill(to_aabb(region), value);
             },
             "region"_a, "value"_a = 1.0f,
             "Set every cell whose centre lies in a ((lo), (hi)) world box. "
             "Filling with 0 releases the region.")
        .def("invert_within",
             [](PyMaskField& m, nb::handle region) {
                 voxel::MaskField& mf_ = m.field();
                 PyMaskStep step_(m, mf_);
                 mf_.invert_within(to_aabb(region));
             },
             "region"_a,
             "Take the complement over a ((lo), (hi)) world box — the bounded\n"
             "form invert() cannot be, and the one 'mask a limb, invert, sculpt\n"
             "everything else' actually means. invert() flips only what has been\n"
             "painted, because a sparse unbounded lattice has no finite\n"
             "complement; here the caller supplies the region, which it always\n"
             "has from a grid's or an item's bounds.")
        .def("apply_stroke",
             [](PyMaskField& m, nb::handle samples, const brush::StrokePreset& preset,
                float target, const std::string& shape, const std::string& falloff) {
                 std::vector<brush::StrokeSample> in = to_stroke_samples(samples);
                 voxel::BrushShape s = parse_brush_shape(shape);
                 voxel::BrushFalloff f = parse_falloff(falloff);
                 voxel::MaskField& field = m.field();
                 nb::gil_scoped_release release;
                 return brush::apply_to_mask(field, brush::resolve_stroke(in, preset), target, s,
                                             f);
             },
             "samples"_a, "preset"_a, "target"_a = 1.0f, "shape"_a = "sphere",
             "falloff"_a = "smooth",
             "Resolve a stroke and paint it into the mask — masking as the same\n"
             "gesture as sculpting, with the same spacing, pressure, taper,\n"
             "steady stroke and jitter.\n\n"
             "`target` is where each cell moves TO, so target=1 masks and\n"
             "target=0 erases: painting and releasing are one call.\n\n"
             "The footprint comes from each stamp's WORLD radius; there is no\n"
             "size in mask cells to pass, because a caller converting it by hand\n"
             "gets a stroke whose width tracks the mask's resolution rather than\n"
             "the brush's radius.")
        .def("to_field",
             [](const PyMaskField& m, float threshold, nb::handle band, float pad,
                nb::handle cell_size) {
                 const float cell =
                     cell_size.is_none() ? 0.0f : nb::cast<float>(cell_size);
                 const float width = band.is_none() ? 0.0f : nb::cast<float>(band);
                 std::optional<field::FieldVolume> volume =
                     brush::mask_to_field(m.field(), threshold, width, pad, cell);
                 if (!volume)
                     throw std::invalid_argument(
                         "nothing painted at or above the threshold: there is no region to "
                         "measure");
                 PyVolume out;
                 out.prim = scene::Prim::volume();
                 out.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
                 return out;
             },
             "threshold"_a = 0.5f, "band"_a = nb::none(), "pad"_a = 0.0f,
             "cell_size"_a = nb::none(),
             "Measure the mask: signed distance to the boundary of the masked\n"
             "region, negative inside, as an ordinary Volume.\n\n"
             "This is what mask extrude is built on, and it is here because a\n"
             "host wants to preview that border. A mask is a [0,1] scalar on a\n"
             "lattice and NOT a distance field — composing one into a field\n"
             "expression directly would put a step in the result and the\n"
             "Lipschitz bound would stop meaning anything.\n\n"
             "`pad` widens the sampled region past the masked one; anything that\n"
             "reaches outside the mask needs it.");

    nb::class_<PySculptLayerScope>(
        m, "SculptLayerScope",
        "The context manager grid.sculpt_layer() returns; entering it starts\n"
        "recording and leaving it stops, including when the block raises.")
        .def("__enter__",
             [](PySculptLayerScope& s) {
                 if (s.grid.grid().recording_sculpt_layer())
                     throw nb::value_error("a sculpt layer is already recording");
                 s.index = s.grid.grid().begin_sculpt_layer(s.name);
                 return s.index;
             })
        .def("__exit__",
             // Variadic: the three arguments are None on a clean exit, and a
             // typed signature would refuse them.
             [](PySculptLayerScope& s, nb::args) {
                 // Ends the pass whether the block finished or threw. The
                 // partial pass is KEPT rather than rolled back: the edits
                 // already happened, and a layer that records them is strictly
                 // more recoverable than one that silently disowns them.
                 s.grid.grid().end_sculpt_layer();
                 return false;  // never swallow the exception
             })
        .def_prop_ro("index", [](const PySculptLayerScope& s) { return s.index; });

    nb::class_<PyVoxelGrab>(
        m, "VoxelGrab",
        "A voxel drag as a GESTURE, which grid.grab() returns.\n\n"
        "`sculpt_grab` does not compose. Each call reads the grid, resamples\n"
        "occupancy through the falloff and writes back, so the next call reads\n"
        "its own output — and the displacement is rounded to whole cells AFTER\n"
        "the falloff weights it, so at one cell only the very middle of the\n"
        "region rounds to a cell, which inside solid material changes no\n"
        "occupancy at all. Split a drag finely enough and it evaporates: a\n"
        "measured 8-cell drag over a 32-cell footprint moved 205 cells in one\n"
        "emission and 0 in eight.\n\n"
        "This keeps the material as it was and resamples from THAT, so every\n"
        "`update` is the TOTAL from the anchor — never an increment — and a run\n"
        "of them ends where a single one to the same total would. Repeating an\n"
        "update changes nothing, and a pointer that comes back to where it\n"
        "started puts the material back.\n\n"
        "Use it as a context manager: leaving the block commits, and an\n"
        "exception cancels.")
        .def("update",
             [](PyVoxelGrab& g, nb::handle total_displacement) {
                 if (!g.tx || !g.tx->live())
                     throw nb::value_error("this grab is already finished");
                 PyVoxelStep step_(g.grid, g.grid.grid());
                 g.tx->update(to_f3(total_displacement, "total_displacement"));
             },
             "total_displacement"_a,
             "The drag so far, measured from the anchor. The TOTAL, never an\n"
             "increment on the last frame.")
        .def("commit",
             [](PyVoxelGrab& g) {
                 if (g.tx && g.tx->live()) g.tx->commit();
             },
             "Keep what the last update wrote. The grid already holds it.")
        .def("cancel",
             [](PyVoxelGrab& g) {
                 if (!g.tx || !g.tx->live()) return;
                 PyVoxelStep step_(g.grid, g.grid.grid());
                 g.tx->cancel();
             },
             "Put the captured material back, exactly as it was at the start.")
        .def_prop_ro("live", [](const PyVoxelGrab& g) { return g.tx && g.tx->live(); })
        .def_prop_ro("written_box",
                     [](const PyVoxelGrab& g) {
                         if (!g.tx) throw nb::value_error("this grab is finished");
                         const voxel::VoxelCoord lo = g.tx->written_lo();
                         const voxel::VoxelCoord hi = g.tx->written_hi();
                         return nb::make_tuple(nb::make_tuple(lo.x, lo.y, lo.z),
                                               nb::make_tuple(hi.x, hi.y, hi.z));
                     },
                     "The cells the gesture writes: the brush's footprint, fixed\n"
                     "for the whole gesture whatever the displacement grows to.")
        .def("__enter__", [](nb::object self) { return self; })
        .def("__exit__",
             // Variadic: the three arguments are None on a clean exit.
             [](PyVoxelGrab& g, nb::args args) {
                 const bool threw = args.size() >= 1 && !args[0].is_none();
                 if (g.tx && g.tx->live()) {
                     PyVoxelStep step_(g.grid, g.grid.grid());
                     // A block that threw leaves the grid as it was: unlike a
                     // sculpt layer, a half-finished drag is not something a
                     // host could make sense of afterwards.
                     if (threw) {
                         g.tx->cancel();
                     } else {
                         g.tx->commit();
                     }
                 }
                 return false;  // never swallow the exception
             });

    nb::class_<PyVoxelGrid>(m, "VoxelGrid", "Palette-indexed colored voxel grid (chunked, sparse)")
        .def(
            "__init__",
            [](PyVoxelGrid* self, float voxel_size) {
                if (voxel_size <= 0.0f) throw std::invalid_argument("voxel_size must be > 0");
                new (self) PyVoxelGrid();
                self->owned = std::make_shared<voxel::VoxelGrid>(voxel_size);
            },
            "voxel_size"_a = 0.1f)
        .def_prop_ro(
            "voxel_size", [](const PyVoxelGrid& g) { return g.grid().voxel_size(); },
            "Cell size of the active resolution level.")
        .def_prop_ro(
            "level_count", [](const PyVoxelGrid& g) { return g.grid().level_count(); },
            "Resolution levels: level 0 is the coarsest and level k has\n"
            "half the cell size of level k-1. A grid always has at least\n"
            "one, and a grid that never gains a second behaves exactly as\n"
            "a grid did before levels existed.")
        .def_prop_rw(
            "active_level", [](const PyVoxelGrid& g) { return g.grid().active_level(); },
            [](PyVoxelGrid& g, std::size_t level) {
                if (!g.grid().set_active_level(level))
                    throw std::invalid_argument("no such resolution level");
            },
            "The level every verb acts on. Setting it is free: editing at\n"
            "a level already averages down into the coarser levels and\n"
            "replays into the finer ones from the offsets they hold.")
        .def(
            "add_level",
            [](PyVoxelGrid& g) {
                std::size_t before = g.grid().level_count();
                std::size_t level = g.grid().add_level();
                if (g.grid().level_count() == before)
                    throw std::invalid_argument("the level stack is at its cap");
                return level;
            },
            "Appends a level at half the finest cell size, subdividing every\n"
            "occupied cell into its eight children so the solid is unchanged.\n"
            "Returns the new level's index.")
        .def(
            "add_level_region",
            [](PyVoxelGrid& g, nb::handle region) {
                const math::Aabb box = to_aabb(region);
                if (box.empty())
                    throw std::invalid_argument("the region is empty; there is nothing to refine");
                std::size_t before = g.grid().level_count();
                std::size_t level = g.grid().add_level(box);
                if (g.grid().level_count() == before)
                    throw std::invalid_argument("the level stack is at its cap");
                return level;
            },
            "region"_a,
            "Appends a level refined only over `region` ((min, max) in world\n"
            "units, rounded OUT to whole chunks).\n\n"
            "Outside the region the level has no storage and reads its parent's\n"
            "value, so the lattice is still uniform and complete — only what is\n"
            "STORED changes, and meshing, bounds and neighbour indexing are as\n"
            "they were. Writing outside the region refines what the write\n"
            "touched, so a brush straddling the boundary works.\n\n"
            "Adding a whole level costs eight times the OCCUPIED VOLUME, and\n"
            "occupancy is volumetric. A chunk is 32 cells across, so a region\n"
            "smaller than that still costs one: the saving is on a form\n"
            "spanning many chunks at the resolution being authored, which is\n"
            "the case a level stack is for.")
        .def(
            "level_chunk_count",
            [](const PyVoxelGrid& g, std::size_t level) {
                return g.grid().level_refined_chunk_count(level);
            },
            "level"_a, "How many chunks a level stores.")
        .def(
            "level_is_whole",
            [](const PyVoxelGrid& g, std::size_t level) { return g.grid().level_is_whole(level); },
            "level"_a,
            "Whether a level stores the whole lattice. True for a level added\n"
            "without a region, and for every grid written before regions existed.")
        .def(
            "drop_level", [](PyVoxelGrid& g) { return g.grid().drop_level(); },
            "Drops the finest level and the detail only it held. False when\n"
            "there is only one left.")
        .def(
            "level_voxel_size",
            [](const PyVoxelGrid& g, std::size_t level) {
                if (level >= g.grid().level_count())
                    throw std::invalid_argument("no such resolution level");
                return g.grid().level_voxel_size(level);
            },
            "level"_a)
        .def(
            "level_occupied_count",
            [](const PyVoxelGrid& g, std::size_t level) {
                if (level >= g.grid().level_count())
                    throw std::invalid_argument("no such resolution level");
                return g.grid().level_occupied_count(level);
            },
            "level"_a, "Occupied cells at one level — what that level costs.")
        .def_prop_ro("occupied_count",
                     [](const PyVoxelGrid& g) { return g.grid().occupied_count(); })
        .def_prop_ro(
            "change_count", [](const PyVoxelGrid& g) { return g.grid().change_count(); },
            "Cell writes that actually changed a cell, since construction.\n"
            "Monotone and never reset, so only the difference between two\n"
            "reads means anything: it is how you tell an edit that did\n"
            "nothing — a sub-cell grab, a flatten on flat ground — from one\n"
            "that did. occupied_count cannot, since grab and magnify\n"
            "conserve material. Exact per cell except for pinch and\n"
            "magnify, which may revisit a cell and so over-count.")
        .def_prop_ro("palette_size", [](const PyVoxelGrid& g) { return g.grid().palette_size(); })
        .def(
            "palette_add",
            [](PyVoxelGrid& g, nb::handle color) {
                return g.grid().palette_add(parse_color(color));
            },
            "color"_a, "Add (or find) a palette entry; returns its index")
        .def(
            "palette_color",
            [](const PyVoxelGrid& g, std::uint8_t index) {
                kernel::cfloat3 c = g.grid().palette_color(index);
                return nb::make_tuple(c.x, c.y, c.z);
            },
            "index"_a)
        .def(
            "palette_set",
            [](PyVoxelGrid& g, std::uint8_t index, nb::handle color) {
                g.grid().palette_set(index, parse_color(color));
            },
            "index"_a, "color"_a, "Recolor a palette entry; voxel data is untouched")
        .def(
            "get",
            [](const PyVoxelGrid& g, nb::handle cell) { return g.grid().get(to_coord(cell)); },
            "cell"_a)
        .def(
            "set",
            [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.set(to_coord(cell), index);
                }
            },
            "cell"_a, "index"_a)
        .def(
            "set_many",
            [](PyVoxelGrid& g, nb::handle cells, std::uint8_t index) {
                std::vector<voxel::VoxelCoord> coords = to_coords(cells);
                voxel::VoxelGrid& grid = g.grid();
                nb::gil_scoped_release release;
                for (const voxel::VoxelCoord& c : coords) grid.set(c, index);
            },
            "cells"_a, "index"_a, "Set every cell of an (N, 3) int32 array in one call")
        .def(
            "erase",
            [](PyVoxelGrid& g, nb::handle cell) {
                voxel::VoxelGrid& gr_ = g.grid();
                PyVoxelStep step_(g, gr_);
                gr_.erase(to_coord(cell));
            },
            "cell"_a)
        .def(
            "erase_many",
            [](PyVoxelGrid& g, nb::handle cells) {
                std::vector<voxel::VoxelCoord> coords = to_coords(cells);
                voxel::VoxelGrid& grid = g.grid();
                nb::gil_scoped_release release;
                for (const voxel::VoxelCoord& c : coords) grid.erase(c);
            },
            "cells"_a)
        .def(
            "paint",
            [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.paint(to_coord(cell), index);
                }
            },
            "cell"_a, "index"_a, "Recolor an occupied cell (no-op on empty cells)")
        .def(
            "set_brush",
            [](PyVoxelGrid& g, nb::handle cell, int n, std::uint8_t index, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().set_brush(to_coord(cell),
                                   make_brush(n, shape, falloff, strength, seed, mask), index);
            },
            "cell"_a, "size"_a, "index"_a, "shape"_a = "cube", "falloff"_a = "constant",
            "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Brush footprint centered on the cell: 'cube' or 'sphere', size n "
            "covering n cells per axis. falloff ('constant', 'linear', 'smooth', "
            "'gaussian') and strength soften the edge by dithering coverage "
            "deterministically against seed.")
        .def(
            "erase_brush",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().erase_brush(to_coord(cell),
                                     make_brush(n, shape, falloff, strength, seed, mask));
            },
            "cell"_a, "size"_a, "shape"_a = "cube", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none())
        .def(
            "paint_brush",
            [](PyVoxelGrid& g, nb::handle cell, int n, std::uint8_t index, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().paint_brush(to_coord(cell),
                                     make_brush(n, shape, falloff, strength, seed, mask), index);
            },
            "cell"_a, "size"_a, "index"_a, "shape"_a = "cube", "falloff"_a = "constant",
            "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Recolor occupied cells in the footprint; empty cells are untouched")
        // -- sculpt layers ----------------------------------------------
        // A pass you can dial back after making it. `with grid.sculpt_layer():`
        // is the shape a Python caller wants, so the context manager is the
        // headline and begin/end sit behind it for the odd case that spans
        // control flow.
        .def(
            "sculpt_layer",
            [](const PyVoxelGrid& g, const std::string& name) {
                return PySculptLayerScope{g, name, 0};
            },
            "name"_a = std::string(),
            "A context manager: every edit inside the block joins one pass\n"
            "whose strength stays adjustable afterwards.\n\n"
            "    with grid.sculpt_layer(\"wrinkles\") as i:\n"
            "        grid.sculpt_inflate(cell, size=9, amount=2)\n"
            "    grid.set_sculpt_layer_strength(i, 0.4)")
        .def(
            "begin_sculpt_layer",
            [](PyVoxelGrid& g, const std::string& name) {
                if (g.grid().recording_sculpt_layer())
                    throw nb::value_error("a sculpt layer is already recording");
                return g.grid().begin_sculpt_layer(name);
            },
            "name"_a = std::string(),
            "Start recording; every edit until end_sculpt_layer joins this pass")
        .def(
            "end_sculpt_layer", [](PyVoxelGrid& g) { g.grid().end_sculpt_layer(); },
            "Stop recording. Later edits belong to no pass")
        .def_prop_ro("recording_sculpt_layer",
                     [](const PyVoxelGrid& g) { return g.grid().recording_sculpt_layer(); })
        .def_prop_ro("sculpt_layer_count",
                     [](const PyVoxelGrid& g) { return g.grid().sculpt_layer_count(); })
        .def(
            "sculpt_layer_name",
            [](const PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                return g.grid().sculpt_layer_name(layer);
            },
            "layer"_a)
        .def(
            "sculpt_layer_cell_count",
            [](const PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                return g.grid().sculpt_layer_cell_count(layer);
            },
            "layer"_a, "How many cells the pass changed")
        .def(
            "sculpt_layer_strength",
            [](const PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                return g.grid().sculpt_layer_strength(layer);
            },
            "layer"_a)
        .def(
            "set_sculpt_layer_strength",
            [](PyVoxelGrid& g, std::size_t layer, float strength) {
                check_sculpt_layer(g, layer);
                g.grid().set_sculpt_layer_strength(layer, strength);
            },
            "layer"_a, "strength"_a,
            "Clamped to [0, 1]. On binary occupancy a fraction is a "
            "reproducible fraction of the CELLS; 0 and 1 are exact")
        .def(
            "sculpt_layer_visible",
            [](const PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                return g.grid().sculpt_layer_visible(layer);
            },
            "layer"_a)
        .def(
            "set_sculpt_layer_visible",
            [](PyVoxelGrid& g, std::size_t layer, bool visible) {
                check_sculpt_layer(g, layer);
                g.grid().set_sculpt_layer_visible(layer, visible);
            },
            "layer"_a, "visible"_a)
        .def(
            "remove_sculpt_layer",
            [](PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                g.grid().remove_sculpt_layer(layer);
            },
            "layer"_a, "Drop a pass; the ones above it replay on what is left")
        .def(
            "merge_sculpt_layer_down",
            [](PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                if (!g.grid().merge_sculpt_layer_down(layer))
                    throw nb::value_error("the bottom sculpt layer has nothing below it");
            },
            "layer"_a, "Fold a pass into the one below, keeping the lower name")
        .def(
            "move_sculpt_layer",
            [](PyVoxelGrid& g, std::size_t from, std::size_t to) {
                check_sculpt_layer(g, from);
                check_sculpt_layer(g, to);
                g.grid().move_sculpt_layer(from, to);
            },
            "from_"_a, "to"_a,
            "Move a pass within the stack. Order is meaningful: where two\n"
            "passes touched the same cell, the higher one wins")
        .def(
            "sculpt_layer_bytes",
            [](const PyVoxelGrid& g, std::size_t layer) {
                check_sculpt_layer(g, layer);
                return g.grid().sculpt_layer_bytes(layer);
            },
            "layer"_a, "What one pass costs in memory — its cells, not the model")
        .def_prop_ro(
            "sculpt_layers_bytes",
            [](const PyVoxelGrid& g) { return g.grid().sculpt_layer_total_bytes(); },
            "What the whole stack costs. Nothing is enforced; merge down\n"
            "or stop recording is the host's call")
        .def(
            "sculpt_smooth",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_smooth(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask));
                }
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none(),
            "Majority filter over the 26-neighbourhood: spurs dissolve, notches fill")
        .def(
            "sculpt_inflate",
            [](PyVoxelGrid& g, nb::handle cell, int n, int amount, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_inflate(
                    to_coord(cell), make_brush(n, shape, falloff, strength, seed, mask), amount);
                }
            },
            "cell"_a, "size"_a, "amount"_a = 1, "shape"_a = "sphere", "falloff"_a = "constant",
            "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Dilate for a positive amount, erode for a negative one")
        .def(
            "sculpt_flatten",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle normal, float offset,
               const std::string& shape, const std::string& falloff, float strength,
               std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_flatten(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask),
                                        to_f3(normal, "normal"), offset);
                }
            },
            "cell"_a, "size"_a, "normal"_a, "offset"_a = 0.0f, "shape"_a = "sphere",
            "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Pull the surface onto the plane through the brush centre")
        .def(
            "sculpt_grab",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle displacement,
               const std::string& shape, const std::string& falloff, float strength,
               std::uint32_t seed, bool front_only, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_grab(to_coord(cell),
                                     make_brush(n, shape, falloff, strength, seed, mask),
                                     to_f3(displacement, "displacement"), front_only);
                }
            },
            "cell"_a, "size"_a, "displacement"_a, "shape"_a = "sphere", "falloff"_a = "smooth",
            "strength"_a = 1.0f, "seed"_a = 0u, "front_only"_a = false, "mask"_a = nb::none(),
            "Translate occupancy through the same map the SDF grab uses. Binary "
            "occupancy means this resamples nearest-cell, so material moves in "
            "whole cells rather than flowing.\n\n"
            "THIS DOES NOT COMPOSE — reach for `grab()` for a drag. Each call "
            "reads the grid and writes back, so the next reads its own output, "
            "and a drag delivered as a stream of small emissions moves less than "
            "one delivered whole (and often nothing at all).")
        .def(
            "grab",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed,
               bool front_only, nb::handle mask) {
                PyVoxelGrab out;
                out.grid = g;
                out.tx = voxel::GrabTransaction::begin(
                    g.grid(), to_coord(cell),
                    make_brush(n, shape, falloff, strength, seed, mask), front_only);
                if (!out.tx) throw nb::value_error("a grab needs a brush size of at least 1");
                return out;
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "smooth",
            "strength"_a = 1.0f, "seed"_a = 0u, "front_only"_a = false, "mask"_a = nb::none(),
            "Begin a DRAG anchored at `cell`, returning a `VoxelGrab`.\n\n"
            "`sculpt_grab` does not compose, so a drag delivered as a stream of\n"
            "small emissions moves less than one delivered whole — often nothing\n"
            "at all. This captures the material as it is now and resamples from\n"
            "that, so every `update` is the TOTAL from the anchor and a run of\n"
            "them ends where a single one would.\n\n"
            "    with grid.grab((0, 0, 0), size=32, front_only=True) as drag:\n"
            "        for total in trail:\n"
            "            drag.update(total)\n"
            "\n"
            "Leaving the block commits; an exception cancels.")
        .def(
            "sculpt_pinch",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_pinch(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed, mask));
                }
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none(),
            "Move surface cells one step toward the brush centre")
        .def(
            "sculpt_magnify",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_magnify(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask));
                }
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none(),
            "Move surface cells one step AWAY from the brush centre: pinch's\n"
            "inverse, sharing its walk so the two cannot drift apart")
        .def(
            "sculpt_fill_cavities",
            [](PyVoxelGrid& g, nb::handle cell, int n, int passes, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_fill_cavities(
                    to_coord(cell), make_brush(n, shape, falloff, strength, seed, mask), passes);
                }
            },
            "cell"_a, "size"_a, "passes"_a = 1, "shape"_a = "sphere", "falloff"_a = "constant",
            "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Fill pockets: an empty cell with at least four of its six face "
            "neighbours occupied is inside a cavity rather than beside a surface. "
            "A through-hole, an open face and a wide shallow dent are left alone — "
            "smoothing is the verb for surface irregularity.")
        .def(
            "sculpt_scrape",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle normal, float offset,
               const std::string& shape, const std::string& falloff, float strength,
               std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_scrape(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask),
                                       to_f3(normal, "normal"), offset);
                }
            },
            "cell"_a, "size"_a, "normal"_a, "offset"_a = 0.0f, "shape"_a = "sphere",
            "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Flatten onto the plane AND smooth, from ONE snapshot. Calling the two "
            "verbs in sequence is not the same thing: the flatten's output would "
            "feed the smooth's neighbourhood.")
        .def(
            "sculpt_smudge",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle displacement,
               const std::string& shape, const std::string& falloff, float strength,
               std::uint32_t seed, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.sculpt_smudge(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask),
                                       to_f3(displacement, "displacement"));
                }
            },
            "cell"_a, "size"_a, "displacement"_a, "shape"_a = "sphere", "falloff"_a = "smooth",
            "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Drag SURFACE material along a direction, leaving the interior where it "
            "was. That is the difference from grab, which translates every cell in "
            "its region: grab moves a lump, smudge smears a skin.")
        .def(
            "sculpt_carve_alpha",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle alpha, nb::handle direction,
               std::uint8_t index, const std::string& shape, const std::string& falloff,
               float strength, std::uint32_t seed, nb::handle mask) {
                nb::module_ np = nb::module_::import_("numpy");
                nb::object arr = np.attr("ascontiguousarray")(alpha, "dtype"_a = "float32");
                nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> view;
                try {
                    view = nb::cast<decltype(view)>(arr);
                } catch (const std::exception&) {
                    throw std::invalid_argument("alpha must be an (H, W) float array");
                }
                voxel::VoxelGrid& gr_ = g.grid();
                PyVoxelStep step_(g, gr_);
                if (!gr_.sculpt_carve_alpha(
                        to_coord(cell), make_brush(n, shape, falloff, strength, seed, mask),
                        view.data(), static_cast<int>(view.shape(1)),
                        static_cast<int>(view.shape(0)), to_f3(direction, "direction"), index))
                    throw std::invalid_argument(
                        "the alpha stamp is malformed: an empty grid, or a zero-length "
                        "direction");
            },
            "cell"_a, "size"_a, "alpha"_a, "direction"_a, "index"_a = 0, "shape"_a = "sphere",
            "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Carve modulated by an (H, W) alpha, projected onto the plane "
            "perpendicular to `direction`. index 0 carves; a non-zero one deposits. "
            "The engine decodes no images — a host that has an alpha has already "
            "loaded the PNG.")
        .def(
            "repair_report",
            [](const PyVoxelGrid& g) {
                voxel::VoxelGrid::RepairReport r = g.grid().repair_report();
                nb::dict out;
                out["enclosed_voids"] = r.enclosed_voids;
                out["void_cells"] = r.void_cells;
                out["largest_void"] = r.largest_void;
                out["airtight"] = r.airtight;
                return out;
            },
            "Non-destructive: what a pre-bake check wants to know without doing the "
            "repair. A destructive operation whose input is somebody's sculpt should "
            "be askable before it is answerable.")
        .def(
            "repair_close_holes",
            [](PyVoxelGrid& g, int passes, nb::handle mask) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.repair_close_holes(passes, borrow_mask(mask));
                }
            },
            "passes"_a = 1, "mask"_a = nb::none(),
            "Seal perforations over the whole grid by the pocket rule. Only ever "
            "adds cells, so no material is lost.")
        .def(
            "repair_fill_voids",
            [](PyVoxelGrid& g, nb::handle mask) {
                voxel::VoxelGrid& gr_ = g.grid();
                PyVoxelStep step_(g, gr_);
                gr_.repair_fill_voids(borrow_mask(mask));
            },
            "mask"_a = nb::none(),
            "Fill every empty cell the outside cannot reach, coloured from the shell "
            "that encloses it. Enclosure is decided by a flood from outside the "
            "bounds, not guessed at from a local neighbourhood.")
        .def(
            "apply_stroke",
            [](PyVoxelGrid& g, nb::handle samples, const brush::StrokePreset& preset,
               std::uint8_t index, const std::string& shape, const std::string& falloff,
               nb::handle mask) {
                std::vector<brush::StrokeSample> in = to_stroke_samples(samples);
                voxel::BrushShape s = parse_brush_shape(shape);
                voxel::BrushFalloff f = parse_falloff(falloff);
                const voxel::MaskField* m = borrow_mask(mask);
                voxel::VoxelGrid& grid = g.grid();
                nb::gil_scoped_release release;
                return brush::apply_to_grid(grid, brush::resolve_stroke(in, preset), index, s, f,
                                            m);
            },
            "samples"_a, "preset"_a, "index"_a, "shape"_a = "sphere", "falloff"_a = "smooth",
            "mask"_a = nb::none(),
            "Resolve a stroke and stamp it into the grid; returns how many stamps "
            "ran. Masked stamps are dropped, so a frozen region receives nothing.")
        .def(
            "mask_extrude",
            [](const PyVoxelGrid& g, nb::handle mask, float thickness, const std::string& side,
               float threshold, int border_smooth) {
                const voxel::MaskField* m = borrow_mask(mask);
                if (!m) throw std::invalid_argument("mask must be a MaskField");
                brush::MaskExtrudeSettings settings = extrude_settings(
                    thickness, side, threshold, 0.0f, border_smooth, nb::none(), nb::none());
                const voxel::VoxelGrid& src = g.grid();
                std::optional<voxel::VoxelGrid> extract;
                {
                    nb::gil_scoped_release release;
                    extract = brush::mask_extrude(src, *m, settings);
                }
                if (!extract)
                    throw std::invalid_argument(
                        "nothing to extrude: the mask is empty, is not on the surface, or the "
                        "grid is");
                PyVoxelGrid out;
                out.owned = std::make_shared<voxel::VoxelGrid>(std::move(*extract));
                return out;
            },
            "mask"_a, "thickness"_a, "side"_a = "outward", "threshold"_a = 0.5f,
            "border_smooth"_a = 0,
            "Mask extrude in CELL space: the masked cells of this grid's\n"
            "surface, thickened, as a new grid carrying this one's colours.\n\n"
            "It does not go through a sampled field — a grid already knows which\n"
            "of its cells are on its surface, so resampling would cost a\n"
            "conversion and lose the palette. The result agrees with\n"
            "Document.mask_extrude to within a voxel, which is the point: what a\n"
            "document means must not depend on how it is stored.\n\n"
            "`border_round`, `cell_size` and `band` have no meaning here; the\n"
            "grid's own resolution is the only one there is. Neither this grid\n"
            "nor the mask is modified.")
        .def(
            "fill_box",
            [](PyVoxelGrid& g, nb::handle a, nb::handle b, std::uint8_t index) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.fill_box(to_coord(a), to_coord(b), index);
                }
            },
            "a"_a, "b"_a, "index"_a, "Inclusive-corner box fill")
        .def(
            "fill_line",
            [](PyVoxelGrid& g, nb::handle a, nb::handle b, std::uint8_t index) {
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.fill_line(to_coord(a), to_coord(b), index);
                }
            },
            "a"_a, "b"_a, "index"_a)
        .def(
            "set_mirrored",
            [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index, const std::string& axes) {
                std::uint8_t mask = 0;
                for (char c : axes) mask |= static_cast<std::uint8_t>(1u << parse_axis({c}));
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.set_mirrored(to_coord(cell), index, mask);
                }
            },
            "cell"_a, "index"_a, "axes"_a = "x",
            "Set the cell and every mirror combination of the given axes ('x', 'xz', ...)")
        .def(
            "paint_mirrored",
            [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index, const std::string& axes) {
                std::uint8_t mask = 0;
                for (char c : axes) mask |= static_cast<std::uint8_t>(1u << parse_axis({c}));
                {
                    voxel::VoxelGrid& gr_ = g.grid();
                    PyVoxelStep step_(g, gr_);
                    gr_.paint_mirrored(to_coord(cell), index, mask);
                }
            },
            "cell"_a, "index"_a, "axes"_a = "x",
            "Recolor the cell and every mirror combination (occupied cells only)")
        .def(
            "flood_select",
            [](const PyVoxelGrid& g, nb::handle seed, bool same_color) {
                std::vector<voxel::VoxelCoord> sel =
                    g.grid().flood_select(to_coord(seed), same_color);
                nb::module_ np = nb::module_::import_("numpy");
                nb::object arr =
                    np.attr("empty")(nb::make_tuple(sel.size(), 3), "dtype"_a = "int32");
                auto view = nb::cast<nb::ndarray<std::int32_t, nb::ndim<2>, nb::c_contig>>(arr);
                for (std::size_t i = 0; i < sel.size(); ++i) {
                    view.data()[i * 3 + 0] = sel[i].x;
                    view.data()[i * 3 + 1] = sel[i].y;
                    view.data()[i * 3 + 2] = sel[i].z;
                }
                return arr;
            },
            "seed"_a, "same_color"_a = true, "6-connected flood select -> (N, 3) int32 coordinates")
        .def(
            "bounds",
            [](const PyVoxelGrid& g) -> nb::object {
                auto lo = g.grid().bounds_min();
                auto hi = g.grid().bounds_max();
                if (!lo || !hi) return nb::none();
                return nb::make_tuple(nb::make_tuple(lo->x, lo->y, lo->z),
                                      nb::make_tuple(hi->x, hi->y, hi->z));
            },
            "Inclusive cell bounds of occupied voxels, or None when empty")
        .def(
            "mesh",
            [](const PyVoxelGrid& g) {
                PyMesh out;
                const voxel::VoxelGrid& grid = g.grid();
                {
                    nb::gil_scoped_release release;
                    out.m = grid.mesh_greedy();
                }
                return out;
            },
            "Greedy-mesh the grid (merged quads, per-face palette color)")
        .def(
            "mesh_smooth",
            [](const PyVoxelGrid& g, int blur) {
                if (blur < 0 || blur > 8) throw nb::value_error("blur must be 0..8 passes");
                PyMesh out;
                const voxel::VoxelGrid& grid = g.grid();
                {
                    nb::gil_scoped_release release;
                    out.m = grid.mesh_smooth(voxel::VoxelGrid::SmoothOptions{blur});
                }
                return out;
            },
            "blur"_a = 0,
            "Mesh the grid as a rounded form (surface nets over occupancy, "
            "per-vertex blended palette color). blur adds 3x3x3 occupancy "
            "passes and can erase a thin feature, so it is 0 by default. A "
            "preview mesh: not manifold, not watertight — mesh() is the "
            "export path and is unaffected.")
        .def(
            "mesh_quads",
            [](const PyVoxelGrid& g, const std::string& mode, nb::handle cell_size,
               nb::handle target, float tolerance, int max_iterations, int blur,
               std::size_t level) {
                return mesh_voxel_quads(g.grid(), mode, cell_size, target, tolerance,
                                        max_iterations, blur, level);
            },
            "mode"_a = "dual", "cell_size"_a = nb::none(), "target"_a = nb::none(),
            "tolerance"_a = 0.10f, "max_iterations"_a = 4, "blur"_a = 0, "level"_a = 0,
            "Mesh the sculpt as QUADS -> Mesh.\n\n"
            "WHAT THIS IS NOT: a REGULAR QUAD GRID DERIVED FROM A LATTICE, which is\n"
            "NOT field-aligned retopology — no edge loops following the form, no\n"
            "poles at features, not animation-ready. It is the input a retopology\n"
            "pass REPLACES.\n\n"
            "Two modes, because a voxel sculpt is two different subjects:\n"
            "  'dual'  the rounded form — the same lattice dual mesh_smooth builds,\n"
            "          quads meeting four to a vertex on average. cell_size\n"
            "          generalises the lattice; coarser low-passes and can drop a\n"
            "          one-voxel feature, finer is CLAMPED to the voxel size because\n"
            "          occupancy is a step field. At the voxel size with blur 0 this\n"
            "          is mesh_smooth's mesh for the SAME LEVEL plus its quads —\n"
            "          note `level` below defaults to 0 while mesh_smooth follows\n"
            "          the ACTIVE level and cannot be asked for another, so on a\n"
            "          multi-level grid the two default calls mesh different\n"
            "          levels and match only when active_level is 0.\n"
            "  'faces' one planar quad per exposed voxel face — the boxes the model\n"
            "          actually is. Dense, corners welded within a palette colour,\n"
            "          and NO vertex normals: a welded corner faces three ways at\n"
            "          once. mesh() is unchanged and stays the merged triangle path.\n\n"
            "`target` asks for a COUNT: APPROACHED, NEVER HIT. In 'faces' mode the\n"
            "lever is the resolution LEVEL, a factor of about four per step, so a\n"
            "target usually lands on the nearest level rather than inside the\n"
            "tolerance: the search walks the stack coarsest first, stops at the\n"
            "first level that reaches the target, returns the nearer of the two it\n"
            "landed between, and ignores max_iterations because the stack is its own\n"
            "bound. quad_report['clamped'] there means the STACK RAN OUT — the target\n"
            "is below what the coarsest level THAT YIELDS ANYTHING gives or above\n"
            "what the finest gives; empty coarse levels are ordinary on a grid\n"
            "sculpted only at a fine level and do not count as a nearer end.\n"
            "quad_report['iterations'] counts every level meshed from the coarsest,\n"
            "so a target met at level k costs k+1 meshes, not the two of the\n"
            "bracket.\n"
            "Mesh.quad_report says what actually happened.\n\n"
            "The knobs take the C ABI's rules: tolerance <= 0 and max_iterations 0\n"
            "mean the defaults (0.10, 4), a negative max_iterations is refused, and\n"
            "target is capped at 16777216 quads.")
        .def(
            "sample_step_field",
            [](const PyVoxelGrid& g, nb::handle points) {
                PointsView pts = to_points(points);
                const voxel::VoxelGrid& grid = g.grid();
                float* out = new float[pts.count ? pts.count : 1];
                nb::capsule owner(out, [](void* p) noexcept { delete[] static_cast<float*>(p); });
                {
                    nb::gil_scoped_release release;
                    for (std::size_t i = 0; i < pts.count; ++i)
                        out[i] = grid.sample_step_field(
                            kernel::cf3(pts.data[i * 3], pts.data[i * 3 + 1], pts.data[i * 3 + 2]));
                }
                return nb::cast(nb::ndarray<nb::numpy, float>(out, {pts.count}, owner));
            },
            "points"_a, "Voxels as a step field for SDF compositing (a bound, not a distance)")
        .def(
            "raycast",
            [](const PyVoxelGrid& g, nb::handle origin, nb::handle direction) -> nb::object {
                math::Ray ray{to_f3(origin, "origin"),
                              kernel::cnormalize(to_f3(direction, "direction"))};
                pick::VoxelHit hit = pick::raycast_voxels(g.grid(), ray);
                if (!hit.hit) return nb::none();
                voxel::VoxelCoord adj = pick::adjacent_cell(hit);
                nb::dict d;
                d["cell"] = nb::make_tuple(hit.cell.x, hit.cell.y, hit.cell.z);
                d["face"] = hit.face;
                d["adjacent"] = nb::make_tuple(adj.x, adj.y, adj.z);
                d["t"] = hit.t;
                return d;
            },
            "origin"_a, "direction"_a,
            "Pick a voxel: cell, entry face id, and the adjacent cell to place into")
        .def(
            "build_plane_pick",
            [](const PyVoxelGrid& g, nb::handle origin, nb::handle direction,
               std::int32_t plane_cell) -> nb::object {
                math::Ray ray{to_f3(origin, "origin"),
                              kernel::cnormalize(to_f3(direction, "direction"))};
                auto cell = pick::pick_build_plane(g.grid(), ray, plane_cell);
                if (!cell) return nb::none();
                return nb::make_tuple(cell->x, cell->y, cell->z);
            },
            "origin"_a, "direction"_a, "plane_cell"_a = 0)
        .def(
            "rasterize",
            [](PyVoxelGrid& g, const PyDocument& d, nb::handle region) {
                scene::Tape tape = scene::compile_document(d.doc->document);
                math::Aabb box = tape.bounds;
                if (!region.is_none()) {
                    nb::sequence s = nb::cast<nb::sequence>(region);
                    if (nb::len(s) != 2)
                        throw std::invalid_argument("region must be ((minx,miny,minz), (max...))");
                    box = math::Aabb{to_f3(s[0], "region min"), to_f3(s[1], "region max")};
                }
                if (box.empty() || box.is_infinite())
                    throw std::invalid_argument("document has no bounded content to rasterize");
                voxel::VoxelGrid& grid = g.grid();
                nb::gil_scoped_release release;
                grid.rasterize_tape(tape, box);
            },
            "document"_a, "region"_a = nb::none(),
            "Rasterize an SDF document into voxels (colors sampled from the field)")
        .def(
            "rasterize_mesh",
            [](PyVoxelGrid& g, const PyMesh& source, nb::handle region) {
                const mesh::Mesh& m = source.data();
                if (m.empty()) throw std::invalid_argument("the mesh has no triangles");
                std::optional<math::Aabb> box;
                if (!region.is_none()) {
                    nb::sequence s = nb::cast<nb::sequence>(region);
                    if (nb::len(s) != 2)
                        throw std::invalid_argument("region must be ((minx,miny,minz), (max...))");
                    box = math::Aabb{to_f3(s[0], "region min"), to_f3(s[1], "region max")};
                    if (box->empty() || box->is_infinite())
                        throw std::invalid_argument(
                            "the region must be finite, non-empty and bounded");
                }
                voxel::VoxelGrid& grid = g.grid();
                nb::gil_scoped_release release;
                if (box)
                    grid.rasterize_mesh(m, *box);
                else
                    grid.rasterize_mesh(m);
            },
            "mesh"_a, "region"_a = nb::none(),
            "Rasterize a TRIANGLE MESH into voxels, in one sampling.\n\n"
            "An imported model reaches an SDF layer in one step (Volume.from_mesh)\n"
            "but reached a grid only through a document: triangles to a narrow band,\n"
            "band into a layer, layer rasterized. Each of those places the surface\n"
            "within about half a cell of its own lattice, so the detour quantised a\n"
            "field that was itself quantised — and a feature that survived the first\n"
            "sampling could fall between centres on the second.\n\n"
            "Membership is the GENERALIZED WINDING NUMBER at the cell centre: the\n"
            "sign that survives a hole, a flipped normal and a self-intersection,\n"
            "because those are what imported meshes have. A model with a missing\n"
            "cap rasterizes without flipping a half-space.\n\n"
            "Colour comes from the mesh's vertex colours where it has them,\n"
            "interpolated at the closest point on the nearest triangle and quantised\n"
            "to the palette by nearest entry. A mesh with no colours takes one\n"
            "neutral entry — a grid's colour is per cell and there is nothing else\n"
            "to read.\n\n"
            "`region` is OPTIONAL here, unlike `rasterize`: a document can be\n"
            "unbounded and a mesh cannot, so None means the mesh's own bounds.\n\n"
            "What the sampling costs: the surface moves by up to half a cell, a\n"
            "feature thinner than a cell can vanish (rasterize finer — nothing\n"
            "downstream can invent what was never stored), a sharp edge staircases,\n"
            "and two colours closer than the palette tolerance become one.\n\n"
            "NOT retopology and not remeshing. The mesh is not modified, and a mesh\n"
            "a document CARRIES stays never-evaluated — this is an explicit\n"
            "conversion you ask for, like every bridge.");

    m.def("load",
          [](const std::string& path) {
              PyDocument d;
              check_io(io::load_clayspace_file(path, d.doc.get()));
              return d;
          },
          "path"_a, "Load a .clayspace document");
    m.def(
        "load_bytes",
        [](nb::bytes data) {
            PyDocument d;
            check_io(io::load_clayspace(reinterpret_cast<const std::uint8_t*>(data.c_str()),
                                        data.size(), d.doc.get()));
            return d;
        },
        "data"_a,
        "Load a .clayspace document from BYTES — the counterpart to\n"
        "Document.to_bytes, and what a host uses when its documents arrive\n"
        "from a container, a database or a network request rather than from a\n"
        "path. Accepts exactly what `load` accepts and refuses a truncated or\n"
        "corrupt buffer without reading past its length.");
    m.def(
        "load_mesh_bytes",
        [](nb::bytes data, const std::string& format, std::size_t max_vertices,
           std::size_t max_triangles) {
            io::ImportBudget limits;
            if (max_vertices) limits.max_vertices = max_vertices;
            if (max_triangles) limits.max_triangles = max_triangles;
            PyMesh out;
            out.m = load_mesh_bytes_any(reinterpret_cast<const std::uint8_t*>(data.c_str()),
                                        data.size(), format, limits);
            return out;
        },
        "data"_a, "format"_a, "max_vertices"_a = 0, "max_triangles"_a = 0,
        "Load a mesh from BYTES, naming the format — 'obj', 'ply', 'fbx' or\n"
        "'glb', matched case-insensitively, because a buffer has no extension.\n\n"
        "The budget is enforced exactly as load_mesh enforces it: a buffer\n"
        "from a network or a pasteboard is the untrusted input it exists for.\n"
        "0 means the library's default.");
    m.def("backends",
          []() {
              std::vector<std::string> names;
              for (eval::Backend* b : eval::Registry::instance().all())
                  names.push_back(b->name());
              return names;
          },
          "Names of the registered evaluation backends (CPU is always present)");
}
