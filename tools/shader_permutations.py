#!/usr/bin/env python3
"""Shader permutation analysis of the exported game shaders (docs/research/shader-permutations.md).

    rvk_shadertool export corpus game-shaders
    python tools/shader_permutations.py game-shaders [-o out/shader-permutations]

The game's material shaders are one HLSL ubershader (data/compiled/effects/shadermaterial/ge_default*.fx, ge_default.fxh)
compiled once per set of XFX_* defines + the material's generated XFX_PS_* graph code. Which defines were active is read back
from each shader's own constant table / declarations (the CTAB names survive in every material shader). Writes macros.csv (one
row per shader: the inferred macro vector, the material key, the family) and prints the summary tables.
"""
import argparse, collections, csv, difflib, glob, os, re, sys


def load(d):
    shaders = {}
    for f in sorted(glob.glob(os.path.join(d, '*.vert')) + glob.glob(os.path.join(d, '*.frag'))):
        name = os.path.splitext(os.path.basename(f))[0]
        ctab, listing, mode = {}, [], 0
        with open(f, encoding='utf-8') as fh:
            for l in fh.read().split('\n'):
                if l.startswith('// ---- D3D9 listing'):
                    mode = 1
                elif mode == 0:
                    m = re.match(r'//   ([a-z])(\d+)(?:\.\.[a-z](\d+))? (\S+)$', l)
                    if m:
                        n = int(m.group(3)) - int(m.group(2)) + 1 if m.group(3) else 1
                        ctab[m.group(4)] = (m.group(1), int(m.group(2)), n)
                elif mode == 1:
                    if l.startswith('// '):
                        listing.append(l[3:])
                    elif not l.strip():
                        mode = 2
        shaders[name] = dict(ctab=ctab, listing=listing, stage=name[:2])
    return shaders


def is_decl(l):
    w = l.split()[0]
    return w.startswith('dcl') or w.startswith(('ps_', 'vs_')) or w == 'def'


def body(s):
    return [l for l in s['listing'] if not is_decl(l)]


def sampler_ids(l):
    m = re.match(r'dcl_(2d|cube|volume) s(\d+)', l)
    return (m.group(1), int(m.group(2))) if m else None


