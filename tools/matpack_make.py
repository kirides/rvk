#!/usr/bin/env python3
"""Make material-pack maps from a diffuse texture (docs/research/material-pack.md 6).

  python -I tools/matpack_make.py <diffuse ._ximg | .dds | .png> <out dir> --preset wood|stone|metal|rust|cobble [--size 256]

Writes <out dir>/<DiffuseName>_orm.png (R occlusion, G roughness, B metalness, A emissive). The maps are derived from the diffuse's
luma (dark = rough / occluded) with a per-preset curve: a starting point for hand authoring, not a physical measurement.
Needs numpy + Pillow (reads DDS DXT1/3/5).
"""
import argparse
import io
import pathlib
import sys

import numpy as np
from PIL import Image


def load(path: pathlib.Path) -> Image.Image:
    data = path.read_bytes()
    if path.suffix.lower() == ".png":
        return Image.open(io.BytesIO(data)).convert("RGB")
    at = data.find(b"DDS ")  # ._ximg = a small header + an ordinary DDS payload
    if at < 0:
        raise SystemExit(f"{path}: no DDS payload")
    return Image.open(io.BytesIO(data[at:])).convert("RGB")


def smooth(a: np.ndarray, r: int) -> np.ndarray:
    k = np.ones(2 * r + 1) / (2 * r + 1)
    a = np.apply_along_axis(lambda m: np.convolve(np.pad(m, r, mode="wrap"), k, mode="valid"), 0, a)
    return np.apply_along_axis(lambda m: np.convolve(np.pad(m, r, mode="wrap"), k, mode="valid"), 1, a)


PRESETS = {
    # roughness = lo + (hi - lo) * (1 - luma) ** gamma ; metalness constant or from a colour test ; occlusion from the local luma
    "wood": dict(lo=0.45, hi=0.85, gamma=1.0, metal=0.0, occ=0.5),
    "stone": dict(lo=0.55, hi=0.95, gamma=0.8, metal=0.0, occ=0.6),
    "cobble": dict(lo=0.30, hi=0.90, gamma=1.4, metal=0.0, occ=0.6),  # wet-looking highlights on the bright stone faces
    "metal": dict(lo=0.20, hi=0.55, gamma=1.0, metal=1.0, occ=0.3),
    "rust": dict(lo=0.30, hi=0.95, gamma=0.7, metal="rust", occ=0.5),  # bright steel scratches vs. rough orange rust
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("diffuse", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--preset", choices=sorted(PRESETS), required=True)
    ap.add_argument("--size", type=int, default=256)
    a = ap.parse_args()
    img = load(a.diffuse).resize((a.size, a.size), Image.LANCZOS)
    rgb = np.asarray(img, dtype=np.float32) / 255.0
    luma = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    p = PRESETS[a.preset]
    lo, hi = np.percentile(luma, [2, 98])
    n = np.clip((luma - lo) / max(hi - lo, 1e-3), 0, 1)
    if a.preset == "cobble":
        rough = p["lo"] + (p["hi"] - p["lo"]) * (1 - n) ** p["gamma"]
    else:
        rough = p["lo"] + (p["hi"] - p["lo"]) * (1 - n) ** p["gamma"]
    if p["metal"] == "rust":
        # orange / red pixels are rust (dielectric, rough); the grey ones are bare steel
        sat = (rgb.max(-1) - rgb.min(-1)) / np.maximum(rgb.max(-1), 1e-3)
        bare = np.clip(1.0 - sat * 2.5, 0, 1)
        metal = bare ** 1.5
        rough = rough * (1 - 0.5 * metal)
    else:
        metal = np.full_like(luma, float(p["metal"]))
    occ = 1.0 - p["occ"] * np.clip(smooth(1.0 - n, 3) - 0.3, 0, 1)
    emis = np.zeros_like(luma)
    out = np.stack([occ, rough, metal, emis], axis=-1)
    out = (np.clip(out, 0, 1) * 255.0 + 0.5).astype(np.uint8)
    a.out.mkdir(parents=True, exist_ok=True)
    name = a.diffuse.name.split(".")[0]
    dst = a.out / f"{name}_orm.png"
    Image.fromarray(out, "RGBA").save(dst, optimize=True)
    print(dst, dst.stat().st_size, "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
