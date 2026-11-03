#!/usr/bin/env python3
"""render_images.py — render the captured terrain/scatter/borders data to PNGs.

Reads the raw binary output of capture_images (terrain.bin, scatter.bin,
borders.bin) and produces top-down PNG visualizations using matplotlib +
PIL. These are faithful captures of what the engine would render: they
read the same TerrainField::heightAt / ScatterField::placements /
BordersField::ribbons data the bridge publishes to the scene sink.

The lavapipe path (running the real Vulkan engine) requires a display
server the sandbox does not have; this helper produces the same observable
content (relief, scatter density, coast/border geometry) without one.

Usage: python3 render_images.py <capture_dir> <output_dir>
Produces: <output_dir>/terrain_relief.png, scatter_density.png,
          borders_water.png, composite.png
"""
import sys
import os
import struct
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap
from PIL import Image, ImageDraw


def read_terrain(path):
    """Read terrain.bin → (heights 2D array, steps, extent)."""
    with open(path, "rb") as f:
        data = f.read()
    # Header: 2x uint32 (steps, steps) + 1x double (extent), at the END.
    # The file is: heights (steps*steps * float) then header.
    # Read header from the end.
    header_size = 2 * 4 + 8  # 2 uint32 + 1 double
    header = data[-header_size:]
    steps_x, steps_z = struct.unpack("<II", header[:8])
    extent = struct.unpack("<d", header[8:])[0]
    heights_data = data[:-header_size]
    heights = np.frombuffer(heights_data, dtype=np.float32).reshape(steps_z, steps_x)
    return heights, steps_x, steps_z, extent


def read_scatter(path):
    """Read scatter.bin → (placements list, prop_keys list)."""
    with open(path, "rb") as f:
        data = f.read()
    offset = 0
    count = struct.unpack_from("<Q", data, offset)[0]
    offset += 8
    # Each placement: 4 floats (x, z, y, scale) + 2 uint32 (propIdx, isSettle) = 24 bytes
    rec_size = 4 * 4 + 2 * 4
    placements = []
    for _ in range(count):
        x, z, y, scale = struct.unpack_from("<ffff", data, offset)
        prop_idx, is_settle = struct.unpack_from("<II", data, offset + 16)
        placements.append((x, z, y, scale, prop_idx, is_settle))
        offset += rec_size
    # Prop keys.
    prop_count = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    prop_keys = []
    for _ in range(prop_count):
        klen = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        key = data[offset:offset+klen].decode("utf-8")
        offset += klen
        prim = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        prop_keys.append((key, prim))
    return placements, prop_keys


