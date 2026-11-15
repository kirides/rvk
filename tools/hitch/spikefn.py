"""spikefn.py <tag> <regex> [n]: spike samples aggregated by the first (leaf-most) frame matching <regex> (inclusive attribution to a function family),
e.g.  spikefn.py uE1 "native::StaticOps::\\w+"  or  "d3d9::Device::\\w+"; samples whose stack has no match are listed as '(none)'."""
import sys, re, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spikecat
tag = sys.argv[1]
pat = re.compile(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
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
agg = collections.Counter()
tot = 0.0
for w, s in rows:
    txt = re.sub(r'd3d9@0x([0-9a-f]+)', lambda m: spikecat.cache.get(int(m.group(1), 16), 'd3d9@' + m.group(1)), s)
    tot += w
    hit = '(none)'
    for f in txt.split(' < '):
        m = pat.search(f)
        if m:
            hit = m.group(0)
            break
    agg[hit] += w
print('total listed samples %.0f' % tot)
for k, w in agg.most_common(top):
    print('%6.0f %5.1f %%  %s' % (w, 100.0 * w / tot, k))
