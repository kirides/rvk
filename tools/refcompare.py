#!/usr/bin/env python3
"""Screenshot comparison for the D3D9 reference runs (docs/research/reference-compare.md section 6).

    python tools/refcompare.py compare A B -o out [--thresholds f] [--meta-a f --meta-b f] [--baseline A2]
    python tools/refcompare.py calibrate A A2 -o thresholds.json [--margin 1.5]
    python tools/refcompare.py selftest

A = reference (D3D9), B = test (Vulkan); each a directory of PNG/BMP screenshots or a single image file. Files pair by spot
name: `<spot>.png` or a burst `<spot>_b0.png .. <spot>_b<n-1>.png` (risen-spass `debug.screenshot{burst = n}`). A tag
prefix (`d3d9_<spot>`, `vk_<spot>`) is stripped with --tag-a/--tag-b, from the metadata's `tag`, or automatically when the
names do not match otherwise.

Per side a burst becomes its per-pixel median (frames whose mean luma deviates > 10 % from the burst are dropped first:
lightning); pixels that change within a burst on either side (> 6/255, dilated 2 px) are masked out of the metrics.
With a second reference run A' (`--baseline A2`, or `calibrate A A2` which stores `<spot>_known.png` masks referenced by
the thresholds file) the pixels that differ between A and A' (> 8/255, dilated 4 px: sea, clouds, idle animations that
the time freeze stops at a different phase in every run) are a known-noise mask, excluded the same way.
Metadata (the RVKCMP JSON lines of spass.log, a JSON list/object, or JSONL; default `<dir>/meta.json`) validates that both
runs looked at the same thing: camera eye/direction/fov, weather mode and weights, clock. A mismatch is INVALID.

Metrics on unmasked pixels of the 8-bit values as stored: mae, bad8/bad32 (% of pixels with max-channel diff > 8/32), psnr,
psnr_q (after 4x4 box downsampling), ssim (Rec.709 luma, Gaussian window sigma 1.5), dluma (B - A), bias_rgb (B - A per
channel), chroma ((R-G, B-G) of bias_rgb), shift (phase correlation; `shift_significant` when aligning by it lowers the
error), worst tiles. Verdicts PASS/WARN/FAIL/INVALID from thresholds (defaults below, a JSON or TOML file overrides them,
per spot under `spot.<id>`). Exit code: 0 all PASS/WARN, 1 any FAIL, 2 any INVALID or nothing compared.
"""

from __future__ import annotations

import argparse
import base64
import copy
import datetime
import html
import io
import json
import math
import re
import sys
import tomllib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import fft as sfft
from scipy import ndimage

# ---------------------------------------------------------------------------------------------------------------------
# thresholds

DEFAULT_THRESHOLDS = {
    # every rule of a tier must hold; PASS if all "pass" rules hold, else WARN if all "warn" rules hold, else FAIL
    "pass": {"ssim_min": 0.98, "bad32_max": 0.2, "dluma_abs_max": 1.0, "psnr_q_min": 38.0, "shift_max": 0},
    "warn": {"ssim_min": 0.95, "bad32_max": 1.0, "dluma_abs_max": 3.0},
    # % of pixels unstable within a burst / covered by the known-noise mask above which the spot is INVALID
    "invalid": {"noise_max": 30.0, "known_max": 75.0},
    # burst mask: > burst_tol within a burst, dilated; known-noise mask (baseline A vs A'): > known_tol between the two
    # reference runs (or unstable in either burst), dilated by known_dilate
    "noise": {"burst_tol": 6, "dilate": 2, "flash_rel": 0.10, "known_tol": 8, "known_dilate": 4},
    "pose": {"eye": 1.0, "dir_deg": 0.1, "fov": 0.01, "weather": 0.01, "clock_min": 1.0},
}

# rule key -> (metric getter, comparison)
RULES = {
    "ssim_min": (lambda m: m["ssim"], ">="),
    "psnr_min": (lambda m: m["psnr"], ">="),
    "psnr_q_min": (lambda m: m["psnr_q"], ">="),
    "mae_max": (lambda m: m["mae"], "<="),
    "bad8_max": (lambda m: m["bad8"], "<="),
    "bad32_max": (lambda m: m["bad32"], "<="),
    "dluma_abs_max": (lambda m: abs(m["dluma"]), "<="),
    "chroma_abs_max": (lambda m: max(abs(c) for c in m["chroma"]), "<="),
    "shift_max": (lambda m: max(abs(v) for v in m["shift"]) if m["shift_significant"] else 0, "<="),
}

PSNR_CAP = 100.0  # identical images (JSON has no inf)


def deep_merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


class Thresholds:
    """`{default: {...}, spot: {<id>: {..., known: "..."}}}`; top-level tier keys count as `default`."""

    def __init__(self, data: dict | None = None, base_dir: Path | None = None):
        data = data or {}
        top = {k: v for k, v in data.items() if k in DEFAULT_THRESHOLDS}
        self.default = deep_merge(deep_merge(DEFAULT_THRESHOLDS, top), data.get("default", {}))
        self.spots = dict(data.get("spot", {}))
        self.base_dir = base_dir  # relative `known_mask` paths are relative to the thresholds file

    @staticmethod
    def load(path: str | Path | None) -> "Thresholds":
        if not path:
            return Thresholds()
        p = Path(path)
        if p.suffix.lower() == ".toml":
            return Thresholds(tomllib.loads(p.read_text(encoding="utf-8")), p.parent)
        return Thresholds(json.loads(p.read_text(encoding="utf-8")), p.parent)

    def for_spot(self, spot: str) -> dict:
        thr = deep_merge(self.default, self.spots.get(spot, {}))
        km = thr.get("known_mask")
        if isinstance(km, str) and self.base_dir is not None and not Path(km).is_absolute():
            thr["known_mask"] = str(self.base_dir / km)
        return thr

    def with_spot_overrides(self, overrides: dict) -> "Thresholds":
        t = Thresholds(base_dir=self.base_dir)
        t.default = copy.deepcopy(self.default)
        t.spots = copy.deepcopy(self.spots)
        for spot, o in overrides.items():
            t.spots[spot] = deep_merge(t.spots.get(spot, {}), o)
        return t


def check_rules(metrics: dict, rules: dict, tier: str) -> list[str]:
    failed = []
    for key, limit in rules.items():
        if key not in RULES:
            continue
        get, op = RULES[key]
        v = get(metrics)
        ok = v >= limit if op == ">=" else v <= limit
        if not ok:
            failed.append(f"{key.rsplit('_', 1)[0]} {fmt(v)} {'<' if op == '>=' else '>'} {fmt(limit)} ({tier})")
    return failed


