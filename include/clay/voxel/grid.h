#pragma once

// Colored voxel engine (voxel-engine spec): palette-indexed dense chunks,
// editing ops (brushes, fills, mirror, flood select), build-plane queries,
// palette+RLE serialization, greedy meshing, and voxel<->SDF bridges.
//
// Voxel addressing is integer lattice cells; cell (x,y,z) occupies world
// space [x,x+1)*voxel_size. Palette index 0 means empty; indices 1..255
// reference the palette color table (editing the palette recolors every
// referencing voxel with no voxel-data touch).

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/math/geom.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace voxel {

inline constexpr int kChunkDim = 32;  // 32^3 voxels = 32 KiB per chunk

struct VoxelCoord {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const VoxelCoord&) const = default;
};

struct VoxelCoordHash {
    std::size_t operator()(const VoxelCoord& c) const {
        std::uint64_t h = static_cast<std::uint32_t>(c.x) * 0x9E3779B185EBCA87ull;
        h ^= static_cast<std::uint32_t>(c.y) * 0xC2B2AE3D27D4EB4Full + (h << 6);
        h ^= static_cast<std::uint32_t>(c.z) * 0x165667B19E3779F9ull + (h >> 3);
        return static_cast<std::size_t>(h);
    }
};

inline constexpr std::uint8_t kVoxMirrorX = 1;
inline constexpr std::uint8_t kVoxMirrorY = 2;
inline constexpr std::uint8_t kVoxMirrorZ = 4;

// Brush footprint. Sphere is the ball of diameter n, so it is always a subset
// of Cube for a given size, and its occupancy ratio approaches pi/6.
enum class BrushShape : std::uint8_t { Cube = 0, Sphere = 1 };

// Falloff curve over the normalized distance from the footprint centre.
// Constant is the hard-edged brush and stays the default.
enum class BrushFalloff : std::uint8_t {
    Constant = 0,
    Linear = 1,
    Smooth = 2,  // smoothstep
    Gaussian = 3,
};

// One brush stamp. Occupancy is binary, so a weight strictly between 0 and 1
// cannot be stored in a cell; it is resolved by dithering against a hash of
// the cell coordinate and `seed`. Weight 1 always applies and weight 0 never
// does, so the falloff shows up as fractional coverage across the footprint
// rather than as partial density in any one cell. The hash makes that
// reproducible: same stamp, same seed, same cells, on every platform.
struct BrushParams {
    int size = 3;
    BrushShape shape = BrushShape::Cube;
    BrushFalloff falloff = BrushFalloff::Constant;
    float strength = 1.0f;
    std::uint32_t seed = 0;
};

class VoxelGrid {
  public:
    explicit VoxelGrid(float voxel_size = 0.1f) : voxel_size_(voxel_size) {
        palette_.resize(1, kernel::cf3(0, 0, 0));  // index 0 = empty, unused
    }

    float voxel_size() const { return voxel_size_; }

    // -- palette -------------------------------------------------------------
    // Returns the palette index for a color, adding it when new (up to 255).
    std::uint8_t palette_add(kernel::cfloat3 color, float tolerance = 1e-3f);
    kernel::cfloat3 palette_color(std::uint8_t index) const;
    void palette_set(std::uint8_t index, kernel::cfloat3 color);
    std::size_t palette_size() const { return palette_.size(); }

    // -- single-voxel ops ----------------------------------------------------
    std::uint8_t get(VoxelCoord c) const;
    void set(VoxelCoord c, std::uint8_t index);            // index 0 erases
    void erase(VoxelCoord c) { set(c, 0); }
    void paint(VoxelCoord c, std::uint8_t index);          // occupied cells only

    // -- brushes and fills ---------------------------------------------------
    // Footprint of size n centered on c, as a solid cube or as the sphere of
    // the same diameter. Size n spans exactly n cells per axis for every n:
    // the footprint runs -((n-1)/2) ..= n/2, symmetric for odd n and biased
    // half a cell toward +XYZ for even n. The sphere admits cells whose
    // centre is within radius n/2 of the footprint centre.
    void set_brush(VoxelCoord c, int n, std::uint8_t index,
                   BrushShape shape = BrushShape::Cube);
    void erase_brush(VoxelCoord c, int n, BrushShape shape = BrushShape::Cube) {
        set_brush(c, n, 0, shape);
    }
    void paint_brush(VoxelCoord c, int n, std::uint8_t index,
                     BrushShape shape = BrushShape::Cube);
    void fill_box(VoxelCoord a, VoxelCoord b, std::uint8_t index);  // inclusive corners
    void fill_line(VoxelCoord a, VoxelCoord b, std::uint8_t index); // 3D DDA

