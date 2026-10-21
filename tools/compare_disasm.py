"""Compares rvk_shadertool's disassembly with D3DXDisassembleShader (the reference) for every shader of a corpus.

    python tools/compare_disasm.py corpus [--tool build/RelWithDebInfo/rvk_shadertool.exe] [--show 5]

Instructions are compared in order; dcl/def lines as sets (D3DX keeps the original interleaving). Exit code 1 on mismatches.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

# the old CRT of d3dx9_39 prints three exponent digits (9.99999975e-006), the UCRT two
EXPONENT = re.compile(r"e([+-])0(\d\d)")


def run(tool, cmd, path):
    out = subprocess.run([tool, cmd, path], capture_output=True).stdout
    return out.replace(b"\0", b"").decode("utf-8", errors="replace")


def normalize_ref(text):
    lines = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        lines.append(EXPONENT.sub(lambda m: "e" + m.group(1) + m.group(2), line))
    return lines


def normalize_ours(text):
    lines = []
    for line in text.splitlines():
        if line.startswith("---- glsl"):
            break
        line = line.strip()
        if line:
            lines.append(line)
    return lines


def split(lines):
    decl = sorted(l for l in lines if l.startswith(("dcl", "def")))
    code = [l for l in lines if not l.startswith(("dcl", "def"))]
    return decl, code


def compare(tool, path):
    ref = split(normalize_ref(run(tool, "refdisasm", path)))
    ours = split(normalize_ours(run(tool, "show", path)))
    if ref == ours:
        return None
    for kind, a, b in (("decl", ref[0], ours[0]), ("code", ref[1], ours[1])):
        if a != b:
            for i in range(max(len(a), len(b))):
                x = a[i] if i < len(a) else "<none>"
                y = b[i] if i < len(b) else "<none>"
                if x != y:
                    return f"{os.path.basename(path)} {kind} line {i}: ref '{x}' ours '{y}'"
    return f"{os.path.basename(path)}: differs"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--tool", default=os.path.join("build", "RelWithDebInfo", "rvk_shadertool.exe"))
    ap.add_argument("--show", type=int, default=10)
    args = ap.parse_args()
    files = sorted(os.path.join(args.corpus, f) for f in os.listdir(args.corpus) if f.startswith(("vs", "ps")))
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        results = list(ex.map(lambda p: compare(args.tool, p), files))
    bad = [r for r in results if r]
    for r in bad[: args.show]:
        print(r)
    print(f"{len(files) - len(bad)} of {len(files)} shaders match the D3DX reference listing")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
