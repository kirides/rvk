#pragma once

// Debug tooling (docs/research/shader-isa.md): with the environment variable RVK_ISA_DUMP=<dir> every pipeline the renderer builds is created with
// VK_PIPELINE_CREATE_CAPTURE_STATISTICS / CAPTURE_INTERNAL_REPRESENTATIONS (VK_KHR_pipeline_executable_properties) and its statistics (VGPRs, SGPRs,
// LDS, scratch, instruction counts) plus the driver's internal representations (the ISA, when the driver offers it) are written below <dir>.
// Off (variable unset) = nothing exists: no flags, no extension request, no files. With it set the on-disk pipeline cache is not used for the
// monolithic builds (a cached pipeline carries no executables of its own) and RVK_GPL_FORCE must not be set (a fast link has no executables).

#include <string>
#include <string_view>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

namespace isa {

// RVK_ISA_DUMP is set (read once)
bool enabled();
// the directory of the dump
const std::string& directory();
// creation flags of a pipeline that is to be dumped (0 when off or the device lacks the extension)
VkPipelineCreateFlags flags(const Context& ctx);
// writes <dir>/<seq>_<label>.txt (statistics + the readable internal representations) and appends a line per executable to <dir>/stats.csv; any thread
void dump(const Context& ctx, VkPipeline pipeline, std::string_view label);

}  // namespace isa
}  // namespace rvk::vk