def ps_macros(s):
    c = s['ctab']
    # g_PointLights: 4 registers per light; the compiler trims unused trailing registers -> ceil
    pnt = -(-c['g_PointLights'][2] // 4) if 'g_PointLights' in c else 0
    return collections.OrderedDict(
        XFX_DIRLIGHT=int('g_DirLight' in c), XFX_PNTLIGHTCOUNT=pnt, XFX_LIGHTMAP=int('smpLightMap1' in c),
        XFX_AMBIENTLIGHT=int('g_AmbientLight' in c), XFX_FOG=int('g_FogSettings' in c),
        XFX_DISTORTION=int('smpRefract' in c), SOFT_DEPTH=int('smpDepthmap' in c), DIRLIGHT_SHADOWS=int('smpShadowMask' in c),
        SHADERDATA=int('g_ShaderData' in c), SAMPLER_DATA=int('g_SamplerData' in c),
        CLIPALPHATEST=int(any(l.startswith(('texkill', 'cmp')) for l in body(s)) and any(l.startswith('texkill') for l in body(s))))


def vs_macros(s):
    c = s['ctab']
    ins = ' '.join(l for l in s['listing'] if l.startswith('dcl_'))
    return collections.OrderedDict(
        XFX_SKINNED=int('dcl_blendindices' in ins and 'g_vWindMatrix' not in c), XFX_INSTANCED=int('dcl_texcoord4' in ins and 'g_vInstanceData' in c),
        SPEEDTREE=int('g_vWindMatrix' in c), LEAVES=int('g_vLeafAngles' in c or 'g_vLeafAngleMatrix' in c),
        XFX_VEGETATION=int('g_vVegetationData1' in c), TANGENT=int('dcl_tangent' in ins), CAMPOS=int('g_vCamPosAndFar' in c),
        SHADERDATA=int('g_vShaderData' in c), INSTANCEMISC=int('g_vInstanceMisc' in c))


def material_key(s):
    """Material graph identity as far as it is visible without evaluating: the material samplers (s9..) and the order the
    shader fetches them in."""
    decl = tuple(f'{t}{i}' for t, i in filter(None, map(sampler_ids, s['listing'])) if i >= 9)
    fetch = tuple(re.search(r's(\d+)\s*$', l.strip()).group(1) for l in body(s)
                  if l.startswith('texld') and re.search(r's(\d+)\s*$', l.strip()) and int(re.search(r's(\d+)\s*$', l.strip()).group(1)) >= 9)
    return decl, fetch


def skel(l):
    w = l.split()
    return ' '.join([w[0]] + [re.sub(r'[a-zA-Z]+\d+', lambda m: re.match(r'[a-zA-Z]+', m.group(0)).group(0), x) for x in w[1:]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('-o', default='out/shader-permutations')
    a = ap.parse_args()
    S = load(a.dir)
    os.makedirs(a.o, exist_ok=True)
    ps = {n: s for n, s in S.items() if s['stage'] == 'ps'}
    vs = {n: s for n, s in S.items() if s['stage'] == 'vs'}
    mac = {n: (ps_macros(s) if n in ps else vs_macros(s)) for n, s in S.items()}

    print(f'{len(ps)} pixel + {len(vs)} vertex shaders')
    print('\n== pixel shaders: macro value histogram (read back from constant tables)')
    for k in next(iter(mac[n] for n in ps)):
        print(f'  {k:18} {dict(sorted(collections.Counter(mac[n][k] for n in ps).items()))}')
    print('\n== vertex shaders')
    for k in next(iter(mac[n] for n in vs)):
        print(f'  {k:18} {dict(sorted(collections.Counter(mac[n][k] for n in vs).items()))}')

    # lighting variant = the macros that the engine's frame state chooses (not the material)
    def light(n):
        m = mac[n]
        return (m['XFX_DIRLIGHT'], m['XFX_PNTLIGHTCOUNT'], m['XFX_LIGHTMAP'], m['XFX_DISTORTION'], m['DIRLIGHT_SHADOWS'], m['XFX_AMBIENTLIGHT'], m['SHADERDATA'])
    lv = collections.Counter(light(n) for n in ps)
    print(f'\n== pixel lighting variants (dir, pointlights, lightmap, distortion, shadowmask, ambient, shaderdata): {len(lv)}')
    for k, v in lv.most_common(14):
        print(f'  {v:5} {k}')

    # materials: shaders sharing a material key, counted per lighting variant: the max is a lower bound of the material count
    byKey = collections.defaultdict(lambda: collections.defaultdict(int))
    fam = {}
    for n, s in ps.items():
        k = (material_key(s), light(n)[2], light(n)[3])
        byKey[k][light(n)] += 1
        fam[n] = k
    mats = sum(max(v.values()) for v in byKey.values())
    print(f'\n== estimated distinct material graphs: >= {mats} (max shaders per lighting variant, summed over {len(byKey)} sampler keys)')
    print(f'   {len(ps)} pixel shaders / {mats} = {len(ps) / mats:.1f} lighting permutations per material')
    core = [n for n in ps if light(n)[2:] == (0, 0, 1, 1, 1)]
    core_by = collections.Counter((light(n)[0], light(n)[1]) for n in core)
    print('   unlit-map main pass (lightmap 0, no distortion, mask+ambient+data): shaders per (dir, pointlights):')
    print('     ' + '  '.join(f'{k}:{v}' for k, v in sorted(core_by.items())))

    # concrete evidence: one material in two lighting variants that differ in exactly one macro
    def shingles(n, k=4):
        b = [skel(l) for l in body(S[n])]
        return set(tuple(b[i:i + k]) for i in range(max(1, len(b) - k + 1)))
    best = None
    pool = [n for n in ps if light(n)[2:] == (0, 0, 1, 1, 1) and light(n)[1] == 1]
    d0 = [n for n in pool if light(n)[0] == 0]
    d1 = {material_key(S[n]): [] for n in pool if light(n)[0] == 1}
    for n in pool:
        if light(n)[0] == 1:
            d1[material_key(S[n])].append(n)
    for n in d0:
        for m in d1.get(material_key(S[n]), []):
            if S[n]['listing'][1:3] != S[m]['listing'][1:3]:
                pass
            sa, sb = shingles(n), shingles(m)
            j = len(sa & sb) / len(sa | sb)
            if best is None or j > best[0]:
                best = (j, n, m)
    if best:
        j, n, m = best
        print(f'\n== one material, XFX_DIRLIGHT 0 -> 1 (pointlights 1, same samplers): {n} vs {m}, 4-gram Jaccard {j:.2f}')
        ops = lambda x: [skel(l) for l in body(S[x])]
        for l in difflib.unified_diff(ops(n), ops(m), n, m, lineterm='', n=0):
            if not l.startswith('@@'):
                print('  ' + l)

    with open(os.path.join(a.o, 'macros.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        cols = list(next(iter(mac[n] for n in ps))) + ['mat_samplers', 'mat_fetch_order']
        w.writerow(['shader', 'stage'] + cols)
        for n in sorted(ps):
            k = material_key(S[n])
            w.writerow([n, 'ps'] + list(mac[n].values()) + [' '.join(k[0]), ''.join(k[1])])
        vcols = list(next(iter(mac[n] for n in vs)))
        for n in sorted(vs):
            w.writerow([n, 'vs'] + list(mac[n].values()))
    print(f'\nwrote {os.path.join(a.o, "macros.csv")}')


if __name__ == '__main__':
    main()
