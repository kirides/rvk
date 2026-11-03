// d3d9::Device scope chunks: the draw packets the opaque passes produce, collected into rhi::DrawChunks and handed to
// the backend (GameDevice::scopeChunk), which records them on its record threads (vk::GameRecord, docs/research/performance.md 12 + 14).

#include <algorithm>
#include <cstring>

#include "d3d9/device.h"

namespace rvk::d3d9 {

// Which scopes the record threads take: the main view's z pre-pass and solid pass, the sun cascades' depth pass (the passes
// whose draws are batched: docs/research/performance.md 12), when the device has the threads' prerequisites.
bool Device::wantSecondaryScope() const {
    // with fewer than four hardware threads the workers would compete with the engine thread and the driver
    static const bool enoughCores = std::thread::hardware_concurrency() >= 4;
    if (!recordThreadsWanted_ || !enoughCores) return false;
    const scene::Phase phase = phases_.current();
    return (phase.view == scene::View::Main && (phase.pass == scene::Pass::DepthPrepass || phase.pass == scene::Pass::Solid)) ||
           (phase.view == scene::View::Shadow && phase.pass == scene::Pass::ShadowDepth);
}

// the scope's open chunk: built by the D3D9 model, moved to the backend at closeChunk
DrawChunk& Device::currentChunk() {
    if (!chunksOpen_) {
        workChunk_.packets.clear();
        workChunk_.clears.clear();
        workChunk_.colorCount = colorCount_;
        std::copy(colorFormats_, colorFormats_ + kRenderTargets, workChunk_.colorFormats);
        workChunk_.depthFormat = depthFormat_;
        workChunk_.stencilFormat = stencilFormat_;
        chunksOpen_ = true;
    }
    return workChunk_;
}

void Device::emitPacket(const DrawPacket& p) {
    DrawChunk& c = currentChunk();
    c.packets.push_back(p);
    if (c.packets.size() >= kChunkPackets) closeChunk();
}

void Device::emitClear(const ClearOp& op) {
    DrawChunk& c = currentChunk();
    DrawPacket p;
    p.op = DrawPacket::Op::Clear;
    p.first = static_cast<std::uint32_t>(c.clears.size());
    c.clears.push_back(op);
    c.packets.push_back(p);
}

// hand the open chunk to the backend: a record worker starts on it at once (when the scope began secondary)
void Device::closeChunk() {
    if (!chunksOpen_) return;
    chunksOpen_ = false;
    game_->scopeChunk(std::move(workChunk_));
}

void Device::scopeLabel(bool begin, const char* text) {
    closeChunk();  // the labels stay in order with the chunks around them
    game_->scopeLabel(begin, text);
}

void Device::finishSecondaryScope() {
    closeChunk();  // the backend waits for the scope's chunks and executes them in order at its endScope
}

}  // namespace rvk::d3d9
