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
// A TABLE ALSO RECORDS THE FLOATING-POINT CONFIGURATION it was baselined
// under, because the platform macros above do not identify one. Build this
// same machine with `-ffp-contract=off` and it selects the same arm and fails
// 72 of the 80 hash comparisons while failing NONE of the 80 moved counts —
// a configuration mismatch wearing the exact costume of arithmetic drift, and
// 72 `CHECK(h == g.hash)` lines that cannot tell the two apart (#581). So the
// build measures what it actually emits, the table says what it was baselined
// under, and when those disagree AND hashes differ the failure says THAT, once,
// instead of 72 times. When they disagree and every hash matches anyway it says
// so and passes: the record is a diagnosis and never a second gate, so a
// record that is wrong costs a message and cannot cost a red build.
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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
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

// -- what arithmetic this build actually emits --------------------------------
//
// The tables below are selected by PLATFORM macros, and a hash of float bits is
// not a property of the platform: it is a property of the arithmetic the
// compiler emitted, and one platform emits several depending on its flags.
//
// These are PROBES rather than macros, for two reasons. There is no portable
// macro to read — `-ffp-contract` defines nothing at all, and flushed
// subnormals are a hardware mode a host can set before it ever calls us. And
// what decides a hash is not what the command line asked for but what the back
// end could deliver: x86-64 without `-march` has no FMA instruction, so
// `-ffp-contract=fast` — the default on GCC and Clang both — contracts nothing
// there and DOES contract on aarch64, where FMA is baseline. That, and not a
// flag, is why the Linux and macOS tables record different configurations.
//
// The probes read THIS translation unit's flags. That is the library's
// configuration whenever the setting is build-wide — `CMAKE_CXX_FLAGS`, a
// preset, a toolchain file — and out of reach of a flag applied to `src/mesh`
// alone, which it always was.

// `volatile` is load-bearing: it is what makes the back end answer this
// question instead of the constant folder.
bool probe_contraction() {
    volatile float a = 1.0f + 0x1p-23f;  // exact, and its square is not
    volatile float rounded = a * a;      // rounded once, forced through memory
    volatile float negated = -rounded;
    // Contracted, the product keeps the bits `rounded` threw away and the
    // difference is exactly that rounding error; uncontracted, both products
    // round identically and it is exactly zero.
    const float residual = a * a + negated;
    return residual != 0.0f;
}

bool probe_flush_to_zero() {
    volatile float tiny = 0x1p-140f;  // subnormal as a float, exact as a bit pattern
    volatile float one = 1.0f;
    return (tiny * one) == 0.0f;
}

