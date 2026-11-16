#!/usr/bin/env python3
"""Summarise shader register / occupancy / ISA numbers (docs/research/shader-isa.md).

Inputs
  --driver <dir>   a RVK_ISA_DUMP directory (src/vk/isa_dump.*: stats.csv + <seq>_<label>.txt with the driver's ISA text): the REAL numbers of the
                   installed driver (RDNA3 on the RX 7900 XTX); wave size and instruction count are read from the ISA text
  --rga <csv>      scripts/isa-survey.ps1's rga_survey.csv (RGA offline LLPC, per asic: gfx1100 / gfx1035)
Output: a markdown table per input (sorted by VGPRs), occupancy = waves per SIMD from the VGPR file.

Occupancy model (vector registers only; LDS / SGPR limits are noted separately):
  RDNA2 (gfx103x, Radeon 680M = gfx1035): 1024 VGPRs per SIMD lane slot, allocation granule 8, max 16 waves / SIMD
  RDNA3 (gfx1100, RX 7900 XTX):           1536 VGPRs per SIMD, granule 8 (wave32), max 16 waves / SIMD
  wave64 holds two halves per register: it consumes 2x the file per wave (pixel / vertex stages are wave64 on the 7900 XTX driver, compute wave32).
"""
import argparse
import csv
import os
import re
import sys

FILE = {'gfx1100': 1536, 'gfx1035': 1024, 'gfx1030': 1024}
MAXW = 16


