#!/usr/bin/env python3
"""First minutes of a session (docs/research/frame-ownership.md 14.52): loading screens, frame intervals per 5 s window after the world appeared (clean = no other
agent's load, from the <tag>_cpu.csv next to the log that scripts/first-minutes.ps1 writes), the engine thread's CPU per frame, variant queue, owned share.

  python tools/firstminutes.py <rvk.log> [--rows]
"""
import re, sys, os
from datetime import datetime

def ts(s): return datetime.strptime(s, '%H:%M:%S.%f')
def hms(s): return datetime.strptime(s, '%H:%M:%S')
path = sys.argv[1]
rows = '--rows' in sys.argv
L = [l.rstrip('\n') for l in open(path, encoding='utf-8', errors='replace')]
t0 = None
loads = []; lstart = None
fi = []; q = []; own = []; world = None; cpuf = {}
rp = re.compile(r'^\[(\d\d:\d\d:\d\d\.\d+)\]')
lastr = 0
for l in L:
    m = rp.match(l)
    if not m: continue
    t = ts(m.group(1))
    if t0 is None: t0 = t
    r = (t - t0).total_seconds(); lastr = r
    mm = re.search(r'engine: menu state (loading|game|menu) \(pages', l)
    if mm:
        if mm.group(1) == 'loading': lstart = r
        elif lstart is not None:
            loads.append((lstart, r - lstart)); lstart = None
        if mm.group(1) == 'game' and world is None and loads: world = r
    mm = re.search(r'perf: frame intervals \(ms\): n (\d+) p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+)', l)
    if mm: fi.append((r, int(mm.group(1)), *map(float, mm.groups()[1:])))
    mm = re.search(r'perf: \d+ frames, interval .*engine\+HAL ([\d.]+) ms', l)
    if mm: cpuf[round(r)] = float(mm.group(1))
    mm = re.search(r'composed late \(1 s\) t\+(\d+) s.*variant queue (\d+) \(built (\d+)', l)
    if mm: q.append((r, int(mm.group(2)), int(mm.group(3))))
    mm = re.search(r'native: owned static \(5 s\):.*owned (\d+) \(([\d.]+) %\)', l)
    if mm: own.append((r, float(mm.group(2))))
# other agents' load per time window
noise = []  # (rel s, others %, compilers)
cp = re.sub(r'_rvk\.log$', '_cpu.csv', path)
if os.path.exists(cp):
    for line in open(cp).read().splitlines()[1:]:
        c = line.split(',')
        try:
            dt = (hms(c[0]) - hms(L[0][1:9])).total_seconds()
            if len(c) >= 6: noise.append((dt, float(c[3]), int(c[5])))
            else: noise.append((dt, 100.0 if int(c[3]) > 0 else 0.0, int(c[3])))
        except Exception: pass
def others(r):
    s = [n for n in noise if r - 5.5 <= n[0] <= r + 0.5]
    return (sum(x[1] for x in s) / len(s), max(x[2] for x in s)) if s else (None, 0)
def clean(r):
    o, cc = others(r)
    return o is not None and o < 12 and cc == 0
print(f'{path.split(chr(92))[-1]}: {len(L)} lines, span {lastr:.0f} s')
print('loads (start s, dur s):', ' '.join(f'{a:.0f}/{d:.1f}' for a, d in loads), '| world (first game after a load) at', f'{world:.0f} s' if world else '-')
w = world or 0
if q:
    z = [r for r, n, b in q if n == 0]
    print(f'variant queue: first {q[0][1]} (t {q[0][0]:.0f} s) ... built {q[-1][2]} by t {q[-1][0]:.0f} s; drained (first 0) at', f'{z[0]:.0f} s (world+{z[0] - w:.0f})' if z else 'not within the lines')
post = [x for x in fi if x[0] >= w and x[1] >= 20]
if noise:
    cl = [x for x in post if clean(x[0])]
    print(f'windows after world: {len(post)}, clean (other agents < 12 % CPU, no compiler): {len(cl)}')
else:
    cl = post
# how long the session was degraded after the world appeared: owned share first >= 90 %, windows whose median interval was above 20 ms (5 s each)
t90 = [r for r, v in own if r >= w and v >= 90.0]
slow = [x for x in post if x[2] > 20.0]
print(f'owned share >= 90 % at world+{t90[0] - w:.0f} s' if t90 else 'owned share >= 90 % not reached', f'| degraded windows (p50 > 20 ms): {len(slow)} x 5 s, last at world+{slow[-1][0] - w:.0f} s' if slow else '| no window with p50 > 20 ms',
      f'| worst frame {max((x[5] for x in post), default=0):.0f} ms')
def stat(s, label):
    if not s: return
    a = sorted(x[2] for x in s); b = sorted(x[3] for x in s)
    cpu = [cpuf.get(round(x[0]), None) for x in s]; cpu = [c for c in cpu if c is not None]
    print(f'  {label}: windows {len(s)}, median p50 {a[len(a)//2]:.2f}, median p95 {b[len(b)//2]:.2f}, worst p95 {b[-1]:.1f}, max {max(x[5] for x in s):.1f}' +
          (f', engine+HAL CPU median {sorted(cpu)[len(cpu)//2]:.1f} ms' if cpu else ''))
for name, group in (('ALL windows', post), ('CLEAN windows', cl)):
    if group is post and noise is None: continue
    print(name)
    for lo, hi in ((0, 60), (60, 120), (120, 1e9)):
        stat([x for x in group if lo <= x[0] - w < hi], f'world+{lo}..{hi if hi < 1e8 else "end"}')
if rows:
    print('t  | frame n p50 p95 p99 max | cpu | queue | owned% | others% cc')
    for r, n, a, b, c, d in fi:
        qq = min(q, key=lambda x: abs(x[0] - r))[1] if q else -1
        oo = min(own, key=lambda x: abs(x[0] - r))[1] if own else -1
        o, cc = others(r)
        print(f'{r:6.0f} {n:5d} {a:6.2f} {b:6.2f} {c:7.2f} {d:8.1f} | {cpuf.get(round(r), 0):5.1f} | {qq:4d} | {oo:5.1f} | {-1 if o is None else o:4.0f} {cc}')
