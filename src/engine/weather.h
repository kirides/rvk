#pragma once

// The engine's current weather (docs/research/ocean.md 6): eCWeatherAdmin blends its weather states (eCWeatherStates, named
// like "Sunny_Full", "Sunny_Half_02", "Stormy_Rain", "Dungeon_*") with blend factors that follow the weather zones (mode 0)
// or a forced state (mode 2, risen-spass risen.world.set_weather). The FFT ocean derives its sea state from it.
// Render thread (the engine's main thread), after the world is loaded; every engine call SEH-guarded.

#include <optional>
#include <string>
#include <vector>

namespace rvk::engine {

struct WeatherState {
    std::string name;
    float weight = 0.0f;  // GetBlendFactor
};

// every weather state with its blend factor; empty when the symbols or the admin are missing
std::vector<WeatherState> weatherStates();

// the sea state level (0 calm, 1 moderate, 2 rough, 3 storm) of a blend of weather states: each state's level from its name
// (storm / thunder 3, rain 2, clouds 1.2, fog 0.5, sun 0.6, other 1), dungeon / cave states carry no weight; nullopt without
// weight. Pure (unit-tested).
std::optional<float> seaLevelOfWeather(const std::vector<WeatherState>& states);

}  // namespace rvk::engine
