#!/usr/bin/env python3
"""Graph-text census of the engine's material shader requests (frame-ownership.md 1.4 / 6.2 step 2).

    python tools/material_graph_census.py <jsonl>... [--json out.json] [--top N]

Input: `debug.material_sources` JSON lines (src/engine/material_source.*): one record per compiled request
{hash, corpus, stage, profile, flags, inline, file, entry, macros: [[name, value], ...]}. The records carry NO draw
counts: every number below weights a request once (a request = one distinct shader the engine asked the pool for).

Per request the macros split into
  graph    the XFX_PS_* / XFX_VS_* expression macros (stage ps: PS_ + VS_ ones, the texcoord setup feeds the PS;
           stage vs: VS_ ones only), NORMALISED: the indices of g_ShaderData[i] / g_vShaderData[i] / g_SamplerData[i]
           become [#] (a slot assignment, not structure); numeric literals stay (`1.0f` is part of the graph);
  dynamic  the light macros the owned shader turns into uniform branches (DYNAMIC_MACROS);
  static   everything else (blend / double sided / skinning / instancing / lighting type ...): still splits pipelines.
Pipeline estimate = distinct (file, entry, graph, static macros). Three looser / stricter views of the graph text are
also counted: `alpha` (indices renumbered by first appearance: keeps which sampler / register is used twice) and
`noconst` (also all numeric literals dropped: graphs that only differ in constants).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Macros the owned shader evaluates per draw from the record (uniform branches), not per pipeline.
DYNAMIC_MACROS = (
    "XFX_DIRLIGHT", "XFX_DIRLIGHTSHADOWS", "XFX_PNTLIGHTCOUNT", "XFX_PNTLIGHTSHADOWS",
    "XFX_LIGHTMAP", "XFX_AMBIENTLIGHT", "XFX_AMBIENTLIGHTTYPE", "XFX_AMBIENTOCCLUSION",
)

_IDX = re.compile(r"\b(g_ShaderData|g_vShaderData|g_SamplerData)\s*\[\s*(\d+)\s*\]")
_NUM = re.compile(r"(?<![\w.])\d+(?:\.\d+)?[fF]?(?![\w.])")


def is_graph_macro(name: str) -> bool:
    return name.startswith("XFX_PS_") or name.startswith("XFX_VS_")


def normalise(text: str) -> str:
    """Slot / register indices -> [#]."""
    return _IDX.sub(lambda m: f"{m.group(1)}[#]", text)


def alpha_rename(items: list[tuple[str, str]]) -> str:
    """Joined graph text with indices renumbered per array by first appearance."""
    seen: dict[tuple[str, str], int] = {}

    def sub(m: re.Match) -> str:
        key = (m.group(1), m.group(2))
        n = seen.setdefault(key, sum(1 for k in seen if k[0] == m.group(1)))
        return f"{m.group(1)}[{n}]"

    return " ;; ".join(f"{k}={_IDX.sub(sub, v)}" for k, v in items)


def drop_constants(text: str) -> str:
    return _NUM.sub("C", text)


def split_request(rec: dict) -> dict:
    macros = [(k, v) for k, v in rec["macros"]]
    stage = rec.get("stage", "ps")
    graph_items = sorted((k, v) for k, v in macros
                         if is_graph_macro(k) and (stage != "vs" or k.startswith("XFX_VS_")))
    graph_norm = " ;; ".join(f"{k}={normalise(v)}" for k, v in graph_items)
    dynamic = {k: v for k, v in macros if k in DYNAMIC_MACROS}
    static = tuple(sorted((k, v) for k, v in macros if not is_graph_macro(k) and k not in DYNAMIC_MACROS))
    return {
        "file": rec["file"], "entry": rec["entry"], "stage": stage,
        "raw": (rec["file"], rec["entry"], tuple(sorted(macros))),
        "graph": graph_norm,
        "alpha": alpha_rename(graph_items),
        "noconst": drop_constants(graph_norm),
        "dynamic": dynamic,
        "static": static,
    }


def load(path: Path) -> list[dict]:
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(split_request(json.loads(line)))
    return out


def coverage(counts: Counter, fractions=(0.5, 0.9, 0.99)) -> dict:
    total = sum(counts.values())
    res, acc, i = {}, 0, 0
    ordered = [c for _, c in counts.most_common()]
    for frac in fractions:
        while i < len(ordered) and acc < frac * total:
            acc += ordered[i]
            i += 1
        res[f"{int(frac * 100)}%"] = i
    return res


def analyse(reqs: list[dict], top: int) -> dict:
    n = len(reqs)
    res: dict = {"requests": n}
    if not n:
        return res
    res["distinct_raw"] = len({r["raw"] for r in reqs})
    res["distinct_graph"] = len({(r["file"], r["entry"], r["graph"]) for r in reqs})
    res["distinct_graph_alpha"] = len({(r["file"], r["entry"], r["alpha"]) for r in reqs})
    res["distinct_graph_noconst"] = len({(r["file"], r["entry"], r["noconst"]) for r in reqs})
    pipes = {(r["file"], r["entry"], r["graph"], r["static"]) for r in reqs}
    res["pipelines"] = len(pipes)
    res["distinct_static_combos"] = len({(r["file"], r["entry"], r["static"]) for r in reqs})

    by_entry: dict = defaultdict(list)
    for r in reqs:
        by_entry[f"{r['file']}:{r['entry']}"].append(r)
    res["entries"] = {}
    for key, rs in sorted(by_entry.items(), key=lambda kv: -len(kv[1])):
        res["entries"][key] = {
            "requests": len(rs),
            "raw": len({r["raw"] for r in rs}),
            "graphs": len({r["graph"] for r in rs}),
            "graphs_alpha": len({r["alpha"] for r in rs}),
            "graphs_noconst": len({r["noconst"] for r in rs}),
            "static_combos": len({r["static"] for r in rs}),
            "pipelines": len({(r["graph"], r["static"]) for r in rs}),
        }

    gc = Counter((r["file"], r["entry"], r["graph"]) for r in reqs)
    res["coverage_graphs"] = coverage(gc)
    res["top_graphs"] = [{"requests": c, "file": f, "entry": e, "graph": g}
                         for (f, e, g), c in gc.most_common(top)]
    pc = Counter((r["file"], r["entry"], r["graph"], r["static"]) for r in reqs)
    res["coverage_pipelines"] = coverage(pc)

    # graphs ignoring the entry (the same text under PS_Default and PS_DefaultZPass is one graph)
    res["distinct_graph_any_entry"] = len({r["graph"] for r in reqs})

    dyn_vals: dict = defaultdict(Counter)
    for r in reqs:
        for k in DYNAMIC_MACROS:
            dyn_vals[k][r["dynamic"].get(k, "<absent>")] += 1
    res["dynamic_macros"] = {k: dict(v) for k, v in dyn_vals.items() if set(v) != {"<absent>"}}
    res["dynamic_combos"] = len({tuple(sorted(r["dynamic"].items())) for r in reqs})

    st_vals: dict = defaultdict(Counter)
    for r in reqs:
        d = dict(r["static"])
        for k in {k for rr in reqs for k, _ in rr["static"]}:
            st_vals[k][d.get(k, "<absent>")] += 1
    res["static_macros"] = {k: dict(v) for k, v in sorted(st_vals.items())}
    sc = Counter(r["static"] for r in reqs)
    res["static_combos_top"] = [{"requests": c, "macros": " ".join(f"{k.removeprefix('XFX_')}={v}" for k, v in s)}
                                for s, c in sc.most_common(top)]
    return res


def fmt(label: str, a: dict, top: int) -> str:
    if not a["requests"]:
        return f"== {label}: no requests\n"
    L = [f"== {label}",
         f"requests {a['requests']} (no draw counts in the records: each request weighs 1), distinct raw macro sets {a['distinct_raw']}",
         f"distinct normalised graphs (file, entry, graph): {a['distinct_graph']}  "
         f"[any entry: {a['distinct_graph_any_entry']}; alpha-renamed {a['distinct_graph_alpha']}; "
         f"no numeric constants {a['distinct_graph_noconst']}]",
         f"graphs covering 50 / 90 / 99 % of the requests: "
         f"{a['coverage_graphs']['50%']} / {a['coverage_graphs']['90%']} / {a['coverage_graphs']['99%']}",
         f"distinct static-macro combinations: {a['distinct_static_combos']}; dynamic-macro combinations (would be uniforms): {a['dynamic_combos']}",
         f"PIPELINE ESTIMATE (graph x static macros): {a['pipelines']}  "
         f"(50 / 90 / 99 % of requests: {a['coverage_pipelines']['50%']} / {a['coverage_pipelines']['90%']} / {a['coverage_pipelines']['99%']})",
         "per entry:        requests  raw graphs alpha noconst static pipelines"]
    for k, e in a["entries"].items():
        L.append(f"  {k:<40} {e['requests']:>5} {e['raw']:>4} {e['graphs']:>5} {e['graphs_alpha']:>5} "
                 f"{e['graphs_noconst']:>7} {e['static_combos']:>6} {e['pipelines']:>9}")
    L.append("dynamic macros (value: requests):")
    for k, v in a["dynamic_macros"].items():
        L.append(f"  {k}: " + ", ".join(f"{x}={c}" for x, c in sorted(v.items())))
    L.append("static macros still splitting pipelines (value: requests):")
    for k, v in a["static_macros"].items():
        if len([x for x in v if x != "<absent>"]) or True:
            L.append(f"  {k}: " + ", ".join(f"{x}={c}" for x, c in sorted(v.items())))
    L.append(f"top {top} graphs:")
    for g in a["top_graphs"]:
        txt = g["graph"] if len(g["graph"]) < 220 else g["graph"][:217] + "..."
        L.append(f"  {g['requests']:>4}  {g['file']}:{g['entry']}  {txt}")
    L.append(f"top {top} static combinations:")
    for s in a["static_combos_top"]:
        L.append(f"  {s['requests']:>4}  {s['macros']}")
    return "\n".join(L) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("jsonl", nargs="+", type=Path)
    ap.add_argument("--json", type=Path, help="write all numbers to this JSON file")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args(argv)

    per_file = {p.name: load(p) for p in args.jsonl}
    results = {name: analyse(rs, args.top) for name, rs in per_file.items()}
    out = ""
    for name, a in results.items():
        out += fmt(name, a, args.top) + "\n"
    if len(per_file) > 1:
        union = [r for rs in per_file.values() for r in rs]
        results["union"] = analyse(union, args.top)
        out += fmt("union of " + ", ".join(per_file), results["union"], args.top)
    sys.stdout.write(out)
    if args.json:
        args.json.write_text(json.dumps(results, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
