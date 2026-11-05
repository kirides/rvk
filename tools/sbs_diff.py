"""Whole-image numbers of water-compare side-by-sides (left = engine run, right = enhanced run): luma, saturation, p99 luma, mean / p95 absolute
difference, share of pixels differing by > 12 / 255, and a heat map next to the pictures.

    python tools/sbs_diff.py <out\\water\\stamp> [--spots a,b] [--heat]
Used for the clustered lights A/B (docs/research/gpu-scene.md 9.7): run it on a control run (same settings both sides = the noise floor) and the test run."""
import argparse
import colorsys
import pathlib
import sys

import numpy as np
from PIL import Image


def luma(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def sat(a):
    mx, mn = a.max(-1), a.min(-1)
    return np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--spots", default="")
    ap.add_argument("--heat", action="store_true")
    args = ap.parse_args()
    d = pathlib.Path(args.dir)
    want = set(s for s in args.spots.split(",") if s)
    print(f"{'spot':16} {'L luma':>7} {'R luma':>7} {'dluma%':>7} {'L p99':>6} {'R p99':>6} {'L sat':>6} {'R sat':>6} {'mean|d|':>8} {'p95|d|':>7} {'>12':>6}")
    for f in sorted(d.glob("sbs_*.png")):
        spot = f.stem[4:]
        if want and spot not in want:
            continue
        im = np.asarray(Image.open(f).convert("RGB"), dtype=np.float32)
        w = im.shape[1] // 2
        L, R = im[:, :w], im[:, w:2 * w]
        ll, rl = luma(L), luma(R)
        df = np.abs(L - R).mean(-1)
        print(f"{spot:16} {ll.mean():7.1f} {rl.mean():7.1f} {100 * (rl.mean() - ll.mean()) / max(ll.mean(), 1e-3):7.1f} {np.percentile(ll, 99):6.0f} {np.percentile(rl, 99):6.0f} "
              f"{sat(L).mean():6.3f} {sat(R).mean():6.3f} {df.mean():8.2f} {np.percentile(df, 95):7.1f} {100 * (df > 12).mean():5.1f}%")
        if args.heat:
            h = np.clip(df * 6, 0, 255).astype(np.uint8)
            out = np.stack([h, np.zeros_like(h), 255 - h], -1)
            Image.fromarray(out).save(d / f"heat_{spot}.png")


if __name__ == "__main__":
    sys.exit(main())
