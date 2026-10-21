#pragma once

#include <filesystem>

namespace rvk::paths {

// Remembers our own module (call from DllMain before anything else).
void init(void* selfModule);

void* selfModule();
// <Risen>\bin (the directory of our d3d9.dll)
const std::filesystem::path& binDir();
// <Risen>\bin\rvk: config.toml, rvk.log, caches, captures
const std::filesystem::path& dataDir();

}  // namespace rvk::paths
