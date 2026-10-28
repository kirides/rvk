#include "vk/debug_names.h"

#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>

#include "vk/context.h"
#include "vk/image.h"

namespace rvk::vk::names {
namespace {

std::atomic<bool> g_enabled{true};

// shader module handle -> its name (pipelines are named after their shaders). Handles of destroyed modules may be reused: the
// next name of a handle replaces the old one, and every module is named when names are on.
std::mutex g_moduleMutex;
std::unordered_map<std::uint64_t, std::string> g_modules;

std::string moduleName(VkShaderModule m) {
    const std::lock_guard lock(g_moduleMutex);
    const auto it = g_modules.find(reinterpret_cast<std::uint64_t>(m));
    return it != g_modules.end() ? it->second : std::string("?");
}

}  // namespace

void setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void object(const Context& ctx, VkObjectType type, std::uint64_t handle, std::string_view name) {
    if (handle == 0 || !ctx.namesEnabled()) return;
    const std::string s(name);  // null-terminated
    ctx.setName(type, handle, s.c_str());
}

void image(const Context& ctx, const Image& img, std::string_view name) {
    if (!img.valid() || !ctx.namesEnabled()) return;
    const std::string s(name);
    ctx.setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(img.image()), s.c_str());
    if (img.view() != VK_NULL_HANDLE) {
        const std::string v = s + " (view)";
        ctx.setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(img.view()), v.c_str());
    }
}

void shaderModule(const Context& ctx, VkShaderModule module, std::string_view name) {
    if (module == VK_NULL_HANDLE || !ctx.namesEnabled()) return;
    {
        const std::lock_guard lock(g_moduleMutex);
        g_modules[reinterpret_cast<std::uint64_t>(module)] = std::string(name);
    }
    object(ctx, VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<std::uint64_t>(module), name);
}

void pipeline(const Context& ctx, VkPipeline p, VkShaderModule vs, VkShaderModule fs) {
    if (p == VK_NULL_HANDLE || !ctx.namesEnabled()) return;
    object(ctx, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(p), "pipe " + moduleName(vs) + " + " + moduleName(fs));
}

std::string textureName(const TextureInfo& t) {
    std::string s;
    if (t.label != nullptr) {
        s = t.label;
        if (t.labelIndex != ~0u) s += std::format(" #{}", t.labelIndex);
        s += " | ";
    }
    s += t.renderTarget || t.depthFormat ? "rt " : "tex ";
    s += std::format("{} {}x{}", t.id, t.width, t.height);
    if (t.volume) s += std::format("x{}", t.depth);
    s += ' ';
    s += t.format;
    if (t.cube) s += " cube";
    if (t.volume) s += " vol";
    if (t.levels > 1) s += std::format(" L{}", t.levels);
    if (t.dynamic) s += " dynamic";
    if (t.slot != 0) s += std::format(" [slot {}]", t.slot);
    return s;
}

void texture(const Context& ctx, const Image& img, const TextureInfo& info) {
    if (!img.valid() || !ctx.namesEnabled()) return;
    image(ctx, img, textureName(info));
}

}  // namespace rvk::vk::names
