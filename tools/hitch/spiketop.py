"""spiketop.py <tag,tag> <substring> [n]: the heaviest spike stacks containing <substring>, symbolized, own frames only (leaf first)."""
import sys, re, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spikecat
tags = sys.argv[1].split(',')
sub = sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 12
agg = collections.Counter()
for tag in tags:
    n = 0
    rows = []
    for l in open(os.path.join(spikecat.D, tag + '_rvk.log'), encoding='utf-8', errors='replace'):
        m = re.search(r'diag: spike: (\d+) samples', l)
        if m:
            n = int(m.group(1))
            continue
        m = re.search(r'diag: spike stack +([\d.]+) % (.*)$', l)
        if m: rows.append((float(m.group(1)) * n / 100.0, m.group(2)))
    allr = set(int(t, 16) for _, s in rows for t in re.findall(r'd3d9@0x([0-9a-f]+)', s))
    spikecat.symbolize(sorted(allr))
    for w, s in rows:
        txt = re.sub(r'd3d9@0x([0-9a-f]+)', lambda m: spikecat.cache.get(int(m.group(1), 16), 'd3d9@' + m.group(1)), s)
        if sub not in txt: continue
        frames = [f.strip() for f in txt.split(' < ')]
        own = []
        for f in frames:
            f2 = re.sub(r'\s*\(~.*?\)$', '', f)
            f2 = re.sub(r'rvk::|\(.*', '', f2) if 'rvk::' in f2 else f2
            if len(own) < 7: own.append(f2[:60])
            if 'detour' in f or 'eCFrustumDatabase' in f: break
        agg[' < '.join(own)] += w
for k, w in agg.most_common(top):
    print('%5.0f  %s' % (w, k))