    // Falloff-weighted forms of the same three brushes.
    void set_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index);
    void erase_brush(VoxelCoord c, const BrushParams& p) { set_brush(c, p, 0); }
    void paint_brush(VoxelCoord c, const BrushParams& p, std::uint8_t index);

    // -- sculpting verbs -----------------------------------------------------
    // These reshape existing material instead of stamping a footprint. Each
    // reads a snapshot of the region first, so a cell's outcome never depends
    // on a neighbour the same call already changed.

    // Majority filter over the 26-neighbourhood: spurs dissolve, notches fill.
    void sculpt_smooth(VoxelCoord c, const BrushParams& p);
    // amount > 0 dilates, < 0 erodes, |amount| times.
    void sculpt_inflate(VoxelCoord c, const BrushParams& p, int amount = 1);
    // Pull the surface onto the plane through the brush centre (offset in
    // cells along `normal`): material on the + side goes, hollows on the -
    // side that touch material are filled.
    void sculpt_flatten(VoxelCoord c, const BrushParams& p, kernel::cfloat3 normal,
                        float offset_cells = 0.0f);
    // Move surface cells one step toward the brush centre.
    void sculpt_pinch(VoxelCoord c, const BrushParams& p);

    // Grab: translate occupancy through the same inverse map the SDF grab
    // deformer uses, so both representations mean the same thing. Occupancy is
    // binary, so this resamples nearest-cell — a displacement larger than a
    // cell moves material in whole cells rather than flowing.
    void sculpt_grab(VoxelCoord c, const BrushParams& p, kernel::cfloat3 displacement,
                     bool front_only = false);

    // -- mirror --------------------------------------------------------------
    // Mirror plane passes through lattice coordinate 0 on each active axis:
    // cell x reflects to -1-x. Ops with `axes` apply the edit and every
    // mirror combination in one call.
    static VoxelCoord mirrored(VoxelCoord c, std::uint8_t axes);
    void set_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes);
    void paint_mirrored(VoxelCoord c, std::uint8_t index, std::uint8_t axes);

    // -- queries -------------------------------------------------------------
    std::size_t occupied_count() const;
    // inclusive cell-index bounds of occupied voxels (nullopt when empty)
    std::optional<VoxelCoord> bounds_min() const;
    std::optional<VoxelCoord> bounds_max() const;

    // Cell a world-space ray enters at build-plane height y = plane_cell
    // (the cell whose bottom face lies on that lattice plane).
    std::optional<VoxelCoord> build_plane_pick(const math::Ray& ray, std::int32_t plane_cell) const;

    // 6-connected flood select over same palette index (same_color=true) or
    // any occupied voxel. Bounded by max_count.
    std::vector<VoxelCoord> flood_select(VoxelCoord seed, bool same_color,
                                         std::size_t max_count = 1 << 20) const;

    // -- serialization (palette + RLE, deterministic) ------------------------
    std::vector<std::uint8_t> serialize() const;
    static std::optional<VoxelGrid> deserialize(const std::uint8_t* data, std::size_t size);

    // -- greedy meshing ------------------------------------------------------
    // Merged quads per axis slice, palette color per face, no color bleed
    // across merged faces; emits two triangles per quad with face normals.
    mesh::Mesh mesh_greedy() const;

    // -- voxel <-> SDF bridges -----------------------------------------------
    // Step-function field: -voxel_size/2 inside occupied cells, +voxel_size/2
    // elsewhere (a bound, not a distance — classify accordingly).
    float sample_step_field(kernel::cfloat3 world_p) const;

    // Rasterize an SDF tape into the grid over a world region: cells whose
    // center evaluates inside are set, colored from the tape's color field
    // via nearest palette entry (added as needed).
    void rasterize_tape(const scene::Tape& tape, const math::Aabb& world_region);

  private:
    struct Chunk {
        std::vector<std::uint8_t> data;  // kChunkDim^3
        int occupied = 0;
    };
    static VoxelCoord chunk_key(VoxelCoord c);
    static std::size_t chunk_offset(VoxelCoord c);
    void emit_quad(mesh::Mesh& out, int axis, int sign, int a, int u, int v, int w, int h,
                   std::uint8_t idx) const;

    float voxel_size_;
    std::vector<kernel::cfloat3> palette_;
    std::unordered_map<VoxelCoord, Chunk, VoxelCoordHash> chunks_;
};

}  // namespace voxel
}  // namespace clay
