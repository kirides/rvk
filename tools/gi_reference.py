#!/usr/bin/env python3
"""GI reference comparison (docs/research/path-tracing.md): DDGI + screen probes against the reference path tracer.

Reads the dumps of `gi.reference` (bin/rvk/gi_ref/e<epoch>_full_<spp>.npy, planes: 0 reference indirect, 1 reference direct,
2 DDGI volume irradiance, 3 DDGI + screen probes, 4 albedo (+ view depth), 5 normal; all "acc" units, float16) and writes,
per spot, side-by-side images, error heat maps, numbers and a ranked list of the places the probes miss.

  python tools/gi_reference.py analyze <run dir> [--spots a,b] [--out <dir>]

<run dir> (made by scripts/gi-reference.ps1) holds: gi_ref/ (the dumps + e*_meta.json), meta.json (the RVKCMP lines: a spot's camera
to find its epoch), optionally off/ ddgi/ ref/ (the game's own pictures of each spot, <tag>_<spot>.png) for the triptychs.
"""
import argparse
import json
import math
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

LW = np.array([0.2126, 0.7152, 0.0722], np.float32)
EPS = 0.02  # relative errors are taken against max(reference, EPS) (acc units: the engine's ambient is 0.1 .. 1)


def lum(a):
    return a[..., :3] @ LW


def load_full(path):
    return np.load(path).astype(np.float32)


def render(albedo, e, exposure):
    """albedo x pow(acc, 0.45) as the engine composes it, a fixed exposure, display gamma."""
    col = albedo[..., :3] * np.power(np.maximum(e[..., :3], 0.0), 0.45)
    return col * exposure


def to_u8(x):
    return (np.clip(x, 0.0, 1.0) ** (1 / 1.0) * 255.0 + 0.5).astype(np.uint8)


def heat(rel):
    """signed relative error -> blue (DDGI too dark) .. white .. red (too bright), +-100 %"""
    r = np.clip(rel, -1.0, 1.0)
    out = np.ones(rel.shape + (3,), np.float32)
    pos = np.clip(r, 0, 1)[..., None]
    neg = np.clip(-r, 0, 1)[..., None]
    out = out * (1 - pos - neg) + pos * np.array([1.0, 0.1, 0.05]) + neg * np.array([0.1, 0.3, 1.0])
    return out


def label(img, text):
    im = Image.fromarray(img)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 8 + 7 * len(text), 14), fill=(0, 0, 0))
    d.text((4, 1), text, fill=(255, 255, 255))
    return np.asarray(im)


def metrics(ref, est, mask, w=None):
    """ref, est: luminance images; mask: valid pixels"""
    r, e = ref[mask], est[mask]
    d = e - r
    rel = d / np.maximum(r, EPS)
    out = {
        'pixels': int(mask.sum()),
        'ref_mean': float(r.mean()), 'est_mean': float(e.mean()),
        'bias': float(d.mean()), 'bias_rel': float(e.mean() / max(r.mean(), 1e-9) - 1.0),
        'mae': float(np.abs(d).mean()), 'rmse': float(np.sqrt((d * d).mean())),
        'mean_rel_err': float(np.abs(rel).mean()), 'median_rel_err': float(np.median(np.abs(rel))),
        'p90_rel_err': float(np.percentile(np.abs(rel), 90)),
        'over_50': float((rel > 0.5).mean()), 'under_33': float((rel < -0.33).mean()),
        'corr': float(np.corrcoef(r, e)[0, 1]) if r.std() > 1e-9 and e.std() > 1e-9 else 0.0,
    }
    return out


def chroma_error(ref_rgb, est_rgb, mask, floor=0.05):
    """mean abs difference of the chromaticities (rgb / luminance) where both are lit"""
    lr, le = lum(ref_rgb), lum(est_rgb)
    m = mask & (lr > floor) & (le > floor)
    if m.sum() == 0:
        return 0.0
    cr = ref_rgb[..., :3][m] / lr[m][:, None]
    ce = est_rgb[..., :3][m] / le[m][:, None]
    return float(np.abs(cr - ce).mean())


