#!/usr/bin/env python3
"""Role table of perf-ab pass rows (docs/research/shader-isa.md): Wrapper vs Full GPU ms per scene, passes grouped by role.

  python tools/perf_roles.py <scene>=<wrapper rvk.log>,<full rvk.log> ...

Both logs hold `perf: window <A|B>_<n> pass ...` rows written with RVK_PERF_WINDOW_ORDINAL=1 (tools/perf_passes.py); the windows of one log are averaged.
Roles (by the engine's attachment shapes, the pass keys are `<w>x<h>, <n> colour (<id>), depth <id>[ #k]`):
  z pass       Wrapper: the R32F-depth-map pass (colour 100) with the most draws; Full: the colour-less main-size pass with the most draws (native.depth_z)
  solid        the colour-97 main pass with the most draws;   alpha / refracted: the colour-97 pass with the second most
  cascades     every 2048x2048 colour-less pass (the sun's cascade maps)
  named        `pass <name> at <point>` rows (injected features: light.cluster, light.shadow, rt.scene, water.rt, water.ocean ...)
  other        everything else (post chain, small passes, barriers)
"""
import re
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from perf_passes import parse, mean  # noqa: E402


def roles(path, mode):
    main, passes, draws = parse(path)
    # the Wrapper log is a renderer.mode A/B session (A = wrapper, B = the run-time switch to Full: not the shipped Full); the Full log's arms are identical replicates
    arms = [a for a in (('A',) if mode == 'wrapper' else ('A', 'B')) if passes.get(a)]
    keys = set()
    for a in arms:
        keys |= set(passes[a])
    ms = {k: mean([mean(passes[a].get(k, [])) for a in arms]) for k in keys}
    dr = {k: mean([mean(draws[a].get(k, [])) for a in arms]) for k in keys}
    c97 = sorted([k for k in keys if re.match(r'^\d+x\d+, 1 colour \(97\), depth 130', k)], key=lambda k: -dr[k])
    out = defaultdict(float)
    taken = set()
    if c97:
        out['solid'] += ms[c97[0]]
        taken.add(c97[0])
    if len(c97) > 1 and dr[c97[1]] > 0:
        out['alpha / refracted'] += ms[c97[1]]
        taken.add(c97[1])
    if mode == 'wrapper':
        z = sorted([k for k in keys if '(100)' in k and 'depth 130' in k], key=lambda k: -dr[k])
    else:
        z = sorted([k for k in keys if re.match(r'^\d+x\d+, 0 colour \(0\), depth 130', k)], key=lambda k: -dr[k])
    if z:
        out['z pass'] += ms[z[0]]
        taken.add(z[0])
    for k in keys:
        if k in taken:
            continue
        if k.startswith('2048x2048, 0 colour'):
            out['cascades'] += ms[k]
        elif k.startswith('pass '):
            name = re.sub(r' #\d+$', '', k)
            out[name.replace('pass ', '')] += ms[k]
        else:
            out['other (post, small passes, barriers)'] += ms[k]
    return mean(sum((main[a] for a in arms), [])), out


def main():
    for spec in sys.argv[1:]:
        scene, _, paths = spec.partition('=')
        wp, fp = paths.split(',')
        wm, wr = roles(wp, 'wrapper')
        fm, fr = roles(fp, 'full')
        names = sorted(set(wr) | set(fr), key=lambda n: -max(wr.get(n, 0), fr.get(n, 0)))
        print('\n### %s: GPU frame Wrapper %.2f ms, Full %.2f ms' % (scene, wm, fm))
        print('| pass | Wrapper ms | Full ms |')
        print('|---|---|---|')
        for n in names:
            a, b = wr.get(n, 0.0), fr.get(n, 0.0)
            if max(a, b) < 0.02:
                continue
            print('| %s | %.3f | %.3f |' % (n, a, b))


if __name__ == '__main__':
    main()
