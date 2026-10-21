#pragma once

// Engine.dll functions and data we bind by decorated export name (docs/research/engine-gfx.md; RVAs are for orientation).
// X(field, module, decorated name, required)

#define RVK_ENGINE_SYMBOLS(X)                                                                                                   \
    /* void eCGfxAdmin::InitializeAdmin(eCGfxShared::eEAPIType) (0x74e510): creates eCDX9Instance, Direct3DCreate9 */           \
    X(gfxAdminInitializeAdmin, L"Engine.dll", "?InitializeAdmin@eCGfxAdmin@@IEAAXW4eEAPIType@eCGfxShared@@@Z", true)            \
    /* static bEResult eCGfxAdmin::sEnumAdapterDesc(unsigned long, eSGfxAdapterDesc&) (0x74eb80): the pixel shader 3.0 check  \
       of eCApplication::CreateEngine, before the window; Direct3DCreate9 + DxDiag while ms_pAPIInstance is null */             \
    X(gfxAdminEnumAdapterDesc, L"Engine.dll",                                                                                   \
      "?sEnumAdapterDesc@eCGfxAdmin@@SA?AW4bEResult@@KAEAUeSGfxAdapterDesc@eCGfxShared@@@Z", false)                             \
    /* eCAPIInstance* eCGfxAdmin::ms_pAPIInstance (0xc62ad8) */                                                                 \
    X(gfxAdminApiInstance, L"Engine.dll", "?ms_pAPIInstance@eCGfxAdmin@@1PEAVeCAPIInstance@@EA", true)                          \
    /* eCAPIDevice* eCGfxContextAdmin::ms_pAPIDevice (0xc62750): the single global device pointer */                           \
    X(gfxContextAdminApiDevice, L"Engine.dll", "?ms_pAPIDevice@eCGfxContextAdmin@@1PEAVeCAPIDevice@@EA", true)

namespace rvk::engine {

struct Symbols {
#define RVK_SYMBOL_FIELD(field, module, name, required) void* field = nullptr;
    RVK_ENGINE_SYMBOLS(RVK_SYMBOL_FIELD)
#undef RVK_SYMBOL_FIELD
};

// Resolves every symbol; returns false (and logs) when a required one is missing.
bool resolveSymbols();
const Symbols& sym();

}  // namespace rvk::engine
