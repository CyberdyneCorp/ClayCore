// Brush stroke engine (brush-engine spec). See include/clay/brush/stroke.h for
// why resolution is pure and why stamps become ordinary edits.

#include "clay/brush/stroke.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clay {
namespace brush {

using kernel::cfloat3;

namespace {

// Same hash the voxel brush dither uses: integer mixing only, no floating
// point until the end, so a stroke jitters identically on every platform.
float stamp_noise(std::uint32_t index, std::uint32_t seed, std::uint32_t channel) {
    std::uint32_t h = seed * 0x9E3779B9u;
    h ^= index * 0x85EBCA6Bu;
    h = (h ^ (h >> 13)) * 0xC2B2AE35u;
    h ^= channel * 0x27D4EB2Fu;
    h = (h ^ (h >> 15)) * 0x165667B1u;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

// [-1, 1)
float stamp_signed_noise(std::uint32_t index, std::uint32_t seed, std::uint32_t channel) {
    return stamp_noise(index, seed, channel) * 2.0f - 1.0f;
}

// Steady stroke: the emission point trails the samples by a first-order lag.
// Applied to the path before arc-length walking, so it smooths the geometry
// rather than the timing.
std::vector<StrokeSample> steady_path(const std::vector<StrokeSample>& samples, float steady) {
    if (steady <= 0.0f || samples.size() < 2) return samples;
    float lag = std::clamp(steady, 0.0f, 0.95f);
    std::vector<StrokeSample> out = samples;
    cfloat3 follower = samples[0].position;
    for (std::size_t i = 1; i < out.size(); ++i) {
        follower = follower * lag + samples[i].position * (1.0f - lag);
        out[i].position = follower;
    }
    return out;
}

// Radius before jitter: base, scaled by pressure and by the end tapers.
float stamp_radius(const StrokePreset& p, float pressure, float along) {
    float r = p.radius;
    if (p.pressure.size > 0.0f) {
        float shaped = std::pow(std::clamp(pressure, 0.0f, 1.0f), std::max(p.pressure.curve, 1e-3f));
        r *= 1.0f - p.pressure.size + p.pressure.size * shaped;
    }
    float start = std::clamp(p.taper_start, 0.0f, 1.0f);
    float end = std::clamp(p.taper_end, 0.0f, 1.0f);
    if (start + end > 1.0f) {  // overlapping tapers: split the stroke between them
        float total = start + end;
        start /= total;
        end /= total;
    }
    if (start > 0.0f && along < start) r *= along / start;
    if (end > 0.0f && along > 1.0f - end) r *= (1.0f - along) / end;
    return std::max(r, 0.0f);
}

// The preset's strength shaped by pressure, before any accumulation
// correction. What Stamp::deposit clamps, and what stamp_strength divides.
float shaped_strength(const StrokePreset& p, float pressure) {
    float s = p.strength;
    if (p.pressure.strength > 0.0f) {
        float shaped = std::pow(std::clamp(pressure, 0.0f, 1.0f), std::max(p.pressure.curve, 1e-3f));
        s *= 1.0f - p.pressure.strength + p.pressure.strength * shaped;
    }
    return s;
}

float stamp_strength(const StrokePreset& p, float pressure) {
    float s = shaped_strength(p, pressure);
    if (p.accumulation == Accumulation::Clamped) {
        // A clamped stroke reaches its strength once however many stamps
        // overlap, so divide by how many cover a point: one diameter of
        // travel fits 1/spacing of them.
        float overlap = std::max(1.0f / std::max(p.spacing, 1e-3f), 1.0f);
        s /= overlap;
    }
    return std::clamp(s, 0.0f, 1.0f);
}

// The stamp at arc-length distance `d`, interpolating the path.
struct PathPoint {
    cfloat3 position;
    cfloat3 direction;
    float pressure;
    float azimuth = 0.0f;
    float velocity = 0.0f;
};

// Interpolating an ANGLE by lerp is wrong across the wrap: 350 degrees and 10
// degrees average to 180, which points the stamp backwards. Lerp the unit
// vector instead and take the angle back, which takes the short way round by
// construction.
float lerp_angle(float a, float b, float t) {
    const float x = std::cos(a) + (std::cos(b) - std::cos(a)) * t;
    const float y = std::sin(a) + (std::sin(b) - std::sin(a)) * t;
    if (std::fabs(x) < 1e-12f && std::fabs(y) < 1e-12f) return a;  // exactly opposed
    return std::atan2(y, x);
}

PathPoint sample_path(const std::vector<StrokeSample>& path, const std::vector<float>& cumulative,
                      float d) {
    if (path.size() == 1)
        return {path[0].position, kernel::cf3(1, 0, 0), path[0].pressure, path[0].azimuth,
                path[0].velocity};
    std::size_t i = 1;
    while (i + 1 < path.size() && cumulative[i] < d) ++i;
    float span = cumulative[i] - cumulative[i - 1];
    float t = span > 1e-9f ? std::clamp((d - cumulative[i - 1]) / span, 0.0f, 1.0f) : 0.0f;
    cfloat3 a = path[i - 1].position, b = path[i].position;
    cfloat3 dir = b - a;
    float len = kernel::clength(dir);
    return {a + (b - a) * t,
            len > 1e-9f ? dir * (1.0f / len) : kernel::cf3(1, 0, 0),
            path[i - 1].pressure + (path[i].pressure - path[i - 1].pressure) * t,
            lerp_angle(path[i - 1].azimuth, path[i].azimuth, t),
            path[i - 1].velocity + (path[i].velocity - path[i - 1].velocity) * t};
}

// Rotation taking +X onto `dir`, so a stamp can follow the path.
math::Quat align_x_to(cfloat3 dir) {
    cfloat3 x = kernel::cf3(1, 0, 0);
    float d = kernel::cdot(x, dir);
    if (d > 0.999999f) return math::Quat::identity();
    if (d < -0.999999f) return math::Quat{0.0f, 0.0f, 1.0f, 0.0f};  // 180 deg about Z
    cfloat3 axis = kernel::ccross(x, dir);
    float s = std::sqrt((1.0f + d) * 2.0f);
    math::Quat q{axis.x / s, axis.y / s, axis.z / s, s * 0.5f};
    return q.normalized();
}

// Gate a stamp on a mask, exactly as the voxel brushes do. Returns false when
// the stamp is fully masked and should not be emitted at all.
bool gate(const voxel::MaskField* mask, Stamp& s) {
    if (!mask) return true;
    float m = mask->sample(s.position);
    if (m >= 1.0f) return false;
    s.strength *= 1.0f - m;
    s.deposit *= 1.0f - m;
    return s.strength > 0.0f;
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void put_f32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

struct Reader {
    const std::uint8_t* p;
    std::size_t remaining;
    bool ok = true;
    std::uint16_t u16() {
        if (remaining < 2) return (ok = false), 0;
        std::uint16_t v = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        p += 2;
        remaining -= 2;
        return v;
    }
    std::uint32_t u32() {
        if (remaining < 4) return (ok = false), 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
        p += 4;
        remaining -= 4;
        return v;
    }
    float f32() {
        std::uint32_t bits = u32();
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
};

}  // namespace

// -- presets -----------------------------------------------------------------

std::vector<std::uint8_t> StrokePreset::serialize() const {
    std::vector<std::uint8_t> out;
    put_u16(out, kPresetVersion);
    put_f32(out, spacing);
    put_f32(out, pressure.size);
    put_f32(out, pressure.strength);
    put_f32(out, pressure.curve);
    put_f32(out, jitter_position);
    put_f32(out, jitter_size);
    put_f32(out, jitter_rotation);
    put_u32(out, seed);
    out.push_back(rotate_along_stroke ? 1 : 0);
    put_f32(out, taper_start);
    put_f32(out, taper_end);
    put_f32(out, steady);
    out.push_back(static_cast<std::uint8_t>(accumulation));
    put_f32(out, radius);
    put_f32(out, strength);
    // -- version 2 ----------------------------------------------------------
    out.push_back(rotate_to_azimuth ? 1 : 0);
    put_f32(out, velocity_response.size);
    put_f32(out, velocity_response.strength);
    put_f32(out, velocity_response.reference);
    return out;
}

std::optional<StrokePreset> StrokePreset::deserialize(const std::uint8_t* data, std::size_t size) {
    Reader r{data, size};
    std::uint16_t version = r.u16();
    if (!r.ok) return std::nullopt;
    // A newer schema is refused rather than read as a prefix: the fields we
    // would skip could change what the ones we read mean, and a preset that
    // silently means something else is exactly the failure the version is
    // here to prevent.
    if (version > kPresetVersion) return std::nullopt;

    // Version 1 is the original layout. A later version reads its own extra
    // fields here, after this block, leaving anything absent at its default.
    StrokePreset p;
    p.spacing = r.f32();
    p.pressure.size = r.f32();
    p.pressure.strength = r.f32();
    p.pressure.curve = r.f32();
    p.jitter_position = r.f32();
    p.jitter_size = r.f32();
    p.jitter_rotation = r.f32();
    p.seed = r.u32();
    if (r.remaining < 1) return std::nullopt;
    p.rotate_along_stroke = *r.p++ != 0;
    --r.remaining;
    p.taper_start = r.f32();
    p.taper_end = r.f32();
    p.steady = r.f32();
    if (r.remaining < 1) return std::nullopt;
    std::uint8_t accumulation = *r.p++;
    --r.remaining;
    if (accumulation > static_cast<std::uint8_t>(Accumulation::Clamped)) return std::nullopt;
    p.accumulation = static_cast<Accumulation>(accumulation);
    p.radius = r.f32();
    p.strength = r.f32();
    if (!r.ok) return std::nullopt;

    // -- version 2 ----------------------------------------------------------
    // A version-1 preset stops here and keeps the defaults, which are exactly
    // "speed changes nothing and the stamp follows the path" — the behaviour it
    // had when it was written. That is the whole point of reading the version
    // rather than the length.
    if (version >= 2) {
        if (r.remaining < 1) return std::nullopt;
        p.rotate_to_azimuth = *r.p++ != 0;
        --r.remaining;
        p.velocity_response.size = r.f32();
        p.velocity_response.strength = r.f32();
        p.velocity_response.reference = r.f32();
        if (!r.ok) return std::nullopt;
    }

    if (!(p.spacing > 0.0f) || !(p.radius > 0.0f)) return std::nullopt;
    return p;
}

// -- resolution --------------------------------------------------------------

std::vector<Stamp> resolve_stroke(const std::vector<StrokeSample>& samples,
                                  const StrokePreset& preset) {
    std::vector<Stamp> stamps;
    if (samples.empty() || !(preset.radius > 0.0f)) return stamps;

    std::vector<StrokeSample> path = steady_path(samples, preset.steady);

    // Arc length, so spacing depends on the path rather than on how fast the
    // finger moved across it.
    std::vector<float> cumulative(path.size(), 0.0f);
    for (std::size_t i = 1; i < path.size(); ++i)
        cumulative[i] = cumulative[i - 1] + kernel::clength(path[i].position - path[i - 1].position);
    const float length = cumulative.back();

    const float step = std::max(preset.spacing, 1e-3f) * preset.radius * 2.0f;
    // A tap, or a drag shorter than one spacing, still has to leave a mark.
    //
    // The epsilon is not cosmetic: `length` is a float sum over however many
    // samples arrived, so the same path sampled sparsely and densely lands a
    // few ulps apart. Without it the stamp count flips whenever length/step
    // is near an integer, and the same gesture would stamp differently
    // depending on the input rate — the one thing arc-length spacing exists
    // to prevent.
    const int count =
        length < step ? 1 : static_cast<int>(std::floor(length / step + 1e-3f)) + 1;

    stamps.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        float d = static_cast<float>(i) * step;
        PathPoint at = sample_path(path, cumulative, d);
        float along = length > 1e-9f ? std::clamp(d / length, 0.0f, 1.0f) : 0.0f;

        Stamp s;
        s.along = along;
        s.position = at.position;
        s.radius = stamp_radius(preset, at.pressure, along);
        s.strength = stamp_strength(preset, at.pressure);
        s.deposit = std::clamp(shaped_strength(preset, at.pressure), 0.0f, 1.0f);

        // Speed, in the same shape pressure already modulates. Signed on
        // purpose: a fast stroke is WIDER for a dry brush and THINNER for an
        // ink pen, and both are things artists ask for.
        const VelocityResponse& vr = preset.velocity_response;
        if (vr.reference > 0.0f && (vr.size != 0.0f || vr.strength != 0.0f)) {
            const float speed = std::clamp(at.velocity / vr.reference, 0.0f, 1.0f);
            if (vr.size != 0.0f) s.radius = std::max(s.radius * (1.0f + vr.size * speed), 0.0f);
            if (vr.strength != 0.0f) {
                s.strength = std::clamp(s.strength * (1.0f + vr.strength * speed), 0.0f, 1.0f);
                s.deposit = std::clamp(s.deposit * (1.0f + vr.strength * speed), 0.0f, 1.0f);
            }
        }

        // The barrel wins over the path where both are asked for: a caller that
        // set rotate_to_azimuth meant the barrel, and a stamp cannot face two
        // ways.
        if (preset.rotate_to_azimuth)
            s.rotation = align_x_to(kernel::cf3(std::cos(at.azimuth), 0.0f, std::sin(at.azimuth)));
        else if (preset.rotate_along_stroke)
            s.rotation = align_x_to(at.direction);

        auto index = static_cast<std::uint32_t>(i);
        if (preset.jitter_position > 0.0f) {
            float amp = preset.jitter_position * preset.radius;
            s.position = s.position + kernel::cf3(stamp_signed_noise(index, preset.seed, 0) * amp,
                                                  stamp_signed_noise(index, preset.seed, 1) * amp,
                                                  stamp_signed_noise(index, preset.seed, 2) * amp);
        }
        if (preset.jitter_size > 0.0f)
            s.radius = std::max(
                s.radius * (1.0f + stamp_signed_noise(index, preset.seed, 3) * preset.jitter_size),
                0.0f);
        if (preset.jitter_rotation > 0.0f) {
            float angle = stamp_signed_noise(index, preset.seed, 4) * preset.jitter_rotation;
            math::Quat spin = math::Quat::from_axis_angle(at.direction, angle);
            s.rotation = spin * s.rotation;
        }
        if (s.radius > 0.0f) stamps.push_back(s);
    }
    return stamps;
}

// -- consumers ---------------------------------------------------------------

std::size_t apply_to_grid(voxel::VoxelGrid& grid, const std::vector<Stamp>& stamps,
                          std::uint8_t index, voxel::BrushShape shape,
                          voxel::BrushFalloff falloff, const voxel::MaskField* mask) {
    const float cell = grid.voxel_size();
    std::size_t applied = 0;
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        Stamp s = stamps[i];
        // Gating happens here rather than inside BrushParams so a fully masked
        // stamp costs nothing at all, and so both consumers agree on what a
        // dropped stamp means.
        if (!gate(mask, s)) continue;
        int size = std::max(static_cast<int>(std::lround(s.radius * 2.0f / cell)), 1);
        voxel::BrushParams p;
        p.size = size;
        p.shape = shape;
        p.falloff = falloff;
        p.strength = s.strength;
        p.seed = static_cast<std::uint32_t>(i);  // per-stamp, so a stroke is not banded
        voxel::VoxelCoord at{static_cast<std::int32_t>(std::floor(s.position.x / cell)),
                             static_cast<std::int32_t>(std::floor(s.position.y / cell)),
                             static_cast<std::int32_t>(std::floor(s.position.z / cell))};
        grid.set_brush(at, p, index);
        ++applied;
    }
    return applied;
}

std::size_t apply_to_mask(voxel::MaskField& mask, const std::vector<Stamp>& stamps, float target,
                          voxel::BrushShape shape, voxel::BrushFalloff falloff) {
    const float cell = mask.cell_size();
    for (const Stamp& s : stamps) {
        voxel::BrushParams p;
        // The conversion this function exists for: a world radius becomes a
        // footprint in MASK cells, by the same rounding apply_to_grid uses for
        // voxel cells, so a stroke covers the same world region at any mask
        // resolution.
        p.size = std::max(static_cast<int>(std::lround(s.radius * 2.0f / cell)), 1);
        p.shape = shape;
        p.falloff = falloff;
        p.strength = s.strength;
        // `seed` stays 0: a mask stores the fractional weight directly rather
        // than dithering it, so there is nothing for a per-stamp seed to
        // decorrelate.
        mask.paint(s.position, p, target);
    }
    // Every stamp runs. There is no gate to drop one, unlike the other two
    // consumers, so the count is the stamp count and is returned for symmetry
    // rather than because it can differ.
    return stamps.size();
}

std::size_t apply_to_mesh(mesh::MeshSculptor& sculptor, const std::vector<Stamp>& stamps,
                          mesh::MeshBrush verb, const mesh::MeshBrushSettings& settings,
                          const voxel::MaskField* mask, mesh::VertexDeltas* deltas,
                          const MeshStrokeOptions& options) {
    if (stamps.empty() || !sculptor.valid()) return 0;

    // The mask reaches every verb here and nowhere else: one conversion from
    // the world-addressed lattice to the per-vertex weight the verbs already
    // take, so a painted mask protects polygons from all eleven of them with
    // no per-verb code.
    field::MaskGate mask_gate;
    if (mask) {
        const math::Transform to_world = options.mesh_to_world;
        mask_gate = [mask, to_world](kernel::cfloat3 p) { return mask->sample(to_world.apply(p)); };
    }

    const bool was_deferring = sculptor.defer_normals();
    sculptor.set_defer_normals(options.defer_normals);

    // GRAB is anchored and SNAKEHOOK walks. Both drag by the motion BETWEEN
    // stamps, so a stroke that stops moving stops pulling.
    const bool dragging = verb == mesh::MeshBrush::Grab || verb == mesh::MeshBrush::Snakehook;
    const bool anchored = verb == mesh::MeshBrush::Grab;

    // SNAKEHOOK re-anchors ON THE SURFACE IT IS DRAGGING, not on the cursor.
    //
    // Anchoring on the stamp position instead was the obvious reading and it
    // does not work: the vertex at the centre moves by the falloff's weight
    // rather than by the whole delta, so the surface falls behind the cursor a
    // little on every stamp, and a few stamps later the brush is beyond its own
    // radius from the mesh and stops reaching it — the tendril stops growing
    // exactly when the pull gets interesting. Anchoring on the class being
    // dragged fixes it outright: that class is at the centre, so its weight is
    // 1, so it moves by the full delta and the anchor keeps up by construction.
    std::uint32_t anchor = mesh::kNoClass;
    if (verb == mesh::MeshBrush::Snakehook) {
        anchor = settings.seed_class;
        // One linear scan per stroke when the caller had no pick to hand over.
        if (anchor >= sculptor.adjacency().class_count())
            anchor = sculptor.nearest_class(stamps.front().position);
    }

    std::size_t applied = 0;
    kernel::cfloat3 previous = stamps.front().position;
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        Stamp s = stamps[i];
        // The same early-out apply_to_grid takes, and for the same reason: a
        // stamp whose centre is frozen costs nothing rather than gathering a
        // region it is not allowed to move. Partially masked stamps still run,
        // and their vertices are weighed one by one below.
        if (mask && mask->sample(options.mesh_to_world.apply(s.position)) >= 1.0f) {
            previous = s.position;
            continue;
        }

        mesh::MeshBrushSettings stamp_settings = settings;
        stamp_settings.radius = s.radius;
        stamp_settings.strength = settings.strength * s.strength;
        if (dragging) {
            stamp_settings.direction = s.position - previous;
            if (anchored) {
                stamp_settings.center = stamps.front().position;
                stamp_settings.seed_class = settings.seed_class;
            } else {
                stamp_settings.center = sculptor.class_position(anchor);
                stamp_settings.seed_class = anchor;
            }
        } else {
            stamp_settings.center = s.position;
        }
        previous = s.position;

        if (sculptor.stamp(verb, stamp_settings, mask_gate, deltas) > 0) ++applied;
    }

