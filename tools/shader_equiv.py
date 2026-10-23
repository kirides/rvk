#!/usr/bin/env python3
"""Does the GLSL port in shaders/material equal the game's shader for a define vector? (docs/research/shader-permutations.md 6)

    python tools/shader_equiv.py [--vs] [--fx out/hlsl-fx] [--only NAME] [--trials N] [-v]

For each test vector (defines + material snippets):
  1. the shipped HLSL (ge_default_phong.fx, `float3 vecWorldEye` -> `half3`: tools/hlsl_infer.py --make-fx) is compiled with
     d3dx9_29 (the pool's compiler, flags 0) to ps_3_0 bytecode,
  2. rvk_shadertool export translates it to GLSL exactly as the renderer does at run time,
  3. shaders/material/ge_default.frag is compiled with the same defines,
  4. rvk_shadereq runs both on the GPU with identical random constants / varyings / textures and compares the colours.

--vs does the same for the vertex shaders (ge_default.vert): both are patched (preprocessed GLSL text) to draw one fixed triangle
and to export their clip position as the varying at location 20; the same random vertices reach them through a per-semantic slot
table (the translated shader's inputs are numbered by its own dcl order, the port's by shaders/material/ge_default.vert); a probe
fragment shader dumps the interpolated varyings both write, rvk_shadereq compares them.
"""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import hlsl_match as hm

