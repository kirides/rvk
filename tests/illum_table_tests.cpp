// The illumination table's pure composition (src/gpuscene/illum_compose.h, docs/research/frame-ownership.md F0): the lighting registers c14 .. c37 rebuilt from
// an illumination record + a light set record equal what StaticOps::writeLighting writes from the engine objects (the reference below is that function's
// reads written out on raw byte images, independent of the record structs).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpuscene/illum_compose.h"
#include "test.h"

using namespace rvk;
using namespace rvk::gpuscene;

namespace {

struct Engine {
    std::uint8_t illum[0x400]{};  // eSShaderMaterialIllumination (+ slack: the engine object is bigger than the record's raw bytes)
    std::uint8_t list[0x400]{};   // light list = light set + 0x10
};

void putF(std::uint8_t* b, std::uint32_t off, float v) { std::memcpy(b + off, &v, 4); }

Engine makeEngine(std::uint32_t seed, int nStatic, int nDynamic, bool hasStatic, bool noise) {
    Engine e;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f * 4.0f - 1.0f;
    };
    // everything random first (the engine reads unused entries too), then the structured fields
    if (noise) {
        for (std::uint32_t o = 8; o < 0x110; o += 4) putF(e.illum, o, rnd());
        for (std::uint32_t o = 4; o < 0x140; o += 4) putF(e.list, o, rnd());
    }
    putF(e.illum, 8, 0.25f + 0.5f * (rnd() + 1.0f) / 5.0f);
    e.illum[0x0c] = 0;
    e.illum[0x0d] = static_cast<std::uint8_t>(nStatic);
    e.illum[0x90] = hasStatic ? 1 : 0;
    e.list[0x34] = static_cast<std::uint8_t>(nDynamic);
    return e;
}

// StaticOps::writeLighting's reads, on raw images (L[r - 14])
void reference(const Engine& e, const ComposeParams& p, float L[24][4]) {
    auto rdF = [](const std::uint8_t* b, std::uint32_t off, void* out, std::size_t n) { std::memcpy(out, b + off, n); };
    rdF(e.list, 0x128, L[0], 12);
    float sh[7][4], staticSh[7][4]{}, indoor[3];
    rdF(e.list, 0xb8, sh, sizeof(sh));
    rdF(e.list, 0x128, indoor, 12);
    const bool hasStatic = e.illum[0x90] != 0;
    if (hasStatic) rdF(e.illum, 0x94, staticSh, sizeof(staticSh));
    float f;
    std::memcpy(&f, e.illum + 8, 4);
    native::sc::ambientSh(sh, indoor, f, hasStatic ? &staticSh[0][0] : nullptr, &L[1]);
    rdF(e.list, 4, L[8], 48);
    if (p.holdsAll) {
        std::memset(L[11], 0, 3 * 4 * 16);
        return;
    }
    const std::uint32_t nStatic = e.illum[0x0d];
    const std::uint32_t have = p.tableLit ? std::min<std::uint32_t>(3u, nStatic + e.list[0x34]) : p.n;
    if (p.tableLit) std::memset(L[11], 0, 3 * 4 * 16);
    for (std::uint32_t j = 0; j < std::min(p.n, have); ++j) {
        const std::uint32_t idx = p.firstLight + j;
        const std::uint8_t* src = idx < nStatic ? e.illum + 0x10 + 0x40 * idx : e.list + 0x38 + 0x40 * (idx - nStatic);
        if (11 + 4 * j + 4 <= 24) rdF(src, 0, L[11 + 4 * j], 64);
    }
}

void toRecords(const Engine& e, IllumRecord& ir, LightSetRecord& sr) {
    std::memset(&ir, 0, sizeof(ir));
    std::memset(&sr, 0, sizeof(sr));
    ir.info[0] = 1;
    ir.info[1] = 0;
    std::memcpy(ir.raw, e.illum, kIllumRawBytes);
    std::memset(ir.raw, 0, 8);  // the list pointer is the set slot
    sr.info[0] = 1;
    std::memcpy(sr.raw, e.list, kLightSetRawBytes);
}

}  // namespace

TEST(illum_table_record_sizes_match_the_shader_strides) {
    CHECK(sizeof(IllumRecord) == 288);
    CHECK(sizeof(LightSetRecord) == 336);
    CHECK(offsetof(IllumRecord, raw) == 16);
    CHECK(offsetof(LightSetRecord, raw) == 16);
}

