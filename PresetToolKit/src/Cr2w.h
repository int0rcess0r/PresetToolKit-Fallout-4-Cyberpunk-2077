#pragma once
#include "Gltf.h"
#include "MathUtil.h"

#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace ptk {

// Oodle Kraken, the compression used for the data buffers inside Cyberpunk's CR2W files.
// On Windows it is loaded from the game's own bin\x64\oo2ext_7_win64.dll (the same library
// WolvenKit uses). Elsewhere a decoder-only library can be supplied through the environment
// variable PTK_KRAKEN_LIB (exporting `int ooz_decompress(src, srcLen, dst, dstLen)`), for tests.
class Kraken {
public:
    // Tries each folder for oo2ext_7_win64.dll (Windows) and PTK_KRAKEN_LIB.
    bool load(const std::vector<std::filesystem::path>& searchDirs);
    bool canDecompress() const { return dec_ != nullptr || ooz_ != nullptr; }
    bool canCompress() const { return comp_ != nullptr; }
    const std::string& source() const { return source_; }

    std::vector<uint8_t> decompress(const uint8_t* src, size_t srcLen, size_t rawLen) const;
    // Returns an empty vector if compression is unavailable or failed.
    std::vector<uint8_t> compress(const std::vector<uint8_t>& raw) const;

private:
    void* dec_ = nullptr;
    void* comp_ = nullptr;
    void* ooz_ = nullptr;
    std::string source_;
};

struct MorphtargetPatchStats {
    size_t vertices = 0;
    size_t moved = 0;
    size_t clamped = 0;
    size_t neutralized = 0;       // character creator targets switched off
    bool recompressed = false;
    bool storedRaw = false;       // a changed buffer had to be stored uncompressed
};

// What to change in a .morphtarget (everything else stays byte-for-byte as in the game file).
struct MorphtargetPatch {
    // New vertex positions / normals / tangents from a WolvenKit export of this exact file.
    const Glb* glb = nullptr;
    // Or: move every vertex by displace(position) (glTF frame, metres); normals are kept.
    std::function<Vec3(const Vec3&)> displace;
    // Switch off the character creator face shapes (eyes / nose / mouth / jaw / ear targets).
    bool neutralizeCharacterCreator = false;
};
MorphtargetPatchStats patchMorphtarget(const std::filesystem::path& vanillaMorphtarget,
                                       const std::filesystem::path& outFile, const Kraken& kraken,
                                       const MorphtargetPatch& patch);

// Writes the vertex positions (and normals/tangents) of `glb` - a WolvenKit export of
// `vanillaMorphtarget`, possibly deformed - straight into the head geometry embedded in the
// .morphtarget (rendRenderMeshBlob render buffer), and saves the result to `outFile`.
// Everything else - skin indices/weights, bone names, rig matrices, CC morph data, the face
// rig binding - stays byte-for-byte as in the game file. Vertices that did not move keep
// their original bytes.
MorphtargetPatchStats patchMorphtargetGeometry(const std::filesystem::path& vanillaMorphtarget, const Glb& glb,
                                               const std::filesystem::path& outFile, const Kraken& kraken);

} // namespace ptk
