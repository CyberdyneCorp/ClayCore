// Golden digests for every fixed-topology mesh verb (add-shared-brush-kernels).
//
// THE GATE THIS FILE IS: the deformation math is being lifted out of
// `src/mesh/sculpt.cpp` into representation-neutral kernels so that the
// adaptive and multiresolution sculptors can call it instead of copying it.
// The acceptance criterion for that extraction is that the fixed path does not
// move by a bit, and these hashes are what "not by a bit" is measured against.
// They were generated on unmodified `main`, before a line was moved.
//
// WHY A HASH AND NOT A TOLERANCE. The mistake an extraction like this makes is
// a re-associated accumulation — summing a weighted normal in a different
// order, or folding two multiplications into one. Float multiplication is not
// associative, so that moves the last bit and nothing else. Every tolerance in
// this tree admits it. A hash does not.
//
// WHAT THESE HASHES ARE NOT. They are not portable across libm
// implementations. `class_normal` reaches `acos` through `corner_angle` for
// every class in every region, and the Gaussian falloff reaches `exp`; neither
// is correctly rounded and glibc and Apple's libm disagree about the last bit
// of both. Every value the fixtures CHOOSE is exactly representable — a power
// of two or a sum of them, the lesson `test_voxel_mesh_fixture.cpp` recorded
// about FMA contraction — so nothing here is gratuitously sensitive, but that
// cannot reach a transcendental. If a non-x86-64-Linux preset reports
// different hashes, the answer is a per-platform table and NOT a tolerance:
// the question this asks is about one machine's before and after.
//
// SO THERE IS ONE TABLE PER PLATFORM, which is that answer taken up. Measured
// on this branch: macOS/AppleClang disagrees with the x86-64 Linux table on 72
// of 80 cases and MSVC on 63, with ZERO moved-count differences on either —
// the verbs reach the same vertices everywhere and only the last bits move.
// Regenerating the table on a different machine does not help and was tried:
// it relocates the failure, because a single table can only ever match one
// toolchain.
//
// It is not FMA contraction, which was the first guess. Rebuilding macOS with
// `-ffp-contract=off` recovers 7 of the 72, and adding `-fno-vectorize
// -fno-slp-vectorize` recovers none beyond that. The rest is the platform's
// libm, which no compiler flag reaches.
//
// A PLATFORM WITH NO TABLE prints one and skips the byte comparison rather
// than failing. The moved counts still gate everywhere — they are the half
// that says the verbs did the same work — and the printed lines are what a
// new platform's table is made of. That keeps an unlisted toolchain honest
// (it says what it is missing) without turning "we have not baselined this
// machine yet" into a red build.
//
// Regenerating: these are outputs, not designed values. Run with
// CLAY_PARITY_REGEN=1 to print the table, and re-baseline only in the same
// commit as a deliberate behaviour change — never to make a red test green.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "clay/mesh/sculpt.h"

using namespace clay;
using namespace clay::kernel;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;
using mesh::VertexDeltas;