std::string fp_fingerprint() {
    std::string s = probe_contraction() ? "contract=on" : "contract=off";
    s += probe_flush_to_zero() ? " subnormals=flush" : " subnormals=keep";
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    s += " fast-math=on";
#else
    s += " fast-math=off";
#endif
    return s;
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

// Each arm names TWO things: the platform whose macros select it, and the
// floating-point configuration `fp_fingerprint()` reported on the build that
// produced the table. A new arm gets its second line from the generated
// table's header, which is written with the configuration that produced it.
#if defined(__linux__) && defined(__x86_64__)
#define CLAY_PARITY_TABLE "x86-64 Linux"
// Baseline x86-64 carries no FMA instruction and the presets pass no `-march`,
// so a default build contracts nothing here — `-ffp-contract=fast` or not.
#define CLAY_PARITY_TABLE_FP "contract=off subnormals=keep fast-math=off"
const Golden kGoldens[] = {
#include "mesh_sculpt_goldens_linux_x64.inc"
};
#elif defined(__APPLE__) && defined(__aarch64__)
#define CLAY_PARITY_TABLE "arm64 macOS"
// Measured on the machine this table was baselined on: aarch64 has FMA in the
// baseline ISA, so Clang's default `-ffp-contract=fast` reaches it.
#define CLAY_PARITY_TABLE_FP "contract=on subnormals=keep fast-math=off"
const Golden kGoldens[] = {
#include "mesh_sculpt_goldens_macos_arm64.inc"
};
#elif defined(_MSC_VER) && defined(_M_X64)
#define CLAY_PARITY_TABLE "x64 MSVC"
// `/fp:precise` over the SSE2 baseline: no `/arch:AVX2`, so no FMA to contract
// into. Unlike the two above this one was reasoned rather than measured — the
// Windows leg is not a machine this repository can run by hand — which costs
// nothing if it is wrong: a record that disagrees with a build whose hashes
// all match prints a line and passes.
#define CLAY_PARITY_TABLE_FP "contract=off subnormals=keep fast-math=off"
const Golden kGoldens[] = {
#include "mesh_sculpt_goldens_msvc_x64.inc"
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

// Null where no configuration is recorded — an unbaselined toolchain, or a
// table that predates the record. Null means "cannot diagnose", never
// "matches".
#if defined(CLAY_PARITY_TABLE_FP)
const char* const kTableFp = CLAY_PARITY_TABLE_FP;
#else
const char* const kTableFp = nullptr;
#endif

// -- what a hash difference MEANS ---------------------------------------------
//
// Decided in one place and off plain numbers, so the decision is testable
// without a mismatched build to hand — see the case at the bottom of this file.

struct HashTally {
    std::size_t total = 0;
    std::size_t hash_mismatches = 0;
    std::size_t moved_mismatches = 0;
    std::vector<std::string> named;  // "fixture/verb", the first few
};

struct HashVerdict {
    bool configuration_mismatch = false;  // the build's FP settings, not its arithmetic
    std::string report;                   // empty when there is nothing to report
};

// At most this many case names in a report, and at most this many individual
// `CHECK`s when the difference is genuinely arithmetic. Three fixtures' worth
// of detail is a defect you can read; eighty is a wall.
constexpr std::size_t kNamedLimit = 8;

std::string join_names(const HashTally& t) {
    std::string s;
    for (std::size_t i = 0; i < t.named.size(); ++i) {
        if (i) s += ", ";
        s += t.named[i];
    }
    if (t.hash_mismatches > t.named.size()) {
        s += ", and ";
        s += std::to_string(t.hash_mismatches - t.named.size());
        s += " more";
    }
    return s;
}

HashVerdict judge(const HashTally& t, const std::string& local_fp, const char* table_fp,
                  const char* table_name, const std::string& out_path) {
    HashVerdict v;
    if (t.hash_mismatches == 0) return v;
    v.configuration_mismatch = table_fp != nullptr && local_fp != table_fp;
    std::ostringstream o;
    o << (v.configuration_mismatch ? "FP CONFIGURATION MISMATCH, NOT ARITHMETIC DRIFT.\n"
                                   : "golden hashes differ.\n");
    o << "  " << t.hash_mismatches << " of " << t.total << " hashes differ from the \""
      << table_name << "\" table, and " << t.moved_mismatches << " of " << t.total
      << " moved counts differ from the reference.\n";
    o << "  this build emits: " << local_fp << "\n";
    o << "  the table records: " << (table_fp ? table_fp : "nothing — no configuration recorded")
      << "\n";
    o << "  cases: " << join_names(t) << "\n";
    if (v.configuration_mismatch) {
        o << "  A hash here is a property of the arithmetic a build EMITS, and these two builds\n"
             "  do not emit the same arithmetic, so every one of these differences is explained\n"
             "  before any of them is evidence about the code. "
          << (t.moved_mismatches == 0
                  ? "The portable half agrees on every\n  case, which is what that looks like.\n"
                  : "The moved counts differ too, which\n  a configuration alone does NOT explain "
                    "— read those first.\n")
          << "  Build with the configuration the table records, or baseline this one: a table for\n"
             "  it was written to "
          << out_path << " (CLAY_PARITY_REGEN=1 to rewrite it deliberately).\n";
    } else if (table_fp == nullptr) {
        o << "  No configuration is recorded for this table, so a mismatch cannot be told apart\n"
             "  from arithmetic drift here. Record one beside CLAY_PARITY_TABLE in this file.\n";
    } else {
        o << "  This build's floating-point configuration is the one the table was baselined\n"
             "  under, so this is arithmetic: something in the kernels moved.\n";
    }
    v.report = o.str();
    return v;
}

}  // namespace

TEST_CASE("mesh sculpt parity: every verb on every fixture is byte-identical") {
    // A platform with no table PRINTS one and skips the byte comparison. The
    // moved counts below still run, so the case keeps saying whether the verbs
    // did the same work; what it stops claiming is byte-identity against a
    // machine that is not this one, which it was never able to claim.
    const bool regen = std::getenv("CLAY_PARITY_REGEN") != nullptr;
    // An unbaselined toolchain WRITES its table out, and printing alone is not
    // enough to make that useful. Every ctest preset here sets
    // outputOnFailure, so a passing test's stdout never reaches a CI log --
    // and a passing test is exactly what an unbaselined toolchain now is. The
    // machine that needs a table is the one nobody has local access to, so the
    // table has to leave the machine as a FILE the workflow can upload.
    //
    // CLAY_PARITY_OUT names it; the default sits in the working directory
    // ctest runs the binary from, which is the build tree. `.generated` is in
    // the name so nothing mistakes it for a committed table: it is an input to
    // a human decision, not a baseline until someone moves it.
    //
    // A build whose floating-point configuration is not the table's writes one
    // too. That build cannot be gated against this table at all, and the table
    // it would need is the only thing that changes that; CI already collects
    // the file as an artifact.
    const std::string fp = fp_fingerprint();
    const bool fp_differs = kTableFp != nullptr && fp != kTableFp;
    const bool write_table = regen || !kHaveGoldens || fp_differs;
    // ...but it does not PRINT one. Eighty lines of table ahead of the one
    // line saying why this build could not be gated is the wall this change
    // exists to remove; the failure names the file instead. The two cases that
    // print are the two where stdout is the mechanism: a deliberate
    // re-baseline, and a toolchain nobody has a table for.
    const bool print_table = regen || !kHaveGoldens;
    const char* out_env = std::getenv("CLAY_PARITY_OUT");
    const std::string out_path = out_env ? out_env : "mesh_sculpt_goldens.generated.inc";
    std::FILE* out = nullptr;
    if (write_table) {
        out = std::fopen(out_path.c_str(), "w");
        // A table that cannot be written is worth saying out loud rather than
        // silently not producing: the run still passes, and someone would
        // otherwise go looking for an artifact that was never created.
        if (!out) MESSAGE("could not open " << out_path << " to write a table");
        // The configuration goes in the file, not just in the log. A table
        // without one cannot be adopted: whoever adds the arm needs the second
        // line as much as the first.
        if (print_table) std::printf("// floating-point configuration: %s\n", fp.c_str());
        if (out) std::fprintf(out, "// floating-point configuration: %s\n", fp.c_str());
    }
    if (!kHaveGoldens) {
        MESSAGE("no hash table for this toolchain: the byte comparison is "
                "skipped and the moved counts are still checked against the "
                "reference. A table for it is written to "
                << out_path
                << " (and printed below) -- move it to "
                   "mesh_sculpt_goldens_<toolchain>.inc and add the arm to the "
                   "#if above. See this file's header for why a table is "
                   "per-platform.");
    }
    // Every case runs and is RECORDED first, and the comparison is a second
    // pass. Not a style choice: what 72 differing hashes mean depends on how
    // many of them there are and on whether the moved counts came with them,
    // and a loop that asserts as it goes has already spent its report by the
    // time it knows.
    std::vector<Golden> results;
    results.reserve(kReferenceCount);
    for (const Fixture& fx : kFixtures) {
        for (const VerbCase& vc : kVerbs) {
            std::uint32_t moved = 0;
            const std::uint64_t h = run_case(fx, vc.verb, &moved);
            results.push_back({fx.name, vc.name, h, moved});
            if (write_table) {
                if (print_table)
                    std::printf("    {\"%s\", \"%s\", %lluull, %uu},\n", fx.name, vc.name,
                                static_cast<unsigned long long>(h), moved);
                if (out)
                    std::fprintf(out, "    {\"%s\", \"%s\", %lluull, %uu},\n", fx.name,
                                 vc.name, static_cast<unsigned long long>(h), moved);
            }
        }
    }
    if (out) std::fclose(out);
    // A DELIBERATE re-baseline prints and checks nothing. An unbaselined
    // toolchain prints AND still gates its moved counts, which is the
    // difference between the two: one is being rewritten on purpose, the other
    // is simply not listed yet.
    if (regen) return;

    CHECK(results.size() == kReferenceCount);
    REQUIRE(results.size() <= kReferenceCount);
    HashTally tally;
    tally.total = results.size();
    std::vector<std::size_t> differing;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Golden& r = results[i];
        const Golden& ref = kReference[i];
        INFO("case " << std::string(r.fixture) << "/" << std::string(r.verb));
        REQUIRE(std::string(ref.fixture) == r.fixture);
        REQUIRE(std::string(ref.verb) == r.verb);
        // The moved count first: when both fail it is the readable half, and a
        // verb that stopped reaching anything is a different defect from one
        // whose arithmetic drifted. Against the REFERENCE table, on every
        // toolchain — this half is portable and measured to be, and it is
        // gated the same way whatever the local build's FP configuration is.
        CHECK(r.moved == ref.moved);
        if (r.moved != ref.moved) ++tally.moved_mismatches;
        // The hash is the point of this file: a re-associated accumulation
        // moves exactly one bit, and every tolerance in this tree admits that.
        // Against THIS toolchain's table, and only where there is one; see the
        // header. It is COLLECTED here and adjudicated below.
        if (!kHaveGoldens) continue;
        const Golden& g = kGoldens[i];
        REQUIRE(std::string(g.fixture) == r.fixture);
        REQUIRE(std::string(g.verb) == r.verb);
        // A comparison that PASSES is asserted where it happens, so the
        // assertion count still says all eighty hashes were compared — this
        // repository reads its differentials in assertions, and a gate that
        // stops counting is a gate that can stop firing unnoticed. A
        // comparison that fails is only recorded: what it means is not known
        // until every case has run.
        if (r.hash == g.hash) {
            CHECK(r.hash == g.hash);
            continue;
        }
        ++tally.hash_mismatches;
        differing.push_back(i);
        if (tally.named.size() < kNamedLimit)
            tally.named.push_back(std::string(r.fixture) + "/" + r.verb);
    }

    const HashVerdict verdict = judge(tally, fp, kTableFp, CLAY_PARITY_TABLE, out_path);
    if (verdict.configuration_mismatch) {
        // ONE failure, saying what it is. The individual comparisons are not
        // repeated, because every one of them is the same fact about the build
        // and none of them is a fact about the kernels.
        FAIL_CHECK(verdict.report);
    } else if (kHaveGoldens && tally.hash_mismatches > 0) {
        MESSAGE(verdict.report);
        for (std::size_t n = 0; n < differing.size() && n < kNamedLimit; ++n) {
            const Golden& r = results[differing[n]];
            INFO("case " << std::string(r.fixture) << "/" << std::string(r.verb));
            CHECK(r.hash == kGoldens[differing[n]].hash);
        }
    } else if (fp_differs) {
        // The record was wrong, or this build reaches the same arithmetic by a
        // different route. Either way the gate held, so this is a note and not
        // a failure — the record exists to explain a difference, never to
        // manufacture one.
        MESSAGE("this build emits " << fp << " where the \"" << CLAY_PARITY_TABLE
                                    << "\" table records " << std::string(kTableFp)
                                    << ", and every hash matched anyway. The recorded "
                                       "configuration is what wants correcting, not the table.");
    }
    if (kHaveGoldens) MESSAGE("golden table: " << CLAY_PARITY_TABLE << " [" << fp << "]");
}

