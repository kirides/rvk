#!/usr/bin/env python3
"""Does the engine's own ge_default_leafs.fx, compiled with d3dx9_29 for each captured request, equal the corpus bytecode?
(docs/research/shader-permutations.md 11)

    python tools/leafs_match.py <patched fx dir> <corpus dir> out/matsrc/*.jsonl [--file ge_default_leafs.fx]

<patched fx dir> = `python tools/hlsl_infer.py <shadermaterial dir> --make-fx <dst>` (the pool's older `half3 vecWorldEye`).
Every request of the effect file (pixel and vertex shaders) is compiled with its macros (XFX_SHADERMODEL_3 added, entry and profile
of the request) and the instruction stream compared with `<ps30|vs30>_<corpus>.bin`. Exit 0 when all match.
"""
import argparse, glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hlsl_match as hm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fxdir')
    ap.add_argument('corpus')
    ap.add_argument('db', nargs='+')
    ap.add_argument('--file', default='ge_default_leafs.fx')
    a = ap.parse_args()
    reqs = {}
    for pat in a.db:
        for f in glob.glob(pat):
            for line in open(f, encoding='utf-8'):
                j = json.loads(line)
                if j['file'] == a.file:
                    reqs.setdefault(j['hash'], j)
    ok = bad = 0
    for h, j in sorted(reqs.items()):
        stage = j['stage']
        name = '%s30_%s' % (stage, j['corpus'])
        path = os.path.join(a.corpus, name + '.bin')
        if not os.path.isfile(path):
            print('%-14s %s NO-CORPUS' % (j['entry'], name))
            bad += 1
            continue
        ref = hm.instructions(open(path, 'rb').read())
        d = dict(j['macros'])
        d.setdefault('XFX_SHADERMODEL_3', '1')
        data, msg = hm.compile_hlsl(os.path.join(a.fxdir, a.file), j['entry'], '%s_3_0' % stage, d)
        if data is None:
            print('%-14s %s COMPILE-ERR %s' % (j['entry'], name, msg[:200].replace('\n', ' ')))
            bad += 1
            continue
        same = hm.instructions(data) == ref
        if same and stage == 'vs':
            # the leaf VS is ge_default_vs.fx, shared with ge_default_phong.fx: the same macros give the same bytecode there (so the
            # GPU-proven vertex ubershader, ge_default.vert, covers it)
            other, _ = hm.compile_hlsl(os.path.join(a.fxdir, 'ge_default_phong.fx'), j['entry'], 'vs_3_0', d)
            if other is None or hm.instructions(other) != ref:
                print('               (the phong effect compiles this VS differently)')
                same = False
        print('%-14s %s %s' % (j['entry'], name, 'MATCH' if same else 'DIFF'))
        ok += same
        bad += not same
    print('%d of %d match' % (ok, ok + bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
