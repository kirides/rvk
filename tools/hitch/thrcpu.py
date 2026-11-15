"""thrcpu.py <tag>...: engine thread CPU time (frames.csv 7th column, WS30 builds) vs wall CPU per frame and frame interval in route windows 0-30 / 30-80 / 80-100."""
import sys, os, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census
for tag in sys.argv[1:]:
    lines, rows, t0, t1 = census.load(tag)
    thr = {}
    for l in open(os.path.join(census.D, tag + '_frames.csv')).read().splitlines()[1:]:
        f = l.split(',')
        if len(f) >= 7: thr[int(f[1])] = float(f[6])
    out = [tag]
    for a, b in ((0, 30), (30, 80), (80, 100)):
        rr = [r for r in rows if t0 + a * 1000 <= r[1] < t0 + b * 1000 and r[1] in thr]
        if not rr: continue
        out.append('%d-%d: n %d interval %.2f wallcpu %.2f threadcpu %.2f (%.0f %%)' % (a, b, len(rr), statistics.mean(r[2] for r in rr), statistics.mean(r[3] for r in rr),
                   statistics.mean(thr[r[1]] for r in rr), 100 * statistics.mean(thr[r[1]] for r in rr) / max(1e-9, statistics.mean(r[2] for r in rr))))
    print(' | '.join(out))