// The verbs that must do SOMETHING on the fixtures, so a golden table full of
// zero-move entries cannot pass as parity. This is the discriminating half:
// the hashes prove nothing changed, this proves there was something to change.
TEST_CASE("mesh sculpt parity: the fixtures are discriminating") {
    for (const Fixture& fx : kFixtures) {
        for (const VerbCase& vc : kVerbs) {
            std::uint32_t moved = 0;
            run_case(fx, vc.verb, &moved);
            INFO("case " << std::string(fx.name) << "/" << std::string(vc.name));
            CHECK(moved > 0);
        }
    }
}

// The probe has to describe THIS build, and a probe that always answers the
// same thing would look exactly like one that works — the constant folder
// answering instead of the back end is the specific way this goes wrong. So
// the two arithmetics are constructed side by side and the probe is required
// to name the one the plain expression actually took.
TEST_CASE("mesh sculpt parity: the FP probe describes the build it is in") {
    volatile float a = 1.0f + 0x1p-23f;
    volatile float rounded = a * a;
    volatile float negated = -rounded;
    const float emitted = a * a + negated;                 // whatever this build does
    const float contracted = std::fmaf(a, a, negated);     // one rounding, by definition
    const float separate = rounded + negated;              // two roundings
    // The fixture asserts its own precondition: if these two agreed, the probe
    // could not distinguish anything and would pass for the wrong reason.
    CHECK(separate == 0.0f);
    CHECK(contracted != 0.0f);
    if (probe_contraction())
        CHECK(emitted == contracted);
    else
        CHECK(emitted == separate);
    // A fingerprint is compared as a whole string, so it has to be total.
    const std::string fp = fp_fingerprint();
    CHECK(fp.find("contract=") != std::string::npos);
    CHECK(fp.find("subnormals=") != std::string::npos);
    CHECK(fp.find("fast-math=") != std::string::npos);
    CHECK(fp == fp_fingerprint());
}