    if (options.defer_normals) sculptor.flush_normals(deltas);
    sculptor.set_defer_normals(was_deferring);
    return applied;
}

std::vector<scene::Node> stamps_to_nodes(scene::SdfContent& content,
                                         const std::vector<Stamp>& stamps,
                                         const scene::Node& templ,
                                         const voxel::MaskField* mask) {
    std::vector<scene::Node> nodes;
    nodes.reserve(stamps.size());
    for (const Stamp& raw : stamps) {
        Stamp s = raw;
        // This is the whole of "freeze" for a declarative edit: a stamp in a
        // frozen region produces no item, so the region receives nothing new.
        if (!gate(mask, s)) continue;
        // An Add stamp at zero strength deposits nothing at all: its deposit
        // scales with the strength (below), and the zero-scale node that would
        // express "nothing" is degenerate, so none is authored.
        if (templ.op == scene::Op::Add && !(s.deposit > 0.0f)) continue;
        scene::Node node = templ;
        // The command vocabulary preserves ids rather than assigning them, so
        // reserve one here. Leaving it at kNoNode would have every node in the
        // stroke share id 0 and collapse into one.
        node.id = content.reserve_id();
        node.xform.position = s.position;
        node.xform.rotation = s.rotation;
        // The template's own scale is the unit the radius is expressed in, so
        // a template authored at radius 1 scales straight to the stamp's.
        node.xform.scale = s.radius;
        // A stamp's STRENGTH reaches an item only where scaling preserves what
        // the op means.
        //
        // For relief and incise, blend.k is the AMPLITUDE — how far the
        // accumulated surface moves along its own normal — and half an
        // amplitude is exactly half the displacement, so pressure and
        // accumulation mean something.
        if (node.op == scene::Op::Relief || node.op == scene::Op::Incise)
            node.blend.k = templ.blend.k * s.strength;
        // For add, the amount IS the stamp: strength scales the whole deposit.
        // Scale, blend radius and rounding shrink together (rounding is local,
        // so it follows the scale), so a half-strength stamp is a self-similar
        // half-size deposit, zero deposits nothing, and full strength is the
        // item exactly as authored — bit for bit, since x * 1.0f is x.
        //
        // The DEPOSIT channel rather than strength: overlapping unions do not
        // add up, so the clamped-accumulation division that keeps a relief
        // stroke at its strength would instead shrink every stamp of a clamped
        // add stroke. Clamped and buildup add strokes stay identical.
        if (node.op == scene::Op::Add) {
            node.xform.scale = s.radius * s.deposit;
            node.blend.k = templ.blend.k * s.deposit;
        }
        // Every other op reads blend.k as a radius, a depth or a half-thickness,
        // and has no scale that is purely an amount. Scaling either would change
        // the SHAPE rather than the amount, silently and differently per op: a
        // groove at half strength is not a shallower groove. They ignore
        // strength.
        nodes.push_back(std::move(node));
    }
    return nodes;
}

// -- snakehook ----------------------------------------------------------------

namespace {

// Cumulative arc length along the path, so the taper follows distance rather
// than sample count. A hand moves at an uneven speed, and tapering by index
// would make how fast the gesture was decide how thick the tendril is — the
// same defect the swept opcode had to avoid when distributing profiles.
std::vector<float> arc_lengths(const std::vector<kernel::cfloat3>& path) {
    std::vector<float> along(path.size(), 0.0f);
    for (std::size_t i = 1; i < path.size(); ++i)
        along[i] = along[i - 1] + kernel::clength(path[i] - path[i - 1]);
    return along;
}

}  // namespace

std::optional<scene::Node> snakehook(kernel::cfloat3 anchor, kernel::cfloat3 inward,
                                     const std::vector<kernel::cfloat3>& path,
                                     const SnakehookSettings& settings) {
    if (path.empty()) return std::nullopt;
    // The normal is still required, and still checked: a caller that cannot say
    // which way is into the body has not picked a surface, and resolving
    // anyway would quietly accept a bad pick.
    if (!(kernel::clength(inward) > 1e-6f)) return std::nullopt;
    if (!(settings.base_radius > 0.0f)) return std::nullopt;

    const std::vector<float> along = arc_lengths(path);
    const float total = along.back();
    // A tap has to leave a mark, the same rule resolve_stroke follows: a drag
    // shorter than a step is a small tendril rather than nothing.
    const float span = std::max(total, settings.base_radius * 0.25f);

    const float tip = std::max(settings.base_radius * std::clamp(settings.tip_fraction, 0.0f, 1.0f),
                               settings.min_tip_radius);
    const float curve = settings.taper_curve > 0.0f ? settings.taper_curve : 1.0f;

    scene::Node n;
    n.prim = scene::Prim::stroke();
    n.curve_tolerance = settings.tolerance > 0.0f ? settings.tolerance : 0.01f;

    // The base, at the point the user actually touched — which is not where the
    // first drag sample is: a pick reports the surface, and the first sample
    // arrives a frame later with the finger already moving.
    scene::StrokePoint root;
    root.pos = anchor;
    root.radius = settings.base_radius;
    root.type = scene::StrokePointType::Spline;
    n.stroke.push_back(root);

    for (std::size_t i = 0; i < path.size(); ++i) {
        scene::StrokePoint p;
        p.pos = path[i];
        const float t = std::clamp(along[i] / span, 0.0f, 1.0f);
        p.radius = tip + (settings.base_radius - tip) * std::pow(1.0f - t, curve);
        p.type = scene::StrokePointType::Spline;
        n.stroke.push_back(p);
    }
    return n;
}

}  // namespace brush
}  // namespace clay
