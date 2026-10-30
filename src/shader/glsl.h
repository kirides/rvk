#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "shader/translate.h"

namespace rvk::shader {

struct GlslResult {
    std::string glsl;
    Reflection reflection;
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

GlslResult toGlsl(const Program& program, const TranslateOptions& options = {});

}  // namespace rvk::shader
