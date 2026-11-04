#pragma once

#include <cstddef>
#include <cstdint>

// Typed replacement of the engine's statically linked CRT qsort (Engine.dll RVA 0x4a4aa0) for the render-op lists that
// eCFrustumDatabase::Execute / ExecuteShadowMap sort every view (24-byte eCRenderOp2 records; comparators z 0x66e670,
// solid 0x66e6a0, alpha 0x66e600; docs/research/performance.md 21). The algorithm is the CRT's own (median of three,
// insertion cut-off 8, the same partition and stack order), so the result is bit-identical to the engine's, equal keys
// included; only the comparator calls are inlined and the byte-wise swaps are three 64-bit moves.
namespace rvk::engine::fastsort {

// The comparator ids of the three sorted lists
enum class Cmp { Z, Solid, Alpha };

// Sorts `count` 24-byte records like the CRT qsort with the engine comparator `cmp` (pure; unit-tested)
void sortOps(void* base, std::size_t count, Cmp cmp);

// Installs the qsort detour (checks the function prologue and the comparators' bytes); false when the engine differs.
bool install();

// true once the detour is installed (the function's first bytes are then our jump: native/frame_tap.cpp skips its prologue check)
bool installed();

// any thread; 0 = off (the engine qsort), 1 = typed sort, 2 = typed sort + compare every result with the engine qsort (diagnostic)
void setMode(int mode);

}  // namespace rvk::engine::fastsort
