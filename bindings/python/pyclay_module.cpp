// pyclay — nanobind extension module (python-bindings spec): numpy-native
// document authoring, field evaluation, meshing, and file I/O over the
// public claycore C++ API. This target compiles WITH exceptions (Python
// error protocol); the core library itself never throws.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/dual_contouring.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/to_field.h"
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/validate.h"
#include "clay/pick/pick.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "clay/brush/stroke.h"
#include "clay/cut/cut.h"
#include "clay/field/flatten.h"
#include "clay/field/relax.h"
#include "clay/field/volume.h"
#include "clay/voxel/mask.h"
#include "clay/version.h"

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
    std::vector<scene::Deformer> deformers;
    scene::Profile profile;
    std::vector<kernel::cfloat2> profile_points;
    std::vector<scene::Profile> profiles;                       // Loft only
    std::vector<std::vector<kernel::cfloat2>> profile_polygons;  // Loft only
    std::shared_ptr<const field::FieldVolume> volume;            // Volume only
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

struct PyMesh {
    mesh::Mesh m;
};

// Zero-copy (owner-tracked) view over a vector of cfloat3 (plain x,y,z floats).
nb::object f3_view(nb::object owner, const std::vector<kernel::cfloat3>& v) {
    if (v.empty()) {
        nb::module_ np = nb::module_::import_("numpy");
        return np.attr("empty")(nb::make_tuple(0, 3), "dtype"_a = "float32");
    }
    return nb::cast(nb::ndarray<nb::numpy, const float>(&v.front().x, {v.size(), 3}, owner));
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

mesh::Mesh load_mesh_any(const std::string& path) {
    std::size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    mesh::Mesh out;
    if (ext == "obj") {
        check_io(io::load_obj_file(path, &out));
    } else if (ext == "ply") {
        check_io(io::load_ply_file(path, &out));
    } else if (ext == "fbx") {
        check_io(io::load_fbx_file(path, &out));
    } else {
        // .glb is saved but not loaded; saying so beats a generic failure.
        throw std::invalid_argument("unsupported mesh extension '." + ext +
                                    "' for loading (supported: .obj, .ply, .fbx)");
    }
    return out;
}

// A cut's swept region: the document being cut, which is the answer in every
// real use, or an explicit ((lo), (hi)) pair for a caller that wants to bound
// it by hand.
math::Aabb to_aabb(nb::handle obj);

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

// Owns a mask, or borrows the one a document holds for a layer, mirroring
// PyVoxelGrid so a mask edited through a document is the one that gets saved.
struct PyMaskField {
    std::shared_ptr<io::ClaySpaceDoc> doc;  // null for standalone masks
    scene::LayerId layer = 0;
    std::shared_ptr<voxel::MaskField> owned;

    voxel::MaskField& field() const {
        if (owned) return *owned;
        auto it = doc->masks.find(layer);
        if (it == doc->masks.end())
            throw std::runtime_error("mask was removed from its document");
        return it->second;
    }
};

const voxel::MaskField* borrow_mask(nb::handle mask) {
    if (!mask.is_valid() || mask.is_none()) return nullptr;
    // Borrowed for the duration of the call only, which is all a BrushParams
    // built at the call site needs.
    return &nb::cast<PyMaskField&>(mask).field();
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
    return math::Aabb(to_f3(s[0], "region lo"), to_f3(s[1], "region hi"));
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
        // spatial morphs: need a transition= argument, and are NON-LOCAL
        // (never culled) because their weight reaches arbitrarily far
        .value("TRANSITION_LINEAR", scene::Op::TransitionLinear)
        .value("TRANSITION_RADIAL", scene::Op::TransitionRadial);

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
        .def("at",
             [](nb::object self, nb::handle position) {
                 nb::cast<PyPrim&>(self).xform.position = to_f3(position, "position");
                 return self;
             },
             "position"_a, "Set the primitive position; returns self for chaining")
        // -- deformer modifiers (add-tape-deformers). Chainable and ordered:
        // the point is warped by the first-called deformer first.
        .def("twist",
             [](nb::object self, float k) {
                 nb::cast<PyPrim&>(self).deformers.push_back(scene::Deformer::twist(k));
                 return self;
             },
             "k"_a, "Twist about Y at k radians per unit of height")
        .def("bend",
             [](nb::object self, float k) {
                 nb::cast<PyPrim&>(self).deformers.push_back(scene::Deformer::bend(k));
                 return self;
             },
             "k"_a, "Bend along X at k radians per unit")
        .def("taper",
             [](nb::object self, float y0, float y1, float s0, float s1, int ease) {
                 if (y1 == y0) throw std::invalid_argument("taper needs y1 != y0");
                 if (s0 <= 0.0f || s1 <= 0.0f)
                     throw std::invalid_argument("taper scales must be > 0");
                 if (ease < 0 || ease >= kernel::ease_count)
                     throw std::invalid_argument("ease must be a valid easing curve index");
                 nb::cast<PyPrim&>(self).deformers.push_back(scene::Deformer::taper(
                     y0, y1, s0, s1, static_cast<std::uint8_t>(ease)));
                 return self;
             },
             "y0"_a, "y1"_a, "s0"_a, "s1"_a, "ease"_a = 0,
             "Scale the cross-section from s0 at y0 to s1 at y1 along an easing curve")
        .def("grab",
             [](nb::object self, nb::handle center, float radius, nb::handle displacement,
                int ease, bool front_only) {
                 PyPrim& p = nb::cast<PyPrim&>(self);
                 if (!(radius > 0.0f))
                     throw std::invalid_argument("grab radius must be > 0");
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
        .def("pose",
             [](nb::object self, nb::handle center, float radius, nb::handle axis, float angle,
                int ease) {
                 PyPrim& p = nb::cast<PyPrim&>(self);
                 if (!(radius > 0.0f))
                     throw std::invalid_argument("pose radius must be > 0");
                 p.deformers.push_back(scene::Deformer::pose(
                     to_f3(center, "center"), radius, to_f3(axis, "axis"), angle,
                     static_cast<std::uint8_t>(ease)));
                 return self;
             },
             "center"_a, "radius"_a, "axis"_a, "angle"_a, "ease"_a = 0,
             nb::rv_policy::reference_internal,
             "Rotate a region about the centre, weighted the same way as grab")
        .def("pose_line",
             [](nb::object self, nb::handle a, nb::handle b, nb::handle axis, float angle,
                int ease) {
                 PyPrim& p = nb::cast<PyPrim&>(self);
                 kernel::cfloat3 pa = to_f3(a, "a"), pb = to_f3(b, "b");
                 if (kernel::cdot2(pb - pa) <= 0.0f)
                     throw std::invalid_argument(
                         "pose_line needs a != b: the segment is the ramp");
                 p.deformers.push_back(scene::Deformer::pose_line(
                     pa, pb, to_f3(axis, "axis"), angle, static_cast<std::uint8_t>(ease)));
                 return self;
             },
             "a"_a, "b"_a, "axis"_a, "angle"_a, "ease"_a = 0,
             nb::rv_policy::reference_internal,
             "Rotate about the axis through a, ramping from nothing at a to the full "
             "angle at b and beyond. Unlike pose() this does not stop at a radius: "
             "everything past b turns with the tip.")
        .def("bend_linear",
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
        .def("bend_radial",
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
        .def("elongate_axis",
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
        .def("elongate",
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
        .def("displace",
             [](nb::object self, float amplitude, float frequency) {
                 nb::cast<PyPrim&>(self).deformers.push_back(
                     scene::Deformer::displace(amplitude, frequency));
                 return self;
             },
             "amplitude"_a, "frequency"_a,
             "Procedural sine displacement of the field (amplitude in world units)")
        .def("wrap_around",
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
        .def("repeat_grid",
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
                         throw std::invalid_argument(
                             "finite grids use one spacing for all axes");
                     p.repeat = scene::Repeat::grid_finite(s3.x, c);
                 }
                 return self;
             },
             "spacing"_a, "counts"_a = nb::none(),
             "Repeat on a grid: infinite without counts, otherwise the max cell "
             "index per axis. An infinite grid is never culled (unbounded influence).")
        .def("repeat_radial",
             [](nb::object self, int count, float offset) {
                 if (count < 2) throw std::invalid_argument("radial count must be >= 2");
                 nb::cast<PyPrim&>(self).repeat = scene::Repeat::radial(count, offset);
                 return self;
             },
             "count"_a, "offset"_a = 0.0f,
             "Circular array of `count` copies about Y at the given radius")
        .def_prop_ro("repeat",
                     [](const PyPrim& p) -> nb::object {
                         if (!p.repeat.active()) return nb::none();
                         nb::dict d;
                         const char* names[] = {"none", "grid_infinite", "grid_finite", "radial"};
                         d["type"] = p.repeat.type < 4 ? names[p.repeat.type] : "unknown";
                         d["spacing"] = nb::make_tuple(p.repeat.spacing.x, p.repeat.spacing.y,
                                                       p.repeat.spacing.z);
                         d["counts"] = nb::make_tuple(p.repeat.counts.x, p.repeat.counts.y,
                                                      p.repeat.counts.z);
                         return d;
                     },
                     "The repetition applied to this primitive, or None")
        .def_prop_ro("deformers",
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
                    region = math::Aabb(region.min - pad, region.max + pad);
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
                         return static_cast<double>(v.volume->to_blob().size() * sizeof(float)) /
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
                if (mesh.m.triangle_count() == 0)
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
                    volume = mesh::to_field(mesh.m, settings);
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
        .def(
            "relaxed",
            [](const PyVolume& self, float strength, int radius_cells, int iterations,
               nb::handle centre, float region_radius, float falloff) {
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
            "it is too narrow to hide the seam the kernel makes.")
        .def_static(
            "flattened_from",
            [](nb::handle source, nb::handle plane_point, nb::handle plane_normal, float cell,
               nb::handle band, nb::handle bounds, float strength, nb::handle centre,
               float region_radius, float falloff, nb::handle position,
               nb::handle rotation_axis_angle, float scale) {
                if (!(cell > 0.0f)) throw std::invalid_argument("cell must be > 0");
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
                    where = math::Aabb(where.min - pad, where.max + pad);
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
            CLAY_PLACE_ARGS,
            "Sample a document with a flatten applied, in one pass — the verb SDF\n"
            "layers were missing, since voxels have had sculpt_flatten all along.\n\n"
            "TWO-SIDED, matching the voxel verb: material on the normal's side\n"
            "goes AND hollows on the other side fill. It is not a subtract, and\n"
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
            "safe step scale drops to match. FLATTEN BAKES, as relax does.")
        .def(
            "flattened",
            [](const PyVolume& self, nb::handle plane_point, nb::handle plane_normal,
               float strength, nb::handle centre, float region_radius, float falloff) {
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
            "region_radius"_a = 0.0f, "falloff"_a = 0.0f,
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
            "engine, the same rule Cut follows. FLATTEN BAKES, as relax does.")
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
          [](const std::string& path) {
              PyMesh out;
              {
                  nb::gil_scoped_release release;
                  out.m = load_mesh_any(path);
              }
              return out;
          },
          "path"_a,
          "Load a mesh by extension: .obj, .ply, .fbx. The counterpart to\n"
          "Mesh.save, and what gives Volume.from_mesh something to sample.\n"
          "(.glb is written but not read.)");

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
                 if (mesh.m.triangle_count() == 0)
                     throw std::invalid_argument("a mesh with no triangles has no surface");
                 new (self) PyMeshQuery{mesh::Bvh::build(mesh.m)};
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
                     [](nb::object self) { return f3_view(self, nb::cast<PyMesh&>(self).m.positions); })
        .def_prop_ro("normals",
                     [](nb::object self) { return f3_view(self, nb::cast<PyMesh&>(self).m.normals); })
        .def_prop_ro("colors",
                     [](nb::object self) { return f3_view(self, nb::cast<PyMesh&>(self).m.colors); })
        .def_prop_ro("indices",
                     [](nb::object self) -> nb::object {
                         const PyMesh& pm = nb::cast<const PyMesh&>(self);
                         if (pm.m.indices.empty()) {
                             nb::module_ np = nb::module_::import_("numpy");
                             return np.attr("empty")(nb::make_tuple(0, 3), "dtype"_a = "uint32");
                         }
                         return nb::cast(nb::ndarray<nb::numpy, const std::uint32_t>(
                             pm.m.indices.data(), {pm.m.indices.size() / 3, 3}, self));
                     })
        .def_prop_ro("triangle_count", [](const PyMesh& pm) { return pm.m.triangle_count(); })
        .def("is_watertight",
             [](const PyMesh& pm) { return mesh::validate(pm.m).watertight; },
             "True when every edge is shared by exactly two triangles")
        .def("is_manifold",
             [](const PyMesh& pm) { return mesh::validate(pm.m).manifold; },
             "True when no edge has more than two incident triangles")
        .def("save",
             [](const PyMesh& pm, const std::string& path) { save_mesh_any(pm.m, path); },
             "path"_a, "Save by extension: .obj, .ply, .fbx or .glb");

    // -- layer -----------------------------------------------------------------------
    nb::class_<PyLayer>(m, "Layer", "SDF layer: an ordered edit list inside a Document")
        .def_prop_ro("id", [](const PyLayer& l) { return l.id; },
                     "The layer's id, which the document-level layer edits take")
        .def_prop_ro("name", [](const PyLayer& l) { return l.layer().name; })
        .def_prop_ro("resolution", [](const PyLayer& l) { return l.layer().resolution; })
        .def("add",
             [](PyLayer& l, const PyPrim& prim, scene::Op op, nb::handle blend, nb::handle color,
                nb::handle rounding, bool mirror, nb::handle transition) {
                 if (op == scene::Op::None)
                     throw std::invalid_argument(
                         "op must be a combine operator, not Op.NONE");
                 scene::Node n;
                 n.prim = prim.prim;
                 n.xform = prim.xform;
                 n.stroke = prim.stroke;
                 n.stroke_blend_k = prim.stroke_blend_k;
                 n.stroke_closed = prim.stroke_closed;
                 n.curve_tolerance = prim.curve_tolerance;
                 n.deformers = prim.deformers;
                 n.profile = prim.profile;
                 n.profile_points = prim.profile_points;
                 n.profiles = prim.profiles;
                 n.profile_polygons = prim.profile_polygons;
                 n.volume = prim.volume;
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
                 // Through the command vocabulary (AddNodeCmd with a reserved
                 // id, since replay preserves ids) so an enabled undo stack
                 // records the add like every other edit. Regression: a
                 // direct insert let adds escape undo.
                 n.id = l.layer().sdf->reserve_id();
                 scene::NodeId id = n.id;
                 std::vector<scene::Node> subtree;
                 subtree.push_back(std::move(n));
                 apply_or_throw(l.doc->document,
                                scene::Command{scene::AddNodeCmd{l.id, scene::kNoNode, -1,
                                                                 std::move(subtree)}},
                                "add", l.undo.get());
                 return id;
             },
             "prim"_a, "op"_a = scene::Op::Add, "blend"_a = nb::none(), "color"_a = nb::none(),
             "rounding"_a = nb::none(), "mirror"_a = false, "transition"_a = nb::none(),
             "Append an edit to the layer; returns the node id")
        .def("set_points",
             [](PyLayer& l, scene::NodeId node, nb::handle points, nb::handle types, bool closed,
                float tolerance, nb::handle in_handles, nb::handle out_handles) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
                 if (!(tolerance > 0.0f)) throw std::invalid_argument("tolerance must be > 0");
                 std::vector<scene::StrokePoint> pts = to_stroke_points(points);
                 apply_point_types(pts, types);
                 apply_handles(pts, in_handles, out_handles);
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
        .def("apply_stroke",
             [](PyLayer& l, nb::handle samples, const brush::StrokePreset& preset,
                const PyPrim& prim, scene::Op op, nb::handle blend, nb::handle color,
                nb::handle mask) {
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
                 templ.op = op;
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
             "color"_a = nb::none(), "mask"_a = nb::none(),
             "Resolve a stroke and append one edit per stamp; returns their node "
             "ids. The prim is the stamp template, scaled to each stamp's radius. "
             "The whole stroke is one undo step, and a masked stamp emits nothing.")
        .def("set_transform",
             [](PyLayer& l, scene::NodeId node, nb::handle position,
                nb::handle rotation_axis_angle, nb::handle scale) {
                 const scene::Node* n = l.layer().sdf->find(node);
                 if (!n) throw std::invalid_argument("no node with that id in this layer");
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
                     if (cmd.op == scene::Op::None)
                         throw std::invalid_argument(
                             "op must be a combine operator, not Op.NONE");
                 }
                 if (!blend.is_none()) cmd.blend = nb::cast<const PyBlend&>(blend).b;
                 if (!rounding.is_none()) {
                     cmd.rounding = nb::cast<float>(rounding);
                     if (cmd.rounding < 0.0f)
                         throw std::invalid_argument("rounding must be >= 0");
                 }
                 apply_or_throw(l.doc->document, scene::Command{cmd}, "set_op_blend", l.undo.get());
             },
             "node"_a, "op"_a = nb::none(), "blend"_a = nb::none(), "rounding"_a = nb::none(),
             "Change how a placed node combines; omitted arguments keep their value")
        .def("move",
             [](PyLayer& l, scene::NodeId node, nb::handle parent, int index) {
                 scene::NodeId new_parent =
                     parent.is_none() ? scene::kNoNode : nb::cast<scene::NodeId>(parent);
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
             "Enable the layer mirror across an axis; items added with mirror=True reflect")
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
        });

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
             "Inclusive cell bounds of the painted region, or None");

    nb::class_<PyVoxelGrid>(m, "VoxelGrid",
                            "Palette-indexed colored voxel grid (chunked, sparse)")
        .def("__init__",
             [](PyVoxelGrid* self, float voxel_size) {
                 if (voxel_size <= 0.0f) throw std::invalid_argument("voxel_size must be > 0");
                 new (self) PyVoxelGrid();
                 self->owned = std::make_shared<voxel::VoxelGrid>(voxel_size);
             },
             "voxel_size"_a = 0.1f)
        .def_prop_ro("voxel_size", [](const PyVoxelGrid& g) { return g.grid().voxel_size(); })
        .def_prop_ro("occupied_count",
                     [](const PyVoxelGrid& g) { return g.grid().occupied_count(); })
        .def_prop_ro("palette_size",
                     [](const PyVoxelGrid& g) { return g.grid().palette_size(); })
        .def("palette_add",
             [](PyVoxelGrid& g, nb::handle color) {
                 return g.grid().palette_add(parse_color(color));
             },
             "color"_a, "Add (or find) a palette entry; returns its index")
        .def("palette_color",
             [](const PyVoxelGrid& g, std::uint8_t index) {
                 kernel::cfloat3 c = g.grid().palette_color(index);
                 return nb::make_tuple(c.x, c.y, c.z);
             },
             "index"_a)
        .def("palette_set",
             [](PyVoxelGrid& g, std::uint8_t index, nb::handle color) {
                 g.grid().palette_set(index, parse_color(color));
             },
             "index"_a, "color"_a, "Recolor a palette entry; voxel data is untouched")
        .def("get", [](const PyVoxelGrid& g,
                       nb::handle cell) { return g.grid().get(to_coord(cell)); }, "cell"_a)
        .def("set",
             [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index) {
                 g.grid().set(to_coord(cell), index);
             },
             "cell"_a, "index"_a)
        .def("set_many",
             [](PyVoxelGrid& g, nb::handle cells, std::uint8_t index) {
                 std::vector<voxel::VoxelCoord> coords = to_coords(cells);
                 voxel::VoxelGrid& grid = g.grid();
                 nb::gil_scoped_release release;
                 for (const voxel::VoxelCoord& c : coords) grid.set(c, index);
             },
             "cells"_a, "index"_a, "Set every cell of an (N, 3) int32 array in one call")
        .def("erase", [](PyVoxelGrid& g,
                         nb::handle cell) { g.grid().erase(to_coord(cell)); }, "cell"_a)
        .def("erase_many",
             [](PyVoxelGrid& g, nb::handle cells) {
                 std::vector<voxel::VoxelCoord> coords = to_coords(cells);
                 voxel::VoxelGrid& grid = g.grid();
                 nb::gil_scoped_release release;
                 for (const voxel::VoxelCoord& c : coords) grid.erase(c);
             },
             "cells"_a)
        .def("paint",
             [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index) {
                 g.grid().paint(to_coord(cell), index);
             },
             "cell"_a, "index"_a, "Recolor an occupied cell (no-op on empty cells)")
        .def("set_brush",
             [](PyVoxelGrid& g, nb::handle cell, int n, std::uint8_t index,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed, nb::handle mask) {
                 g.grid().set_brush(to_coord(cell),
                                    make_brush(n, shape, falloff, strength, seed, mask), index);
             },
             "cell"_a, "size"_a, "index"_a, "shape"_a = "cube", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Brush footprint centered on the cell: 'cube' or 'sphere', size n "
             "covering n cells per axis. falloff ('constant', 'linear', 'smooth', "
             "'gaussian') and strength soften the edge by dithering coverage "
             "deterministically against seed.")
        .def("erase_brush",
             [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                 g.grid().erase_brush(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed, mask));
             },
             "cell"_a, "size"_a, "shape"_a = "cube", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none())
        .def("paint_brush",
             [](PyVoxelGrid& g, nb::handle cell, int n, std::uint8_t index,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed, nb::handle mask) {
                 g.grid().paint_brush(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed, mask), index);
             },
             "cell"_a, "size"_a, "index"_a, "shape"_a = "cube", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Recolor occupied cells in the footprint; empty cells are untouched")
        .def("sculpt_smooth",
             [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                 g.grid().sculpt_smooth(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask));
             },
             "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Majority filter over the 26-neighbourhood: spurs dissolve, notches fill")
        .def("sculpt_inflate",
             [](PyVoxelGrid& g, nb::handle cell, int n, int amount, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                 g.grid().sculpt_inflate(to_coord(cell),
                                         make_brush(n, shape, falloff, strength, seed, mask), amount);
             },
             "cell"_a, "size"_a, "amount"_a = 1, "shape"_a = "sphere",
             "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Dilate for a positive amount, erode for a negative one")
        .def("sculpt_flatten",
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
        .def("sculpt_grab",
             [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle displacement,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed, bool front_only,
                nb::handle mask) {
                 g.grid().sculpt_grab(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed, mask),
                                      to_f3(displacement, "displacement"), front_only);
             },
             "cell"_a, "size"_a, "displacement"_a, "shape"_a = "sphere",
             "falloff"_a = "smooth", "strength"_a = 1.0f, "seed"_a = 0u,
             "front_only"_a = false, "mask"_a = nb::none(),
             "Translate occupancy through the same map the SDF grab uses. Binary "
             "occupancy means this resamples nearest-cell, so material moves in "
             "whole cells rather than flowing.")
        .def("sculpt_pinch",
             [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed, nb::handle mask) {
                 g.grid().sculpt_pinch(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed, mask));
             },
             "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Move surface cells one step toward the brush centre")
        .def("sculpt_fill_cavities",
             [](PyVoxelGrid& g, nb::handle cell, int n, int passes, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed,
                nb::handle mask) {
                 g.grid().sculpt_fill_cavities(to_coord(cell),
                                               make_brush(n, shape, falloff, strength, seed, mask),
                                               passes);
             },
             "cell"_a, "size"_a, "passes"_a = 1, "shape"_a = "sphere", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Fill pockets: an empty cell with at least four of its six face "
             "neighbours occupied is inside a cavity rather than beside a surface. "
             "A through-hole, an open face and a wide shallow dent are left alone — "
             "smoothing is the verb for surface irregularity.")
        .def("sculpt_scrape",
             [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle normal, float offset,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed, nb::handle mask) {
                 g.grid().sculpt_scrape(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask),
                                        to_f3(normal, "normal"), offset);
             },
             "cell"_a, "size"_a, "normal"_a, "offset"_a = 0.0f, "shape"_a = "sphere",
             "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u,
             "mask"_a = nb::none(),
             "Flatten onto the plane AND smooth, from ONE snapshot. Calling the two "
             "verbs in sequence is not the same thing: the flatten's output would "
             "feed the smooth's neighbourhood.")
        .def("sculpt_smudge",
             [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle displacement,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed, nb::handle mask) {
                 g.grid().sculpt_smudge(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed, mask),
                                        to_f3(displacement, "displacement"));
             },
             "cell"_a, "size"_a, "displacement"_a, "shape"_a = "sphere",
             "falloff"_a = "smooth", "strength"_a = 1.0f, "seed"_a = 0u, "mask"_a = nb::none(),
             "Drag SURFACE material along a direction, leaving the interior where it "
             "was. That is the difference from grab, which translates every cell in "
             "its region: grab moves a lump, smudge smears a skin.")
        .def("sculpt_carve_alpha",
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
             "cell"_a, "size"_a, "alpha"_a, "direction"_a, "index"_a = 0,
             "shape"_a = "sphere", "falloff"_a = "constant", "strength"_a = 1.0f,
             "seed"_a = 0u, "mask"_a = nb::none(),
             "Carve modulated by an (H, W) alpha, projected onto the plane "
             "perpendicular to `direction`. index 0 carves; a non-zero one deposits. "
             "The engine decodes no images — a host that has an alpha has already "
             "loaded the PNG.")
        .def("repair_report",
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
        .def("repair_close_holes",
             [](PyVoxelGrid& g, int passes, nb::handle mask) {
                 g.grid().repair_close_holes(passes, borrow_mask(mask));
             },
             "passes"_a = 1, "mask"_a = nb::none(),
             "Seal perforations over the whole grid by the pocket rule. Only ever "
             "adds cells, so no material is lost.")
        .def("repair_fill_voids",
             [](PyVoxelGrid& g, nb::handle mask) {
                 g.grid().repair_fill_voids(borrow_mask(mask));
             },
             "mask"_a = nb::none(),
             "Fill every empty cell the outside cannot reach, coloured from the shell "
             "that encloses it. Enclosure is decided by a flood from outside the "
             "bounds, not guessed at from a local neighbourhood.")
        .def("apply_stroke",
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
        .def("fill_box",
             [](PyVoxelGrid& g, nb::handle a, nb::handle b, std::uint8_t index) {
                 g.grid().fill_box(to_coord(a), to_coord(b), index);
             },
             "a"_a, "b"_a, "index"_a, "Inclusive-corner box fill")
        .def("fill_line",
             [](PyVoxelGrid& g, nb::handle a, nb::handle b, std::uint8_t index) {
                 g.grid().fill_line(to_coord(a), to_coord(b), index);
             },
             "a"_a, "b"_a, "index"_a)
        .def("set_mirrored",
             [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index, const std::string& axes) {
                 std::uint8_t mask = 0;
                 for (char c : axes) mask |= static_cast<std::uint8_t>(1u << parse_axis({c}));
                 g.grid().set_mirrored(to_coord(cell), index, mask);
             },
             "cell"_a, "index"_a, "axes"_a = "x",
             "Set the cell and every mirror combination of the given axes ('x', 'xz', ...)")
        .def("paint_mirrored",
             [](PyVoxelGrid& g, nb::handle cell, std::uint8_t index, const std::string& axes) {
                 std::uint8_t mask = 0;
                 for (char c : axes) mask |= static_cast<std::uint8_t>(1u << parse_axis({c}));
                 g.grid().paint_mirrored(to_coord(cell), index, mask);
             },
             "cell"_a, "index"_a, "axes"_a = "x",
             "Recolor the cell and every mirror combination (occupied cells only)")
        .def("flood_select",
             [](const PyVoxelGrid& g, nb::handle seed, bool same_color) {
                 std::vector<voxel::VoxelCoord> sel =
                     g.grid().flood_select(to_coord(seed), same_color);
                 nb::module_ np = nb::module_::import_("numpy");
                 nb::object arr = np.attr("empty")(nb::make_tuple(sel.size(), 3),
                                                   "dtype"_a = "int32");
                 auto view = nb::cast<nb::ndarray<std::int32_t, nb::ndim<2>, nb::c_contig>>(arr);
                 for (std::size_t i = 0; i < sel.size(); ++i) {
                     view.data()[i * 3 + 0] = sel[i].x;
                     view.data()[i * 3 + 1] = sel[i].y;
                     view.data()[i * 3 + 2] = sel[i].z;
                 }
                 return arr;
             },
             "seed"_a, "same_color"_a = true,
             "6-connected flood select -> (N, 3) int32 coordinates")
        .def("bounds",
             [](const PyVoxelGrid& g) -> nb::object {
                 auto lo = g.grid().bounds_min();
                 auto hi = g.grid().bounds_max();
                 if (!lo || !hi) return nb::none();
                 return nb::make_tuple(nb::make_tuple(lo->x, lo->y, lo->z),
                                       nb::make_tuple(hi->x, hi->y, hi->z));
             },
             "Inclusive cell bounds of occupied voxels, or None when empty")
        .def("mesh",
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
        .def("sample_step_field",
             [](const PyVoxelGrid& g, nb::handle points) {
                 PointsView pts = to_points(points);
                 const voxel::VoxelGrid& grid = g.grid();
                 float* out = new float[pts.count ? pts.count : 1];
                 nb::capsule owner(out,
                                   [](void* p) noexcept { delete[] static_cast<float*>(p); });
                 {
                     nb::gil_scoped_release release;
                     for (std::size_t i = 0; i < pts.count; ++i)
                         out[i] = grid.sample_step_field(kernel::cf3(pts.data[i * 3],
                                                                     pts.data[i * 3 + 1],
                                                                     pts.data[i * 3 + 2]));
                 }
                 return nb::cast(nb::ndarray<nb::numpy, float>(out, {pts.count}, owner));
             },
             "points"_a,
             "Voxels as a step field for SDF compositing (a bound, not a distance)")
        .def("raycast",
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
        .def("build_plane_pick",
             [](const PyVoxelGrid& g, nb::handle origin, nb::handle direction,
                std::int32_t plane_cell) -> nb::object {
                 math::Ray ray{to_f3(origin, "origin"),
                               kernel::cnormalize(to_f3(direction, "direction"))};
                 auto cell = pick::pick_build_plane(g.grid(), ray, plane_cell);
                 if (!cell) return nb::none();
                 return nb::make_tuple(cell->x, cell->y, cell->z);
             },
             "origin"_a, "direction"_a, "plane_cell"_a = 0)
        .def("rasterize",
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
             "Rasterize an SDF document into voxels (colors sampled from the field)");

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
