#!/usr/bin/env python3
"""S-7d (docs/research/frame-ownership.md 14.18): generates the effect ports in shaders/post/fx from the game's fx_2_0 effects.

The engine's effect shaders of the post chain, the shadow mask / SSAO chain / PSSM pass (post_SSM.fx), the terrain map's blur (Blur7) and the rare
image effects (RadialBlur, Wobble, EdgeSmooth) are rewritten as HLSL files of our own: every vertex and pixel shader of these effects, de-duplicated
by bytecode. Each port is the translator's instruction-for-instruction HLSL (src/shader/hlsl.cpp: the D3D9 arithmetic rules, the ABI's register windows,
the half-pixel / reversed-depth position fix) restructured into one entry function with local registers and the shared helpers of
shaders/post/fx/post_port.hlsli; the D3D9 listing itself is not copied (game-derived).
The table src/native/post_port_table.inl maps FNV-1a 64 of the engine shader's tokens to its port: native::OwnPost draws a composed effect draw
with the ports while `native.own_post` is on (Full) and checks them in compare mode; tools/post_port.py --check proves every port EQUAL to the
translated shader on the GPU (rvk_shadereq).

  python tools/post_port.py --corpus <corpus dir> --work <scratch dir>            # regenerate shaders/post/fx + the table
  python tools/post_port.py --corpus <corpus dir> --work <scratch dir> --check    # + rvk_shadereq of every port against the translated shader
"""
import argparse, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'RelWithDebInfo')

# family label -> one fx binary of the corpus (every captured variant of a family has the same shader bytecode: checked by --verify-variants)
FAMILIES = [
    ('ssm', 'fx_2f37cffe26bf235a'),     # post_SSM.fx: DSSM VSSM SSM SSM_Low PSSM PSSM_Low SSAO SSAO_Low BlurSSAO UpscaleSSAO Blur
    ('blur7', 'fx_17ae618a55e2a8f1'),   # the terrain map's Blur7H / Blur7V / Blur7
    ('hdr', 'fx_02af14896acdb188'),     # post_hdr.fx
    ('dof', 'fx_2681404bbe324279'),     # post_dof.fx
    ('bloom', 'fx_56902b99c7d4f3ab'),   # post_bloom.fx
    ('streak', 'fx_2cff9717d1a09269'),  # post_streaks.fx
    ('ov', 'fx_0bf1d6f6a9603f7a'),      # overlay_color.fx
    ('radial', 'fx_1d6219a429fea570'),  # post_radialBlur.fx
    ('wobble', 'fx_2f47482808345820'),  # post_wobble.fx
    ('edge', 'fx_358932593eb9a842'),    # post_edgesmooth.fx
    ('gui', 'fx_d6ce5091aa988c66'),     # F13 (14.24): the sprite effect of eCSprite (inline HLSL, technique Main: GUI, text, cursor, loading screens, videos)
]
# F12 (frame-ownership.md 14.24): single shaders of the shader pool (not fx effects): corpus stem -> (family, name, use)
SINGLES = [
    ('vs20_98629e7bbba95f12', 'sky', 'dome', 'ge_skydome_2_0.fx VS (eCShaderVersionSkyDome_2_0 +0x1b0)'),
    ('ps20_38851f343d71da1e', 'sky', 'dome', 'ge_skydome_2_0.fx PS (eCShaderVersionSkyDome_2_0 +0x1d0)'),
    # 14.28 addendum (own particles): ge_particle_2_0.fx (eCShaderVersionParticle): the vertex shaders are ported in their GPU-expansion form (EXPAND_FAMILIES: the
    # prologue of Device::particleVs, shaders/include/rvk_particle.hlsli, is part of the port); the lit ones appear only in a rvk_rare / rvk_spells corpus
    ('vs20_04929cb82716b77c', 'particle', 'vs_unlit', 'ge_particle_2_0.fx VS, unlit, one texture (eCShaderVersionParticle +0x1b0..)'),
    ('vs20_60faa4ebe3d86b65', 'particle', 'vs_unlit_blend', 'ge_particle_2_0.fx VS, unlit, two textures blended by the vertex colour alpha'),
    ('vs20_6e471f76fa15c52a', 'particle', 'vs_dirlight', 'ge_particle_2_0.fx VS, one directional light + ambient (LightingStyle lit)'),
    ('ps20_eb41d0e19e965023', 'particle', 'ps_plain', 'ge_particle_2_0.fx PS, one texture, no fog (eCShaderVersionParticle +0x2f0)'),
    ('ps20_9a24395316e6e10a', 'particle', 'ps_fog', 'ge_particle_2_0.fx PS, one texture, fog (+0x300)'),
    ('ps20_3add35a94a540348', 'particle', 'ps_blend', 'ge_particle_2_0.fx PS, two textures blended by the vertex colour alpha, no fog'),
]
# families whose vertex shaders carry the particle expansion (Device::particleVs's prologue for kKindParticle)
USAGE_FIELD = {('position', ''): 'q.pos', ('normal', ''): 'q.nrm', ('color', ''): 'q.c0', ('color', '1'): 'q.c1', ('texcoord', ''): 'q.t0', ('texcoord', '1'): 'q.t1'}