def fmt(v) -> str:
    if isinstance(v, float):
        return f"{v:.4g}"
    return str(v)


def verdict_for(metrics: dict, thr: dict) -> tuple[str, list[str]]:
    burst = metrics.get("noise_burst", metrics["noise"])
    if burst > thr["invalid"]["noise_max"]:
        return "INVALID", [f"noise {burst:.1f} % > {thr['invalid']['noise_max']} % (pixels unstable within a burst)"]
    known = metrics.get("noise_known", 0.0)
    if known > thr["invalid"].get("known_max", 100.0):
        return "INVALID", [f"known noise {known:.1f} % > {thr['invalid']['known_max']} % (pixels that differ between two reference runs)"]
    failed_pass = check_rules(metrics, thr["pass"], "pass")
    if not failed_pass:
        return "PASS", []
    failed_warn = check_rules(metrics, thr["warn"], "warn")
    if not failed_warn:
        return "WARN", failed_pass
    # plus the pass-tier failures the warn tier has no rule for (a significant shift, ...)
    extra = [r for r in failed_pass if r.split(" ", 1)[0] not in {k.rsplit("_", 1)[0] for k in thr["warn"]}]
    return "FAIL", failed_warn + extra


# ---------------------------------------------------------------------------------------------------------------------
# inputs

IMAGE_EXT = {".png", ".bmp"}
BURST_RE = re.compile(r"^(?P<spot>.+)_b(?P<idx>\d+)$")


def scan_images(path: Path) -> dict[str, list[Path]]:
    """spot name -> image files (burst order). A single file is one spot named after its stem."""
    files = [path] if path.is_file() else sorted(p for p in path.iterdir() if p.suffix.lower() in IMAGE_EXT)
    groups: dict[str, list[tuple[int, Path]]] = {}
    for f in files:
        m = BURST_RE.match(f.stem)
        spot, idx = (m.group("spot"), int(m.group("idx"))) if m else (f.stem, -1)
        groups.setdefault(spot, []).append((idx, f))
    return {s: [f for _, f in sorted(v)] for s, v in groups.items()}


def strip_tag(groups: dict[str, list[Path]], tag: str | None) -> dict[str, list[Path]]:
    if not tag:
        return groups
    p = tag + "_"
    return {(s[len(p):] if s.startswith(p) and len(s) > len(p) else s): v for s, v in groups.items()}


def common_tag(names) -> str | None:
    """The first `_`-separated token when every name has it and more after it."""
    heads = {n.split("_", 1)[0] for n in names if "_" in n}
    if len(heads) == 1 and all("_" in n for n in names):
        return heads.pop()
    return None


def pair_spots(ga, gb, tag_a, tag_b):
    ga, gb = strip_tag(ga, tag_a), strip_tag(gb, tag_b)
    if ga and gb and not (set(ga) & set(gb)):
        # auto: strip a common first token (`d3d9_`, `vk_`) on the side(s) that have one
        ta, tb = (None if tag_a else common_tag(ga)), (None if tag_b else common_tag(gb))
        for ca, cb in ((ta, tb), (ta, None), (None, tb)):
            sa, sb = strip_tag(ga, ca), strip_tag(gb, cb)
            if set(sa) & set(sb):
                return sa, sb
    if len(ga) == 1 and len(gb) == 1 and not (set(ga) & set(gb)):
        # two single images (file arguments or one shot per directory): compare them under A's name
        (na, fa), (_, fb) = next(iter(ga.items())), next(iter(gb.items()))
        return {na: fa}, {na: fb}
    return ga, gb


def load_meta(path: Path | None) -> dict[str, dict]:
    """RVKCMP lines of a log, JSONL, a JSON list of entries, `{spots: [...]}`, or `{<id>: {...}}` -> id -> entry."""
    if not path or not Path(path).is_file():
        return {}
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    entries: list[dict] = []
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        obj = None
        for line in text.splitlines():
            if "RVKCMP" in line:
                line = line.split("RVKCMP", 1)[1]
            line = line.strip()
            if line.startswith("{"):
                try:
                    entries.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    if isinstance(obj, dict) and "spots" in obj:
        obj = obj["spots"]
    if isinstance(obj, dict):
        if "id" in obj:
            entries = [obj]
        else:
            entries = [dict(v, id=v.get("id", k)) for k, v in obj.items() if isinstance(v, dict)]
    elif isinstance(obj, list):
        entries = [e for e in obj if isinstance(e, dict)]
    return {str(e["id"]): e for e in entries if "id" in e}  # last wins (a retried spot)


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.float32)


# ---------------------------------------------------------------------------------------------------------------------
# noise model


def luma(img: np.ndarray) -> np.ndarray:
    return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722


def prepare_burst(frames: list[np.ndarray], noise_cfg: dict) -> tuple[np.ndarray, np.ndarray, dict]:
    """-> representative frame (median), unstable mask (not dilated), info."""
    info = {"frames": len(frames), "dropped": []}
    if len(frames) >= 3:
        means = np.array([float(luma(f).mean()) for f in frames])
        med = float(np.median(means))
        keep = [i for i, m in enumerate(means) if abs(m - med) <= noise_cfg["flash_rel"] * max(med, 1.0)]
        if keep and len(keep) < len(frames):
            info["dropped"] = [i for i in range(len(frames)) if i not in keep]
            frames = [frames[i] for i in keep]
    if len(frames) == 1:
        return frames[0], np.zeros(frames[0].shape[:2], bool), info
    stack = np.stack(frames)
    rep = np.median(stack, axis=0).astype(np.float32)
    spread = (stack.max(axis=0) - stack.min(axis=0)).max(axis=2)
    unstable = spread > noise_cfg["burst_tol"]
    info["unstable"] = round(float(unstable.mean()) * 100, 3)
    return rep, unstable, info


# ---------------------------------------------------------------------------------------------------------------------
# metrics


