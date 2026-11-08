#!/usr/bin/env python3
"""Does the HLSL material vertex shader (shaders/material/ge_default.vert.hlsl), built from the macros the engine asked for and the inputs the
bytecode declares, compute the same vertex outputs as the game's translated vertex shader? (S-1b, docs/research/frame-ownership.md 14.9)

    python tools/vs_equiv.py out/matsrc/*.jsonl [--corpus game-shaders] [--jobs 3] [--trials 8] [--only HASH] [--limit N] [-o out/vs_equiv] [-v]

Input: the JSON lines of `-Setting debug.material_sources=true` runs. For every vertex shader of ge_default_phong.fx / ge_default_leafs.fx (VS_Default /
VS_DefaultZPass) the translated game shader (game-shaders/vs30_<corpus>.vs.hlsl from `rvk_shadertool export corpus game-shaders`) and the port, generated
exactly like the run time's text (shader::uberVertexHlsl: the macros as #defines + RVK_VS_DECL + one RVK_LOC_/RVK_SEM_ pair per declared input, the
location = the dcl register), run on the GPU behind a wrapper that draws one fixed triangle and exports its clip position + every varying the translated
shader writes; rvk_shadereq --vs-test feeds both the same random vertices (per-semantic slots) and the same random constants and compares the interpolated
varyings (|a - b| <= tol (1 + max(|a|, |b|)), both NaN = equal).

Result per shader: EQUAL, DIFF, NO-HLSL (not in the corpus export), UNSUPPORTED, PORT-ERR / TRANSLATED-ERR / PROBE-ERR (does not compile). Writes
<out>/report.json and prints a summary. Exit 1 when anything but EQUAL / NO-HLSL / UNSUPPORTED.
"""
import argparse, concurrent.futures as cf, glob, json, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(os.environ.get('RVK_BIN', os.path.join(ROOT, 'build', 'RelWithDebInfo')))
TOOL = os.path.join(BIN, 'rvk_shadertool.exe')
EQ = os.path.join(BIN, 'rvk_shadereq.exe')
ENV = dict(os.environ, RVK_SHADER_DIR=os.path.join(ROOT, 'shaders'))

SLOT = {  # (usage, index) -> the vec4 attribute slot of rvk_shadereq --vs-test (slot 0: w = corner index, 9: instance index, 18: blend indices, 19: billboard data)
    ('position', 0): 0, ('normal', 0): 1, ('tangent', 0): 2, ('color', 0): 3, ('color', 1): 4, ('texcoord', 0): 5, ('texcoord', 1): 6,
    ('texcoord', 2): 7, ('texcoord', 3): 8, ('texcoord', 4): 9, ('texcoord', 5): 10, ('texcoord', 6): 11, ('position', 1): 12,
    ('normal', 1): 13, ('position', 2): 14, ('normal', 2): 15, ('position', 3): 16, ('normal', 3): 17, ('blendindices', 0): 18,
    ('blendweight', 0): 19, ('blendweight', 1): 20, ('blendweight', 2): 21,
}
ATTR = {  # uberAttrName (src/shader/uber.cpp)
    ('position', 0): 'aPos', ('position', 1): 'aPosDelta1', ('position', 2): 'aPosDelta2', ('position', 3): 'aPosDelta3',
    ('normal', 0): 'aNorm', ('normal', 1): 'aNormDelta1', ('normal', 2): 'aNormDelta2', ('normal', 3): 'aNormDelta3',
    ('tangent', 0): 'aTangent', ('color', 0): 'aCol0', ('color', 1): 'aCol1',
    ('texcoord', 0): 'aTex0', ('texcoord', 1): 'aTex1', ('texcoord', 2): 'aTex2', ('texcoord', 3): 'aTex3', ('texcoord', 4): 'aInstance',
    ('texcoord', 5): 'aBillboard', ('texcoord', 6): 'aLightmap', ('blendindices', 0): 'aIndices0',
    ('blendweight', 0): 'aWeights0', ('blendweight', 1): 'aWeights1', ('blendweight', 2): 'aWeights2',
}
# the port's outputs by varying location: (member, components)
PORT_OUT = {0: ('rvk_vTex0', 4), 1: ('rvk_vTex1', 4), 2: ('rvk_vViewPos', 4), 3: ('rvk_vPositionAndAlpha', 4), 4: ('rvk_vNormalAndAO', 4),
            5: ('rvk_vTangent', 3), 6: ('rvk_vBinormal', 3), 7: ('rvk_vLightmap', 2), 16: ('rvk_vColor0', 4)}
TRI = 'float4(float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0, 0.0, 1.0)'


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, env=ENV, **kw)


