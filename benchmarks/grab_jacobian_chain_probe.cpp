// WHERE THE 291,000x PESSIMISM COMES FROM, IN THREE LAYERS.
// NOT a gated benchmark.
//
// #541 established the gap: at 48 Move gestures the layer declares a Lipschitz
// bound of 1,443,068 while the field's real gradient measures 4.958. It did not
// establish WHY, and the answer decides whether anything can be done.
//
// The declared bound is reached by two approximations, one after the other, and
// this separates them:
//
//   A  DECLARED   what deformer_lipschitz computes today: for links that can
//                 meet, the product of each link's GLOBAL maximum stretch.
//   B  LOCAL      the same product, but each link's Jacobian evaluated where
//                 the composed point ACTUALLY passes through it rather than at
//                 that link's worst point anywhere in its support.
//   C  COMPOSED   the spectral norm of the Jacobian MATRIX product
//                 J_{n-1} ... J_0, which is <= the product of the norms and is
//                 strictly smaller whenever the stretch directions disagree.
//   D  FIELD      the finite-difference gradient of the compiled field, which
//                 is what sphere tracing is actually limited by.
//
// A -> B prices "global max versus local value".
// B -> C prices submultiplicativity: ||J1 J2|| <= ||J1|| ||J2||.
// C -> D is whatever the base primitive's own gradient contributes.
//
// WHAT IS ALREADY RULED OUT, so this does not re-measure it. The slack is NOT
// spatial. deformer_lipschitz already prices per-link NEIGHBOURHOODS (#386 then
// #452), and ClaySpaceDesktop's control shows it is exact on the case it exists
// for: forty-eight DISJOINT grabs declare 1.19 at depth 48 exactly as at depth
// 1, flat. Everything below is the overlapping case, where the product is
// CORRECTLY derived and still enormous -- and that is the case a sculptor
// produces by definition, because working one area makes grabs overlap.
//
// NOTHING HERE MAY BE DECLARED. B, C and D are all sampled maxima: lower bounds
// on their suprema, with no test that they found THE maximum rather than A
// maximum. They say how much slack exists, not what to put in a header. A bound
// below the true gradient does not make the marcher slow, it makes it step
// through the surface -- and #540 already shows what a field that stops being
// traceable looks like. Only a provably conservative construction may be
// declared: closed form, interval arithmetic, affine arithmetic, interval
// Jacobian propagation, or a certified grid bound with a derivative remainder.
//
// Exits non-zero if the layering is violated -- D > C > B > A must hold at
// every depth, since each is a relaxation of the next, and a breach means one
// of them is measuring something other than what it is named.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

constexpr float kMoveRadius = 0.40f;

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

// Row-major 3x3.
struct M3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

M3 mul(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a.m[i * 3 + k] * b.m[k * 3 + j];
            r.m[i * 3 + j] = s;
        }
    return r;
}

// Largest singular value by power iteration on J^T J. Enough digits for a
// ratio, and it is the norm submultiplicativity is stated in.
double spectral_norm(const M3& j) {
    M3 jt;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) jt.m[i * 3 + k] = j.m[k * 3 + i];
    const M3 a = mul(jt, j);
    double v[3] = {0.5773502691896258, 0.5773502691896258, 0.5773502691896258};
    double lambda = 0;
    for (int it = 0; it < 64; ++it) {
        double w[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) w[i] += a.m[i * 3 + k] * v[k];
        const double n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (n < 1e-300) return 0.0;
        for (int i = 0; i < 3; ++i) v[i] = w[i] / n;
        lambda = n;
    }
    return std::sqrt(lambda);
}

// One resolved grab, in the item's frame. With a single sphere at the origin
// carrying no transform, that frame is the world's.
struct Grab {
    V3 centre;
    double radius = 0;
    V3 disp;
    int32_t ease = 0;
    int32_t front_only = 0;
};

// cgrab_point with ease_linear and front_only 0:
//     w(p) = clamp(1 - |p-c|/r, 0, 1)
//     G(p) = p - d * w(p)
double weight(const Grab& g, const V3& p) {
    const double dist = len(sub(p, g.centre));
    return std::clamp(1.0 - dist / std::max(g.radius, 1e-6), 0.0, 1.0);
}

V3 apply(const Grab& g, const V3& p) {
    const double w = weight(g, p);
    return {p.x - g.disp.x * w, p.y - g.disp.y * w, p.z - g.disp.z * w};
}

