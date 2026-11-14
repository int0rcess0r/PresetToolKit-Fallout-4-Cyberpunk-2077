#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ptk {

// One facial bone region sculpt ("Morphs" -> "Regions" in a LooksMenu / F4EE preset).
// The float array is the region transform as LooksMenu stores it. With the default
// mapping it is interpreted as [posX, posY, posZ, rotX, rotY, rotZ, scale, ...].
struct RegionSculpt {
    uint32_t id = 0;
    std::vector<double> values;
};

// One LooksMenu tint ("Tints" entry).
struct PresetTint {
    uint32_t index = 0;       // tint template index
    int type = 0;             // 0 mask, 1 palette, 2 texture
    uint32_t color = 0;       // palette colour as LooksMenu stores it: 0x00BBGGRR
    int colorId = 0;          // palette colour template ID
    int percent = 100;
    std::string kind;
};

struct LooksMenuPreset {
    std::string name;            // taken from the file name
    int gender = -1;             // FO4: 0 = male, 1 = female, -1 unknown
    std::string race;
    std::string hairColor;       // "Plugin.esm|FORMID" (HairColor); empty = not set
    std::vector<std::string> headParts;           // head parts to set ("Plugin.esm|FORMID")
    std::vector<std::string> replaceHeadParts;    // base preset head parts these replace (e.g. all eye parts)
    std::vector<PresetTint> tints;                // tints to set
    std::vector<uint32_t> replaceTints;           // base preset tint indices these replace
    std::vector<RegionSculpt> regions;             // Morphs.Regions
    std::map<uint32_t, double> morphValues;        // Morphs.Values  (slider key -> value)
    std::map<uint32_t, double> presetWeights;      // Morphs.Presets (face preset index -> weight)
    std::vector<std::string> parseWarnings;
};

LooksMenuPreset loadLooksMenuPreset(const std::filesystem::path& path);

// Writes a LooksMenu (F4EE) preset .json with the preset's gender and face morphs. Everything
// else (hair, head parts, tints, body weight) is taken from `basePresetJson` when given, so an
// existing preset can be used as the starting point; `weight` overrides the body weight.
std::string looksMenuPresetJson(const LooksMenuPreset& preset, const std::vector<double>& weight = {},
                                const std::filesystem::path& basePresetJson = {});

} // namespace ptk