namespace {

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void u64(std::uint64_t v) { bytes(&v, sizeof(v)); }
    // An empty vector's data() may be null, and hashing zero bytes from null is
    // undefined even though it reads nothing — UBSan gates CI on exactly that.
    template <typename T>
    void vec(const std::vector<T>& v) {
        u64(v.size());
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
};

// Positions, normals and colours are the payload. `indices` and `quads` are in
// the digest too, and that is not redundancy: the contract this whole feature
// rests on is that topology never changes, so the hash that pins the geometry
// should fail if a refactor ever writes an index buffer.
std::uint64_t digest(const Mesh& m) {
    Fnv f;
    f.vec(m.positions);
    f.vec(m.normals);
    f.vec(m.colors);
    f.vec(m.uvs);
    f.vec(m.indices);
    f.vec(m.quads);
    return f.h;
}

// -- fixtures -----------------------------------------------------------------
//
// Five surfaces, each chosen for a property the verbs read differently:
// a flat grid (the analytic case), a sphere (curvature, so draw and inflate
// diverge), a cube (hard edges, so the weld classes and polish's gate matter),
// a folded sheet (a crease across the region), and two close sheets (a
// geodesic walk must not cross the gap a euclidean ball does).
//
// Every chosen coordinate is a multiple of a power of two, so the fixture
// itself contributes no rounding the platform could disagree about.

// RIPPLED, not flat, and that is the fixture rather than a decoration. On a
// perfectly flat grid the Laplacian of every interior vertex is the vertex
// itself, so Smooth, Relax and Polish move nothing and Flatten onto the
// surface's own plane moves nothing either — five of the sixteen verbs would
// contribute a golden that pins a no-op. The heights come from an exact
// eighth-scale table, so the surface has something to remove and the fixture
// still contributes no rounding of its own.
float ripple_height(int x, int z) {
    static const float kWave[8] = {0.0f,     0.0625f,  0.125f,  0.0625f,
                                   0.0f,     -0.0625f, -0.125f, -0.0625f};
    return kWave[(x + z) & 7];
}

Mesh plane_grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), ripple_height(x, z),
                                      -half + step * static_cast<float>(z)));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// A cube-sphere rather than a UV sphere: normalizing a cube grid uses only
// multiply, add, divide and sqrt, all correctly rounded, where sin and cos are
// not. The surface is the same sphere and the fixture stops being a second
// source of platform disagreement.
Mesh cube_sphere(int n, float radius) {
    Mesh m;
    std::vector<std::uint32_t> face_start;
    // Six faces, each a grid over [-1,1]^2 lifted onto the unit cube then
    // normalized. Vertices along shared edges are duplicated and the weld
    // classes rejoin them, which is what an imported model looks like.
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        face_start.push_back(static_cast<std::uint32_t>(m.positions.size()));
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const cfloat3 p = cf3(c[0], c[1], c[2]);
                const float len = clength(p);
                const cfloat3 unit = p / len;
                m.positions.push_back(unit * radius);
                m.normals.push_back(unit);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        const std::uint32_t base = face_start.back();
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                // Winding follows the face sign so the sphere is not
                // half-inside-out, which would make every normal fixture read
                // its own mirror image.
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

// A cube with genuinely hard edges: the same six grids, unnormalized. Polish's
// gate exists for this surface and reads nothing interesting on a sphere.
Mesh cube_box(int n, float half) {
    Mesh m;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = (-1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n)) * half;
                c[axes[f][1]] = (-1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n)) * half;
                // Noise on the face INTERIOR only. A cube with flat faces makes
                // polish a no-op — its gate protects the edge, and a flat face
                // has nothing to remove — so the one verb this fixture exists
                // for would pin a zero. The border stays exactly on the cube so
                // that vertices shared between two faces remain
                // position-coincident and the weld classes still rejoin them;
                // displacing them along each face's own normal would split the
                // cube into six disconnected sheets.
                const bool interior = u > 0 && u < n && v > 0 && v < n;
                // A QUARTER of the ripple the plane carries. Full amplitude over this
                // face's step bends the surface by more than `polish_angle`
                // everywhere, so polish's gate closes across the whole region
                // and the verb this fixture exists for moves nothing — which is
                // correct behaviour and a useless golden. At a quarter the gate
                // is open on the face and still shut on the 90-degree edge,
                // which is the distinction being pinned.
                c[axes[f][2]] =
                    signs[f] * half + (interior ? signs[f] * ripple_height(u, v) * 0.25f : 0.0f);
                m.positions.push_back(cf3(c[0], c[1], c[2]));
                float nc[3] = {0.0f, 0.0f, 0.0f};
                nc[axes[f][2]] = signs[f];
                m.normals.push_back(cf3(nc[0], nc[1], nc[2]));
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

