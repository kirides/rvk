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
    X(gfxContextAdminApiDevice, L"Engine.dll", "?ms_pAPIDevice@eCGfxContextAdmin@@1PEAVeCAPIDevice@@EA", true)              \
    /* snapshots (missing-slots.md 3, engine/vfs.h). eCVirtualFileSystem& eCVirtualFileSystem::GetInstance() (0x736450) */    \
    X(vfsGetInstance, L"Engine.dll", "?GetInstance@eCVirtualFileSystem@@SAAEAV1@XZ", false)                                     \
    /* bool eCVirtualFileSystem::MakeAbsPhysicalPath(bCString&) (0x733990) */                                                  \
    X(vfsMakeAbsPhysicalPath, L"Engine.dll", "?MakeAbsPhysicalPath@eCVirtualFileSystem@@QEAA_NAEAVbCString@@@Z", false)         \
    /* eCApplication& eCApplication::GetInstance() (0x4c64e0) */                                                               \
    X(appGetInstance, L"Engine.dll", "?GetInstance@eCApplication@@SAAEAV1@XZ", false)                                           \
    /* void eCApplication::Snapshot() (0x4bfc90): the debug snapshot key's action */                                           \
    X(appSnapshot, L"Engine.dll", "?Snapshot@eCApplication@@QEAAXXZ", false)                                                    \
    /* bCString const& eCApplication::GetLastSnapshotName() const (0x4c4040) */                                                \
    X(appGetLastSnapshotName, L"Engine.dll", "?GetLastSnapshotName@eCApplication@@QEBAAEBVbCString@@XZ", false)                 \
    /* virtual void eCApplication::Draw(HWND) (0x4c13b0): GUI, DoSnapshot, SwapScreen; hooked for [debug] snapshot_frames */  \
    X(appDraw, L"Engine.dll", "?Draw@eCApplication@@UEAAXPEAUHWND__@@@Z", false)                                                \
    /* SharedBase.dll bCString(char const*), ~bCString(), char const* GetText() const */                                       \
    X(stringCtor, L"SharedBase.dll", "??0bCString@@QEAA@PEBD@Z", false)                                                         \
    X(stringDtor, L"SharedBase.dll", "??1bCString@@QEAA@XZ", false)                                                             \
    X(stringGetText, L"SharedBase.dll", "?GetText@bCString@@QEBAPEBDXZ", false)

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