def occupancy(vgprs, asic, wave=32):
    if vgprs <= 0:
        return 0
    alloc = ((vgprs + 7) // 8) * 8
    if wave == 64:
        alloc *= 2
    return max(1, min(MAXW, FILE[asic] // alloc))


INSTR = re.compile(r'^\s+([sv]_|buffer_|image_|global_|ds_|scratch_|flat_|exp |lds_|tbuffer_)')


def parse_isa_text(path):
    """(instruction count, wave size, spill-ish scratch ops, vmem ops, smem loads, ds ops, branches) of the ISA sections of a dump file."""
    n = 0
    wave = 0
    scratch = vmem = smem = ds = br = vdual = 0
    inisa = False
    wave64 = wave32 = 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('----- begin ISA'):
                inisa = True
                continue
            if line.startswith('----- end ISA'):
                inisa = False
                continue
            if not inisa:
                continue
            if INSTR.match(line):
                n += 1
                op = line.split()[0]
                if op.startswith('scratch_') or 'scratch' in op:
                    scratch += 1
                if op.startswith(('buffer_', 'image_', 'global_', 'flat_', 'tbuffer_')):
                    vmem += 1
                if op.startswith('s_load') or op.startswith('s_buffer_load'):
                    smem += 1
                if op.startswith('ds_'):
                    ds += 1
                if op.startswith(('s_cbranch', 's_branch')):
                    br += 1
                if '::' in line.split(';')[0] and 'v_dual' in op:
                    vdual += 1
                if 'exec_lo' in line or 'vcc_lo' in line:
                    wave32 += 1
                elif re.search(r'\b(exec|vcc)\b(?!_)', line):
                    wave64 += 1
    wave = 64 if wave64 > wave32 else 32
    return dict(instr=n, wave=wave, scratch_ops=scratch, vmem=vmem, smem=smem, ds=ds, branches=br, dual=vdual)


def stat(d, key, default=0):
    return int(d.get(key, default))


def driver_rows(dirpath):
    rows = []
    with open(os.path.join(dirpath, 'stats.csv'), encoding='utf-8', newline='') as f:
        for rec in csv.reader(f):
            if len(rec) < 5:
                continue
            seq, label, ex, name, sub = rec[:5]
            st = {}
            for kv in rec[5:]:
                k, _, v = kv.partition('=')
                st[k] = v
            rows.append(dict(seq=seq, label=label, exe=int(ex), name=name, sub=int(sub), st=st))
    out = []
    for r in rows:
        # the file of the pipeline: <seq>_*.txt
        files = [x for x in os.listdir(dirpath) if x.startswith(r['seq'] + '_') and x.endswith('.txt')]
        isa = dict(instr=0, wave=0)
        if files:
            # one section per executable: parse the whole file once per executable index by splitting on "=== executable"
            txt = open(os.path.join(dirpath, files[0]), encoding='utf-8', errors='replace').read()
            parts = txt.split('\n=== executable ')
            if r['exe'] + 1 < len(parts):
                tmp = os.path.join(dirpath, '_part.tmp')
                open(tmp, 'w', encoding='utf-8').write(parts[r['exe'] + 1])
                isa = parse_isa_text(tmp)
                os.remove(tmp)
        v = stat(r['st'], 'numUsedVgprs')
        wave = isa.get('wave', 32) or 32
        out.append(dict(label=r['label'], exe=r['name'], vgprs=v, sgprs=stat(r['st'], 'numUsedSgprs'), lds=stat(r['st'], 'ldsUsageSizeInBytes'),
                        scratch=stat(r['st'], 'scratchMemUsageInBytes'), wave=wave, subgroup=r['sub'], occ3=occupancy(v, 'gfx1100', wave),
                        occ2=occupancy(v, 'gfx1035', wave), **{k: isa.get(k, 0) for k in ('instr', 'scratch_ops', 'vmem', 'smem', 'ds', 'branches', 'dual')}))
    return out


def rga_rows(path):
    out = []
    with open(path, encoding='utf-8-sig', newline='') as f:
        for r in csv.DictReader(f):
            out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--driver')
    ap.add_argument('--rga')
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--filter', default='')
    a = ap.parse_args()
    if a.driver:
        rows = [r for r in driver_rows(a.driver) if a.filter in r['label']]
        rows.sort(key=lambda r: (-r['vgprs'], r['label']))
        if a.top:
            rows = rows[:a.top]
        print('| pipeline (executable) | wave | VGPR | SGPR | LDS B | scratch B | instr | vmem | s_load | ds | branch | occ RDNA3 | occ RDNA2 |')
        print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
        for r in rows:
            print('| %s (%s) | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d |' % (
                r['label'][:90], r['exe'].replace('ShaderProperties.', ''), r['wave'], r['vgprs'], r['sgprs'], r['lds'], r['scratch'], r['instr'], r['vmem'],
                r['smem'], r['ds'], r['branches'], r['occ3'], r['occ2']))
    if a.rga:
        rows = [r for r in rga_rows(a.rga) if a.filter in r['shader']]
        by = {}
        for r in rows:
            by.setdefault(r['shader'], {})[r['asic']] = r
        print('| shader | stage | gfx1100 VGPR / SGPR / LDS / scratch / instr | occ | gfx1035 VGPR / SGPR / LDS / scratch / instr | occ |')
        print('|---|---|---|---|---|---|')
        items = sorted(by.items(), key=lambda kv: -int(kv[1].get('gfx1100', kv[1].get('gfx1035', {'vgprs': 0}))['vgprs']))
        if a.top:
            items = items[:a.top]
        for name, d in items:
            cells = []
            st = ''
            for asic in ('gfx1100', 'gfx1035'):
                r = d.get(asic)
                if r is None or r['status'] != 'ok':
                    cells += ['RGA failed', '-']
                    continue
                st = r['stage']
                v = int(r['vgprs'])
                # (the driver compiles pixel and vertex stages as wave64, compute as wave32: measured with RVK_ISA_DUMP, docs/research/shader-isa.md)
                wave = 32 if r['stage'] == 'comp' else 64
                cells += ['%s / %s / %s / %s / %s' % (v, r['sgprs'], r['lds'], r['scratch'], r['isa_instr']), str(occupancy(v, asic, wave))]
            print('| %s | %s | %s | %s | %s | %s |' % (name, st, *cells))


if __name__ == '__main__':
    sys.exit(main())
