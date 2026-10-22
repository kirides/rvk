// Fixed-function emulation for the engine's debug drawing (d3d9/ffshader.h).

#include "d3d9/ffshader.h"

#include <bit>

namespace rvk::d3d9::ff {
namespace {

// SM 3.0 tokens (D3D9 shader bytecode)
enum : std::uint32_t { kTemp = 0, kInput = 1, kConst = 2, kOutput = 6, kColorOut = 8 };
enum : std::uint32_t { kMov = 1, kMad = 4, kDp4 = 9, kDcl = 0x1F, kDef = 0x51 };
enum : std::uint32_t { kUsagePosition = 0, kUsageColor = 10 };

std::uint32_t reg(std::uint32_t type, std::uint32_t index) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | (index & 0x7FFu);
}
std::uint32_t dst(std::uint32_t type, std::uint32_t index, std::uint32_t mask = 0xF) { return reg(type, index) | (mask << 16); }
std::uint32_t src(std::uint32_t type, std::uint32_t index) { return reg(type, index) | (0xE4u << 16); }  // .xyzw
std::uint32_t ins(std::uint32_t op, std::uint32_t operands) { return op | (operands << 24); }

void dcl(std::vector<std::uint32_t>& t, std::uint32_t usage, std::uint32_t type, std::uint32_t index) {
    t.insert(t.end(), {ins(kDcl, 2), 0x80000000u | usage, dst(type, index)});
}

// D3DTA_* (low 4 bits; the modifiers are ignored) as vertexColour * k + c
struct Arg {
    float k[4];
    float c[4];
};

Arg argOf(std::uint32_t a, const float tf[4]) {
    switch (a & 0xF) {
    case 0:  // DIFFUSE
    case 1:  // CURRENT (stage 0: the diffuse colour)
        return {{1, 1, 1, 1}, {0, 0, 0, 0}};
    case 3:  // TFACTOR
        return {{0, 0, 0, 0}, {tf[0], tf[1], tf[2], tf[3]}};
    default:  // TEXTURE (white), SPECULAR, TEMP, CONSTANT
        return {{0, 0, 0, 0}, {1, 1, 1, 1}};
    }
}

// D3DTOP_* on components [first, last)
void apply(std::uint32_t op, std::uint32_t a1, std::uint32_t a2, const float tf[4], int first, int last, Linear& out) {
    const Arg x = argOf(a1, tf), y = argOf(a2, tf);
    for (int i = first; i < last; ++i) {
        float k = 0, c = 0;
        switch (op) {
        case 1:  // DISABLE: the diffuse colour passes through
            k = 1;
            break;
        case 2:  // SELECTARG1
            k = x.k[i], c = x.c[i];
            break;
        case 3:  // SELECTARG2
            k = y.k[i], c = y.c[i];
            break;
        case 7:  // ADD
            k = x.k[i] + y.k[i], c = x.c[i] + y.c[i];
            break;
        default: {  // MODULATE (2X, 4X); anything else approximated as MODULATE
            const float scale = op == 5 ? 2.0f : op == 6 ? 4.0f : 1.0f;
            if (x.k[i] != 0 && y.k[i] != 0) {
                k = 1;  // diffuse * diffuse: not linear, keep the diffuse colour
            } else {
                k = (x.k[i] * y.c[i] + y.k[i] * x.c[i]) * scale;
                c = x.c[i] * y.c[i] * scale;
            }
        }
        }
        out.mul[static_cast<std::size_t>(i)] = k;
        out.add[static_cast<std::size_t>(i)] = c;
    }
}

}  // namespace

bool supportedFvf(std::uint32_t fvf) { return (fvf & kFvfPositionMask) == kFvfXyz; }

std::vector<std::uint32_t> vertexShader(bool diffuse) {
    std::vector<std::uint32_t> t{0xFFFE0300u};
    dcl(t, kUsagePosition, kInput, 0);
    if (diffuse) dcl(t, kUsageColor, kInput, 1);
    dcl(t, kUsagePosition, kOutput, 0);
    dcl(t, kUsageColor, kOutput, 1);
    constexpr std::uint32_t kWhite = kVsMatrix - 1;
    if (!diffuse) {
        const std::uint32_t one = std::bit_cast<std::uint32_t>(1.0f);
        t.insert(t.end(), {ins(kDef, 5), dst(kConst, kWhite), one, one, one, one});
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        t.insert(t.end(), {ins(kDp4, 3), dst(kOutput, 0, 1u << i), src(kInput, 0), src(kConst, kVsMatrix + i)});
    }
    t.insert(t.end(), {ins(kMov, 2), dst(kOutput, 1), diffuse ? src(kInput, 1) : src(kConst, kWhite)});
    t.push_back(0x0000FFFFu);
    return t;
}

std::vector<std::uint32_t> pixelShader() {
    std::vector<std::uint32_t> t{0xFFFF0300u};
    dcl(t, kUsageColor, kInput, 0);
    t.insert(t.end(), {ins(kMad, 4), dst(kColorOut, 0), src(kInput, 0), src(kConst, kPsMul), src(kConst, kPsAdd)});
    t.push_back(0x0000FFFFu);
    return t;
}

Linear reduce(const Stage0& s, std::uint32_t textureFactor) {
    const float tf[4] = {static_cast<float>((textureFactor >> 16) & 0xFF) / 255.0f, static_cast<float>((textureFactor >> 8) & 0xFF) / 255.0f,
                         static_cast<float>(textureFactor & 0xFF) / 255.0f, static_cast<float>(textureFactor >> 24) / 255.0f};
    Linear out;
    if (s.colorOp == 1) {
        // stage 0 disabled: the whole pipeline passes the diffuse colour (alpha included)
        out.mul = {1, 1, 1, 1};
        return out;
    }
    apply(s.colorOp, s.colorArg1, s.colorArg2, tf, 0, 3, out);
    apply(s.alphaOp, s.alphaArg1, s.alphaArg2, tf, 3, 4, out);
    return out;
}

std::array<float, 16> transposedProduct(const float* w, const float* v, const float* p) {
    const auto mul = [](const float* a, const float* b) {
        std::array<float, 16> r{};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
                r[static_cast<std::size_t>(i * 4 + j)] = s;
            }
        }
        return r;
    };
    const std::array<float, 16> wv = mul(w, v);
    const std::array<float, 16> wvp = mul(wv.data(), p);
    std::array<float, 16> t{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) t[static_cast<std::size_t>(i * 4 + j)] = wvp[static_cast<std::size_t>(j * 4 + i)];
    }
    return t;
}

}  // namespace rvk::d3d9::ff
