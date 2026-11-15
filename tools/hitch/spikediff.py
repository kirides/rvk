"""spikediff.py <fullTags> <wrapperTags> [kind=incl|self] [n]: spike samples per function, Full minus Wrapper (per session, ms of spike time per 180 s)"""
import sys, re, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spikecat

def collect(tags, kind):
    agg = collections.Counter()
    for tag in tags:
        n = 0
        rows = []
        for l in open(os.path.join(spikecat.D, tag + '_rvk.log'), encoding='utf-8', errors='replace'):
            m = re.search(r'diag: spike: (\d+) samples', l)
            if m:
                n = int(m.group(1))
                continue
            m = re.search(r'diag: spike ' + kind + r' +([\d.]+) % (.*)$', l)
            if m: rows.append((float(m.group(1)) * n / 100.0, m.group(2).strip()))
        spikecat.symbolize(sorted(set(int(t, 16) for _, s in rows for t in re.findall(r'd3d9@0x([0-9a-f]+)', s))))
        for w, s in rows:
            s = re.sub(r'd3d9@0x([0-9a-f]+)', lambda m: spikecat.cache.get(int(m.group(1), 16), 'd3d9@' + m.group(1)), s)
            s = re.sub(r'\s*\(~.*?\)$', '', s)
            agg[s[:90]] += w * 0.5 / len(tags)
    return agg

kind = sys.argv[3] if len(sys.argv) > 3 else 'incl'
top = int(sys.argv[4]) if len(sys.argv) > 4 else 25
f = collect(sys.argv[1].split(','), kind)
w = collect(sys.argv[2].split(','), kind)
keys = set(f) | set(w)
rows = sorted(keys, key=lambda k: -(f[k] - w[k]))
print('%-90s %8s %8s %8s   (spike ms per session)' % (kind, 'Full', 'Wrapper', 'delta'))
for k in rows[:top]: print('%-90s %8.0f %8.0f %8.0f' % (k, f[k], w[k], f[k] - w[k]))
print('...')
for k in rows[-6:]: print('%-90s %8.0f %8.0f %8.0f' % (k, f[k], w[k], f[k] - w[k]))
