#!/usr/bin/env python3
"""Call-site view of run.ps1 -CpuProfile logs (needs the `diag: profile site` / `site2` / `crt` lines of V4's sampler).

  python tools/pcsites.py <rvk.log> --dll <d3d9.dll of that run> [--top 60] [--skip 2] [--fns] [--callers]

`site` = the first frame inside our d3d9.dll of each sampled stack (the leaf pc itself or the return address - 1) with
everything below it, keyed by the leaf's module ("self" = the leaf is ours): inclusive cost of one source line of ours
including the CRT / ntdll / driver calls it makes. `crt` = leaf in vcruntime140 / ucrtbase <- the first caller frame outside
them (function name, plus the exact site when it is in our module). `--fns`: the `self` / `incl` function tables with our
unnamed functions symbolized. `--callers`: `site2` = the first own function's caller (who reaches operator new, steady_clock).
"""
import argparse
import collections
import re
import subprocess

SITE = re.compile(r"diag: profile site\s+([\d.]+) % 0x([0-9a-f]+) (\S+)")
SITE2 = re.compile(r"diag: profile site2\s+([\d.]+) % 0x([0-9a-f]+) 0x([0-9a-f]+) (\S+)")
CRT = re.compile(r"diag: profile crt\s+([\d.]+) % (.+?) <- (.+)$")
FN = re.compile(r"diag: profile (self|incl)\s+([\d.]+) % (.+?)\s*$")
MOD = "diag: profile module"


def symbolize(sym, dll, rvas):
    if not rvas:
        return []
    addrs = [f"0x{0x180000000 + r:x}" for r in rvas]
    out = subprocess.run([sym, f"--obj={dll}", "--inlines", "--demangle", "--basenames", "--functions=short"],
                         input="\n".join(addrs) + "\n", capture_output=True, text=True).stdout
    res = []
    for rec in out.strip().split("\n\n"):
        lines = [l for l in rec.split("\n") if l]
        res.append([(lines[i], lines[i + 1]) for i in range(0, len(lines) - 1, 2)])
    return res


def average(windows):
    tot = collections.Counter()
    for w in windows:
        tot.update(w)
    for k in tot:
        tot[k] /= max(1, len(windows))
    return tot


def functions(a, wf):
    tot = average(wf)
    own = [k for k in tot if re.match(r"d3d9@0x[0-9a-f]+", k[1])]
    starts = {k: int(re.match(r"d3d9@0x([0-9a-f]+)", k[1]).group(1), 16) for k in own}
    syms = dict(zip(own, symbolize(a.symbolizer, a.dll, [starts[k] for k in own])))
    for kind in ("self", "incl"):
        print(f"\n== {kind} by function (named, our unnamed ones symbolized), top {a.top}")
        rows = []
        for k, pct in tot.items():
            if k[0] != kind:
                continue
            name = k[1]
            if k in syms and syms[k]:
                name = "d3d9!" + syms[k][0][0][:90] + f" ({k[1]})"
            rows.append((pct, name))
        for pct, name in sorted(rows, reverse=True)[: a.top]:
            print(f"{pct:5.2f} %  {name}")


