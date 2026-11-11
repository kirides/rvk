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
]
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


def convert(text, vertex, title, techs):
    """the translator's HLSL -> the port's text"""
    lines = text.splitlines()
    body = [l for l in lines if not l.startswith('//')]
    src = '\n'.join(body)
    structs = re.findall(r'^struct \w+ \{.*?^\};', src, re.S | re.M)
    if len(structs) != 2:
        raise ValueError('%s: expected two structs, found %d' % (title, len(structs)))
    statics = re.findall(r'^static (?!const)([\w]+) (\w+)( = [^;]*)?;$', src, re.M)
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
    out_ent = ['    const RvkDrawData rvk_draw = rvkLoadDraw();'] + decls
    for l in ent_lines[1:]:
        if l.strip() == 'rvk_main();':
            out_ent.append('    // ---- the program')
            out_ent.extend(main.rstrip('\n').splitlines())
            out_ent.append('    // ---- outputs')
        else:
            out_ent.append(l)
    # (the inputs are read once into the locals above: `v0 = rvk_in.v0;` lines stay as they are)
    head = ['// S-7d (docs/research/frame-ownership.md 14.18): %s, our port (generated by tools/post_port.py; do not edit by hand, regenerate).' % title,
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
        title = '%s shader #%d of the %s effect' % ('vertex' if vertex else 'pixel', p['idx'], p['fam'])
        port = convert(text, vertex, title, p['uses'])
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
    ins = sorted(int(m.group(1)) for m in re.finditer(r'\[\[vk::location\((\d+)\)\]\] float4 v\d+ : ATTR', ta))
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