def ssim_map(x: np.ndarray, y: np.ndarray, sigma: float = 1.5) -> np.ndarray:
    x = x.astype(np.float64)
    y = y.astype(np.float64)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2

    def f(z):
        return ndimage.gaussian_filter(z, sigma, truncate=3.5, mode="reflect")

    mx, my = f(x), f(y)
    sxx = f(x * x) - mx * mx
    syy = f(y * y) - my * my
    sxy = f(x * y) - mx * my
    return ((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    if a.size == 0:
        return PSNR_CAP
    mse = float(np.mean((a.astype(np.float64) - b) ** 2))
    return PSNR_CAP if mse <= 1e-12 else min(PSNR_CAP, 10 * math.log10(255.0 ** 2 / mse))


def box4(img: np.ndarray) -> np.ndarray:
    h, w = img.shape[0] // 4 * 4, img.shape[1] // 4 * 4
    v = img[:h, :w]
    return v.reshape(h // 4, 4, w // 4, 4, *img.shape[2:]).mean(axis=(1, 3))


def estimate_shift(la: np.ndarray, lb: np.ndarray, valid: np.ndarray, max_shift: int = 4) -> dict:
    """Phase correlation of the lumas: (dx, dy) = how far B's content is moved relative to A (+x right, +y down)."""
    h, w = la.shape
    a = la.astype(np.float64)
    b = lb.astype(np.float64)
    if not valid.all():
        a = np.where(valid, a, a[valid].mean() if valid.any() else 0.0)
        b = np.where(valid, b, b[valid].mean() if valid.any() else 0.0)
    win = np.outer(np.hanning(h), np.hanning(w))
    fa = sfft.rfft2((a - a.mean()) * win)
    fb = sfft.rfft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-12
    corr = sfft.irfft2(r, s=(h, w))
    k = max_shift
    sub = np.roll(np.roll(corr, k, axis=0), k, axis=1)[: 2 * k + 1, : 2 * k + 1]  # offsets -k..k
    iy, ix = np.unravel_index(int(np.argmax(sub)), sub.shape)
    dy, dx = iy - k, ix - k

    def parabola(m1, c0, p1):
        d = m1 - 2 * c0 + p1
        return 0.0 if abs(d) < 1e-12 else 0.5 * (m1 - p1) / d

    fy = parabola(corr[(dy - 1) % h, dx % w], corr[dy % h, dx % w], corr[(dy + 1) % h, dx % w])
    fx = parabola(corr[dy % h, (dx - 1) % w], corr[dy % h, dx % w], corr[dy % h, (dx + 1) % w])

    # does aligning by it actually lower the error? (an unrelated image has some peak too)
    m = k + 1
    ca = la[m:h - m, m:w - m]
    cv = valid[m:h - m, m:w - m] & valid[m + dy:h - m + dy, m + dx:w - m + dx]
    e0 = float(np.abs(ca - lb[m:h - m, m:w - m])[cv].mean()) if cv.any() else 0.0
    es = float(np.abs(ca - lb[m + dy:h - m + dy, m + dx:w - m + dx])[cv].mean()) if cv.any() else 0.0
    significant = (dx, dy) != (0, 0) and e0 > 0.25 and es < 0.75 * e0
    return {
        "shift": [int(dx), int(dy)],
        "shift_sub": [round(dx + fx, 3), round(dy + fy, 3)],
        "shift_peak": round(float(sub[iy, ix]), 4),
        "shift_significant": bool(significant),
        "shift_mae": [round(e0, 3), round(es, 3)],
    }


def compute_metrics(a: np.ndarray, b: np.ndarray, valid: np.ndarray, tile: int = 32, ntiles: int = 10):
    """-> (metrics dict, maps dict). a, b float32 HxWx3 (0..255); valid HxW bool."""
    diff = b - a
    adiff = np.abs(diff)
    dmax = adiff.max(axis=2)
    la, lb = luma(a), luma(b)
    smap = ssim_map(la, lb)
    v = valid
    n = int(v.sum())
    m: dict = {"pixels": n, "noise": round(100.0 * (1 - n / v.size), 3)}
    if n == 0:
        m.update(mae=0.0, bad8=0.0, bad32=0.0, psnr=PSNR_CAP, psnr_q=PSNR_CAP, ssim=1.0, dluma=0.0,
                 bias_rgb=[0.0] * 3, chroma=[0.0, 0.0], shift=[0, 0], shift_significant=False)
        return m, {"dmax": dmax, "ssim": smap}
    bias = [float(diff[..., c][v].mean()) for c in range(3)]
    qa, qb, qv = box4(a), box4(b), box4(v.astype(np.float32)) >= 1.0
    m.update(
        mae=round(float(adiff[v].mean()), 4),
        bad8=round(100.0 * float((dmax[v] > 8).mean()), 4),
        bad32=round(100.0 * float((dmax[v] > 32).mean()), 4),
        psnr=round(psnr(a[v], b[v]), 3),
        psnr_q=round(psnr(qa[qv], qb[qv]), 3),
        ssim=round(float(smap[v].mean()), 5),
        dluma=round(float((lb - la)[v].mean()), 4),
        bias_rgb=[round(x, 4) for x in bias],
        chroma=[round(bias[0] - bias[1], 4), round(bias[2] - bias[1], 4)],
        max_diff=int(dmax[v].max()),
    )
    m.update(estimate_shift(la, lb, v))
    # worst tiles by MAE
    h, w = dmax.shape
    tiles = []
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            tv = v[y:y + tile, x:x + tile]
            if tv.sum() < 0.5 * tile * tile:  # mostly masked, or a sliver at the image edge
                continue
            tmae = float(adiff[y:y + tile, x:x + tile][tv].mean())
            tiles.append({
                "x": x, "y": y, "w": tv.shape[1], "h": tv.shape[0], "mae": round(tmae, 3),
                "bad32": round(100.0 * float((dmax[y:y + tile, x:x + tile][tv] > 32).mean()), 2),
                "ssim": round(float(smap[y:y + tile, x:x + tile][tv].mean()), 4),
            })
    tiles.sort(key=lambda t: -t["mae"])
    m["tiles"] = [t for t in tiles[:ntiles] if t["mae"] > 0]
    return m, {"dmax": dmax, "ssim": smap}


# ---------------------------------------------------------------------------------------------------------------------
# pose / state check


def _vec(d: dict, keys) -> np.ndarray | None:
    try:
        return np.array([float(d[k]) for k in keys])
    except (KeyError, TypeError, ValueError):
        return None


def _clock_minutes(t) -> float | None:
    if isinstance(t, (int, float)):
        return float(t) * 60.0
    if isinstance(t, (list, tuple)) and t and all(isinstance(x, (int, float)) for x in t[:2]):
        return float(t[0]) * 60.0 + (float(t[1]) if len(t) > 1 else 0.0)
    if isinstance(t, dict):
        h = t.get("h", t.get("hour"))
        mi = t.get("m", t.get("min", t.get("minute", 0)))
        if isinstance(h, (int, float)):
            return float(h) * 60.0 + float(mi or 0)
    return None


def check_pose(ma: dict | None, mb: dict | None, tol: dict) -> dict:
    """-> {ok, problems, deltas, unchecked}."""
    res = {"ok": True, "problems": [], "deltas": {}, "unchecked": []}
    if not ma or not mb:
        res["unchecked"].append("no metadata" if not ma and not mb else "metadata on one side only")
        return res
    ca, cb = ma.get("camera"), mb.get("camera")
    if isinstance(ca, dict) and isinstance(cb, dict):
        ea, eb = _vec(ca, "xyz"), _vec(cb, "xyz")
        if ea is not None and eb is not None:
            d = float(np.linalg.norm(ea - eb))
            res["deltas"]["eye"] = round(d, 4)
            if d > tol["eye"]:
                res["problems"].append(f"camera eye differs by {d:.3g} > {tol['eye']}")
        da, db = _vec(ca, ("dx", "dy", "dz")), _vec(cb, ("dx", "dy", "dz"))
        if da is not None and db is not None and np.linalg.norm(da) > 0 and np.linalg.norm(db) > 0:
            c = float(np.clip(np.dot(da, db) / np.linalg.norm(da) / np.linalg.norm(db), -1, 1))
            ang = math.degrees(math.acos(c))
            res["deltas"]["dir_deg"] = round(ang, 4)
            if ang > tol["dir_deg"]:
                res["problems"].append(f"view direction differs by {ang:.3g} deg > {tol['dir_deg']}")
        if "fov" in ca and "fov" in cb:
            d = abs(float(ca["fov"]) - float(cb["fov"]))
            res["deltas"]["fov"] = round(d, 5)
            if d > tol["fov"]:
                res["problems"].append(f"fov differs by {d:.3g} > {tol['fov']}")
    else:
        res["unchecked"].append("camera")
    wa, wb = ma.get("weather"), mb.get("weather")
    if isinstance(wa, dict) and isinstance(wb, dict):
        if "mode" in wa and "mode" in wb and wa["mode"] != wb["mode"]:
            res["problems"].append(f"weather mode {wa['mode']} vs {wb['mode']}")
        ka, kb = wa.get("weights") or {}, wb.get("weights") or {}
        d = max((abs(float(ka.get(k, 0)) - float(kb.get(k, 0))) for k in set(ka) | set(kb)), default=0.0)
        res["deltas"]["weather"] = round(d, 5)
        if d > tol["weather"]:
            res["problems"].append(f"weather weights differ by {d:.3g} > {tol['weather']}")
    elif isinstance(wa, str) and isinstance(wb, str):
        if wa != wb:
            res["problems"].append(f"weather {wa} vs {wb}")
    else:
        res["unchecked"].append("weather")
    ta, tb = _clock_minutes(ma.get("time")), _clock_minutes(mb.get("time"))
    if ta is not None and tb is not None:
        d = abs(ta - tb) % 1440
        d = min(d, 1440 - d)
        res["deltas"]["clock_min"] = round(d, 3)
        if d > tol["clock_min"]:
            res["problems"].append(f"clock differs by {d:.3g} min > {tol['clock_min']}")
    else:
        res["unchecked"].append("time")
    res["ok"] = not res["problems"]
    return res


# ---------------------------------------------------------------------------------------------------------------------
# comparison of one spot


def known_noise_mask(frames_a: list[np.ndarray], frames_a2: list[np.ndarray], noise_cfg: dict) -> np.ndarray:
    """Pixels that differ between two reference runs of the same spot (> known_tol, max channel, burst medians) or are
    unstable within either burst, dilated by known_dilate: what the game itself does not reproduce from run to run
    (sea, clouds, idle animation phases that the time freeze stops at a different point in every run)."""
    rep_a, un_a, _ = prepare_burst(frames_a, noise_cfg)
    rep_b, un_b, _ = prepare_burst(frames_a2, noise_cfg)
    if rep_a.shape != rep_b.shape:
        raise ValueError(f"size mismatch {rep_a.shape} vs {rep_b.shape}")
    mask = (np.abs(rep_b - rep_a).max(axis=2) > noise_cfg.get("known_tol", 8)) | un_a | un_b
    r = int(noise_cfg.get("known_dilate", 4))
    if mask.any() and r > 0:
        mask = ndimage.binary_dilation(mask, structure=np.ones((2 * r + 1, 2 * r + 1), bool))
    return mask


def load_mask(path: str | Path) -> np.ndarray:
    with Image.open(path) as im:
        return np.asarray(im.convert("L")) >= 128


def save_mask(path: Path, mask: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray((mask.astype(np.uint8) * 255)).save(path)


def compare_spot(spot: str, files_a: list[Path] | None, files_b: list[Path] | None, thr: dict,
                 meta_a: dict | None = None, meta_b: dict | None = None, out_dir: Path | None = None,
                 tile: int = 32, ntiles: int = 10, frames_a=None, frames_b=None, known: np.ndarray | None = None) -> dict:
    """Files or preloaded frames (lists of float32 HxWx3 arrays) per side. `known`: a known-noise mask (HxW bool, from a
    baseline run) excluded from the metrics like the burst mask; default: the thresholds' `known_mask` file. -> result dict."""
    res: dict = {"spot": spot, "verdict": "INVALID", "reasons": [], "known": thr.get("known"),
                 "files_a": [p.name for p in files_a or []], "files_b": [p.name for p in files_b or []],
                 "images": {}}
    if frames_a is None and files_a:
        frames_a = [load_rgb(p) for p in files_a]
    if frames_b is None and files_b:
        frames_b = [load_rgb(p) for p in files_b]
    if not frames_a or not frames_b:
        res["reasons"].append("missing in " + ("A" if not frames_a else "B"))
        return res
    for side, fr in (("A", frames_a), ("B", frames_b)):
        if len({f.shape for f in fr}) != 1:
            res["reasons"].append(f"burst frames of {side} differ in size")
            return res
    if frames_a[0].shape != frames_b[0].shape:
        sa, sb = frames_a[0].shape, frames_b[0].shape
        res["reasons"].append(f"size mismatch: A {sa[1]}x{sa[0]}, B {sb[1]}x{sb[0]}")
        return res
    pose = check_pose(meta_a, meta_b, thr["pose"])
    res["pose"] = pose
    rep_a, un_a, info_a = prepare_burst(frames_a, thr["noise"])
    rep_b, un_b, info_b = prepare_burst(frames_b, thr["noise"])
    res["burst"] = {"a": info_a, "b": info_b}
    bmask = un_a | un_b
    if bmask.any() and thr["noise"]["dilate"] > 0:
        r = int(thr["noise"]["dilate"])
        bmask = ndimage.binary_dilation(bmask, structure=np.ones((2 * r + 1, 2 * r + 1), bool))
    if known is None and thr.get("known_mask"):
        try:
            known = load_mask(thr["known_mask"])
        except OSError as ex:
            res["reasons"].append(f"known mask unreadable: {ex}")
            return res
    if known is not None and known.shape != bmask.shape:
        res["reasons"].append(f"known mask size {known.shape[1]}x{known.shape[0]} differs from the pictures")
        return res
    mask = bmask | known if known is not None else bmask
    metrics, maps = compute_metrics(rep_a, rep_b, ~mask, tile=tile, ntiles=ntiles)
    metrics["noise_burst"] = round(100.0 * float(bmask.mean()), 3)
    metrics["noise_known"] = round(100.0 * float(known.mean()), 3) if known is not None else 0.0
    res["metrics"] = metrics
    verdict, reasons = verdict_for(metrics, thr)
    if not pose["ok"]:
        verdict, reasons = "INVALID", ["pose/state mismatch: " + "; ".join(pose["problems"])] + reasons
    res["verdict"], res["reasons"] = verdict, reasons
    if out_dir is not None:
        res["images"] = write_images(out_dir, spot, rep_a, rep_b, maps, mask, metrics)
    return res


# ---------------------------------------------------------------------------------------------------------------------
# images

RAMP = np.array([[0, 0, 0], [40, 60, 255], [255, 230, 0], [255, 0, 0]], np.float32)  # black-blue-yellow-red


def heatmap(ref: np.ndarray, dmax: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """Max-channel difference (x4, clamped at 64) coloured over the desaturated reference at 50 %; masked hatched."""
    t = np.clip(dmax / 64.0, 0, 1) * (len(RAMP) - 1)
    i = np.minimum(t.astype(int), len(RAMP) - 2)
    f = (t - i)[..., None]
    col = RAMP[i] * (1 - f) + RAMP[i + 1] * f
    alpha = np.clip(dmax / 8.0, 0, 1)[..., None]  # small differences fade in
    base = (luma(ref) * 0.5)[..., None].repeat(3, axis=2)
    out = base * (1 - alpha) + col * alpha
    if mask.any():
        yy, xx = np.indices(mask.shape)
        hatch = np.where(((xx + yy) // 4) % 2 == 0, 150, 90).astype(np.float32)
        out[mask] = hatch[mask][:, None]
    return np.clip(out, 0, 255).astype(np.uint8)


def to_u8(img: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


def label(im: Image.Image, text: str) -> Image.Image:
    d = ImageDraw.Draw(im)
    box = d.textbbox((0, 0), text)
    d.rectangle((0, 0, box[2] + 8, box[3] + 6), fill=(0, 0, 0))
    d.text((4, 2), text, fill=(255, 255, 255))
    return im


def thumb_b64(img: Image.Image, width: int = 240) -> str:
    t = img.copy()
    t.thumbnail((width, width * 4))
    buf = io.BytesIO()
    t.convert("RGB").save(buf, "JPEG", quality=80)
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode("ascii")


def write_images(out: Path, spot: str, a: np.ndarray, b: np.ndarray, maps: dict, mask: np.ndarray, metrics: dict) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    ia, ib = Image.fromarray(to_u8(a)), Image.fromarray(to_u8(b))
    heat = Image.fromarray(heatmap(a, maps["dmax"], mask))
    files = {"diff": f"{spot}_diff.png", "ssim": f"{spot}_ssim.png", "blink": f"{spot}_blink.gif",
             "side": f"{spot}_side.jpg", "a": f"{spot}_a.png", "b": f"{spot}_b.png"}
    heat.save(out / files["diff"])
    s = np.clip((1 - maps["ssim"]) * 4, 0, 1)  # dissimilarity, 0.25 -> white
    Image.fromarray((s * 255).astype(np.uint8)).save(out / files["ssim"])
    ia.save(out / files["a"])
    ib.save(out / files["b"])
    label(ia.copy(), "A").save(out / files["blink"], save_all=True, append_images=[label(ib.copy(), "B")],
                               duration=1000, loop=0)
    w, h = ia.size
    half = (max(1, w // 2), max(1, h // 2))
    side = Image.new("RGB", (half[0] * 3, half[1]))
    for k, im in enumerate((ia, ib, heat)):
        side.paste(label(im.resize(half, Image.Resampling.BOX), "ABD"[k]), (k * half[0], 0))
    side.save(out / files["side"], quality=90)
    if metrics.get("tiles"):
        files["tiles"] = f"{spot}_tiles.png"
        rows = []
        for t in metrics["tiles"][:3]:
            pad = 16
            x0, y0 = max(0, t["x"] - pad), max(0, t["y"] - pad)
            x1, y1 = min(w, t["x"] + t["w"] + pad), min(h, t["y"] + t["h"] + pad)
            crops = [im.crop((x0, y0, x1, y1)) for im in (ia, ib, heat)]
            sc = 4
            row = Image.new("RGB", ((x1 - x0) * sc * 3 + 8, (y1 - y0) * sc), (40, 40, 40))
            for k, c in enumerate(crops):
                row.paste(c.resize(((x1 - x0) * sc, (y1 - y0) * sc), Image.Resampling.NEAREST), (k * ((x1 - x0) * sc + 4), 0))
            rows.append(label(row, f"tile x={t['x']} y={t['y']} mae={t['mae']}  A | B | diff"))
        tw = max(r.size[0] for r in rows)
        sheet = Image.new("RGB", (tw, sum(r.size[1] + 4 for r in rows)), (40, 40, 40))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.size[1] + 4
        sheet.save(out / files["tiles"])
    files["thumbs"] = {"a": thumb_b64(ia), "b": thumb_b64(ib), "diff": thumb_b64(heat)}
    return files


# ---------------------------------------------------------------------------------------------------------------------
# reports

SEVERITY = {"FAIL": 0, "INVALID": 1, "WARN": 2, "PASS": 3}
COLOURS = {"FAIL": "#d33", "INVALID": "#a5a", "WARN": "#d90", "PASS": "#3a3"}


def sort_results(results: list[dict]) -> list[dict]:
    return sorted(results, key=lambda r: (SEVERITY[r["verdict"]], r.get("metrics", {}).get("ssim", 0), r["spot"]))


def exit_code(results: list[dict]) -> int:
    if not results or any(r["verdict"] == "INVALID" for r in results):
        return 2
    return 1 if any(r["verdict"] == "FAIL" for r in results) else 0


def summarize(results):
    counts = {k: 0 for k in SEVERITY}
    for r in results:
        counts[r["verdict"]] += 1
    return counts


def write_reports(out: Path, results: list[dict], header: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    results = sort_results(results)
    counts = summarize(results)
    js = {**header, "summary": counts, "exit_code": exit_code(results),
          "spots": [{k: v for k, v in r.items() if k != "images"} | {"images": {k: v for k, v in r.get("images", {}).items()
                                                                                 if k != "thumbs"}} for r in results]}
    (out / "report.json").write_text(json.dumps(js, indent=1), encoding="utf-8")
    (out / "report.html").write_text(render_html(results, header, counts), encoding="utf-8")
    lines = [f"refcompare {header['generated']}: " + ", ".join(f"{v} {k}" for k, v in counts.items() if v), "",
             "| spot | verdict | ssim | psnr_q | bad32 % | dluma | shift | masked % (burst+known) | reasons |", "|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        m = r.get("metrics")
        if m:
            sh = f"{m['shift'][0]},{m['shift'][1]}" + ("" if m["shift_significant"] else "?" if m["shift"] != [0, 0] else "")
            lines.append(f"| {r['spot']} | {r['verdict']} | {m['ssim']:.4f} | {m['psnr_q']:.1f} | {m['bad32']:.2f} | "
                         f"{m['dluma']:+.2f} | {sh} | {m['noise']:.1f} ({m.get('noise_burst', m['noise']):.1f}+{m.get('noise_known', 0):.1f}) | {'; '.join(r['reasons'])} |")
        else:
            lines.append(f"| {r['spot']} | {r['verdict']} | | | | | | | {'; '.join(r['reasons'])} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_html(results, header, counts) -> str:
    e = html.escape
    rows = []
    for r in results:
        m = r.get("metrics") or {}
        im = r.get("images") or {}
        th = im.get("thumbs") or {}

        def img(key, link):
            if key not in th:
                return "<td></td>"
            return f'<td><a href="{e(im.get(link, ""))}"><img src="{th[key]}"></a></td>'

        if m:
            mt = (f"ssim {m['ssim']:.4f}<br>psnr {m['psnr']:.1f} / q {m['psnr_q']:.1f} dB<br>mae {m['mae']:.2f}<br>"
                  f"bad8 {m['bad8']:.2f} % bad32 {m['bad32']:.3f} %<br>dluma {m['dluma']:+.2f} "
                  f"rgb {', '.join(f'{x:+.2f}' for x in m['bias_rgb'])}<br>"
                  f"shift {m['shift']} {'<b>significant</b>' if m['shift_significant'] else ''} (sub {m['shift_sub']})<br>"
                  f"noise {m['noise']:.2f} % (burst {m.get('noise_burst', m['noise']):.2f}, known {m.get('noise_known', 0):.2f})")
            tiles = "<br>".join(f"({t['x']},{t['y']}) mae {t['mae']} ssim {t['ssim']}" for t in m.get("tiles", [])[:5])
        else:
            mt, tiles = "", ""
        pose = r.get("pose") or {}
        pz = e(", ".join(f"{k} {v}" for k, v in pose.get("deltas", {}).items()))
        if pose.get("unchecked"):
            pz += "<br><small>unchecked: " + e(", ".join(pose["unchecked"])) + "</small>"
        links = " ".join(f'<a href="{e(im[k])}">{k}</a>' for k in ("diff", "ssim", "blink", "side", "tiles", "a", "b") if k in im)
        reasons = "<br>".join(e(x) for x in r["reasons"])
        known = f'<div class="known">known: {e(r["known"])}</div>' if r.get("known") else ""
        rows.append(f'<tr><td class="v" style="background:{COLOURS[r["verdict"]]}">{r["verdict"]}</td>'
                    f'<td><b>{e(r["spot"])}</b><br><small>{e(", ".join(r["files_a"]))}<br>{e(", ".join(r["files_b"]))}</small>'
                    f'<br>{links}</td><td>{reasons}{known}</td><td class="m">{mt}</td><td class="m">{tiles}</td>'
                    f'<td class="m">{pz}</td>{img("a", "a")}{img("b", "b")}{img("diff", "diff")}</tr>')
    summ = " ".join(f'<span class="v" style="background:{COLOURS[k]}">{v} {k}</span>' for k, v in counts.items() if v)
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>refcompare {e(header['generated'])}</title>
<style>
body {{ font: 13px sans-serif; background: #222; color: #ddd; }}
a {{ color: #8cf; }}
table {{ border-collapse: collapse; }}
td, th {{ border: 1px solid #444; padding: 4px; vertical-align: top; }}
.v {{ color: #fff; font-weight: bold; padding: 2px 6px; }}
.m {{ font-family: monospace; white-space: nowrap; }}
.known {{ color: #fc6; }}
img {{ display: block; }}
</style></head><body>
<h2>refcompare: {summ}</h2>
<p>A (reference) <code>{e(str(header['a']))}</code><br>B (test) <code>{e(str(header['b']))}</code><br>
thresholds <code>{e(str(header.get('thresholds') or 'built-in defaults'))}</code>{' + baseline <code>' + e(str(header['baseline'])) + '</code>' if header.get('baseline') else ''}
<br>generated {e(header['generated'])}. Heat map: max channel difference, black-blue-yellow-red clamped at 64, over the
reference at 50 %; hatched = masked (unstable within a burst, or known noise: differs between two reference runs). B - A for biases; shift = B's content offset relative to A.</p>
<table><tr><th>verdict</th><th>spot</th><th>reasons</th><th>metrics</th><th>worst tiles</th><th>pose deltas</th>
<th>A</th><th>B</th><th>diff</th></tr>
{chr(10).join(rows)}
</table></body></html>
"""


# ---------------------------------------------------------------------------------------------------------------------
# drivers


def side(path: Path, meta_arg: str | None, tag_arg: str | None) -> tuple[dict, dict, str | None]:
    """-> (images by spot, metadata by spot, tag)."""
    meta = load_meta(Path(meta_arg) if meta_arg else (path / "meta.json" if path.is_dir() else None))
    tag = tag_arg or next((str(e["tag"]) for e in meta.values() if e.get("tag")), None)
    return scan_images(path), meta, tag


def gather(a: Path, b: Path, args) -> tuple[dict, dict, dict, dict]:
    ia, meta_a, tag_a = side(a, args.meta_a, args.tag_a)
    ib, meta_b, tag_b = side(b, args.meta_b, args.tag_b)
    ga, gb = pair_spots(ia, ib, tag_a, tag_b)
    if getattr(args, "spots", None):
        keep = set(args.spots.split(","))
        ga = {k: v for k, v in ga.items() if k in keep}
        gb = {k: v for k, v in gb.items() if k in keep}
    return ga, gb, meta_a, meta_b


def run_pairs(ga, gb, meta_a, meta_b, thresholds: Thresholds, out: Path | None, args,
              known_masks: dict | None = None) -> list[dict]:
    results = []
    for spot in sorted(set(ga) | set(gb)):
        r = compare_spot(spot, ga.get(spot), gb.get(spot), thresholds.for_spot(spot), meta_a.get(spot), meta_b.get(spot),
                         out, tile=args.tile, ntiles=args.ntiles, known=(known_masks or {}).get(spot))
        results.append(r)
        m = r.get("metrics")
        extra = (f"ssim {m['ssim']:.4f} psnr_q {m['psnr_q']:.1f} bad32 {m['bad32']:.2f}% dluma {m['dluma']:+.2f} "
                 f"shift {m['shift']}{'!' if m['shift_significant'] else ''} noise {m['noise_burst']:.1f}%"
                 f"+known {m['noise_known']:.1f}%") if m else ""
        print(f"{r['verdict']:7} {spot:32} {extra}  {'; '.join(r['reasons'])}", flush=True)
    return results


def derive_thresholds(base: dict, m: dict, margin: float) -> dict:
    """Per-spot overrides so that A vs A' (the noise floor) x margin still passes; never stricter than base."""
    o: dict = {}
    db = 20 * math.log10(margin) if margin > 0 else 0.0
    floor = {
        "ssim_min": 1 - margin * (1 - m["ssim"]), "psnr_min": m["psnr"] - db, "psnr_q_min": m["psnr_q"] - db,
        "mae_max": margin * m["mae"], "bad8_max": margin * m["bad8"], "bad32_max": margin * m["bad32"],
        "dluma_abs_max": margin * abs(m["dluma"]), "chroma_abs_max": margin * max(abs(c) for c in m["chroma"]),
        "shift_max": max(abs(v) for v in m["shift"]) if m["shift_significant"] else 0,
    }
    for tier in ("pass", "warn"):
        for key, limit in base[tier].items():
            if key not in floor:
                continue
            new = min(limit, floor[key]) if RULES[key][1] == ">=" else max(limit, floor[key])
            if new != limit:
                o.setdefault(tier, {})[key] = round(new, 5) if isinstance(new, float) else new
    return o


def calibrate_overrides(ga, gb, meta_a, meta_b, thresholds: Thresholds, margin: float, args,
                        use_known: bool = True) -> tuple[dict, list, dict]:
    """A vs A' (two reference runs) per spot -> (threshold overrides, results, known-noise masks). With `use_known` the
    pixels that differ between the runs become the spot's known-noise mask and the floor metrics are taken outside it."""
    overrides, results, masks = {}, [], {}
    for spot in sorted(set(ga) & set(gb)):
        thr = thresholds.for_spot(spot)
        thr.pop("known_mask", None)  # the mask is being made here
        fa, fb = [load_rgb(p) for p in ga[spot]], [load_rgb(p) for p in gb[spot]]
        known = None
        if use_known and fa and fb and len({f.shape for f in fa + fb}) == 1:
            known = known_noise_mask(fa, fb, thr["noise"])
            masks[spot] = known
        r = compare_spot(spot, ga[spot], gb[spot], thr, meta_a.get(spot), meta_b.get(spot), None,
                         tile=args.tile, ntiles=0, frames_a=fa, frames_b=fb, known=known)
        results.append(r)
        if "metrics" not in r or not r.get("pose", {}).get("ok", True):
            # a pose/state mismatch between the reference runs is a scenario problem: no floor, no mask from it
            masks.pop(spot, None)
            print(f"skip    {spot:32} {'; '.join(r['reasons'])}")
            continue
        m = r["metrics"]
        o = derive_thresholds(thr, m, margin)
        o["baseline"] = {k: m[k] for k in ("ssim", "psnr", "psnr_q", "mae", "bad8", "bad32", "dluma", "chroma", "shift",
                                           "shift_significant", "noise", "noise_burst", "noise_known")}
        overrides[spot] = o
        print(f"floor   {spot:32} ssim {m['ssim']:.4f} psnr_q {m['psnr_q']:.1f} bad32 {m['bad32']:.2f}% "
              f"dluma {m['dluma']:+.2f} noise {m['noise_burst']:.1f}% known {m['noise_known']:.1f}% "
              f"-> {({k: v for k, v in o.items() if k != 'baseline'}) or 'defaults'}")
    return overrides, results, masks


def cmd_compare(args) -> int:
    a, b, out = Path(args.a), Path(args.b), Path(args.out)
    thresholds = Thresholds.load(args.thresholds)
    ga, gb, meta_a, meta_b = gather(a, b, args)
    masks = None
    if args.baseline:
        ga2, gb2, _, meta_a2 = gather(a, Path(args.baseline),
                                      argparse.Namespace(**{**vars(args), "meta_b": args.meta_baseline, "tag_b": None}))
        print("baseline (A vs A'):")
        overrides, _, masks = calibrate_overrides(ga2, gb2, meta_a, meta_a2, thresholds, args.margin, args,
                                                  use_known=not args.no_known_mask)
        thresholds = thresholds.with_spot_overrides(overrides)
        print("comparison (A vs B):")
    results = run_pairs(ga, gb, meta_a, meta_b, thresholds, None if args.no_images else out, args, known_masks=masks)
    header = {"generated": datetime.datetime.now().isoformat(timespec="seconds"), "a": str(a), "b": str(b),
              "thresholds": args.thresholds, "baseline": args.baseline, "meta_a": bool(meta_a), "meta_b": bool(meta_b)}
    write_reports(out, results, header)
    code = exit_code(results)
    print(f"{summarize(results)} -> exit {code}; report {out / 'report.html'}")
    return code


def cmd_calibrate(args) -> int:
    a, a2 = Path(args.a), Path(args.a2)
    thresholds = Thresholds.load(args.thresholds)
    ga, gb, meta_a, meta_b = gather(a, a2, args)
    overrides, results, masks = calibrate_overrides(ga, gb, meta_a, meta_b, thresholds, args.margin, args,
                                                    use_known=not args.no_known_mask)
    out = Path(args.out)
    mask_dir = Path(args.mask_dir) if args.mask_dir else out.with_name(out.stem + "_masks")
    for spot, mk in masks.items():
        p = mask_dir / f"{spot}_known.png"
        save_mask(p, mk)
        try:
            rel = p.resolve().relative_to(out.resolve().parent).as_posix()
        except ValueError:
            rel = str(p.resolve())
        overrides.setdefault(spot, {})["known_mask"] = rel
    spots = {**thresholds.spots}
    for spot, o in overrides.items():
        spots[spot] = deep_merge(spots.get(spot, {}), o)
    data = {"generated": datetime.datetime.now().isoformat(timespec="seconds"), "source": [str(a), str(a2)],
            "margin": args.margin, "default": thresholds.default, "spot": spots}
    Path(args.out).write_text(json.dumps(data, indent=1), encoding="utf-8")
    print(f"{len(overrides)} spots calibrated -> {args.out}")
    return 0 if overrides else 2


# ---------------------------------------------------------------------------------------------------------------------
# self-test (synthetic images; the unit tests in tools/tests/test_refcompare.py go further)


def synthetic(h: int = 144, w: int = 256, seed: int = 1) -> np.ndarray:
    """A textured RGB image: smoothed noise plus hard edges (rectangles)."""
    rng = np.random.default_rng(seed)
    base = ndimage.gaussian_filter(rng.random((h, w, 3)) * 255, (2, 2, 0)) * 2 - 128
    img = np.clip(base + 64, 0, 255)
    for _ in range(12):
        y, x = rng.integers(0, h - 20), rng.integers(0, w - 20)
        img[y:y + rng.integers(4, 20), x:x + rng.integers(4, 20)] = rng.integers(0, 256, 3)
    return np.rint(img).astype(np.float32)


def shifted(img: np.ndarray, dx: int, dy: int) -> np.ndarray:
    """Content moved by (dx, dy), edges replicated (not circular)."""
    h, w = img.shape[:2]
    ys = np.clip(np.arange(h) - dy, 0, h - 1)
    xs = np.clip(np.arange(w) - dx, 0, w - 1)
    return img[ys][:, xs]


def selftest_cases() -> list[tuple[str, list, list, set]]:
    a = synthetic()
    sq = a.copy()
    sq[40:80, 100:140] = 0
    noisy = [a.copy() for _ in range(3)]
    rng = np.random.default_rng(7)
    for f in noisy:
        f[10:40, 10:60] = rng.integers(0, 256, (30, 50, 3))
    return [
        ("identical", [a], [a.copy()], {"PASS"}),
        ("brightness+1", [a], [np.clip(a + 1, 0, 255)], {"PASS", "WARN"}),
        ("brightness+5", [a], [np.clip(a + 5, 0, 255)], {"FAIL"}),
        ("shift1px", [a], [shifted(a, 1, 0)], {"WARN", "FAIL"}),
        ("missing_square", [a], [sq], {"FAIL"}),
        ("burst_noise", noisy[:3], [n.copy() for n in noisy[::-1]], {"PASS"}),
        ("size", [a], [a[:-4]], {"INVALID"}),
    ]


def cmd_selftest(_args) -> int:
    thr = Thresholds().for_spot("x")
    bad = 0
    for name, fa, fb, want in selftest_cases():
        r = compare_spot(name, None, None, thr, frames_a=fa, frames_b=fb)
        ok = r["verdict"] in want
        bad += not ok
        m = r.get("metrics") or {}
        print(f"{'ok ' if ok else 'BAD'} {name:16} {r['verdict']:7} (want {'/'.join(sorted(want))}) "
              f"ssim {m.get('ssim', '-')} dluma {m.get('dluma', '-')} shift {m.get('shift', '-')}"
              f"{'!' if m.get('shift_significant') else ''} {'; '.join(r['reasons'])}")
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--thresholds", help="JSON or TOML thresholds (default: built-in)")
        p.add_argument("--meta-a", help="metadata of A (RVKCMP log lines / JSON; default <A>/meta.json)")
        p.add_argument("--meta-b", help="metadata of B (default <B>/meta.json)")
        p.add_argument("--tag-a", help="file name prefix of A to strip (<tag>_<spot>)")
        p.add_argument("--tag-b", help="file name prefix of B to strip")
        p.add_argument("--spots", help="comma-separated spot ids (default: all)")
        p.add_argument("--margin", type=float, default=1.5, help="noise floor multiplier for calibration (1.5)")
        p.add_argument("--tile", type=int, default=32, help="tile size for the worst-tile list (32)")
        p.add_argument("--ntiles", type=int, default=10, help="worst tiles to list (10)")
        p.add_argument("--no-known-mask", action="store_true",
                       help="baseline/calibrate: thresholds only, no known-noise mask from A vs A'")

    p = sub.add_parser("compare", help="compare A (reference) with B (test)")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("-o", "--out", required=True, help="report directory")
    p.add_argument("--baseline", help="a second reference run A': per-spot known-noise masks (pixels that differ "
                   "between A and A') and thresholds from A vs A' (x margin)")
    p.add_argument("--meta-baseline", help="metadata of the baseline run (default <baseline>/meta.json)")
    p.add_argument("--no-images", action="store_true", help="metrics and reports only")
    common(p)
    p.set_defaults(fn=cmd_compare)

    p = sub.add_parser("calibrate", help="derive per-spot thresholds from two runs of the same renderer")
    p.add_argument("a")
    p.add_argument("a2")
    p.add_argument("-o", "--out", required=True, help="thresholds JSON to write")
    p.add_argument("--mask-dir", help="where the known-noise masks go (default <out stem>_masks next to the JSON)")
    common(p)
    p.set_defaults(fn=cmd_calibrate)

    p = sub.add_parser("selftest", help="synthetic images must give the expected verdicts")
    p.set_defaults(fn=cmd_selftest)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
