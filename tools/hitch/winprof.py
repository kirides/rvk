"""winprof.py (WS30): the engine thread's inclusive / self time per function in ONE window of a traversal session, in ms per frame, side by side.

  python winprof.py [--from 30 --to 80] [--kind incl|self|module] [--top 40] [--dll d3d9.dll] [--filter RE] [--stats] tagA tagB [tagC ...]
  (files <WS26 dir>/<tag>_rvk.log, _frames.csv, _spass.log; the profile lines come from -CpuProfile: one block per 5 s, top 120 inclusive)
ms per frame = the block's percent x the mean CPU per frame (frames.csv `cpu` column) of the block's 5 s / 100.
With two tags a diff column (second minus first) is added and the rows are sorted by it. --stats prints frame statistics of the window."""
import sys, re, os, argparse, collections, statistics, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census

SYM = r'E:\apps\clang\bin\llvm-symbolizer.exe'
LINE = re.compile(r'\[(\d\d:\d\d:\d\d\.\d+)\] \[[^\]]*\] \[\w+\] diag: profile (module|self|incl)\s+([\d.]+) % (.*)$')
cache = {}


def symbolize(names, dll):
    todo = sorted({int(m.group(1), 16) for n in names for m in [re.match(r'd3d9@0x([0-9a-f]+)', n)] if m} - set(cache))
    if not todo or not dll:
        return
    inp = '\n'.join('0x%x' % (0x180000000 + r) for r in todo) + '\n'
    out = subprocess.run([SYM, '--obj=' + dll, '--functions=short', '--demangle', '--inlining=false'], input=inp, capture_output=True, text=True).stdout
    blocks = out.strip().split('\n\n')
    for r, b in zip(todo, blocks):
        cache[r] = b.split('\n')[0].strip()[:100]


def name_of(n):
    m = re.match(r'd3d9@0x([0-9a-f]+)', n)
    if m:
        return 'd3d9!' + cache.get(int(m.group(1), 16), n)
    n = re.sub(r'\?([A-Za-z0-9_]+)@([A-Za-z0-9_]+)@@.*', r'\2::\1', n)
    return n[:100]


def blocks_of(tag, a, b):
    lines, rows, t0, t1 = census.load(tag)
    cur = None
    blocks = []
    for l in open(os.path.join(census.D, tag + '_rvk.log'), encoding='utf-8', errors='replace'):
        m = LINE.match(l)
        if not m:
            continue
        t = census.tod(m.group(1))
        if cur is None or t - cur[0] > 500:
            cur = [t, collections.defaultdict(collections.Counter)]
            blocks.append(cur)
        cur[1][m.group(2)][m.group(4).strip()] += float(m.group(3))
    sel = []
    for t, d in blocks:
        rel = (t - t0) / 1000.0
        if a + 2.5 <= rel <= b + 2.5:
            fr = [r[3] for r in rows if t - 5000 <= r[1] < t]
            if fr:
                sel.append((t, d, statistics.mean(fr)))
    return sel, rows, t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from', dest='a', type=float, default=30)
    ap.add_argument('--to', dest='b', type=float, default=80)
    ap.add_argument('--kind', default='incl')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--filter', default='')
    ap.add_argument('--dll', default=os.environ.get('RVK_DLL', ''))
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('tags', nargs='+')
    o = ap.parse_args()
    data = {}
    for tag in o.tags:
        sel, rows, t0 = blocks_of(tag, o.a, o.b)
        tot = collections.Counter()
        cpu = statistics.mean(s[2] for s in sel) if sel else 0
        for t, d, c in sel:
            for k, v in d[o.kind].items():
                tot[k] += v * c / 100.0
        for k in tot:
            tot[k] /= max(1, len(sel))
        symbolize(list(tot), o.dll)
        agg = collections.Counter()
        for k, v in tot.items():
            agg[name_of(k)] += v
        data[tag] = agg
        print('%s: %d blocks in %g-%g s, mean CPU/frame %.2f ms' % (tag, len(sel), o.a, o.b, cpu))
        if o.stats:
            rr = [r for r in rows if t0 + o.a * 1000 <= r[1] <= t0 + o.b * 1000]
            iv = [r[2] for r in rr]
            print('   frames %d p50 %.2f p95 %.2f p99 %.2f max %.1f >25 %d >33 %d >50 %d cpu mean %.2f' % (
                len(iv), census.pct(iv, .5), census.pct(iv, .95), census.pct(iv, .99), max(iv), sum(x > 25 for x in iv), sum(x > 33 for x in iv),
                sum(x > 50 for x in iv), statistics.mean(r[3] for r in rr)))
    rx = re.compile(o.filter) if o.filter else None
    names = set()
    for c in data.values():
        names |= set(c)
    def key(n):
        v = [data[t].get(n, 0.0) for t in o.tags]
        return abs(v[-1] - v[0]) if len(v) > 1 else v[0]
    rows = sorted((n for n in names if not rx or rx.search(n)), key=key, reverse=True)[:o.top]
    print('%-100s' % 'ms per frame (%s)' % o.kind + ''.join('%9s' % t[:8] for t in o.tags) + ('     diff' if len(o.tags) > 1 else ''))
    for n in rows:
        v = [data[t].get(n, 0.0) for t in o.tags]
        print('%-100s' % n + ''.join('%9.3f' % x for x in v) + ('%9.3f' % (v[-1] - v[0]) if len(v) > 1 else ''))


if __name__ == '__main__':
    main()