def expansion_assign(text):
    """the `v<n> = q.<field>;` list of Device::particleVs for a particle vertex shader (from the listing's dcl lines)"""
    out = ''
    for m in re.finditer(r'^// dcl_(position|normal|color|texcoord)(\d*) v(\d+)\s*$', text, re.M):
        f = USAGE_FIELD.get((m.group(1), m.group(2) if m.group(2) not in ('', '0') else ''))
        if f is None:
            raise ValueError('particle vertex shader: input %s%s has no expansion field' % (m.group(1), m.group(2)))
        out += ' v%s = %s;' % (m.group(3), f)
    return out


def expansion_prologue(assign):
    return ('    { RvkParticleVertex q = rvkParticleExpand(rvk_in.rvk_vertexId, rvk_in.rvk_pr0, rvk_in.rvk_pr1, rvk_in.rvk_pr2, rvk_in.rvk_pr3, rvk_in.rvk_pr4, '
            'rvk_in.rvk_pr5, rvk_in.rvk_pr6, rvk_in.rvk_pr7.x);%s }' % assign)


def expansion_inputs(struct):
    """RvkVsIn + the record's TEXCOORD8..15 stream (extraInputs of Device::particleVs)"""
    lines = struct.splitlines()
    extra = ['    [[vk::location(%d)]] float4 rvk_pr%d : ATTR%d;' % (8 + k, k, 8 + k) for k in range(8)]
    at = max(i for i, l in enumerate(lines) if 'ATTR' in l) + 1
    return '\n'.join(lines[:at] + extra + lines[at:])


def inject_expansion(text, assign):
    """the translated vertex shader with the expansion hooks the translator would add (the `a` side of --check)"""
    text = text.replace('#include "rvk_draw.hlsli"', '#include "rvk_draw.hlsli"\n#include "rvk_particle.hlsli"', 1)
    m = re.search(r'^struct RvkVsIn \{.*?^\};', text, re.S | re.M)
    text = text[:m.start()] + expansion_inputs(m.group(0)) + text[m.end():]
    i = text.index('RvkVsOut VSMain(')
    j = text.index('    rvk_main();\n', i)
    return text[:j] + expansion_prologue(assign) + '\n' + text[j:]

