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
#include "clay/mesh/surface_nets.h"
#include "clay/mesh/validate.h"
#include "clay/pick/pick.h"
#include "clay/scene/bounds.h"
#include "clay/scene/commands.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
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

voxel::BrushParams make_brush(int size, const std::string& shape, const std::string& falloff,
                              float strength, std::uint32_t seed) {
    if (size <= 0) throw std::invalid_argument("brush size must be > 0");
    voxel::BrushParams p;
    p.size = size;
    p.shape = parse_brush_shape(shape);
    p.falloff = parse_falloff(falloff);
    p.strength = strength;
    p.seed = seed;
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
    std::vector<scene::Deformer> deformers;
    scene::Profile profile;
    std::vector<kernel::cfloat2> profile_points;
    scene::Repeat repeat;
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

// -- mesh wrapper ---------------------------------------------------------------

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
#undef CLAY_PLACE_ARGS

    nb::class_<PyStroke, PyPrim>(
        m, "Stroke",
        "Sculpt stroke: a polyline chain of sphere-swept cones with per-point "
        "radius. One stroke is ONE edit item, not one per segment.")
        .def("__init__",
             [](PyStroke* self, nb::handle points, float blend_k, nb::handle position,
                nb::handle rotation_axis_angle, float scale) {
                 new (self) PyStroke();
                 self->prim = scene::Prim::stroke();
                 if (!points.is_none()) self->stroke = to_stroke_points(points);
                 if (blend_k < 0.0f) throw std::invalid_argument("blend_k must be >= 0");
                 self->stroke_blend_k = blend_k;
                 place(*self, position, rotation_axis_angle, scale);
             },
             "points"_a = nb::none(), "blend_k"_a = 0.0f, "position"_a = nb::none(),
             "rotation_axis_angle"_a = nb::none(), "scale"_a = 1.0f)
        .def("add_point",
             [](PyStroke& self, nb::handle position, float radius) {
                 if (radius < 0.0f) throw std::invalid_argument("radius must be >= 0");
                 self.stroke.push_back(
                     scene::StrokePoint{to_f3(position, "stroke point"), radius});
                 return &self;
             },
             "position"_a, "radius"_a, nb::rv_policy::reference_internal,
             "Append one point (chainable) — the incremental authoring path")
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

    nb::class_<PyMesh>(m, "Mesh", "Triangle mesh with numpy buffer views")
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
                float rounding, bool mirror, nb::handle transition) {
                 if (op == scene::Op::None)
                     throw std::invalid_argument(
                         "op must be a combine operator, not Op.NONE");
                 scene::Node n;
                 n.prim = prim.prim;
                 n.xform = prim.xform;
                 n.stroke = prim.stroke;
                 n.stroke_blend_k = prim.stroke_blend_k;
                 n.deformers = prim.deformers;
                 n.profile = prim.profile;
                 n.profile_points = prim.profile_points;
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
                 if (rounding < 0.0f) throw std::invalid_argument("rounding must be >= 0");
                 n.rounding = rounding;
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
             "rounding"_a = 0.0f, "mirror"_a = false, "transition"_a = nb::none(),
             "Append an edit to the layer; returns the node id")
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
                 scene::Tape tape = scene::compile_document(d.doc->document);
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
                 scene::Tape tape = scene::compile_document(d.doc->document);
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
                std::uint32_t seed) {
                 g.grid().set_brush(to_coord(cell),
                                    make_brush(n, shape, falloff, strength, seed), index);
             },
             "cell"_a, "size"_a, "index"_a, "shape"_a = "cube", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u,
             "Brush footprint centered on the cell: 'cube' or 'sphere', size n "
             "covering n cells per axis. falloff ('constant', 'linear', 'smooth', "
             "'gaussian') and strength soften the edge by dithering coverage "
             "deterministically against seed.")
        .def("erase_brush",
             [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed) {
                 g.grid().erase_brush(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed));
             },
             "cell"_a, "size"_a, "shape"_a = "cube", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u)
        .def("paint_brush",
             [](PyVoxelGrid& g, nb::handle cell, int n, std::uint8_t index,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed) {
                 g.grid().paint_brush(to_coord(cell),
                                      make_brush(n, shape, falloff, strength, seed), index);
             },
             "cell"_a, "size"_a, "index"_a, "shape"_a = "cube", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u,
             "Recolor occupied cells in the footprint; empty cells are untouched")
        .def("sculpt_smooth",
             [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed) {
                 g.grid().sculpt_smooth(to_coord(cell),
                                        make_brush(n, shape, falloff, strength, seed));
             },
             "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u,
             "Majority filter over the 26-neighbourhood: spurs dissolve, notches fill")
        .def("sculpt_inflate",
             [](PyVoxelGrid& g, nb::handle cell, int n, int amount, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed) {
                 g.grid().sculpt_inflate(to_coord(cell),
                                         make_brush(n, shape, falloff, strength, seed), amount);
             },
             "cell"_a, "size"_a, "amount"_a = 1, "shape"_a = "sphere",
             "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u,
             "Dilate for a positive amount, erode for a negative one")
        .def("sculpt_flatten",
             [](PyVoxelGrid& g, nb::handle cell, int n, nb::handle normal, float offset,
                const std::string& shape, const std::string& falloff, float strength,
                std::uint32_t seed) {
                 g.grid().sculpt_flatten(to_coord(cell),
                                         make_brush(n, shape, falloff, strength, seed),
                                         to_f3(normal, "normal"), offset);
             },
             "cell"_a, "size"_a, "normal"_a, "offset"_a = 0.0f, "shape"_a = "sphere",
             "falloff"_a = "constant", "strength"_a = 1.0f, "seed"_a = 0u,
             "Pull the surface onto the plane through the brush centre")
        .def("sculpt_pinch",
             [](PyVoxelGrid& g, nb::handle cell, int n, const std::string& shape,
                const std::string& falloff, float strength, std::uint32_t seed) {
                 g.grid().sculpt_pinch(to_coord(cell),
                                       make_brush(n, shape, falloff, strength, seed));
             },
             "cell"_a, "size"_a, "shape"_a = "sphere", "falloff"_a = "constant",
             "strength"_a = 1.0f, "seed"_a = 0u,
             "Move surface cells one step toward the brush centre")
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
