"""winsum.py [--a 30 --b 80] <tag>...: frame statistics of one route window (route seconds) per session: p50 / p95 / p99 / max / > 25 / 33 / 50 ms / mean wall CPU per frame,
plus the means over the tags (WS30). Files <WS26 dir>/<tag>_frames.csv + _spass.log."""
import sys, os, statistics, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census

ap = argparse.ArgumentParser()
ap.add_argument('--a', type=float, default=30)
ap.add_argument('--b', type=float, default=80)
ap.add_argument('tags', nargs='+')
o = ap.parse_args()
allv = []
print('%-8s %6s %6s %6s %7s %5s %5s %5s %7s' % ('tag', 'p50', 'p95', 'p99', 'max', '>25', '>33', '>50', 'cpu'))
for tag in o.tags:
    lines, rows, t0, t1 = census.load(tag)
    rr = [r for r in rows if t0 + o.a * 1000 <= r[1] < t0 + o.b * 1000]
    iv = [r[2] for r in rr]
    v = (census.pct(iv, .5), census.pct(iv, .95), census.pct(iv, .99), max(iv), sum(x > 25 for x in iv), sum(x > 33 for x in iv), sum(x > 50 for x in iv), statistics.mean(r[3] for r in rr))
    allv.append(v)
    print('%-8s %6.2f %6.2f %6.2f %7.1f %5d %5d %5d %7.2f' % ((tag,) + v))
if len(allv) > 1:
    m = [statistics.mean(c) for c in zip(*allv)]
    print('%-8s %6.2f %6.2f %6.2f %7.1f %5.0f %5.0f %5.1f %7.2f' % (('mean',) + tuple(m)))
