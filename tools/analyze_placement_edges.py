"""Edge analysis of grass placement exports (FasterNGIO --export-blades).

Usage: python analyze_placement_edges.py <min cell x> <min cell y> <cells per side> [null] <export.bin>...
  e.g. FasterNGIO ... --radius 4 -3 8 --export-blades vanilla.bin
       python analyze_placement_edges.py -4 -11 17 null vanilla.bin smooth.bin
'null' synthesises grid-free blades over the region as a control: its enrichments are ~1.

Reported per export: how much more often than chance strong edges lie on the 128-unit vertex grid,
the 512-unit patch grid, quadrant borders and cell borders, how often corners sit on patch-grid
nodes, and how axis-aligned edges are (1 = isotropic).

Each grass type's blades are rasterised, smoothed into a coverage field (0..1 of that type's full
density), and run through Sobel + non-maximum suppression. Statistics are aggregated over types,
weighted by edge strength, and compared with what an isotropic, grid-free edge set would give.
"""
import sys
import numpy as np
from scipy import ndimage

PIXEL = 32.0           # game units per pixel
SIGMA_UNITS = 64.0     # smoothing of blade counts into a density estimate
CELL = 4096.0


def load(path):
    return np.fromfile(path, dtype=np.dtype([('x', '<f4'), ('y', '<f4'), ('g', '<u4')]))


def coverage_fields(blades, origin, size):
    fields = {}
    for g in np.unique(blades['g']):
        sel = blades[blades['g'] == g]
        if len(sel) < 2000:
            continue  # too sparse to estimate edges
        ix = ((sel['x'] - origin[0]) / PIXEL).astype(np.int64)
        iy = ((sel['y'] - origin[1]) / PIXEL).astype(np.int64)
        ok = (ix >= 0) & (iy >= 0) & (ix < size) & (iy < size)
        counts = np.zeros((size, size), np.float64)
        np.add.at(counts, (iy[ok], ix[ok]), 1.0)
        density = ndimage.gaussian_filter(counts, SIGMA_UNITS / PIXEL)
        full = np.percentile(density[density > 0], 95)
        fields[int(g)] = np.clip(density / full, 0.0, 1.5)
    return fields


def thin_edges(field):
    gx = ndimage.sobel(field, axis=1) / 8.0   # per pixel
    gy = ndimage.sobel(field, axis=0) / 8.0
    mag = np.hypot(gx, gy)
    angle = np.arctan2(gy, gx)
    # Non-maximum suppression along the gradient (4 directions).
    q = (np.round(angle / (np.pi / 4)) % 4).astype(np.int8)
    offsets = {0: (0, 1), 1: (1, 1), 2: (1, 0), 3: (1, -1)}
    keep = np.zeros_like(mag, bool)
    for d, (dy, dx) in offsets.items():
        a = np.roll(mag, (dy, dx), (0, 1))
        b = np.roll(mag, (-dy, -dx), (0, 1))
        keep |= (q == d) & (mag >= a) & (mag >= b)
    # A strong edge: coverage changes by at least ~half its range across ~2 smoothing widths.
    strong = keep & (mag > 0.5 / (2.0 * SIGMA_UNITS / PIXEL))
    return gx, gy, mag, angle, strong


def harris(field):
    gx = ndimage.sobel(field, axis=1) / 8.0
    gy = ndimage.sobel(field, axis=0) / 8.0
    s = 1.5
    sxx = ndimage.gaussian_filter(gx * gx, s)
    syy = ndimage.gaussian_filter(gy * gy, s)
    sxy = ndimage.gaussian_filter(gx * gy, s)
    r = sxx * syy - sxy * sxy - 0.05 * (sxx + syy) ** 2
    peaks = (r == ndimage.maximum_filter(r, size=5)) & (r > 2e-4)
    return peaks


def grid_distance(coord, pitch):
    m = np.mod(coord, pitch)
    return np.minimum(m, pitch - m)


