"""Engine water vs enhanced water: mean luma / hue / saturation over the water of each spot (docs/research/water.md 8).

    python tools/water_stats.py out/water/<stamp> [--mask-dir out/water/<stamp>/msk]

<stamp>/eng/eng_<spot>.png (engine water) and <stamp>/enh/enh_<spot>.png (enhanced water) are compared over a water mask:
where the debug-view run (msk/msk_<spot>.png, water.debug=normals: the water pixels show their normal as a colour) differs
from the engine picture. Without masks, the pixels where engine and enhanced differ are used. Prints a table and writes
water_stats.json next to the pictures. Luma = Rec. 709 Y' of the 8-bit (display) values, 0..255; hue = circular mean of
the HSV hue (degrees, weighted by saturation); sat = mean HSV saturation.
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0


def stats(img, mask):
    px = img[mask]
    if px.size == 0:
        return None
    luma = (0.2126 * px[:, 0] + 0.7152 * px[:, 1] + 0.0722 * px[:, 2]) * 255.0
    mx = px.max(axis=1)
    mn = px.min(axis=1)
    sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
    # hue (HSV), vectorised
    d = np.maximum(mx - mn, 1e-6)
    r, g, b = px[:, 0], px[:, 1], px[:, 2]
    h = np.where(mx == r, ((g - b) / d) % 6.0, np.where(mx == g, (b - r) / d + 2.0, (r - g) / d + 4.0)) * 60.0
    w = sat
    ang = np.radians(h)
    hx, hy = float(np.sum(w * np.cos(ang))), float(np.sum(w * np.sin(ang)))
    hue = math.degrees(math.atan2(hy, hx)) % 360.0 if (hx * hx + hy * hy) > 1e-9 else 0.0
    mean = px.mean(axis=0) * 255.0
    return {'luma': float(luma.mean()), 'luma_p10': float(np.percentile(luma, 10)), 'luma_p90': float(np.percentile(luma, 90)),
            'hue': hue, 'sat': float(sat.mean()), 'rgb': [float(v) for v in mean], 'pixels': int(mask.sum())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--mask-dir', default='')
    ap.add_argument('--threshold', type=float, default=0.12)
    a = ap.parse_args()
    eng_dir, enh_dir = os.path.join(a.dir, 'eng'), os.path.join(a.dir, 'enh')
    mask_dir = a.mask_dir or os.path.join(a.dir, 'msk')
    out = {}
    print(f"{'spot':18} {'px':>8}  {'engine luma':>11} {'enh luma':>9} {'d':>6}   {'eng hue':>7} {'enh hue':>7}   {'eng sat':>7} {'enh sat':>7}")
    for f in sorted(os.listdir(enh_dir)):
        if not f.endswith('.png'):
            continue
        spot = f[len('enh_'):-4]
        pe = os.path.join(eng_dir, 'eng_' + spot + '.png')
        if not os.path.exists(pe):
            continue
        eng, enh = load(pe), load(os.path.join(enh_dir, f))
        pm = os.path.join(mask_dir, 'msk_' + spot + '.png')
        if os.path.exists(pm):
            msk = load(pm)
            mask = np.abs(msk - eng).sum(axis=2) > a.threshold * 3
        else:
            mask = np.abs(enh - eng).sum(axis=2) > a.threshold
        # the letterbox of the 1600x900 pictures and the HUD rows are never water
        if eng.shape[0] == 900:
            mask[:113, :] = False
            mask[787:, :] = False
        se, sn = stats(eng, mask), stats(enh, mask)
        out[spot] = {'engine': se, 'enhanced': sn}
        if se is None:
            print(f'{spot:18} no water pixels')
            continue
        Image.fromarray((mask * 255).astype(np.uint8)).save(os.path.join(a.dir, 'mask_' + spot + '.png'))
        print(f"{spot:18} {se['pixels']:8d}  {se['luma']:11.1f} {sn['luma']:9.1f} {sn['luma'] - se['luma']:+6.1f}   {se['hue']:7.0f} {sn['hue']:7.0f}"
              f"   {se['sat']:7.2f} {sn['sat']:7.2f}")
    with open(os.path.join(a.dir, 'water_stats.json'), 'w', encoding='utf-8') as fp:
        json.dump(out, fp, indent=1)


if __name__ == '__main__':
    sys.exit(main())
