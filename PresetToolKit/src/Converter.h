#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ptk {

// Everything one conversion needs. Shared by the command-line tool and the GUI.
struct ConvertOptions {
    std::filesystem::path preset;               // LooksMenu preset (.json) or Fallout 4 save (.fos)
    std::filesystem::path fo4Dir;               // optional Fallout 4 folder (reads loose files + .ba2 archives)
    std::filesystem::path cpGameDir;            // optional Cyberpunk 2077 folder (head pulled via WolvenKit.CLI)
    std::filesystem::path cacheDir;             // where heads extracted from the game are cached
    std::filesystem::path headGlb;              // head .morphtarget GLB (optional when cpGameDir is set)
    std::filesystem::path headMeshGlb;          // optional head .mesh exported to GLB
    std::filesystem::path wolvenkit;            // optional WolvenKit.CLI executable
    std::filesystem::path templateMorphtarget;  // optional vanilla .morphtarget
    std::filesystem::path templateMesh;         // optional vanilla .mesh
    std::string headDepotPath, meshDepotPath;   // optional overrides
    std::filesystem::path fo4Regions;           // optional HumanRaceFacialBoneRegions*.txt
    std::filesystem::path workDir = "PresetToolKit_out";// report, WolvenKit project, intermediate files
    std::filesystem::path archiveDest;          // optional: copy the finished .archive here
    bool archiveToDest = true;                  // copy the standalone .archive into archiveDest
    bool zipToDest = false;                     // copy the installable .zip (mod managers) into archiveDest
    std::filesystem::path dataDir;              // folder with default_mapping.json
    std::filesystem::path mapping;              // optional mapping override
    std::string modName;                        // default LM_<preset>_Head
    std::string gender;                         // "", "female", "male"
    std::string meshFilter;
    double strength = 1.0;
    bool keepMorphTargets = false;
    // Also re-import V's .mesh. Off by default: the .morphtarget defines the head shape, and WolvenKit 9's
    // mesh import rebuilds the bone bind matrices from the GLB, which breaks skinning/facial animation
    // for heads exported with the classic exporter.
    bool replaceMesh = false;
    bool wolvenkitMorphImport = false;          // old path: WolvenKit glTF import of the .morphtarget
    // The character creator's eyes / nose / mouth / jaw / ear shapes are switched off by default
    // (in the head and the other face parts) so V's face is exactly the converted one.
    bool keepCharacterCreatorShapes = false;
    bool companions = true;                     // eyes, brows, lashes, teeth, decals follow the sculpt
    bool approximateSculpt = false;             // use the bone model on V's head even with Fallout 4's files

    // ---- Cyberpunk 2077 -> Fallout 4 (LooksMenu preset from a Cyberpunk save)
    bool cpToFo4 = false;
    std::filesystem::path cpSave;               // sav.dat (or its folder)
    std::filesystem::path presetOut;            // preset .json file or folder (default: workDir)
    std::filesystem::path fo4SaveTarget;        // optional FO4 save: a copy of it gets the face (CP -> FO4)
    std::filesystem::path fo4SaveOut;           // that copy (default: next to the target, "<stem>_<name>.fos")
    std::filesystem::path basePreset;           // optional preset .json (hair/tints/body kept) or FO4 save (body weight)
    bool extendedSculpting = false;             // Extended Facial Sculpting mod: its region files (data/efs) are used
    bool extendedSculptingRedux = false;        // Extended Facial Sculpting REDUX (data/efs_redux); wins over the above
    bool approximateCustomization = true;       // carry hair colour, eyes, skin, brows, freckles, blemishes, makeup over
};

struct ConvertResult {
    int exitCode = 1;                           // 0 = archive built, 2 = nothing to do, 3 = staged only
    std::string report;
    std::filesystem::path archive;              // final .archive (in archiveDest if given)
    std::filesystem::path zip;                  // installable zip (in archiveDest when zipToDest, else workDir)
    bool archiveCopied = false;                 // .archive was placed in archiveDest
    bool zipCopied = false;                     // .zip was placed in archiveDest
    std::filesystem::path projectDir;           // WolvenKit project folder
    std::filesystem::path manualSteps;          // MANUAL_STEPS.txt when not built
    std::filesystem::path presetFile;           // Cyberpunk -> Fallout 4: the LooksMenu preset written
    std::filesystem::path saveFile;             // Cyberpunk -> Fallout 4: the edited save copy
};

// Runs the whole pipeline. Throws std::exception on hard errors.
// Fallout 4 (LooksMenu preset or .fos save) -> Cyberpunk 2077 head replacement, or with
// opt.cpToFo4 Cyberpunk 2077 save -> LooksMenu preset.
ConvertResult runConversion(const ConvertOptions& opt);
ConvertResult runReverseConversion(const ConvertOptions& opt);

// The generated face as a 3D model (.glb or .obj, by the extension of `out`; a folder -> a .glb named
// after the input). Source: opt.preset (Fallout 4 preset / save), or with opt.cpToFo4 opt.cpSave.
// Head: "cp" = V's head (Cyberpunk 2077's mesh), "fo4" = Fallout 4's head mesh (needs the Fallout 4 folder).
struct ModelExportResult {
    std::filesystem::path file;
    std::string report;
};
ModelExportResult exportFaceModel(const ConvertOptions& opt, const std::string& head, const std::filesystem::path& out);

// Folder containing default_mapping.json next to `exeDir`, or ./data as a fallback.
std::filesystem::path defaultDataDir(const std::filesystem::path& exeDir);

// UTF-8 text of a path (safe for logging on every platform).
std::string u8(const std::filesystem::path& p);

} // namespace ptk
