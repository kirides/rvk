#include "engine/weather.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <windows.h>

#include "engine/symbols.h"

namespace rvk::engine {
namespace {

struct alignas(16) EngineString {
    unsigned char storage[32]{};
};

using StringCtorFn = void* (*)(EngineString*, const char*);
using StringDtorFn = void (*)(EngineString*);
using StringGetTextFn = const char* (*)(const void*);
using ModuleAdminFn = void* (*)();
using FindModuleFn = void* (*)(const void* admin, const EngineString* className);
using StatesCountFn = int (*)(const void* admin);
using StatesAtFn = void* (*)(const void* admin, int index);
using StateNameFn = const void* (*)(const void* state);
using StateBlendFn = float (*)(const void* state);
using PlayerPositionFn = const float* (*)(const void* admin);

struct RawState {
    char name[64];
    float weight;
};

// SEH only (no C++ objects with destructors in here): the engine's admin, its states, their names and weights
int readRaw(RawState* out, int max) {
    const auto& s = sym();
    __try {
        EngineString cls;
        reinterpret_cast<StringCtorFn>(s.stringCtor)(&cls, "eCWeatherAdmin");
        void* admin = reinterpret_cast<FindModuleFn>(s.moduleAdminFindModule)(reinterpret_cast<ModuleAdminFn>(s.moduleAdminGetInstance)(), &cls);
        reinterpret_cast<StringDtorFn>(s.stringDtor)(&cls);
        if (admin == nullptr || *static_cast<void* const*>(admin) != s.weatherAdminVftable) return 0;
        const int n = reinterpret_cast<StatesCountFn>(s.weatherStatesCount)(admin);
        int k = 0;
        for (int i = 0; i < n && k < max; ++i) {
            void* st = reinterpret_cast<StatesAtFn>(s.weatherStatesAt)(admin, i);
            if (st == nullptr) continue;
            const void* nm = reinterpret_cast<StateNameFn>(s.weatherStateName)(st);
            const char* text = nm != nullptr ? reinterpret_cast<StringGetTextFn>(s.stringGetText)(nm) : nullptr;
            RawState& r = out[k++];
            r.name[0] = '\0';
            if (text != nullptr) {
                std::size_t len = 0;
                while (len + 1 < sizeof(r.name) && text[len] != '\0') ++len;
                std::memcpy(r.name, text, len);
                r.name[len] = '\0';
            }
            r.weight = reinterpret_cast<StateBlendFn>(s.weatherStateBlend)(st);
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// SEH only: the weather admin's player position
bool readPlayer(float* out) {
    const auto& s = sym();
    __try {
        EngineString cls;
        reinterpret_cast<StringCtorFn>(s.stringCtor)(&cls, "eCWeatherAdmin");
        void* admin = reinterpret_cast<FindModuleFn>(s.moduleAdminFindModule)(reinterpret_cast<ModuleAdminFn>(s.moduleAdminGetInstance)(), &cls);
        reinterpret_cast<StringDtorFn>(s.stringDtor)(&cls);
        if (admin == nullptr || *static_cast<void* const*>(admin) != s.weatherAdminVftable) return false;
        const float* p = reinterpret_cast<PlayerPositionFn>(s.weatherPlayerPosition)(admin);
        if (p == nullptr) return false;
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool contains(const std::string& lower, const char* what) { return lower.find(what) != std::string::npos; }

}  // namespace

std::vector<WeatherState> weatherStates() {
    const auto& s = sym();
    std::vector<WeatherState> out;
    if (s.stringCtor == nullptr || s.stringDtor == nullptr || s.stringGetText == nullptr || s.moduleAdminGetInstance == nullptr ||
        s.moduleAdminFindModule == nullptr || s.weatherAdminVftable == nullptr || s.weatherStatesCount == nullptr ||
        s.weatherStatesAt == nullptr || s.weatherStateName == nullptr || s.weatherStateBlend == nullptr) {
        return out;
    }
    static RawState raw[256];
    const int n = readRaw(raw, 256);
    for (int i = 0; i < n; ++i) out.push_back({raw[i].name, raw[i].weight});
    return out;
}

std::optional<std::array<float, 3>> playerPosition() {
    const auto& s = sym();
    if (s.stringCtor == nullptr || s.stringDtor == nullptr || s.moduleAdminGetInstance == nullptr || s.moduleAdminFindModule == nullptr ||
        s.weatherAdminVftable == nullptr || s.weatherPlayerPosition == nullptr) {
        return std::nullopt;
    }
    std::array<float, 3> p{};
    if (!readPlayer(p.data())) return std::nullopt;
    if (!(std::abs(p[0]) + std::abs(p[1]) + std::abs(p[2]) > 1e-3f) || !std::isfinite(p[0] + p[1] + p[2])) return std::nullopt;
    return p;
}

std::optional<float> seaLevelOfWeather(const std::vector<WeatherState>& states) {
    float sum = 0.0f, weight = 0.0f;
    for (const WeatherState& st : states) {
        if (!(st.weight > 1e-3f)) continue;
        std::string l = st.name;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (contains(l, "dungeon") || contains(l, "cave") || contains(l, "indoor")) continue;
        float level = 1.0f;
        if (contains(l, "storm") || contains(l, "thunder")) level = 3.0f;
        else if (contains(l, "rain")) level = 2.0f;
        else if (contains(l, "cloud") || contains(l, "overcast") || contains(l, "half")) level = 1.2f;
        else if (contains(l, "fog") || contains(l, "mist")) level = 0.5f;
        else if (contains(l, "sun") || contains(l, "clear")) level = 0.6f;
        sum += level * st.weight;
        weight += st.weight;
    }
    if (weight <= 1e-3f) return std::nullopt;
    return sum / weight;
}

}  // namespace rvk::engine
