#!/usr/bin/env python3
"""Does the GLSL material shader (shaders/material/ge_default.frag), built from the macros the engine asked for, equal the game's
precompiled pixel shader? (docs/research/shader-permutations.md 8)

    python tools/ubershader_check.py out/matsrc/*.jsonl [--corpus-glsl game-shaders] [--jobs 6] [--trials 16] [--only HASH]
                                     [--limit N] [-o out/ubershader]

Input: the JSON lines of `-Setting debug.material_sources=true` runs (bin/rvk/cache/materials/sources.jsonl: one line per
shader with its effect, entry function and the final macro list). For every pixel shader of ge_default_phong.fx (PS_Default /
PS_DefaultZPass) the translated game shader (game-shaders/ps30_<corpus>.frag from `rvk_shadertool export corpus game-shaders`)
and the ubershader with the same macros as -D defines run on the GPU with identical random constants / varyings / textures
(rvk_shadereq, the check of tools/shader_equiv.py) and the colours are compared.

Result per shader: EQUAL, DIFF (colours differ), NO-GLSL (the shader is not in the corpus export), UNSUPPORTED (other effect /
entry), PORT-ERR (the ubershader does not compile with these macros: stderr kept), TRANSLATED-ERR. Writes <out>/report.json and
prints a summary.
"""
import argparse, concurrent.futures as cf, glob, json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, 'build', 'RelWithDebInfo')
GLSLC = os.path.join(os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.363.0'), 'Bin', 'glslc.exe')
INC = os.path.join(ROOT, 'shaders', 'include')
MAT = os.path.join(ROOT, 'shaders', 'material')


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def defines(macros, entry):
    out = []
    for name, value in macros:
        value = {'true': '1', 'false': '0'}.get(value, value)
        value = ' '.join(value.split())  # a macro body on one line
        out.append('-D%s=%s' % (name, value))
    if entry.endswith('ZPass'):
        out.append('-DXFX_PASS=1')
    return out


def check(entry, tmp, glsl_dir, vs_spv, trials, keep=False):
    h = entry['corpus']
    res = {'hash': entry['hash'], 'corpus': h, 'entry': entry['entry'], 'file': entry['file']}
    if entry['file'] != 'ge_default_phong.fx' or entry['entry'] not in ('PS_Default', 'PS_DefaultZPass'):
        res['status'] = 'UNSUPPORTED'
        return res
    src = os.path.join(glsl_dir, 'ps30_%s.frag' % h)
    if not os.path.isfile(src):
        res['status'] = 'NO-GLSL'
        return res
    a_spv = os.path.join(tmp, 'a_%s.spv' % h)
    b_spv = os.path.join(tmp, 'b_%s.spv' % h)
    r = run([GLSLC, '--target-env=vulkan1.3', '-I', os.path.join(glsl_dir, 'include'), src, '-o', a_spv])
    if r.returncode:
        res['status'] = 'TRANSLATED-ERR'
        res['log'] = r.stderr.strip()[:400]
        return res
    r = run([GLSLC, '--target-env=vulkan1.3', '-I', INC, '-I', MAT, '-fshader-stage=frag', os.path.join(MAT, 'ge_default.frag'),
             '-o', b_spv] + defines(entry['macros'], entry['entry']))
    if r.returncode:
        res['status'] = 'PORT-ERR'
        res['log'] = r.stderr.strip()[:600]
        return res
    r = run([os.path.join(BIN, 'rvk_shadereq.exe'), '--vs', vs_spv, '--fs', a_spv, b_spv, '--trials', str(trials)])
    tail = r.stdout.strip().splitlines()
    last = tail[-1] if tail else r.stderr.strip()[:300]
    res['status'] = 'EQUAL' if r.returncode == 0 and last.startswith('EQUAL') else 'DIFF'
    res['detail'] = last[:300]
    res['full'] = r.stdout.strip()[-1500:]
    for p in () if keep else (a_spv, b_spv):
        try:
            os.remove(p)
        except OSError:
            pass
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db', nargs='+', help='sources.jsonl files (globs ok); the union by shader hash is checked')
    ap.add_argument('--corpus-glsl', default=os.path.join(ROOT, 'game-shaders'))
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--trials', type=int, default=16)
    ap.add_argument('--only', default='')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--keep', action='store_true', help='keep the .spv files of the checked shaders (with --only)')
    ap.add_argument('-v', action='store_true', help='print the harness output of every DIFF')
    ap.add_argument('-o', '--out', default=os.path.join(ROOT, 'out', 'ubershader'))
    a = ap.parse_args()

    entries = {}
    for pat in a.db:
        for f in glob.glob(pat):
            for line in open(f, encoding='utf-8'):
                j = json.loads(line)
                if j['stage'] == 'ps':
                    entries.setdefault(j['hash'], j)
    todo = [e for e in entries.values() if not a.only or a.only in (e['hash'], e['corpus'])]
    todo.sort(key=lambda e: e['hash'])
    if a.limit:
        todo = todo[:a.limit]
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='ubershader_')
    vs_spv = os.path.join(tmp, 'eq_varyings.spv')
    r = run([GLSLC, '--target-env=vulkan1.3', '-I', INC, os.path.join(ROOT, 'shaders', 'test', 'eq_varyings.vert'), '-o', vs_spv])
    if r.returncode:
        print(r.stderr)
        return 2
    t0 = time.time()
    results = []
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(check, e, tmp, a.corpus_glsl, vs_spv, a.trials, a.keep) for e in todo]
        for i, f in enumerate(cf.as_completed(futs)):
            results.append(f.result())
            if (i + 1) % 25 == 0:
                print('  %d / %d (%.0f s)' % (i + 1, len(todo), time.time() - t0), flush=True)
    results.sort(key=lambda r: r['hash'])
    json.dump(results, open(os.path.join(a.out, 'report.json'), 'w'), indent=1)
    counts = {}
    for r in results:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    print('\n%d pixel shaders: %s (%.0f s)' % (len(results), ', '.join('%s %d' % kv for kv in sorted(counts.items())), time.time() - t0))
    for st in ('DIFF', 'PORT-ERR', 'TRANSLATED-ERR'):
        shown = [r for r in results if r['status'] == st][:6]
        for r in shown:
            print('  %s %s %s: %s' % (st, r['hash'], r['entry'], (r.get('detail') or r.get('log') or '')[:200].replace('\n', ' | ')))
            if a.v and r.get('full'):
                print(r['full'])
    if a.keep:
        print('spv files in', tmp)
    return 0 if counts.get('EQUAL', 0) == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
