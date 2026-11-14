#pragma once
#include "GameSaves.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ptk {

// Fallout 4 character creation data (data/fo4_chargen.json, generated from Fallout4.esm by
// tools/make_fo4_chargen.py): HumanRace tint layers per sex, eye head parts, hair colours.
struct Fo4TintColor {
    int id = 0;               // ColorID in a LooksMenu preset
    std::string name;         // colour form editor ID
    uint8_t r = 0, g = 0, b = 0;
};
struct Fo4TintLayer {
    uint32_t index = 0;       // tint template index (LooksMenu "Tints" key)
    std::string kind;         // skin, lipstick, eyeshadow, eyeliner, blush, freckles, blemish, brows, ...
    int type = 0;             // 0 mask, 1 palette, 2 texture
    int number = 0;           // number in the texture name (Freckles2_d.dds -> 2)
    std::string texture;
    std::vector<Fo4TintColor> colors;
};
struct Fo4FaceMorphTable {
    std::map<uint32_t, std::string> presets;                              // preset key -> .tri morph
    std::map<uint32_t, std::pair<std::string, std::string>> sliders;      // slider key -> (negative, positive) morph
    std::map<uint32_t, std::string> groups;                               // preset key -> group (Eyes, Nose, ...)
};
struct Fo4Chargen {
    std::map<std::string, Fo4FaceMorphTable> faceMorphs;                  // "female" / "male"
    std::map<std::string, std::vector<Fo4TintLayer>> tints;              // "female" / "male"
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> eyes;   // sex -> (form, editor id)
    bool loaded = false;
};
Fo4Chargen loadFo4Chargen(const std::filesystem::path& json);

// V's look in Fallout 4 terms.
struct Fo4Look {
    std::string eyesForm;                 // "Fallout4.esm|14EC28" (eye head part), empty if unknown
    std::vector<PresetTint> tints;         // skin tone, brows, freckles, blemishes, makeup
    std::vector<std::string> lines;       // what was mapped, for the report
};

// Maps the save's eye colour, skin tone, eyebrows, freckles, blemishes and makeup to the closest
// Fallout 4 eye head part and tint layers / colours.
Fo4Look mapCyberpunkLook(const CpAppearance& app, const Fo4Chargen& data, bool female);

// Colour of a name made of colour words ("06_black_carbon", "12_gradient_brown"); false if none known.
bool colorFromName(const std::string& name, uint8_t rgb[3]);

} // namespace ptk
