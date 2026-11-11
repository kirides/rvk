#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "native/own_post.h"
#include "shader/embedded.h"

// S-7d (docs/research/frame-ownership.md 14.18): the effect ports. Every vertex and pixel shader of the effects the native frame composes (post_SSM.fx's
// shadow mask / SSAO chain / PSSM pass, the terrain map's Blur7, post_hdr / post_dof / post_bloom / post_streaks / overlay_color and the rare image effects
// post_radialBlur / post_wobble / post_edgesmooth) has a port in shaders/post/fx (tools/post_port.py; proven EQUAL to the translated shaders by
// `post_port.py --check`, rvk_shadereq). A composed effect draw (native::EffectOps::drawComposed) is drawn with them while `native.own_post=on`; the port's
// d3d9::Shader carries the engine shader's reflection and semantics (the device uploads the same registers, binds the same stages; enh/shadows still
// recognises the cascade draws by their semantics).

namespace rvk::native {

namespace {

struct FxRow {
    std::uint64_t key;
    shader::Stage stage;
    const char* shader;
    const char* uses;
};
constexpr FxRow kFxTable[] = {
#include "native/post_port_table.inl"
};
constexpr std::size_t kFxRows = std::size(kFxTable);

std::uint64_t fnv1a64(const void* data, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

OwnPost* g_ownPost = nullptr;

constexpr const char* kFxKindNames[OwnPost::kFxKinds] = {"shadow mask", "SSAO chain", "PSSM", "terrain blur", "post", "other"};

}  // namespace

int ownPostMode() { return g_ownPost != nullptr ? static_cast<int>(g_ownPost->mode()) : 0; }
std::uint64_t ownPostActs() { return g_ownPost != nullptr ? g_ownPost->acts() : 0; }
OwnPost* ownPost() { return g_ownPost; }
void registerOwnPost(OwnPost* p, OwnPost* only) {
    if (only == nullptr || g_ownPost == only) g_ownPost = p;
}
int ownSceneMode(OwnPost::Scene which) { return g_ownPost != nullptr ? static_cast<int>(g_ownPost->sceneMode(which)) : 0; }
std::uint64_t ownSceneActs(OwnPost::Scene which) { return g_ownPost != nullptr ? g_ownPost->sceneActs(which) : 0; }

// ---- F12 / F13 (frame-ownership.md 14.24) ----

void OwnPost::setSceneMode(Scene which, Mode m) {
    Mode& cur = sceneMode_[static_cast<std::size_t>(which)];
    if (cur == m) return;
    cur = m;
    if (m != Mode::Off && which != Scene::Particles) buildFxPorts();  // (F11 uses no port)
    spdlog::info("native: own {}: {}", which == Scene::Sky ? "sky" : which == Scene::Gui ? "gui" : "particles", m == Mode::Off ? "off" : m == Mode::On ? "on" : "compare");
}

std::uint64_t OwnPost::drawSkips() { return dev_.stats().skippedCompiling + dev_.stats().skippedNoShader + dev_.game().pipelineSkips(); }

void OwnPost::countScene(Scene which, bool ours, const char* why) {
    SceneCounts& c = sceneW_[static_cast<std::size_t>(which)];
    ++c.draws;
    if (ours) {
        ++c.ours;
        ++sceneActs_[static_cast<std::size_t>(which)];
    } else {
        ++c.left;
        if (why != nullptr && c.why.empty()) c.why = why;
    }
}

void OwnPost::reportScene() {
    static constexpr const char* kNames[3] = {"sky", "gui", "particles"};
    static constexpr const char* kWhat[3] = {"sky dome draws (main view + sky cube faces)", "sprite batch draws (GUI, text, cursor, loading screens)",
                                             "particle draws (quads expanded on the GPU)"};
    for (std::size_t k = 0; k < 3; ++k) {
        SceneCounts& c = sceneW_[k];
        if (sceneMode_[k] != Mode::Off || c.draws != 0) {
            spdlog::info("native: own {} (5 s): {}: {} {}: ours {} / engine's shaders {}{}{}", kNames[k], sceneMode_[k] == Mode::Off ? "off" : sceneMode_[k] == Mode::On ? "on" : "compare",
                         c.draws, kWhat[k], c.ours, c.left, c.why.empty() ? std::string() : std::format(" (first reason: {})", c.why),
                         k != 0 ? std::format("; compares dropped (a draw skipped while its shader / pipeline built, run) {}", droppedCompares_) : std::string());
        }
        c = {};
    }
}

d3d9::Surface* OwnPost::scratchOf(d3d9::Surface* src, int tag, bool copy) {
    if (src == nullptr || src->texture() == nullptr) return nullptr;
    d3d9::Texture* t = src->texture();
    const std::uint32_t w = src->width(), h = src->height();
    const auto fmt = t->desc().format;
    std::unique_ptr<d3d9::Texture>* slot = nullptr;
    for (std::size_t i = 0; i < std::size(sceneScratch_); ++i) {
        auto& s = sceneScratch_[i];
        if (s && sceneScratchTag_[i] == tag && s->desc().width == w && s->desc().height == h && s->desc().format == fmt) slot = &s;
    }
    if (slot == nullptr) {
        const std::size_t i = sceneScratchNext_++ % std::size(sceneScratch_);
        slot = &sceneScratch_[i];
        if (*slot) dev_.release(std::move(*slot));
        d3d9::TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = fmt;
        desc.usage = d3d9::USAGE_RENDERTARGET;
        desc.pool = d3d9::POOL_DEFAULT;
        // (the same VkFormat as the source: with own_post = on a small A8R8G8B8 render target is made RGBA8 for storage (S-7b), the sky cube's faces are
        // BGRA8; a copy between the two keeps the bytes, not the channels)
        const bool storageWas = d3d9::Texture::smallStorage();
        for (const bool storage : {false, true}) {
            d3d9::Texture::setSmallStorage(storage);
            *slot = dev_.createTexture(desc);
            if (*slot && (*slot)->rhi().desc().format == t->rhi().desc().format) break;
            if (*slot) dev_.release(std::move(*slot));
        }
        d3d9::Texture::setSmallStorage(storageWas);
        sceneScratchTag_[i] = tag;
        if (!*slot) return nullptr;
    }
    d3d9::Texture* s = slot->get();
    if (copy) {
        bool ok = false;
        const std::uint32_t level = src->level(), face = src->face();
        dev_.runPassNow("own scene (compare copy)", [&](d3d9::PassContext& ctx) {
            rhi::CommandList& cmd = ctx.cmd;
            cmd.transition(t->rhi(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cmd.transition(s->rhi(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            rhi::TextureCopy region{};
            region.src = {level, face};
            region.extent = {w, h, 1};
            cmd.copyTexture(s->rhi(), t->rhi(), std::span<const rhi::TextureCopy>(&region, 1));
            ok = true;
        });
        if (!ok) return nullptr;
    }
    return s->surface(0, 0);
}

void OwnPost::buildFxPorts() {
    g_ownPost = this;
    if (!fxModules_.empty()) return;
    fxModules_ = std::vector<std::atomic<rhi::ShaderModule>>(kFxRows);
    fxState_ = std::vector<std::atomic<std::uint8_t>>(kFxRows);
    for (auto& s : fxState_) s.store(1, std::memory_order_relaxed);
    rhi::GameDevice* game = &dev_.game();
    constexpr std::size_t kWorkers = 4;
    for (std::size_t w = 0; w < kWorkers; ++w) {
        fxBuilding_.push_back(std::async(std::launch::async, [this, game, w]() {
            for (std::size_t i = w; i < kFxRows; i += kWorkers) {
                const char* name = kFxTable[i].shader;
                const rhi::ShaderModule m = game->createShaderModule({.spirv = shader::embedded::spirv(name), .dxil = shader::embedded::dxilOnDemand(name)}, name);
                fxModules_[i].store(m, std::memory_order_release);
                fxState_[i].store(m != rhi::ShaderModule::None ? 2 : 3, std::memory_order_release);
                if (m == rhi::ShaderModule::None) spdlog::error("native: own post: the effect port {} failed: its draws stay on the engine's shader", name);
            }
        }));
    }
}

const d3d9::Shader* OwnPost::fxPort(const void* enginePtr, const char** why) {
    const auto* engine = static_cast<const d3d9::Shader*>(enginePtr);
    if (engine == nullptr) return nullptr;
    if (auto it = fxOurs_.find(engine); it != fxOurs_.end()) return it->second.get();
    int row = -1;
    if (auto it = fxRow_.find(engine); it != fxRow_.end()) {
        row = it->second;
    } else {
        if (!engine->bytecode.empty()) {
            const std::uint64_t key = fnv1a64(engine->bytecode.data(), engine->bytecode.size() * 4);
            for (std::size_t i = 0; i < kFxRows; ++i) {
                if (kFxTable[i].key == key && kFxTable[i].stage == engine->stage) {
                    row = static_cast<int>(i);
                    break;
                }
            }
        }
        fxRow_.emplace(engine, row);
        if (row >= 0) spdlog::info("native: own post: engine shader {:016x} -> port {} ({})", engine->hash, kFxTable[row].shader, kFxTable[row].uses);
    }
    if (row < 0) {
        if (why != nullptr) *why = "no port of this shader";
        return nullptr;
    }
    const auto r = static_cast<std::size_t>(row);
    if (fxState_.empty() || fxState_[r].load(std::memory_order_acquire) != 2) {
        if (why != nullptr) *why = fxState_.empty() || fxState_[r].load(std::memory_order_acquire) != 3 ? "its port is not built yet" : "its port failed to build";
        return nullptr;
    }
    auto s = std::make_shared<d3d9::Shader>();
    s->stage = engine->stage;
    s->module.store(fxModules_[r].load(std::memory_order_acquire), std::memory_order_release);
    s->reflection = engine->reflection;
    s->semantics = engine->semantics;
    s->bytecode = engine->bytecode;  // (GetFunctionData; never translated: every variant below is "not applicable")
    s->samplerMask = engine->samplerMask;
    s->hash = engine->hash ^ (0x5c7d000000000000ull + (static_cast<std::uint64_t>(row) << 32));
    s->tintFailed = true;
    s->lodFailed = true;
    for (auto& v : s->variantState) v.store(3, std::memory_order_relaxed);
    fxOurs_.emplace(engine, s);
    return s.get();
}

const char* OwnPost::fxPortName(const void* engine) const {
    const auto it = fxRow_.find(engine);
    if (it == fxRow_.end() || it->second < 0) return nullptr;
    const char* n = kFxTable[static_cast<std::size_t>(it->second)].shader;
    return std::strncmp(n, "post/fx/", 8) == 0 ? n + 8 : n;
}

void OwnPost::countFx(int kind, bool vsOurs, bool psOurs, bool vsLeft, bool psLeft, const char* why) {
    FxCounts& c = fxW_[static_cast<std::size_t>(std::clamp(kind, 0, kFxKinds - 1))];
    ++c.draws;
    c.vsOurs += vsOurs ? 1 : 0;
    c.psOurs += psOurs ? 1 : 0;
    c.vsLeft += vsLeft ? 1 : 0;
    c.psLeft += psLeft ? 1 : 0;
    if ((vsLeft || psLeft) && why != nullptr && c.why.empty()) c.why = why;
    if (vsOurs || psOurs) ++acts_;
}

void OwnPost::reportFx() {
    std::string s;
    for (int k = 0; k < kFxKinds; ++k) {
        FxCounts& c = fxW_[static_cast<std::size_t>(k)];
        if (c.draws == 0) continue;
        s += std::format("{}{} {} draws: vs ours {} / engine {}, ps ours {} / engine {}{}", s.empty() ? "" : "; ", kFxKindNames[k], c.draws, c.vsOurs, c.vsLeft, c.psOurs, c.psLeft,
                         c.why.empty() ? std::string() : std::format(" (first engine reason: {})", c.why));
        c = {};
    }
    std::size_t ready = 0;
    for (const auto& st : fxState_) ready += st.load(std::memory_order_relaxed) == 2 ? 1 : 0;
    spdlog::info("native: own fx shaders (5 s): {}; ports ready {} of {}", s.empty() ? std::string("no composed effect draw") : s, ready, kFxRows);
}

void OwnPost::destroyFxPorts() {
    for (auto& f : fxBuilding_) {
        if (f.valid()) f.wait();
    }
    for (auto& m : fxModules_) {
        const rhi::ShaderModule v = m.load(std::memory_order_acquire);
        if (v != rhi::ShaderModule::None) dev_.game().destroyShaderModule(v, true);
    }
    if (g_ownPost == this) g_ownPost = nullptr;
}

}  // namespace rvk::native