# pixel shaders with a hand-written S-7c port (shaders/post/post_*.frag.hlsl, native::OwnPost::unitDraw): not generated
# (--check proves those too: the hand port against the translated shader)
S7C = {('hdr', 'ToneMap'): 'post/post_tonemap.frag', ('hdr', 'HDRI'): 'post/post_hdri.frag', ('dof', 'DoFDown'): 'post/post_dofdown.frag',
       ('dof', 'DoF'): 'post/post_dof.frag', ('ov', 'TOverlayColor'): 'post/post_ov_color.frag', ('ov', 'OverlayRadialColor'): 'post/post_ov_radialcolor.frag',
       ('ov', 'OverlayRadialBlur'): 'post/post_ov_radialblur.frag', ('ov', 'OverlayImage'): 'post/post_ov_image.frag',
       ('ov', 'OverlayMovieStrips'): 'post/post_ov_strips.frag', ('ov', 'OverlayMovieStripsAbs'): 'post/post_ov_strips.frag'}


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def techniques(dump):
    """[(technique, pass, vs index, ps index)] from `rvk_fxtool dump`"""
    out, tech, pas, vs = [], None, None, None
    for line in dump.splitlines():
        m = re.match(r'^  technique (\S+)', line)
        if m:
            tech = m.group(1)
            continue
        m = re.match(r'^    pass (\S+)', line)
        if m:
            pas, vs = m.group(1), None
            continue
        m = re.search(r'Vertexshader = shader #(\d+)', line)
        if m:
            vs = int(m.group(1))
        m = re.search(r'Pixelshader = shader #(\d+)', line)
        if m:
            out.append((tech, pas, vs, int(m.group(1))))
        if line.startswith('shaders:'):
            break
    return out


def ctab_names(text):
    names = []
    for line in text.splitlines():
        m = re.match(r'^//   ((?:c|s|i|b)\d+(?:\.\.\S+)?) (\S+)$', line)
        if m:
            names.append('%s %s' % (m.group(1), m.group(2)))
    return names


HELPERS = {'rvk_nrm(': 'rvkPortNrm(', 'rvk_pow(': 'rvkPortPow(', 'rvk_lit(': 'rvkPortLit(',
           'rvk_kill(': 'rvkPortKill(rvk_draw, rvk_fragCoord, ', 'rvk_alphaTest(': 'rvkPortAlphaTest(rvk_draw, rvk_fragCoord, '}


def dense_varyings(struct):
    """F12 / F13: the varyings of a stage interface dense from location 0 to the highest one (zero-filled `rvk_fill<n>` members, in location order). D3D12 links
    the stages by register in declaration order: the sky dome's pixel shader reads VARY0, 1, 3..7, 16 of the VS's 0..7, 16, which fails PSO creation (the
    translator's text has the same fillers on D3D12, `varyingFillers`); the dense pair links on both backends."""
    if 'RvkVsOut' not in struct and 'RvkPsIn' not in struct:
        return struct
    lines = struct.splitlines()
    vary = {}
    keep = []
    for l in lines[1:-1]:
        m = re.match(r'^\s+\[\[vk::location\((\d+)\)\]\] .*: VARY(\d+);$', l)
        if m:
            vary[int(m.group(1))] = l
        else:
            keep.append(l)
    if not vary:
        return struct
    for n in range(max(vary) + 1):
        if n not in vary:
            vary[n] = '    [[vk::location(%d)]] float4 rvk_fill%d : VARY%d;' % (n, n, n)
    return '\n'.join([lines[0]] + keep + [vary[n] for n in sorted(vary)] + [lines[-1]])


