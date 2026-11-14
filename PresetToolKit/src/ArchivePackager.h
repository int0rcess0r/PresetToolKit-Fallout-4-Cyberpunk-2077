#pragma once
#include "Gltf.h"
#include "Mapping.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ptk {

// One game file to replace: the deformed GLB is imported over `templateFile`
// (the vanilla file extracted with WolvenKit) and packed at `depotPath`.
struct HeadFile {
    const Glb* glb = nullptr;
    std::filesystem::path templateFile;  // may be empty (stage-only)
    std::string depotPath;               // e.g. base\characters\head\...\x.morphtarget
};

struct PackageOptions {
    std::string modName;                 // archive name, e.g. "LM_MyCharacter_Head"
    std::filesystem::path outDir;
    std::filesystem::path wolvenkit;     // WolvenKit.CLI executable; empty = stage only
    std::vector<HeadFile> files;         // imported in order (.mesh before .morphtarget)
    // .morphtarget files: write the GLB's vertices straight into the game file instead of a
    // WolvenKit import (keeps the skinning / face rig binding exactly as in the game).
    bool patchMorphtargets = true;
    bool neutralizeCharacterCreator = false;      // switch off the CC face shapes in the head too
    // Ready-made game files to pack as they are: (file, depot path)
    std::vector<std::pair<std::filesystem::path, std::string>> preparedFiles;
    std::vector<std::filesystem::path> oodleDirs;  // where to look for oo2ext_7_win64.dll
};

struct PackageResult {
    bool complete = false;                 // .archive was built
    std::filesystem::path zipPath;         // installable mod zip (if complete)
    std::filesystem::path projectDir;      // WolvenKit-style project folder (always written)
    std::vector<std::string> notes;
    std::vector<std::string> report;       // lines for report.txt
};

PackageResult packageHeadReplacement(const Mapping& mapping, const PackageOptions& opt);

} // namespace ptk
