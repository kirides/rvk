#pragma once

// Debug names of render targets that no role tag reveals (vk/debug_names.h): the targets the engine binds while the screen-space
// shadow mask is built (eCIlluminationAdmin::UpdateShadowMask: the 1/4 depth copy, the SSAO and blur buffers, the mask, its
// blurred copy). Called from Device::setRenderTarget in that pass only; render thread.

#include <cstdint>

namespace rvk::d3d9 {

class Device;
class Surface;

namespace debugnames {

// A colour target (index 0) was bound inside the shadow mask pass. Sorts the frame's targets by format, size and bind order:
// R32F = "shadow mask: 1/4 depth", smaller A8R8G8B8 = "shadow mask: SSAO buffer #k", scene-sized A8R8G8B8 = "shadow mask" (the
// last one bound is the final mask: the one before is renamed "shadow mask: before blur").
void onShadowMaskTarget(Device& device, Surface* surface);

}  // namespace debugnames
}  // namespace rvk::d3d9