def convert(text, vertex, title, techs, ref='S-7d (docs/research/frame-ownership.md 14.18)', static_pos=False, expand=None):
    """the translator's HLSL -> the port's text"""
    lines = text.splitlines()
    body = [l for l in lines if not l.startswith('//')]
    src = '\n'.join(body)
    structs = re.findall(r'^struct \w+ \{.*?^\};', src, re.S | re.M)
    if len(structs) != 2:
        raise ValueError('%s: expected two structs, found %d' % (title, len(structs)))
    if static_pos:
        structs = [dense_varyings(s) for s in structs]
    statics =re.findall(r'^static (?!const)([\w]+) (\w+)( = [^;]*)?;$', src, re.M)
    consts = re.findall(r'^static const .*;$', src, re.M)
    m = re.search(r'^void rvk_main\(\) \{\n(.*?)^\}', src, re.S | re.M)
    if not m:
        raise ValueError(title + ': no rvk_main')
    main = m.group(1)
    if re.search(r'\bl\d+\(\)', main) or re.search(r'^void l\d+\(', src, re.M):
        raise ValueError(title + ': subroutines are not supported')
    entry = 'VSMain' if vertex else 'PSMain'
    m = re.search(r'^(Rvk\w+ %s\(Rvk\w+ rvk_in\)) \{\n(.*?)^\}' % entry, src, re.S | re.M)
    if not m:
        raise ValueError(title + ': no entry')
    sig, ent = m.group(1), m.group(2)
    rel = 'rvkPortVsRel(' if vertex else 'rvkPortPsRel('
    for a, b in list(HELPERS.items()) + [('rvk_cRel(', rel)]:
        main = main.replace(a, b)
        ent = ent.replace(a, b)
    used = main + ent
    decls = []
    for typ, name, init in statics:
        if name == 'rvk_draw':
            continue
        if not re.search(r'\b%s\b' % re.escape(name), used):
            continue
        decls.append('    %s %s%s;' % (typ, name, init if init else (' = (%s)0' % typ)))
    ent_lines = ent.splitlines()
    if not ent_lines or ent_lines[0].strip() != 'rvk_draw = rvkLoadDraw();':
        raise ValueError(title + ': unexpected entry start')
    if static_pos and vertex:
        # F12 / F13: the vertex shader keeps the translator's shape (the registers as static globals, the program in its own function): DXC's `precise` of the
        # position then does not reach into the program's arithmetic, as in the translated shader (one entry function with locals marks the dot products
        # NoContraction: 1 ulp differences of the sprite VS's clip position against the translated shader, which GUI edges must not have)
        glob = ['static RvkDrawData rvk_draw;']
        for typ, name, init in statics:
            if name == 'rvk_draw' or not re.search(r'\b%s\b' % re.escape(name), used):
                continue
            glob.append('static %s %s%s;' % (typ, name, init if init else ''))
        head = ['// %s: %s, our port (generated by tools/post_port.py; do not edit by hand, regenerate).' % (ref, title),
                '// Used by: %s.' % ', '.join(techs)]
        names = ctab_names(text)
        if names:
            head.append('// Registers / samplers (the engine\'s constant table): %s.' % '; '.join(names))
        incs = ['#include "post_port.hlsli"']
        if expand is not None:
            # 14.28 addendum: the GPU particle expansion in front of the program (the prologue of Device::particleVs, kKindParticle)
            incs.append('#include "rvk_particle.hlsli"')
            structs = [expansion_inputs(structs[0])] + structs[1:]
            head.append('// The particle expansion (shaders/include/rvk_particle.hlsli) replaces the vertex inputs; the engine\'s own program follows it.')
            ent_lines = [(expansion_prologue(expand) + '\n    rvk_program();') if l.strip() == 'rvk_main();' else l for l in ent_lines]
        else:
            ent_lines = [l.replace('rvk_main();', 'rvk_program();') for l in ent_lines]
        return '\n'.join(head + incs + [''] + structs + [''] + consts + ([''] if consts else []) + glob + [''] +
                         ['void rvk_program() {'] + main.rstrip('\n').splitlines() + ['}', ''] +
                         ['%s {' % sig] + ent_lines + ['}', ''])
    out_ent = ['    const RvkDrawData rvk_draw = rvkLoadDraw();'] + decls
    for l in ent_lines[1:]:
        if l.strip() == 'rvk_main();':
            out_ent.append('    // ---- the program')
            out_ent.extend(main.rstrip('\n').splitlines())
            out_ent.append('    // ---- outputs')
        else:
            out_ent.append(l)
    # (the inputs are read once into the locals above: `v0 = rvk_in.v0;` lines stay as they are)
    head = ['// %s: %s, our port (generated by tools/post_port.py; do not edit by hand, regenerate).' % (ref, title),
            '// Used by: %s.' % ', '.join(techs)]
    names = ctab_names(text)
    if names:
        head.append('// Registers / samplers (the engine\'s constant table): %s.' % '; '.join(names))
    return '\n'.join(head + ['#include "post_port.hlsli"', ''] + structs + [''] + consts + ([''] if consts else []) +
                     ['%s {' % sig] + out_ent + ['}', ''])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--work', required=True, help='scratch directory (outside the repo: the extracted bytecode is game-derived)')
    ap.add_argument('--check', action='store_true', help='rvk_shadereq of every port against the translated shader')
    ap.add_argument('--trials', type=int, default=64)
    a = ap.parse_args()
    fxtool, shtool = os.path.join(BIN, 'rvk_fxtool.exe'), os.path.join(BIN, 'rvk_shadertool.exe')
    bins, hlsl = os.path.join(a.work, 'bin'), os.path.join(a.work, 'hlsl')
    out_dir = os.path.join(ROOT, 'shaders', 'post', 'fx')
    os.makedirs(bins, exist_ok=True)
    ports = {}  # key -> dict
    order = []
    hand = []  # the S-7c hand ports (checked, not generated)
    for fam, stem in FAMILIES:
        fx = os.path.join(a.corpus, stem + '.bin')
        run([fxtool, 'extract', fx, bins])
        for tech, pas, vs, ps in techniques(run([fxtool, 'dump', fx])):
            for idx, stage in ((vs, 'vs'), (ps, 'ps')):
                if idx is None:
                    continue
                f = [n for n in os.listdir(bins) if re.match(r'%s\d\d_%s_s%d\.bin$' % (stage, stem, idx), n)]
                if len(f) != 1:
                    sys.exit('%s %s shader #%d: not extracted' % (fam, tech, idx))
                if stage == 'ps' and (fam, tech) in S7C:
                    hand.append({'name': 's7c_%s_%s' % (fam, tech.lower()), 'stage': 'ps', 'embedded': S7C[(fam, tech)],
                                 'src': os.path.join(hlsl, f[0][:-4] + '.ps.hlsl')})
                    continue
                data = open(os.path.join(bins, f[0]), 'rb').read()
                key = fnv1a(data)
                use = '%s %s%s' % (fam, tech, '' if pas == 'p0' else ' ' + pas)
                if key in ports:
                    if use not in ports[key]['uses']:
                        ports[key]['uses'].append(use)
                    continue
                name = '%s_%s%s' % (fam, tech.lower(), '' if pas == 'p0' else '_' + pas)
                ports[key] = {'name': name, 'stage': stage, 'bin': f[0], 'uses': [use], 'fam': fam, 'idx': idx}
                order.append(key)
    for stem, fam, nm, use in SINGLES:
        stage = stem[:2]
        data = open(os.path.join(a.corpus, stem + '.bin'), 'rb').read()
        fname = '%s_%s_single_s0.bin' % (stem, fam)
        open(os.path.join(bins, fname), 'wb').write(data)
        key = fnv1a(data)
        if key in ports:
            continue
        ports[key] = {'name': '%s_%s' % (fam, nm), 'stage': stage, 'bin': fname, 'uses': [use], 'fam': fam, 'idx': 0}
        order.append(key)
    run([shtool, 'export', bins, hlsl])
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.endswith('.hlsl'):
            os.remove(os.path.join(out_dir, old))
    rows = []
    for key in order:
        p = ports[key]
        vertex = p['stage'] == 'vs'
        src = os.path.join(hlsl, p['bin'][:-4] + ('.vs.hlsl' if vertex else '.ps.hlsl'))
        text = open(src, encoding='utf-8').read()
        title = ('%s shader #%d of the %s effect' % ('vertex' if vertex else 'pixel', p['idx'], p['fam'])) if p['fam'] not in ('sky', 'particle') else (
            '%s shader of the sky dome (ge_skydome_2_0.fx, shader pool)' % ('vertex' if vertex else 'pixel') if p['fam'] == 'sky' else
            '%s shader of the particles (ge_particle_2_0.fx, shader pool)' % ('vertex' if vertex else 'pixel'))
        ref = ('F12 / F13 (docs/research/frame-ownership.md 14.24)' if p['fam'] in ('sky', 'gui') else
               'own particles (docs/research/frame-ownership.md 14.28, particle ports)' if p['fam'] == 'particle' else 'S-7d (docs/research/frame-ownership.md 14.18)')
        p['assign'] = expansion_assign(text) if vertex and p['fam'] == 'particle' else None
        port = convert(text, vertex, title, p['uses'], ref, p['fam'] in ('sky', 'gui', 'particle'), p['assign'])
        fname = p['name'] + ('.vert.hlsl' if vertex else '.frag.hlsl')
        with open(os.path.join(out_dir, fname), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(port)
        p['embedded'] = 'post/fx/' + p['name'] + ('.vert' if vertex else '.frag')
        p['src'] = src
        rows.append('    {0x%016xull, %s, "%s", "%s"},' % (key, 'shader::Stage::Vertex' if vertex else 'shader::Stage::Pixel', p['embedded'], ', '.join(p['uses'])))
    table = os.path.join(ROOT, 'src', 'native', 'post_port_table.inl')
    with open(table, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('// generated by tools/post_port.py (S-7d, docs/research/frame-ownership.md 14.18): FNV-1a 64 of an engine effect shader\'s tokens -> its port\n')
        fh.write('// {key, stage, embedded shader, the techniques that use it}\n')
        fh.write('\n'.join(rows) + '\n')
    print('%d ports (%d vertex, %d pixel) -> %s, table %s' % (len(order), sum(1 for k in order if ports[k]['stage'] == 'vs'),
                                                           sum(1 for k in order if ports[k]['stage'] == 'ps'), out_dir, table))
    if a.check:
        for i, h in enumerate(hand):
            ports[('hand', i)] = h
            order.append(('hand', i))
        check(ports, order, a)


def check(ports, order, a):
    os.environ['RVK_SHADER_DIR'] = os.path.join(ROOT, 'shaders')  # (the ports from disk: no rebuild of the tools needed)
    shtool = os.path.join(BIN, 'rvk_shadertool.exe')
    eq = os.path.join(BIN, 'rvk_shadereq.exe')
    spv = os.path.join(a.work, 'spv')
    os.makedirs(spv, exist_ok=True)
    vs_spv = os.path.join(spv, 'eq_varyings.spv')
    run([shtool, 'hlsl', 'test/eq_varyings.vert', '-o', vs_spv])
    fails = 0
    for key in order:
        p = ports[key]
        a_spv = os.path.join(spv, p['name'] + '_a.spv')
        b_spv = os.path.join(spv, p['name'] + '_b.spv')
        if p.get('assign') is not None:
            # (the translated shader with the expansion hooks the translator adds: Device::particleVs's form)
            inj = os.path.join(spv, p['name'] + '_inj.vs.hlsl')
            open(inj, 'w', encoding='utf-8').write(inject_expansion(open(p['src'], encoding='utf-8').read(), p['assign']))
            p['src'] = inj
        run([shtool, 'hlsl', p['src'], '-o', a_spv])
        run([shtool, 'hlsl', p['embedded'], '-o', b_spv])
        if p['stage'] == 'ps':
            cmd = [eq, '--vs', vs_spv, '--fs', a_spv, b_spv, '--trials', str(a.trials)]
        else:
            cmd = vs_check_cmd(eq, p, a_spv, b_spv, a)
            if cmd is None:
                continue
        r = subprocess.run(cmd, capture_output=True, text=True)
        ok = r.returncode == 0
        fails += 0 if ok else 1
        last = (r.stdout.strip().splitlines() or [''])[-1]
        print('%s %s %-30s %s' % ('EQUAL' if ok else 'DIFF ', p['stage'], p['name'], last))
    print('%d of %d ports differ' % (fails, len(order)))
    sys.exit(1 if fails else 0)


TRI = 'float4(float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0, 0.0, 1.0)'


def vs_wrapped(text, outs):
    """a vertex shader's text with its entry renamed + a wrapper that draws one fixed triangle and exports the clip position and every varying (tools/vs_equiv.py)"""
    body = re.sub(r'\bVSMain\b', 'RvkOrigVS', text) + '\nstruct RvkProbeVsOut {\n    float4 position : SV_Position;\n'
    for k in range(len(outs) + 1):
        body += '    [[vk::location(%d)]] float4 p%d : VARY%d;\n' % (k, k, k)
    body += '};\nRvkProbeVsOut VSMain(RvkVsIn rvk_in) {\n    uint vid = rvk_in.rvk_vertexId;\n    RvkVsOut o = RvkOrigVS(rvk_in);\n    RvkProbeVsOut r;\n'
    body += '    r.position = %s;\n    r.p0 = o.position;\n' % TRI
    for k, loc in enumerate(outs):
        body += '    r.p%d = o.rvk_out%d;\n' % (k + 1, loc)
    return body + '    return r;\n}\n'


def probe_ps(count, pixels=4):
    s = '#include "rvk_draw.hlsli"\nstruct PIn {\n    float4 pos : SV_Position;\n'
    for k in range(count + 1):
        s += '    [[vk::location(%d)]] float4 p%d : VARY%d;\n' % (k, k, k)
    s += '};\nfloat4 PSMain(PIn i) : SV_Target0 {\n    uint px = uint(i.pos.y) * %du + uint(i.pos.x);\n' % pixels
    for k in range(count + 1):
        s += '    rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(pc.ps), (px * %du + %du) * 16u), i.p%d);\n' % (count + 1, k, k)
    return s + '    return float4(0.0, 0.0, 0.0, 0.0);\n}\n'


def vs_check_cmd(eq, p, a_spv, b_spv, a):
    shtool = os.path.join(BIN, 'rvk_shadertool.exe')
    spv = os.path.dirname(a_spv)
    ta = open(p['src'], encoding='utf-8').read()
    tb = open(os.path.join(ROOT, 'shaders', p['embedded'] + '.hlsl'), encoding='utf-8').read()
    outs = sorted(int(m.group(1)) for m in re.finditer(r'rvk_out(\d+)\s*:\s*VARY', ta))
    ins = sorted(int(m.group(1)) for m in re.finditer(r'\[\[vk::location\((\d+)\)\]\] float4 (?:v|rvk_pr)\d+ : ATTR', ta))
    files = {}
    for tag, text in (('a', ta), ('b', tb.replace('#include "post_port.hlsli"', '#include "post/fx/post_port.hlsli"'))):
        f = os.path.join(spv, '%s_%s.vs.hlsl' % (p['name'], tag))
        open(f, 'w', encoding='utf-8').write(vs_wrapped(text, outs))
        files[tag] = os.path.join(spv, '%s_%s_w.spv' % (p['name'], tag))
        run([shtool, 'hlsl', f, '-o', files[tag]])
    pf = os.path.join(spv, '%s_probe.ps.hlsl' % p['name'])
    open(pf, 'w', encoding='utf-8').write(probe_ps(len(outs)))
    ps = os.path.join(spv, '%s_probe.spv' % p['name'])
    run([shtool, 'hlsl', pf, '-o', ps])
    attrs = ','.join('%d:%d' % (k, k) for k in ins)
    return [eq, '--vs-test', '--vsa', files['a'], files['b'], '--attrs-a', attrs, '--attrs-b', attrs, '--probe', ps, '--probe-count', str(len(outs) + 1),
            '--trials', str(a.trials)]


if __name__ == '__main__':
    main()
