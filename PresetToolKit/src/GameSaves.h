#pragma once
#include "LooksMenuPreset.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ptk {

// ------------------------------------------------------------------ Fallout 4 (.fos)
// Reads the player's face from a Fallout 4 save: the face sculpt regions, morph slider values
// and face presets of the player's NPC_ change form (the same data a LooksMenu preset holds),
// plus gender, name and body weight from the save.
struct Fo4SaveInfo {
    std::string playerName;
    std::string race;
    int level = 0;
    bool hasFaceData = false;
    std::string faceSource;         // "player" or "spouse NPC (prologue)"
    std::vector<double> weight;   // thin / muscular / large (empty if not found)
};
LooksMenuPreset loadFo4Save(const std::filesystem::path& fos, Fo4SaveInfo* info = nullptr);
bool isFo4Save(const std::filesystem::path& p);

// Writes `face` into a copy of a Fallout 4 save: face presets, sculpt regions, Morphs.Values,
// tints (merged: replaceTints / the face's tint indices are swapped out, the rest kept), the
// eye head part (replaceHeadParts -> headParts[0]) and the hair colour (Fallout4.esm forms only),
// plus the body weight when `weight` has 3 values. The face form the save already uses (the player,
// or the prologue spouse NPC) is rewritten; everything else is copied byte for byte. The .f4se
// co-save, if any, is copied next to `out`. Throws if the save's sex does not match face.gender.
struct Fo4SaveWriteInfo {
    std::string faceSource;           // "player" or "spouse NPC (prologue)"
    std::vector<std::string> lines;   // what was changed
    bool coSaveCopied = false;
};
Fo4SaveWriteInfo writeFo4SaveFace(const std::filesystem::path& in, const std::filesystem::path& out,
                                  const LooksMenuPreset& face, const std::vector<double>& weight = {});

// Reads any Fallout 4 character input: a LooksMenu preset (.json) or a save (.fos).
LooksMenuPreset loadFo4Character(const std::filesystem::path& p, Fo4SaveInfo* info = nullptr);

// ------------------------------------------------------------------ Cyberpunk 2077 (sav.dat)
// V's character-creator choices from a Cyberpunk 2077 (2.x) save.
struct CpAppearance {
    bool isMale = false;
    bool genderKnown = false;
    // region ("eyes", "nose", "mouth", "jaw", "ear") -> morph target name ("h141")
    std::map<std::string, std::string> morphs;
    std::string hairColor;         // character creator colour, e.g. "06_black_carbon"
    int hairStyle = -1;            // character creator hair style number (hair_color32 -> 32)
    std::string eyebrowColor;      // e.g. "female__06_black_carbon"
    // every head appearance choice (name -> definition), e.g. "eyes_color" -> "he_000_pwa__basehead__12_gradient_brown",
    // "skin_type_01" -> "h0_000_pwa__basehead__01_ca_pale_00_warm_ivory", "makeupPimples_03" -> "...pimples_01__brown_01"
    std::vector<std::pair<std::string, std::string>> appearances;
    std::string saveName;          // folder name of the save
    std::vector<std::string> notes;
};
// `p` may be sav.dat or the save's folder.
CpAppearance loadCpSave(const std::filesystem::path& p);
bool isCpSave(const std::filesystem::path& p);

// Fallout 4 hair colour (Fallout4.esm form) closest to a Cyberpunk hair colour name.
struct Fo4HairColor {
    uint32_t formId;
    const char* editorId;
    uint8_t r, g, b;
};
const Fo4HairColor& nearestFo4HairColor(const std::string& cyberpunkColor, bool* recognised = nullptr);
std::string fo4FormRef(uint32_t formId);   // "Fallout4.esm|19EE60"

// LZ4 block decompression (exposed for tests). Throws on malformed input.
std::vector<uint8_t> lz4DecompressBlock(const uint8_t* src, size_t srcLen, size_t rawLen);

} // namespace ptk
