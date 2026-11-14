#include "ArchivePackager.h"
#include "Cr2w.h"

#include "Util.h"
#include "Zip.h"

#include <sstream>
#include <stdexcept>

namespace ptk {

namespace {

std::string fillCommand(std::string tpl, const fs::path& wk, const fs::path& path, const fs::path& outdir) {
    tpl = replaceAll(tpl, "{wolvenkit}", wk.u8string());
    tpl = replaceAll(tpl, "{path}", path.u8string());
    tpl = replaceAll(tpl, "{outdir}", outdir.u8string());
    return tpl;
}

std::string commandOr(const Mapping& m, const std::string& key, const std::string& def) {
    auto it = m.commands.find(key);
    return it != m.commands.end() ? it->second : def;
}

void copyFile(const fs::path& from, const fs::path& to) {
    copyFileReplace(from, to);
}

fs::path findFirstWithExt(const fs::path& dir, const std::string& ext) {
    if (!fs::exists(longPath(dir))) return {};
    for (auto& e : fs::recursive_directory_iterator(longPath(dir)))
        if (e.is_regular_file() && toLower(e.path().extension().u8string()) == ext) return e.path();
    return {};
}

// WolvenKit pairs raw\...\<name>.glb with archive\...\<file>; how <name> is formed is configurable.
std::string glbNameFor(const fs::path& rel, const Mapping& m) {
    const std::string mode = toLower(m.glbNaming);
    if (mode == "stem") return rel.stem().u8string() + ".glb";
    if (mode == "withextension") return rel.filename().u8string() + ".glb";
    // auto (matches WolvenKit's exports): x.mesh -> x.glb, x.morphtarget -> x.morphtarget.glb
    if (toLower(rel.extension().u8string()) == ".mesh") return rel.stem().u8string() + ".glb";
    return rel.filename().u8string() + ".glb";
}

} // namespace

PackageResult packageHeadReplacement(const Mapping& mapping, const PackageOptions& opt) {
    PackageResult res;
    if (opt.files.empty()) throw std::runtime_error("nothing to package");

    // ------------------------------------------------ WolvenKit-style project (always written)
    res.projectDir = opt.outDir / "project";      // kept short: Windows paths are limited
    const fs::path src = res.projectDir / "source";
    fs::remove_all(longPath(src));
    for (const auto& f : opt.files) {
        const fs::path rel = depotToFs(toDepotPath(f.depotPath));
        saveGlb(*f.glb, src / "raw" / rel.parent_path() / glbNameFor(rel, mapping));
        if (!f.templateFile.empty()) copyFile(f.templateFile, src / "archive" / rel);
    }

    // ------------------------------------------------ automated WolvenKit build
    bool haveTemplates = true;
    for (const auto& f : opt.files) haveTemplates &= !f.templateFile.empty();

    if (opt.wolvenkit.empty()) {
        res.notes.push_back("No --wolvenkit given: staged a WolvenKit project instead of building the .archive.");
    } else if (!haveTemplates) {
        res.notes.push_back("WolvenKit can only import a .glb over an existing game file; pass the vanilla "
                            "file(s) with --template-morphtarget / --template-mesh.");
    } else {
        const fs::path work = opt.outDir / "w";
        fs::remove_all(longPath(work));
        const fs::path packRoot = work / "pack" / opt.modName;
        bool ok = true;
        Kraken kraken;
        bool krakenTried = false;

        for (const auto& [file, depot] : opt.preparedFiles) {
            const fs::path rel = depotToFs(toDepotPath(depot));
            copyFile(file, packRoot / rel);
            copyFile(file, src / "archive" / rel);
        }

        for (size_t i = 0; i < opt.files.size() && ok; ++i) {
            const auto& f = opt.files[i];
            const fs::path rel = depotToFs(toDepotPath(f.depotPath));
            const fs::path impRoot = work / ("i" + std::to_string(i));
            const fs::path target = impRoot / rel;
            const bool isMorphFile = toLower(rel.extension().u8string()) == ".morphtarget";
            if (isMorphFile && opt.patchMorphtargets) {
                if (!krakenTried) {
                    krakenTried = true;
                    if (kraken.load(opt.oodleDirs)) Log::info("Oodle            " + kraken.source());
                }
                Log::info("Patching         " + rel.filename().u8string() + " (vertex positions only)");
                MorphtargetPatch mp;
                mp.glb = f.glb;
                mp.neutralizeCharacterCreator = opt.neutralizeCharacterCreator;
                MorphtargetPatchStats ps = patchMorphtarget(f.templateFile, target, kraken, mp);
                std::string line = rel.filename().u8string() + ": " + std::to_string(ps.moved) + " of " +
                                   std::to_string(ps.vertices) + " vertices written into the game file" +
                                   (ps.neutralized ? ", " + std::to_string(ps.neutralized) + " character creator shapes off" : std::string()) +
                                   (ps.storedRaw ? " (stored uncompressed)" : " (Oodle-compressed)");
                Log::info("                 " + line);
                res.report.push_back(line);
                if (ps.clamped)
                    res.notes.push_back(std::to_string(ps.clamped) +
                                        " vertices were outside the head's quantization box and were clamped");
                copyFile(target, packRoot / rel);
                copyFile(target, src / "archive" / rel);
                continue;
            }
            copyFile(f.templateFile, target);
            if (isMorphFile && mapping.stripSkinForMorphImport) {
                // WolvenKit.CLI's import loads no game archives, so it cannot find the morphtarget's base
                // mesh and then refuses a GLB that carries a skeleton ("import bones into a morphtarget that
                // doesn't have them"). Without the skin it keeps each vertex's bone indices as they are -
                // which, for a GLB exported from this very head, already are the head's own bone order.
                Glb noSkin = *f.glb;
                noSkin.doc.erase("skins");
                if (noSkin.doc.contains("nodes"))
                    for (auto& n : noSkin.doc["nodes"]) n.erase("skin");
                saveGlb(noSkin, target.parent_path() / glbNameFor(rel, mapping));
            } else {
                saveGlb(*f.glb, target.parent_path() / glbNameFor(rel, mapping));
            }
            auto before = readFileBytes(target);

            std::string cmd = fillCommand(commandOr(mapping, "import", "\"{wolvenkit}\" import -p \"{path}\" -k"),
                                          opt.wolvenkit, impRoot, impRoot);
            // WolvenKit.CLI picks its glTF importer from configuration, not from the file type:
            // tell it explicitly (env vars override its appsettings.json for this process only).
            const bool isMorph = toLower(rel.extension().u8string()) == ".morphtarget";
            int rc = runCommand(cmd, true, {{"GltfImportArgs__ImportFormat", isMorph ? "Morphtarget" : "Mesh"}});
            const bool changed = readFileBytes(target) != before;
            if (!changed) {
                ok = false;
                res.notes.push_back("WolvenKit import of " + rel.filename().u8string() + " did not update the file (exit " +
                                    std::to_string(rc) + ") - see WolvenKit's messages in log.txt");
            } else {
                if (rc != 0)
                    res.notes.push_back("WolvenKit import of " + rel.filename().u8string() + " reported exit code " +
                                        std::to_string(rc) + " but the file was updated - continuing (see log.txt)");
                Log::info("Imported         " + rel.filename().u8string());
                copyFile(target, packRoot / rel);
            }
        }

        if (ok) {
            const fs::path packed = work / "packed";
            fs::create_directories(packed);
            std::string cmd = fillCommand(commandOr(mapping, "pack", "\"{wolvenkit}\" pack -p \"{path}\" -o \"{outdir}\""),
                                          opt.wolvenkit, packRoot, packed);
            int rc = runCommand(cmd, true);
            fs::path archive = findFirstWithExt(packed, ".archive");
            if (archive.empty()) archive = findFirstWithExt(work / "pack", ".archive");
            if (archive.empty()) {
                res.notes.push_back("WolvenKit pack did not produce an .archive (exit " + std::to_string(rc) + ") - see log.txt");
            } else {
                const fs::path dist = opt.outDir / "dist";
                fs::remove_all(longPath(dist));
                copyFile(archive, dist / "archive" / "pc" / "mod" / (opt.modName + ".archive"));
                res.zipPath = opt.outDir / (opt.modName + ".zip");
                zipDirectory(dist, res.zipPath);
                res.complete = true;
            }
        }
    }

    if (!res.complete) {
        std::ostringstream m;
        m << "Manual steps to finish \"" << opt.modName << "\" in WolvenKit (GUI):\n\n"
          << "1. Create a WolvenKit project named " << opt.modName << " and copy the 'source' folder from\n   "
          << res.projectDir.u8string() << "\n   over the project's 'source' folder.\n"
          << "2. Make sure these vanilla files are in the project (Asset Browser > Add to project):\n";
        for (const auto& f : opt.files) m << "     " << toDepotPath(f.depotPath) << "\n";
        m << "3. Import tool: import each .glb in source\\raw over its file"
          << (opt.files.size() > 1 ? " (the .mesh first, then the .morphtarget)" : "") << ".\n"
          << "4. Pack/Install the project. The result is a plain .archive in archive\\pc\\mod.\n";
        writeFileText(opt.outDir / "MANUAL_STEPS.txt", m.str());
        res.notes.push_back("See " + (opt.outDir / "MANUAL_STEPS.txt").u8string());
    }
    return res;
}

} // namespace ptk
