// PresetToolKit (command line) - bakes a Fallout 4 character (LooksMenu preset or save) into V's head and
// packages it as a Cyberpunk 2077 player head replacement (.archive), or turns V's face from a
// Cyberpunk 2077 save into a LooksMenu preset for Fallout 4.

#include "Converter.h"
#include "GameSaves.h"
#include "LooksMenuPreset.h"
#include "Util.h"

#include <iostream>
#include <map>
#include <string>

using namespace ptk;

namespace {

const char* kUsage = R"(PresetToolKit - Fallout 4 <-> Cyberpunk 2077 faces

Usage:
  Fallout 4 -> Cyberpunk 2077 (player head replacement .archive):
    PresetToolKitCLI --preset <preset.json | save.fos> --fo4-dir <Fallout 4> --cp-game <Cyberpunk 2077> --wolvenkit <exe> [options]
    PresetToolKitCLI --preset <preset.json | save.fos> --head <head.morphtarget.glb> [options]
  Cyberpunk 2077 -> Fallout 4 (LooksMenu preset .json):
    PresetToolKitCLI --cp-save <sav.dat | save folder> --cp-game <Cyberpunk 2077> --wolvenkit <exe> --fo4-dir <Fallout 4>
          [--preset-out <file.json | folder>] [--base-preset <preset.json | save.fos>] [--fo4-save <save.fos>]
  Fallout 4 save -> LooksMenu preset (no conversion):
    PresetToolKitCLI --export-preset <out.json> --preset <save.fos>

Fallout 4 input:
  --preset <file>              LooksMenu preset (Data\F4SE\Plugins\F4EE\Presets\*.json) or a Fallout 4
                               save (Documents\My Games\Fallout4\Saves\*.fos) - the player's face is read

Cyberpunk 2077 -> Fallout 4:
  --cp-save <path>             sav.dat (or its folder) from Saved Games\CD Projekt Red\Cyberpunk 2077
  --preset-out <path>          Where to write the preset (file, or folder; e.g.
                               Fallout 4\Data\F4SE\Plugins\F4EE\Presets). Default: --out folder
  --efs                        Extended Facial Sculpting mod: use its sculpt regions (bundled in
                               data/efs) - read them from a preset, and fit with them (the preset then
                               needs the mod). Presets using its sliders are detected automatically.
  --efs-redux                  Extended Facial Sculpting REDUX instead (bundled in data/efs_redux):
                               more sliders, the closest Fallout 4 presets. Detected automatically too.
  --no-customization           Face shape only: don't carry hair colour, eyes, skin tone, brows, freckles,
                               blemishes and makeup over from the save (approximate customization)
  --base-preset <path>         Keep hair, head parts, tints and body from this LooksMenu preset, or the
                               body weight from this Fallout 4 save
  --fo4-save <save.fos>        Also write the face into a COPY of this Fallout 4 save (same sex as V);
                               the save's hairstyle, body and other tints are kept
  --fo4-save-out <path>        That copy (file or folder; default "<save>_<name>.fos" next to it)

Export model (the generated face as a 3D model, instead of a mod / preset):
  --export-model <path>        .glb or .obj file (or a folder: <name>_<head>_head.glb). The face comes
                               from --preset (Fallout 4 preset / save) or --cp-save (Cyberpunk save)
  --model-head cp|fo4          Head mesh: cp = Cyberpunk 2077's (V's head, default),
                               fo4 = Fallout 4's (needs --fo4-dir)

Game folders (no manual extraction needed):
  --fo4-dir <dir>              Fallout 4 folder (or its Data folder). Reads loose files and the .ba2
                               archives: sculpt regions + face bones from the head .nif files
  --cp-game <dir>              Cyberpunk 2077 folder. V's head is pulled from archive\pc\content with
                               WolvenKit.CLI (cached after the first run)
  --cache-dir <dir>            Where extracted heads are cached (default: <out>/cache)

V's head files (instead of --cp-game, e.g. from a WolvenKit project):
  --head <file.glb>            V's head .morphtarget exported to GLB by WolvenKit

Optional (advanced, used only with --replace-mesh):
  --head-mesh <file.glb>       V's head .mesh exported to GLB by WolvenKit