// The diagnosis itself, off plain numbers — this is the half that has to hold
// on a machine where no mismatched build exists to produce. #581: a build with
// different FP flags selects the same table, fails 72 of 80 hashes and 0 of 80
// moved counts, and every one of those failures reads as arithmetic.
TEST_CASE("mesh sculpt parity: a configuration mismatch is not reported as drift") {
    const char* kBaselined = "contract=on subnormals=keep fast-math=off";
    const std::string kLocal = "contract=off subnormals=keep fast-math=off";
    HashTally wholesale;
    wholesale.total = 80;
    wholesale.hash_mismatches = 72;
    wholesale.moved_mismatches = 0;
    wholesale.named = {"plane/grab", "plane/draw"};

    SUBCASE("a differing configuration is named as the cause") {
        const HashVerdict v = judge(wholesale, kLocal, kBaselined, "arm64 macOS", "out.inc");
        CHECK(v.configuration_mismatch);
        CHECK(v.report.find("CONFIGURATION MISMATCH") != std::string::npos);
        // Both configurations, or the report cannot be acted on.
        CHECK(v.report.find(kLocal) != std::string::npos);
        CHECK(v.report.find(kBaselined) != std::string::npos);
        CHECK(v.report.find("72 of 80") != std::string::npos);
        CHECK(v.report.find("out.inc") != std::string::npos);
    }
    SUBCASE("the same configuration means the arithmetic moved") {
        HashTally one = wholesale;
        one.hash_mismatches = 1;
        one.named = {"plane/grab"};
        const HashVerdict v = judge(one, kLocal, kLocal.c_str(), "arm64 macOS", "out.inc");
        CHECK_FALSE(v.configuration_mismatch);
        CHECK(v.report.find("something in the kernels moved") != std::string::npos);
    }
    SUBCASE("a wholesale difference under the SAME configuration is still drift") {
        // The count is not the evidence — the configuration is. 72 differences
        // on a build that matches the baseline is a defect, however unlikely,
        // and must not be explained away by its size.
        const HashVerdict v = judge(wholesale, kLocal, kLocal.c_str(), "arm64 macOS", "out.inc");
        CHECK_FALSE(v.configuration_mismatch);
    }
    SUBCASE("an unrecorded configuration cannot diagnose and says so") {
        const HashVerdict v = judge(wholesale, kLocal, nullptr, "x64 MSVC", "out.inc");
        CHECK_FALSE(v.configuration_mismatch);
        CHECK(v.report.find("No configuration is recorded") != std::string::npos);
        CHECK(v.report.find(kLocal) != std::string::npos);
    }
    SUBCASE("moved counts differing too is called out, because a flag does not explain them") {
        HashTally with_moved = wholesale;
        with_moved.moved_mismatches = 3;
        const HashVerdict v = judge(with_moved, kLocal, kBaselined, "arm64 macOS", "out.inc");
        CHECK(v.configuration_mismatch);
        CHECK(v.report.find("read those first") != std::string::npos);
    }
    SUBCASE("nothing differing reports nothing") {
        HashTally clean;
        clean.total = 80;
        const HashVerdict v = judge(clean, kLocal, kBaselined, "arm64 macOS", "out.inc");
        CHECK_FALSE(v.configuration_mismatch);
        CHECK(v.report.empty());
    }
    SUBCASE("the named cases are capped and the remainder counted") {
        HashTally many = wholesale;
        many.named = {"a/1", "b/2", "c/3", "d/4", "e/5", "f/6", "g/7", "h/8"};
        const HashVerdict v = judge(many, kLocal, kBaselined, "arm64 macOS", "out.inc");
        CHECK(v.report.find("and 64 more") != std::string::npos);
    }
}