def classes(ref_ind, est_ind, direct, albedo, normal, mask):
    """error shares by kind of pixel: rank the failure modes. Returns a list of dicts (class, share of pixels, bias, mean rel error,
    share of the summed squared error)."""
    z = albedo[..., 3]
    zz = np.log(np.maximum(z, 1.0))
    gy, gx = np.gradient(zz)
    edge = np.hypot(gx, gy)
    edge_thr = np.percentile(edge[mask], 85) if mask.any() else 1
    dl = lum(direct)
    sunlit = dl > 0.05
    rl = lum(ref_ind) if ref_ind.ndim == 3 else ref_ind
    el = lum(est_ind) if est_ind.ndim == 3 else est_ind
    ny = normal[..., 1]
    chroma = np.zeros_like(rl)
    ok = rl > 0.03
    rgb = ref_ind[..., :3]
    chroma[ok] = (rgb[ok].max(-1) - rgb[ok].min(-1)) / np.maximum(rl[ok], 1e-6)
    d = el - rl
    rel = d / np.maximum(rl, EPS)
    sq = d * d
    total_sq = sq[mask].sum() + 1e-12
    sets = {
        'sunlit surface': sunlit,
        'shaded surface (no sun)': ~sunlit,
        'dark indirect (ref < 0.06)': rl < 0.06,
        'mid indirect (0.06 - 0.3)': (rl >= 0.06) & (rl < 0.3),
        'bright indirect (> 0.3)': rl >= 0.3,
        'depth edges / creases (top 15 % view-depth gradient)': edge > edge_thr,
        'flat areas': edge <= edge_thr,
        'floors (normal up)': ny > 0.7,
        'walls (normal sideways)': np.abs(ny) <= 0.7,
        'ceilings / undersides': ny < -0.3,
        'coloured light (ref saturation > 0.25)': chroma > 0.25,
        'near the camera (< 5 m)': z < 500,
        'mid distance (5 - 25 m)': (z >= 500) & (z < 2500),
        'far (> 25 m)': z >= 2500,
    }
    rows = []
    for name, s in sets.items():
        m = mask & s
        n = int(m.sum())
        if n < 200:
            continue
        rows.append({'class': name, 'share_pixels': n / max(mask.sum(), 1), 'bias_rel': float(el[m].mean() / max(rl[m].mean(), 1e-9) - 1.0),
                     'mean_rel_err': float(np.abs(rel[m]).mean()), 'share_sq_error': float(sq[m].sum() / total_sq),
                     'over_50': float((rel[m] > 0.5).mean()), 'under_33': float((rel[m] < -0.33).mean())})
    return rows


def crops(ref_img, est_img, heat_img, rel, mask, outdir, name, count=3, tile=96, zoom=3):
    """the tiles with the largest summed |relative error| (not overlapping), ref | est | heat, scaled up"""
    h, w = rel.shape
    score = np.abs(rel) * mask
    ty, tx = h // tile, w // tile
    s = score[:ty * tile, :tx * tile].reshape(ty, tile, tx, tile).sum((1, 3))
    used = np.zeros_like(s, bool)
    files = []
    for k in range(count):
        s2 = np.where(used, -1, s)
        iy, ix = np.unravel_index(np.argmax(s2), s2.shape)
        if s2[iy, ix] <= 0:
            break
        used[max(0, iy - 1):iy + 2, max(0, ix - 1):ix + 2] = True
        y0, x0 = iy * tile, ix * tile
        parts = [a[y0:y0 + tile, x0:x0 + tile] for a in (ref_img, est_img, heat_img)]
        big = [np.asarray(Image.fromarray(p).resize((tile * zoom, tile * zoom), Image.NEAREST)) for p in parts]
        strip = np.concatenate(big, 1)
        f = os.path.join(outdir, f'{name}_crop{k + 1}.png')
        Image.fromarray(strip).save(f)
        files.append({'file': os.path.basename(f), 'x': x0, 'y': y0, 'tile': tile})
    return files


