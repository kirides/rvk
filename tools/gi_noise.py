"""GI noise of burst captures of a still view (docs/research/rtgi.md 20).

Usage: python tools/gi_noise.py <dir of enh_<spot>[_b<k>].png> [--crop y0:y1] [--json out.json]

Per spot: the temporal standard deviation of each pixel's luma over the burst, averaged over the pixels (`tstd`), over
the dark half of the pixels (`tstd_dark`, where the GI's grain shows most), plus the mean luma and the mean of the
frame-to-frame absolute difference of consecutive frames (`flick`, in 0..255 luma). rvkcompare spots freeze game time,
so the scene itself does not move: what varies is the renderer's noise.
"""
import json
import os
import re
import sys

import numpy as np
from PIL import Image


def luma(path, crop):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    if crop:
        im = im[crop[0]:crop[1]]
    return im @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    crop = None
    out_json = None
    for i, a in enumerate(sys.argv):
        if a == "--crop":
            y0, y1 = sys.argv[i + 1].split(":")
            crop = (int(y0), int(y1))
            args.remove(sys.argv[i + 1])
        if a == "--json":
            out_json = sys.argv[i + 1]
            args.remove(sys.argv[i + 1])
    d = args[0]
    spots = {}
    for f in sorted(os.listdir(d)):
        m = re.match(r"(?:enh|eng)_(.+?)(?:_b(\d+))?\.png$", f)
        if m and not f.endswith("_gen.png"):
            spots.setdefault(m.group(1), []).append((int(m.group(2) or 0), f))
    result = {}
    for spot, frames in sorted(spots.items()):
        frames.sort()
        if len(frames) < 3:
            continue
        stack = np.stack([luma(os.path.join(d, f), crop) for _, f in frames])
        mean = stack.mean(0)
        std = stack.std(0)
        dark = mean < np.median(mean)
        flick = np.abs(np.diff(stack, axis=0)).mean()
        result[spot] = {
            "frames": len(frames),
            "luma": float(mean.mean()),
            "tstd": float(std.mean()),
            "tstd_dark": float(std[dark].mean()),
            "flick": float(flick),
        }
        r = result[spot]
        print(f"{spot:24s} n={r['frames']:2d} luma {r['luma']:6.1f}  tstd {r['tstd']:.3f}  dark {r['tstd_dark']:.3f}  flick {r['flick']:.3f}")
    if result:
        print(f"{'MEAN':24s}      tstd {np.mean([r['tstd'] for r in result.values()]):.3f}  dark {np.mean([r['tstd_dark'] for r in result.values()]):.3f}")
    if out_json:
        with open(out_json, "w") as fh:
            json.dump(result, fh, indent=1)


if __name__ == "__main__":
    main()
