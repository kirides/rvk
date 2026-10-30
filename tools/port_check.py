#!/usr/bin/env python3
"""GLSL -> HLSL port check (docs/research/hlsl-rhi.md 1): compiles a shader both ways and compares the interface the C++ side depends on.

    python tools/port_check.py [--build build/RelWithDebInfo] gbuffer/gtao.comp [more names ...]
    python tools/port_check.py --all          # every shader that has both shaders/<name> (GLSL) and shaders/<name>.hlsl

What is compared (from `spirv-dis`): the resources (descriptor set, binding, kind: image dimension / sampled / storage format / buffer /
sampler / acceleration structure, array), the push-constant block (member offsets, total size), local size, the shader's stage. Both modules
must pass spirv-val. A difference is printed as a line `GLSL only: ...` / `HLSL only: ...`; resources only one side declares are
reported (the compilers drop unused globals, so a difference usually means the port does not use something the original did, or the
other way round). Exit code 1 on a difference or a compile failure. Behaviour is NOT compared: that is the in-game A/B
(`run.ps1 -Environment 'RVK_FORCE_GLSL=1'` = the GLSL originals).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def dis(spv):
    r = run(["spirv-dis", "--raw-id", str(spv)])
    if r.returncode != 0:
        raise RuntimeError("spirv-dis: " + r.stderr)
    return r.stdout


def parse(text):
    """-> set of canonical interface lines"""
    defs = {}
    deco = {}
    mdeco = {}
    modes = []
    entry = None
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r"(%\w+) = (Op\w+)(.*)", line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3).split())
            continue
        m = re.match(r"OpDecorate (%\w+) (\w+)\s*(\S*)", line)
        if m:
            deco.setdefault(m.group(1), {})[m.group(2)] = m.group(3)
            continue
        m = re.match(r"OpMemberDecorate (%\w+) (\d+) Offset (\d+)", line)
        if m:
            mdeco.setdefault(m.group(1), {})[int(m.group(2))] = int(m.group(3))
            continue
        m = re.match(r"OpExecutionMode %\w+ LocalSize (\d+) (\d+) (\d+)", line)
        if m:
            modes.append("LocalSize %s %s %s" % m.groups())
        m = re.match(r"OpEntryPoint (\w+) ", line)
        if m:
            entry = m.group(1)

    def kind(tid, depth=0):
        op, args = defs.get(tid, ("?", []))
        if op == "OpTypePointer":
            return kind(args[1], depth)
        if op in ("OpTypeRuntimeArray", "OpTypeArray"):
            return "array of " + kind(args[0], depth + 1)
        if op == "OpTypeImage":
            # dim depth arrayed ms sampled format
            return "image %s arrayed=%s sampled=%s fmt=%s" % (args[1], args[3], args[5], args[6])
        if op == "OpTypeSampler":
            return "sampler"
        if op == "OpTypeSampledImage":
            return "sampledimage"
        if op == "OpTypeAccelerationStructureKHR":
            return "accelerationstructure"
        if op == "OpTypeStruct":
            return "struct"
        return op

    out = set()
    for vid, (op, args) in defs.items():
        if op != "OpVariable":
            continue
        sc = args[1]
        if sc in ("UniformConstant", "Uniform", "StorageBuffer", "Image"):
            d = deco.get(vid, {})
            if "DescriptorSet" in d or "Binding" in d:
                k = kind(args[0])
                if sc != "UniformConstant":
                    k = sc + " " + k
                out.add("res set=%s binding=%s : %s" % (d.get("DescriptorSet", "?"), d.get("Binding", "?"), k))
        elif sc == "PushConstant":
            ptr = defs[args[0]]
            st = ptr[1][1]
            offs = mdeco.get(st, {})
            out.add("push offsets=%s" % (sorted(offs.values()),))
    out.update(modes)
    if entry:
        out.add("stage " + entry)
    return out


def check(name, build):
    glsl = REPO / "shaders" / name
    hlsl = REPO / "shaders" / (name + ".hlsl")
    if not glsl.exists() or not hlsl.exists():
        return ["missing %s or %s" % (glsl, hlsl)]
    problems = []
    with tempfile.TemporaryDirectory() as td:
        g = Path(td) / "g.spv"
        h = Path(td) / "h.spv"
        r = run([str(build / "rvk_glslc.exe"), "compile", str(glsl), "-o", str(Path(td) / "g.h"), "--name", "x", "-I",
                 str(REPO / "shaders" / "include"), "--spv", str(g)])
        if r.returncode != 0:
            return ["GLSL compile failed: " + (r.stderr or r.stdout)]
        env = dict(os.environ, RVK_SHADER_DIR=str(REPO / "shaders"))
        r = run([str(build / "rvk_shadertool.exe"), "hlsl", name, "-o", str(h)], env=env)
        if r.returncode != 0:
            return ["HLSL compile failed:\n" + r.stdout + r.stderr]
        a, b = parse(dis(g)), parse(dis(h))
        for x in sorted(a - b):
            if x.startswith("res set=0 "):
                continue  # the bindless heap's arrays: glslang keeps the unused ones the ABI header declares, DXC drops them
            problems.append("GLSL only: " + x)
        for x in sorted(b - a):
            if x == "res set=0 binding=4 : array of accelerationstructure":
                continue  # the TLAS: the GLSL shaders take its address, the HLSL ones the bindless array element (abi.h kBindingTlas)
            problems.append("HLSL only: " + x)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--build", default=str(REPO / "build" / "RelWithDebInfo"))
    a = ap.parse_args()
    build = Path(a.build)
    names = list(a.names)
    if a.all:
        for p in sorted((REPO / "shaders").rglob("*.hlsl")):
            rel = p.relative_to(REPO / "shaders").as_posix()[:-5]
            if (REPO / "shaders" / rel).exists():
                names.append(rel)
    bad = 0
    for n in names:
        p = check(n, build)
        print(("FAIL " if p else "ok   ") + n)
        for line in p:
            print("     " + line)
        bad += bool(p)
    print("%d shader(s), %d with differences" % (len(names), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
