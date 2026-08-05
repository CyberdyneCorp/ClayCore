// clay-cli — CI's workhorse and a user-facing converter (build-packaging
// spec): mesh / validate / eval / convert / sample / parity-fixture /
// selftest, all through public library APIs only.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/io/parity_fixture.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/validate.h"
#include "clay/scene/tape.h"

using namespace clay;

namespace {

int usage() {
    std::fprintf(stderr,
                 "clay-cli — claycore command line\n"
                 "  clay sample <out.clayspace>                     write a demo document\n"
                 "  clay mesh <in.clayspace> [--res N] [--voxel S]\n"
                 "            [--decimate R] -o <out.{obj,ply,fbx,glb}>  mesh a document\n"
                 "  clay validate <mesh.{obj,ply,fbx}>              watertight/manifold gate\n"
                 "  clay eval <in.clayspace> --points <pts.npy>\n"
                 "            -o <dists.npy>                        batch field evaluation\n"
                 "  clay convert <in.{obj,ply,fbx}> -o <out.{obj,ply,fbx,glb}>\n"
                 "  clay parity-fixture -o <fixture.json>           tapes + probes + reference\n"
                 "                                                  values for a host GPU preview\n"
                 "  clay selftest                                   end-to-end pipeline check\n");
    return 2;
}

std::string ext_of(const std::string& path) {
    std::size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? "" : path.substr(dot + 1);
}

io::IoStatus save_mesh_any(const mesh::Mesh& m, const std::string& path) {
    std::string ext = ext_of(path);
    if (ext == "obj") return io::save_obj_file(m, path);
    if (ext == "ply") return io::save_ply_file(m, path);
    if (ext == "fbx") return io::save_fbx_file(m, path);
    if (ext == "glb") return io::save_glb_file(m, path);
    return io::IoStatus::fail(io::IoError::Unsupported, "unknown output extension: " + ext);
}

io::IoStatus load_mesh_any(const std::string& path, mesh::Mesh* out) {
    std::string ext = ext_of(path);
    if (ext == "obj") return io::load_obj_file(path, out);
    if (ext == "ply") return io::load_ply_file(path, out);
    if (ext == "fbx") return io::load_fbx_file(path, out);
    return io::IoStatus::fail(io::IoError::Unsupported, "unknown input extension: " + ext);
}

int fail(const io::IoStatus& s) {
    std::fprintf(stderr, "error: %s\n", s.detail.c_str());
    return 1;
}

// -- minimal .npy float32 support (eval spec: clay eval --points pts.npy) ----

bool save_npy_f32(const std::string& path, const float* data, std::size_t rows,
                  std::size_t cols) {
    std::string shape = cols == 1 ? "(" + std::to_string(rows) + ",)"
                                  : "(" + std::to_string(rows) + ", " + std::to_string(cols) + ")";
    std::string header =
        "{'descr': '<f4', 'fortran_order': False, 'shape': " + shape + ", }";
    std::size_t total = 10 + header.size() + 1;
    std::size_t pad = (64 - total % 64) % 64;
    header += std::string(pad, ' ');
    header += "\n";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const unsigned char magic[8] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    std::fwrite(magic, 1, 8, f);
    std::uint16_t hlen = static_cast<std::uint16_t>(header.size());
    std::fwrite(&hlen, 2, 1, f);
    std::fwrite(header.data(), 1, header.size(), f);
    std::fwrite(data, 4, rows * cols, f);
    std::fclose(f);
    return true;
}

bool load_npy_f32(const std::string& path, std::vector<float>* out, std::size_t* rows,
                  std::size_t* cols) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        std::fclose(f);
        return false;
    }
    std::uint16_t hlen = 0;
    if (std::fread(&hlen, 2, 1, f) != 1) {
        std::fclose(f);
        return false;
    }
    std::string header(hlen, '\0');
    if (std::fread(header.data(), 1, hlen, f) != hlen ||
        header.find("'<f4'") == std::string::npos) {
        std::fclose(f);
        return false;
    }
    std::size_t sh = header.find("'shape':");
    if (sh == std::string::npos) {
        std::fclose(f);
        return false;
    }
    unsigned long r = 0, c = 1;
    if (std::sscanf(header.c_str() + sh, "'shape': (%lu, %lu", &r, &c) < 1) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<std::size_t>(r) * c);
    bool ok = std::fread(out->data(), 4, out->size(), f) == out->size();
    std::fclose(f);
    *rows = r;
    *cols = c;
    return ok;
}