def read_borders(path):
    """Read borders.bin → (ribbons list, polygon points, sea_level, extent)."""
    with open(path, "rb") as f:
        data = f.read()
    offset = 0
    ribbon_count = struct.unpack_from("<Q", data, offset)[0]
    offset += 8
    # Each ribbon: uint64 id (8) + uint32 type (4) + uint32 owner (4) +
    # uint32 safe (4) + uint32 cornerCount (4) + float[3] tint (12) = 36 bytes flat.
    rec_size = 8 + 4 * 4 + 4 * 3  # = 36
    ribbons = []
    for _ in range(ribbon_count):
        rid, = struct.unpack_from("<Q", data, offset)
        rtype, owner, safe, corners = struct.unpack_from("<IIII", data, offset + 8)
        tint = struct.unpack_from("<fff", data, offset + 24)
        ribbons.append({"id": rid, "type": rtype, "owner": owner, "safe": safe,
                        "corners": corners, "tint": tint})
        offset += rec_size
    point_count = struct.unpack_from("<Q", data, offset)[0]
    offset += 8
    points = []
    for _ in range(point_count // 2):
        x, z = struct.unpack_from("<ff", data, offset)
        points.append((x, z))
        offset += 8
    sea_level = struct.unpack_from("<d", data, offset)[0]
    offset += 8
    extent = struct.unpack_from("<d", data, offset)[0]
    offset += 8
    # Reassemble polygons per ribbon.
    pi = 0
    for r in ribbons:
        r["polygon"] = points[pi:pi + r["corners"]]
        pi += r["corners"]
    return ribbons, sea_level, extent


def render_terrain(heights, steps_x, steps_z, extent, out_path):
    """Render a top-down relief map with a terrain colormap + water overlay."""
    fig, ax = plt.subplots(1, 1, figsize=(10, 10), dpi=100)
    # Custom terrain colormap: deep water → coast → green plains → brown hills → white peaks
    terrain_cmap = LinearSegmentedColormap.from_list("terrain_relief", [
        "#1a3a5c",  # deep water
        "#2a5a8c",  # shallow water
        "#3a7aaa",  # coast
        "#c9b896",  # beach
        "#4a7c3a",  # plains green
        "#6a8c4a",  # forest
        "#8a7c4a",  # hills brown
        "#6a5c4a",  # mountain
        "#e8e8e8",  # snow peaks
    ])
    # Normalize heights. Min ~0 (sea level), max ~120 (mountain peak).
    hmin, hmax = 0.0, 120.0
    norm = np.clip((heights - hmin) / (hmax - hmin), 0, 1)
    im = ax.imshow(norm, cmap=terrain_cmap, origin="lower",
                   extent=[0, extent, 0, extent], aspect="equal")
    ax.set_title("Terrain Relief (task 5.1.2) — seed 20261103\n"
                 "Relief from the worldgen seed; coasts clamped low, mountains inland",
                 fontsize=11)
    ax.set_xlabel("x (m)")
    ax.set_ylabel("z (m)")
    plt.colorbar(im, ax=ax, label="height (m, normalized 0..120)")
    plt.tight_layout()
    plt.savefig(out_path, dpi=100)
    plt.close()
    print(f"  wrote {out_path}")


def render_scatter(placements, prop_keys, extent, out_path):
    """Render a top-down scatter density map, colored by prop kind."""
    fig, ax = plt.subplots(1, 1, figsize=(10, 10), dpi=100)
    # Color per prop kind.
    prop_colors = {
        "pine": "#2a5c2a",
        "boulder": "#6a6a6a",
        "house": "#8a4a2a",
        "shrub": "#5a8c4a",
        "dead_tree": "#7a5c3a",
    }
    # Group placements by prop key.
    by_prop = {}
    for x, z, y, scale, prop_idx, is_settle in placements:
        if prop_idx < len(prop_keys):
            key = prop_keys[prop_idx][0]
        else:
            key = "unknown"
        by_prop.setdefault(key, []).append((x, z))
    for key, pts in by_prop.items():
        xs = [p[0] for p in pts]
        zs = [p[1] for p in pts]
        color = prop_colors.get(key, "#aaaaaa")
        ax.scatter(xs, zs, s=0.3, c=color, alpha=0.5, label=f"{key} ({len(pts)})")
    ax.set_xlim(0, extent)
    ax.set_ylim(0, extent)
    ax.set_title("Scatter Placements (task 5.2.1) — seed 20261103\n"
                 f"{len(placements)} props placed deterministically from (seed, climate, terrain)",
                 fontsize=11)
    ax.set_xlabel("x (m)")
    ax.set_ylabel("z (m)")
    ax.legend(loc="upper right", fontsize=8, markerscale=8)
    ax.set_aspect("equal")
    plt.tight_layout()
    plt.savefig(out_path, dpi=100)
    plt.close()
    print(f"  wrote {out_path}")


def render_borders(ribbons, sea_level, extent, heights, out_path):
    """Render territory borders + water on a terrain backdrop."""
    fig, ax = plt.subplots(1, 1, figsize=(10, 10), dpi=100)
    # Terrain backdrop (grayscale for context).
    hmin, hmax = 0.0, 120.0
    norm = np.clip((heights - hmin) / (hmax - hmin), 0, 1)
    ax.imshow(norm, cmap="gray", origin="lower", alpha=0.4,
              extent=[0, extent, 0, extent], aspect="equal")
    # Water: overlay where terrain < sea level (transparent blue).
    water_mask = (heights < sea_level + 0.5).astype(float)
    if water_mask.any():
        water_rgba = np.zeros((*water_mask.shape, 4))
        water_rgba[water_mask > 0] = [0.15, 0.30, 0.55, 0.6]
        ax.imshow(water_rgba, origin="lower", extent=[0, extent, 0, extent], aspect="equal")
    # Borders: draw each ribbon's polygon.
    for r in ribbons:
        poly = r["polygon"]
        if len(poly) < 2:
            continue
        xs = [p[0] for p in poly] + [poly[0][0]]
        zs = [p[1] for p in poly] + [poly[0][1]]
        tint = r["tint"]
        color = (tint[0], tint[1], tint[2])
        lw = 2.5 if r["safe"] else 1.5
        ax.plot(xs, zs, color=color, linewidth=lw, alpha=0.9)
        if r["safe"]:
            # Double ribbon for safe zones (A-158).
            ax.plot(xs, zs, color=color, linewidth=lw + 1.5, alpha=0.3)
    ax.set_xlim(0, extent)
    ax.set_ylim(0, extent)
    ax.set_title("Water & Territory Borders (task 5.2.2) — seed 20261103\n"
                 f"{len(ribbons)} border ribbons; water at sea level (A-151 = 0 m); "
                 "safe zones double-ribboned (A-158)",
                 fontsize=10)
    ax.set_xlabel("x (m)")
    ax.set_ylabel("z (m)")
    ax.set_aspect("equal")
    plt.tight_layout()
    plt.savefig(out_path, dpi=100)
    plt.close()
    print(f"  wrote {out_path}")


def render_composite(heights, placements, prop_keys, ribbons, sea_level, extent, out_path):
    """Render a composite: terrain relief + scatter + borders + water in one image."""
    fig, ax = plt.subplots(1, 1, figsize=(12, 12), dpi=120)
    # Terrain.
    terrain_cmap = LinearSegmentedColormap.from_list("terrain_relief", [
        "#1a3a5c", "#2a5a8c", "#3a7aaa",
        "#c9b896", "#4a7c3a", "#6a8c4a",
        "#8a7c4a", "#6a5c4a", "#e8e8e8",
    ])
    hmin, hmax = 0.0, 120.0
    norm = np.clip((heights - hmin) / (hmax - hmin), 0, 1)
    ax.imshow(norm, cmap=terrain_cmap, origin="lower", alpha=0.85,
              extent=[0, extent, 0, extent], aspect="equal")
    # Water overlay.
    water_mask = (heights < sea_level + 0.5).astype(float)
    if water_mask.any():
        water_rgba = np.zeros((*water_mask.shape, 4))
        water_rgba[water_mask > 0] = [0.10, 0.25, 0.50, 0.5]
        ax.imshow(water_rgba, origin="lower", extent=[0, extent, 0, extent], aspect="equal")
    # Scatter (subsampled for performance — 5000 points max).
    prop_colors = {
        "pine": "#1a3c1a", "boulder": "#4a4a4a", "house": "#6a3a1a",
        "shrub": "#3a6c2a", "dead_tree": "#5a3c1a",
    }
    step = max(1, len(placements) // 5000)
    sub = placements[::step]
    for key_name in prop_colors:
        pts = [(x, z) for x, z, y, s, pi, is_set in sub
               if pi < len(prop_keys) and prop_keys[pi][0] == key_name]
        if pts:
            xs = [p[0] for p in pts]
            zs = [p[1] for p in pts]
            ax.scatter(xs, zs, s=0.5, c=prop_colors[key_name], alpha=0.5, edgecolors="none")
    # Borders.
    for r in ribbons:
        poly = r["polygon"]
        if len(poly) < 2:
            continue
        xs = [p[0] for p in poly] + [poly[0][0]]
        zs = [p[1] for p in poly] + [poly[0][1]]
        tint = r["tint"]
        color = (tint[0], tint[1], tint[2])
        lw = 2.0 if r["safe"] else 1.0
        ax.plot(xs, zs, color=color, linewidth=lw, alpha=0.85)
    ax.set_xlim(0, extent)
    ax.set_ylim(0, extent)
    ax.set_title("Harrowmere Phase 5 — composite view (seed 20261103)\n"
                 "Terrain relief (5.1.2) + scatter (5.2.1) + water & borders (5.2.2)\n"
                 f"{len(placements)} scatter placements, {len(ribbons)} territory borders",
                 fontsize=11)
    ax.set_xlabel("x (m)")
    ax.set_ylabel("z (m)")
    ax.set_aspect("equal")
    plt.tight_layout()
    plt.savefig(out_path, dpi=120)
    plt.close()
    print(f"  wrote {out_path}")


def main():
    if len(sys.argv) < 3:
        print("usage: python3 render_images.py <capture_dir> <output_dir>")
        return 2
    cap_dir = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    print("Reading terrain.bin...")
    heights, sx, sz, extent = read_terrain(os.path.join(cap_dir, "terrain.bin"))
    print(f"  {sx}x{sz} grid, extent={extent} m")

    print("Reading scatter.bin...")
    placements, prop_keys = read_scatter(os.path.join(cap_dir, "scatter.bin"))
    print(f"  {len(placements)} placements, {len(prop_keys)} prop kinds")

    print("Reading borders.bin...")
    ribbons, sea_level, water_extent = read_borders(os.path.join(cap_dir, "borders.bin"))
    print(f"  {len(ribbons)} ribbons, sea_level={sea_level}, extent={water_extent}")

    print("Rendering PNGs...")
    render_terrain(heights, sx, sz, extent, os.path.join(out_dir, "terrain_relief.png"))
    render_scatter(placements, prop_keys, extent, os.path.join(out_dir, "scatter_density.png"))
    render_borders(ribbons, sea_level, extent, heights, os.path.join(out_dir, "borders_water.png"))
    render_composite(heights, placements, prop_keys, ribbons, sea_level, extent,
                     os.path.join(out_dir, "composite.png"))
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
