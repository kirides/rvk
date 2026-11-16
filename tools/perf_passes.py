#!/usr/bin/env python3
"""Pass table of perf-ab sessions (docs/research/shader-isa.md): GPU ms per pass key and arm, averaged over the windows of one rvk.log.

  python tools/perf_passes.py <scene>=<rvk.log> ... [--arms wrapper,full] [--min 0.02] [--md]

The windows are the tags `A_<n>` / `B_<n>` of `scripts/perf-ab.ps1 -Key renderer.mode -A wrapper -B full` (A = first arm). Run the game with
RVK_PERF_WINDOW_ORDINAL=1 so passes of one shape (z passes, main colour passes, cascades) keep their own rows (`<key> #2`).
Prints, per scene, the passes sorted by the larger arm's time; the pass key is the engine's attachment shape (`1280x720, 1 colour (97), depth 130`), a
pass of an injected feature its name (`pass light.cluster at AfterDepthPrepass`).
"""
import argparse
import re
import sys
from collections import defaultdict

WIN = re.compile(r'perf: window (\w+?)_(\d+) (gpu|pass)')
GPU = re.compile(r'perf: window ([AB])_(\d+) gpu: (\d+) frames timed .*?main mean ([\d.]+) ms')
PASS = re.compile(r'perf: window ([AB])_(\d+) pass mean ([\d.]+) ms sd [\d.]+ n (\d+) draws ([\d.]+) \| (.*)$')


def parse(path):
    main = defaultdict(list)
    passes = defaultdict(lambda: defaultdict(list))
    draws = defaultdict(lambda: defaultdict(list))
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = GPU.search(line)
            if m:
                main[m.group(1)].append(float(m.group(4)))
                continue
            m = PASS.search(line)
            if m:
                passes[m.group(1)][m.group(6).strip()].append(float(m.group(3)))
                draws[m.group(1)][m.group(6).strip()].append(float(m.group(5)))
    return main, passes, draws


def mean(v):
    return sum(v) / len(v) if v else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenes', nargs='+')
    ap.add_argument('--arms', default='wrapper,full')
    ap.add_argument('--min', type=float, default=0.02)
    a = ap.parse_args()
    names = a.arms.split(',')
    for spec in a.scenes:
        scene, _, path = spec.partition('=')
        m, p, d = parse(path)
        print('\n### %s' % scene)
        print('GPU frame (main submission): %s = %.3f ms, %s = %.3f ms' % (names[0], mean(m['A']), names[1], mean(m['B'])))
        keys = set(p['A']) | set(p['B'])
        rows = []
        for k in keys:
            ta, tb = mean(p['A'].get(k, [])), mean(p['B'].get(k, []))
            rows.append((max(ta, tb), k, ta, tb, mean(d['A'].get(k, [])), mean(d['B'].get(k, []))))
        rows.sort(reverse=True)
        print('| pass | %s ms | %s ms | draws %s / %s |' % (names[0], names[1], names[0], names[1]))
        print('|---|---|---|---|')
        for _, k, ta, tb, da, db in rows:
            if max(ta, tb) < a.min:
                continue
            print('| %s | %.3f | %.3f | %.0f / %.0f |' % (k, ta, tb, da, db))


if __name__ == '__main__':
    sys.exit(main())
