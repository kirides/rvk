"""spikecat.py <tag> [...] : spike-profile samples (RVK_SPIKE_MS) of a session by category (substring rules over the symbolized stacks)
usage: python spikecat.py PF1 PF2   (reads <tag>_rvk.log; the d3d9.dll of that build symbolizes)"""
import sys, re, os, subprocess, collections
D = os.environ.get('WS26', r'B:\DevDrive\rvk-work\scratch\ws26')
DLL = os.environ.get('RVK_DLL', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'RelWithDebInfo', 'd3d9.dll'))  # the d3d9.dll of the profiled run
SYM = r'E:\apps\clang\bin\llvm-symbolizer.exe'
cache = {}
def symbolize(rvas):
    need = [r for r in rvas if r not in cache]
    if need:
        inp = '\n'.join('0x%x' % (0x180000000 + r) for r in need) + '\n'
        out = subprocess.run([SYM, '--obj=' + DLL, '--functions=short', '--demangle', '--inlining=false'], input=inp, capture_output=True, text=True).stdout
        blocks = out.strip().split('\n\n')
        for r, b in zip(need, blocks):
            cache[r] = b.split('\n')[0].strip()
    return [cache[r] for r in rvas]

RULES = [
    ('water floor field upload (Water::serviceFloor)', ['Water::serviceFloor', 'uploadHalf4']),
    ('water observeOpaque (Water::observeOpaque, vector copy)', ['Water::observeOpaque']),
    ('rt scene prepareInputs / buildPending (zeroed vectors)', ['RtScene::prepareInputs', 'RtScene::buildPending']),
    ('rt scene other (RtScene::*)', ['RtScene::']),
    ('cluster apply (Device::clusterApply)', ['clusterApply']),
    ('owned static ops (native::StaticOps*)', ['native::StaticOps', 'native::']),
    ('sector / entity read (OnRead, ReadObject)', ['OnRead@', 'ReadObject', 'OnLoadDynamicContext']),
    ('lazy resource queue (eCResourceModule2::Process)', ['eCResourceModule2']),
    ('physics FetchResults', ['FetchResults']),
    ('sound (fmod)', ['fmod']),
    ('d3dx9 / texture decode', ['d3dx9']),
    ('HAL texture upload / lock', ['Texture::upload', 'convertLevel', 'Texture::lock']),
]
def analyse(tag):
    cats = collections.Counter()
    total = 0
    rows = []
    n = 0
    for l in open(os.path.join(D, tag + '_rvk.log'), encoding='utf-8', errors='replace'):
        m = re.search(r'diag: spike: (\d+) samples', l)
        if m:
            n = int(m.group(1)); total += n
            continue
        m = re.search(r'diag: spike stack +([\d.]+) % (.*)$', l)
        if m:
            rows.append((float(m.group(1)) * n / 100.0, m.group(2)))
    allr = set()
    for _, s in rows:
        for t in re.findall(r'd3d9@0x([0-9a-f]+)', s): allr.add(int(t, 16))
    symbolize(sorted(allr))
    seen = 0.0
    for w, s in rows:
        txt = re.sub(r'd3d9@0x([0-9a-f]+)', lambda m: cache.get(int(m.group(1), 16), 'd3d9@' + m.group(1)), s)
        cat = 'other'
        for name, keys in RULES:
            if any(k in txt for k in keys):
                cat = name
                break
        cats[cat] += w
        seen += w
    print(tag, 'spike samples', total, '(%.0f ms), stacks listed cover %.0f samples' % (total * 0.5, seen))
    for c, w in cats.most_common():
        print('   %6.0f samples %5.1f %%  %s' % (w, 100 * w / max(seen, 1), c))
    return cats

if __name__ == '__main__':
    for t in sys.argv[1:]: analyse(t)
