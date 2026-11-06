#!/usr/bin/env python3
"""Does the HLSL material shader (shaders/material/ge_default.{frag,vert}.hlsl) compile for every request the engine made?
(docs/research/shader-permutations.md 8-10, hlsl-porting-guide.md)

    python tools/uber_hlsl_check.py [out/matsrc/cap1.jsonl ...] [--jobs 8] [--hooks none|gi|gbuffer|full|all] [--vs] [--only HASH]
                                    [--limit N] [--show 5]

Input: the JSON lines of `-Setting debug.material_sources=true` runs (default out/matsrc/*.jsonl). For every request of
ge_default_phong.fx / ge_default_leafs.fx (PS_Default / PS_DefaultZPass; with --vs also VS_Default / VS_DefaultZPass) the macros become -D defines (true /
false -> 1 / 0, a body on one line, XFX_PASS=1 for the ZPass entries; as shader/uber.cpp does) and
`rvk_shadertool hlsl material/ge_default.<stage> -D ...` compiles shaders/material/ge_default.<stage>.hlsl from the disk
(RVK_SHADER_DIR) with DXC to SPIR-V and validates it with spirv-val. No GPU involved.

Pixel shader configurations (--hooks): none = the plain shader; gi = RVK_UBER_HOOKS with the ambient from the GI probes (+ the indoor
colour, the interpolated normal); gbuffer = RVK_UBER_HOOKS with the thin G-buffer only (no ambient); full = ambient + G-buffer +
per-object motion. `all` (default) runs every one of them. Exit 0 when everything compiles.
"""
import argparse, concurrent.futures as cf, glob, json, os, subprocess, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, 'build', 'RelWithDebInfo', 'rvk_shadertool.exe')

HOOKS = {
    'none': [],
    'gi': ['RVK_UBER_HOOKS=1', 'RVK_MV_AMBIENT=1', 'RVK_MV_AMB_REG=15', 'RVK_MV_HAS_IND=1', 'RVK_MV_IND_REG=14',
            'RVK_MV_KIND_SKINNED=4', 'RVK_MV_VNORMAL=1'],
    'gbuffer': ['RVK_UBER_HOOKS=1', 'RVK_MV_GBUFFER=1', 'RVK_MV_KIND_SKINNED=4', 'RVK_MV_VNORMAL=1'],
    'full': ['RVK_UBER_HOOKS=1', 'RVK_MV_AMBIENT=1', 'RVK_MV_AMB_REG=15', 'RVK_MV_HAS_IND=1', 'RVK_MV_IND_REG=14',
             'RVK_MV_GBUFFER=1', 'RVK_MV_MOTION=1', 'RVK_MV_KIND_SKINNED=4', 'RVK_MV_VNORMAL=1'],
}


def defines(macros, entry):
    out = []
    for name, value in macros:
        value = {'true': '1', 'false': '0'}.get(value, value)
        value = ' '.join(value.split())  # a macro body on one line
        out += ['-D', '%s=%s' % (name, value)]
    if entry.endswith('ZPass'):
        out += ['-D', 'XFX_PASS=1']
    return out


def compile_one(req, stage, hooks):
    name = 'material/ge_default.' + stage
    cmd = [TOOL, 'hlsl', name] + defines(req['macros'], req['entry'])
    if req['file'] == 'ge_default_leafs.fx':
        cmd += ['-D', 'RVK_LEAFS=1']  # the SpeedTree leaf hooks (shader-permutations.md 11)
    for h in HOOKS[hooks]:
        cmd += ['-D', h]
    env = dict(os.environ, RVK_SHADER_DIR=os.path.join(ROOT, 'shaders'))
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=ROOT)
    out = (r.stdout + r.stderr).strip()
    ok = r.returncode == 0 and ('\nok ' in '\n' + out)
    return {'hash': req['hash'], 'entry': req['entry'], 'stage': stage, 'hooks': hooks, 'ok': ok, 'log': out}


def first_error(log):
    for line in log.splitlines():
        if 'error' in line.lower() or line.startswith('spirv-val'):
            return line.strip()[:240]
    return (log.splitlines() or ['(no output)'])[0][:240]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='*')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--hooks', default='all', choices=['all'] + list(HOOKS))
    ap.add_argument('--vs', action='store_true', help='also compile the vertex shader requests (ge_default.vert.hlsl)')
    ap.add_argument('--only', help='one request hash')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--show', type=int, default=6, help='failing logs printed in full')
    args = ap.parse_args()
    files = args.inputs or sorted(glob.glob(os.path.join(ROOT, 'out', 'matsrc', '*.jsonl')))
    if not os.path.isfile(TOOL):
        print('missing', TOOL)
        return 2
    reqs, seen = [], set()
    for f in files:
        for line in open(f, encoding='utf-8'):
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get('file') not in ('ge_default_phong.fx', 'ge_default_leafs.fx') or r['hash'] in seen:
                continue
            if args.only and r['hash'] != args.only:
                continue
            seen.add(r['hash'])
            reqs.append(r)
    jobs = []
    configs = list(HOOKS) if args.hooks == 'all' else [args.hooks]
    for r in reqs:
        if r['stage'] == 'ps' and r['entry'] in ('PS_Default', 'PS_DefaultZPass'):
            for c in configs:
                jobs.append((r, 'frag', c))
        elif args.vs and r['stage'] == 'vs' and r['entry'] in ('VS_Default', 'VS_DefaultZPass'):
            jobs.append((r, 'vert', 'none'))
    if args.limit:
        jobs = jobs[:args.limit]
    print('%d requests from %d file(s), %d compiles' % (len(reqs), len(files), len(jobs)), flush=True)
    results = []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(compile_one, *j) for j in jobs]
        for i, f in enumerate(cf.as_completed(futs), 1):
            results.append(f.result())
            if i % 50 == 0:
                print('  %d / %d' % (i, len(jobs)), flush=True)
    summary = collections.OrderedDict()
    for r in results:
        key = (r['stage'], r['hooks'])
        s = summary.setdefault(key, [0, 0])
        s[0 if r['ok'] else 1] += 1
    print()
    for (stage, hooks), (ok, bad) in summary.items():
        print('%-5s hooks=%-8s compiled %4d  failed %4d' % (stage, hooks, ok, bad))
    bad = [r for r in results if not r['ok']]
    print('total: %d compiled, %d failed' % (len(results) - len(bad), len(bad)))
    if bad:
        groups = collections.Counter(first_error(r['log']) for r in bad)
        print('\nfailure groups:')
        for msg, n in groups.most_common(12):
            print('  %4d  %s' % (n, msg))
        for r in bad[:args.show]:
            print('\n---- %s %s %s hooks=%s ----\n%s' % (r['hash'], r['entry'], r['stage'], r['hooks'], r['log'][:2500]))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
