"""Shared rendering helpers for the examples gallery.

Deliberately dependency-free: `pyclay`, `numpy` and the standard library only.
PNGs are written by hand (zlib + struct) so the examples run against a bare
wheel — no Pillow, no matplotlib.

The renderer is a plain sphere-traced camera using `Document.raycast_many`,
shaded with a headlight plus a fill light and a touch of rim. It is meant to
make geometry legible, not to be pretty: no shadows, no materials, no AA
beyond optional supersampling.
"""

import os
import struct
import zlib

import numpy as np

import pyclay as clay

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")

# Modest by design: the renders are committed, so they must stay small.
WIDTH, HEIGHT = 480, 360
SUPERSAMPLE = 2

# CI runs the gallery to prove every example still RUNS, and throws the images
# away afterwards (`git checkout -- examples/output`) precisely because float
# renders differ across platforms and pixel equality was never the gate. At
# full quality that costs about seventy minutes on a two-core runner, and one
# example accounts for most of it: 34_organic_character renders 560x680 with
# twelve occlusion rays per pixel, where the rest of the gallery renders about
# 205x195. Occlusion multiplies raycasts per pixel, so it dominates.
#
# CLAY_EXAMPLES_FAST scales the pixels and the occlusion rays down. It changes
# how the images LOOK and nothing about what runs, so the gate survives intact
# and the committed gallery is still regenerated at full quality by a plain
# `python examples/run_all.py`.
FAST = os.environ.get("CLAY_EXAMPLES_FAST", "") not in ("", "0")
FAST_SCALE = 0.5     # per axis, so a quarter of the pixels
FAST_AO_MAX = 3      # occlusion rays per pixel


def _fast_pixels(n):
    return max(16, int(n * FAST_SCALE)) if FAST else n


def _fast_ao(samples):
    return min(samples, FAST_AO_MAX) if FAST else samples

BACKGROUND_TOP = np.array([0.16, 0.18, 0.22], dtype=np.float32)
BACKGROUND_BOTTOM = np.array([0.05, 0.05, 0.07], dtype=np.float32)


