"""WS26 hitch census: python census.py <tag> [<tag> ...]  (files <dir>/<tag>_rvk.log + <tag>_frames.csv)
Frames inside the route (rvk_traverse begins .. ends, by time of day) -> distribution, hitch counts, per-hitch causes."""
import sys, re, statistics, os, bisect
D = os.environ.get('WS26', r'B:\DevDrive\rvk-work\scratch\ws26')

def tod(s):
    h, m, r = s.split(':'); sec, ms = r.split('.')
    return ((int(h) * 60 + int(m)) * 60 + int(sec)) * 1000 + int(ms)

def load(tag):
    log = open(os.path.join(D, tag + '_rvk.log'), encoding='utf-8', errors='replace').read().splitlines()
    t0 = t1 = None
    lines = []
    sp = os.path.join(D, tag + '_spass.log')
    for l in open(sp, encoding='utf-8', errors='replace').read().splitlines():
        m = re.match(r'\[(\d\d:\d\d:\d\d\.\d+)\] \[\w+\] \[t\d+\] (.*)', l)
        if not m: continue
        if 'rvk_traverse: begins' in m.group(2) and t0 is None: t0 = tod(m.group(1))
        if 'rvk_traverse: ends' in m.group(2): t1 = tod(m.group(1))
    for l in log:
        m = re.match(r'\[(\d\d:\d\d:\d\d\.\d+)\] \[[^\]]*\] \[(\w+)\] (.*)', l)
        if not m: continue
        t = tod(m.group(1)); lines.append((t, m.group(3)))
    rows = []
    p = os.path.join(D, tag + '_frames.csv')
    for l in open(p).read().splitlines()[1:]:
        f = l.split(',')
        if len(f) < 6: continue
        try: rows.append((int(f[0]), int(f[1]), float(f[2]), float(f[3]), float(f[4]), float(f[5])))
        except ValueError: pass
    return lines, rows, t0, t1

def pct(v, q):
    s = sorted(v); return s[min(len(s) - 1, int(q * (len(s) - 1) + .5))]

def analyse(tag, verbose=True):
    lines, rows, t0, t1 = load(tag)
    if t0 is None: print(tag, 'no route markers'); return None
    if t1 is None: t1 = rows[-1][1]
    rr = [r for r in rows if t0 <= r[1] <= t1]
    iv = [r[2] for r in rr]
    out = dict(tag=tag, n=len(iv), p50=pct(iv, .5), p95=pct(iv, .95), p99=pct(iv, .99), mx=max(iv))
    for th in (25, 33, 50, 100): out['gt%d' % th] = sum(1 for x in iv if x > th)
    # rolling median (61 frames)
    rel = []
    for i, x in enumerate(iv):
        w = iv[max(0, i - 30):i + 31]
        med = statistics.median(w)
        if x > 2 * med and x > med + 4: rel.append(i)
    out['gt2med'] = len(rel)
    out['dur'] = (t1 - t0) / 1000
    hitches = [i for i, x in enumerate(iv) if x > 25]
    out['hitches'] = [(rr[i][1], rr[i][0], iv[i], rr[i][3], rr[i][4], rr[i][5]) for i in hitches]
    # events by time
    ev = {'d3dx': 'compiled from source on the engine thread', 'hitchdiag': 'diag: hitch', 'pipelines': 'pipelines', 'lateq': 'composed late'}
    out['lines'] = lines
    return out

def cause_of(h, lines):
    t, fr, iv, cpu, pres, slot = h
    near = [(lt, s) for lt, s in lines if t - iv - 20 <= lt <= t + 20]
    tags = []
    for lt, s in near:
        if 'compiled from source on the engine thread' in s: tags.append('D3DX')
        if s.startswith('diag: hitch'):
            m = re.search(r'pipelines built (\d+) .*shaders built (\d+).*read-backs (\d+)', s)
            if m and int(m.group(1)): tags.append('pipelines')
            if m and int(m.group(2)): tags.append('shaders')
        if 'stream' in s or 'loading' in s.lower() or 'menu state' in s: tags.append('load')
    kind = 'present' if pres > cpu else ('slotwait' if slot > 0.5 * cpu else 'cpu')
    return kind, sorted(set(tags))

if __name__ == '__main__':
    print('tag n dur p50 p95 p99 max >25 >33 >50 >100 >2xmed')
    for tag in sys.argv[1:]:
        o = analyse(tag)
        if not o: continue
        print(o['tag'], o['n'], '%.0f' % o['dur'], '%.2f %.2f %.2f %.1f' % (o['p50'], o['p95'], o['p99'], o['mx']), o['gt25'], o['gt33'], o['gt50'], o['gt100'], o['gt2med'])