def callers2(a, w2):
    tot = average(w2)
    keys = list(tot)
    s2 = symbolize(a.symbolizer, a.dll, [k[0] for k in keys])
    s1 = symbolize(a.symbolizer, a.dll, [k[1] for k in keys])
    rows = collections.Counter()
    for k, c2, c1 in zip(keys, s2, s1):
        f1 = c1[0][0][:50] if c1 else hex(k[1])
        l2 = f"{c2[0][1].split(chr(92))[-1]} {c2[0][0][:50]}" if c2 else hex(k[0])
        rows[(k[2], f1, l2)] += tot[k]
    print("\n== function called from own code (leaf module) <- its caller line")
    for (lm, f1, l2), pct in rows.most_common(a.top):
        print(f"{pct:5.2f} %  {lm:12s} {f1:50s} <- {l2}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--dll", default="build/RelWithDebInfo/d3d9.dll")
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--skip", type=int, default=2)
    ap.add_argument("--symbolizer", default="llvm-symbolizer")
    ap.add_argument("--fns", action="store_true")
    ap.add_argument("--callers", action="store_true")
    a = ap.parse_args()

    ws, wc, w2, wf = [], [], [], []
    cs = cc = c2 = cf = None
    for line in open(a.log, encoding="utf-8", errors="replace"):
        if MOD in line:
            cs = cc = c2 = cf = None
        m = SITE2.search(line)
        if m:
            if c2 is None:
                c2 = collections.Counter()
                w2.append(c2)
            c2[(int(m.group(2), 16), int(m.group(3), 16), m.group(4))] += float(m.group(1))
            continue
        m = SITE.search(line)
        if m:
            if cs is None:
                cs = collections.Counter()
                ws.append(cs)
            cs[(int(m.group(2), 16), m.group(3))] += float(m.group(1))
            continue
        m = CRT.search(line)
        if m:
            if cc is None:
                cc = collections.Counter()
                wc.append(cc)
            cc[(m.group(2), m.group(3))] += float(m.group(1))
            continue
        m = FN.search(line)
        if m:
            if cf is None:
                cf = collections.Counter()
                wf.append(cf)
            cf[(m.group(1), m.group(3))] += float(m.group(2))
    ws, wc, w2, wf = ws[a.skip:], wc[a.skip:], w2[a.skip:], wf[a.skip:]
    if not ws:
        raise SystemExit("no site lines found (sampler of V4 needed)")
    if a.fns:
        functions(a, wf)
    if a.callers:
        callers2(a, w2)
    ts = average(ws)
    keys = list(ts)
    syms = symbolize(a.symbolizer, a.dll, [k[0] for k in keys])
    by_leaf = collections.Counter()
    by_line = collections.Counter()
    by_fn = collections.Counter()
    for k, chain in zip(keys, syms):
        pct = ts[k]
        by_leaf[k[1]] += pct
        if not chain:
            continue
        inner_fn, inner_loc = chain[0]
        outer_fn = chain[-1][0]
        by_line[(k[1], inner_loc, inner_fn[:60], outer_fn[:50])] += pct
        by_fn[(k[1], outer_fn[:70])] += pct
    print(f"{len(ws)} windows; the engine thread's share by what the first own frame is calling (self = own code)")
    for leaf, pct in by_leaf.most_common(12):
        print(f"{pct:6.2f} %  {leaf}")
    print("\n== own call sites that call out (inclusive of the callee), by source line")
    n = 0
    for (leaf, loc, fn, outer), pct in by_line.most_common():
        if leaf == "self":
            continue
        print(f"{pct:5.2f} %  {leaf:13s} {loc.split(chr(92))[-1]:36s} {fn}  [{outer}]")
        n += 1
        if n >= a.top:
            break
    print("\n== by outermost own function and callee module")
    n = 0
    for (leaf, fn), pct in by_fn.most_common():
        if leaf == "self":
            continue
        print(f"{pct:5.2f} %  {leaf:13s} {fn}")
        n += 1
        if n >= a.top:
            break
    tc = average(wc)
    print("\n== CRT leaf <- first caller outside the CRT")
    sk = [k for k in tc if k[1].startswith("site 0x")]
    ssyms = dict(zip(sk, symbolize(a.symbolizer, a.dll, [int(k[1][5:], 16) for k in sk])))
    shown = 0
    for k, pct in tc.most_common():
        if k[1].startswith("site 0x"):
            chain = ssyms.get(k, [])
            txt = " < ".join(f"{loc.split(chr(92))[-1]} {fn[:40]}" for fn, loc in chain[:4]) or k[1]
            print(f"{pct:5.2f} %  {k[0][-30:]:30s} <- SITE {txt}")
        else:
            print(f"{pct:5.2f} %  {k[0][-30:]:30s} <- {k[1][:110]}")
        shown += 1
        if shown >= a.top * 2:
            break


if __name__ == "__main__":
    main()
