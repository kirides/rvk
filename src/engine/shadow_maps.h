#pragma once

// The engine's sun cascades (eCCascadeShadowMap, docs/research/shadows.md 3): player overrides of the cascade map size, the
// shadow range (MaxDistance) and the split factor (LogSplitFactor), applied in a pre-hook of RenderShadowMaps, which fits and
// renders every cascade each frame. The game's own values (its Shadow Quality: High = 1024, 75 m, 0.4) are tracked, so
// "game" restores them and a changed quality option is followed. Vulkan mode only (installed with the scene hooks).

namespace rvk::engine::shadowmaps {

struct Overrides {
    int mapSize = 0;            // texels per side, 0 = the game's
    float range = 0.0f;         // world units (cm), 0 = the game's
    float splitFactor = -1.0f;  // 0 (logarithmic) .. 1 (uniform) splits, < 0 = the game's
};

// any thread; applies at the next RenderShadowMaps
void setOverrides(const Overrides& o);

// Creates the detour (enabled with the others by hooks::enableAll); false when the symbols are missing
bool install();

// The split distances of the engine's formula (eCCascadeShadowMap::RenderShadowMaps, shadows.md 3.1): split i of n starts
// at (lin - log) * factor + log with lin = (range - 1) * i / n + 1, log = range ^ (i / n). Pure (unit-tested).
float splitStart(float range, float factor, int i, int n);

}  // namespace rvk::engine::shadowmaps