def atrous(img, normal, z, valid, iters=5, sig_n=32.0, sig_z=0.05, sig_l=4.0):
    """edge-avoiding a-trous (our baseline denoiser, offline here): weights from the normal, the view depth and the luminance"""
    cur = img.copy()
    h, w = img.shape[:2]
    kern = [1 / 16, 1 / 4, 3 / 8, 1 / 4, 1 / 16]
    zl = np.log(np.maximum(z, 1.0))
    for it in range(iters):
        step = 1 << it
        acc = np.zeros_like(cur)
        wsum = np.zeros(cur.shape[:2], np.float32)
        lc = lum(cur)
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                ys = np.clip(np.arange(h) + dy * step, 0, h - 1)
                xs = np.clip(np.arange(w) + dx * step, 0, w - 1)
                n2 = normal[ys][:, xs]
                wn = np.maximum((normal * n2).sum(-1), 0) ** sig_n
                wz = np.exp(-np.abs(zl - zl[ys][:, xs]) / sig_z)
                l2 = lc[ys][:, xs]
                wl = np.exp(-np.abs(lc - l2) / (sig_l * (np.sqrt(lc * 0 + 0.05) + 1e-3) * (2.0 ** it) ** 0.5 + 1e-3))
                wgt = kern[dy + 2] * kern[dx + 2] * wn * wz * wl
                acc += cur[ys][:, xs] * wgt[..., None]
                wsum += wgt
        cur = acc / np.maximum(wsum, 1e-6)[..., None]
    return cur


def find_epoch(gi_dir, camera, tol=80.0):
    """the full dump nearest to a camera position (the latest epoch among the matches)"""
    best = None
    for f in sorted(os.listdir(gi_dir)):
        m = re.match(r'e(\d+)_meta\.json$', f)
        if not m:
            continue
        meta = json.load(open(os.path.join(gi_dir, f)))
        c = meta['camera']
        d = math.dist(c, camera)
        if d <= tol:
            if best is None or meta['epoch'] > best['epoch']:
                best = meta
    return best


def convergence(gi_dir, epoch, final_ind, mask, final_spp, var_plane):
    """error of the reference at 64, 128, ... samples against the final one, and the standard error the variance plane predicts"""
    rows = []
    fl = lum(final_ind)
    for f in sorted(os.listdir(gi_dir)):
        m = re.match(rf'e{epoch:03d}_(\d+)\.npy$', f)
        if not m:
            continue
        spp = int(m.group(1))
        a = np.load(os.path.join(gi_dir, f)).astype(np.float32)[0]
        l = lum(a)
        rel = (l - fl) / np.maximum(fl, EPS)
        rows.append({'spp': spp, 'rms_rel_vs_final': float(np.sqrt((rel[mask] ** 2).mean())),
                     'median_abs_rel_vs_final': float(np.median(np.abs(rel[mask])))})
    # the predicted standard error of the final mean: sqrt(var / n)
    l2 = var_plane
    var = np.maximum(l2 - fl * fl, 0.0)
    se = np.sqrt(var / final_spp)
    rel_se = se / np.maximum(fl, EPS)
    pred = {'spp': final_spp, 'median_rel_se': float(np.median(rel_se[mask])), 'p90_rel_se': float(np.percentile(rel_se[mask], 90)),
            'share_pixels_se_over_5pct': float((rel_se[mask] > 0.05).mean())}
    return rows, pred


