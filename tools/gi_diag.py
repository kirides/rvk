"""Per-spot table of the screen probes' light decomposition (RVK_GI_DIAG=1, docs/research/rtgi.md 23).

Usage: python -I tools/gi_diag.py <water-compare dir with enh/ and enh/rvk.log> [more dirs]

The diag lines of rvk.log (every 5 s, per sun class shaded / sun-lit) are matched with the capture time of each spot's first
picture (file time; the spot is frozen while it is held): the window ending first after the capture is the spot's.
Printed per spot and class: probes, the engine's ambient, the traced irradiance (share of the engine's), far-field rays
(share of rays, light carried, the engine's sky on the same rays), hit rays (share, light carried, mean albedo), the
back-face and near-hit shares.
"""
import datetime
import os
import re
import sys

LINE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d\.\d+)\].*enh: gi diag \[(shaded|sun-lit)\] (\d+) probes: engine ambient ([\d.]+), ddgi ([\d.]+), traced ([\d.]+) "
    r"\(uncapped ([\d.]+)\) = (\d+)% of the engine's; far field ([\d.]+) \((\d+)% of rays; the engine's sky on them ([\d.]+), far field towards \+dir ([\d.]+), "
    r"as radiance ([\d.]+)\); hits (\d+)% of rays: sun ([\d.]+) point ([\d.]+) bounce ([\d.]+) emissive ([\d.]+) \(hit rays' mean radiance ([\d.]+), mean albedo\^2.2 ([\d.]+)\); "
    r"back faces (\d+)%; hits < 2 m (\d+)%, < 10 m (\d+)%; all rays as the engine's sky ([\d.]+)")


def secs(h, m, s):
    return int(h) * 3600 + int(m) * 60 + float(s)


def main():
    for d in sys.argv[1:]:
        enh = os.path.join(d, "enh")
        log = os.path.join(enh, "rvk.log")
        if not os.path.exists(log):
            log = os.path.join(d, "enh", "rvk.log")
        rows = []
        for ln in open(log, encoding="utf-8", errors="replace"):
            m = LINE.search(ln)
            if m:
                g = m.groups()
                rows.append((secs(*g[:3]), g[3], [float(x) for x in g[4:]]))
        caps = {}
        for f in os.listdir(enh):
            mm = re.match(r"enh_(.+?)(_b0)?\.png$", f)
            if mm and not re.search(r"_b[1-9]\.png$", f):
                t = datetime.datetime.fromtimestamp(os.path.getmtime(os.path.join(enh, f)))
                caps[mm.group(1)] = t.hour * 3600 + t.minute * 60 + t.second + t.microsecond / 1e6
        print(os.path.basename(os.path.normpath(d)))
        print(f"{'spot':18s} {'class':8s} {'probes':>8s} {'engine':>6s} {'traced':>6s} {'%eng':>5s} | {'far%':>4s} {'farL':>6s} {'engSky':>6s} {'+dir':>6s} {'rad':>6s} | "
              f"{'hit%':>4s} {'hitL':>6s} {'sun':>6s} {'bnc':>6s} {'alb':>6s} {'bf%':>3s} {'<2m':>3s} {'<10':>3s}")
        for spot, t in sorted(caps.items()):
            sel = [r for r in rows if t - 1.0 <= r[0] <= t + 6.0]
            if not sel:
                continue
            end = min(r[0] for r in sel)
            for r in sel:
                if r[0] != end:
                    continue
                v = r[2]
                print(f"{spot:18s} {r[1]:8s} {int(v[0]):8d} {v[1]:6.3f} {v[3]:6.3f} {v[5]:5.0f} | {v[7]:4.0f} {v[6]:6.3f} {v[8]:6.3f} {v[9]:6.3f} {v[10]:6.3f} | "
                      f"{v[11]:4.0f} {v[12] + v[13] + v[14] + v[15]:6.3f} {v[12]:6.3f} {v[14]:6.3f} {v[17]:6.3f} {v[18]:3.0f} {v[19]:3.0f} {v[20]:3.0f}")


if __name__ == "__main__":
    main()