BIN = os.path.join(ROOT, 'build', 'RelWithDebInfo')
GLSLC = os.path.join(os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.363.0'), 'Bin', 'glslc.exe')
D0 = 'tex2D( g_SamplerData[0], XFX_PS_TEX0 )'
N1 = 'tex2D( g_SamplerData[1], XFX_PS_TEX0 )'
S2 = 'tex2D( g_SamplerData[2], XFX_PS_TEX0 )'

LIGHT = {'XFX_AMBIENTLIGHT': 'true', 'XFX_AMBIENTLIGHTTYPE': '2', 'XFX_FOG': 'true'}


def vec(name, entry='PS_Default', **d):
    return name, entry, d


def vectors():
    v = []
    base = dict(LIGHT, XFX_PS_DIFFUSE=D0)
    v.append(vec('unlit-diffuse', XFX_FOG='true', XFX_DISABLELIGHTING='true', XFX_AMBIENTOCCLUSION='false', XFX_PS_DIFFUSE=D0, XFX_PS_SELFILLUMINATION='1.0'))
    for dr in ('false', 'true'):
        for pnt in (0, 1, 2, 3):
            v.append(vec('diffuse dir=%s pnt=%d' % (dr, pnt), **dict(base, XFX_DIRLIGHT=dr, XFX_PNTLIGHTCOUNT=pnt)))
    n = dict(base, XFX_PS_NORMAL=N1, XFX_COMPRESSEDNORMALMAP='true')
    for dr, pnt in (('true', 0), ('true', 2), ('false', 3)):
        v.append(vec('normal(dxt5nm) dir=%s pnt=%d' % (dr, pnt), **dict(n, XFX_DIRLIGHT=dr, XFX_PNTLIGHTCOUNT=pnt)))
    v.append(vec('normal(rgb) dir pnt1', **dict(base, XFX_PS_NORMAL=N1, XFX_DIRLIGHT='true', XFX_PNTLIGHTCOUNT=1)))
    v.append(vec('specular dir pnt2', **dict(n, XFX_DIRLIGHT='true', XFX_PNTLIGHTCOUNT=2, XFX_SPECULAR='true', XFX_PS_SPECULAR=S2 + '.rgb',
                                              XFX_PS_SPECULARPOWER='g_ShaderData[0].x')))
    v.append(vec('specular simple-brdf', **dict(n, XFX_DIRLIGHT='true', XFX_PNTLIGHTCOUNT=1, XFX_SPECULAR='true', XFX_LIGHTINGTYPE='0',
                                                 XFX_PS_SPECULARPOWER='g_ShaderData[0].x')))
    v.append(vec('lightmap normal', **dict(n, XFX_LIGHTMAP='true', XFX_DIRLIGHT='true', XFX_PNTLIGHTCOUNT=1, XFX_SPECULAR='true')))
    v.append(vec('lightmap only', **dict(base, XFX_LIGHTMAP='true')))
    v.append(vec('ambient simple', **dict(base, XFX_AMBIENTLIGHTTYPE='1', XFX_DIRLIGHT='true')))
    v.append(vec('ambient none+indoor', **dict(base, XFX_AMBIENTLIGHTTYPE='0', XFX_DIRLIGHT='true')))
    v.append(vec('no AO, no shadows', **dict(base, XFX_AMBIENTOCCLUSION='false', XFX_DIRLIGHTSHADOWS='false', XFX_PNTLIGHTSHADOWS='false',
                                              XFX_DIRLIGHT='true', XFX_PNTLIGHTCOUNT=2)))
    v.append(vec('no fog, alpha blend', **dict(base, XFX_FOG='false', XFX_ALPHABLEND='true', XFX_DIRLIGHT='true')))
    v.append(vec('opacity + selfillum + emission', **dict(base, XFX_PS_OPACITY=S2 + '.a', XFX_PS_SELFILLUMINATION=S2 + '.rgb * g_ShaderData[1].xyz',
                                                          XFX_PS_EMISSION=S2 + '.rgb', XFX_DIRLIGHT='true')))
    v.append(vec('distortion', **dict(n, XFX_PS_DISTORTION='g_ShaderData[1].xy', XFX_DIRLIGHT='true', XFX_PNTLIGHTCOUNT=1)))
    v.append(vec('double sided', **dict(n, XFX_DOUBLESIDED='true', XFX_DIRLIGHT='true')))
    v.append(vec('material graph: lerp/mask', **dict(base, XFX_PS_DIFFUSE='lerp( tex2D( g_SamplerData[0], XFX_PS_TEX0 ), tex2D( g_SamplerData[1], XFX_PS_TEX1 ) * g_ShaderData[0], '
                                                                          'tex2D( g_SamplerData[2], XFX_PS_TEX0 ).a )', XFX_DIRLIGHT='true')))
    # z / shadow map pass
    z = dict(XFX_PS_DIFFUSE=D0)
    v.append(vec('zpass', 'PS_DefaultZPass', **z))
    v.append(vec('zpass masked', 'PS_DefaultZPass', **dict(z, XFX_MASKED='true')))
    v.append(vec('zpass shadowmap', 'PS_DefaultZPass', **dict(z, XFX_SHADOWMAPPASS='true')))
    v.append(vec('zpass shadowmap projected', 'PS_DefaultZPass', **dict(z, XFX_SHADOWMAPPASS='true', XFX_SHADOWMAPPASS_PROJECTED='true')))
    v.append(vec('zpass shadowmap point', 'PS_DefaultZPass', **dict(z, XFX_SHADOWMAPPASS='true', XFX_SHADOWMAPPASS_POINT='true')))
    v.append(vec('zpass variance', 'PS_DefaultZPass', **dict(z, XFX_SHADOWMAPPASS='true', XFX_VARIANCESHADOWMAP='true', XFX_MASKED='true')))
    return v


def glsl_defines(d, entry):
    out = []
    for k, val in d.items():
        val = {'true': '1', 'false': '0'}.get(val, val) if isinstance(val, str) else str(val)
        out.append('-D%s=%s' % (k, val))
    if entry.endswith('ZPass'):
        out.append('-DXFX_PASS=1')
    return out


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


# ---- vertex shaders ---------------------------------------------------------------------------------------------------------------

SLOT = {  # (usage, index) -> the vec4 attribute slot of rvk_shadereq --vs-test
    ('position', 0): 0, ('normal', 0): 1, ('tangent', 0): 2, ('color', 0): 3, ('color', 1): 4, ('texcoord', 0): 5, ('texcoord', 1): 6,
    ('texcoord', 2): 7, ('texcoord', 3): 8, ('texcoord', 4): 9, ('texcoord', 5): 10, ('texcoord', 6): 11, ('position', 1): 12,
    ('normal', 1): 13, ('position', 2): 14, ('normal', 2): 15, ('position', 3): 16, ('normal', 3): 17, ('blendindices', 0): 18,
    ('blendweight', 0): 19, ('blendweight', 1): 20, ('blendweight', 2): 21,
}
PORT_ATTR = {
    'aPos': ('position', 0), 'aNorm': ('normal', 0), 'aTangent': ('tangent', 0), 'aCol0': ('color', 0), 'aCol1': ('color', 1),
    'aTex0': ('texcoord', 0), 'aTex1': ('texcoord', 1), 'aTex2': ('texcoord', 2), 'aTex3': ('texcoord', 3), 'aInstance': ('texcoord', 4),
    'aBillboard': ('texcoord', 5), 'aLightmap': ('texcoord', 6), 'aPosDelta1': ('position', 1), 'aNormDelta1': ('normal', 1),
    'aPosDelta2': ('position', 2), 'aNormDelta2': ('normal', 2), 'aPosDelta3': ('position', 3), 'aNormDelta3': ('normal', 3),
    'aIndices0': ('blendindices', 0), 'aWeights0': ('blendweight', 0), 'aWeights1': ('blendweight', 1), 'aWeights2': ('blendweight', 2),
}
TRI = 'vec4(float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0, float(gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0)'


def vs_vectors():
    v = []
    v.append(vec('default', 'VS_Default'))
    v.append(vec('default, 4 texcoord sets', 'VS_Default', XFX_VS_TEX1='XFX_VS_INPUT_TEX1', XFX_VS_TEX2='XFX_VS_INPUT_TEX2 * 2.0',
                 XFX_VS_TEX3='XFX_VS_INPUT_TEX3 + g_vShaderData[0].xy', GLSL_TEXSETS=4))
    v.append(vec('instanced', 'VS_Default', XFX_INSTANCED='true'))
    v.append(vec('skinned', 'VS_Default', XFX_SKINNED='true'))
    v.append(vec('skinned + morphed', 'VS_Default', XFX_SKINNED='true', XFX_MORPHED='true'))
    v.append(vec('billboard', 'VS_Default', XFX_BILLBOARD='true'))
    v.append(vec('vegetation', 'VS_Default', XFX_VEGETATION='true'))
    v.append(vec('speedtree branch', 'VS_Default', XFX_ST_BRANCH='true', XFX_INSTANCED='true'))
    v.append(vec('speedtree leafmesh', 'VS_Default', XFX_ST_LEAFMESH='true', XFX_INSTANCED='true'))
    v.append(vec('speedtree leafcards', 'VS_Default', XFX_ST_LEAFCARDS='true', XFX_INSTANCED='true'))
    v.append(vec('speedtree billboards', 'VS_Default', XFX_ST_BILLBOARDS='true'))
    v.append(vec('speedtree billboards vert1', 'VS_Default', XFX_ST_BILLBOARDS='true', XFX_ST_BILLBOARDS_VERT1='true'))
    v.append(vec('zpass', 'VS_DefaultZPass'))
    v.append(vec('zpass masked instanced', 'VS_DefaultZPass', XFX_MASKED='true', XFX_INSTANCED='true'))
    v.append(vec('zpass shadowmap skinned', 'VS_DefaultZPass', XFX_SHADOWMAPPASS='true', XFX_SKINNED='true'))
    v.append(vec('zpass shadowmap leafcards', 'VS_DefaultZPass', XFX_SHADOWMAPPASS='true', XFX_ST_LEAFCARDS='true', XFX_INSTANCED='true'))
    v.append(vec('zpass vegetation', 'VS_DefaultZPass', XFX_VEGETATION='true'))
    return v


def preprocess(src, includes, defs=()):
    return run([GLSLC, '-E', '--target-env=vulkan1.3', '-fshader-stage=vert'] + [x for i in includes for x in ('-I', i)] + list(defs) + [src])


def patch_vs(text):
    assert text.count('gl_Position = rvk_pos;') == 1, 'gl_Position line'
    text = text.replace('gl_Position = rvk_pos;', 'rvk_testpos = rvk_pos; gl_Position = %s;' % TRI)
    k = text.rindex('void main()')
    return text[:k] + 'layout(location = 20) out vec4 rvk_testpos;\n' + text[k:]


def interface(text, kind):
    """{location: (components, name)} of the in / out variables of preprocessed GLSL"""
    out = {}
    for m in re.finditer(r'layout\(\s*location\s*=\s*([0-9 +()]+)\)\s*%s\s+(vec[234]|float)\s+(\w+)' % kind, text):
        n = {'float': 1, 'vec2': 2, 'vec3': 3, 'vec4': 4}[m.group(2)]
        out[eval(m.group(1))] = (n, m.group(3))
    return out


def translated_attrs(listing_text):
    amap = {}
    for m in re.finditer(r'//\s*dcl_([a-z]+?)(\d*) v(\d+)', listing_text):
        usage, idx, reg = m.group(1), int(m.group(2) or 0), int(m.group(3))
        if (usage, idx) in SLOT:
            amap[reg] = SLOT[(usage, idx)]
        else:
            print('  note: unmapped vertex input', usage, idx)
    return amap


def probe_glsl(locs, pixels=4):
    comp = {1: ('float', ', 0.0, 0.0, 0.0'), 2: ('vec2', ', 0.0, 0.0'), 3: ('vec3', ', 0.0'), 4: ('vec4', '')}
    lines = ['#version 460', '#extension GL_GOOGLE_include_directive : require', '#extension GL_EXT_buffer_reference_uvec2 : require',
             '#include "rvk_abi.glsl"', 'layout(buffer_reference, std430) buffer Probe { vec4 v[]; };']
    for loc, n in locs:
        lines.append('layout(location = %d) in %s i%d;' % (loc, comp[n][0], loc))
    lines += ['layout(location = 0) out vec4 dummy;', 'void main() {', '    Probe p = Probe(uvec2(pc.ps));',
              '    uint px = uint(gl_FragCoord.y) * %du + uint(gl_FragCoord.x);' % pixels]
    for k, (loc, n) in enumerate(locs):
        lines.append('    p.v[px * %du + %du] = vec4(i%d%s);' % (len(locs), k, loc, comp[n][1]))
    lines += ['    dummy = vec4(0.0);', '}']
    return '\n'.join(lines) + '\n'


def run_vs(a):
    tmp = tempfile.mkdtemp(prefix='shader_equiv_vs_')
    inc = os.path.join(ROOT, 'shaders', 'include')
    mat = os.path.join(ROOT, 'shaders', 'material')
    fx = os.path.join(a.fx, 'ge_default_phong.fx')
    results = []
    for i, (name, entry, d) in enumerate(vs_vectors()):
        if a.only and a.only not in name:
            continue
        d = dict(d)
        texsets = d.pop('GLSL_TEXSETS', 1)
        defs = dict(d)
        defs.setdefault('XFX_SHADERMODEL_3', '1')
        defs.setdefault('XFX_QUALITY', '2')
        data, msg = hm.compile_hlsl(fx, entry, 'vs_3_0', defs, 0)
        if data is None:
            results.append((name, 'HLSL compile error: ' + msg.strip()[:200]))
            continue
        d_in = os.path.join(tmp, 'in%d' % i)
        d_out = os.path.join(tmp, 'out%d' % i)
        os.makedirs(d_in)
        open(os.path.join(d_in, 'vs30_%016x.bin' % i), 'wb').write(data)
        r = run([os.path.join(BIN, 'rvk_shadertool.exe'), 'export', d_in, d_out])
        if r.returncode:
            results.append((name, 'translate failed: ' + r.stdout.strip()[-200:]))
            continue
        a_file = os.path.join(d_out, 'vs30_%016x.vert' % i)
        listing = open(a_file, encoding='utf-8').read()
        pa = preprocess(a_file, [os.path.join(d_out, 'include')])
        gd = glsl_defines(d, entry) + (['-DXFX_VS_INPUT_TEXSETS=%d' % texsets] if texsets > 1 else [])
        pb = preprocess(os.path.join(mat, 'ge_default.vert'), [inc, mat], gd)
        if pa.returncode or pb.returncode:
            results.append((name, 'preprocess failed: ' + (pa.stderr + pb.stderr).strip()[:300]))
            continue
        ta, tb = patch_vs(pa.stdout), patch_vs(pb.stdout)
        ib = interface(tb, 'in')
        oa, ob = interface(ta, 'out'), interface(tb, 'out')
        amap_a = translated_attrs(listing)
        amap_b = {loc: SLOT[PORT_ATTR[nm]] for loc, (n, nm) in ib.items()}
        lacking_in = sorted(set(amap_a.values()) - set(amap_b.values()))
        shared = sorted(l for l in oa if l in ob)
        note = ''
        if set(oa) - set(ob):
            note += ' [port lacks outputs %s]' % sorted(set(oa) - set(ob))
        if lacking_in:
            note += ' [port lacks input slots %s]' % lacking_in
        files = {}
        for tag, text in (('a', ta), ('b', tb)):
            src = os.path.join(tmp, '%s%d.vert' % (tag, i))
            open(src, 'w').write(text)
            spv = os.path.join(tmp, '%s%d.spv' % (tag, i))
            r = run([GLSLC, '--target-env=vulkan1.3', '-fshader-stage=vert', src, '-o', spv])
            if r.returncode:
                results.append((name, '%s does not compile after patching: %s' % ('translated' if tag == 'a' else 'port', r.stderr.strip()[:300])))
                break
            files[tag] = spv
        else:
            locs = [(l, min(oa[l][0], ob[l][0])) for l in shared]
            probe_src = os.path.join(tmp, 'probe%d.frag' % i)
            open(probe_src, 'w').write(probe_glsl(locs))
            probe_spv = os.path.join(tmp, 'probe%d.spv' % i)
            r = run([GLSLC, '--target-env=vulkan1.3', '-I', inc, probe_src, '-o', probe_spv])
            if r.returncode:
                results.append((name, 'probe does not compile: ' + r.stderr.strip()[:300]))
                continue
            attrs = lambda m: ','.join('%d:%d' % (k, v) for k, v in sorted(m.items()))
            r = run([os.path.join(BIN, 'rvk_shadereq.exe'), '--vs-test', '--vsa', files['a'], files['b'], '--attrs-a', attrs(amap_a), '--attrs-b', attrs(amap_b),
                     '--probe', probe_spv, '--probe-count', str(len(locs)), '--trials', str(a.trials)] + (['--billboard-valid'] if a.valid or 'XFX_ST_BILLBOARDS' in d else []))
            tail = r.stdout.strip().splitlines()
            results.append((name, (tail[-1] if tail else r.stderr.strip()[:200]) + ' (%d varyings)%s' % (len(locs), note)))
            if r.returncode and a.v:
                print(r.stdout)
    width = max(len(n) for n, _ in results)
    bad = 0
    for n, m in results:
        bad += 0 if m.startswith('EQUAL') else 1
        print('%-*s  %s' % (width, n, m))
    print('\n%d of %d vertex vectors equal (artifacts: %s)' % (len(results) - bad, len(results), tmp))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fx', default=os.path.join(ROOT, 'out', 'hlsl-fx'))
    ap.add_argument('--only', default='')
    ap.add_argument('--vs', action='store_true', help='vertex shaders instead of pixel shaders')
    ap.add_argument('--valid', action='store_true', help='with --vs: valid SpeedTree billboard inputs (random ones reach D3DX int() rounding noise, see the doc)')
    ap.add_argument('--trials', type=int, default=8)
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.fx, 'ge_default_phong.fx')):
        print('missing %s: python tools/hlsl_infer.py <data/compiled/effects/shadermaterial> --make-fx out/hlsl-fx' % a.fx)
        return 2
    if a.vs:
        return run_vs(a)
    tmp = tempfile.mkdtemp(prefix='shader_equiv_')
    inc = os.path.join(ROOT, 'shaders', 'include')
    mat = os.path.join(ROOT, 'shaders', 'material')
    vs_spv = os.path.join(tmp, 'eq_varyings.spv')
    r = run([GLSLC, '--target-env=vulkan1.3', '-I', inc, os.path.join(ROOT, 'shaders', 'test', 'eq_varyings.vert'), '-o', vs_spv])
    if r.returncode:
        print(r.stderr)
        return 2
    fx = os.path.join(a.fx, 'ge_default_phong.fx')
    results = []
    for i, (name, entry, d) in enumerate(vectors()):
        if a.only and a.only not in name:
            continue
        defs = dict(d)
        defs.setdefault('XFX_SHADERMODEL_3', '1')
        defs.setdefault('XFX_QUALITY', '2')
        data, msg = hm.compile_hlsl(fx, entry, 'ps_3_0', defs, 0)
        if data is None:
            results.append((name, 'HLSL compile error: ' + msg.strip()[:200]))
            continue
        d_in = os.path.join(tmp, 'in%d' % i)
        d_out = os.path.join(tmp, 'out%d' % i)
        os.makedirs(d_in)
        open(os.path.join(d_in, 'ps30_%016x.bin' % i), 'wb').write(data)
        r = run([os.path.join(BIN, 'rvk_shadertool.exe'), 'export', d_in, d_out])
        if r.returncode:
            results.append((name, 'translate failed: ' + r.stdout.strip()[-200:]))
            continue
        a_spv = os.path.join(tmp, 'a%d.spv' % i)
        b_spv = os.path.join(tmp, 'b%d.spv' % i)
        r = run([GLSLC, '--target-env=vulkan1.3', '-I', os.path.join(d_out, 'include'), os.path.join(d_out, 'ps30_%016x.frag' % i), '-o', a_spv])
        if r.returncode:
            results.append((name, 'translated GLSL does not compile: ' + r.stderr.strip()[:200]))
            continue
        r = run([GLSLC, '--target-env=vulkan1.3', '-I', inc, '-I', mat, '-fshader-stage=frag', os.path.join(mat, 'ge_default.frag'), '-o', b_spv] + glsl_defines(d, entry))
        if r.returncode:
            results.append((name, 'port does not compile: ' + r.stderr.strip()[:300]))
            continue
        r = run([os.path.join(BIN, 'rvk_shadereq.exe'), '--vs', vs_spv, '--fs', a_spv, b_spv, '--trials', str(a.trials)])
        tail = r.stdout.strip().splitlines()
        results.append((name, (tail[-1] if tail else r.stderr.strip()[:200]) if r.returncode or a.v else tail[-1]))
        if r.returncode and a.v:
            print(r.stdout)
    width = max(len(n) for n, _ in results)
    bad = 0
    for n, m in results:
        ok = m.startswith('EQUAL')
        bad += 0 if ok else 1
        print('%-*s  %s' % (width, n, m))
    print('\n%d of %d vectors equal (artifacts: %s)' % (len(results) - bad, len(results), tmp))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