// -- commands ----------------------------------------------------------------

io::ClaySpaceDoc make_sample() {
    io::ClaySpaceDoc cs;
    scene::Layer& l = cs.document.add_sdf_layer("body");
    l.mirror_axes = scene::kMirrorX;
    l.mirror_k = 0.05f;
    scene::Node sphere;
    sphere.prim = scene::Prim::sphere(0.8f);
    sphere.color = kernel::cf3(0.85f, 0.4f, 0.3f);
    l.sdf->insert(sphere);
    scene::Node arm;
    arm.prim = scene::Prim::round_cone(0.28f, 0.12f, 0.7f);
    arm.xform.position = kernel::cf3(0.7f, 0.2f, 0);
    arm.xform.rotation = math::Quat::from_axis_angle(kernel::cf3(0, 0, 1), -1.2f);
    arm.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.12f};
    arm.color = kernel::cf3(0.9f, 0.7f, 0.3f);
    arm.mirror = true;
    l.sdf->insert(arm);
    scene::Node cut;
    cut.prim = scene::Prim::capped_cylinder(0.25f, 1.5f);
    cut.xform.position = kernel::cf3(0, 0.55f, 0.55f);
    cut.xform.rotation = math::Quat::from_axis_angle(kernel::cf3(1, 0, 0), 1.5707963f);
    cut.op = scene::Op::Subtract;
    cut.blend = scene::Blend{scene::BlendProfile::Cubic, 0.06f};
    l.sdf->insert(cut);
    return cs;
}

int cmd_mesh(const std::string& in, float voxel, int res, float decimate_ratio,
             const std::string& out_path) {
    io::ClaySpaceDoc cs;
    io::IoStatus s = io::load_clayspace_file(in, &cs);
    if (!s.ok()) return fail(s);
    scene::Tape tape = scene::compile_document(cs.document);
    if (tape.empty() || tape.bounds.empty() || tape.bounds.is_infinite())
        return fail(io::IoStatus::fail(io::IoError::Malformed, "document has no bounded SDF"));
    if (voxel <= 0) {
        kernel::cfloat3 ext = tape.bounds.extent();
        voxel = kernel::cmax(ext.x, kernel::cmax(ext.y, ext.z)) / static_cast<float>(res);
    }
    mesh::Mesh m = mesh::mesh_tape(tape, tape.bounds, voxel);
    if (decimate_ratio > 0) {
        mesh::DecimateOptions opts;
        opts.target_ratio = decimate_ratio;
        m = mesh::decimate(m, opts);
    }
    mesh::ValidationReport r = mesh::validate(m);
    std::printf("meshed %zu triangles (watertight=%d manifold=%d)\n", m.triangle_count(),
                r.watertight ? 1 : 0, r.manifold ? 1 : 0);
    s = save_mesh_any(m, out_path);
    if (!s.ok()) return fail(s);
    return r.watertight && r.manifold ? 0 : 1;
}

int cmd_validate(const std::string& path) {
    mesh::Mesh m;
    io::IoStatus s = load_mesh_any(path, &m);
    if (!s.ok()) return fail(s);
    mesh::ValidationReport r = mesh::validate(m, 20000);
    std::printf(
        "%s: %zu verts, %zu tris | watertight=%d manifold=%d oriented=%d degenerate=%zu "
        "boundary_edges=%zu intersecting=%zu euler=%lld\n",
        path.c_str(), r.vertices, r.triangles, r.watertight ? 1 : 0, r.manifold ? 1 : 0,
        r.oriented ? 1 : 0, r.degenerate_triangles, r.boundary_edges, r.intersecting_pairs,
        r.euler_characteristic);
    return r.watertight && r.manifold ? 0 : 1;
}

int cmd_eval(const std::string& in, const std::string& pts_path, const std::string& out_path) {
    io::ClaySpaceDoc cs;
    io::IoStatus s = io::load_clayspace_file(in, &cs);
    if (!s.ok()) return fail(s);
    std::vector<float> pts;
    std::size_t rows = 0, cols = 0;
    if (!load_npy_f32(pts_path, &pts, &rows, &cols) || cols != 3)
        return fail(io::IoStatus::fail(io::IoError::Malformed,
                                       "points file must be a float32 (N,3) .npy"));
    scene::Tape tape = scene::compile_document(cs.document);
    std::vector<float> dists(rows);
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    eval::PointQuery q{pts.data(), rows, 1e-4f};
    eval::PointResults out{dists.data(), nullptr, nullptr};
    if (cpu->eval_points(tape, q, out) != eval::Status::Ok)
        return fail(io::IoStatus::fail(io::IoError::Unsupported, "evaluation failed"));
    if (!save_npy_f32(out_path, dists.data(), rows, 1))
        return fail(io::IoStatus::fail(io::IoError::WriteFailed, out_path));
    std::printf("evaluated %zu points -> %s\n", rows, out_path.c_str());
    return 0;
}