Packaging (builds the .archive automatically when WolvenKit.CLI and the templates are given):
  --wolvenkit <exe>            Path to WolvenKit.CLI (WolvenKit.CLI.exe / wolvenkit.cli)
  --template-morphtarget <f>   The vanilla head .morphtarget the --head GLB came from
  --template-mesh <f>          The vanilla head .mesh the --head-mesh GLB came from
  --head-depot-path <path>     Depot path of the .morphtarget (auto-detected from a WolvenKit
                               project path, else taken from the mapping file)
  --mesh-depot-path <path>     Depot path of the .mesh (default: same as above with .mesh)

Fallout 4 region names (needed for vanilla sculpt IDs such as 186A0..., 274E):
  --fo4-regions <file>         HumanRaceFacialBoneRegionsFemale.txt (or ...Male.txt); not needed with
                               --fo4-dir (a bundled copy is used otherwise)

Options:
  --out <dir>                  Work folder: report, WolvenKit project, zip (default: ./PresetToolKit_out)
  --archive-dest <dir>         Destination folder (e.g. Cyberpunk 2077\archive\pc\mod); the standalone
                               .archive is copied there unless --no-archive-to-dest is given
  --zip-to-dest                Also put the installable .zip (for Vortex / MO2) in --archive-dest
                               (or, for presets, in --preset-out)
  --no-archive-to-dest         Don't copy the standalone .archive (or preset .json) to the destination
  --name <ModName>             Archive name (default: LM_<preset name>_Head)
  --gender <female|male>       Override the preset's gender (selects the default head path)
  --mapping <file.json>        Region mapping (default: <exe dir>/data/default_mapping.json)
  --data-dir <dir>             Folder containing default_mapping.json
  --strength <f>               Overall multiplier for the sculpt (default 1.0)
  --mesh-filter <text>         Only deform meshes whose name contains <text>
  --keep-morph-targets         Don't rotate/scale the head's existing character-creator morphs
  --replace-mesh               Also re-import V's .mesh (needs --head-mesh; not recommended - the
                               .morphtarget defines the shape and mesh re-import can break skinning)
  --keep-cc-shapes             Keep the character creator's eyes/nose/mouth/jaw/ear shapes on top of the
                               converted face (default: switched off, so V's face is the converted one)
  --no-face-parts              Leave eyes, brows, lashes, teeth and face decals as in the game (default:
                               they follow the sculpt and lose the character creator shapes too)
  --wolvenkit-morph-import     Re-import the .morphtarget with WolvenKit instead of patching the game
                               file directly (old method - loses the head's skinning)
  -v, --verbose                Echo extra details
  -h, --help                   Show this help
)";

struct Args {
    std::map<std::string, std::string> kv;
    std::map<std::string, bool> flags;
};

Args parseArgs(int argc, char** argv) {
    static const char* valued[] = {"--preset", "--head", "--head-mesh", "--wolvenkit", "--template-morphtarget",
                                   "--template-mesh", "--head-depot-path", "--mesh-depot-path", "--out", "--name",
                                   "--gender", "--mapping", "--data-dir", "--strength", "--mesh-filter", "--fo4-regions",
                                   "--archive-dest", "--fo4-dir", "--cp-game", "--cache-dir",
                                   "--cp-save", "--preset-out", "--base-preset", "--export-preset", "--fo4-save", "--fo4-save-out", "--export-model", "--model-head"};
    static const char* boolean[] = {"--keep-morph-targets", "--replace-mesh", "--zip-to-dest", "--wolvenkit-morph-import", "--keep-cc-shapes", "--no-face-parts", "--no-customization", "--efs", "--efs-redux", "--approximate-sculpt", "--no-archive-to-dest", "-v", "--verbose", "-h", "--help"};
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i], val;
        auto eq = s.find('=');
        if (s.rfind("--", 0) == 0 && eq != std::string::npos) { val = s.substr(eq + 1); s = s.substr(0, eq); }
        bool known = false;
        for (const char* v : valued)
            if (s == v) {
                known = true;
                if (val.empty()) {
                    if (i + 1 >= argc) throw std::runtime_error("missing value for " + s);
                    val = argv[++i];
                }
                a.kv[s] = val;
            }
        for (const char* b : boolean)
            if (s == b) { known = true; a.flags[s] = true; }
        if (!known) throw std::runtime_error("unknown argument: " + s + " (see --help)");
    }
    return a;
}

std::string get(const Args& a, const std::string& k, const std::string& def = "") {
    auto it = a.kv.find(k);
    return it == a.kv.end() ? def : it->second;
}

fs::path exeDir(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(argv0), ec);
    if (ec) p = fs::absolute(argv0);
    return p.parent_path();
}

} // namespace

