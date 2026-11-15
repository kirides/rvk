import sys, re, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census

PAT = re.compile(r'diag: hitch: frame (\d+) interval ([\d.]+) ms = previous present ([\d.]+) \+ CPU ([\d.]+) \(slot wait ([\d.]+)\); this frame: draw calls ([\d.]+) ms, pipelines built (\d+) \(([\d.]+) ms\), shaders built (\d+) \(([\d.]+) ms\), uploads tex (\d+) KiB buf (\d+) KiB, read-backs (\d+) \(([\d.]+) ms\), draws skipped while compiling (\d+), compile jobs pending (\d+)(?:, packets replayed (\d+) / refused (\d+) / captured (\d+), variants built \(all threads\) (\d+))?')

def parse(tag):
    lines, rows, t0, t1 = census.load(tag)
    diag = {}
    for t, s in lines:
        m = PAT.match(s)
        if m:
            g = [float(x) if x is not None else 0.0 for x in m.groups()]
            diag[int(g[0])] = dict(t=t, iv=g[1], prev=g[2], cpu=g[3], slot=g[4], draw=g[5], pipes=g[6], pipems=g[7], shaders=g[8], shms=g[9], tex=g[10], buf=g[11], rb=g[12], rbms=g[13], skip=g[14], jobs=g[15], replayed=g[16], refused=g[17], captured=g[18], variants=g[19])
    d3dx = [t for t, s in lines if 'compiled from source on the engine thread' in s]
    big = [(t, s) for t, s in lines if s.startswith('diag: big upload')]
    return lines, rows, t0, t1, diag, d3dx, big

def classify(h, d3dx):
    iv = h['iv']
    cls = []
    if h['tex'] >= 60000: cls.append('tex64MiB')
    elif h['tex'] >= 4096: cls.append('tex>=4MiB')
    if h['buf'] >= 8192: cls.append('buf>=8MiB')
    if h['pipes'] > 0: cls.append('pipeline')
    if h['shaders'] > 0: cls.append('shader')
    if h['rb'] > 0: cls.append('readback')
    if any(h['t'] - iv - 5 <= t <= h['t'] + 5 for t in d3dx): cls.append('D3DX')
    if h['prev'] > 0.5 * iv: cls.append('present')
    if h['slot'] > 0.3 * iv: cls.append('slotwait')
    if h['refused'] >= 50: cls.append('late-compose')
    if h['captured'] >= 50: cls.append('captures')
    if h['draw'] > 0.4 * h['cpu'] and h['draw'] > 8: cls.append('drawcalls')
    if not cls: cls.append('engine/unattributed')
    return cls

if __name__ == '__main__':
    for tag in sys.argv[1:]:
        lines, rows, t0, t1, diag, d3dx, big = parse(tag)
        rr = [r for r in rows if t0 <= r[1] <= t1]
        hs = [diag[r[0]] for r in rr if r[0] in diag and r[2] > 25]
        cnt = collections.Counter()
        ms = collections.Counter()
        mx = collections.Counter()
        for h in hs:
            c = tuple(classify(h, d3dx))
            cnt[c] += 1
            ms[c] += h['iv']
            mx[c] = max(mx[c], h['iv'])
        print(tag, 'hitch frames >25 ms:', sum(1 for r in rr if r[2] > 25), 'with a diag line:', len(hs))
        for c, n in cnt.most_common():
            print('   ', '+'.join(c), n, 'frames, %.0f ms total, max %.0f' % (ms[c], mx[c]))
        print('   D3DX compiles in the session:', len(d3dx), ' big uploads logged:', len(big))
