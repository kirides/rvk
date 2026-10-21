// FVF vertex size and declaration, against d3dx9_39's D3DXGetFVFVertexSize / D3DXDeclaratorFromFVF (skipped without it).

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d3d9/fvf.h"
#include "test.h"

using namespace rvk;
using namespace rvk::d3d9;

namespace {

struct D3DXFvf {
    using SizeFn = UINT(WINAPI*)(DWORD);
    using DeclFn = long(WINAPI*)(DWORD, VertexElement*);
    SizeFn size = nullptr;
    DeclFn decl = nullptr;

    static const D3DXFvf& get() {
        static const D3DXFvf d = [] {
            D3DXFvf x;
            if (HMODULE m = LoadLibraryW(L"d3dx9_39.dll")) {
                x.size = reinterpret_cast<SizeFn>(GetProcAddress(m, "D3DXGetFVFVertexSize"));
                x.decl = reinterpret_cast<DeclFn>(GetProcAddress(m, "D3DXDeclaratorFromFVF"));
            }
            return x;
        }();
        return d;
    }
};

std::string describe(const std::vector<VertexElement>& e) {
    std::string s;
    for (const auto& v : e) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "{%u,%u,t%u,m%u,u%u,%u} ", v.stream, v.offset, v.type, v.method, v.usage, v.usageIndex);
        s += buf;
    }
    return s;
}

std::vector<std::uint32_t> fvfCases() {
    std::vector<std::uint32_t> c = {0x242, 0x002, 0x004, 0x4002, 0x012, 0x112, 0x152, 0x1c4, 0x144, 0x2c2, 0x822, 0x0e2, 0x103,
                                    0x006, 0x008, 0x00a, 0x00c, 0x1006, 0x100c, 0x100e, 0x800e, 0x8006, 0x1018, 0x318};
    // every position type x the optional components x 0..8 texture sets with mixed sizes
    const std::uint32_t positions[] = {0, 0x002, 0x004, 0x006, 0x008, 0x00a, 0x00c, 0x00e, 0x4002};
    std::uint32_t mix = 0x9e3779b9;
    for (std::uint32_t pos : positions) {
        for (std::uint32_t parts = 0; parts < 16; ++parts) {
            for (std::uint32_t tex = 0; tex <= 8; ++tex) {
                mix = mix * 1664525u + 1013904223u;
                const std::uint32_t sizes = tex == 0 ? 0 : (mix & ((1u << (2 * tex)) - 1)) << 16;
                const std::uint32_t beta = pos >= 0x006 && pos <= 0x00e && (mix >> 30) != 0 ? ((mix >> 30) == 1 ? 0x1000u : 0x8000u) : 0;
                c.push_back(pos | (parts << 4) | (tex << 8) | sizes | beta);
            }
        }
    }
    return c;
}

}  // namespace

TEST(fvf_vertex_size_known) {
    CHECK(fvfVertexSize(0) == 0);
    CHECK(fvfVertexSize(0x242) == 32);  // XYZ | DIFFUSE | TEX2 (the engine's FVF buffers)
    CHECK(fvfVertexSize(0x112) == 32);  // XYZ | NORMAL | TEX1
    CHECK(fvfVertexSize(0x144) == 28);  // XYZRHW | DIFFUSE | TEX1
    CHECK(fvfVertexSize(0x100e) == 32);  // XYZB5 | LASTBETA_UBYTE4
    CHECK(fvfVertexSize(0x0002 | 0x100 | (3u << 16)) == 16);  // TEXCOORDSIZE1(0)
    const auto d = fvfDeclaration(0x242);
    CHECK(d.size() == 5);
    if (d.size() == 5) {
        CHECK(d[0].usage == DECLUSAGE_POSITION && d[0].type == DECLTYPE_FLOAT3 && d[0].offset == 0);
        CHECK(d[1].usage == DECLUSAGE_COLOR && d[1].type == DECLTYPE_D3DCOLOR && d[1].offset == 12);
        CHECK(d[2].usage == DECLUSAGE_TEXCOORD && d[2].usageIndex == 0 && d[2].offset == 16);
        CHECK(d[3].usage == DECLUSAGE_TEXCOORD && d[3].usageIndex == 1 && d[3].offset == 24);
        CHECK(d[4].stream == kDeclEndStream);
    }
}

TEST(fvf_against_d3dx) {
    const auto& dx = D3DXFvf::get();
    if (dx.size == nullptr || dx.decl == nullptr) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    int compared = 0;
    for (std::uint32_t fvf : fvfCases()) {
        VertexElement ref[65]{};
        const long hr = dx.decl(fvf, ref);
        const auto ours = fvfDeclaration(fvf);
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%x", fvf);
        if (hr < 0) {
            // d3dx9_39 predates D3DFVF_XYZW (the D3D9 runtime accepts it in SetFVF / CreateVertexBuffer): keep ours
            if ((fvf & FVF_POSITION_MASK) == FVF_XYZW) {
                CHECK_MSG(!ours.empty() && ours[0].type == DECLTYPE_FLOAT4 && ours[0].usage == DECLUSAGE_POSITION, hex);
                continue;
            }
            CHECK_MSG(ours.empty(), std::string("fvf ") + hex + ": D3DX rejects it, ours: " + describe(ours));
            continue;
        }
        std::vector<VertexElement> want;
        for (const auto& e : ref) {
            want.push_back(e);
            if (e.stream == kDeclEndStream) break;
        }
        const bool same = want.size() == ours.size() &&
                          std::memcmp(want.data(), ours.data(), want.size() * sizeof(VertexElement)) == 0;
        CHECK_MSG(same, std::string("fvf ") + hex + "\n  d3dx: " + describe(want) + "\n  ours: " + describe(ours));
        CHECK_MSG(dx.size(fvf) == fvfVertexSize(fvf), std::string("fvf ") + hex + ": size d3dx " + std::to_string(dx.size(fvf)) +
                                                          " ours " + std::to_string(fvfVertexSize(fvf)));
        ++compared;
    }
    CHECK(compared > 1000);
}
