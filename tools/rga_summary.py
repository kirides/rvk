#!/usr/bin/env python3
"""Summarise scripts/rga-survey.ps1 CSVs: per tag (variant family) module count, VGPR mean / p90 / max, spills, scratch, and the RDNA2 wave32 occupancy
(waves per SIMD = min(16, 1024 / VGPRs rounded up to the granule of 8)); `-b before.csv -a after.csv` adds the paired change per shader hash x tag.

    python tools/rga_summary.py rga_base.csv [-b before.csv -a after.csv]
"""
import argparse, csv, statistics, sys


def occ(v):
    v = max(8, -(-v // 8) * 8)
    return min(16, 1024 // v)


def load(p):
    return [r for r in csv.DictReader(open(p, encoding='utf-8-sig')) if r['tag'] != 'FAILED']


def table(rows):
    by = {}
    for r in rows:
        by.setdefault(r['tag'], []).append(r)
    out = []
    for tag, rs in sorted(by.items()):
        v = sorted(int(r['vgprs']) for r in rs)
        out.append((tag, len(rs), statistics.mean(v), v[int(0.9 * (len(v) - 1))], v[-1], sum(int(r['vgpr_spills']) > 0 for r in rs), sum(int(r['scratch']) > 0 for r in rs),
                    statistics.mean(occ(x) for x in v), statistics.mean(int(r['isa_bytes']) for r in rs)))
    return out


def show(rows, title):
    print(title)
    print('%-34s %5s %7s %5s %5s %6s %7s %6s %9s' % ('tag', 'n', 'meanVGPR', 'p90', 'max', 'spill', 'scratch', 'waves', 'ISA bytes'))
    for t in table(rows):
        print('%-34s %5d %7.1f %5d %5d %6d %7d %6.1f %9.0f' % t)
    allv = [int(r['vgprs']) for r in rows]
    print('%-34s %5d %7.1f %5d %5d' % ('ALL', len(allv), statistics.mean(allv), sorted(allv)[int(0.9 * (len(allv) - 1))], max(allv)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv', nargs='?')
    ap.add_argument('-b')
    ap.add_argument('-a')
    a = ap.parse_args()
    if a.csv:
        show(load(a.csv), a.csv)
    if a.b and a.a:
        b, c = load(a.b), load(a.a)
        key = lambda r: (r['shader'], r['tag'])
        bm = {key(r): int(r['vgprs']) for r in b}
        pairs = [(bm[key(r)], int(r['vgprs']), r['tag']) for r in c if key(r) in bm]
        print('paired %d modules (same shader hash and tag)' % len(pairs))
        by = {}
        for x, y, t in pairs:
            by.setdefault(t, []).append((x, y))
        for t, ps in sorted(by.items()):
            print('  %-32s n=%4d  %6.1f -> %6.1f  (%+.1f %%)' % (t, len(ps), statistics.mean(p[0] for p in ps), statistics.mean(p[1] for p in ps),
                                                              100.0 * (statistics.mean(p[1] for p in ps) / statistics.mean(p[0] for p in ps) - 1)))


if __name__ == '__main__':
    sys.exit(main())