def analyze_spot(spot, meta, gi_dir, outdir, shots, ref_meta=None, ref_dir=None):
    """ref_meta / ref_dir: take the reference planes (0 indirect, 1 direct, 4 albedo + depth, 5 normal) from an earlier, converged run and
    only the DDGI planes (2, 3, 6, 7) from this one (a quick re-measure after a change to the probes: this run needs only a few reference
    samples; the cameras of both runs are pinned, the images align). The convergence rows come from the earlier run."""
    full = os.path.join(gi_dir, f"e{meta['epoch']:03d}_full_{meta['spp']:05d}.npy")
    P = load_full(full)
    if ref_meta is not None:
        R = load_full(os.path.join(ref_dir, f"e{ref_meta['epoch']:03d}_full_{ref_meta['spp']:05d}.npy"))
        if R.shape[1:3] != P.shape[1:3]:
            raise SystemExit(f'{spot}: image sizes differ between the runs')
        for k in (0, 1, 4, 5):
            P[k] = R[k]
        meta = ref_meta
        gi_dir = ref_dir
    ref_ind, ref_dir, ddgi_v, ddgi_s, alb_z, nrm = P[:6]
    pix1, pix4 = (P[6], P[7]) if P.shape[0] >= 8 else (None, None)
    valid = nrm[..., 3] > 0.5
    albedo = alb_z.copy()
    # inputs of the comparison: indirect, per estimator; and the lit image (direct + indirect) in the engine's units
    lr, lv, ls = lum(ref_ind), lum(ddgi_v), lum(ddgi_s)
    have_scr = (ddgi_s[..., 3] > 0.5) & valid
    res = {'spot': spot, 'epoch': meta['epoch'], 'spp': meta['spp'], 'size': [int(P.shape[2]), int(P.shape[1])], 'valid_pixels': int(valid.sum()),
           'screen_probe_coverage': float(have_scr.sum() / max(valid.sum(), 1))}
    mask = valid & (lr > 1e-4)
    res['indirect'] = {'volume': metrics(lr, lv, mask), 'screen': metrics(lr, ls, mask)}
    if pix1 is not None:
        res['pixel'] = {'raw_1spp': metrics(lr, lum(pix1), mask), 'raw_4spp': metrics(lr, lum(pix4), mask)}
        for name, plane in (('atrous_1spp', pix1), ('atrous_4spp', pix4)):
            den = atrous(plane[..., :3], nrm[..., :3], albedo[..., 3], valid)
            res['pixel'][name] = metrics(lr, lum(den), mask)
            if name == 'atrous_1spp':
                pix_den = den
    res['chroma'] = {'volume': chroma_error(ref_ind, ddgi_v, mask), 'screen': chroma_error(ref_ind, ddgi_s, mask)}
    # the lit image: direct (traced, the same for both) + indirect, through the engine's pow(acc, 0.45)
    tot_r = lum(np.power(np.maximum(ref_dir[..., :3] + ref_ind[..., :3], 0), 0.45))
    tot_s = lum(np.power(np.maximum(ref_dir[..., :3] + ddgi_s[..., :3], 0), 0.45))
    res['lit_image'] = metrics(tot_r, tot_s, mask)
    res['classes_screen'] = sorted(classes(ref_ind, ddgi_s, ref_dir, albedo, nrm, mask), key=lambda r: -r['share_sq_error'])
    res['classes_volume'] = sorted(classes(ref_ind, ddgi_v, ref_dir, albedo, nrm, mask), key=lambda r: -r['share_sq_error'])
    # the reference's own noise and convergence
    res['convergence'], res['predicted_noise'] = convergence(gi_dir, meta['epoch'], ref_ind, mask, meta['spp'], ref_ind[..., 3])
    # pictures: one exposure for all columns, chosen on the reference
    lit_ref = render(albedo, ref_dir + ref_ind, 1.0)
    exposure = 0.45 / max(float(np.percentile(lum(lit_ref)[valid], 75)) if valid.any() else 1.0, 1e-4)
    imgs = {
        'reference': to_u8(render(albedo, ref_dir + ref_ind, exposure)),
        'ddgi_volume': to_u8(render(albedo, ref_dir + ddgi_v, exposure)),
        'ddgi_screen': to_u8(render(albedo, ref_dir + ddgi_s, exposure)),
    }
    rel_s = (ls - lr) / np.maximum(lr, EPS)
    rel_v = (lv - lr) / np.maximum(lr, EPS)
    hs, hv = to_u8(heat(rel_s)), to_u8(heat(rel_v))
    hs[~valid] = 0
    hv[~valid] = 0
    ind_exp = 0.5 / max(float(np.percentile(lr[valid], 75)) ** 0.45 if valid.any() else 1.0, 1e-4)
    ind = lambda a: to_u8(np.repeat((np.power(np.maximum(lum(a), 0.0), 0.45) * ind_exp)[..., None], 3, -1))
    row1 = np.concatenate([label(imgs['reference'], 'reference'), label(imgs['ddgi_volume'], 'DDGI volume'), label(imgs['ddgi_screen'], 'DDGI + screen probes')], 1)
    row2 = np.concatenate([label(ind(ref_ind), 'indirect: reference'), label(ind(ddgi_s), 'indirect: DDGI + screen'), label(hs, 'error (red: DDGI too bright)')], 1)
    if pix1 is not None:
        row3 = np.concatenate([label(ind(pix1), 'pixel GI 1 spp raw'), label(ind(pix_den), 'pixel GI 1 spp + a-trous'), label(to_u8(heat((lum(pix_den) - lr) / np.maximum(lr, EPS))), 'error of the filtered')], 1)
        row2 = np.concatenate([row2, row3], 0)
    sheet = np.concatenate([row1, row2], 0)
    Image.fromarray(sheet).save(os.path.join(outdir, f'sbs_{spot}.png'))
    Image.fromarray(np.concatenate([label(imgs['reference'], 'reference'), label(hv, 'error: volume only')], 1)).save(os.path.join(outdir, f'err_volume_{spot}.png'))
    res['crops'] = crops(imgs['reference'], imgs['ddgi_screen'], hs, rel_s, mask, outdir, f'crop_{spot}')
    # the game's own pictures (gi off | DDGI as played | reference view)
    trip = []
    for tag, name in (('off', 'gi off'), ('ddgi', 'DDGI (game)'), ('ref', 'reference view')):
        p = shots.get(tag, {}).get(spot)
        if p and os.path.exists(p):
            trip.append(label(np.asarray(Image.open(p).convert('RGB')), name))
    if len(trip) >= 2 and len({t.shape for t in trip}) == 1:
        Image.fromarray(np.concatenate(trip, 1)).save(os.path.join(outdir, f'triptych_{spot}.png'))
    return res


