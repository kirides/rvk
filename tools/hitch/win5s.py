"""win5s.py (WS30): per 5 s of a traversal session (route seconds), the engine-side counters that explain a slow window.
  python win5s.py <tag> [from to]     (<WS26 dir>/<tag>_rvk.log, _frames.csv, _spass.log)
Columns: route second, frame p50 / p95 ms, mean CPU ms, mean thread CPU ms (frames.csv 7th column, WS30 builds), variant queue, built, pending ops composed late (per s),
packets replayed share (static meshes), variant-decision refusals, rt gpu build instances / frame, input / row writes per frame, instance+record CPU ms, owned share (main z)."""
import sys, re, os, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census

tag = sys.argv[1]
a = float(sys.argv[2]) if len(sys.argv) > 2 else 0
b = float(sys.argv[3]) if len(sys.argv) > 3 else 100
lines, rows, t0, t1 = census.load(tag)
thr = {}
for l in open(os.path.join(census.D, tag + '_frames.csv')).read().splitlines()[1:]:
    f = l.split(',')
    if len(f) >= 7:
        try: thr[int(f[1])] = float(f[6])
        except ValueError: pass


def num(rx, txt, g=1, cast=float):
    m = re.search(rx, txt)
    return cast(m.group(g)) if m else None


print('t     p50   p95   cpu  thrcpu | queue built | late/s | pk.replay  refus.var | rt inst  wr.in  wr.row  rtcpu | owned-z')
for w0 in range(int(a), int(b), 5):
    lo, hi = t0 + w0 * 1000, t0 + (w0 + 5) * 1000
    rr = [r for r in rows if lo <= r[1] < hi]
    if not rr: continue
    iv = [r[2] for r in rr]
    cpu = statistics.mean(r[3] for r in rr)
    tc = [thr[r[1]] for r in rr if r[1] in thr]
    q = built = late = rep = ref = inst = wi = wr = rtc = oz = None
    lates = []
    for t, txt in lines:
        if not lo <= t < hi: continue
        if txt.startswith('native: composed late (1 s)'):
            lates.append(num(r'main (\d+) ops', txt) or 0)
            q = num(r'variant queue (\d+)', txt); built = num(r'built (\d+)', txt)
        elif 'packet cache replayed share' in txt:
            rep = num(r'static meshes ([\d.]+) %', txt)
        elif 'packet cache refusals' in txt:
            ref = num(r'variant decision (\d+)', txt)
        elif txt.startswith('rt: gpu build (5 s)'):
            inst = num(r'\((\d+) instances / frame', txt)
            m = re.search(r'writes per frame: inputs ([\d.]+), BLAS rows ([\d.]+)', txt)
            if m: wi, wr = float(m.group(1)), float(m.group(2))
            rtc = num(r'instance \+ record CPU ([\d.]+) ms / GPU-built', txt)
        elif txt.startswith('native: owned actors (5 s)'):
            oz = num(r'main z \d+ of \d+ ops \(([\d.]+) %', txt)
    f = lambda v, fmt='%6.1f': (fmt % v) if v is not None else '     -'
    print('%3d %6.1f %5.1f %5.1f %5s | %5s %5s | %6.0f | %6s %9s | %6s %6s %6s %5s | %s' % (
        w0, census.pct(iv, .5), census.pct(iv, .95), cpu, ('%.1f' % statistics.mean(tc)) if tc else '-',
        f(q, '%.0f'), f(built, '%.0f'), (sum(lates) / max(1, len(lates))) if lates else 0, f(rep), f(ref, '%.0f'), f(inst, '%.0f'), f(wi), f(wr), f(rtc, '%.2f'), f(oz)))
