#!/usr/bin/env python3
"""Loading-screen durations of an rvk.log: `engine: menu state loading ...` -> the next `menu state game` (docs/research/streaming.md).

python tools/loadtimes.py <rvk.log> [...]
"""
import re
import sys
from datetime import datetime

pat = re.compile(r'^\[(\d\d:\d\d:\d\d\.\d+)\].*engine: menu state (loading|game|menu) \(pages')
for path in sys.argv[1:]:
    start = None
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.match(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), '%H:%M:%S.%f')
        if m.group(2) == 'loading':
            start = t
        elif start is not None:
            out.append((t - start).total_seconds())
            start = None
    print(path.split('/')[-1].split('\\')[-1], ' '.join(f'{v:.2f}' for v in out), f'(sum of loads 2..: {sum(out[1:]):.2f} s)' if len(out) > 1 else '')
