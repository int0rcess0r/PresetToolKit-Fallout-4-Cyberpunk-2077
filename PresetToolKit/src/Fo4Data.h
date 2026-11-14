#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ptk {

// Read-only view of a Fallout 4 installation: loose files in Data\ win (like the game, so mods
// such as HiPoly Faces override), then general .ba2 archives ("Fallout4 - Meshes.ba2", mod BA2s).
class Fo4Data {
public:
    // `dir` may be the game folder or its Data folder.
    explicit Fo4Data(const std::filesystem::path& dir);

    bool valid() const { return !dataDir_.empty(); }
    const std::filesystem::path& dataDir() const { return dataDir_; }

    // path relative to Data, e.g. "Meshes\\Actors\\Character\\CharacterAssets\\BaseFemaleHead.nif"
    std::optional<std::vector<uint8_t>> read(const std::string& relPath, std::string* source = nullptr) const;

    size_t archiveCount() const { return archives_.size(); }

private:
    struct Entry {
        size_t archive;
        uint64_t offset;
        uint32_t packed, size;
    };
    std::filesystem::path dataDir_;
    std::vector<std::filesystem::path> archives_;
    std::map<std::string, Entry> index_;   // lower-case, backslash paths

    void indexArchive(const std::filesystem::path& p);
};

} // namespace ptk
