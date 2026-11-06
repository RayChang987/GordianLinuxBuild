#!/usr/bin/env python3
"""Turn the snapshots written by gordian_linux_build into an animated GIF.

    python3 scripts/visualize.py output/netlist_L            # -> output/netlist_L/placement.gif
    python3 scripts/visualize.py output/netlist_L -o demo.gif --frame-ms 500

Each frame shows the cell placement after one quadratic solve together with the
region cuts derived from it (left), and the wirelength so far (right).
"""
import argparse
import glob
import io
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402
from matplotlib.collections import LineCollection, PatchCollection  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.patches import Rectangle  # noqa: E402
from PIL import Image  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
MOVABLE = "#2a78d6"
FIXED = "#eb6834"


def read_snapshot(path):
    with open(path) as f:
        lines = [l.split() for l in f if l.strip() and not l.startswith("#")]
    it = iter(lines)
    snap = {}
    for row in it:
        key = row[0]
        if key == "frame":
            snap["frame"] = int(row[1])
        elif key == "chip":
            snap["chip"] = (float(row[1]), float(row[2]))  # (H, W)
        elif key == "wirelength":
            snap["wirelength"] = float(row[1])
        elif key == "regions":
            snap["regions"] = [tuple(map(float, next(it))) for _ in range(int(row[1]))]
        elif key == "cells":
            snap["cells"] = [
                (float(a), float(b), float(c), float(d), r[4] == "1")
                for r in (next(it) for _ in range(int(row[1])))
                for a, b, c, d in [r[:4]]
            ]
    return snap


def render(snap, history, total_frames, wl_max, title, dpi):
    H, W = snap["chip"]
    fig = plt.figure(figsize=(9.6, 5.4), dpi=dpi, facecolor=SURFACE)
    gs = fig.add_gridspec(1, 2, width_ratios=[1.15, 1], wspace=0.28,
                          left=0.06, right=0.97, top=0.84, bottom=0.12)
    ax = fig.add_subplot(gs[0])
    wl = fig.add_subplot(gs[1])

    # --- placement panel (horizontal = W axis, vertical = H axis) ---
    ax.set_facecolor(SURFACE)
    pad = 0.08 * max(H, W)  # cells may sit slightly outside the chip
    ax.set_xlim(-pad, W + pad)
    ax.set_ylim(-pad, H + pad)
    ax.set_aspect("equal")
    ax.tick_params(colors=INK_2, labelsize=8, length=0)
    for s in ax.spines.values():
        s.set_visible(False)

    segs = []
    for lo0, lo1, hi0, hi1 in snap["regions"]:
        segs += [[(lo1, lo0), (hi1, lo0)], [(hi1, lo0), (hi1, hi0)],
                 [(hi1, hi0), (lo1, hi0)], [(lo1, hi0), (lo1, lo0)]]
    ax.add_collection(LineCollection(segs, colors=INK_2, linewidths=0.6, alpha=0.55, zorder=1))
    ax.add_patch(Rectangle((0, 0), W, H, fill=False, ec=INK, lw=1.0, zorder=2))

    movable, fixed = [], []
    for p0, p1, s0, s1, is_fixed in snap["cells"]:
        rect = Rectangle((p1 - s1 / 2, p0 - s0 / 2), s1, s0)
        (fixed if is_fixed else movable).append(rect)
    ax.add_collection(PatchCollection(movable, fc=MOVABLE, ec=SURFACE, lw=0.3, alpha=0.85, zorder=3))
    ax.add_collection(PatchCollection(fixed, fc=FIXED, ec=SURFACE, lw=0.3, zorder=4))
    ax.legend(
        handles=[Line2D([], [], marker="s", ls="", color=MOVABLE, label=f"movable ({len(movable)})"),
                 Line2D([], [], marker="s", ls="", color=FIXED, label=f"fixed ({len(fixed)})"),
                 Line2D([], [], color=INK_2, lw=0.8, alpha=0.7, label="region cut")],
        loc="upper center", bbox_to_anchor=(0.5, -0.05), ncol=3, frameon=False,
        fontsize=8, labelcolor=INK_2, handletextpad=0.3, columnspacing=1.2)

    # --- wirelength panel ---
    wl.set_facecolor(SURFACE)
    xs = list(range(len(history)))
    wl.set_xlim(-0.5, total_frames - 0.5)
    wl.set_ylim(0, wl_max * 1.12)
    wl.plot(xs, history, color=MOVABLE, lw=2, solid_capstyle="round", zorder=3)
    wl.plot(xs[-1:], history[-1:], "o", ms=8, color=MOVABLE, mec=SURFACE, mew=2, zorder=4)
    wl.annotate(f"{history[-1]:,.0f}", (xs[-1], history[-1]), xytext=(0, 10),
                textcoords="offset points", ha="center", fontsize=9, color=INK)
    wl.grid(axis="y", color=GRID, lw=0.8)
    wl.set_axisbelow(True)
    for s in ("top", "right", "left"):
        wl.spines[s].set_visible(False)
    wl.spines["bottom"].set_color(GRID)
    wl.tick_params(colors=INK_2, labelsize=8, length=0)
    wl.xaxis.get_major_locator().set_params(integer=True)
    wl.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    wl.set_xlabel("iteration", color=INK_2, fontsize=9)
    wl.set_title("Wirelength (star model)", loc="left", color=INK, fontsize=10)

    frame = snap["frame"]
    label = "initial placement" if frame == 0 else f"iteration {frame} / {total_frames - 1}"
    fig.text(0.06, 0.93, title, color=INK, fontsize=13, weight="bold")
    fig.text(0.06, 0.885, f"{label}  ·  {len(snap['regions'])} regions",
             color=INK_2, fontsize=10)

    buf = io.BytesIO()
    fig.savefig(buf, format="png", facecolor=SURFACE)
    plt.close(fig)
    buf.seek(0)
    return Image.open(buf).convert("RGB")


