#pragma once
#include "MathUtil.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ptk {

// A Cyberpunk-side deformation region. Coordinates are normalised to the head mesh's
// bounding box in "head space": u = right (0..1), v = up (0..1), w = forward (0 = back, 1 = front).
struct RegionDef {
    std::string name;
    Vec3 center{0.5, 0.5, 0.5};
    Vec3 radius{0.1, 0.1, 0.1};
    bool mirror = false;     // also apply mirrored at u' = 1 - u (ears, eyes, cheeks...)
    double strength = 1.0;
};

struct IdRange {
    uint32_t from = 0, to = 0;
    std::string region;
};

// Rule for a Morphs.Values slider: per unit of slider value, add this to a region.
// Keyword rule: a FO4 region whose name contains `match` (at a word start) maps to `region`.
struct NameRule {
    std::string match;
    std::string region;
};

struct ValueRule {
    std::string region;
    Vec3 translate;     // FO4 axes (x right, y forward, z up), normalised units
    Vec3 rotate;        // FO4 axes, normalised units (multiplied by rotation gain)
    double scale = 0;   // normalised units (multiplied by scale gain)
};

// One bone of a Fallout 4 sculpt region (HumanRaceFacialBoneRegions*.txt). A preset value v
// in [-1, 1] moves the bone by  v * Maxima  (v >= 0)  or  -v * Minima  (v < 0), per component.
// Axes: x = character's right, y = up, z = forward (FO4 units / degrees / scale fraction).
struct Fo4Bone {
    std::string name;   // e.g. "bone_R_EyebrowIn"
    Vec3 posMax, posMin, rotMax, rotMin, sclMax, sclMin;
};

struct Fo4Region {
    uint32_t id = 0;
    std::string name;   // e.g. "Eyebrows - Inner"
    std::string group;  // AssociatedMorphGroup / AssociatedTintGroup
    std::vector<Fo4Bone> bones;
};

// Where a FO4 face bone sits on the Cyberpunk head, in normalised head space.
// Calibrated entries (tools/calibrate.py) are per side ("r_eye", "l_eye", "c_nose") and carry
// the bone's pivot and local axes; hand-made entries ("eye") are mirrored for the left side.
struct BoneLocation {
    Vec3 center{0.5, 0.5, 0.5};   // centre of the skin the bone moves
    Vec3 radius{0.05, 0.05, 0.05};
    Vec3 pivot{0.5, 0.5, 0.5};    // where rotation / scale pivot (bone position)
    bool hasPivot = false;
    Vec3 axes[3];                 // bone local X/Y/Z in head space (right, up, forward)
    bool hasAxes = false;
};

struct BoneSet {
    double metersPerUnit = 0.01428;
    double rotationScale = 1.0, scaleScale = 1.0, strength = 1.0;
    std::map<std::string, BoneLocation> locations;
    bool present = false;
};

struct Mapping {
    // Head frame in glTF space. up/forward: "+x","-x","+y","-y","+z","-z" or "auto".
    std::string up = "+y";
    std::string forward = "auto";

    // Gains: FO4 normalised value 1.0 ->
    double positionGain = 0.05;     // fraction of head width
    double rotationGainDeg = 15.0;  // degrees
    double scaleGain = 0.25;        // relative scale change
    double globalStrength = 1.0;
    double clampValue = 1.0;        // FO4 values are clamped to [-clamp, clamp]
    std::string combine = "average"; // "average" or "sum" when several IDs hit one region

    // Indices into the FO4 region float array (-1 = absent)
    int idxPos[3] = {0, 1, 2};
    int idxRot[3] = {3, 4, 5};
    int idxScale = 6;

    std::vector<RegionDef> regions;
    std::vector<IdRange> ranges;
    std::map<uint32_t, std::string> idOverrides;
    std::vector<NameRule> nameRules;                  // ordered; first match wins
    std::map<uint32_t, std::string> fo4RegionNames;   // from HumanRaceFacialBoneRegions*.txt
    std::map<uint32_t, Fo4Region> fo4Regions;         // bone data from the same file
    std::map<std::string, BoneLocation> boneLocations; // active set (see selectGender)
    std::map<std::string, BoneSet> boneSets;           // "", "female", "male"
    double fo4UnitMeters = 0.01428;                    // 1 Fallout 4 unit in metres
    double fo4RotationScale = 1.0;
    double fo4ScaleScale = 1.0;
    double fo4Strength = 1.0;
    std::map<uint32_t, ValueRule> valueRules;

    // Packaging
    std::map<std::string, std::string> headDepotPath;  // "female"/"male" -> depot path of head .morphtarget
    std::map<std::string, std::string> headMeshDepotPath; // "female"/"male" -> depot path of head .mesh
    std::map<std::string, std::string> commands;       // "import","pack"
    bool stripSkinForMorphImport = true;               // see ArchivePackager (WolvenKit.CLI morphtarget import)
    std::string glbNaming = "auto";                    // auto: x.mesh -> x.glb, x.morphtarget -> x.morphtarget.glb

    const RegionDef* findRegion(const std::string& name) const;
    std::optional<std::string> regionForId(uint32_t id) const;
    // Activates the gender-specific calibrated bone set (falls back to the generic one).
    void selectGender(const std::string& gender);
    // Looks up a bone by side ('L','R','C') and key; *mirrored is set when a right-side
    // hand-made entry is reused for the left side.
    const BoneLocation* findBone(char side, const std::string& key, bool* mirrored) const;
    std::optional<std::string> regionForName(const std::string& fo4Name) const;
    std::string fo4NameOf(uint32_t id) const;
};

Mapping loadMapping(const std::filesystem::path& path);

// Reads Fallout 4's HumanRaceFacialBoneRegions(Female|Male).txt from
// Data/Meshes/Actors/Character/CharacterAssets and returns region ID -> region name. Tolerant of the exact layout.
std::map<uint32_t, std::string> loadFo4RegionNames(const std::filesystem::path& path);

// Full bone data from the same file (empty if the file is not valid JSON).
std::map<uint32_t, Fo4Region> loadFo4Regions(const std::filesystem::path& path);

// "bone_R_EyebrowIn" -> side 'R', key "eyebrowin"
void splitBoneName(const std::string& bone, char& side, std::string& key);

} // namespace ptk