def output_path(name):
    """Absolute path inside examples/output/, creating the directory."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    return os.path.join(OUTPUT_DIR, name)


def write_png(path, rgb):
    """Write an (H, W, 3) float array in [0, 1] as an 8-bit PNG.

    Pure stdlib: zlib for the pixel stream, struct for the chunk headers.
    """
    h, w = rgb.shape[0], rgb.shape[1]
    eight_bit = np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)

    # PNG scanlines are each prefixed with a filter byte; 0 = no filtering.
    raw = np.concatenate(
        [np.zeros((h, 1), dtype=np.uint8), eight_bit.reshape(h, w * 3)], axis=1
    ).tobytes()

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit truecolour
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)
    return path


def _normalize(v):
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


def camera_rays(eye, target, up=(0.0, 1.0, 0.0), fov_degrees=35.0,
                width=WIDTH, height=HEIGHT, supersample=SUPERSAMPLE):
    """Generate an (N, 6) origin+direction array for a pinhole camera."""
    eye = np.asarray(eye, dtype=np.float32)
    target = np.asarray(target, dtype=np.float32)

    forward = _normalize(target - eye)
    right = _normalize(np.cross(forward, np.asarray(up, dtype=np.float32)))
    true_up = np.cross(right, forward)

    w, h = width * supersample, height * supersample
    # Pixel centres in NDC, y flipped so +y is up in the image.
    xs = (np.arange(w, dtype=np.float32) + 0.5) / w * 2.0 - 1.0
    ys = 1.0 - (np.arange(h, dtype=np.float32) + 0.5) / h * 2.0
    gx, gy = np.meshgrid(xs, ys)

    aspect = w / float(h)
    scale = np.tan(np.radians(fov_degrees) * 0.5)
    dirs = (
        forward[None, None, :]
        + right[None, None, :] * (gx * aspect * scale)[:, :, None]
        + true_up[None, None, :] * (gy * scale)[:, :, None]
    )
    dirs /= np.linalg.norm(dirs, axis=2, keepdims=True)

    rays = np.empty((h * w, 6), dtype=np.float32)
    rays[:, 0:3] = eye
    rays[:, 3:6] = dirs.reshape(-1, 3)
    return rays, (h, w)


def orbit_camera(bounds, azimuth=35.0, elevation=22.0, fov_degrees=35.0, margin=1.15):
    """Eye/target that frames `bounds` — ((minx,miny,minz), (maxx,maxy,maxz)).

    Saves every example from hand-tuning a camera: the distance is derived
    from the bounding sphere and the vertical field of view.
    """
    if bounds is None:
        raise ValueError("cannot frame empty bounds (nothing was added?)")
    lo = np.asarray(bounds[0], dtype=np.float64)
    hi = np.asarray(bounds[1], dtype=np.float64)
    # Unbounded content (plane, infinite cylinder, infinite grids) reports a
    # bound near FLT_MAX. Framing that silently produces a blank image, so
    # say so instead and let the caller pass an explicit camera.
    if not np.all(np.isfinite(lo)) or not np.all(np.isfinite(hi)) or np.max(hi - lo) > 1e6:
        raise ValueError(
            "cannot auto-frame unbounded content; pass an explicit eye/target"
        )
    center = (lo + hi) * 0.5
    radius = float(np.linalg.norm(hi - lo)) * 0.5
    radius = max(radius, 1e-3)

    distance = margin * radius / np.tan(np.radians(fov_degrees) * 0.5)
    az, el = np.radians(azimuth), np.radians(elevation)
    offset = np.array(
        [np.cos(el) * np.sin(az), np.sin(el), np.cos(el) * np.cos(az)], dtype=np.float32
    )
    return tuple((center + offset * distance).tolist()), tuple(center.tolist())


def layer_camera(layer, **kwargs):
    """Frame an SDF layer using its own tight bounds."""
    return orbit_camera(layer.bounds(), **kwargs)


def voxel_camera(grid, voxel_size, **kwargs):
    """Frame a voxel grid; `VoxelGrid.bounds()` is in cells, so scale it."""
    cells = grid.bounds()
    if cells is None:
        raise ValueError("voxel grid is empty")
    lo = np.asarray(cells[0], dtype=np.float32) * voxel_size
    # bounds are inclusive cell indices: the far corner spans one more cell
    hi = (np.asarray(cells[1], dtype=np.float32) + 1.0) * voxel_size
    return orbit_camera((lo, hi), **kwargs)


def _background(shape):
    """Vertical gradient, so unlit silhouettes still read against it."""
    h, w = shape
    t = np.linspace(0.0, 1.0, h, dtype=np.float32)[:, None, None]
    column = BACKGROUND_TOP[None, None, :] * t + BACKGROUND_BOTTOM[None, None, :] * (1.0 - t)
    return np.repeat(column, w, axis=1)


def shade(hit, normal, position, view_dir, colors=None):
    """Headlight + fill + rim over a gradient background."""
    key = _normalize(np.array([0.5, 0.8, 0.6], dtype=np.float32))
    fill = _normalize(np.array([-0.6, 0.2, 0.4], dtype=np.float32))

    n = normal
    lambert = np.clip(n @ key, 0.0, 1.0)
    fill_term = np.clip(n @ fill, 0.0, 1.0) * 0.22
    ambient = 0.10

    base = np.full((n.shape[0], 3), 0.70, dtype=np.float32) if colors is None else colors
    # Kept well under 1.0 on purpose: a saturated term flattens the shape into
    # a white silhouette, which is exactly what these renders need to show.
    lit = base * (ambient + lambert[:, None] * 0.62 + fill_term[:, None])

    # Specular gives curvature a highlight to read against.
    half = key - view_dir
    half /= np.maximum(np.linalg.norm(half, axis=1, keepdims=True), 1e-8)
    spec = np.clip(np.sum(n * half, axis=1), 0.0, 1.0) ** 32
    lit += spec[:, None] * 0.18

    # Rim light: brighten where the surface turns away from the viewer.
    facing = np.clip(1.0 - np.abs(np.sum(n * view_dir, axis=1)), 0.0, 1.0)
    lit += (facing ** 4)[:, None] * 0.18

    return np.clip(lit, 0.0, 1.0)


def occlusion(doc, position, normal, samples=10, reach=0.10, seed=11):
    """Fraction of a hemisphere of short rays that hit something nearby.

    `shade` reads a surface entirely through its normal, which is enough for a
    smooth form and not enough for a hard-surface one: a panel gap has almost
    the same normal as the plate either side of it, so it lights identically
    and disappears. What separates them is that the gap is OCCLUDED — and the
    document already traces rays, so occlusion is just more rays.

    Deterministic: the gallery is committed, so the sample directions come from
    a fixed seed rather than fresh randomness.
    """
    rng = np.random.default_rng(seed)
    occ = np.zeros(len(position), dtype=np.float32)
    origin = position + normal * (reach * 0.04)
    for _ in range(samples):
        d = rng.normal(size=position.shape).astype(np.float32)
        d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-8)
        # Fold each direction into the surface's own hemisphere.
        d = np.where((np.sum(d * normal, axis=1) < 0.0)[:, None], -d, d)
        probe = np.concatenate([origin, d], axis=1).astype(np.float32)
        r = doc.raycast_many(probe)
        near = np.linalg.norm(r["position"] - origin, axis=1) < reach
        occ += (r["hit"] & near).astype(np.float32)
    return occ / float(samples)


def render_array(doc, eye=(2.6, 2.0, 3.2), target=(0.0, 0.0, 0.0),
                 fov_degrees=35.0, colors_from_field=False,
                 width=WIDTH, height=HEIGHT, supersample=SUPERSAMPLE,
                 ao=0, ao_reach=0.10, ao_strength=0.85):
    """Raycast and shade `doc`, returning the (H, W, 3) image array.

    `ao` is the number of occlusion rays per pixel, 0 to skip it. Off by
    default: it costs a raycast pass per sample, and only earns that on a model
    whose detail is cavities rather than curvature.
    """
    width, height, ao = _fast_pixels(width), _fast_pixels(height), _fast_ao(ao)
    rays, (h, w) = camera_rays(eye, target, fov_degrees=fov_degrees,
                               width=width, height=height, supersample=supersample)
    result = doc.raycast_many(rays)

    hit = result["hit"]
    image = _background((h, w)).copy()
    flat = image.reshape(-1, 3)

    if np.any(hit):
        idx = np.nonzero(hit)[0]
        normal = result["normal"][idx]
        position = result["position"][idx]
        view_dir = rays[idx, 3:6]

        surface_colors = None
        if colors_from_field:
            surface_colors = doc.colors(position)

        lit = shade(hit[idx], normal, position, view_dir, surface_colors)
        if ao:
            occ = occlusion(doc, position, normal, samples=ao, reach=ao_reach)
            lit = lit * (1.0 - ao_strength * occ)[:, None]
        flat[idx] = lit

    image = flat.reshape(h, w, 3)

    # Box-filter the supersampled buffer down to the committed resolution.
    if supersample > 1:
        s = supersample
        image = image.reshape(h // s, s, w // s, s, 3).mean(axis=(1, 3))

    # Gamma for display.
    return np.power(np.clip(image, 0.0, 1.0), 1.0 / 2.2)


def render(doc, name, eye=(2.6, 2.0, 3.2), target=(0.0, 0.0, 0.0),
           fov_degrees=35.0, colors_from_field=False, caption=None):
    """Raycast `doc`, shade it, downsample, and write examples/output/<name>.

    Returns the written path. `colors_from_field=True` shades with the
    document's own per-point colors instead of a neutral grey.
    """
    image = render_array(doc, eye=eye, target=target, fov_degrees=fov_degrees,
                         colors_from_field=colors_from_field)
    path = output_path(name)
    write_png(path, image)
    print(f"  wrote {os.path.relpath(path, os.path.dirname(OUTPUT_DIR))}"
          + (f"  ({caption})" if caption else ""))
    return path


# Entry-face ids from pick/: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z.
_FACE_NORMALS = np.array(
    [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]],
    dtype=np.float32,
)


def render_voxels_array(grid, eye=(2.6, 2.0, 3.2), target=(0.0, 0.0, 0.0),
                        fov_degrees=35.0, width=240, height=180):
    """Render a VoxelGrid via its own DDA raycast; returns the image array.

    `VoxelGrid.raycast` is one ray at a time, so this loops in Python and
    therefore renders at a lower resolution than the SDF path. The hit dict
    carries `cell` and `face`: the palette lookup gives colour, the face id
    gives an exact cube normal (no finite differences needed).
    """
    width, height = _fast_pixels(width), _fast_pixels(height)
    rays, (h, w) = camera_rays(
        eye, target, fov_degrees=fov_degrees, width=width, height=height, supersample=1
    )

    image = _background((h, w)).reshape(-1, 3).copy()
    key = _normalize(np.array([0.5, 0.8, 0.6], dtype=np.float32))
    palette_cache = {}

    for i in range(rays.shape[0]):
        hit = grid.raycast(rays[i, 0:3], rays[i, 3:6])
        if hit is None:
            continue
        index = grid.get(hit["cell"])
        if index not in palette_cache:
            palette_cache[index] = np.asarray(grid.palette_color(index), dtype=np.float32)
        color = palette_cache[index]
        normal = _FACE_NORMALS[hit["face"]]
        lambert = float(np.clip(normal @ key, 0.0, 1.0))
        image[i] = np.clip(color * (0.25 + 0.8 * lambert), 0.0, 1.0)

    return np.power(np.clip(image.reshape(h, w, 3), 0.0, 1.0), 1.0 / 2.2)


def render_voxels(grid, name, eye=(2.6, 2.0, 3.2), target=(0.0, 0.0, 0.0),
                  fov_degrees=35.0, caption=None, width=240, height=180):
    """Render a VoxelGrid and write examples/output/<name>."""
    image = render_voxels_array(grid, eye=eye, target=target,
                                fov_degrees=fov_degrees, width=width, height=height)
    path = output_path(name)
    write_png(path, image)
    print(f"  wrote {os.path.relpath(path, os.path.dirname(OUTPUT_DIR))}"
          + (f"  ({caption})" if caption else ""))
    return path


def mesh_preview_doc(mesh, cell, colour):
    """A DISPLAY-ONLY document for a mesh, shared by every example that shows one.

    A mesh layer is never evaluated, so the renderer — which raycasts a FIELD —
    has nothing to trace against. Resampling the mesh here is exactly the
    approximation a mesh layer exists to avoid, and it is fine for a picture and
    wrong for the export.

    IT REFUSES AN INSIDE-OUT MESH, and that guard is the whole reason this is
    one function rather than the nine identical copies it replaced.

    A closed mesh wound inward has a generalized winding number of -1 in its
    interior rather than +1, so `is_inside` is false everywhere and
    `Volume.from_mesh` samples a field that is POSITIVE at the centre of the
    model. There is then no zero crossing for a ray to find: the preview comes
    out as scattered speckle where a ray happened to graze the marcher's
    epsilon, and nothing anywhere says why. `66_dynamic_topology` shipped like
    that, because its assertions are about topology and never looked at the
    picture.

    Checked only on a CLOSED mesh: `signed_volume` means nothing on an open one,
    and previewing an open surface is a thing the examples legitimately do.
    """
    if mesh.is_watertight() and mesh.signed_volume < 0.0:
        raise SystemExit(
            "this mesh is inside-out: it is closed and its signed volume is "
            f"{mesh.signed_volume:.4f}, so its triangles wind INWARD. "
            "Volume.from_mesh would sample a field with no interior and the "
            "preview would render as speckle. Reverse the triangle winding at "
            "the source rather than here."
        )
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def render_mesh_array(mesh, eye=(2.6, 2.0, 3.2), target=(0.0, 0.0, 0.0),
                      width=260, height=260, colors=None):
    """Rasterise a mesh's own VERTEX COLOURS — a painter's algorithm with a
    z-buffer, barycentric interpolation per triangle, and one lambert term.

    `render_array` cannot do this and never will: it traces a DOCUMENT, and a
    mesh reaches a document through `Volume.from_mesh`, which carries one colour
    for the whole item. A picture of per-vertex colour has to come from the
    vertices. Used by the mesh colour and attribute-transfer examples.

    `colors` overrides the mesh's own, as an (N, 3) array in 0..1. A pyclay
    mesh's colour buffer is a READ-ONLY view, so an example that wants to paint
    a derived quantity over the geometry — a density map, an error map — has no
    way to put it on the mesh; this is that way. `67_voxel_remesh` colours a
    surface by its own edge lengths, which is how "the topology is uniform now"
    becomes a picture rather than a number."""
    width, height = _fast_pixels(width), _fast_pixels(height)
    p = np.asarray(mesh.positions, dtype=np.float64)
    idx = np.asarray(mesh.indices, dtype=np.int64).reshape(-1, 3)
    col = np.asarray(mesh.colors if colors is None else colors, dtype=np.float64)
    if len(col) != len(p):
        raise ValueError(
            f"need one colour per vertex: {len(col)} colours for {len(p)} vertices"
        )

    forward = np.array(target, dtype=np.float64) - np.array(eye, dtype=np.float64)
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, (0.0, 1.0, 0.0))
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)

    rel = p - np.array(eye, dtype=np.float64)
    cam = np.stack([rel @ right, rel @ up, rel @ forward], axis=1)
    depth = np.maximum(cam[:, 2], 1e-6)
    focal = 1.0 / np.tan(np.radians(35.0) * 0.5)
    sx = (cam[:, 0] / depth * focal * 0.5 + 0.5) * width
    sy = (0.5 - cam[:, 1] / depth * focal * 0.5) * height

    image = np.zeros((height, width, 3))
    image[:] = np.linspace(0.10, 0.16, height)[:, None, None]
    zbuf = np.full((height, width), np.inf)

    # Backface cull in screen space, then draw. Front faces only means the far
    # side of the ball never overwrites the near side, whatever the order.
    a, b, c = idx[:, 0], idx[:, 1], idx[:, 2]
    area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a])
    front = np.where((area < 0) & (depth[a] > 0) & (depth[b] > 0) & (depth[c] > 0))[0]

    light = np.array([0.4, 0.8, 0.5])
    light /= np.linalg.norm(light)
    # Vertex normals from the geometry itself, area-weighted, so this works on
    # any mesh rather than only on something centred at the origin.
    normals = np.zeros_like(p)
    tri = p[idx]
    fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    for k in range(3):
        np.add.at(normals, idx[:, k], fn)
    lens = np.linalg.norm(normals, axis=1, keepdims=True)
    normals = np.divide(normals, lens, out=np.zeros_like(normals), where=lens > 1e-20)
    shade = np.clip(np.abs(normals @ light), 0.0, 1.0) * 0.75 + 0.25

    for t in front:
        i, j, k = idx[t]
        xs, ys = np.array([sx[i], sx[j], sx[k]]), np.array([sy[i], sy[j], sy[k]])
        x0, x1 = int(max(0, np.floor(xs.min()))), int(min(width - 1, np.ceil(xs.max())))
        y0, y1 = int(max(0, np.floor(ys.min()))), int(min(height - 1, np.ceil(ys.max())))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        det = area[t]
        w0 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / det
        w1 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / det
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        z = w0 * depth[i] + w1 * depth[j] + w2 * depth[k]
        tile = zbuf[y0:y1 + 1, x0:x1 + 1]
        hit = inside & (z < tile)
        if not hit.any():
            continue
        rgb = (w0[..., None] * col[i] + w1[..., None] * col[j] + w2[..., None] * col[k])
        lit = (w0 * shade[i] + w1 * shade[j] + w2 * shade[k])[..., None]
        patch = image[y0:y1 + 1, x0:x1 + 1]
        patch[hit] = np.clip(rgb * lit, 0.0, 1.0)[hit]
        tile[hit] = z[hit]
    return np.clip(image, 0.0, 1.0)


def side_by_side(left, right, name, gap=12, caption=None):
    """Join two equal-height images with a thin divider between them."""
    if left.shape[0] != right.shape[0]:
        raise ValueError(
            f"side_by_side needs equal heights, got {left.shape[0]} and {right.shape[0]}"
        )
    h = left.shape[0]
    divider = np.tile(
        np.power(BACKGROUND_BOTTOM, 1.0 / 2.2)[None, None, :], (h, gap, 1)
    ).astype(np.float32)
    joined = np.concatenate([left, divider, right], axis=1)

    path = output_path(name)
    write_png(path, joined)
    print(f"  wrote {os.path.relpath(path, os.path.dirname(OUTPUT_DIR))}"
          + (f"  ({caption})" if caption else ""))
    return path


def contact_sheet(tiles, name, columns=4, caption=None):
    """Compose already-rendered (H, W, 3) tiles into one grid image."""
    if not tiles:
        raise ValueError("contact_sheet needs at least one tile")
    rows = (len(tiles) + columns - 1) // columns
    th, tw = tiles[0].shape[0], tiles[0].shape[1]
    # Fill with the background so unused cells match the tiles, not black.
    sheet = np.tile(
        np.power(BACKGROUND_BOTTOM, 1.0 / 2.2)[None, None, :],
        (rows * th, columns * tw, 1),
    ).astype(np.float32)
    for i, tile in enumerate(tiles):
        r, c = divmod(i, columns)
        sheet[r * th:(r + 1) * th, c * tw:(c + 1) * tw] = tile
    path = output_path(name)
    write_png(path, sheet)
    print(f"  wrote {os.path.relpath(path, os.path.dirname(OUTPUT_DIR))}"
          + (f"  ({caption})" if caption else ""))
    return path


def render_tile(doc, eye=(2.4, 1.9, 3.0), target=(0.0, 0.0, 0.0),
                fov_degrees=32.0, size=160, layer=None, colors_from_field=False,
                ao=0, ao_reach=0.10, ao_strength=0.85):
    """Render a small square tile for a contact sheet; returns the array.

    Pass `layer` to frame the tile automatically from that layer's bounds.
    `ao` is as render_array takes it.
    """
    if layer is not None:
        eye, target = layer_camera(layer, fov_degrees=fov_degrees)
    size, ao = _fast_pixels(size), _fast_ao(ao)
    rays, (h, w) = camera_rays(
        eye, target, fov_degrees=fov_degrees, width=size, height=size, supersample=2
    )
    result = doc.raycast_many(rays)
    hit = result["hit"]
    image = _background((h, w)).reshape(-1, 3).copy()
    if np.any(hit):
        idx = np.nonzero(hit)[0]
        position = result["position"][idx]
        normal = result["normal"][idx]
        surface_colors = doc.colors(position) if colors_from_field else None
        lit = shade(hit[idx], normal, position, rays[idx, 3:6], surface_colors)
        if ao:
            occ = occlusion(doc, position, normal, samples=ao, reach=ao_reach)
            lit = lit * (1.0 - ao_strength * occ)[:, None]
        image[idx] = lit
    image = image.reshape(h, w, 3).reshape(h // 2, 2, w // 2, 2, 3).mean(axis=(1, 3))
    return np.power(np.clip(image, 0.0, 1.0), 1.0 / 2.2)


# Committed models must stay small — this is version-controlled binary. The
# budget is deliberately tight; exceeding it fails rather than quietly
# growing the repository.
MODEL_SIZE_BUDGET = 400 * 1024


def save_model(mesh, name):
    """Save a mesh into examples/output/ and report its size."""
    path = output_path(name)
    mesh.save(path)
    size = os.path.getsize(path)
    print(f"  wrote {os.path.relpath(path, os.path.dirname(OUTPUT_DIR))}"
          f"  ({mesh.triangle_count} triangles, {size // 1024} KiB)")
    if size > MODEL_SIZE_BUDGET:
        raise SystemExit(
            f"{name} is {size // 1024} KiB, over the "
            f"{MODEL_SIZE_BUDGET // 1024} KiB budget for committed models — "
            f"lower the resolution or decimate harder"
        )
    return path


def export_model(doc, name, resolution=56, decimate=0.12, mesher="marching"):
    """Mesh a document at gallery settings and save it.

    One place to tune, so no example accidentally commits a huge model.
    Binary PLY is far more compact than ASCII OBJ for the same mesh.
    """
    mesh = doc.mesh(resolution=resolution, decimate=decimate, mesher=mesher)
    return save_model(mesh, name)


def banner(title):
    print(f"\n=== {title} ===")