def analyze(path, origin, size):
    blades = load(path)
    fields = coverage_fields(blades, origin, size)
    ys, xs = np.mgrid[0:size, 0:size]
    wx = origin[0] + (xs + 0.5) * PIXEL
    wy = origin[1] + (ys + 0.5) * PIXEL
    border = int(3 * SIGMA_UNITS / PIXEL) + 2

    weights, angles, across = [], [], []
    corners_total, corners_on_patch = 0, 0
    edge_length = 0.0
    for g, field in fields.items():
        gx, gy, mag, angle, strong = thin_edges(field)
        strong[:border, :] = strong[-border:, :] = False
        strong[:, :border] = strong[:, -border:] = False
        w = mag[strong]
        weights.append(w)
        angles.append(angle[strong])
        # The coordinate across the edge: x for edges whose gradient is mostly horizontal.
        horizontal = np.abs(gx[strong]) >= np.abs(gy[strong])
        across.append(np.where(horizontal, wx[strong], wy[strong]))
        edge_length += strong.sum() * PIXEL
        peaks = harris(field)
        peaks[:border, :] = peaks[-border:, :] = False
        peaks[:, :border] = peaks[:, -border:] = False
        corners_total += peaks.sum()
        on_node = (grid_distance(wx[peaks], 512.0) <= 48.0) & (grid_distance(wy[peaks], 512.0) <= 48.0)
        corners_on_patch += on_node.sum()

    w = np.concatenate(weights)
    a = np.concatenate(angles)
    c = np.concatenate(across)
    # Orientation: angle of the gradient folded to [0, 90) degrees.
    fold = np.degrees(np.mod(a, np.pi / 2))
    axis_dev = np.minimum(fold, 90.0 - fold)
    aligned = (w * (axis_dev <= 7.5)).sum() / w.sum()
    result = {
        'blades': len(blades),
        'types': len(fields),
        'edge_km': edge_length / 70.0 / 1000.0,  # ~70 units per metre
        'axis_aligned_fraction': aligned,
        'axis_alignment_index': aligned / (15.0 / 90.0),
    }
    # Chance rates come from the pixel lattice itself (pixel centres sit at 16 mod 32 units).
    lattice = origin[0] + (np.arange(border, size - border) + 0.5) * PIXEL
    def on_lines(coord, pitch, tol):
        near = grid_distance(coord, pitch) <= tol
        if pitch == 2048.0:  # quadrant lines only, not the cell lines among them
            near &= grid_distance(coord, 4096.0) > tol
        return near

    for pitch, tol in ((128.0, 16.0), (512.0, 32.0), (2048.0, 32.0), (4096.0, 32.0)):
        near = (w * on_lines(c, pitch, tol)).sum() / w.sum()
        chance = on_lines(lattice, pitch, tol).mean()
        result[f'on_{int(pitch)}_grid'] = near
        result[f'on_{int(pitch)}_grid_enrichment'] = near / chance
    result['corners_per_km_edge'] = corners_total / (edge_length / 70.0 / 1000.0)
    result['corners_on_patch_nodes'] = corners_on_patch / max(corners_total, 1)
    node_chance = (grid_distance(lattice, 512.0) <= 48.0).mean() ** 2
    result['corners_on_patch_nodes_enrichment'] = result['corners_on_patch_nodes'] / node_chance
    # Sharpness: the width a full 0-to-1 coverage transition would take at the edge's slope.
    result['median_transition_width_units'] = PIXEL / np.median(w)
    hist, _ = np.histogram(axis_dev, bins=9, range=(0, 45), weights=w)
    result['orientation_hist'] = hist / hist.sum()
    return result, fields


def synthesize_null(path, origin, size, seed=7):
    # Blades from a random, isotropic, grid-free coverage field: several "grass types", each a
    # thresholded Gaussian random field, sampled on a jittered 24-unit lattice.
    rng = np.random.default_rng(seed)
    records = []
    for g in range(8):
        noise = ndimage.gaussian_filter(rng.standard_normal((size, size)), 900.0 / PIXEL)
        noise = (noise - noise.mean()) / noise.std()
        coverage = np.clip((noise - rng.uniform(-0.5, 0.5)) * 2.0 + 0.5, 0.0, 1.0)
        n = int(size * PIXEL / 24.0)
        gx, gy = np.meshgrid(np.arange(n), np.arange(n))
        x = (gx + rng.random(gx.shape)) * 24.0
        y = (gy + rng.random(gy.shape)) * 24.0
        cov = coverage[np.clip((y / PIXEL).astype(int), 0, size - 1), np.clip((x / PIXEL).astype(int), 0, size - 1)]
        keep = rng.random(cov.shape) < cov * 0.8
        rec = np.zeros(keep.sum(), dtype=np.dtype([('x', '<f4'), ('y', '<f4'), ('g', '<u4')]))
        rec['x'] = x[keep] + origin[0]
        rec['y'] = y[keep] + origin[1]
        rec['g'] = g
        records.append(rec)
    np.concatenate(records).tofile(path)


def render(fields, path):
    from PIL import Image
    total = sum(fields.values())
    img = np.clip(total / np.percentile(total, 99), 0, 1)
    Image.fromarray((img[::-1] * 255).astype(np.uint8)).save(path)


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    cell_x, cell_y, side = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    origin = (cell_x * CELL, cell_y * CELL)
    size = int(side * CELL / PIXEL)
    for path in sys.argv[4:]:
        if path == 'null':
            path = 'null.bin'
            synthesize_null(path, origin, size)
        res, fields = analyze(path, origin, size)
        render(fields, path.rsplit('.', 1)[0] + '.png')
        print(f'== {path}')
        for k, v in res.items():
            if isinstance(v, np.ndarray):
                print(f'  {k}: ' + ' '.join(f'{x:.3f}' for x in v))
            elif isinstance(v, float):
                print(f'  {k}: {v:.3f}')
            else:
                print(f'  {k}: {v}')
