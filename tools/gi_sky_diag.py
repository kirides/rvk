"""Per-spot table of the sky terms of the screen probes' diagnostics (RVK_GI_DIAG=1, docs/research/rtgi.md 23 / 27).

Usage: python -I tools/gi_sky_diag.py <run dir with rvk.log + spass.log> [more dirs]

Each spot's window = the last 5 s diag line pair (shaded / sun-lit) logged before the scenario's RVKCMP capture line (the spot is
frozen while it is held). Printed per spot and class: probes, far-ray share, the far rays' mean radiance as used, the engine's
hemisphere sky on the same rays, the sky cube along the same rays (raw display value and ^2.2 = acc units), the traced irradiance
as a share of the engine's ambient.
"""
import json
import os
import re
import sys

DIAG = re.compile(
    r"\[(\d\d):(\d\d):(\d\d\.\d+)\].*enh: gi diag \[(shaded|sun-lit)\] (\d+) probes: engine ambient ([\d.]+), ddgi ([\d.]+), traced ([\d.]+) "
    r"\(uncapped ([\d.]+)\) = (\d+)% of the engine's; far field ([\d.]+) \((\d+)% of rays; the engine's sky on them ([\d.]+), far field towards \+dir ([\d.]+), "
    r"as radiance ([\d.]+)\);.*all rays as the engine's sky ([\d.]+)(?:; sky cube: far rays \(per far ray\) raw ([\d.]+) \^2.2 ([\d.]+), all rays \^2.2 ([\d.]+)(?:; cube / engine sky on far rays: mean ([\d.]+), rms ([\d.]+))?)?")
CMP = re.compile(r"\[(\d\d):(\d\d):(\d\d\.\d+)\].*RVKCMP (\{.*\})$")


def secs(h, m, s):
    return int(h) * 3600 + int(m) * 60 + float(s)


def main():
    for d in sys.argv[1:]:
        rows, caps = [], []
        for ln in open(os.path.join(d, "rvk.log"), encoding="utf-8", errors="replace"):
            m = DIAG.search(ln)
            if m:
                g = m.groups()
                rows.append((secs(*g[:3]), g[3], [float(x) if x is not None else 0.0 for x in g[4:]]))
        for ln in open(os.path.join(d, "spass.log"), encoding="utf-8", errors="replace"):
            m = CMP.search(ln)
            if m:
                try:
                    j = json.loads(m.group(4))
                except ValueError:
                    continue
                caps.append((secs(*m.groups()[:3]), j.get("id", "?")))
        print(os.path.basename(os.path.normpath(d)))
        print(f"{'spot':16s} {'class':8s} {'probes':>7s} {'far%':>4s} | {'used':>6s} {'engSky':>6s} {'cubeRaw':>7s} {'cube^2.2':>8s} | {'allEng':>6s} {'allCube':>7s} | {'ambient':>7s} {'traced':>6s} {'%eng':>4s} | {'q mean':>6s} {'q cv':>5s}")
        for t, spot in caps:
            sel = [r for r in rows if t - 7.0 <= r[0] <= t + 0.5]
            if not sel:
                continue
            end = max(r[0] for r in sel)
            for r in sel:
                if r[0] != end:
                    continue
                v = r[2]
                far = max(v[7], 1e-9) / 100.0  # share of rays
                # used / engSky / +dir values are sums over all rays (weights = far share): per far ray
                print(f"{spot:16s} {r[1]:8s} {int(v[0]):7d} {v[7]:4.0f} | {v[6] / far:6.3f} {v[8] / far:6.3f} {v[12]:7.3f} {v[13]:8.3f} | "
                      f"{v[11]:6.3f} {v[14]:7.3f} | {v[1]:7.3f} {v[3]:6.3f} {v[5]:4.0f} | {v[15]:6.3f} {((max(v[16] ** 2 - v[15] ** 2, 0.0)) ** 0.5 / v[15]) if v[15] > 0 else 0.0:5.2f}")

if __name__ == "__main__":
    main()
