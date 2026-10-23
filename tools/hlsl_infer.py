#!/usr/bin/env python3
"""Find the XFX_* define vector + material snippets that make the game's shipped HLSL reproduce a corpus pixel shader exactly
(docs/research/shader-permutations.md 5).

    python tools/hlsl_infer.py <fx dir> <corpus dir> <game-shaders dir> -o out/shader-permutations/solved.json [--sample N]
                               [--shaders ps30_a,ps30_b] [-j 16]

The facts that are readable from the shader (lights, lightmap, fog, ...) come from its constant table; the rest (specular,
ambient type, alpha blend, normal map encoding, which sampler is which role) is searched, cheapest first. A shader counts as
solved only when D3DX (d3dx9_29 = the compiler of the pool, flags 0) produces the same instruction stream (hlsl_match.py).
<fx dir> must be the shadermaterial directory with ge_default_ps.fx's `float3 vecWorldEye` changed to `half3` (the pool was
built from an older revision; see the doc): `tools/hlsl_infer.py --make-fx <src> <dst>` writes that copy.
"""
import argparse, collections, itertools, json, multiprocessing, os, random, re, shutil, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hlsl_match as hm
import shader_permutations as sp

TEX = 'tex2D( g_SamplerData[%d], XFX_PS_TEX0 )'
ROLE_EXPR = {  # canonical expressions per role, k = the sampler index
    'NORMAL': TEX, 'SPECULAR': TEX + '.rgb', 'SPECULARPOWER': TEX + '.r', 'OPACITY': TEX + '.a', 'SELFILLUMINATION': TEX + '.rgb',
    'EMISSION': TEX + '.rgb',
}
ROLES = list(ROLE_EXPR)

G = {}


def init(fxdir, corpus, S):
    G.update(fxdir=fxdir, corpus=corpus, S=S)


def base_defines(n):
    s = G['S'][n]
    c = s['ctab']
    m = sp.ps_macros(s)
    d = collections.OrderedDict()
    d['XFX_SHADERMODEL_3'] = '1'
    d['XFX_QUALITY'] = '2'
    d['XFX_FOG'] = 'true' if m['XFX_FOG'] else 'false'
    d['XFX_DIRLIGHT'] = 'true' if m['XFX_DIRLIGHT'] else 'false'
    d['XFX_PNTLIGHTCOUNT'] = str(m['XFX_PNTLIGHTCOUNT'])
    d['XFX_LIGHTMAP'] = 'true' if m['XFX_LIGHTMAP'] else 'false'
    has_mask = 'smpShadowMask' in c
    d['XFX_AMBIENTOCCLUSION'] = 'true' if has_mask else 'false'
    if not has_mask:
        d['XFX_DIRLIGHTSHADOWS'] = 'false'
        d['XFX_PNTLIGHTSHADOWS'] = 'false'
    return d


def ambient_options(n):
    c = G['S'][n]['ctab']
    if 'g_AmbientLight' in c:
        return [('true', t) for t in ('2', '1')]  # complex / simple (trimmed register count would tell, but cheap to try)
    if 'g_IndoorAmbientColor' in c:
        return [('true', '0')]
    return [('false', '0')]


