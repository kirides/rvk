#!/usr/bin/env python3
"""Summary of in-run A/B perf windows (scripts/perf-ab.ps1, docs/research/performance.md 33).

    python tools/perf_ab.py [--key K] [--json out.json] [--top 14] <scene>=<rvk.log> [<scene>=<rvk.log> ...]

Reads, per perf window (tags A_<n> / B_<n>, `perf window <frames> <tag>`):
  perf: window <tag> gpu: N frames timed (of M), main mean X ms sd S, uploads mean U ms
  perf: window <tag> pass mean X ms sd S n K draws D | <key>
  and between `perf: window <tag> closing` and `perf: window <tag> end` the interval's `perf: N frames, interval p50 ... CPU per frame: engine+HAL C ms`
Prints per scene and arm the mean and the standard deviation OVER THE WINDOWS (the spread that decides whether a difference is real) of: the GPU frame
(main submission), each pass key (the largest by mean), CPU engine+HAL, present, interval p50; the B - A delta and delta / pooled sd.
A scene argument can repeat (several sessions of one scene: the restart fallback); windows are pooled per arm.
"""
import argparse
import json
import math
import re
import statistics
import sys
from collections import defaultdict

RE_GPU = re.compile(r'perf: window (\S+) gpu: (\d+) frames timed \(of (\d+)\), main mean ([\d.]+) ms sd ([\d.]+), uploads mean ([\d.]+) ms')
RE_PASS = re.compile(r'perf: window (\S+) pass mean ([\d.]+) ms sd ([\d.]+) n (\d+) draws ([\d.]+) \| (.*)$')
RE_CLOSING = re.compile(r'perf: window (\S+) closing at frame (\d+)')
RE_END = re.compile(r'perf: window (\S+) end at frame (\d+)')
RE_BEGIN = re.compile(r'perf: window (\S+) begin at frame (\d+)')
RE_CPU = re.compile(r'perf: (\d+) frames, interval p50 ([\d.]+) / p95 ([\d.]+) / max ([\d.]+) ms; CPU per frame: engine\+HAL ([\d.]+) ms .*?present ([\d.]+) ms')


def parse(log):
    wins = defaultdict(dict)  # tag -> fields
    inside = None
    for line in open(log, encoding='utf-8', errors='replace'):
        m = RE_BEGIN.search(line)
        if m:
            wins[m.group(1)]['first'] = int(m.group(2))
            continue
        m = RE_CLOSING.search(line)
        if m:
            inside = m.group(1)
            continue
        m = RE_END.search(line)
        if m:
            wins[m.group(1)]['last'] = int(m.group(2))
            inside = None
            continue
        if inside is not None:
            m = RE_CPU.search(line)
            if m:
                w = wins[inside]
                w['cpu_frames'] = int(m.group(1))
                w['interval_p50'] = float(m.group(2))
                w['cpu'] = float(m.group(5))
                w['present'] = float(m.group(6))
        m = RE_GPU.search(line)
        if m:
            w = wins[m.group(1)]
            w['timed'] = int(m.group(2))
            w['frames'] = int(m.group(3))
            w['gpu'] = float(m.group(4))
            w['gpu_sd'] = float(m.group(5))
            w.setdefault('passes', {})
            continue
        m = RE_PASS.search(line)
        if m:
            w = wins[m.group(1)]
            w.setdefault('passes', {})[m.group(6).strip()] = (float(m.group(2)), float(m.group(5)))
    return wins


def stats(v):
    if not v:
        return float('nan'), float('nan')
    return statistics.mean(v), (statistics.stdev(v) if len(v) > 1 else 0.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+', help='<scene>=<rvk.log>')
    ap.add_argument('--key', default='', help='the setting the arms toggle (title only)')
    ap.add_argument('--json', default='', help='write the numbers as JSON')
    ap.add_argument('--top', type=int, default=14, help='pass keys shown per scene (largest mean first)')
    a = ap.parse_args()
    scenes = defaultdict(lambda: {'A': [], 'B': []})
    for spec in a.logs:
        scene, _, log = spec.partition('=')
        for tag, w in parse(log).items():
            arm = tag.split('_')[0]
            if arm in ('A', 'B') and 'gpu' in w:
                w['tag'] = tag
                scenes[scene][arm].append(w)
    result = {'key': a.key, 'scenes': {}}
    print('perf A/B%s: mean +- sd over the windows (ms); delta = B - A, z = delta / pooled sd' % (' of ' + a.key if a.key else ''))
    for scene, arms in scenes.items():
        na, nb = len(arms['A']), len(arms['B'])
        timed = [w.get('timed', 0) for w in arms['A'] + arms['B']]
        print('\n== %s: %d A windows, %d B windows (%s frames timed each)' % (scene, na, nb, '/'.join(str(t) for t in timed)))
        rows = []

        def row(name, fa, fb):
            va = [x for x in (fa(w) for w in arms['A']) if x is not None]
            vb = [x for x in (fb(w) for w in arms['B']) if x is not None]
            ma, sa = stats(va)
            mb, sb = stats(vb)
            d = mb - ma
            pooled = math.sqrt((sa * sa + sb * sb) / 2) if not (math.isnan(sa) or math.isnan(sb)) else float('nan')
            z = d / pooled if pooled and pooled > 0 else float('nan')
            rows.append({'name': name, 'a_mean': ma, 'a_sd': sa, 'b_mean': mb, 'b_sd': sb, 'delta': d, 'z': z, 'a': va, 'b': vb})
            print('  %-58s A %8.4f +- %.4f   B %8.4f +- %.4f   delta %+8.4f (%+5.1f %%)  z %+5.1f' % (
                name[:58], ma, sa, mb, sb, d, 100.0 * d / ma if ma else float('nan'), z))

        g = lambda k: (lambda w: w.get(k))
        row('GPU frame (main submission)', g('gpu'), g('gpu'))
        row('CPU engine+HAL per frame', g('cpu'), g('cpu'))
        row('present per frame', g('present'), g('present'))
        row('interval p50', g('interval_p50'), g('interval_p50'))
        keys = defaultdict(float)
        for w in arms['A'] + arms['B']:
            for k, (ms, _) in w.get('passes', {}).items():
                keys[k] += ms
        for k in sorted(keys, key=lambda k: -keys[k])[:a.top]:
            draws = [w['passes'][k][1] for w in arms['A'] + arms['B'] if k in w.get('passes', {})]
            name = k if k.startswith('pass ') else 'pass ' + k
            label = '%s (%.0f draws)' % (name, statistics.mean(draws)) if draws and max(draws) > 0 else name
            row(label, lambda w, k=k: w.get('passes', {}).get(k, (0.0, 0))[0], lambda w, k=k: w.get('passes', {}).get(k, (0.0, 0))[0])
        result['scenes'][scene] = {'windows': {'A': [w['tag'] for w in arms['A']], 'B': [w['tag'] for w in arms['B']]}, 'rows': rows}
    if not scenes:
        print('no perf windows found (is the overlay up, -Perf on, and did the schedule\'s anchor fire?)')
        return 1
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
