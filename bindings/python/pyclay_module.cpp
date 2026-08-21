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
#include "clay/brush/move.h"
#include "clay/brush/stroke.h"
#include "clay/brush/tube.h"
#include "clay/cut/cut.h"
#include "clay/eval/backend.h"
#include "clay/eval/bake_points.h"
#include "clay/field/flatten.h"
#include "clay/field/move_topological.h"
#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/lattice.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/deform.h"
#include "clay/mesh/sculpt.h"
#include "clay/mesh/transfer.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/validate.h"
#include "clay/pick/pick.h"
#include "clay/scene/armature.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/curve.h"
#include "clay/scene/tape.h"
#include "clay/version.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace nb = nanobind;
using namespace nb::literals;
using namespace clay;

namespace {

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

void place(PyPrim& p, nb::handle position, nb::handle rotation_axis_angle, float scale) {
    if (!position.is_none()) p.xform.position = to_f3(position, "position");
    if (!rotation_axis_angle.is_none()) p.xform.rotation = to_axis_angle(rotation_axis_angle);
    if (scale <= 0.0f) throw std::invalid_argument("scale must be > 0");
    p.xform.scale = scale;
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
struct PyMesh {
    mesh::Mesh m;                           // the owned mesh; empty on a borrow
    std::shared_ptr<io::ClaySpaceDoc> doc;  // non-null: borrowed from a mesh layer
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

struct PyMeshSculptor {
    nb::object owner;  // the Python Mesh, kept alive for the session's lifetime
    PyMesh* mesh = nullptr;
    mesh::Mesh* bound = nullptr;
    std::shared_ptr<mesh::MeshSculptor> sculptor;

    mesh::MeshSculptor& live(bool for_edit) const {
        if (!sculptor) throw std::runtime_error("this sculptor was never built");
        mesh::Mesh& now = for_edit ? mesh->editable() : const_cast<mesh::Mesh&>(mesh->data());
        if (&now != bound)
            throw std::runtime_error("the mesh this sculptor was built over has been replaced");
        if (!sculptor->valid())
            throw std::runtime_error(
                "the mesh changed its vertex or index count under this sculptor");
        return *sculptor;
    }
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
    if (width < 3 || width > 5)
        throw std::invalid_argument("samples must be an (N, 3), (N, 4) or (N, 5) array "
                                    "(position, optional pressure, optional tilt)");
    std::vector<brush::StrokeSample> out(view.shape(0));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float* row = view.data() + i * width;
        out[i].position = kernel::cf3(row[0], row[1], row[2]);
        if (width > 3) out[i].pressure = row[3];
        if (width > 4) out[i].tilt = row[4];
    }
    return out;
}

// -- voxel wrapper -------------------------------------------------------------

// Owns a grid, or borrows the one stored in a document's voxel layer so edits
// are visible to save()/mesh() without a copy.
struct PyVoxelGrid {
    std::shared_ptr<io::ClaySpaceDoc> doc;  // null for standalone grids
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

struct PySculptLayerScope {
    PyVoxelGrid grid;
    std::string name;
    std::size_t index = 0;
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

// -- document / layer wrappers ------------------------------------------------

// The undo stack is opt-in and shared by a document and every Layer handle
// onto it, so an edit made through a layer records into the same history as
// one made through the document. Null means undo is off, and a document with
// no stack behaves exactly as it did before the feature existed.
using UndoRef = std::shared_ptr<scene::UndoStack>;

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
                if (ease < 0 || ease >= kernel::ease_count)
                    throw std::invalid_argument("ease must be a valid easing curve index");
                p.deformers.push_back(
                    scene::Deformer::alpha(to_f3(centre, "centre"), to_f3(direction, "direction"),
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
            "read it back with `Document.safe_step_scale()`.")
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
    // rotation_axis_angle=((x, y, z), radians) and scale=<uniform factor>.
#define CLAY_PLACE_ARGS                                            \
    nb::arg("position") = nb::none(),                              \
    nb::arg("rotation_axis_angle") = nb::none(), nb::arg("scale") = 1.0f

    nb::class_<PySphere, PyPrim>(m, "Sphere", "Sphere of radius r")
        .def("__init__",
             [](PySphere* self, float r, nb::handle pos, nb::handle rot, float scale) {
                 new (self) PySphere();
                 self->prim = scene::Prim::sphere(r);
                 place(*self, pos, rot, scale);
             },
             "r"_a = 1.0f, CLAY_PLACE_ARGS);
    nb::class_<PyBox, PyPrim>(m, "Box", "Axis-aligned box; size = full side lengths")
        .def("__init__",
             [](PyBox* self, nb::handle size, nb::handle pos, nb::handle rot, float scale) {
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
                float scale) {
                 new (self) PyRoundBox();
                 kernel::cfloat3 s = to_f3(size, "size");
                 self->prim = scene::Prim::round_box(s * 0.5f, r);
                 place(*self, pos, rot, scale);
             },
             "size"_a, "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyTorus, PyPrim>(m, "Torus", "Torus: R = ring radius, r = tube radius")
        .def("__init__",
             [](PyTorus* self, float R, float r, nb::handle pos, nb::handle rot, float scale) {
                 new (self) PyTorus();
                 self->prim = scene::Prim::torus(R, r);
                 place(*self, pos, rot, scale);
             },
             "R"_a, "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyCapsule, PyPrim>(m, "Capsule", "Capsule between local endpoints a and b")
        .def("__init__",
             [](PyCapsule* self, nb::handle a, nb::handle b, float r, nb::handle pos,
                nb::handle rot, float scale) {
                 new (self) PyCapsule();
                 self->prim = scene::Prim::capsule(to_f3(a, "a"), to_f3(b, "b"), r);
                 place(*self, pos, rot, scale);
             },
             "a"_a, "b"_a, "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyCylinder, PyPrim>(m, "Cylinder",
                                   "Vertical capped cylinder: radius r, half-height h")
        .def("__init__",
             [](PyCylinder* self, float r, float h, nb::handle pos, nb::handle rot,
                float scale) {
                 new (self) PyCylinder();
                 self->prim = scene::Prim::capped_cylinder(r, h);
                 place(*self, pos, rot, scale);
             },
             "r"_a, "h"_a, CLAY_PLACE_ARGS);
    nb::class_<PyCone, PyPrim>(m, "Cone",
                               "Capped cone: half-height h, base radius r1, top radius r2")
        .def("__init__",
             [](PyCone* self, float h, float r1, float r2, nb::handle pos, nb::handle rot,
                float scale) {
                 new (self) PyCone();
                 self->prim = scene::Prim::capped_cone(h, r1, r2);
                 place(*self, pos, rot, scale);
             },
             "h"_a, "r1"_a, "r2"_a, CLAY_PLACE_ARGS);
    nb::class_<PyRoundCone, PyPrim>(
        m, "RoundCone", "Sphere-swept cone: radius r1 at origin, r2 at height h up the y axis")
        .def("__init__",
             [](PyRoundCone* self, float r1, float r2, float h, nb::handle pos, nb::handle rot,
                float scale) {
                 new (self) PyRoundCone();
                 self->prim = scene::Prim::round_cone(r1, r2, h);
                 place(*self, pos, rot, scale);
             },
             "r1"_a, "r2"_a, "h"_a, CLAY_PLACE_ARGS);
    nb::class_<PyEllipsoid, PyPrim>(m, "Ellipsoid",
                                    "Ellipsoid with per-axis radii r=(rx, ry, rz) (bound field)")
        .def("__init__",
             [](PyEllipsoid* self, nb::handle r, nb::handle pos, nb::handle rot, float scale) {
                 new (self) PyEllipsoid();
                 self->prim = scene::Prim::ellipsoid(to_f3(r, "r"));
                 place(*self, pos, rot, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);
    nb::class_<PyOctahedron, PyPrim>(m, "Octahedron", "Octahedron of size s")
        .def("__init__",
             [](PyOctahedron* self, float s, nb::handle pos, nb::handle rot, float scale) {
                 new (self) PyOctahedron();
                 self->prim = scene::Prim::octahedron(s);
                 place(*self, pos, rot, scale);
             },
             "s"_a, CLAY_PLACE_ARGS);
    nb::class_<PyHexPrism, PyPrim>(m, "HexPrism",
                                   "Hexagonal prism: hx = flat-to-flat half-width, hy = half-height")
        .def("__init__",
             [](PyHexPrism* self, float hx, float hy, nb::handle pos, nb::handle rot,
                float scale) {
                 new (self) PyHexPrism();
                 self->prim = scene::Prim::hex_prism(hx, hy);
                 place(*self, pos, rot, scale);
             },
             "hx"_a, "hy"_a, CLAY_PLACE_ARGS);
    nb::class_<PyPyramid, PyPrim>(m, "Pyramid", "Unit-base pyramid of height h")
        .def("__init__",
             [](PyPyramid* self, float h, nb::handle pos, nb::handle rot, float scale) {
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
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyCappedTorus();
                 self->prim = scene::Prim::capped_torus(aperture, ra, rb);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "aperture"_a, "ra"_a, "rb"_a, CLAY_PLACE_ARGS);

    nb::class_<PyLink, PyPrim>(m, "Link",
                           "Chain link: straight length, ring radius r1, tube radius r2")
        .def("__init__",
             [](PyLink* self, float length, float r1, float r2, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyLink();
                 self->prim = scene::Prim::link(length, r1, r2);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "length"_a, "r1"_a, "r2"_a, CLAY_PLACE_ARGS);

    nb::class_<PyCylinderInfinite, PyPrim>(m, "CylinderInfinite",
                           "Infinite cylinder along Y (UNBOUNDED: never culled)")
        .def("__init__",
             [](PyCylinderInfinite* self, float cx, float cz, float r, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyCylinderInfinite();
                 self->prim = scene::Prim::cylinder_infinite(cx, cz, r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "cx"_a = 0.0f, "cz"_a = 0.0f, "r"_a = 1.0f, CLAY_PLACE_ARGS);

    nb::class_<PyExactCone, PyPrim>(m, "ExactCone",
                           "Exact cone: half-angle at the apex, height h")
        .def("__init__",
             [](PyExactCone* self, float half_angle, float h, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyExactCone();
                 self->prim = scene::Prim::cone(half_angle, h);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "half_angle"_a, "h"_a, CLAY_PLACE_ARGS);

    nb::class_<PyPlane, PyPrim>(m, "Plane",
                           "Half-space with the given normal and offset (UNBOUNDED: never culled)")
        .def("__init__",
             [](PyPlane* self, nb::handle normal, float offset, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyPlane();
                 self->prim = scene::Prim::plane(to_f3(normal, "normal"), offset);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "normal"_a, "offset"_a = 0.0f, CLAY_PLACE_ARGS);

    nb::class_<PyCutSphere, PyPrim>(m, "CutSphere",
                           "Sphere of radius r cut by the plane y = h")
        .def("__init__",
             [](PyCutSphere* self, float r, float h, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyCutSphere();
                 self->prim = scene::Prim::cut_sphere(r, h);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, "h"_a, CLAY_PLACE_ARGS);

    nb::class_<PyCutHollowSphere, PyPrim>(m, "CutHollowSphere",
                           "Hollow cut sphere of wall thickness t")
        .def("__init__",
             [](PyCutHollowSphere* self, float r, float h, float t, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyCutHollowSphere();
                 self->prim = scene::Prim::cut_hollow_sphere(r, h, t);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, "h"_a, "t"_a, CLAY_PLACE_ARGS);

    nb::class_<PySolidAngle, PyPrim>(m, "SolidAngle",
                           "Spherical wedge: cone half-angle and radius")
        .def("__init__",
             [](PySolidAngle* self, float angle, float ra, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PySolidAngle();
                 self->prim = scene::Prim::solid_angle(angle, ra);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "angle"_a, "ra"_a, CLAY_PLACE_ARGS);

    nb::class_<PyTetrahedron, PyPrim>(m, "Tetrahedron",
                           "Regular tetrahedron of size r")
        .def("__init__",
             [](PyTetrahedron* self, float r, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyTetrahedron();
                 self->prim = scene::Prim::tetrahedron(r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);

    nb::class_<PyDodecahedron, PyPrim>(m, "Dodecahedron",
                           "Regular dodecahedron (plane folds)")
        .def("__init__",
             [](PyDodecahedron* self, float r, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyDodecahedron();
                 self->prim = scene::Prim::dodecahedron(r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);

    nb::class_<PyIcosahedron, PyPrim>(m, "Icosahedron",
                           "Regular icosahedron (plane folds)")
        .def("__init__",
             [](PyIcosahedron* self, float r, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyIcosahedron();
                 self->prim = scene::Prim::icosahedron(r);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "r"_a, CLAY_PLACE_ARGS);

    nb::class_<PyTriPrism, PyPrim>(m, "TriPrism",
                           "Triangular prism (BOUND field, not exact)")
        .def("__init__",
             [](PyTriPrism* self, float hx, float hy, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyTriPrism();
                 self->prim = scene::Prim::tri_prism(hx, hy);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "hx"_a, "hy"_a, CLAY_PLACE_ARGS);

    nb::class_<PyOctahedronCheap, PyPrim>(m, "OctahedronCheap",
                           "Cheap octahedron (BOUND field, not exact)")
        .def("__init__",
             [](PyOctahedronCheap* self, float s, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyOctahedronCheap();
                 self->prim = scene::Prim::octahedron_cheap(s);
                 place(*self, position, rotation_axis_angle, scale);
             },
             "s"_a, CLAY_PLACE_ARGS);

    nb::class_<PyLNormSphere, PyPrim>(m, "LNormSphere",
                           "Superellipsoid / L-norm sphere, n >= 2 (BOUND field)")
        .def("__init__",
             [](PyLNormSphere* self, float r, float n, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
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
                float scale) {
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
             "rotation_axis_angle"_a = nb::none(), "scale"_a = 1.0f)
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
                nb::handle position, nb::handle rotation_axis_angle, float scale) {
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
                nb::handle rotation_axis_angle, float scale) {
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
                nb::handle position, nb::handle rotation_axis_angle, float scale) {
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
                float scale) {
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
               nb::handle position, nb::handle rotation_axis_angle, float scale) {
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
                out.volume = std::make_shared<const field::FieldVolume>(field::FieldVolume::sample(
                    [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, region, cell, width));
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
               nb::handle rotation_axis_angle, float scale) {
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
               nb::handle position, nb::handle rotation_axis_angle, float scale) {
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
               nb::handle rotation_axis_angle, float scale) {
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
                        field::move_topological(
                            [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, where, cell,
                            width, settings));
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
               nb::handle position, nb::handle rotation_axis_angle, float scale,
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
                        [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, where, cell, width,
                        settings));
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
                nb::handle position, nb::handle rotation_axis_angle, float scale) {
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
             "rotation_axis_angle"_a = nb::none(), "scale"_a = 1.0f)
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
             "True when every edge is shared by exactly two triangles")
        .def("is_manifold",
             [](const PyMesh& pm) { return mesh::validate(pm.data()).manifold; },
             "True when no edge has more than two incident triangles")
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
        .def("save",
             [](const PyMesh& pm, const std::string& path) { save_mesh_any(pm.data(), path); },
             "path"_a, "Save by extension: .obj, .ply, .fbx or .glb");

    // -- fixed-topology mesh brushes -------------------------------------------------
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
            "refresh", [](PyMeshSculptor& s) { s.live(false).refresh_bvh(); },
            "Rebuild the ray tree over the vertices as they now are. Until you\n"
            "call this, `raycast` reports the surface as it was when the tree was\n"
            "built — which is usually what a stroke wants, since a brush that\n"
            "keeps its depth from the first pick is how sculpting feels right.");

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
                 scene::SetTransformCmd cmd{l.id, node, n->xform};
                 if (!position.is_none()) cmd.xform.position = to_f3(position, "position");
                 if (!rotation_axis_angle.is_none())
                     cmd.xform.rotation = to_axis_angle(rotation_axis_angle);
                 if (!scale.is_none()) cmd.xform.scale = nb::cast<float>(scale);
                 apply_or_throw(l.doc->document, scene::Command{cmd}, "set_transform", l.undo.get());
             },
             "node"_a, "position"_a = nb::none(), "rotation_axis_angle"_a = nb::none(),
             "scale"_a = nb::none(),
             "Retransform a placed node; omitted arguments keep their current value")
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
                 out["item_count"] = r.item_count;
                 out["advises_consolidation"] = r.advises_consolidation;
                 return out;
             },
             "advise_below_step_scale"_a = 0.0f,
             "What this layer's chain costs the marcher, and what is causing it.\n\n"
             "The region verbs each work once and none of them chain, for TWO\n"
             "different reasons. A polish samples a document and hands back a\n"
             "volume, so the second pass samples a VOLUME — `steepest_volume`\n"
             "is that. A move stroke never touches a volume at all: each drag\n"
             "appends a grab to the deformer chain and those multiply —\n"
             "`longest_deformer_chain` is that. The aggregate step scale says\n"
             "something is wrong; those two say which thing.\n\n"
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
        .def("consolidate",
             [](PyLayer& l, float cell, nb::handle band, nb::handle padding, nb::handle region,
                bool redistance) {
                 scene::ConsolidationCost cost;
                 scene::ConsolidationParams p =
                     to_consolidation(cell, band, padding, region, redistance);
                 if (l.layer().protected_from_edits())
                     throw std::invalid_argument("layer is protected (ghosted or locked)");
                 if (!scene::consolidate_layer(l.doc->document, l.id, p,
                                               l.undo ? l.undo->get() : nullptr, &cost,
                                               eval::pooled_bake_eval()))
                     throw std::invalid_argument(
                         "nothing to consolidate: the layer is empty, unbounded, or the region "
                         "contains no surface");
                 return cost_dict(cost);
             },
             "cell"_a, "band"_a = nb::none(), "padding"_a = nb::none(), "region"_a = nb::none(),
             "redistance"_a = true,
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
                 scene::Layer& l = d.doc->document.add_sdf_layer(name);
                 l.kind = scene::LayerKind::Voxel;
                 l.sdf.reset();
                 d.doc->voxel_layers.emplace(l.id, voxel::VoxelGrid(voxel_size));
                 PyVoxelGrid g;
                 g.doc = d.doc;
                 g.layer = l.id;
                 return g;
             },
             "name"_a, "voxel_size"_a = 0.1f,
             "Add a voxel layer and return its grid (edits are stored in the document)")
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
                nb::handle band, nb::handle layer) {
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
                         [&tape](kernel::cfloat3 p) { return tape.eval(p).d; }, *m, settings);
                 }
                 if (!volume)
                     throw std::invalid_argument(
                         "nothing to extrude: the mask is empty, does not reach the surface, or "
                         "the wall is thinner than a cell");
                 PyVolume out;
                 out.prim = scene::Prim::volume();
                 out.volume = std::make_shared<const field::FieldVolume>(std::move(*volume));
                 return out;
             },
             "mask"_a, "thickness"_a, "side"_a = "outward", "threshold"_a = 0.5f,
             "border_round"_a = 0.0f, "border_smooth"_a = 0, "cell_size"_a = nb::none(),
             "band"_a = nb::none(), "layer"_a = nb::none(),
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
                 {
                     nb::gil_scoped_release release;
                     hit = pick::raycast_scene(d.doc->document, ray);
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
                 {
                     nb::gil_scoped_release release;
                     if (cpu->raycast(tape, q, hits.data()) != eval::Status::Ok)
                         throw std::runtime_error("batch raycast failed");
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
                 apply_or_throw(d.doc->document, scene::Command{scene::RemoveLayerCmd{layer}},
                                "move_layer", d.undo.get());
                 apply_or_throw(d.doc->document,
                                scene::Command{scene::AddLayerCmd{std::move(copy), index}},
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
                 if (!*d.undo) *d.undo = std::make_shared<scene::UndoStack>();
             },
             "Start recording edits. Off by default, so a document that never "
             "calls this pays nothing; edits made before it are not undoable.")
        .def_prop_ro("undo_enabled", [](const PyDocument& d) { return bool(*d.undo); })
        .def("undo",
             [](PyDocument& d) {
                 if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                 return (*d.undo)->undo(d.doc->document);
             },
             "Reverse the last recorded step; returns False when there is nothing to undo")
        .def("redo",
             [](PyDocument& d) {
                 if (!*d.undo) throw std::runtime_error("undo is not enabled on this document");
                 return (*d.undo)->redo(d.doc->document);
             },
             "Reapply the last undone step; returns False when there is nothing to redo")
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
                 m.field().set(to_coord(cell), value);
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
                 m.field().paint(to_f3(point, "point"),
                                 make_brush(n, shape, falloff, strength, 0u), target);
             },
             "point"_a, "size"_a, "target"_a = 1.0f, "shape"_a = "sphere",
             "falloff"_a = "smooth", "strength"_a = 1.0f,
             "Brush the mask toward `target` at a world position. target=1 masks, "
             "target=0 erases. Size is in mask cells.")
        .def("paint_cell",
             [](PyMaskField& m, nb::handle cell, int n, float target, const std::string& shape,
                const std::string& falloff, float strength) {
                 m.field().paint(to_coord(cell), make_brush(n, shape, falloff, strength, 0u),
                                 target);
             },
             "cell"_a, "size"_a, "target"_a = 1.0f, "shape"_a = "sphere",
             "falloff"_a = "smooth", "strength"_a = 1.0f,
             "As paint(), centred on a mask cell rather than a world position")
        .def("invert", [](PyMaskField& m) { m.field().invert(); },
             "Flip the painted region (a sparse field has no finite complement)")
        .def("clear", [](PyMaskField& m) { m.field().clear(); })
        .def("expand", [](PyMaskField& m, int steps) { m.field().expand(steps); }, "steps"_a = 1,
             "Grow the mask by grey dilation")
        .def("contract", [](PyMaskField& m, int steps) { m.field().contract(steps); },
             "steps"_a = 1, "Shrink the mask by grey erosion")
        .def("smooth", [](PyMaskField& m, int iterations) { m.field().smooth(iterations); },
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
                 m.field().fill(to_aabb(region), value);
             },
             "region"_a, "value"_a = 1.0f,
             "Set every cell whose centre lies in a ((lo), (hi)) world box. "
             "Filling with 0 releases the region.")
        .def("invert_within",
             [](PyMaskField& m, nb::handle region) { m.field().invert_within(to_aabb(region)); },
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
                g.grid().set(to_coord(cell), index);
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
            "erase", [](PyVoxelGrid& g, nb::handle cell) { g.grid().erase(to_coord(cell)); },
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
                g.grid().paint(to_coord(cell), index);
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
                g.grid().sculpt_smooth(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask));
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none(),
            "Majority filter over the 26-neighbourhood: spurs dissolve, notches fill")
        .def(
            "sculpt_inflate",
            [](PyVoxelGrid& g, nb::handle cell, int n, int amount, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().sculpt_inflate(
                    to_coord(cell), make_brush(n, shape, falloff, strength, seed, mask), amount);
            },
            "cell"_a, "size"_a, "amount"_a = 1, "shape"_a = "sphere", "falloff"_a = "constant",
            "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Dilate for a positive amount, erode for a negative one")
        .def(
            "sculpt_flatten",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle normal, float offset,
               const std::string& shape, const std::string& falloff, float strength,
               std::uint32_t seed, nb::handle mask) {
                g.grid().sculpt_flatten(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask),
                                        to_f3(normal, "normal"), offset);
            },
            "cell"_a, "size"_a, "normal"_a, "offset"_a = 0.0f, "shape"_a = "sphere",
            "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
            "Pull the surface onto the plane through the brush centre")
        .def(
            "sculpt_grab",
            [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle displacement,
               const std::string& shape, const std::string& falloff, float strength,
               std::uint32_t seed, bool front_only, nb::handle mask) {
                g.grid().sculpt_grab(to_coord(cell),
                                     make_brush(n, shape, falloff, strength, seed, mask),
                                     to_f3(displacement, "displacement"), front_only);
            },
            "cell"_a, "size"_a, "displacement"_a, "shape"_a = "sphere", "falloff"_a = "smooth",
            "strength"_a = 1.0f, "seed"_a = 0u, "front_only"_a = false, "mask"_a = nb::none(),
            "Translate occupancy through the same map the SDF grab uses. Binary "
            "occupancy means this resamples nearest-cell, so material moves in "
            "whole cells rather than flowing.")
        .def(
            "sculpt_pinch",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().sculpt_pinch(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed, mask));
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none(),
            "Move surface cells one step toward the brush centre")
        .def(
            "sculpt_magnify",
            [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().sculpt_magnify(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask));
            },
            "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
            "seed"_a = 0u, "mask"_a = nb::none(),
            "Move surface cells one step AWAY from the brush centre: pinch's\n"
            "inverse, sharing its walk so the two cannot drift apart")
        .def(
            "sculpt_fill_cavities",
            [](PyVoxelGrid& g, nb::handle cell, int n, int passes, const std::string& shape,
               const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                g.grid().sculpt_fill_cavities(
                    to_coord(cell), make_brush(n, shape, falloff, strength, seed, mask), passes);
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
                g.grid().sculpt_scrape(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask),
                                       to_f3(normal, "normal"), offset);
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
                g.grid().sculpt_smudge(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask),
                                       to_f3(displacement, "displacement"));
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
                if (!g.grid().sculpt_carve_alpha(
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
                g.grid().repair_close_holes(passes, borrow_mask(mask));
            },
            "passes"_a = 1, "mask"_a = nb::none(),
            "Seal perforations over the whole grid by the pocket rule. Only ever "
            "adds cells, so no material is lost.")
        .def(
            "repair_fill_voids",
            [](PyVoxelGrid& g, nb::handle mask) { g.grid().repair_fill_voids(borrow_mask(mask)); },
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
                g.grid().fill_box(to_coord(a), to_coord(b), index);
            },
            "a"_a, "b"_a, "index"_a, "Inclusive-corner box fill")
        .def(
            "fill_line",
            [](PyVoxelGrid& g, nb::handle a, nb::handle b, std::uint8_t index) {
                g.grid().fill_line(to_coord(a), to_coord(b), index);
            },
            "a"_a, "b"_a, "index"_a)
        .def(
            "set_mirrored",
            [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index, const std::string& axes) {
                std::uint8_t mask = 0;
                for (char c : axes) mask |= static_cast<std::uint8_t>(1u << parse_axis({c}));
                g.grid().set_mirrored(to_coord(cell), index, mask);
            },
            "cell"_a, "index"_a, "axes"_a = "x",
            "Set the cell and every mirror combination of the given axes ('x', 'xz', ...)")
        .def(
            "paint_mirrored",
            [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index, const std::string& axes) {
                std::uint8_t mask = 0;
                for (char c : axes) mask |= static_cast<std::uint8_t>(1u << parse_axis({c}));
                g.grid().paint_mirrored(to_coord(cell), index, mask);
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
    m.def("backends",
          []() {
              std::vector<std::string> names;
              for (eval::Backend* b : eval::Registry::instance().all())
                  names.push_back(b->name());
              return names;
          },
          "Names of the registered evaluation backends (CPU is always present)");
}
