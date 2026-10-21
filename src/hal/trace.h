#pragma once

#include <filesystem>

namespace rvk::hal::trace {

struct Options {
    int logFirst = 3;                   // calls per slot logged with arguments
    bool capture = true;                // write shader bytecode / effect sources to `captureDir`
    std::filesystem::path captureDir;   // <bin>\rvk\corpus
    std::filesystem::path statsFile;    // <bin>\rvk\trace-stats.txt (rewritten every few seconds)
};

void configure(const Options& options);

// Replaces eCGfxAdmin::ms_pAPIInstance by a tracing wrapper (call right after the original InitializeAdmin).
// Every object the engine gets from it afterwards (device, textures, buffers, ...) is a tracing wrapper too.
bool install(void** apiInstanceSlot);

// Writes the call statistics now (also done periodically from SwapScreen and at process exit).
void writeStats();

}  // namespace rvk::hal::trace
