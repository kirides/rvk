#!/usr/bin/env python3
"""Compile the game's shipped material HLSL with d3dx9_39 and compare with the captured corpus (docs/research/shader-permutations.md).

    python tools/hlsl_match.py <fx dir> <corpus dir> <shader> [-D NAME=VALUE ...] [--entry PS_Default] [--profile ps_3_0]
                               [--file ge_default_phong.fx] [--search]

<fx dir> = data\\compiled\\effects\\shadermaterial (read only). Without --search: compile once with the -D macros and report
whether the instruction stream equals <shader>'s (comments = CTAB / creator ignored). --search tries the toggle combinations of
the macros that are not readable from the shader (specular, ambient type, alpha blend, ...) and prints the matching vector.
"""
import argparse, ctypes, difflib, itertools, os, struct, sys
from ctypes import POINTER, c_char_p, c_uint32, c_void_p, c_long, Structure


class Macro(Structure):
    _fields_ = [('Name', c_char_p), ('Definition', c_char_p)]


d3dx = ctypes.WinDLL(os.environ.get('RVK_D3DX', 'd3dx9_29.dll'))  # 9.11.519 = the compiler the game's shader pool was built with
_compile = d3dx.D3DXCompileShaderFromFileA
_compile.argtypes = [c_char_p, POINTER(Macro), c_void_p, c_char_p, c_char_p, c_uint32, POINTER(c_void_p), POINTER(c_void_p), POINTER(c_void_p)]
_compile.restype = c_long


def _vcall(obj, idx, restype, *args):
    vt = ctypes.cast(ctypes.cast(obj, POINTER(c_void_p))[0], POINTER(c_void_p))
    fn = ctypes.CFUNCTYPE(restype, c_void_p, *[c_void_p] * len(args))(vt[idx])
    return fn(obj, *args)


def buffer_bytes(buf):
    ptr = _vcall(buf, 3, c_void_p)
    size = _vcall(buf, 4, c_uint32)
    return ctypes.string_at(ptr, size)


def release(buf):
    if buf:
        _vcall(buf, 2, c_uint32)


def compile_hlsl(path, entry, profile, defines, flags=0):
    macros = (Macro * (len(defines) + 1))(*[Macro(k.encode(), str(v).encode()) for k, v in defines.items()], Macro(None, None))
    code, err = c_void_p(), c_void_p()
    hr = _compile(path.encode(), macros, None, entry.encode(), profile.encode(), flags, ctypes.byref(code), ctypes.byref(err), None)
    msg = buffer_bytes(err).decode('latin1') if err else ''
    release(err)
    if hr < 0:
        return None, msg
    data = buffer_bytes(code)
    release(code)
    return data, msg


def instructions(data):
    """Instruction tokens without comment blocks (CTAB, creator string). The partial-precision bit of the dcl register tokens
    is cleared: the pool's shaders carry _pp on the arithmetic but not on the declarations."""
    t = struct.unpack('<%dI' % (len(data) // 4), data)
    out = [t[0]]
    i = 1
    while i < len(t):
        op = t[i] & 0xffff
        if op == 0xfffe:
            i += 1 + ((t[i] >> 16) & 0x7fff)
        elif op == 0xffff:
            out.append(t[i])
            break
        else:
            n = (t[i] >> 24) & 0xf
            if op == 31 and i + 2 <= len(t):  # dcl: usage token, then the register token
                out += [t[i], t[i + 1], t[i + 2] & ~0x00200000]
                i += 3
                continue
            out.append(t[i])
            i += 1
            out += list(t[i:i + n])
            i += n
    return out


def disasm(data):
    dis = d3dx.D3DXDisassembleShader
    dis.argtypes = [c_char_p, c_uint32, c_char_p, POINTER(c_void_p)]
    b = c_void_p()
    dis(data, 0, None, ctypes.byref(b))
    text = buffer_bytes(b).decode('latin1')
    release(b)
    return [l.strip() for l in text.splitlines() if l.strip() and not l.lstrip().startswith('//')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fxdir')
    ap.add_argument('corpus')
    ap.add_argument('shader')
    ap.add_argument('-D', action='append', default=[])
    ap.add_argument('--entry', default='PS_Default')
    ap.add_argument('--profile', default='ps_3_0')
    ap.add_argument('--file', default='ge_default_phong.fx')
    ap.add_argument('--flags', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--search', action='store_true')
    ap.add_argument('--diff', action='store_true', help='with --search: diff the closest candidate against the corpus shader')
    ap.add_argument('--disasm', action='store_true', help='print the compiled listing')
    a = ap.parse_args()
    ref = instructions(open(os.path.join(a.corpus, a.shader + '.bin'), 'rb').read())
    base = dict(kv.split('=', 1) for kv in a.D)
    base.setdefault('XFX_SHADERMODEL_3', '1')
    base.setdefault('XFX_QUALITY', '2')
    path = os.path.join(a.fxdir, a.file)
    toggles = {'XFX_SPECULAR': ['false', 'true'], 'XFX_AMBIENTLIGHTTYPE': ['0', '1', '2'], 'XFX_ALPHABLEND': ['false', 'true'],
               'XFX_COMPRESSEDNORMALMAP': ['false', 'true'], 'XFX_DOUBLESIDED': ['false', 'true'], 'XFX_MASKED': ['false', 'true']} if a.search else {}
    keys = [k for k in toggles if k not in base]
    tried = 0
    best = (-1, None, None)
    refdata = open(os.path.join(a.corpus, a.shader + '.bin'), 'rb').read()
    for combo in itertools.product(*[toggles[k] for k in keys]):
        d = dict(base)
        d.update(zip(keys, combo))
        data, msg = compile_hlsl(path, a.entry, a.profile, d, a.flags)
        tried += 1
        if data is None:
            if not a.search:
                print('compile error:', msg[:400])
                return 2
            continue  # e.g. vFace with partial precision
        got = instructions(data)
        if a.disasm:
            dis = d3dx.D3DXDisassembleShader
            dis.argtypes = [c_char_p, c_uint32, c_char_p, POINTER(c_void_p)]
            b = c_void_p()
            dis(data, 0, None, ctypes.byref(b))
            print(buffer_bytes(b).decode('latin1'))
        if got == ref:
            print('MATCH after', tried, 'compiles:', {k: v for k, v in d.items() if k not in base or True})
            return 0
        score = difflib.SequenceMatcher(None, got, ref, autojunk=False).ratio()
        if score > best[0]:
            best = (score, d, data)
        if not a.search:
            print('no match: %d tokens vs %d in the corpus shader' % (len(got), len(ref)))
            return 1
    print('no match in %d combinations; closest (%.3f): %s' % (tried, best[0], best[1]))
    if best[2] is not None and a.diff:
        for l in difflib.unified_diff(disasm(refdata), disasm(best[2]), 'corpus', 'compiled', lineterm='', n=1):
            print(l)
    return 1


if __name__ == '__main__':
    sys.exit(main())