// A sheet folded along x = 0: two flat halves meeting at a crease. The slope is
// 1/2 so every lifted coordinate stays exact.
Mesh folded_sheet(int n, float half) {
    Mesh m = plane_grid(n, half);
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const float x = m.positions[i].x;
        // The fold, PLUS the ripple the grid already carried: a crease alone
        // leaves two flat halves, and the smoothing family reads nothing on a
        // flat half.
        const float y = (x >= 0.0f ? x : -x) * 0.5f + m.positions[i].y;
        m.positions[i] = cf3(x, y, m.positions[i].z);
        // Face normals of the two halves; the crease row takes the +x side,
        // which is what an exporter would have written.
        const float s = x >= 0.0f ? -0.5f : 0.5f;
        const cfloat3 nr = cf3(s, 1.0f, 0.0f);
        m.normals[i] = nr / clength(nr);
    }
    return m;
}

// Two parallel sheets a thirty-second apart, as the two sides of a thin fin or
// the two lips of a closed mouth. A euclidean ball spanning the gap reaches
// both; the geodesic walk reaches one. That difference is the whole reason
// `MeshBrushSettings::geodesic` exists, so it belongs in the goldens.
Mesh two_close_sheets(int n, float half) {
    Mesh top = plane_grid(n, half);
    Mesh m = top;
    const std::uint32_t offset = static_cast<std::uint32_t>(top.positions.size());
    for (std::size_t i = 0; i < top.positions.size(); ++i) {
        m.positions.push_back(top.positions[i] - cf3(0.0f, 0.03125f, 0.0f));
        m.normals.push_back(cf3(0, -1, 0));
    }
    for (std::size_t i = 0; i < top.indices.size(); i += 3) {
        // Reversed winding on the lower sheet, so the pair reads as a shell.
        m.indices.push_back(top.indices[i] + offset);
        m.indices.push_back(top.indices[i + 2] + offset);
        m.indices.push_back(top.indices[i + 1] + offset);
    }
    return m;
}

// Each fixture carries the path its stamps walk. A shared centre cannot work:
// the sphere and the cube enclose the origin, so a brush there reaches nothing
// at all and its golden pins an empty region.
struct Fixture {
    const char* name;
    Mesh (*build)();
    cfloat3 base_center;  // the first stamp
    cfloat3 step;         // added per stamp, twice
};

Mesh fx_plane() { return plane_grid(8, 1.0f); }
Mesh fx_sphere() { return cube_sphere(4, 1.0f); }
Mesh fx_cube() { return cube_box(8, 1.0f); }
Mesh fx_folded() { return folded_sheet(8, 1.0f); }
Mesh fx_sheets() { return two_close_sheets(6, 1.0f); }

const Fixture kFixtures[] = {
    // Across the ripple.
    {"plane", fx_plane, cf3(-0.25f, 0.0f, 0.0f), cf3(0.25f, 0.0f, 0.0f)},
    // On the +z cap, walking in x.
    {"sphere", fx_sphere, cf3(-0.25f, 0.0f, 1.0f), cf3(0.25f, 0.0f, 0.0f)},
    // Straddling the +y/+z EDGE, which is the surface polish's gate exists for
    // and the one a sphere cannot present.
    {"cube", fx_cube, cf3(-0.25f, 0.75f, 1.0f), cf3(0.25f, 0.0f, 0.0f)},
    // Across the crease.
    {"folded", fx_folded, cf3(-0.25f, 0.125f, 0.0f), cf3(0.25f, 0.0f, 0.0f)},
    // On the upper sheet, close enough that a euclidean ball spans the gap and
    // a geodesic walk does not.
    {"sheets", fx_sheets, cf3(-0.25f, 0.0f, 0.0f), cf3(0.25f, 0.0f, 0.0f)},
};

struct VerbCase {
    const char* name;
    MeshBrush verb;
};

const VerbCase kVerbs[] = {
    {"grab", MeshBrush::Grab},         {"draw", MeshBrush::Draw},
    {"inflate", MeshBrush::Inflate},   {"smooth", MeshBrush::Smooth},
    {"pinch", MeshBrush::Pinch},       {"flatten", MeshBrush::Flatten},
    {"clay", MeshBrush::Clay},         {"crease", MeshBrush::Crease},
    {"scrape", MeshBrush::Scrape},     {"polish", MeshBrush::Polish},
    {"snakehook", MeshBrush::Snakehook}, {"relax", MeshBrush::Relax},
    {"layer", MeshBrush::Layer},       {"nudge", MeshBrush::Nudge},
    {"paint", MeshBrush::Paint},       {"smear", MeshBrush::Smear},
};