def macro_defines(macros, entry):
    out = ''
    for name, value in macros:
        value = {'true': '1', 'false': '0'}.get(value, value)
        value = ' '.join(value.split())
        out += '#define %s %s\n' % (name, value)
    if entry.endswith('ZPass'):
        out += '#define XFX_PASS 1\n'
    return out


def parse_inputs(text):
    ins = []
    for m in re.finditer(r'//\s*dcl_([a-z]+?)(\d*) v(\d+)', text):
        usage, idx, reg = m.group(1), int(m.group(2) or 0), int(m.group(3))
        ins.append((usage, idx, reg))
    return ins


def probe_members(shared, a_names):
    """(probe struct, copy statements) for the shared varyings: probe 0 = clip position, probe k = shared[k-1]"""
    st = '    float4 position : SV_Position;\n'
    for k in range(len(shared) + 1):
        st += '    [[vk::location(%d)]] float4 p%d : VARY%d;\n' % (k, k, k)
    return st


def wrapper(side, shared, vid_expr, vid_param):
    members = probe_members(shared, None)
    body = 'struct RvkProbeVsOut {\n' + members + '};\n'
    body += 'RvkProbeVsOut VSMain(RvkVsIn rvk_in%s) {\n    uint vid = %s;\n    RvkVsOut o = RvkOrigVS(rvk_in);\n    RvkProbeVsOut r;\n' % (vid_param, vid_expr)
    body += '    r.position = %s;\n    r.p0 = o.position;\n' % TRI
    for k, loc in enumerate(shared):
        if side == 'a':
            body += '    r.p%d = o.rvk_out%d;\n' % (k + 1, loc)
        else:
            name, n = PORT_OUT[loc]
            pad = {4: '', 3: ', 0.0', 2: ', 0.0, 0.0'}[n]
            body += '    r.p%d = float4(o.%s.xyzw);\n' % (k + 1, name) if n == 4 else '    r.p%d = float4(o.%s.%s%s);\n' % (k + 1, name, 'xyz'[:n], pad)
    body += '    return r;\n}\n'
    return body


def probe_ps(count, pixels=4):
    s = '#include "rvk_draw.hlsli"\nstruct PIn {\n    float4 pos : SV_Position;\n'
    for k in range(count + 1):
        s += '    [[vk::location(%d)]] float4 p%d : VARY%d;\n' % (k, k, k)
    s += '};\nfloat4 PSMain(PIn i) : SV_Target0 {\n    uint px = uint(i.pos.y) * %du + uint(i.pos.x);\n' % pixels
    for k in range(count + 1):
        s += '    rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.ps), (px * %du + %du) * 16u), i.p%d);\n' % (count + 1, k, k)
    s += '    return float4(0.0, 0.0, 0.0, 0.0);\n}\n'
    return s


