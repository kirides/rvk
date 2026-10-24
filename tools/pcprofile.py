#!/usr/bin/env python3
"""Aggregates rvk.log's `diag: profile pc` lines (run.ps1 -CpuProfile) over all 5 s windows and symbolizes them with
llvm-symbolizer: share of the engine thread per function and per source line of our d3d9.dll.

  python tools/pcprofile.py <rvk.log> [--dll build/RelWithDebInfo/d3d9.dll] [--top 40] [--skip 2]
"""
import argparse
import collections
import re
import subprocess

PC = re.compile(r"diag: profile pc\s+([\d.]+) % 0x([0-9a-f]+)")
BLOCK = re.compile(r"diag: profile module")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--dll", default="build/RelWithDebInfo/d3d9.dll")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--skip", type=int, default=2, help="leading windows to ignore (loading)")
    ap.add_argument("--symbolizer", default="llvm-symbolizer")
    a = ap.parse_args()

    windows = []
    cur = None
    for line in open(a.log, encoding="utf-8", errors="replace"):
        if BLOCK.search(line) and "% d3d9" not in line and cur is None:
            pass
        m = PC.search(line)
        if m:
            if cur is None:
                cur = collections.Counter()
                windows.append(cur)
            cur[int(m.group(2), 16)] += float(m.group(1))
        elif "diag: profile module" in line:
            cur = None
    windows = windows[a.skip:]
    if not windows:
        raise SystemExit("no pc windows found")
    total = collections.Counter()
    for w in windows:
        total.update(w)
    for k in total:
        total[k] /= len(windows)
    print(f"{len(windows)} windows, our module = {sum(total.values()):.1f} % of the engine thread")

    addrs = [f"0x{0x180000000 + rva:x}" for rva in total]
    out = subprocess.run([a.symbolizer, f"--obj={a.dll}", "--inlines", "--demangle", "--basenames", "--functions=short"],
                         input="\n".join(addrs) + "\n", capture_output=True, text=True).stdout
    # one record per address: function / file:line pairs (inline chain, innermost first), blank line
    records = out.strip().split("\n\n")
    by_fn = collections.Counter()
    by_line = collections.Counter()
    for (rva, pct), rec in zip(total.items(), records):
        lines = [l for l in rec.split("\n") if l]
        if len(lines) < 2:
            continue
        by_line[(lines[0][:70], lines[1])] += pct
        # the outermost function = the last pair
        by_fn[lines[-2][:90]] += pct
    print("\n== by outermost function")
    for fn, pct in by_fn.most_common(a.top):
        print(f"{pct:5.2f} %  {fn}")
    print("\n== by innermost line")
    for (fn, loc), pct in by_line.most_common(a.top):
        print(f"{pct:5.2f} %  {loc:40s} {fn}")


if __name__ == "__main__":
    main()