// One settings block per verb, with every value exactly representable. The
// defaults are deliberately NOT taken wholesale: a verb whose sign, direction
// or mode is left at zero does nothing, and a golden over a no-op verb pins
// nothing.
MeshBrushSettings settings_for(MeshBrush verb) {
    MeshBrushSettings s;
    s.center = cf3(0.0f, 0.0f, 0.0f);
    s.radius = 0.5f;
    s.strength = 0.5f;
    s.falloff = mesh::MeshFalloff::Smooth;
    s.geodesic = mesh::default_geodesic(verb);
    // Grab, Snakehook, Nudge and Smear are the four that read it.
    s.direction = cf3(0.25f, 0.125f, 0.0f);
    s.smooth_iterations = 2;
    s.polish_angle = 0.25f;
    s.layer_height = 0.125f;
    s.color = cf3(0.25f, 0.5f, 0.75f);
    if (verb == MeshBrush::Flatten || verb == MeshBrush::Scrape)
        s.flatten_mode = field::FlattenMode::CutOnly;
    return s;
}

// A colour ramp with exactly-representable components, so Smear has something
// to drag and Paint has something to blend against that is not uniform.
void seed_colors(Mesh& m) {
    m.colors.resize(m.positions.size());
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const float t = static_cast<float>(i % 8) / 8.0f;
        m.colors[i] = cf3(t, 1.0f - t, 0.5f);
    }
}

// THREE STAMPS ALONG A PATH, not one. A single stamp cannot catch the
// accumulation defects this gate is for: Layer's ceiling only means something
// across stamps, Clay's clamp converges over them, and a re-anchoring
// Snakehook differs from a Grab only between them.
std::uint64_t run_case(const Fixture& fx, MeshBrush verb, std::uint32_t* moved_out) {
    Mesh m = fx.build();
    seed_colors(m);
    MeshSculptor sculptor(m);
    VertexDeltas record;
    MeshBrushSettings s = settings_for(verb);
    std::size_t moved = 0;
    for (int step = 0; step < 3; ++step) {
        s.center = fx.base_center + fx.step * static_cast<float>(step);
        moved += sculptor.stamp(verb, s, {}, &record);
    }
    *moved_out = static_cast<std::uint32_t>(moved);
    return digest(m);
}

struct Golden {
    const char* fixture;
    const char* verb;
    std::uint64_t hash;
    std::uint32_t moved;
};

// GENERATED ON MAIN at 0bb4c81, before the kernels were extracted — one table
// per toolchain, because a hash of float bits is a property of the machine
// that produced it. `kHaveGoldens` is what tells the case below whether this
// platform has been baselined at all.
// TWO TABLES, because the two halves of a case travel differently.
//
// The MOVED COUNTS are portable and measured to be: macOS and MSVC each agree
// with the x86-64 Linux table on all 80, while disagreeing on 72 and 63 hashes
// respectively. So the reference table gates the moved counts on EVERY
// toolchain, baselined or not — a verb that stopped reaching a vertex is
// caught everywhere.
//
// The HASHES are a property of the machine that produced them, so they are
// compared against this toolchain's own table, and a toolchain with no table
// prints one instead of failing.
const Golden kReference[] = {
#include "mesh_sculpt_goldens_linux_x64.inc"
};
constexpr std::size_t kReferenceCount = sizeof(kReference) / sizeof(kReference[0]);

#if defined(__linux__) && defined(__x86_64__)
#define CLAY_PARITY_TABLE "x86-64 Linux"
const Golden kGoldens[] = {
#include "mesh_sculpt_goldens_linux_x64.inc"
};
#elif defined(__APPLE__) && defined(__aarch64__)
#define CLAY_PARITY_TABLE "arm64 macOS"
const Golden kGoldens[] = {
#include "mesh_sculpt_goldens_macos_arm64.inc"
};
#else
#define CLAY_PARITY_TABLE "none: this toolchain has no hash table yet"
#define CLAY_PARITY_NO_TABLE 1
// One placeholder, never read — `kHaveGoldens` gates every use. A zero-length
// array is ill-formed, and sizeof on one is not a way to ask this question.
const Golden kGoldens[] = {{"", "", 0ull, 0u}};
#endif

