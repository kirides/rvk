"""Generates the C++ declarations of Risen's graphics HAL (eCAPIInstance, eCAPIDevice, eCAPITexture, ...) and the tracing
wrappers from Engine.dll's vtables and demangled export names.

    python tools/gen_hal.py [--engine <Risen>\\bin\\Engine.dll] [--demangled %TEMP%\\risen_re\\Engine.demangled.txt]

Writes src/hal/api_generated.h and src/hal/trace_generated.h. The slot order comes from the engine's own vtables (the eCAPI*
interface vtable gives the slot count and the destructor slot, the eCDX9* implementation vtable gives names and signatures), so
the generated classes have exactly the engine's vtable layout. Overloads get unique names (MSVC groups overloaded virtuals,
which would reorder slots). docs/research/engine-gfx.md section 1.1, docs/research/hal.md.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from dataclasses import dataclass, field

import pefile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ENGINE = r"B:\SteamLibrary\steamapps\common\Risen\bin\Engine.dll"
DEFAULT_DEMANGLED = os.path.join(os.environ.get("TEMP", ""), "risen_re", "Engine.demangled.txt")

# interface -> implementation class. Order = declaration order in the generated header (dependencies first).
INTERFACES = [
    ("eCAPIVertexData", "eCDX9VertexData"),
    ("eCAPIIndexData", "eCDX9IndexData"),
    ("eCAPITexture", "eCDX9Texture"),
    ("eCAPICubeMap", "eCDX9CubeMap"),
    ("eCAPIVolume", "eCDX9Volume"),
    ("eCAPIPrimitive", "eCDX9Primitive"),
    ("eCAPIHLSLShader", "eCDX9HLSLShader"),
    ("eCAPIEffect", "eCDX9Effect"),
    ("eCAPISprite", "eCDX9Sprite"),
    ("eCAPIFont", "eCDX9Font"),
    ("eCAPIDevice", "eCDX9Device"),
    ("eCAPIInstance", "eCDX9Instance"),
]
# eCAPIPrimitive has no reference count (its constructor only sets the vtable); every other interface keeps one at +8.
NO_REFCOUNT = {"eCAPIPrimitive"}

PRIMITIVES = {
    "void", "bool", "char", "wchar_t", "short", "int", "long", "float", "double", "unsigned", "signed", "__int64", "const",
}
WINDOWS_TYPES = {"tagRECT", "HWND__", "HDC__"}


@dataclass
class Param:
    ctype: str          # C++ type as written in the declaration
    by_value_class: bool = False  # a non-trivial class passed by value: ABI = pointer to a caller copy, callee destroys


@dataclass
class Slot:
    index: int
    name: str
    ret: str = "void"
    params: list[Param] = field(default_factory=list)
    const: bool = False
    is_dtor: bool = False
    rva: int = 0


@dataclass
class Interface:
    name: str
    impl: str
    slots: list[Slot]


def split_params(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class TypeCollector:
    def __init__(self) -> None:
        self.classes: set[str] = set()       # top-level classes/structs
        self.enums: set[str] = set()         # top-level enums
        self.nested: dict[str, dict[str, str]] = {}  # outer -> {name: "struct"|"enum"}
        self.templates: set[str] = set()

    def note(self, kind: str, qualified: str) -> None:
        if "<" in qualified:
            base = qualified.split("<", 1)[0]
            self.templates.add(base)
            return
        if "::" in qualified:
            outer, inner = qualified.split("::", 1)
            self.nested.setdefault(outer, {})[inner] = kind
        elif kind == "enum":
            self.enums.add(qualified)
        else:
            self.classes.add(qualified)


TYPE_RE = re.compile(r"\b(class|struct|enum) ([A-Za-z_][\w:]*(?:<[^>]*>)?)")


def convert_type(t: str, types: TypeCollector) -> str:
    """'class bTValArray<class bCVector> const &' -> 'bTValArray<bCVector> const&' and note the types."""
    for kind, name in TYPE_RE.findall(t):
        types.note("enum" if kind == "enum" else "struct" if kind == "struct" else "class", name)
        if "<" in name:
            for k2, n2 in TYPE_RE.findall(name[name.index("<"):]):
                types.note(k2, n2)
    t = re.sub(r"\b(class|struct|enum) ", "", t)
    t = re.sub(r"\s*\*", "*", t)
    t = re.sub(r"\s*&", "&", t)
    return t.strip()


def parse_signature(sig: str, cls: str, types: TypeCollector) -> tuple[str, str, list[Param], bool]:
    m = re.match(r"^(?P<ret>.*?)\s*\b" + cls + r"::(?P<name>~?\w+)\((?P<params>.*)\)(?P<const>const)?\s*$", sig)
    if not m:
        raise ValueError(f"cannot parse: {sig}")
    ret = convert_type(m.group("ret"), types) if m.group("ret") else "void"
    params: list[Param] = []
    raw = m.group("params")
    if raw.strip() != "void":
        for i, p in enumerate(split_params(raw)):
            by_value = bool(re.match(r"^(class|struct) [\w:<>, ]+$", p.strip())) and "*" not in p and "&" not in p
            ct = convert_type(p, types)
            if "(*)" in ct:
                ct = ct.replace("(*)", f"(*a{i})")
            params.append(Param(ct, by_value))
    return ret, m.group("name"), params, m.group("const") is not None


def load_names(path: str) -> dict[int, list[str]]:
    names: dict[int, list[str]] = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split(None, 2)
            if len(parts) < 3:
                continue
            names.setdefault(int(parts[0], 16), []).append(parts[2])
    return names


def load_exports(pe: pefile.PE) -> dict[str, int]:
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    return {e.name.decode(): e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}


def read_vtable(pe: pefile.PE, img: bytes, rva: int) -> list[int]:
    ib = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
    out = []
    while True:
        p = struct.unpack_from("<Q", img, rva + 8 * len(out))[0] - ib
        if not lo <= p < hi:
            return out
        out.append(p)


def unique_names(slots: list[Slot]) -> None:
    seen: dict[str, int] = {}
    for s in slots:
        seen[s.name] = seen.get(s.name, 0) + 1
    for s in slots:
        if seen[s.name] > 1 and not s.is_dtor:
            joined = " ".join(p.ctype for p in s.params)
            if "wchar_t" in joined or "bCUnicodeString" in joined:
                s.name += "_W"
            elif "char" in joined or "bCString" in joined:
                s.name += "_A"
            elif s.const:
                s.name += "_const"
            else:
                s.name += f"_{s.index}"


def build(engine: str, demangled: str) -> tuple[list[Interface], TypeCollector]:
    pe = pefile.PE(engine, fast_load=True)
    img = pe.get_memory_mapped_image()
    exports = load_exports(pe)
    names = load_names(demangled)
    types = TypeCollector()
    result = []
    purecall = None
    for iface, impl in INTERFACES:
        api_vt = read_vtable(pe, img, exports[f"??_7{iface}@@6B@"])
        impl_vt = read_vtable(pe, img, exports[f"??_7{impl}@@6B@"])
        if purecall is None:
            purecall = max(set(api_vt), key=api_vt.count)
        if len(impl_vt) < len(api_vt):
            raise SystemExit(f"{impl} vtable shorter than {iface}")
        slots = []
        for i, target in enumerate(api_vt):
            impl_rva = impl_vt[i]
            if target != purecall:
                slots.append(Slot(i, f"~{iface}", is_dtor=True, rva=impl_rva))
                continue
            cands = [n for n in names.get(impl_rva, []) if f"{impl}::" in n]
            if not cands:
                raise SystemExit(f"{iface} slot {i}: no {impl} name at 0x{impl_rva:x}")
            ret, name, params, const = parse_signature(cands[0], impl, types)
            slots.append(Slot(i, name, ret, params, const, rva=impl_rva))
        dtors = [s for s in slots if s.is_dtor]
        if len(dtors) != 1:
            raise SystemExit(f"{iface}: expected one destructor slot, found {len(dtors)}")
        unique_names(slots)
        result.append(Interface(iface, impl, slots))
    return result, types


def decl_params(s: Slot) -> str:
    out = []
    for i, p in enumerate(s.params):
        if p.by_value_class:
            out.append(f"{p.ctype}* a{i} /* by value: caller copy, callee destroys */")
        elif "(*a" in p.ctype:
            out.append(p.ctype)
        else:
            out.append(f"{p.ctype} a{i}")
    return ", ".join(out)


HEADER = """// GENERATED by tools/gen_hal.py from Engine.dll's vtables and export names. Do not edit; rerun the generator.
// Slot order = the engine's vtable order (docs/research/hal.md). All interfaces except eCAPIPrimitive keep a reference count
// at +8, managed by the engine's non-virtual AddRef/Release; Release destroys through the destructor slot and frees with
// bCMemoryAdmin::DeleteObject, so implementations must be allocated with bCMemoryAdmin (hal::EngineObject).
"""


def gen_api(ifaces: list[Interface], types: TypeCollector) -> str:
    iface_names = {i.name for i in ifaces}
    lines = [HEADER, "#pragma once", "", '#include "hal/types_base.h"', ""]
    for w in sorted(WINDOWS_TYPES):
        lines.append(f"struct {w};")
    lines += ["", "namespace rvk::hal {", ""]
    for t in sorted(types.templates):
        lines.append(f"template <class T> class {t};")
    for c in sorted(types.classes - iface_names - WINDOWS_TYPES - set(types.nested)):
        lines.append(f"class {c};")
    for e in sorted(types.enums - {"bEResult"}):
        lines.append(f"enum {e} : int;")
    for i in ifaces:
        lines.append(f"class {i.name};")
    lines.append("")
    for outer, inner in sorted(types.nested.items()):
        if outer in iface_names:
            continue
        lines.append(f"class {outer} {{")
        lines.append("public:")
        for name, kind in sorted(inner.items()):
            lines.append(f"    enum {name} : int;" if kind == "enum" else f"    struct {name};")
        lines.append("};")
        lines.append("")
    for i in ifaces:
        nested = types.nested.get(i.name, {})
        lines.append(f"// {len(i.slots)} slots, engine vtable of {i.impl}")
        lines.append(f"class {i.name} : public EngineObject {{")
        lines.append("public:")
        for name, kind in sorted(nested.items()):
            lines.append(f"    enum {name} : int;" if kind == "enum" else f"    struct {name};")
        for s in i.slots:
            if s.is_dtor:
                lines.append(f"    /* {s.index:3d} */ virtual ~{i.name}() = default;")
            else:
                c = " const" if s.const else ""
                lines.append(f"    /* {s.index:3d} */ virtual {s.ret} {s.name}({decl_params(s)}){c} = 0;")
        if i.name not in NO_REFCOUNT:
            lines += ["", "    // +8: the reference count of the engine's AddRef/Release (starts at 1)", "    int refCount = 1;"]
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace rvk::hal")
    lines.append("")
    return "\n".join(lines)


def is_iface_ptr(ctype: str, iface_names: set[str]) -> str | None:
    """'eCAPITexture*' -> 'eCAPITexture' (an input object)."""
    m = re.fullmatch(r"(\w+)\*", ctype)
    return m.group(1) if m and m.group(1) in iface_names else None


def is_iface_out(ctype: str, iface_names: set[str]) -> str | None:
    """'eCAPITexture**' or 'eCAPITexture*&' -> 'eCAPITexture' (an object handed back to the engine)."""
    m = re.fullmatch(r"(\w+)\*(\*|&)", ctype)
    return m.group(1) if m and m.group(1) in iface_names else None


def gen_slots(ifaces: list[Interface]) -> str:
    lines = [HEADER, "#pragma once", "", "namespace rvk::hal {", ""]
    for n, i in enumerate(ifaces):
        lines.append(f"inline constexpr int k{i.name}Id = {n};")
    lines.append(f"inline constexpr int kInterfaceCount = {len(ifaces)};")
    lines.append("")
    lines.append("inline constexpr const char* kInterfaceNames[] = {")
    for i in ifaces:
        lines.append(f'    "{i.name}",')
    lines.append("};")
    lines.append("")
    for i in ifaces:
        lines.append(f"inline constexpr const char* k{i.name}Slots[] = {{")
        for s in i.slots:
            lines.append(f'    "{s.name if not s.is_dtor else "~" + i.name[1:]}",')
        lines.append("};")
    lines.append("")
    lines.append("}  // namespace rvk::hal")
    lines.append("")
    return "\n".join(lines)


FAILING_PREFIXES = ("Create", "Get", "Lock", "Compile", "Assemble", "Read", "Preprocess", "Load", "Save", "Copy", "Convert",
                    "Compute", "Generate")


def default_return(s: Slot) -> str | None:
    """The value a not-yet-implemented slot returns (None for void)."""
    r = s.ret
    if r == "void":
        return None
    if r == "bEResult":
        return "bEResult_False" if s.name.startswith(FAILING_PREFIXES) else "bEResult_Ok"
    if r == "bool":
        return "false"
    if r.endswith("&"):
        base = r[:-1].strip()
        return f"*static_cast<{base}*>(stubStorage())"
    if r.endswith("*"):
        return "nullptr"
    return "0"


def gen_stub(ifaces: list[Interface]) -> str:
    lines = [HEADER, "#pragma once", "", '#include "hal/api_generated.h"', '#include "hal/slots_generated.h"',
             '#include "hal/stub_support.h"', "", "namespace rvk::hal {", "",
             "// Every slot: log once that it is not implemented, null object out-parameters, return a neutral value",
             "// (getters / creators / locks fail, setters succeed). Implementations derive from these and override.", ""]
    for i in ifaces:
        sn = "Stub" + i.name[len("eCAPI"):]
        lines.append(f"class {sn} : public {i.name} {{")
        lines.append("public:")
        for s in i.slots:
            if s.is_dtor:
                continue
            c = " const" if s.const else ""
            lines.append(f"    {s.ret} {s.name}({decl_params(s)}){c} override {{")
            lines.append(f"        noteUnimplemented(k{i.name}Id, {s.index});")
            for k, p in enumerate(s.params):
                if p.ctype.endswith("**"):
                    lines.append(f"        if (a{k} != nullptr) *a{k} = nullptr;")
                elif p.ctype.endswith("*&"):
                    lines.append(f"        a{k} = nullptr;")
            unused = [f"a{k}" for k, p in enumerate(s.params) if not p.ctype.endswith(("**", "*&")) and "(*a" not in p.ctype]
            fnptr = [re.search(r"\(\*(a\d+)\)", p.ctype).group(1) for p in s.params if "(*a" in p.ctype]
            for u in unused + fnptr:
                lines.append(f"        (void){u};")
            r = default_return(s)
            if r is not None:
                lines.append(f"        return {r};")
            lines.append("    }")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace rvk::hal")
    lines.append("")
    return "\n".join(lines)


def gen_trace(ifaces: list[Interface]) -> str:
    iface_names = {i.name for i in ifaces}
    lines = [HEADER, "#pragma once", "", '#include "hal/api_generated.h"', '#include "hal/slots_generated.h"',
             '#include "hal/trace_support.h"', "", "namespace rvk::hal::trace {", ""]
    lines.append("// Creates (or finds) the wrapper of an engine object (defined in hal/trace.cpp).")
    for i in ifaces:
        lines.append(f"{i.name}* wrap({i.name}* inner);")
    lines += ["",
              "template <class T>",
              "void wrapOut(T** pp) {",
              "    if (pp != nullptr && *pp != nullptr) {",
              "        *pp = wrap(*pp);",
              "    }",
              "}",
              "template <class T>",
              "void wrapRef(T*& p) {",
              "    if (p != nullptr) {",
              "        p = wrap(p);",
              "    }",
              "}",
              ""]
    lines.append("// Forwarding wrappers: count + log every call, unwrap object arguments, wrap objects handed back.")
    for i in ifaces:
        tn = "Trace" + i.name[len("eCAPI"):]
        lines.append(f"class {tn} : public {i.name} {{")
        lines.append("public:")
        lines.append(f"    explicit {tn}({i.name}* inner) : inner_(inner) {{ registerWrapper(this, inner); }}")
        lines.append(f"    ~{tn}() override {{ releaseInner(k{i.name}Id, this, inner_); }}")
        lines.append(f"    {i.name}* inner() const {{ return inner_; }}")
        lines.append("")
        for s in i.slots:
            if s.is_dtor:
                continue
            c = " const" if s.const else ""
            lines.append(f"    {s.ret} {s.name}({decl_params(s)}){c} override {{")
            args, post = [], []
            for k, p in enumerate(s.params):
                a = f"a{k}"
                if (t := is_iface_ptr(p.ctype, iface_names)) is not None:
                    args.append(f"unwrap({a})")
                elif (t := is_iface_out(p.ctype, iface_names)) is not None:
                    args.append(a)
                    post.append(f"wrapOut({a});" if p.ctype.endswith("**") else f"wrapRef({a});")
                else:
                    args.append(a)
            names = ", ".join(f"a{k}" for k in range(len(s.params)))
            lines.append(f"        const CallScope scope(k{i.name}Id, {s.index}{', ' if names else ''}{names});")
            call = f"inner_->{s.name}({', '.join(args)})"
            if s.ret == "void":
                lines.append(f"        {call};")
                lines += [f"        {p}" for p in post]
            elif post:
                lines.append(f"        decltype(auto) r = {call};")
                lines += [f"        {p}" for p in post]
                lines.append("        scope.result(r);")
                lines.append("        return r;")
            else:
                lines.append(f"        decltype(auto) r = {call};")
                lines.append("        scope.result(r);")
                lines.append("        return r;")
            lines.append("    }")
        lines.append("")
        lines.append("protected:")
        lines.append(f"    {i.name}* inner_;")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace rvk::hal::trace")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default=DEFAULT_ENGINE)
    ap.add_argument("--demangled", default=DEFAULT_DEMANGLED)
    ap.add_argument("--out", default=os.path.join(REPO, "src", "hal"))
    args = ap.parse_args()
    ifaces, types = build(args.engine, args.demangled)
    os.makedirs(args.out, exist_ok=True)
    outputs = (("api_generated.h", gen_api(ifaces, types)), ("slots_generated.h", gen_slots(ifaces)),
               ("trace_generated.h", gen_trace(ifaces)), ("stub_generated.h", gen_stub(ifaces)))
    for fname, text in outputs:
        with open(os.path.join(args.out, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    for i in ifaces:
        dt = next(s.index for s in i.slots if s.is_dtor)
        byval = [f"{s.name}#{k}" for s in i.slots for k, p in enumerate(s.params) if p.by_value_class]
        print(f"{i.name:16s} {len(i.slots):3d} slots, destructor at {dt}" + (f", by-value classes: {byval}" if byval else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
