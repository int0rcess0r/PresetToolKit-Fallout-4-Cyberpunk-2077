#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ptk {

// Writes a store-only (uncompressed) .zip containing every file under `root`
// (paths inside the zip are relative to `root`). Mod managers (Vortex/MO2) accept it.
void zipDirectory(const std::filesystem::path& root, const std::filesystem::path& zipPath);

} // namespace ptk
