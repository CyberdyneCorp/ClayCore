#include "clay/brush/preset.h"

#include <cstring>

namespace clay {
namespace brush {
namespace {

constexpr std::uint32_t kMagic = 0x52504243u;  // 'CBPR'

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_i32(std::vector<std::uint8_t>& out, std::int32_t v) {
    put_u32(out, static_cast<std::uint32_t>(v));
}

void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

void put_vec3(std::vector<std::uint8_t>& out, kernel::cfloat3 v) {
    put_f32(out, v.x);
    put_f32(out, v.y);
    put_f32(out, v.z);
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (at + 1 > size) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    std::uint16_t u16() {
        const std::uint8_t a = u8(), b = u8();
        return static_cast<std::uint16_t>(a | (static_cast<std::uint16_t>(b) << 8));
    }
    std::uint32_t u32() {
        if (at + 4 > size) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(data[at]) |
                                (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(data[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const std::uint32_t bits = u32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    kernel::cfloat3 vec3() {
        const float x = f32(), y = f32(), z = f32();
        return kernel::cf3(x, y, z);
    }
};

// A named brush, built from a verb's own decomposition and then adjusted. The
// point of starting from `model_of` is that a preset cannot silently disagree
// with the vocabulary about what a verb IS — it can only choose among the axes.
BrushPreset make(const char* name, mesh::MeshBrush verb) {
    BrushPreset p;
    p.name = name;
    p.model = mesh::model_of(verb);
    p.settings.falloff = p.model.falloff;
    p.settings.geodesic = p.model.footprint == mesh::BrushFootprint::SurfaceWalk;
    p.stroke.spacing = 0.25f;
    p.stroke.radius = 0.25f;
    p.stroke.strength = 1.0f;
    return p;
}

}  // namespace

std::vector<std::uint8_t> BrushPreset::serialize() const {
    std::vector<std::uint8_t> out;
    put_u32(out, kMagic);
    put_u16(out, kBrushPresetVersion);

    put_u32(out, static_cast<std::uint32_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());

    // The stroke preset writes itself, so its own version rules apply to its
    // own bytes and this schema does not have to know its layout.
    const std::vector<std::uint8_t> stroke_bytes = stroke.serialize();
    put_u32(out, static_cast<std::uint32_t>(stroke_bytes.size()));
    out.insert(out.end(), stroke_bytes.begin(), stroke_bytes.end());

    put_u8(out, static_cast<std::uint8_t>(model.verb));
    put_u8(out, static_cast<std::uint8_t>(model.footprint));
    put_u8(out, static_cast<std::uint8_t>(model.falloff));
    put_u8(out, static_cast<std::uint8_t>(model.frame));
    put_u8(out, static_cast<std::uint8_t>(model.kernel));
    put_u8(out, static_cast<std::uint8_t>(model.target));
    put_u8(out, static_cast<std::uint8_t>(model.post));

    // The settings that are the brush's identity. NOT `center`, `direction` or
    // `seed_class` — those are where a stamp landed — and NOT the alpha, which
    // is caller-owned image content this format refuses to carry.
    put_u8(out, static_cast<std::uint8_t>(settings.falloff));
    put_u8(out, settings.geodesic ? 1 : 0);
    put_u8(out, static_cast<std::uint8_t>(settings.flatten_mode));
    put_u8(out, settings.use_given_plane ? 1 : 0);
    put_vec3(out, settings.plane_point);
    put_vec3(out, settings.plane_normal);
    put_vec3(out, settings.deposit_normal);
    put_vec3(out, settings.color);
    put_f32(out, settings.polish_angle);
    put_f32(out, settings.layer_height);
    put_i32(out, settings.smooth_iterations);
    put_f32(out, settings.strength);

    put_u32(out, settings.automask.factors);
    put_f32(out, settings.automask.normal_angle);
    put_i32(out, settings.automask.boundary_rings);
    put_f32(out, settings.automask.cavity_strength);
    return out;
}

std::optional<BrushPreset> BrushPreset::deserialize(const std::uint8_t* data, std::size_t size) {
    if (!data) return std::nullopt;
    Reader r{data, size};
    if (r.u32() != kMagic) return std::nullopt;
    const std::uint16_t version = r.u16();
    // REFUSED rather than reinterpreted. A newer layout read as this one gives
    // a brush that is not the brush somebody saved, which is worse than an
    // error because it looks like it worked.
    if (!r.ok || version > kBrushPresetVersion) return std::nullopt;

    BrushPreset p;
    const std::uint32_t name_len = r.u32();
    if (!r.ok || name_len > size - r.at) return std::nullopt;
    p.name.assign(reinterpret_cast<const char*>(data + r.at), name_len);
    r.at += name_len;

    const std::uint32_t stroke_len = r.u32();
    if (!r.ok || stroke_len > size - r.at) return std::nullopt;
    const std::optional<StrokePreset> stroke = StrokePreset::deserialize(data + r.at, stroke_len);
    if (!stroke) return std::nullopt;
    p.stroke = *stroke;
    r.at += stroke_len;

    p.model.verb = static_cast<mesh::MeshBrush>(r.u8());
    p.model.footprint = static_cast<mesh::BrushFootprint>(r.u8());
    p.model.falloff = static_cast<mesh::MeshFalloff>(r.u8());
    p.model.frame = static_cast<mesh::BrushFrame>(r.u8());
    p.model.kernel = static_cast<mesh::BrushKernelId>(r.u8());
    p.model.target = static_cast<mesh::BrushWriteTarget>(r.u8());
    p.model.post = static_cast<mesh::BrushPostPolicy>(r.u8());

    p.settings.falloff = static_cast<mesh::MeshFalloff>(r.u8());
    p.settings.geodesic = r.u8() != 0;
    p.settings.flatten_mode = static_cast<field::FlattenMode>(r.u8());
    p.settings.use_given_plane = r.u8() != 0;
    p.settings.plane_point = r.vec3();
    p.settings.plane_normal = r.vec3();
    p.settings.deposit_normal = r.vec3();
    p.settings.color = r.vec3();
    p.settings.polish_angle = r.f32();
    p.settings.layer_height = r.f32();
    p.settings.smooth_iterations = r.i32();
    p.settings.strength = r.f32();

    p.settings.automask.factors = r.u32();
    p.settings.automask.normal_angle = r.f32();
    p.settings.automask.boundary_rings = r.i32();
    p.settings.automask.cavity_strength = r.f32();

    if (!r.ok) return std::nullopt;
    // A verb outside the vocabulary is a corrupt or hostile record, and
    // constructing a preset around it would put an out-of-range enumerator into
    // a switch that has no default.
    if (static_cast<std::uint8_t>(p.model.verb) > static_cast<std::uint8_t>(mesh::MeshBrush::Smear))
        return std::nullopt;
    return p;
}

std::vector<BrushPreset> reference_presets() {
    std::vector<BrushPreset> out;

    // -- the deposit family --------------------------------------------------
    {
        // ZBrush's Standard: a rounded organic swell along the region's normal.
        BrushPreset p = make("Standard", mesh::MeshBrush::Draw);
        p.settings.strength = 0.5f;
        out.push_back(p);
    }
    {
        // Clay: the deposit CLAMPED to a plane at the stamp height, so the
        // stroke leaves a flat-topped strip rather than following whatever the
        // surface was doing underneath.
        BrushPreset p = make("Clay", mesh::MeshBrush::Clay);
        p.settings.strength = 0.5f;
        out.push_back(p);
    }
    {
        // Clay Buildup: the same kernel, denser and accumulating. THIS IS THE
        // ROW THE MODEL EXISTS FOR — it differs from Clay only in the stroke,
        // and it needs no engine path at all.
        BrushPreset p = make("Clay Buildup", mesh::MeshBrush::Clay);
        p.settings.strength = 0.35f;
        p.stroke.spacing = 0.1f;
        p.stroke.accumulation = Accumulation::Buildup;
        out.push_back(p);
    }
    {
        // Clay Strips: a square-ish footprint in ZBrush, which this library
        // reaches through an alpha rather than a second falloff. Constant
        // falloff is the closest the curve vocabulary comes, and the alpha the
        // caller lends does the rest.
        BrushPreset p = make("Clay Strips", mesh::MeshBrush::Clay);
        p.settings.falloff = mesh::MeshFalloff::Constant;
        p.model.falloff = mesh::MeshFalloff::Constant;
        p.settings.strength = 0.4f;
        p.stroke.spacing = 0.15f;
        out.push_back(p);
    }
    {
        BrushPreset p = make("Inflate", mesh::MeshBrush::Inflate);
        p.settings.strength = 0.35f;
        out.push_back(p);
    }
    {
        // Layer: deposits to a ceiling measured from where the stroke started,
        // so a slow pass and a fast one over the same path agree.
        BrushPreset p = make("Layer", mesh::MeshBrush::Layer);
        p.settings.layer_height = 0.05f;
        p.settings.strength = 1.0f;
        out.push_back(p);
    }

    // -- the smoothing family ------------------------------------------------
    {
        BrushPreset p = make("Smooth", mesh::MeshBrush::Smooth);
        p.settings.strength = 0.5f;
        p.settings.smooth_iterations = 1;
        out.push_back(p);
    }
    {
        // Relax slides vertices ALONG the surface to even their spacing where
        // Smooth moves them toward the neighbourhood average. Topology is fixed
        // here, so this is what recovers a stretched patch.
        BrushPreset p = make("Relax", mesh::MeshBrush::Relax);
        p.settings.strength = 0.5f;
        out.push_back(p);
    }
    {
        // hPolish: smoothing GATED by how far the surface bends, so noise goes
        // and a hard edge stays. The tight default angle is what makes it a
        // polish rather than a smooth wearing a different name.
        BrushPreset p = make("hPolish", mesh::MeshBrush::Polish);
        p.settings.polish_angle = 0.2f;
        p.settings.strength = 0.6f;
        p.settings.smooth_iterations = 2;
        out.push_back(p);
    }

    // -- the hard-surface family ---------------------------------------------
    {
        // Trim Dynamic: flatten onto the region's own plane, CUT ONLY, so a
        // facet forms against untouched surface. Cutting without filling is the
        // whole brush.
        BrushPreset p = make("Trim Dynamic", mesh::MeshBrush::Flatten);
        p.settings.flatten_mode = field::FlattenMode::CutOnly;
        p.settings.strength = 0.7f;
        out.push_back(p);
    }
    {
        BrushPreset p = make("Flatten", mesh::MeshBrush::Flatten);
        p.settings.flatten_mode = field::FlattenMode::TwoSided;
        p.settings.strength = 0.5f;
        out.push_back(p);
    }
    {
        BrushPreset p = make("Scrape", mesh::MeshBrush::Scrape);
        p.settings.flatten_mode = field::FlattenMode::CutOnly;
        p.settings.strength = 0.5f;
        out.push_back(p);
    }
    {
        // Dam Standard: a tight cut AND a squeeze in one stamp, which is what
        // closes the fold as it forms instead of leaving a rounded ditch.
        BrushPreset p = make("Dam Standard", mesh::MeshBrush::Crease);
        p.settings.strength = 0.5f;
        p.stroke.spacing = 0.1f;
        out.push_back(p);
    }
    {
        BrushPreset p = make("Pinch", mesh::MeshBrush::Pinch);
        p.settings.strength = 0.4f;
        out.push_back(p);
    }

    // -- the moving family ---------------------------------------------------
    {
        // Move anchors on the first stamp and drags by the motion between
        // stamps.
        BrushPreset p = make("Move", mesh::MeshBrush::Grab);
        p.stroke.spacing = 0.05f;
        out.push_back(p);
    }
    {
        // Move Topological is the same brush measuring its falloff ALONG the
        // surface, so a brush on the upper lip does not drag the chin through
        // the closed mouth. One axis apart from Move.
        BrushPreset p = make("Move Topological", mesh::MeshBrush::Grab);
        p.settings.geodesic = true;
        p.model.footprint = mesh::BrushFootprint::SurfaceWalk;
        p.stroke.spacing = 0.05f;
        out.push_back(p);
    }
    {
        // Snake Hook re-anchors on every stamp, so its region walks with the
        // pull. That difference lives in the stroke consumer, not in the verb.
        BrushPreset p = make("Snake Hook", mesh::MeshBrush::Snakehook);
        p.stroke.spacing = 0.05f;
        out.push_back(p);
    }
    {
        BrushPreset p = make("Nudge", mesh::MeshBrush::Nudge);
        p.stroke.spacing = 0.05f;
        out.push_back(p);
    }
    {
        // Rake: a directional brush whose stamp follows the STYLUS BARREL
        // rather than the path. Nothing about the deformation is new — what
        // makes it a rake is the orientation reaching the alpha, which is why
        // `MeshStrokeOptions::orient_alpha_by_stamp` exists.
        BrushPreset p = make("Rake", mesh::MeshBrush::Draw);
        p.stroke.rotate_to_azimuth = true;
        p.stroke.spacing = 0.1f;
        p.settings.strength = 0.35f;
        out.push_back(p);
    }

    // -- the colour pair -----------------------------------------------------
    {
        BrushPreset p = make("Paint", mesh::MeshBrush::Paint);
        p.settings.strength = 0.75f;
        out.push_back(p);
    }
    {
        BrushPreset p = make("Smear", mesh::MeshBrush::Smear);
        p.settings.strength = 0.5f;
        p.stroke.spacing = 0.05f;
        out.push_back(p);
    }
    return out;
}

std::optional<BrushPreset> reference_preset(const std::string& name) {
    for (BrushPreset& p : reference_presets())
        if (p.name == name) return p;
    return std::nullopt;
}

}  // namespace brush
}  // namespace clay
