#!/usr/bin/env python3
"""Aggregate the `diag: spike` windows of an rvk.log (docs/research/streaming.md).

python tools/spikesum.py <rvk.log> [--from HH:MM:SS] [--to HH:MM:SS] [--kind self|incl|module|stack] [--top N]
Each window's rows are weighted by its spike sample count (0.5 ms per sample), so the result is the share of all spike time.
"""
import argparse
import collections
import re

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--from', dest='t0', default='00:00:00')
ap.add_argument('--to', dest='t1', default='99:99:99')
ap.add_argument('--kind', default='self')
ap.add_argument('--top', type=int, default=30)
ap.add_argument('--minsamples', type=int, default=1)
ap.add_argument('--width', type=int, default=600)
a = ap.parse_args()
hdr = re.compile(r'^\[(\d\d:\d\d:\d\d)\.\d+\].*diag: spike: (\d+) samples')
row = re.compile(r'^\[(\d\d:\d\d:\d\d)\.\d+\].*diag: spike (self|incl|module|stack) +([\d.]+) % (.*)$')
cur = 0
tot = 0
acc = collections.Counter()
win = 0
for line in open(a.log, encoding='utf-8', errors='replace'):
    m = hdr.match(line)
    if m:
        t = m.group(1)
        n = int(m.group(2))
        cur = n if a.t0 <= t <= a.t1 and n >= a.minsamples else 0
        tot += cur
        win += 1 if cur else 0
        continue
    m = row.match(line)
    if m and cur and m.group(2) == a.kind:
        acc[m.group(4)] += cur * float(m.group(3)) / 100.0
print(f'{win} windows, {tot} spike samples (0.5 ms each = {tot * 0.5:.0f} ms)')
for k, v in acc.most_common(a.top):
    print(f'{v:8.0f} {100 * v / max(tot, 1):5.1f} %  {k[:a.width]}')
