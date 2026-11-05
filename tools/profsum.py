#!/usr/bin/env python3
"""Averages the named `diag: profile module|self|incl` lines of rvk.log's 5 s windows (run.ps1 -CpuProfile) over the steady windows
and prints ms per frame (share x the frame interval) side by side for several logs.

  python tools/profsum.py [--skip 6] [--top 40] [--kind self|incl|module] [--filter REGEX] tagA=log tagB=log ...
"""
import argparse
import collections
import re
import statistics

LINE = re.compile(r"diag: profile (module|self|incl)\s+([\d.]+) % (.*)$")
PERF = re.compile(r"perf: \d+ frames, interval p50 ([\d.]+) ")


def load(path, skip):
    wins = []  # list of (interval, {kind: Counter})
    cur = None
    interval = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = PERF.search(line)
        if m:
            interval = float(m.group(1))
        m = LINE.search(line)
        if m:
            kind, pct, name = m.group(1), float(m.group(2)), m.group(3).strip()
            if kind == "module" and (cur is None or cur[2]):
                cur = [interval, collections.defaultdict(collections.Counter), False]
                wins.append(cur)
            if cur is None:
                continue
            cur[2] = kind != "module"
            cur[1][kind][name] += pct
    wins = wins[skip:]
    return wins


SYM = {}


def symbolize(names, dll):
    import subprocess
    todo = sorted({m.group(1) for n in names for m in [re.match(r"d3d9@0x([0-9a-f]+)", n)] if m} - set(SYM))
    if not todo or not dll:
        return
    nl = chr(10)
    out = subprocess.run(["llvm-symbolizer", "--obj=" + dll, "--demangle", "--functions=short", "--basenames"],
                         input=nl.join("0x%x" % (0x180000000 + int(r, 16)) for r in todo) + nl, capture_output=True, text=True).stdout
    recs = out.strip().split(nl + nl)
    for r, rec in zip(todo, recs):
        SYM[r] = rec.split(nl)[0][:90]


def short(n):
    m = re.match(r"d3d9@0x([0-9a-f]+)", n)
    if m and m.group(1) in SYM:
        return "d3d9!" + SYM[m.group(1)]
    n = re.sub(r"^(engine|sharedbase|ntdll|d3d9|vcruntime140|amdvlk64)!", r"\1!", n)
    n = re.sub(r"\?([A-Za-z0-9_]+)@([A-Za-z0-9_]+)@@.*", r"\2::\1", n)
    return n[:110]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip", type=int, default=6)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--kind", default="self")
    ap.add_argument("--filter", default="")
    ap.add_argument("--dll", default="out/cpu/base.dll")
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    data = {}
    ivs = {}
    for spec in a.logs:
        tag, path = spec.split("=", 1)
        wins = load(path, a.skip)
        iv = statistics.mean(w[0] for w in wins if w[0]) if wins else 0
        ivs[tag] = iv
        tot = collections.Counter()
        for w in wins:
            for k, v in w[1][a.kind].items():
                tot[k] += v
        for k in tot:
            tot[k] /= max(1, len(wins))
        symbolize(tot.keys(), a.dll)
        data[tag] = tot
        print(f"{tag}: {len(wins)} windows, interval {iv:.2f} ms")
    names = collections.Counter()
    for tag, c in data.items():
        for k, v in c.items():
            names[short(k)] = max(names[short(k)], v)
    rx = re.compile(a.filter) if a.filter else None
    rows = [n for n, _ in names.most_common() if not rx or rx.search(n)][: a.top]
    print("%-110s" % "name (ms per frame)" + "".join("%9s" % t for t in data))
    for n in rows:
        vals = []
        for tag, c in data.items():
            s = sum(v for k, v in c.items() if short(k) == n)
            vals.append(s * ivs[tag] / 100.0)
        print("%-110s" % n + "".join("%9.3f" % v for v in vals))


if __name__ == "__main__":
    main()