int main(int argc, char** argv) {
    try {
        Args args = parseArgs(argc, argv);
        if (argc == 1 || args.flags.count("-h") || args.flags.count("--help")) {
            std::cout << kUsage;
            return argc == 1 ? 1 : 0;
        }
        Log::verbose = args.flags.count("-v") || args.flags.count("--verbose");

        if (!get(args, "--export-preset").empty()) {
            const fs::path in = get(args, "--preset");
            if (in.empty()) throw std::runtime_error("--export-preset needs --preset <save.fos>");
            Fo4SaveInfo si;
            LooksMenuPreset p = loadFo4Character(in, &si);
            for (const auto& w : p.parseWarnings) Log::warn(w);
            writeFileText(get(args, "--export-preset"), looksMenuPresetJson(p, si.weight));
            Log::info("Wrote " + get(args, "--export-preset") + " (" + std::to_string(p.regions.size()) + " sculpts, " +
                      std::to_string(p.presetWeights.size()) + " face presets)");
            return 0;
        }

        ConvertOptions o;
        o.cpSave = get(args, "--cp-save");
        o.cpToFo4 = !o.cpSave.empty();
        o.presetOut = get(args, "--preset-out");
        o.basePreset = get(args, "--base-preset");
        o.fo4SaveTarget = get(args, "--fo4-save");
        o.fo4SaveOut = get(args, "--fo4-save-out");
        o.preset = get(args, "--preset");
        o.headGlb = get(args, "--head");
        o.fo4Dir = get(args, "--fo4-dir");
        o.cpGameDir = get(args, "--cp-game");
        o.cacheDir = get(args, "--cache-dir");
        if (o.preset.empty() && o.cpSave.empty()) throw std::runtime_error("--preset or --cp-save is required (see --help)");
        if (o.headGlb.empty() && o.cpGameDir.empty()) throw std::runtime_error("give --head or --cp-game (see --help)");
        o.headMeshGlb = get(args, "--head-mesh");
        o.wolvenkit = get(args, "--wolvenkit");
        o.templateMorphtarget = get(args, "--template-morphtarget");
        o.templateMesh = get(args, "--template-mesh");
        o.headDepotPath = get(args, "--head-depot-path");
        o.meshDepotPath = get(args, "--mesh-depot-path");
        o.fo4Regions = get(args, "--fo4-regions");
        o.workDir = get(args, "--out", "PresetToolKit_out");
        o.archiveDest = get(args, "--archive-dest");
        o.dataDir = get(args, "--data-dir");
        if (o.dataDir.empty()) o.dataDir = defaultDataDir(exeDir(argv[0]));
        o.mapping = get(args, "--mapping");
        o.modName = get(args, "--name");
        o.gender = get(args, "--gender");
        o.meshFilter = get(args, "--mesh-filter");
        o.strength = std::stod(get(args, "--strength", "1.0"));
        o.keepMorphTargets = args.flags.count("--keep-morph-targets") > 0;
        o.replaceMesh = args.flags.count("--replace-mesh") > 0;
        o.wolvenkitMorphImport = args.flags.count("--wolvenkit-morph-import") > 0;
        o.keepCharacterCreatorShapes = args.flags.count("--keep-cc-shapes") > 0;
        o.companions = args.flags.count("--no-face-parts") == 0;
        o.approximateCustomization = args.flags.count("--no-customization") == 0;
        o.extendedSculpting = args.flags.count("--efs") != 0;
        o.extendedSculptingRedux = args.flags.count("--efs-redux") != 0;
        o.approximateSculpt = args.flags.count("--approximate-sculpt") > 0;
        o.zipToDest = args.flags.count("--zip-to-dest") > 0;
        o.archiveToDest = args.flags.count("--no-archive-to-dest") == 0;
        if (o.zipToDest && o.archiveDest.empty() && o.presetOut.empty())
            throw std::runtime_error("--zip-to-dest needs --archive-dest (or --preset-out)");

        if (!get(args, "--export-model").empty() || !get(args, "--model-head").empty()) {
            ModelExportResult mr = exportFaceModel(o, get(args, "--model-head", "cp"), get(args, "--export-model"));
            std::cout << "\n" << mr.report << "\n";
            return 0;
        }

        ConvertResult r = runConversion(o);
        std::cout << "\n" << r.report << "\n";
        if (r.exitCode == 0 && !o.cpToFo4) Log::info("Install the .archive (or the zip) into Cyberpunk 2077\\archive\\pc\\mod.");
        return r.exitCode;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