TEST(illum_table_compose_equals_the_engine_composition) {
    int cases = 0, bad = 0;
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        for (int nStatic = 0; nStatic <= 2; ++nStatic) {
            for (int nDyn = 0; nDyn <= 2; ++nDyn) {
                for (int hasStatic = 0; hasStatic <= 1; ++hasStatic) {
                    const Engine e = makeEngine(seed * 97 + static_cast<std::uint32_t>(nStatic * 7 + nDyn), nStatic, nDyn, hasStatic != 0, true);
                    IllumRecord ir;
                    LightSetRecord sr;
                    toRecords(e, ir, sr);
                    for (int tableLit = 0; tableLit <= 1; ++tableLit) {
                        for (int holdsAll = 0; holdsAll <= (tableLit ? 1 : 0); ++holdsAll) {
                            for (std::uint32_t n = 0; n <= 3; ++n) {
                                for (std::uint32_t first = 0; first <= 3; ++first) {
                                    ComposeParams p;
                                    p.tableLit = tableLit != 0;
                                    p.holdsAll = holdsAll != 0;
                                    p.n = p.tableLit ? 3u : n;
                                    p.firstLight = first;
                                    // the register file before the call: the old content survives where the function does not write (c14.w, c37)
                                    float prior[24][4];
                                    for (int r = 0; r < 24; ++r) {
                                        for (int c = 0; c < 4; ++c) prior[r][c] = static_cast<float>(r * 4 + c) * 0.5f - 7.0f;
                                    }
                                    float want[24][4], got[24][4];
                                    std::memcpy(want, prior, sizeof(want));
                                    std::memcpy(got, prior, sizeof(got));
                                    reference(e, p, want);
                                    const bool ok = composeLighting(ir, sr, p, got);
                                    // only light indices inside the raw bytes are supported: with <= 2 dynamic + 2 static + first <= 3 all of these are
                                    if (!ok) continue;
                                    ++cases;
                                    if (std::memcmp(want, got, sizeof(want)) != 0) ++bad;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(cases > 1000);
    CHECK(bad == 0);
}

TEST(illum_table_compose_leaves_c14_w_and_the_unused_registers_alone) {
    const Engine e = makeEngine(5, 1, 1, true, true);
    IllumRecord ir;
    LightSetRecord sr;
    toRecords(e, ir, sr);
    float L[24][4];
    for (auto& r : L) {
        for (float& v : r) v = -123.0f;
    }
    ComposeParams p;
    p.n = 1;
    CHECK(composeLighting(ir, sr, p, L));
    CHECK(L[0][3] == -123.0f);                 // c14.w: the engine copies 12 bytes of the indoor colour
    CHECK(L[11 + 4][0] == -123.0f);            // the second light's registers: one light only
    CHECK(L[23][0] == -123.0f);                // c37
    CHECK(L[11][0] != -123.0f);                // the first light was written
}

TEST(illum_table_compose_reads_beyond_the_records_are_reported) {
    const Engine e = makeEngine(9, 2, 1, false, true);
    IllumRecord ir;
    LightSetRecord sr;
    toRecords(e, ir, sr);
    float L[24][4]{};
    ComposeParams p;
    p.n = 3;
    p.firstLight = 6;  // a dynamic light index 5 of the set: 0x38 + 0x40 * 4 + 64 > the raw 0x140 bytes
    CHECK(!composeLighting(ir, sr, p, L));
    p.firstLight = 0;
    CHECK(composeLighting(ir, sr, p, L));
}

TEST(illum_table_f_changes_the_sh_and_the_indoor_blend) {
    Engine e = makeEngine(3, 0, 0, false, true);
    IllumRecord ir;
    LightSetRecord sr;
    toRecords(e, ir, sr);
    float a[24][4]{}, b[24][4]{};
    ComposeParams p;
    CHECK(composeLighting(ir, sr, p, a));
    putF(ir.raw, 8, 0.1f);  // an f change = a patched word of the record
    CHECK(composeLighting(ir, sr, p, b));
    CHECK(std::memcmp(a[1], b[1], 16) != 0);
    CHECK(std::memcmp(a[8], b[8], 48) == 0);  // the sun does not depend on f
}
