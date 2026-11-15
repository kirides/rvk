import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hitches

def bins(tags, width=10):
    out = collections.Counter()
    for tag in tags:
        lines, rows, t0, t1, diag, d3dx, big = hitches.parse(tag)
        for r in rows:
            if t0 <= r[1] <= t1 and r[2] > 25 and r[0] in diag:
                h = diag[r[0]]
                if 'engine/unattributed' in hitches.classify(h, d3dx):
                    out[int((r[1] - t0) / 1000 / width)] += 1
    return out

width = 10
f = bins(sys.argv[1].split(','), width)
w = bins(sys.argv[2].split(','), width)
print('t(s)   Full  Wrapper  (unattributed hitch frames > 25 ms, summed over the sessions)')
for b in range(0, 180 // width):
    print('%3d-%3d %4d %4d' % (b * width, (b + 1) * width, f[b], w[b]))