// J = I - d (grad w)^T. Inside the support, grad w = -(p-c) / (r |p-c|), so
// J = I + (1/r) d ((p-c)/|p-c|)^T. Outside, and in the clamped plateau, grad w
// is zero and J is the identity.
M3 jacobian(const Grab& g, const V3& p) {
    M3 j;  // identity
    const V3 rel = sub(p, g.centre);
    const double dist = len(rel);
    const double t = 1.0 - dist / std::max(g.radius, 1e-6);
    if (t <= 0.0 || t >= 1.0 || dist < 1e-9) return j;  // clamped: no slope
    const double inv = 1.0 / (g.radius * dist);
    const double d[3] = {g.disp.x, g.disp.y, g.disp.z};
    const double u[3] = {rel.x, rel.y, rel.z};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) j.m[i * 3 + k] += d[i] * u[k] * inv;
    return j;
}

// The GLOBAL maximum of ||J|| over a grab's support, which is what a per-link
// declared bound uses. |grad w| is 1/r everywhere the weight is unclamped, so
// the worst case is 1 + |d|/r, achieved where d aligns with the radial
// direction.
double global_link_bound(const Grab& g) {
    return 1.0 + len(g.disp) / std::max(g.radius, 1e-6);
}

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
    clay_node_id node = 0;
};

bool build(Doc* out) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    const float r = 1.0f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    clay_item_set_op(it, CLAY_OP_ADD);
    clay_node_id n = 0;
    const clay_result res = clay_layer_add_item(doc, layer, it, &n);
    clay_item_destroy(it);
    if (res != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    *out = Doc{doc, layer, n};
    return true;
}

// Apply one dab AND capture the grab it resolved, read back from the
// transaction rather than reconstructed -- clay_sdf_move_preview_grab reports
// exactly the parameters clay_item_add_deformer(CLAY_DEFORM_GRAB, ...) takes.
bool dab(Doc* d, int index, std::vector<Grab>* chain) {
    const float t = 0.37f * static_cast<float>(index);
    const float cx = std::cos(t) * 0.42f, cy = std::sin(t) * 0.42f;
    const float cz = std::sqrt(std::max(0.05f, 1.0f - cx * cx - cy * cy));
    const float anchor[3] = {cx, cy, cz};
    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = kMoveRadius;
    mp.ease = 0;
    mp.front_only = 0;

    clay_sdf_move_tx* tx = clay_sdf_move_begin(d->doc, d->layer, anchor, &mp, nullptr);
    if (!tx) return false;
    const float total[3] = {cx * 0.11f, cy * 0.11f, cz * 0.11f};
    clay_sculpt_dirty dirty;
    std::memset(&dirty, 0, sizeof dirty);
    dirty.struct_size = sizeof dirty;
    bool ok = clay_sdf_move_update(tx, total, &dirty) == CLAY_OK;

    if (ok) {
        std::size_t count = 0;
        if (clay_sdf_move_preview_grab_count(tx, d->node, &count) == CLAY_OK && count > 0) {
            // Newest goes to the FRONT of the chain, which is the order
            // ctape_deform_point applies: deformers[0] warps the point first.
            std::vector<Grab> fresh;
            for (std::size_t i = 0; i < count; ++i) {
                float c[3] = {0, 0, 0}, dsp[3] = {0, 0, 0}, rad = 0;
                int32_t ease = 0, front = 0;
                if (clay_sdf_move_preview_grab(tx, d->node, i, c, &rad, dsp, &ease, &front) ==
                    CLAY_OK) {
                    Grab g;
                    g.centre = {c[0], c[1], c[2]};
                    g.radius = rad;
                    g.disp = {dsp[0], dsp[1], dsp[2]};
                    g.ease = ease;
                    g.front_only = front;
                    fresh.push_back(g);
                }
            }
            chain->insert(chain->begin(), fresh.begin(), fresh.end());
        }
        clay_sculpt_budget b;
        std::memset(&b, 0, sizeof b);
        b.struct_size = sizeof b;
        ok = clay_sdf_move_commit(tx, &b) == CLAY_OK;
    }
    clay_sdf_move_destroy(tx);
    return ok;
}

struct Sample {
    double local_product = 0;   // B
    double composed_norm = 0;   // C
};

// Walk the chain at one point, accumulating both the product of per-link norms
// and the matrix product itself.
Sample walk(const std::vector<Grab>& chain, const V3& p) {
    Sample s;
    s.local_product = 1.0;
    M3 j;  // identity
    V3 q = p;
    for (const Grab& g : chain) {
        const M3 jd = jacobian(g, q);
        s.local_product *= spectral_norm(jd);
        j = mul(jd, j);
        q = apply(g, q);
    }
    s.composed_norm = spectral_norm(j);
    return s;
}

std::vector<V3> shell(int side) {
    std::vector<V3> pts;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < 5; ++k) {
                const double u = -0.9 + 1.8 * i / double(side - 1);
                const double v = -0.9 + 1.8 * j / double(side - 1);
                const double rr = u * u + v * v;
                if (rr > 0.81) continue;
                const double base = std::sqrt(std::max(0.02, 1.0 - rr));
                pts.push_back({u, v, base + (-0.14 + 0.07 * k)});
            }
    return pts;
}