int cmd_convert(const std::string& in, const std::string& out) {
    mesh::Mesh m;
    io::IoStatus s = load_mesh_any(in, &m);
    if (!s.ok()) return fail(s);
    s = save_mesh_any(m, out);
    if (!s.ok()) return fail(s);
    std::printf("converted %s -> %s (%zu triangles)\n", in.c_str(), out.c_str(),
                m.triangle_count());
    return 0;
}

// Export the host parity fixture (build-packaging spec). A host GPU preview
// evaluates these tapes with kernels compiled from clay/kernel/*.h and asserts
// agreement in its own CI — the gate that would have caught the 4k-support
// blend drift before it reached a bake.
int cmd_parity_fixture(const std::string& out) {
    io::IoStatus s = io::save_kernel_parity_fixture(out);
    if (!s.ok()) return fail(s);
    std::printf("wrote %s (%zu cases x %zu probe points)\n", out.c_str(),
                io::kernel_parity_cases().size(), io::kernel_parity_probe_points().size());
    return 0;
}

int cmd_selftest() {
    // end-to-end: sample -> save -> mesh -> validate -> eval -> convert
    io::ClaySpaceDoc cs = make_sample();
    if (!io::save_clayspace_file(cs, "clay_selftest.clayspace").ok()) return 1;
    if (cmd_mesh("clay_selftest.clayspace", 0, 96, 0.5f, "clay_selftest.obj") != 0) return 1;
    if (cmd_validate("clay_selftest.obj") != 0) return 1;
    std::vector<float> pts = {0, 0, 0, 3, 0, 0, 0.7f, 0.2f, 0};
    if (!save_npy_f32("clay_selftest_pts.npy", pts.data(), 3, 3)) return 1;
    if (cmd_eval("clay_selftest.clayspace", "clay_selftest_pts.npy",
                 "clay_selftest_dists.npy") != 0)
        return 1;
    std::vector<float> dists;
    std::size_t rows, cols;
    if (!load_npy_f32("clay_selftest_dists.npy", &dists, &rows, &cols) || rows != 3) return 1;
    if (!(dists[0] < 0 && dists[1] > 0)) return 1;
    if (cmd_convert("clay_selftest.obj", "clay_selftest.ply") != 0) return 1;
    if (cmd_convert("clay_selftest.ply", "clay_selftest.fbx") != 0) return 1;
    if (cmd_convert("clay_selftest.obj", "clay_selftest.glb") != 0) return 1;
    if (cmd_parity_fixture("clay_selftest_fixture.json") != 0) return 1;
    std::printf("selftest: OK\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    std::string cmd = args[0];

    auto flag = [&](const std::string& name, const std::string& fallback) -> std::string {
        for (std::size_t i = 1; i + 1 < args.size() + 0; ++i)
            if (args[i] == name && i + 1 < args.size()) return args[i + 1];
        return fallback;
    };

    if (cmd == "sample" && args.size() >= 2)
        return io::save_clayspace_file(make_sample(), args[1]).ok() ? 0 : 1;
    if (cmd == "mesh" && args.size() >= 2) {
        std::string out = flag("-o", "");
        if (out.empty()) return usage();
        return cmd_mesh(args[1], std::stof(flag("--voxel", "0")),
                        std::stoi(flag("--res", "128")), std::stof(flag("--decimate", "0")),
                        out);
    }
    if (cmd == "validate" && args.size() >= 2) return cmd_validate(args[1]);
    if (cmd == "eval" && args.size() >= 2) {
        std::string pts = flag("--points", "");
        std::string out = flag("-o", "dists.npy");
        if (pts.empty()) return usage();
        return cmd_eval(args[1], pts, out);
    }
    if (cmd == "convert" && args.size() >= 2) {
        std::string out = flag("-o", "");
        if (out.empty()) return usage();
        return cmd_convert(args[1], out);
    }
    if (cmd == "parity-fixture") {
        std::string out = flag("-o", "");
        if (out.empty()) return usage();
        return cmd_parity_fixture(out);
    }
    if (cmd == "selftest") return cmd_selftest();
    return usage();
}