def check(entry, tmp, corpus_dir, trials, valid_bb, keep=False, tol=None):
    h = entry['corpus']
    res = {'hash': entry['hash'], 'corpus': h, 'entry': entry['entry'], 'file': entry['file']}
    if entry['file'] not in ('ge_default_phong.fx', 'ge_default_leafs.fx') or entry['entry'] not in ('VS_Default', 'VS_DefaultZPass'):
        res['status'] = 'UNSUPPORTED'
        return res
    src = os.path.join(corpus_dir, 'vs30_%s.vs.hlsl' % h)
    if not os.path.isfile(src):
        res['status'] = 'NO-HLSL'
        return res
    text = open(src, encoding='utf-8').read()
    inputs = parse_inputs(text)
    outs = sorted(int(m.group(1)) for m in re.finditer(r'rvk_out(\d+)\s*:\s*VARY', text))
    shared = [l for l in outs if l in PORT_OUT]
    res['inputs'] = len(inputs)
    res['varyings'] = len(shared)
    notes = []
    if set(outs) - set(PORT_OUT):
        notes.append('port lacks outputs %s' % sorted(set(outs) - set(PORT_OUT)))
    amap = {}
    unmapped = []
    for usage, idx, reg in inputs:
        if (usage, idx) in SLOT:
            amap[reg] = SLOT[(usage, idx)]
        else:
            unmapped.append((usage, idx))
    if unmapped:
        notes.append('unmapped inputs %s' % unmapped)
    # translated side: the corpus text, VSMain renamed, + the wrapper
    a_text = re.sub(r'\bVSMain\b', 'RvkOrigVS', text) + '\n' + wrapper('a', shared, 'rvk_in.rvk_vertexId', '')
    a_src = os.path.join(tmp, 'a_%s.vs.hlsl' % h)
    open(a_src, 'w', encoding='utf-8').write(a_text)
    # the port: the run time's text (shader::uberVertexHlsl) + the wrapper
    b_text = '#define RVK_VS_DECL 1\n' + macro_defines(entry['macros'], entry['entry'])
    for usage, idx, reg in inputs:
        n = ATTR.get((usage, idx))
        if n:
            b_text += '#define RVK_LOC_%s %d\n#define RVK_SEM_%s ATTR%d\n' % (n, reg, n, reg)
    b_text += '#define VSMain RvkOrigVS\n#include "material/ge_default.vert.hlsl"\n#undef VSMain\n' + wrapper('b', shared, 'vid', ', uint vid_ : SV_VertexID').replace('uint vid = vid;', 'uint vid = vid_;')
    b_src = os.path.join(tmp, 'b_%s.vs.hlsl' % h)
    open(b_src, 'w', encoding='utf-8').write(b_text)
    p_src = os.path.join(tmp, 'probe_%s.ps.hlsl' % h)
    open(p_src, 'w', encoding='utf-8').write(probe_ps(len(shared)))
    spv = {}
    for tag, f, status in (('a', a_src, 'TRANSLATED-ERR'), ('b', b_src, 'PORT-ERR'), ('p', p_src, 'PROBE-ERR')):
        out = os.path.join(tmp, '%s_%s.spv' % (tag, h))
        r = run([TOOL, 'hlsl', f, '-o', out])
        if r.returncode:
            res['status'] = status
            res['log'] = (r.stdout + r.stderr).strip()[:700]
            return res
        spv[tag] = out
    attrs = ','.join('%d:%d' % (k, v) for k, v in sorted(amap.items()))
    cmd = [EQ, '--vs-test', '--vsa', spv['a'], spv['b'], '--attrs-a', attrs, '--attrs-b', attrs, '--probe', spv['p'], '--probe-count', str(len(shared) + 1),
           '--trials', str(trials)]
    if valid_bb or 'true' in [v for n, v in entry['macros'] if n == 'XFX_ST_BILLBOARDS']:
        cmd.append('--billboard-valid')
    if tol is not None:
        cmd += ['--tol', str(tol)]
    r = run(cmd)
    tail = r.stdout.strip().splitlines()
    last = tail[-1] if tail else r.stderr.strip()[:300]
    res['status'] = 'EQUAL' if r.returncode == 0 and last.startswith('EQUAL') else 'DIFF'
    res['detail'] = last[:300] + (' [' + '; '.join(notes) + ']' if notes else '')
    if res['status'] == 'DIFF':
        res['log'] = r.stdout.strip()[-800:]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('jsonl', nargs='+')
    ap.add_argument('--corpus', default=os.path.join(ROOT, 'game-shaders'))
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--trials', type=int, default=8)
    ap.add_argument('--only', default='')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--tol', type=float, default=None)
    ap.add_argument('--valid', action='store_true', help='valid SpeedTree billboard inputs for every shader')
    ap.add_argument('-o', default=os.path.join(ROOT, 'out', 'vs_equiv'))
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    entries, seen = [], set()
    for pat in a.jsonl:
        for f in glob.glob(pat):
            for line in open(f, encoding='utf-8'):
                line = line.strip()
                if not line:
                    continue
                e = json.loads(line)
                if e.get('stage') not in ('vs', 'vertex', 0, 'Vertex') and not e['entry'].startswith('VS'):
                    continue
                if (e['hash'], e['entry']) in seen:
                    continue
                seen.add((e['hash'], e['entry']))
                if a.only and a.only not in e['hash'] and a.only not in e['corpus']:
                    continue
                entries.append(e)
    if a.limit:
        entries = entries[:a.limit]
    os.makedirs(a.o, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='vs_equiv_')
    t0 = time.time()
    results = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(check, e, tmp, a.corpus, a.trials, a.valid, False, a.tol) for e in entries]
        for f in futs:
            r = f.result()
            results.append(r)
            if a.v or r['status'] not in ('EQUAL', 'NO-HLSL', 'UNSUPPORTED'):
                print('%-12s %s %s %s' % (r['status'], r['corpus'], r['entry'], r.get('detail', '') or r.get('log', '')[:300]))
    counts = {}
    for r in results:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    json.dump(results, open(os.path.join(a.o, 'report.json'), 'w'), indent=1)
    print('%d vertex shaders in %.0f s: %s (artifacts: %s)' % (len(results), time.time() - t0, ', '.join('%s %d' % kv for kv in sorted(counts.items())), tmp))
    bad = sum(v for k, v in counts.items() if k not in ('EQUAL', 'NO-HLSL', 'UNSUPPORTED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