def quantize_shared(frames, colors=96):
    """Quantize all frames with one palette that is guaranteed to contain the
    key colors (small marks such as fixed cells would otherwise be dropped)."""
    w, h = frames[-1].size
    ref = Image.new("RGB", (w, h + 64), SURFACE)
    ref.paste(frames[-1], (0, 0))
    for i, color in enumerate((MOVABLE, FIXED, INK, INK_2, GRID)):
        ref.paste(color, (i * w // 5, h, (i + 1) * w // 5, h + 64))
    # method=0 is median cut (the Image.Quantize enum needs Pillow >= 9.1)
    palette = ref.quantize(colors=colors, method=0)
    return [f.quantize(palette=palette, dither=0) for f in frames]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("result_dir", help="output directory of gordian_linux_build (contains snapshots/)")
    ap.add_argument("-o", "--output", help="GIF path (default: <result_dir>/placement.gif)")
    ap.add_argument("--title", help="title shown on every frame (default: result_dir name)")
    ap.add_argument("--frame-ms", type=int, default=700, help="duration of each frame (ms)")
    ap.add_argument("--hold-ms", type=int, default=2500, help="duration of the last frame (ms)")
    ap.add_argument("--dpi", type=int, default=100, help="resolution (100 -> 960x540)")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.result_dir, "snapshots", "frame_*.txt")))
    if not paths:
        sys.exit(f"no snapshots found in {args.result_dir}/snapshots (run without --no-snapshots)")
    snaps = [read_snapshot(p) for p in paths]
    title = args.title or os.path.basename(os.path.normpath(args.result_dir))
    wirelengths = [s["wirelength"] for s in snaps]

    frames = []
    for i, snap in enumerate(snaps):
        frames.append(render(snap, wirelengths[: i + 1], len(snaps), max(wirelengths), title, args.dpi))
        print(f"\rrendered frame {i + 1}/{len(snaps)}", end="", flush=True)
    print()

    out = args.output or os.path.join(args.result_dir, "placement.gif")
    pal = quantize_shared(frames)
    durations = [args.frame_ms] * (len(pal) - 1) + [args.hold_ms]
    pal[0].save(out, save_all=True, append_images=pal[1:], duration=durations, loop=0, optimize=True)
    print(f"wrote {out} ({os.path.getsize(out) / 1024:.0f} KiB, {len(pal)} frames)")


if __name__ == "__main__":
    main()
