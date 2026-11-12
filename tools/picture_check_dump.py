"""Decode a picture_check dump (enh/picture_check.cpp, RVK_PICTURE_DUMP) -> PNGs: ref / cur (simple tone map), colour diff mask, depth diff mask, side by side."""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(path):
    b = Path(path).read_bytes()
    magic, w, h, cp, dp = struct.unpack_from('<5I', b, 0)
    offs = struct.unpack_from('<4Q', b, 20)
    base = 20 + 32
    def part(i, pitch, dtype, comps):
        raw = np.frombuffer(b, dtype=np.uint8, count=pitch * h, offset=base + offs[i]).reshape(h, pitch)
        return raw[:, : w * comps * np.dtype(dtype).itemsize].copy().view(dtype).reshape(h, w, comps)
    rc = part(0, cp, np.float16, 4).astype(np.float32)[..., :3]
    cc = part(1, cp, np.float16, 4).astype(np.float32)[..., :3]
    rd = part(2, dp, np.float32, 1)[..., 0]
    cd = part(3, dp, np.float32, 1)[..., 0]
    return w, h, rc, cc, rd, cd


def tone(c):
    c = np.nan_to_num(c, nan=0.0, posinf=4.0)
    c = c / (1.0 + c)
    return (np.clip(c, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


def main():
    p = Path(sys.argv[1])
    w, h, rc, cc, rd, cd = load(p)
    scale = 0.05 + np.maximum(rc.max(-1), cc.max(-1))
    dc = np.abs(cc - rc).max(-1) / scale
    dd = np.abs(cd - rd) / np.maximum(np.minimum(cd, rd), 1e-4)
    cm = dc > 0.04
    dm = dd > 0.004
    print(f'{p.name}: {w}x{h} colour diff {cm.sum()} ({100 * cm.mean():.3f} %), depth diff {dm.sum()} ({100 * dm.mean():.3f} %), max dc {np.nanmax(dc):.3f}')
    if cm.any():
        ys, xs = np.nonzero(cm)
        print(f'  colour box x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}')
        lr = rc[cm].mean(0)
        lc = cc[cm].mean(0)
        print(f'  mean colour in the differing pixels: ref {lr}, cur {lc}, ratio {lc / np.maximum(lr, 1e-6)}')
    if dm.any():
        ys, xs = np.nonzero(dm)
        print(f'  depth box x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}; ref depth there {np.median(rd[dm]):.1f}, cur {np.median(cd[dm]):.1f}')
    out = p.with_suffix('')
    a, c = tone(rc), tone(cc)
    mask = np.zeros((h, w, 3), np.uint8)
    mask[cm] = (255, 0, 0)
    mask[dm] = (0, 255, 255)
    over = (c.astype(np.float32) * 0.5).astype(np.uint8)
    over[cm | dm] = mask[cm | dm]
    Image.fromarray(np.concatenate([a, c], 1)).save(str(out) + '_refcur.png')
    Image.fromarray(over).save(str(out) + '_mask.png')


if __name__ == '__main__':
    main()
