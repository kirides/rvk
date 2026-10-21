#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace rvk::engine {

// eCVirtualFileSystem::MakeAbsPhysicalPath: a virtual engine path ("#G3:/snapshots/x.bmp", mounts from mountlist_*.ini and
// the embedded [Physical] section) -> the physical file path. Engine strings are ANSI. nullopt when the engine symbols are
// missing or the engine cannot map the path. Call only after the engine is initialised (not in DllMain).
std::optional<std::filesystem::path> physicalPath(const char* enginePath, std::string* error = nullptr);

// eCApplication::Snapshot() (the debug key's action: names #G3:/snapshots/<prefix>_<date>_<NNNN>.bmp and sets the request
// flag that eCApplication::Draw turns into HAL device slot 159). False when the symbols are missing.
bool requestEngineSnapshot();
// eCApplication::GetLastSnapshotName() (the virtual path of the last request), empty when unavailable
std::string lastEngineSnapshotName();

}  // namespace rvk::engine