#if defined(CLAY_PARITY_NO_TABLE)
constexpr bool kHaveGoldens = false;
#else
constexpr bool kHaveGoldens = true;
#endif

}  // namespace

TEST_CASE("mesh sculpt parity: every verb on every fixture is byte-identical") {
    // A platform with no table PRINTS one and skips the byte comparison. The
    // moved counts below still run, so the case keeps saying whether the verbs
    // did the same work; what it stops claiming is byte-identity against a
    // machine that is not this one, which it was never able to claim.
    const bool regen = std::getenv("CLAY_PARITY_REGEN") != nullptr;
    // An unbaselined toolchain PRINTS its table without being asked. The
    // alternative -- telling a reader to re-run with an env var set -- costs a
    // CI round trip on exactly the machine nobody has local access to, which
    // is the machine that needs a table. Printing is what makes this
    // self-service: the lines below are the file.
    const bool print_table = regen || !kHaveGoldens;
    if (!kHaveGoldens) {
        MESSAGE("no hash table for this toolchain: the byte comparison is "
                "skipped and the moved counts are still checked against the "
                "reference. The lines printed below ARE a table for it -- copy "
                "them into mesh_sculpt_goldens_<toolchain>.inc and add the arm "
                "to the #if above. See this file's header for why a table is "
                "per-platform.");
    }
    std::size_t index = 0;
    for (const Fixture& fx : kFixtures) {
        for (const VerbCase& vc : kVerbs) {
            std::uint32_t moved = 0;
            const std::uint64_t h = run_case(fx, vc.verb, &moved);
            if (print_table) {
                std::printf("    {\"%s\", \"%s\", %lluull, %uu},\n", fx.name, vc.name,
                            static_cast<unsigned long long>(h), moved);
            }
            // A DELIBERATE re-baseline prints and checks nothing. An
            // unbaselined toolchain prints AND still gates its moved counts,
            // which is the difference between the two: one is being rewritten
            // on purpose, the other is simply not listed yet.
            if (regen) {
                ++index;
                continue;
            }
            REQUIRE(index < kReferenceCount);
            const Golden& ref = kReference[index];
            CAPTURE(fx.name);
            CAPTURE(vc.name);
            REQUIRE(std::string(ref.fixture) == fx.name);
            REQUIRE(std::string(ref.verb) == vc.name);
            // The moved count first: when both fail it is the readable half,
            // and a verb that stopped reaching anything is a different defect
            // from one whose arithmetic drifted. Against the REFERENCE table,
            // on every toolchain — this half is portable and measured to be.
            CHECK(moved == ref.moved);
            // The hash is the point of this file: a re-associated accumulation
            // moves exactly one bit, and every tolerance in this tree admits
            // that. Against THIS toolchain's table, and only where there is
            // one; see the header.
            if (kHaveGoldens) {
                const Golden& g = kGoldens[index];
                REQUIRE(std::string(g.fixture) == fx.name);
                REQUIRE(std::string(g.verb) == vc.name);
                CHECK(h == g.hash);
            }
            ++index;
        }
    }
    if (!regen) CHECK(index == kReferenceCount);
    if (kHaveGoldens) MESSAGE("golden table: " << CLAY_PARITY_TABLE);
}

// The verbs that must do SOMETHING on the fixtures, so a golden table full of
// zero-move entries cannot pass as parity. This is the discriminating half:
// the hashes prove nothing changed, this proves there was something to change.
TEST_CASE("mesh sculpt parity: the fixtures are discriminating") {
    for (const Fixture& fx : kFixtures) {
        for (const VerbCase& vc : kVerbs) {
            std::uint32_t moved = 0;
            run_case(fx, vc.verb, &moved);
            CAPTURE(fx.name);
            CAPTURE(vc.name);
            CHECK(moved > 0);
        }
    }
}