double field_gradient_max(const clay_document* doc, clay_layer_id layer,
                          const std::vector<V3>& pts, double h) {
    const std::size_t n = pts.size();
    std::vector<float> probe(n * 6 * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const double base[3] = {pts[i].x, pts[i].y, pts[i].z};
        for (int axis = 0; axis < 3; ++axis)
            for (int sgn = 0; sgn < 2; ++sgn) {
                const std::size_t k = ((i * 3 + axis) * 2 + sgn) * 3;
                for (int a = 0; a < 3; ++a) probe[k + a] = float(base[a]);
                probe[k + axis] = float(base[axis] + (sgn ? -h : h));
            }
    }
    std::vector<float> dist(n * 6, 0.0f);
    if (clay_layer_eval_points(doc, layer, "cpu", probe.data(), n * 6, dist.data(), nullptr) !=
        CLAY_OK)
        return -1.0;
    double mx = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double g[3];
        for (int axis = 0; axis < 3; ++axis)
            g[axis] = (double(dist[(i * 3 + axis) * 2]) - dist[(i * 3 + axis) * 2 + 1]) / (2 * h);
        const double m = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (std::isfinite(m)) mx = std::max(mx, m);
    }
    return mx;
}

}  // namespace

int main() {
    std::printf("grab_jacobian_chain_probe: where the pessimism lives, in three layers\n");
    std::printf("  A declared   B local product   C composed matrix norm   D field gradient\n\n");
    std::printf("  moves |          A |   prod(max) |          B |      C |      D | A/B      B/C    C/D\n");

    const std::vector<V3> pts = shell(40);
    int failures = 0;

    for (const int n : {1, 4, 8, 16, 32, 48}) {
        Doc d;
        if (!build(&d)) {
            std::printf("FAIL build\n");
            return 1;
        }
        std::vector<Grab> chain;
        for (int i = 0; i < n; ++i)
            if (!dab(&d, i, &chain)) {
                std::printf("FAIL dab %d\n", i);
                return 1;
            }
        clay_field_report fr;
        std::memset(&fr, 0, sizeof fr);
        fr.struct_size = sizeof fr;
        clay_layer_field_report(d.doc, d.layer, 0.5f, &fr);

        // The product of each link's GLOBAL maximum stretch — what a per-link
        // declared bound is built from. Shown so A's derivation is visible
        // rather than asserted: if this tracks A, the declared bound really is
        // the product of global maxima and nothing else is in play.
        double global_product = 1.0;
        for (const Grab& g : chain) global_product *= global_link_bound(g);

        double bmax = 0, cmax = 0;
        for (const V3& p : pts) {
            const Sample s = walk(chain, p);
            bmax = std::max(bmax, s.local_product);
            cmax = std::max(cmax, s.composed_norm);
        }
        const double dmax = field_gradient_max(d.doc, d.layer, pts, 1e-3);
        const double A = fr.lipschitz;

        std::printf("  %5d | %10.2f | %11.2f | %10.3f | %6.3f | %6.3f | %7.1fx %6.2fx %5.2fx\n",
                    n, A, global_product, bmax, cmax, dmax, bmax > 0 ? A / bmax : 0.0,
                    cmax > 0 ? bmax / cmax : 0.0, dmax > 0 ? cmax / dmax : 0.0);

        // Each quantity is a relaxation of the next, so the ordering must hold.
        // A breach means one of them is not what it is named -- the failure this
        // file is most likely to have, and the one a table would hide.
        if (chain.size() != std::size_t(n))
            std::printf("        NOTE: captured %zu grabs for %d dabs\n", chain.size(), n);
        if (!(A >= bmax * 0.999)) {
            std::printf("        FAIL: declared %.3f is BELOW the local product %.3f\n", A, bmax);
            ++failures;
        }
        if (!(bmax >= cmax * 0.999)) {
            std::printf("        FAIL: local product %.3f is BELOW the composed norm %.3f, which\n"
                        "              violates submultiplicativity and means one is misnamed\n",
                        bmax, cmax);
            ++failures;
        }
        clay_document_destroy(d.doc);
    }

    std::printf("\n  A/B prices global-max-per-link against the local value on the trajectory.\n"
                "  B/C prices submultiplicativity: ||J1 J2|| <= ||J1|| ||J2||.\n"
                "  B, C and D are SAMPLED MAXIMA -- lower bounds on their suprema, with no test\n"
                "  that they found THE maximum rather than A maximum. They say how much slack\n"
                "  exists. None of them may be declared.\n");
    if (failures) {
        std::printf("\n%d layering failure(s); do not quote the table.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