def find_shots(run):
    shots = {}
    for tag in ('off', 'ddgi', 'ref'):
        d = os.path.join(run, tag)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            m = re.match(rf'{tag}_(.+?)(?:_b0)?\.png$', f)
            if m:
                shots.setdefault(tag, {})[m.group(1)] = os.path.join(d, f)
    return shots


def fmt_pct(x):
    return f'{100 * x:+.1f}%'


def write_summary(results, outdir):
    lines = ['# GI reference comparison', '',
             'Indirect light (acc units, mean radiance of cosine-weighted rays) of the DDGI probes (`volume`) and with the screen probes (`screen`) against the reference path tracer.', '',
             '| spot | spp | ref mean | screen bias | screen MAE | screen mean rel err | volume bias | volume mean rel err | over +50% | under -33% | chroma err | lit image rel err |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|']
    for r in results:
        s, v = r['indirect']['screen'], r['indirect']['volume']
        lines.append(f"| {r['spot']} | {r['spp']} | {s['ref_mean']:.3f} | {fmt_pct(s['bias_rel'])} | {s['mae']:.3f} | {100 * s['mean_rel_err']:.0f}% | {fmt_pct(v['bias_rel'])} | "
                     f"{100 * v['mean_rel_err']:.0f}% | {100 * s['over_50']:.0f}% | {100 * s['under_33']:.0f}% | {r['chroma']['screen']:.3f} | {100 * r['lit_image']['mean_rel_err']:.1f}% |")
    lines += ['', '## Reference noise (standard error of the final mean) and convergence', '', '| spot | spp | median rel. s.e. | p90 | pixels > 5% | error vs final at 64 / 128 / 256 / 512 spp (rms rel.) |', '|---|---|---|---|---|---|']
    for r in results:
        p = r['predicted_noise']
        conv = ' / '.join(f"{100 * c['rms_rel_vs_final']:.1f}%" for c in r['convergence'][:4])
        lines.append(f"| {r['spot']} | {p['spp']} | {100 * p['median_rel_se']:.2f}% | {100 * p['p90_rel_se']:.2f}% | {100 * p['share_pixels_se_over_5pct']:.1f}% | {conv} |")
    # failure modes over all spots: the class table, ranked by the mean share of the squared error
    agg = {}
    for r in results:
        for c in r['classes_screen']:
            a = agg.setdefault(c['class'], {'n': 0, 'sq': 0.0, 'bias': 0.0, 'rel': 0.0, 'over': 0.0, 'under': 0.0})
            a['n'] += 1
            a['sq'] += c['share_sq_error']
            a['bias'] += c['bias_rel']
            a['rel'] += c['mean_rel_err']
            a['over'] += c['over_50']
            a['under'] += c['under_33']
    lines += ['', '## Where the probes miss (screen probes on, all spots; share of the summed squared error of the pixels of that kind)', '',
              '| kind of pixel | spots | share of sq. error | bias | mean rel. err | over +50% | under -33% |', '|---|---|---|---|---|---|---|']
    for name, a in sorted(agg.items(), key=lambda kv: -kv[1]['sq'] / kv[1]['n']):
        n = a['n']
        lines.append(f"| {name} | {n} | {100 * a['sq'] / n:.0f}% | {fmt_pct(a['bias'] / n)} | {100 * a['rel'] / n:.0f}% | {100 * a['over'] / n:.0f}% | {100 * a['under'] / n:.0f}% |")
    open(os.path.join(outdir, 'summary.md'), 'w', encoding='utf8').write('\n'.join(lines) + '\n')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['analyze'])
    ap.add_argument('run')
    ap.add_argument('--spots', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--ref-run', default='', help="an earlier run dir whose converged reference planes replace this run's (quick re-measure)")
    a = ap.parse_args()
    run = a.run
    gi_dir = os.path.join(run, 'gi_ref')
    out = a.out or os.path.join(run, 'analysis')
    os.makedirs(out, exist_ok=True)
    meta = json.load(open(os.path.join(run, 'meta.json')))
    want = set(a.spots.split(',')) if a.spots else None
    shots = find_shots(run)
    results = []
    for m in meta:
        spot = m['id']
        if want and spot not in want:
            continue
        cam = m['camera']
        em = find_epoch(gi_dir, [cam['x'], cam['y'], cam['z']])
        if em is None:
            print(f'{spot}: no reference dump near the camera', file=sys.stderr)
            continue
        rm, rdir = None, None
        if a.ref_run:
            # several run dirs may be given (comma separated): the first that holds the spot wins
            for d in a.ref_run.split(','):
                rm = find_epoch(os.path.join(d, 'gi_ref'), [cam['x'], cam['y'], cam['z']])
                if rm is not None:
                    rdir = os.path.join(d, 'gi_ref')
                    break
            if rm is None:
                print(f'{spot}: no reference dump in {a.ref_run}')
                continue
        r = analyze_spot(spot, em, gi_dir, out, shots, rm, rdir)
        results.append(r)
        s = r['indirect']['screen']
        print(f"{spot}: spp {r['spp']}, ref {s['ref_mean']:.3f}, screen bias {fmt_pct(s['bias_rel'])}, mean rel err {100 * s['mean_rel_err']:.0f}%, "
              f"volume {fmt_pct(r['indirect']['volume']['bias_rel'])} / {100 * r['indirect']['volume']['mean_rel_err']:.0f}%")
    json.dump(results, open(os.path.join(out, 'results.json'), 'w'), indent=1, default=lambda o: o.item() if hasattr(o, 'item') else str(o))
    write_summary(results, out)
    print('written', out)


if __name__ == '__main__':
    main()
