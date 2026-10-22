#pragma once

// A CPU interpreter of decoded vs_3_0 / ps_3_0 programs (shader/sm_ir.h) and a backward slice over them. Used by the ray
// tracing scene (enh/rt_material, docs/research/rt-reflections.md 3.2) to learn, per draw, how the pixel shader's diffuse
// coordinate follows from the vertex uv: the draw's own vertex shader and pixel shader are evaluated on probe vertices with
// the draw's constants (the vertex shader's outputs feed the pixel shader's inputs by semantic), and the slice names the
// constants that coordinate depends on (the cache key). Pure logic (unit tests: tests/shader_eval_tests.cpp).
//
// Supported: every arithmetic instruction of SM 3.0, a0 / aL relative constant addressing, predication, if / ifc / else,
// rep / loop / break(c/p) with integer constants; texld / texldl / texldb / texldp / texldd call a sampling callback; dsx /
// dsy are 0; call / callnz / labels are not supported (run() fails). Values are IEEE floats like the GPU's (no D3D9
// special cases beyond the translator's: rcp(0) = inf, rsq / log of |x|, pow of |x|).

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shader/sm_ir.h"

namespace rvk::shader::eval {

using Vec4 = std::array<float, 4>;

struct Env {
    const float (*floats)[4] = nullptr;       // c0.. (as the device has them; the program's def constants override them)
    std::uint32_t floatCount = 0;
    const std::int32_t (*ints)[4] = nullptr;  // i0..i15
    const std::uint32_t* bools = nullptr;     // b0..b15
    std::array<Vec4, 16> inputs{};            // v# by register index
    Vec4 position{};                          // ps vPos (misc 0)
    float face = 1.0f;                        // ps vFace (misc 1)
    // texld* of sampler `sampler` at `coord` (instruction index `at`); unset = (0.5, 0.5, 0.5, 1)
    std::function<Vec4(std::uint32_t sampler, const Vec4& coord, std::size_t at)> sample;
    std::size_t maxSteps = 20000;             // executed instructions (loops) before giving up
};

struct Result {
    bool ok = false;
    std::string error;
    std::array<Vec4, 16> outputs{};  // vs: o#; ps: oC#
    float depth = 0.0f;              // ps oDepth
    bool killed = false;             // ps texkill fired
};

Result run(const Program& program, const Env& env);

// Backward slice: which inputs and constants the value of `target` depends on.
struct Slice {
    std::vector<std::uint16_t> floatConsts;  // c# read on the way (not counting def'd ones), sorted
    std::array<std::uint8_t, 16> inputs{};   // v# component masks
    bool relative = false;  // a constant was read with relative addressing (base registers listed, the range is not)
    bool flow = false;      // the program has flow control / predication: conservative (all constants read up to there)
    bool usesInts = false;  // integer / bool constants matter (flow control)
};

// Slice of source operand `src` (component mask from its swizzle and `components`) of instruction `at`
Slice sliceSource(const Program& program, std::size_t at, std::size_t src, std::uint8_t components);
// Slice of the vs_3_0 output registers in `outputs` (register index -> component mask)
Slice sliceOutputs(const Program& program, const std::array<std::uint8_t, 16>& outputs);

// The first texld* sampling sampler `sampler` (instruction index), or SIZE_MAX
std::size_t firstSample(const Program& program, std::uint32_t sampler);

}  // namespace rvk::shader::eval