def solve(n):
    S = G['S'][n]
    s = S
    nsamp = len([1 for l in s['listing'] if re.match(r'dcl_(2d|cube|volume) s(\d+)', l) and int(re.match(r'dcl_\w+ s(\d+)', l).group(1)) >= 9])
    if any('cube' in l for l in s['listing'] if l.startswith('dcl_cube')):
        return None, 'cube sampler'
    if any(l.startswith('dcl_volume') for l in s['listing']):
        return None, 'volume sampler'
    base = base_defines(n)
    path_fx = os.path.join(G['fxdir'], 'ge_default_phong.fx')
    ref = hm.instructions(open(os.path.join(G['corpus'], n + '.bin'), 'rb').read())
    masked = any(l.startswith('texkill') for l in s['listing'])
    has_face = any('face' in l.lower() for l in s['listing'] if l.startswith('dcl'))
    tried = 0
    t0 = time.time()
    k_all = list(range(1, nsamp))
    # role assignment: each extra sampler k>=1 gets a distinct role, or none (used by diffuse only via the first fetch)
    assign_space = []
    for r in range(0, nsamp):
        for roles in itertools.permutations(ROLES, r):
            assign_space.append(roles)
        if r >= 3:
            break
    # samplers beyond diffuse must each be used: assign len(k_all) roles when possible
    want = [a for a in assign_space if len(a) == len(k_all)] or assign_space[:1]
    for roles in want:
        for amb, amb_type in ambient_options(n):
            for spec, comp, ab in itertools.product(('false', 'true'), ('false', 'true') if 'NORMAL' in roles else ('false',), ('false', 'true')):
                d = dict(base)
                d['XFX_AMBIENTLIGHT'] = amb
                d['XFX_AMBIENTLIGHTTYPE'] = amb_type
                d['XFX_SPECULAR'] = spec
                d['XFX_COMPRESSEDNORMALMAP'] = comp
                d['XFX_ALPHABLEND'] = ab
                if masked:
                    d['XFX_MASKED'] = 'true'
                    d['XFX_ST_BILLBOARDS'] = 'true'
                if has_face:
                    d['XFX_DOUBLESIDED'] = 'true'
                d['XFX_PS_DIFFUSE'] = TEX % 0
                for role, k in zip(roles, k_all):
                    d['XFX_PS_' + role] = ROLE_EXPR[role] % k
                data, msg = hm.compile_hlsl(path_fx, 'PS_Default', 'ps_3_0', d, 0)
                tried += 1
                if data is not None and hm.instructions(data) == ref:
                    return d, 'match after %d compiles, %.1fs' % (tried, time.time() - t0)
    return None, 'no match in %d compiles' % tried


def work(n):
    try:
        d, msg = solve(n)
    except Exception as e:  # keep the pool alive
        d, msg = None, 'error: %s' % e
    return n, d, msg


def make_fx(src, dst):
    shutil.copytree(src, dst, dirs_exist_ok=True)
    p = os.path.join(dst, 'ge_default_ps.fx')
    t = open(p, encoding='latin1', newline='').read()
    new = re.sub(r'float3(\s+)vecWorldEye(\s*)= g_CamPos', r'half3\1vecWorldEye\2= g_CamPos', t)
    assert new != t, 'vecWorldEye line not found (already patched?)'
    open(p, 'w', encoding='latin1', newline='').write(new)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fxdir')
    ap.add_argument('corpus', nargs='?')
    ap.add_argument('shaders_dir', nargs='?')
    ap.add_argument('-o', default='out/shader-permutations/solved.json')
    ap.add_argument('--sample', type=int, default=0)
    ap.add_argument('--shaders', default='')
    ap.add_argument('--make-fx', metavar='DST')
    ap.add_argument('-j', type=int, default=8)
    a = ap.parse_args()
    if a.make_fx:
        make_fx(a.fxdir, a.make_fx)
        print('patched copy:', a.make_fx)
        return
    S = sp.load(a.shaders_dir)
    names = [n for n in sorted(S) if n.startswith('ps30')]
    if a.shaders:
        names = a.shaders.split(',')
    elif a.sample:
        random.Random(1).shuffle(names)
        names = names[:a.sample]
    ok = 0
    out = {}
    reasons = collections.Counter()
    with multiprocessing.Pool(a.j, init, (a.fxdir, a.corpus, S)) as pool:
        for i, (n, d, msg) in enumerate(pool.imap_unordered(work, names, chunksize=1)):
            out[n] = dict(defines=d, note=msg)
            if d:
                ok += 1
            else:
                reasons[msg.split(' in ')[0] if msg.startswith('no match') else msg] += 1
            if (i + 1) % 20 == 0:
                print('%d/%d solved %d' % (i + 1, len(names), ok), flush=True)
    os.makedirs(os.path.dirname(a.o), exist_ok=True)
    json.dump(out, open(a.o, 'w'), indent=1)
    print('solved %d of %d (%.1f%%); unsolved reasons: %s' % (ok, len(names), 100.0 * ok / len(names), dict(reasons)))


if __name__ == '__main__':
    main()
