#include "engine/symbols.h"

#include "core/exports.h"

#include <spdlog/spdlog.h>

namespace rvk::engine {
namespace {
Symbols g_sym;
}

bool resolveSymbols() {
    bool ok = true;
    int missing = 0;
#define RVK_SYMBOL_RESOLVE(field, module, name, required)                                \
    g_sym.field = exports::resolve(module, name);                                        \
    if (g_sym.field == nullptr) {                                                        \
        ++missing;                                                                       \
        if (required) {                                                                  \
            ok = false;                                                                  \
            spdlog::error("engine symbol {} missing: {}", #field, name);                 \
        } else {                                                                         \
            spdlog::warn("optional engine symbol {} missing: {}", #field, name);         \
        }                                                                                \
    }
    RVK_ENGINE_SYMBOLS(RVK_SYMBOL_RESOLVE)
#undef RVK_SYMBOL_RESOLVE
    spdlog::info("engine symbols resolved ({} missing)", missing);
    return ok;
}

const Symbols& sym() { return g_sym; }

}  // namespace rvk::engine
