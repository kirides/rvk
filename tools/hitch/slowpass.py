import sys, re, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census
print('tag   slow passes  total ms  max ms   BLAS-build ms  (rt: scene: slow pass lines inside the route window)')
for tag in sys.argv[1:]:
    lines, rows, t0, t1 = census.load(tag)
    n = 0; tot = 0.0; mx = 0.0; bb = 0.0; blas = 0
    for t, s in lines:
        if t0 <= t <= t1:
            m = re.match(r'rt: scene: slow pass: ([\d.]+) ms CPU \((\d+) BLAS built this interval, .*BLAS builds ([\d.]+),', s)
            if m:
                n += 1; v = float(m.group(1)); tot += v; mx = max(mx, v); bb += float(m.group(3)); blas += int(m.group(2))
    print('%-5s %5d %9.0f %8.1f %10.0f  (%d BLAS)' % (tag, n, tot, mx, bb, blas))
