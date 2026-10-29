"""Shade depth of GI against the engine's lighting (docs/research/rtgi.md 21).

Usage: python tools/gi_shade_stats.py <dir with eng/ and enh/ (water-compare output)> [<other enh-like dir> ...]

For each spot of <dir>/eng/eng_<spot>.png against <dir>/enh/enh_<spot>[_b0].png (and the same spot of the further dirs'
enh/): whole-image luma, the luma of the shaded pixels (engine luma < 60) in both, the mean change there, the share of
dark pixels (luma < 12) and the 5th / 25th luma percentile of the whole image. The side-by-sides show whether the
numbers mean "deeper shadows".
"""
import os
import re
import sys

import numpy as np
from PIL import Image

W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def load(path):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    h = im.shape[0]
    if h == 900:
        im = im[113:787]
    return im @ W


def find(d, tag, spot):
    for name in (f"{tag}_{spot}.png", f"{tag}_{spot}_b0.png"):
        p = os.path.join(d, tag, name)
        if os.path.exists(p):
            return p
    return None


def main():
    root = sys.argv[1]
    others = sys.argv[2:]
    spots = sorted({re.match(r"eng_(.+)\.png$", f).group(1) for f in os.listdir(os.path.join(root, "eng")) if re.match(r"eng_(.+)\.png$", f)})
    dirs = [root] + others
    print(f"{'spot':20s} {'variant':18s} {'luma':>6s} {'engL':>6s} {'shaded':>7s} {'engSh':>7s} {'dSh':>6s} {'dark':>6s} {'engDk':>6s} {'p5':>6s} {'engP5':>6s} {'p25':>6s} {'engP25':>6s}")
    for spot in spots:
        pe = find(root, "eng", spot)
        eng = load(pe)
        for k, d in enumerate(dirs):
            pn = find(d, "enh", spot)
            if pn is None:
                continue
            enh = load(pn)
            if enh.shape != eng.shape:
                continue
            m = eng < 60
            sh_e = eng[m].mean() if m.any() else 0.0
            sh_n = enh[m].mean() if m.any() else 0.0
            print(f"{spot:20s} {os.path.basename(os.path.normpath(d)):18s} {enh.mean():6.1f} {eng.mean():6.1f} {sh_n:7.1f} {sh_e:7.1f} {sh_n - sh_e:6.1f} "
                  f"{(enh < 12).mean():6.3f} {(eng < 12).mean():6.3f} {np.percentile(enh, 5):6.1f} {np.percentile(eng, 5):6.1f} "
                  f"{np.percentile(enh, 25):6.1f} {np.percentile(eng, 25):6.1f}")


if __name__ == "__main__":
    main()
