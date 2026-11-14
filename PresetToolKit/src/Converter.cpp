#include "Converter.h"

#include "ArchivePackager.h"
#include "Calibrator.h"
#include "Fo4Data.h"
#include "GameSaves.h"
#include "PresetFit.h"
#include "Fo4FaceFit.h"
#include "Fo4Look.h"
#include "Tri.h"
#include "Zip.h"
#include "Cr2w.h"
#include "Nif.h"
#include "Gltf.h"
#include "HeadDeformer.h"
#include "LooksMenuPreset.h"
#include "Mapping.h"
#include "ModelExport.h"
#include "Util.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace ptk {

std::string u8(const fs::path& p) { return p.u8string(); }

fs::path defaultDataDir(const fs::path& exeDir) {
    fs::path d = exeDir / "data";
    if (!fs::exists(d / "default_mapping.json") && fs::exists(fs::path("data") / "default_mapping.json")) d = "data";
    return d;
}

namespace {

// "...\source\archive\base\characters\...\x.morphtarget" -> "base\characters\...\x.morphtarget"
std::string depotFromProjectPath(const fs::path& p) {
    std::string g = fs::absolute(p).generic_u8string();
    std::string lower = toLower(g);
    for (const char* marker : {"/source/archive/", "/archive/"}) {
        auto pos = lower.rfind(marker);
        if (pos != std::string::npos) return toDepotPath(g.substr(pos + std::string(marker).size()));
    }
    return {};
}

std::string formatReport(const Mapping& mapping, const LooksMenuPreset& preset, const SculptInput& in, const HeadFrame& f,
                         const std::vector<std::pair<std::string, DeformResult>>& results,
                         const std::vector<std::string>& frameWarnings) {
    std::ostringstream o;
    o << "LooksMenu preset : " << preset.name << "\n";
    o << "Gender / race    : " << (preset.gender == 1 ? "female" : preset.gender == 0 ? "male" : "unknown") << " / "
      << (preset.race.empty() ? "?" : preset.race) << "\n";
    o << "Input            : " << preset.regions.size() << " region sculpts, " << preset.morphValues.size()
      << " slider values, " << preset.presetWeights.size() << " face presets\n";
    o << "Head frame       : right=" << axisName(f.right) << " up=" << axisName(f.up) << " forward=" << axisName(f.forward) << "\n\n";
    if (!in.boneSculpts.empty()) {
        o << "Sculpts applied through Fallout 4 bones (HumanRaceFacialBoneRegions):\n";
        for (const auto& b : in.boneSculpts) {
            o << "  " << toHex(b.id) << "  " << b.name << "  [";
            for (size_t i = 0; i < b.values.size() && i < 7; ++i) o << (i ? ", " : "") << b.values[i];
            o << "]\n";
        }
        o << "\n";
    }
    if (!in.unknownBones.empty()) {
        o << "Bones with no location in the mapping (skipped - add them to fo4Bones.locations):\n";
        for (const auto& b : in.unknownBones) o << "  " << b << "\n";
        o << "\n";
    }
    if (!in.regions.empty()) o << "Sculpts applied through coarse face regions:\n";
    for (const auto& [name, a] : in.regions) {
        o << "  " << name << " (" << a.count << " ids)  T=(" << a.translate.x << ", " << a.translate.y << ", " << a.translate.z
          << ")  R=(" << a.rotate.x << ", " << a.rotate.y << ", " << a.rotate.z << ")  S=" << a.scale << "\n";
        for (uint32_t id : a.sourceIds) {
            std::string n = mapping.fo4NameOf(id);
            o << "      " << toHex(id) << (n.empty() ? "" : "  " + n) << "\n";
        }
    }
    for (const auto& [file, r] : results) {
        o << "\n" << file << ":\n  " << r.verticesMoved << " of " << r.vertexCount << " vertices moved, max "
          << r.maxDisplacement * 1000.0 << " mm, " << r.morphTargetsAdjusted << " CC morph deltas adjusted\n";
        for (const auto& [reg, mx] : r.maxPerRegion) o << "    " << reg << ": up to " << mx * 1000.0 << " mm\n";
    }
    auto list = [&](const char* title, const std::vector<uint32_t>& ids) {
        if (ids.empty()) return;
        o << "\n" << title << " (" << ids.size() << "):\n";
        for (uint32_t id : ids) {
            std::string n = mapping.fo4NameOf(id);
            o << "  " << toHex(id) << (n.empty() ? "" : "  " + n) << "\n";
        }
    };
    list(mapping.fo4RegionNames.empty()
             ? "Unmapped region ids - add the FO4 region file, or idOverrides in the mapping"
             : "Unmapped region ids - add nameRules/idOverrides in the mapping", in.unmappedRegionIds);
    list("Unmapped slider (Values) ids - add valueRules in the mapping", in.unmappedValueIds);
    list("Face presets (Morphs.Presets) ignored - they reference FO4 facegen data", in.ignoredPresets);
    std::vector<std::string> warns = preset.parseWarnings;
    warns.insert(warns.end(), frameWarnings.begin(), frameWarnings.end());
    for (const auto& [file, r] : results) warns.insert(warns.end(), r.warnings.begin(), r.warnings.end());
    if (!warns.empty()) {
        o << "\nWarnings:\n";
        for (const auto& w : warns) o << "  " << w << "\n";
    }
    return o.str();
}


std::string fillCmd(std::string tpl, const std::map<std::string, std::string>& kv) {
    for (const auto& [k, v] : kv) tpl = replaceAll(tpl, "{" + k + "}", v);
    return tpl;
}

struct ExtractedHead { fs::path morphGlb, morphFile, meshGlb, meshFile; };

// Pulls V's head (.morphtarget + .mesh, plus GLB exports) out of Cyberpunk's own .archive files with
// WolvenKit.CLI "uncook". Results are cached, so the (slow) archive scan happens once per gender.
ExtractedHead extractHeadFromGame(const ConvertOptions& opt, const Mapping& mapping, const std::string& gender,
                                  const std::string& morphDepot, const std::string& meshDepot) {
    if (opt.wolvenkit.empty())
        throw std::runtime_error("reading the head from Cyberpunk's archives needs WolvenKit.CLI - set its path");
    fs::path archives = opt.cpGameDir / "archive" / "pc" / "content";
    std::error_code ec;
    if (!fs::is_directory(archives, ec))
        throw std::runtime_error("not a Cyberpunk 2077 folder (no archive\\pc\\content): " + u8(opt.cpGameDir));
    fs::path cache = (opt.cacheDir.empty() ? opt.workDir / "cache" : opt.cacheDir) / "cyberpunk" / gender;
    const fs::path morphName = depotToFs(morphDepot).filename(), meshName = depotToFs(meshDepot).filename();

    // Resolve by the exact game path: the wildcard can also match same-named NPC files
    // (e.g. head\\pwa\\h0_000_pwa_c__basehead\\h0_000_pwa_c__basehead.mesh).
    const fs::path morphRel = depotToFs(morphDepot), meshRel = depotToFs(meshDepot);
    auto firstExisting = [&](const std::vector<fs::path>& cands) {
        std::error_code e2;
        for (const auto& c : cands)
            if (fs::is_regular_file(longPath(c), e2)) return c;
        return fs::path();
    };
    auto lookup = [&]() {
        ExtractedHead h;
        h.morphFile = firstExisting({cache / "archive" / morphRel});
        h.morphGlb = firstExisting({cache / "raw" / morphRel.parent_path() / (morphName.u8string() + ".glb"),
                                    cache / "raw" / morphRel.parent_path() / (morphName.stem().u8string() + ".glb"),
                                    cache / "archive" / morphRel.parent_path() / (morphName.u8string() + ".glb")});
        h.meshFile = firstExisting({cache / "archive" / meshRel});
        h.meshGlb = firstExisting({cache / "raw" / meshRel.parent_path() / (meshName.stem().u8string() + ".glb"),
                                   cache / "raw" / meshRel.parent_path() / (meshName.u8string() + ".glb"),
                                   cache / "archive" / meshRel.parent_path() / (meshName.stem().u8string() + ".glb")});
        return h;
    };
    ExtractedHead h = lookup();
    if (!h.morphGlb.empty() && !h.morphFile.empty()) {
        Log::info("Using cached head from " + u8(cache));
        return h;
    }
    Log::info("Extracting V's head from " + u8(archives) + " (first time only, this can take a few minutes)...");
    auto it = mapping.commands.find("uncook");
    const std::string tpl = it != mapping.commands.end()
        ? it->second
        : "\"{wolvenkit}\" uncook -p \"{path}\" -o \"{outdir}\" -or \"{rawdir}\" -w \"{pattern}\"";
    fs::create_directories(cache / "archive");
    fs::create_directories(cache / "raw");
    for (const fs::path& name : {morphName}) {
        std::string cmd = fillCmd(tpl, {{"wolvenkit", u8(opt.wolvenkit)}, {"path", u8(archives)},
                                        {"outdir", u8(cache / "archive")}, {"rawdir", u8(cache / "raw")},
                                        {"pattern", "*" + name.u8string()}});
        int rc = runCommand(cmd, true);
        if (rc != 0) Log::warn("WolvenKit uncook returned " + std::to_string(rc));
    }
    h = lookup();
    if (h.morphGlb.empty() || h.morphFile.empty())
        throw std::runtime_error("WolvenKit did not produce " + morphDepot + " and its GLB in " + u8(cache) +
                                 " - check the \"uncook\" command in the mapping, or export the head manually");
    return h;
}

// The other morph targets of V's head folder (eyes, brows, lashes, teeth, makeup decals ...):
// WolvenKit.CLI "unbundle" with a regex, cached. Returns (file, depot path) without the head itself.
std::vector<std::pair<fs::path, std::string>> extractCompanionMorphtargets(const ConvertOptions& opt, const Mapping& mapping,
                                                                           const std::string& gender,
                                                                           const std::string& morphDepot) {
    std::vector<std::pair<fs::path, std::string>> out;
    if (opt.wolvenkit.empty() || opt.cpGameDir.empty()) return out;
    const fs::path archives = opt.cpGameDir / "archive" / "pc" / "content";
    const fs::path cache = (opt.cacheDir.empty() ? opt.workDir / "cache" : opt.cacheDir) / "cyberpunk" / gender / "companions";
    const fs::path headRel = depotToFs(morphDepot);
    const fs::path folderRel = headRel.parent_path();
    const fs::path marker = cache / ".extracted";
    std::error_code ec;
    if (!fs::exists(marker, ec)) {
        Log::info("Extracting the rest of V's face (eyes, brows, lashes, teeth, decals) - first time only...");
        auto it = mapping.commands.find("unbundle");
        const std::string tpl = it != mapping.commands.end()
            ? it->second
            : "\"{wolvenkit}\" unbundle -p \"{path}\" -o \"{outdir}\" -r \"{regex}\"";
        fs::create_directories(cache);
        // '.' instead of a path separator keeps the regex free of backslashes
        const std::string regex = folderRel.filename().u8string() + ".[a-z0-9_]+\\.morphtarget$";
        const std::string cmd = fillCmd(tpl, {{"wolvenkit", u8(opt.wolvenkit)}, {"path", u8(archives)},
                                              {"outdir", u8(cache)}, {"regex", regex}});
        const int rc = runCommand(cmd, true);
        if (rc != 0) Log::warn("WolvenKit unbundle returned " + std::to_string(rc));
        writeFileText(marker, "done\n");
    }
    const fs::path dir = cache / folderRel;
    if (!fs::is_directory(longPath(dir), ec)) return out;
    for (const auto& e : fs::directory_iterator(longPath(dir), ec)) {
        if (!e.is_regular_file()) continue;
        const fs::path name = e.path().filename();
        if (toLower(name.extension().u8string()) != ".morphtarget") continue;
        if (toLower(name.u8string()) == toLower(headRel.filename().u8string())) continue;
        out.push_back({dir / name, (folderRel / name).generic_u8string()});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Smooth displacement field from the head's sculpt: the moved head vertices near a point carry it
// along (Gaussian falloff), so eyeballs, lashes, brows, teeth and skin decals follow the face.
struct SculptField {
    std::vector<Vec3> base, delta;
    std::unordered_map<long long, std::vector<int>> grid;
    double cell = 0.01, sigma = 0.006, radius = 0.02;
    static long long key(long long x, long long y, long long z) { return ((x + 100000) * 200003LL + (y + 100000)) * 200003LL + (z + 100000); }
    SculptField(const std::vector<Vec3>& b, const std::vector<Vec3>& moved) : base(b) {
        delta.resize(b.size());
        for (size_t i = 0; i < b.size(); ++i) {
            delta[i] = moved[i] - b[i];
            if (length(delta[i]) < 1e-7) continue;
            grid[key((long long)std::floor(b[i].x / cell), (long long)std::floor(b[i].y / cell), (long long)std::floor(b[i].z / cell))].push_back(int(i));
        }
    }
    Vec3 operator()(const Vec3& q) const {
        const long long cx = (long long)std::floor(q.x / cell), cy = (long long)std::floor(q.y / cell), cz = (long long)std::floor(q.z / cell);
        double ws = 0;
        Vec3 d;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dz = -2; dz <= 2; ++dz) {
                    auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (int i : it->second) {
                        const double r = length(base[i] - q);
                        if (r > radius) continue;
                        const double w = std::exp(-(r * r) / (sigma * sigma));
                        ws += w;
                        d += delta[i] * w;
                    }
                }
        if (ws < 1e-9) return Vec3();
        // fade out where only a few far-away moved vertices contribute
        const double conf = std::min(1.0, ws / 0.5);
        return d * (conf / ws);
    }
};

// Writes new positions into every POSITION accessor (same order as read) and updates the normals by
// the change of the triangle normals, so hard edges / seams in the original normals survive.
void setHeadPositions(Glb& g, const std::vector<Vec3>& newPos) {
    size_t base = 0;
    const nlohmann::json meshes = g.doc.value("meshes", nlohmann::json::array());
    for (const auto& m : meshes) {
        const nlohmann::json prims = m.value("primitives", nlohmann::json::array());
        for (const auto& p : prims) {
            if (!p["attributes"].contains("POSITION")) continue;
            int c = 3;
            const int pa = p["attributes"]["POSITION"].get<int>();
            std::vector<float> P = readFloatAccessor(g, pa, c);
            const size_t n = P.size() / 3;
            std::vector<float> Q(P.size());
            for (size_t i = 0; i < n && base + i < newPos.size(); ++i)
                for (int k = 0; k < 3; ++k) Q[i * 3 + k] = float(newPos[base + i][k]);
            if (p["attributes"].contains("NORMAL") && p.contains("indices")) {
                const int na = p["attributes"]["NORMAL"].get<int>();
                std::vector<float> N = readFloatAccessor(g, na, c);
                const std::vector<uint32_t> idxf = readIndexAccessor(g, p["indices"].get<int>());
                auto vnormals = [&](const std::vector<float>& X) {
                    std::vector<Vec3> vn(n);
                    for (size_t t = 0; t + 2 < idxf.size(); t += 3) {
                        const size_t a = size_t(idxf[t]), b = size_t(idxf[t + 1]), cc = size_t(idxf[t + 2]);
                        if (a >= n || b >= n || cc >= n) continue;
                        const Vec3 A(X[a * 3], X[a * 3 + 1], X[a * 3 + 2]), B(X[b * 3], X[b * 3 + 1], X[b * 3 + 2]),
                            C(X[cc * 3], X[cc * 3 + 1], X[cc * 3 + 2]);
                        const Vec3 fn = cross(B - A, C - A);
                        vn[a] += fn; vn[b] += fn; vn[cc] += fn;
                    }
                    for (auto& v : vn) { const double l = length(v); if (l > 1e-12) v = v * (1.0 / l); }
                    return vn;
                };
                const auto n0 = vnormals(P), n1 = vnormals(Q);
                for (size_t i = 0; i < n; ++i) {
                    Vec3 v(N[i * 3], N[i * 3 + 1], N[i * 3 + 2]);
                    v += n1[i] - n0[i];
                    const double l = length(v);
                    if (l > 1e-9) for (int k = 0; k < 3; ++k) N[i * 3 + k] = float(v[k] / l);
                }
                writeFloatAccessor(g, na, N);
            }
            writeFloatAccessor(g, pa, Q);
            base += n;
        }
    }
}

// Gives the Fallout 4 head model its face presets / sliders (chargen .tri + HumanRace tables).
bool attachFaceMorphs(Fo4FaceModel& model, const std::shared_ptr<TriFile>& tri, const fs::path& dataDir, const std::string& gender) {
    if (!tri) return false;
    const Fo4Chargen cg = loadFo4Chargen(dataDir / "fo4_chargen.json");
    auto it = cg.faceMorphs.find(gender);
    if (it == cg.faceMorphs.end() || it->second.presets.empty()) return false;
    model.setFaceMorphs(*tri, it->second.presets, it->second.sliders);
    return model.hasFaceMorphs();
}

const std::string kCharAssets = "Meshes\\Actors\\Character\\CharacterAssets\\";

} // namespace

namespace {

// Everything both directions need: the mapping with Fallout 4's region/bone data for the gender,
// V's head (from the game via WolvenKit, or a given GLB) and its frame, bones calibrated against it.
struct HeadSetup {
    Mapping mapping;
    std::string gender;
    std::string morphDepot, meshDepot;
    fs::path headGlbPath, meshGlbPath, tplMorph, tplMesh;
    Glb headGlb;
    std::optional<Glb> meshGlb;
    HeadFrame frame;
    // Fallout 4's head + face skeleton and their alignment to V (when the FO4 folder is set)
    std::shared_ptr<NifFile> fo4Head, fo4Skeleton;
    std::optional<CalibrationResult> calibration;
    std::shared_ptr<TriFile> fo4ChargenTri;       // face presets / sliders morphs
    std::vector<std::string> frameWarnings;
};

HeadSetup setupHead(const ConvertOptions& opt, const fs::path& mappingPath, const fs::path& dataDir,
                    std::string gender, const fs::path& extraRegionsDir, bool allowMesh) {
    HeadSetup hs;
    Log::info("Reading mapping  " + u8(mappingPath));
    hs.mapping = loadMapping(mappingPath);
    Mapping& mapping = hs.mapping;
    if (gender != "female" && gender != "male") throw std::runtime_error("gender must be female or male");
    hs.gender = gender;
    const std::string sexName = gender == "male" ? "Male" : "Female";

    // ---------------------------------------------------------------- Fallout 4 install (optional)
    std::optional<Fo4Data> fo4;
    if (!opt.fo4Dir.empty()) {
        fo4.emplace(opt.fo4Dir);
        if (!fo4->valid()) throw std::runtime_error("Fallout 4 folder not found: " + u8(opt.fo4Dir));
        Log::info("Fallout 4 data   " + u8(fo4->dataDir()) + " (" + std::to_string(fo4->archiveCount()) + " general .ba2 archives)");
    }

    // Region file: explicit, from the FO4 install, next to the input, or bundled
    fs::path regionsFile = opt.fo4Regions;
    const std::string regionsName = "HumanRaceFacialBoneRegions" + sexName + ".txt";
    if (regionsFile.empty() && (opt.extendedSculpting || opt.extendedSculptingRedux)) {
        // Extended Facial Sculpting (or its REDUX): its region files (the vanilla regions plus its own) are bundled
        const std::string folder = opt.extendedSculptingRedux ? "efs_redux" : "efs";
        const char* modName = opt.extendedSculptingRedux ? "Extended Facial Sculpting REDUX" : "Extended Facial Sculpting";
        if (fs::exists(dataDir / folder / regionsName)) {
            regionsFile = dataDir / folder / regionsName;
            Log::info(std::string("Regions from     ") + modName + " (" + u8(regionsFile) + ")");
        } else {
            Log::warn(std::string(modName) + " region file missing: " + u8(dataDir / folder / regionsName));
        }
    }
    if (regionsFile.empty() && fo4) {
        std::string src;
        if (auto bytes = fo4->read(kCharAssets + regionsName, &src)) {
            regionsFile = opt.workDir / "fo4" / regionsName;
            writeFileBytes(regionsFile, *bytes);
            Log::info("Regions from     " + src);
        }
    }
    if (regionsFile.empty())
        for (const fs::path& dir : {extraRegionsDir, dataDir})
            if (regionsFile.empty() && !dir.empty() && fs::exists(dir / regionsName)) regionsFile = dir / regionsName;
    if (!regionsFile.empty()) {
        Log::info("Reading regions  " + u8(regionsFile));
        mapping.fo4Regions = loadFo4Regions(regionsFile);
        mapping.fo4RegionNames = loadFo4RegionNames(regionsFile);
        for (const auto& [id, r] : mapping.fo4Regions)
            if (!r.name.empty()) mapping.fo4RegionNames[id] = r.name;
        Log::info("                 " + std::to_string(mapping.fo4RegionNames.size()) + " sculpt regions, " +
                  std::to_string(mapping.fo4Regions.size()) + " with bone data");
        if (mapping.fo4RegionNames.empty())
            Log::warn("no region IDs/names found in " + u8(regionsFile) + " - is it the right file?");
    } else {
        Log::warn("no HumanRaceFacialBoneRegions file found - vanilla sculpts will not be recognised");
    }

    // ---------------------------------------------------------------- files that get replaced
    for (const auto& t : {opt.templateMorphtarget, opt.templateMesh})
        if (!t.empty() && !fs::exists(t)) throw std::runtime_error("file not found: " + u8(t));
    std::string morphDepot = opt.headDepotPath;
    if (morphDepot.empty() && !opt.templateMorphtarget.empty()) morphDepot = depotFromProjectPath(opt.templateMorphtarget);
    if (morphDepot.empty()) {
        auto it = mapping.headDepotPath.find(gender);
        if (it == mapping.headDepotPath.end())
            throw std::runtime_error("no head depot path: set one, or headDepotPath." + gender + " in the mapping");
        morphDepot = it->second;
    }
    morphDepot = toDepotPath(morphDepot);
    std::string meshDepot = opt.meshDepotPath;
    if (meshDepot.empty() && !opt.templateMesh.empty()) meshDepot = depotFromProjectPath(opt.templateMesh);
    if (meshDepot.empty())
        if (auto it = mapping.headMeshDepotPath.find(gender); it != mapping.headMeshDepotPath.end()) meshDepot = it->second;
    if (meshDepot.empty()) meshDepot = fs::path(morphDepot).replace_extension(".mesh").generic_u8string();
    meshDepot = toDepotPath(meshDepot);
    hs.morphDepot = morphDepot;
    hs.meshDepot = meshDepot;

    // ---------------------------------------------------------------- V's head: given, or from the game
    fs::path headGlbPath = opt.headGlb, meshGlbPath = opt.headMeshGlb;
    fs::path tplMorph = opt.templateMorphtarget, tplMesh = opt.templateMesh;
    if (headGlbPath.empty()) {
        if (opt.cpGameDir.empty())
            throw std::runtime_error("select V's head GLB, or the Cyberpunk 2077 folder so it can be read from the game");
        ExtractedHead h = extractHeadFromGame(opt, mapping, gender, morphDepot, meshDepot);
        headGlbPath = h.morphGlb;
        if (tplMorph.empty()) tplMorph = h.morphFile;
        // The .morphtarget alone defines V's head shape; the .mesh is left vanilla (see ConvertOptions).
    }
    if (!meshGlbPath.empty() && (!opt.replaceMesh || !allowMesh)) {
        if (allowMesh) {
            Log::info("Head .mesh GLB given but not replaced (the .morphtarget defines the shape; re-importing the");
            Log::info("                 .mesh can corrupt its bone bind matrices). Enable 'replace mesh' to force it.");
        }
        meshGlbPath.clear();
    }
    Log::info("Reading head     " + u8(headGlbPath));
    hs.headGlb = loadGlb(headGlbPath);
    if (!meshGlbPath.empty()) {
        Log::info("Reading mesh     " + u8(meshGlbPath));
        hs.meshGlb = loadGlb(meshGlbPath);
    }
    hs.headGlbPath = headGlbPath;
    hs.meshGlbPath = meshGlbPath;
    hs.tplMorph = tplMorph;
    hs.tplMesh = tplMesh;

    // The .morphtarget defines V's in-game shape, so it is the reference for the head frame.
    hs.frame = computeHeadFrame(hs.headGlb, mapping, opt.meshFilter, hs.frameWarnings);

    // ---------------------------------------------------------------- bone calibration
    mapping.selectGender(gender);
    if (fo4) {
        const std::string headNif = kCharAssets + "Base" + sexName + "Head_faceBones.nif";
        const std::string skelNif = kCharAssets + (gender == "male" ? "skeleton_faceBones.nif" : "skeleton_female_faceBones.nif");
        std::string srcHead, srcSkel;
        auto hb = fo4->read(headNif, &srcHead);
        auto sb = fo4->read(skelNif, &srcSkel);
        if (hb && sb) {
            try {
                hs.fo4Head = std::make_shared<NifFile>(*hb, headNif);
                hs.fo4Skeleton = std::make_shared<NifFile>(*sb, skelNif);
                const NifFile& headN = *hs.fo4Head;
                const NifFile& skelN = *hs.fo4Skeleton;
                CalibrationResult cal = calibrateBones(headN, skelN, hs.headGlb, hs.frame);
                hs.calibration = cal;
                const std::string triPath = kCharAssets + (gender == "male" ? "BaseMaleHeadCharGen.tri" : "BaseFemaleHeadChargen.tri");
                std::string srcTri;
                if (auto tb = fo4->read(triPath, &srcTri)) {
                    try {
                        hs.fo4ChargenTri = std::make_shared<TriFile>(loadTri(*tb));
                        Log::info("Face presets     " + std::to_string(hs.fo4ChargenTri->morphs.size()) + " morphs from " + srcTri);
                    } catch (const std::exception& e) {
                        Log::warn(std::string("could not read ") + triPath + ": " + e.what());
                    }
                } else {
                    Log::warn("no " + triPath + " in the Fallout 4 data - face presets / sliders are not converted");
                }
                mapping.boneSets[gender] = cal.bones;
                mapping.selectGender(gender);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%zu bones, %d landmarks, fit error %.1f mm avg / %.1f mm max",
                              cal.bones.locations.size(), cal.landmarks, cal.meanErrorMm, cal.maxErrorMm);
                Log::info("Calibrated       " + std::string(buf));
                Log::info("                 from " + srcHead);
                hs.frameWarnings.push_back(std::string("bones calibrated from your Fallout 4 files (") + srcHead + "): " + buf);
            } catch (const std::exception& e) {
                Log::warn(std::string("calibration from the Fallout 4 files failed, using the built-in calibration: ") + e.what());
            }
        } else {
            Log::warn("could not find " + (hb ? skelNif : headNif) + " in the Fallout 4 data - using the built-in calibration");
        }
    }
    return hs;
}

// Positions of every primitive of a head GLB, in order (the order setHeadPositions writes back).
std::vector<Vec3> glbHeadPositions(const Glb& g) {
    std::vector<Vec3> v;
    const nlohmann::json meshes = g.doc.value("meshes", nlohmann::json::array());
    for (const auto& m : meshes) {
        const nlohmann::json prims = m.value("primitives", nlohmann::json::array());
        for (const auto& p : prims) {
            if (!p["attributes"].contains("POSITION")) continue;
            int c = 3;
            auto f = readFloatAccessor(g, p["attributes"]["POSITION"].get<int>(), c);
            for (size_t i = 0; i + 2 < f.size(); i += 3) v.emplace_back(f[i], f[i + 1], f[i + 2]);
        }
    }
    return v;
}

// The preset's face presets / sliders that Fallout 4's head model has a shape for.
std::map<uint32_t, double> knownFacePresets(const Fo4FaceModel& model, const LooksMenuPreset& preset, size_t* unknown) {
    std::map<uint32_t, double> out;
    for (const auto& [k, w] : preset.presetWeights) {
        if (model.presetKeys().count(k) || model.sliderKeys().count(k)) out[k] = w;
        else if (unknown) ++*unknown;
    }
    return out;
}

// A preset made with Extended Facial Sculpting has its region IDs (500000 and up): use its region files.
ConvertOptions withEfsDetected(const ConvertOptions& opt, const LooksMenuPreset& preset) {
    ConvertOptions o = opt;
    if (opt.extendedSculpting || opt.extendedSculptingRedux || !opt.fo4Regions.empty()) return o;
    // which bundled region files know the preset's sculpt IDs (vanilla IDs are in all of them)
    const fs::path dataDir = opt.dataDir.empty() ? fs::path("data") : opt.dataDir;
    const std::string sexName = preset.gender == 0 ? "Male" : "Female";
    auto ids = [&](const std::string& folder) {
        std::set<uint32_t> s;
        const fs::path f = dataDir / folder / ("HumanRaceFacialBoneRegions" + sexName + ".txt");
        std::error_code ec;
        if (fs::exists(f, ec))
            for (const auto& [id, r] : loadFo4Regions(f)) s.insert(id);
        return s;
    };
    const std::set<uint32_t> efs = ids("efs"), redux = ids("efs_redux");
    bool extended = false, reduxOnly = false;
    for (const auto& r : preset.regions) {
        const bool inEfs = efs.count(r.id) > 0, inRedux = redux.count(r.id) > 0;
        if (r.id >= 500000 && (inEfs || inRedux)) extended = true;
        if (inRedux && !inEfs) reduxOnly = true;
    }
    if (reduxOnly) {
        o.extendedSculptingRedux = true;
        Log::info("                 the preset uses Extended Facial Sculpting REDUX sliders - using its region files");
    } else if (extended) {
        o.extendedSculpting = true;
        Log::info("                 the preset uses Extended Facial Sculpting sliders - using its region files");
    }
    return o;
}

// Fallout 4 -> V: the preset sculpted into V's head (hs.headGlb is changed).
struct ForwardSculpt {
    SculptInput sculpt;
    std::vector<Vec3> headBase;                                  // V's head before
    std::vector<std::pair<std::string, DeformResult>> results;
    std::string nativeLine;                                      // set when Fallout 4's own head did it
};
ForwardSculpt sculptVHead(const ConvertOptions& opt, const LooksMenuPreset& preset, HeadSetup& hs, const fs::path& dataDir) {
    ForwardSculpt out;
    Mapping& mapping = hs.mapping;
    Glb& headGlb = hs.headGlb;
    std::optional<Glb>& meshGlb = hs.meshGlb;
    const HeadFrame& frame = hs.frame;
    const fs::path& headGlbPath = hs.headGlbPath;
    const fs::path& meshGlbPath = hs.meshGlbPath;
    SculptInput& sculpt = out.sculpt;
    sculpt = resolveSculpts(preset, mapping);
    if (sculpt.empty()) Log::warn("nothing in the preset matched the mapping - the head would be unchanged.");

    DeformOptions dopt;
    dopt.meshFilter = opt.meshFilter;
    dopt.strength = opt.strength;
    dopt.adjustMorphTargets = !opt.keepMorphTargets;

    std::vector<Vec3>& headBase = out.headBase;
    headBase = glbHeadPositions(headGlb);
    std::vector<std::pair<std::string, DeformResult>>& results = out.results;
    std::string& nativeLine = out.nativeLine;
    const bool native = hs.fo4Head && hs.fo4Skeleton && hs.calibration && !opt.approximateSculpt &&
                        (!sculpt.empty() || !preset.presetWeights.empty());
    if (native) {
        // Fallout 4's own head, sculpted with its own bones and skin weights; its face movement is
        // carried over to V's vertices.
        Log::info("Sculpting Fallout 4's head and carrying the shape over to V's head...");
        Fo4FaceModel model(*hs.fo4Head, *hs.fo4Skeleton, mapping, *hs.calibration);
        const bool morphs = attachFaceMorphs(model, hs.fo4ChargenTri, dataDir, hs.gender);
        auto toH = [&](const Vec3& p) { return Vec3(dot(p, frame.right), dot(p, frame.up), dot(p, frame.forward)); };
        std::vector<Vec3> vPos;
        for (const auto& p : headBase) vPos.push_back(toH(p));
        HeadAlignment ha = alignFo4Face(model, vPos);
        size_t unknownPresets = 0;
        const std::map<uint32_t, double> facePresets = morphs ? knownFacePresets(model, preset, &unknownPresets)
                                                              : std::map<uint32_t, double>{};
        std::vector<Vec3> disp = transferFo4Sculpt(model, preset.regions, ha, vPos, facePresets);
        DeformResult dr;
        dr.vertexCount = headBase.size();
        std::vector<Vec3> moved(headBase.size());
        for (size_t i = 0; i < headBase.size(); ++i) {
            const Vec3 d = disp[i] * opt.strength;
            const Vec3 g = frame.right * d.x + frame.up * d.y + frame.forward * d.z;
            moved[i] = headBase[i] + g;
            if (length(g) > 1e-6) ++dr.verticesMoved;
            dr.maxDisplacement = std::max(dr.maxDisplacement, length(g));
        }
        setHeadPositions(headGlb, moved);
        results.emplace_back(u8(headGlbPath.filename()), dr);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "Fallout 4's own head and face bones (%zu vertices moved, up to %.1f mm)",
                      dr.verticesMoved, dr.maxDisplacement * 1000);
        nativeLine = buf;
        if (morphs) {
            nativeLine += ", " + std::to_string(facePresets.size()) + " face presets / sliders";
            if (unknownPresets) nativeLine += " (" + std::to_string(unknownPresets) + " without a shape skipped)";
        }
        Log::info("                 " + nativeLine);
    } else {
        Log::info("Sculpting head...");
        results.emplace_back(u8(headGlbPath.filename()), deformHead(headGlb, sculpt, mapping, frame, dopt));
        if (meshGlb) results.emplace_back(u8(meshGlbPath.filename()), deformHead(*meshGlb, sculpt, mapping, frame, dopt));
    }

    return out;
}

// Cyberpunk -> Fallout 4: Fallout 4 sculpts (and face presets) fitted to V's face from the save.
struct ReverseFit {
    std::vector<std::string> used;          // the save's character creator choices found in V's head
    std::vector<RegionSculpt> fitted;
    std::map<uint32_t, double> presets;
    std::string fitLine, fitNote;
};
ReverseFit fitFo4ToSave(const ConvertOptions& opt, const CpAppearance& app, HeadSetup& hs, const fs::path& dataDir) {
    ReverseFit out;
    std::vector<std::string>& used = out.used;
    std::vector<std::string> missing;
    std::vector<RegionSculpt>& fitted = out.fitted;
    std::map<uint32_t, double>& fittedPresets = out.presets;
    std::string& fitLine = out.fitLine;
    std::string& fitNote = out.fitNote;
    const bool native = hs.fo4Head && hs.fo4Skeleton && hs.calibration;
    if (native) {
        // V's whole face (base head + character creator choices) against Fallout 4's default face,
        // with Fallout 4's own bones and skin weights doing the sculpting.
        std::vector<Vec3> d = ccMorphDisplacement(hs.headGlb, app.morphs, used, missing, false);
        for (const auto& m : missing) Log::warn("character creator choice not in V's head file: " + m);
        if (used.empty() && !app.morphs.empty()) throw std::runtime_error("none of the save's face choices exist in V's head file");
        const auto& prim = hs.headGlb.doc["meshes"][0]["primitives"][0]["attributes"];
        int c = 3;
        auto P = readFloatAccessor(hs.headGlb, prim["POSITION"].get<int>(), c);
        std::vector<float> N;
        if (prim.contains("NORMAL")) N = readFloatAccessor(hs.headGlb, prim["NORMAL"].get<int>(), c);
        const HeadFrame& f = hs.frame;
        auto toH = [&](const Vec3& p) { return Vec3(dot(p, f.right), dot(p, f.up), dot(p, f.forward)); };
        std::vector<Vec3> vPos, vNrm;
        for (size_t i = 0; i < d.size() && i * 3 + 2 < P.size(); ++i) {
            vPos.push_back(toH(Vec3(P[i * 3], P[i * 3 + 1], P[i * 3 + 2]) + d[i] * opt.strength));
            vNrm.push_back(N.size() > i * 3 + 2 ? normalize(toH(Vec3(N[i * 3], N[i * 3 + 1], N[i * 3 + 2]))) : Vec3(0, 0, 1));
        }
        Log::info("Fitting Fallout 4's face to V's face on Fallout 4's own head (this takes a moment)...");
        Fo4FaceModel model(*hs.fo4Head, *hs.fo4Skeleton, hs.mapping, *hs.calibration);
        const bool morphs = attachFaceMorphs(model, hs.fo4ChargenTri, dataDir, hs.gender);
        NativeFitOptions no;
        no.faceMorphs = morphs;
        no.regularization = 2e-2;
        NativeFitResult fit = fitFo4SculptsNative(model, hs.mapping, vPos, vNrm, no);
        fitted = fit.regions;
        fittedPresets = fit.presets;
        char buf[256];
        const double match = fit.alignedMm > 0 ? 100.0 * std::max(0.0, 1.0 - fit.afterMm / fit.alignedMm) : 100.0;
        std::snprintf(buf, sizeof(buf), "Fallout 4's default face is %.2f mm (mean) from V's face; with the preset %.2f mm "
                      "(%.0f%% closer)", fit.alignedMm, fit.afterMm, match);
        fitLine = buf;
        for (const auto& n : fit.notes) fitNote += n + "\n";
    } else {
        Log::warn("Fallout 4 folder not set: using the approximate fit (set it for Fallout 4's own head and face bones)");
        std::vector<Vec3> target = ccMorphDisplacement(hs.headGlb, app.morphs, used, missing);
        for (const auto& m : missing) Log::warn("character creator choice not in V's head file: " + m);
        if (used.empty() && !app.morphs.empty()) throw std::runtime_error("none of the save's face choices exist in V's head file");
        for (Vec3& v : target) v = v * opt.strength;
        Log::info("Fitting Fallout 4 face sculpts to V's face (this takes a moment)...");
        FitResult fit = fitFo4Sculpts(hs.headGlb, target, hs.mapping, hs.frame, FitOptions{});
        fitted = fit.regions;
        char buf[256];
        const double match = fit.targetRmsMm > 0
            ? 100.0 * std::max(0.0, 1.0 - (fit.residualRmsMm * fit.residualRmsMm) / (fit.targetRmsMm * fit.targetRmsMm))
            : 100.0;
        std::snprintf(buf, sizeof(buf), "approximate: shape match %.0f%% of V's differences from the average face", match);
        fitLine = buf;
        for (const auto& n : fit.notes) fitNote += n + "\n";
    }
    return out;
}

struct LogFile {
    explicit LogFile(const fs::path& p) { Log::setLogFile(p); }
    ~LogFile() { Log::setLogFile({}); }
};

} // namespace

ConvertResult runConversion(const ConvertOptions& opt) {
    if (opt.cpToFo4) return runReverseConversion(opt);
    ConvertResult res;
    if (opt.preset.empty()) throw std::runtime_error("a LooksMenu preset or a Fallout 4 save is required");

    const fs::path dataDir = opt.dataDir.empty() ? fs::path("data") : opt.dataDir;
    const fs::path mappingPath = opt.mapping.empty() ? dataDir / "default_mapping.json" : opt.mapping;
    fs::create_directories(longPath(opt.workDir));
    LogFile logFile(opt.workDir / "log.txt");

    Fo4SaveInfo saveInfo;
    const bool fromSave = isFo4Save(opt.preset);
    Log::info(std::string(fromSave ? "Reading save     " : "Reading preset   ") + u8(opt.preset));
    LooksMenuPreset preset = loadFo4Character(opt.preset, &saveInfo);
    if (fromSave) {
        Log::info("                 " + saveInfo.playerName + ", level " + std::to_string(saveInfo.level) + ", " +
                  (preset.gender == 1 ? "female" : "male") + ", " + std::to_string(preset.regions.size()) +
                  " face sculpts, " + std::to_string(preset.presetWeights.size()) + " face presets");
        for (const auto& w : preset.parseWarnings) Log::warn(w);
    }

    std::string gender = toLower(opt.gender);
    if (gender.empty()) gender = preset.gender == 0 ? "male" : "female";
    const ConvertOptions headOpt = withEfsDetected(opt, preset);
    HeadSetup hs = setupHead(headOpt, mappingPath, dataDir, gender, opt.preset.parent_path(), true);
    Mapping& mapping = hs.mapping;
    Glb& headGlb = hs.headGlb;
    std::optional<Glb>& meshGlb = hs.meshGlb;
    const HeadFrame& frame = hs.frame;
    std::vector<std::string>& frameWarnings = hs.frameWarnings;
    const fs::path& tplMorph = hs.tplMorph;
    const fs::path& tplMesh = hs.tplMesh;
    const std::string& morphDepot = hs.morphDepot;
    const std::string& meshDepot = hs.meshDepot;

    ForwardSculpt fs_ = sculptVHead(opt, preset, hs, dataDir);
    SculptInput& sculpt = fs_.sculpt;
    std::vector<Vec3>& headBase = fs_.headBase;
    std::vector<std::pair<std::string, DeformResult>>& results = fs_.results;
    std::string& nativeLine = fs_.nativeLine;
    auto headPositions = [](const Glb& g) { return glbHeadPositions(g); };

    res.report = formatReport(mapping, preset, sculpt, frame, results, frameWarnings);
    if (!nativeLine.empty()) res.report += "\nSculpt method    : " + nativeLine + "\n";
    else if (!sculpt.empty()) res.report += "\nSculpt method    : approximate (bones on V's head) - set the Fallout 4 folder for the exact method\n";
    if (!preset.presetWeights.empty() && (nativeLine.empty() || nativeLine.find("face presets") == std::string::npos))
        res.report += "Not converted    : the preset's face presets / sliders (Morphs.Presets) - needs the Fallout 4 folder with the chargen .tri\n";
    writeFileText(opt.workDir / "report.txt", res.report);
    if (sculpt.empty() && results.empty()) { res.exitCode = 2; return res; }

    Log::info("Replacing        " + morphDepot);
    PackageOptions popt;
    popt.modName = opt.modName.empty() ? "LM_" + sanitizeId(preset.name) + "_Head" : opt.modName;
    popt.neutralizeCharacterCreator = !opt.keepCharacterCreatorShapes;

    // ---- the rest of V's face: switch off the same character creator shapes and follow the sculpt
    std::vector<std::string> companionLines;
    if (!opt.keepCharacterCreatorShapes || opt.companions) {
        auto comps = extractCompanionMorphtargets(opt, mapping, hs.gender, morphDepot);
        if (!comps.empty()) {
            Kraken kraken;
            std::vector<fs::path> od;
            if (!opt.cpGameDir.empty()) od.push_back(opt.cpGameDir / "bin" / "x64");
            if (!opt.wolvenkit.empty()) od.push_back(opt.wolvenkit.parent_path());
            kraken.load(od);
            SculptField field(headBase, headPositions(headGlb));
            for (const auto& [file, depot] : comps) {
                MorphtargetPatch mp;
                mp.neutralizeCharacterCreator = !opt.keepCharacterCreatorShapes;
                if (opt.companions) mp.displace = [&field](const Vec3& g) { return field(g); };
                const fs::path outF = opt.workDir / "companions" / depotToFs(depot);
                try {
                    MorphtargetPatchStats st = patchMorphtarget(file, outF, kraken, mp);
                    if (st.moved == 0 && st.neutralized == 0) continue;   // unaffected: leave the game file alone
                    popt.preparedFiles.push_back({outF, depot});
                    std::string line = file.filename().u8string() + ": " + std::to_string(st.moved) + " vertices follow the sculpt";
                    if (st.neutralized) line += ", " + std::to_string(st.neutralized) + " character creator shapes off";
                    companionLines.push_back(line);
                    Log::info("Face part        " + line);
                } catch (const std::exception& e) {
                    Log::warn("skipped " + file.filename().u8string() + ": " + e.what());
                }
            }
        } else if (!opt.wolvenkit.empty() && !opt.cpGameDir.empty()) {
            Log::warn("no other face morph targets found next to the head - eyes/brows/lashes keep their character creator shapes");
        }
    }
    popt.outDir = opt.workDir;
    popt.wolvenkit = opt.wolvenkit;
    popt.patchMorphtargets = !opt.wolvenkitMorphImport;
    if (!opt.cpGameDir.empty()) popt.oodleDirs.push_back(opt.cpGameDir / "bin" / "x64");
    if (!opt.wolvenkit.empty()) popt.oodleDirs.push_back(opt.wolvenkit.parent_path());
    if (!opt.dataDir.empty()) popt.oodleDirs.push_back(opt.dataDir.parent_path());
    if (meshGlb) {
        Log::info("Replacing        " + meshDepot);
        popt.files.push_back({&*meshGlb, tplMesh, meshDepot}); // .mesh is imported first
    }
    popt.files.push_back({&headGlb, tplMorph, morphDepot});

    Log::info("Packaging...");
    PackageResult pr = packageHeadReplacement(mapping, popt);
    for (const auto& n : pr.notes) Log::warn(n);
    if (!pr.report.empty() || !companionLines.empty()) {
        res.report += "\nGame file patch:\n";
        for (const auto& l : pr.report) res.report += "  " + l + "\n";
        for (const auto& l : companionLines) res.report += "  " + l + "\n";
        writeFileText(opt.workDir / "report.txt", res.report);
    }
    res.projectDir = pr.projectDir;
    Log::info("WolvenKit project: " + u8(pr.projectDir));
    if (!pr.complete) {
        res.manualSteps = opt.workDir / "MANUAL_STEPS.txt";
        Log::info("The .archive was not built automatically; follow " + u8(res.manualSteps));
        res.exitCode = 3;
        return res;
    }

    res.zip = pr.zipPath;
    res.archive = opt.workDir / "dist" / "archive" / "pc" / "mod" / (popt.modName + ".archive");
    if (!opt.archiveDest.empty() && (opt.archiveToDest || opt.zipToDest)) {
        fs::create_directories(longPath(opt.archiveDest));
        if (opt.archiveToDest) {
            fs::path dest = opt.archiveDest / (popt.modName + ".archive");
            copyFileReplace(res.archive, dest);
            res.archive = dest;
            res.archiveCopied = true;
        }
        if (opt.zipToDest && !res.zip.empty()) {
            fs::path zdest = opt.archiveDest / (popt.modName + ".zip");
            copyFileReplace(res.zip, zdest);
            res.zip = zdest;
            res.zipCopied = true;
        }
    }
    Log::info("Archive          " + u8(res.archive));
    Log::info("Mod zip          " + u8(res.zip));
    res.exitCode = 0;
    return res;
}

// ======================================================================== Cyberpunk -> Fallout 4
ConvertResult runReverseConversion(const ConvertOptions& opt) {
    ConvertResult res;
    if (opt.cpSave.empty()) throw std::runtime_error("a Cyberpunk 2077 save (sav.dat) is required");
    const fs::path dataDir = opt.dataDir.empty() ? fs::path("data") : opt.dataDir;
    const fs::path mappingPath = opt.mapping.empty() ? dataDir / "default_mapping.json" : opt.mapping;
    fs::create_directories(longPath(opt.workDir));
    LogFile logFile(opt.workDir / "log.txt");

    Log::info("Reading save     " + u8(opt.cpSave));
    CpAppearance app = loadCpSave(opt.cpSave);
    for (const auto& n : app.notes) Log::warn(n);
    std::string gender = toLower(opt.gender);
    if (gender.empty()) gender = app.genderKnown ? (app.isMale ? "male" : "female") : "female";
    {
        std::string choices;
        for (const auto& [r, t] : app.morphs) choices += (choices.empty() ? "" : ", ") + r + " " + t;
        Log::info("                 " + std::string(app.isMale ? "male" : "female") + " body; " + choices);
    }

    HeadSetup hs = setupHead(opt, mappingPath, dataDir, gender, {}, false);
    if (hs.mapping.fo4Regions.empty())
        throw std::runtime_error("no Fallout 4 sculpt region data - set the Fallout 4 folder (or bundle "
                                 "HumanRaceFacialBoneRegions*.txt in the data folder)");

    ReverseFit rf = fitFo4ToSave(opt, app, hs, dataDir);
    std::vector<std::string>& used = rf.used;
    std::vector<RegionSculpt>& fitted = rf.fitted;
    std::map<uint32_t, double>& fittedPresets = rf.presets;
    std::string& fitLine = rf.fitLine;
    std::string& fitNote = rf.fitNote;
    Log::info("Fit              " + fitLine);

    // ---- the preset
    LooksMenuPreset out;
    out.gender = gender == "male" ? 0 : 1;
    out.regions = fitted;
    out.presetWeights = fittedPresets;
    std::string hairLine;
    if (!app.hairColor.empty() && opt.approximateCustomization) {
        bool ok = false;
        const Fo4HairColor& h = nearestFo4HairColor(app.hairColor, &ok);
        out.hairColor = fo4FormRef(h.formId);
        hairLine = app.hairColor + " -> " + h.editorId + " (" + out.hairColor + ")" + (ok ? "" : " - colour name not recognised, guessed");
        Log::info("Hair colour      " + hairLine);
    }
    // eye colour, skin tone, eyebrows, freckles, blemishes, makeup
    std::vector<std::string> lookLines;
    {
        const Fo4Chargen cg = opt.approximateCustomization ? loadFo4Chargen(dataDir / "fo4_chargen.json") : Fo4Chargen{};
        if (!opt.approximateCustomization) {
            Log::info("Customization    not carried over (face shape only)");
        } else if (!cg.loaded) {
            Log::warn("data\\fo4_chargen.json missing - eye colour, skin and makeup are not carried over");
        } else {
            const bool female = gender != "male";
            Fo4Look look = mapCyberpunkLook(app, cg, female);
            lookLines = look.lines;
            if (!look.eyesForm.empty()) {
                out.headParts.push_back(look.eyesForm);
                for (const auto& [sx, list] : cg.eyes)
                    for (const auto& e : list) out.replaceHeadParts.push_back("Fallout4.esm|" + e.first);
            }
            out.tints = look.tints;
            const std::string sx = female ? "female" : "male";
            if (cg.tints.count(sx))
                for (const auto& t : look.tints)
                    for (const auto& l : cg.tints.at(sx))
                        if (l.kind == t.kind) out.replaceTints.push_back(l.index);
            for (const auto& l : lookLines) Log::info("Look             " + l);
        }
    }
    std::vector<double> weight;
    fs::path baseJson;
    if (!opt.basePreset.empty()) {
        if (isFo4Save(opt.basePreset)) {
            Fo4SaveInfo si;
            loadFo4Save(opt.basePreset, &si);
            weight = si.weight;
            Log::info("Body weight from " + u8(opt.basePreset));
        } else {
            baseJson = opt.basePreset;
            Log::info("Head parts/tints/body from " + u8(opt.basePreset));
        }
    }
    const std::string name = opt.modName.empty() ? "CP_" + sanitizeId(app.saveName.empty() ? "V" : app.saveName) : opt.modName;
    const std::string json = looksMenuPresetJson(out, weight, baseJson);

    // work copy + mod manager package (Data-relative: F4SE\Plugins\F4EE\Presets\<name>.json)
    const fs::path workJson = opt.workDir / (name + ".json");
    writeFileText(workJson, json);
    const fs::path pkg = opt.workDir / "preset_pkg";
    std::error_code ec;
    fs::remove_all(longPath(pkg), ec);
    writeFileText(pkg / "F4SE" / "Plugins" / "F4EE" / "Presets" / (name + ".json"), json);
    const fs::path workZip = opt.workDir / (name + ".zip");
    zipDirectory(pkg, workZip);
    res.presetFile = workJson;
    res.zip = workZip;

    fs::path dest = opt.presetOut;
    if (!dest.empty() && dest.has_extension() && toLower(dest.extension().u8string()) == ".json" && !fs::is_directory(dest, ec)) {
        // a file name was given
        writeFileText(dest, json);
        res.presetFile = dest;
        res.archiveCopied = true;
    } else if (!dest.empty()) {
        fs::create_directories(longPath(dest));
        if (opt.archiveToDest) {
            copyFileReplace(workJson, dest / (name + ".json"));
            res.presetFile = dest / (name + ".json");
            res.archiveCopied = true;
        }
        if (opt.zipToDest) {
            copyFileReplace(workZip, dest / (name + ".zip"));
            res.zip = dest / (name + ".zip");
            res.zipCopied = true;
        }
    }
    Log::info("Preset           " + u8(res.presetFile));
    Log::info("Mod zip          " + u8(res.zip));

    // ---- the face written into a copy of a Fallout 4 save
    Fo4SaveWriteInfo saveInfoOut;
    if (!opt.fo4SaveTarget.empty()) {
        fs::path so = opt.fo4SaveOut;
        if (so.empty() || fs::is_directory(so, ec))
            so = (so.empty() ? opt.fo4SaveTarget.parent_path() : so) / (opt.fo4SaveTarget.stem().u8string() + "_" + name + ".fos");
        std::vector<double> w = weight;
        saveInfoOut = writeFo4SaveFace(opt.fo4SaveTarget, so, out, w);
        res.saveFile = so;
        Log::info("Fallout 4 save   " + u8(so) + " (face of the " + saveInfoOut.faceSource + ")");
        for (const auto& l : saveInfoOut.lines) Log::info("                 " + l);
        if (saveInfoOut.coSaveCopied) Log::info("                 .f4se co-save copied");
    }

    // ---- report
    std::ostringstream r;
    r << "Cyberpunk 2077 save : " << u8(opt.cpSave) << "\n";
    r << "Body / gender       : " << (app.isMale ? "male" : "female") << " -> Fallout 4 " << gender << "\n";
    r << "Face choices        :";
    for (const auto& u : used) r << "  " << u;
    if (used.empty()) r << "  none in the save (default V: base face)";
    r << "\n";
    if (!hairLine.empty()) r << "Hair colour         : " << hairLine << "\n";
    for (size_t i = 0; i < lookLines.size(); ++i)
        r << (i ? "                      " : "Look                : ") << lookLines[i] << "\n";
    r << "Fit                 : " << fitLine << "\n";
    std::istringstream notes(fitNote);
    for (std::string l; std::getline(notes, l);) r << "                      " << l << "\n";
    if (!fittedPresets.empty()) {
        r << "\nFallout 4 face presets / sliders (LooksMenu 'Morphs.Presets'):\n";
        for (const auto& [k, w] : fittedPresets) r << "  " << toHex(k) << "  " << w << "\n";
    }
    r << "\nFallout 4 sculpts (LooksMenu 'Regions'):\n";
    for (const auto& rs : fitted) {
        auto it = hs.mapping.fo4RegionNames.find(rs.id);
        r << "  " << toHex(rs.id) << "  " << (it != hs.mapping.fo4RegionNames.end() ? it->second : std::string("?")) << "  [";
        for (size_t i = 0; i < rs.values.size(); ++i) r << (i ? ", " : "") << rs.values[i];
        r << "]\n";
    }
    r << "\nPreset: " << u8(res.presetFile) << "\n";
    r << "Mod manager zip: " << u8(res.zip) << " (installs to Data\\F4SE\\Plugins\\F4EE\\Presets)\n";
    r << "In Fallout 4: LooksMenu -> Load preset.\n";
    if (opt.extendedSculptingRedux)
        r << "The preset uses Extended Facial Sculpting REDUX sliders: the mod must be installed in Fallout 4.\n";
    else if (opt.extendedSculpting)
        r << "The preset uses Extended Facial Sculpting sliders: the mod must be installed in Fallout 4.\n";
    if (!res.saveFile.empty()) {
        r << "\nFallout 4 save with this face: " << u8(res.saveFile) << "\n";
        r << "  (a copy of " << u8(opt.fo4SaveTarget) << ", face of the " << saveInfoOut.faceSource << ")\n";
        for (const auto& l : saveInfoOut.lines) r << "  " << l << "\n";
        r << "  Load it from Fallout 4's Load menu; the original save is untouched.\n";
    }
    if (opt.basePreset.empty() && res.saveFile.empty())
        r << "Hairstyle and body weight are not in a Cyberpunk save - pick one of your presets as the base to keep yours.\n";
    for (const auto& w : hs.frameWarnings) r << "note: " << w << "\n";
    res.report = r.str();
    writeFileText(opt.workDir / "report.txt", res.report);
    res.exitCode = 0;
    return res;
}

} // namespace ptk

// ======================================================================== the face as a 3D model
namespace ptk {

namespace {

// V's head GLB (positions as given) -> exportable parts: every primitive with its UVs and triangles
std::vector<ModelPart> partsFromGlb(const Glb& g, const std::string& name) {
    std::vector<ModelPart> parts;
    const nlohmann::json meshes = g.doc.value("meshes", nlohmann::json::array());
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const nlohmann::json prims = meshes[mi].value("primitives", nlohmann::json::array());
        for (size_t pi = 0; pi < prims.size(); ++pi) {
            const auto& p = prims[pi];
            if (!p["attributes"].contains("POSITION") || !p.contains("indices")) continue;
            if (p.value("mode", 4) != 4) continue;   // triangles only
            ModelPart part;
            part.name = name + (parts.empty() ? std::string() : "_" + std::to_string(parts.size()));
            int c = 3;
            const auto P = readFloatAccessor(g, p["attributes"]["POSITION"].get<int>(), c);
            for (size_t i = 0; i + 2 < P.size(); i += 3) part.pos.emplace_back(P[i], P[i + 1], P[i + 2]);
            if (p["attributes"].contains("NORMAL")) {
                const auto N = readFloatAccessor(g, p["attributes"]["NORMAL"].get<int>(), c);
                for (size_t i = 0; i + 2 < N.size(); i += 3) part.nrm.emplace_back(N[i], N[i + 1], N[i + 2]);
                if (part.nrm.size() != part.pos.size()) part.nrm.clear();
            }
            if (p["attributes"].contains("TEXCOORD_0")) {
                const auto T = readFloatAccessor(g, p["attributes"]["TEXCOORD_0"].get<int>(), c);
                for (size_t i = 0; i + 1 < T.size(); i += c) part.uv.push_back({T[i], T[i + 1]});
                if (part.uv.size() != part.pos.size()) part.uv.clear();
            }
            part.idx = readIndexAccessor(g, p["indices"].get<int>());
            parts.push_back(std::move(part));
        }
    }
    if (parts.empty()) throw std::runtime_error("V's head file has no triangle mesh to export");
    return parts;
}

// Fallout 4's head (the shape the face model uses) with the given head-space positions
ModelPart fo4HeadPart(const NifFile& head, const std::vector<Vec3>& headSpace, const HeadFrame& f) {
    const NifShape* shape = nullptr;
    for (const auto& s : head.shapes())
        if (!s.verts.empty() && !s.skinBones.empty() && s.skinR.size() == s.skinBones.size()) { shape = &s; break; }
    if (!shape || shape->tris.empty()) throw std::runtime_error("the Fallout 4 head NIF has no triangles to export");
    if (shape->verts.size() != headSpace.size())
        throw std::runtime_error("Fallout 4 head: vertex count mismatch (" + std::to_string(shape->verts.size()) + " / " +
                                 std::to_string(headSpace.size()) + ")");
    ModelPart part;
    part.name = "Fallout4Head";
    for (const Vec3& h : headSpace) part.pos.push_back(f.right * h.x + f.up * h.y + f.forward * h.z);
    if (shape->uv.size() == shape->verts.size()) part.uv = shape->uv;
    for (const auto& t : shape->tris) part.idx.insert(part.idx.end(), {t[0], t[1], t[2]});
    // triangles facing outwards (the head-space mapping may mirror Fallout 4's winding)
    Vec3 c;
    for (const Vec3& p : part.pos) c += p;
    c = c * (1.0 / double(part.pos.size()));
    double out = 0;
    for (size_t t = 0; t + 2 < part.idx.size(); t += 3) {
        const Vec3 &a = part.pos[part.idx[t]], &b = part.pos[part.idx[t + 1]], &d = part.pos[part.idx[t + 2]];
        out += dot(cross(b - a, d - a), (a + b + d) * (1.0 / 3) - c);
    }
    if (out < 0)
        for (size_t t = 0; t + 2 < part.idx.size(); t += 3) std::swap(part.idx[t + 1], part.idx[t + 2]);
    computeSmoothNormals(part);
    return part;
}

} // namespace

ModelExportResult exportFaceModel(const ConvertOptions& opt, const std::string& headIn, const fs::path& outIn) {
    ModelExportResult res;
    const std::string head = toLower(headIn);
    if (head != "cp" && head != "fo4") throw std::runtime_error("head mesh must be cp (Cyberpunk 2077) or fo4 (Fallout 4)");
    const fs::path dataDir = opt.dataDir.empty() ? fs::path("data") : opt.dataDir;
    const fs::path mappingPath = opt.mapping.empty() ? dataDir / "default_mapping.json" : opt.mapping;
    fs::create_directories(longPath(opt.workDir));
    LogFile logFile(opt.workDir / "log.txt");
    std::ostringstream r;

    std::vector<ModelPart> parts;
    std::string name;
    auto needFo4Head = [](const HeadSetup& hs) {
        if (!hs.fo4Head || !hs.fo4Skeleton || !hs.calibration)
            throw std::runtime_error("exporting with Fallout 4's head needs the Fallout 4 folder (its head and face bones)");
    };

    if (opt.cpToFo4) {
        // ---- Cyberpunk 2077 save
        if (opt.cpSave.empty()) throw std::runtime_error("a Cyberpunk 2077 save (sav.dat) is required");
        Log::info("Reading save     " + u8(opt.cpSave));
        CpAppearance app = loadCpSave(opt.cpSave);
        for (const auto& n : app.notes) Log::warn(n);
        std::string gender = toLower(opt.gender);
        if (gender.empty()) gender = app.genderKnown ? (app.isMale ? "male" : "female") : "female";
        name = sanitizeId(app.saveName.empty() ? "V" : app.saveName);
        r << "Source            : Cyberpunk 2077 save " << u8(opt.cpSave) << " (" << gender << ")\n";
        HeadSetup hs = setupHead(opt, mappingPath, dataDir, gender, {}, false);
        if (head == "cp") {
            std::vector<std::string> used, missing;
            const std::vector<Vec3> d = ccMorphDisplacement(hs.headGlb, app.morphs, used, missing, false);
            for (const auto& m : missing) Log::warn("character creator choice not in V's head file: " + m);
            std::vector<Vec3> P = glbHeadPositions(hs.headGlb);
            for (size_t i = 0; i < P.size() && i < d.size(); ++i) P[i] += d[i] * opt.strength;
            setHeadPositions(hs.headGlb, P);
            parts = partsFromGlb(hs.headGlb, "V_head");
            r << "Head mesh         : Cyberpunk 2077 - V's head with the save's character creator choices\n";
            r << "Face choices      :";
            for (const auto& u : used) r << "  " << u;
            if (used.empty()) r << "  none (default V)";
            r << "\n";
        } else {
            needFo4Head(hs);
            ReverseFit rf = fitFo4ToSave(opt, app, hs, dataDir);
            Fo4FaceModel model(*hs.fo4Head, *hs.fo4Skeleton, hs.mapping, *hs.calibration);
            attachFaceMorphs(model, hs.fo4ChargenTri, dataDir, hs.gender);
            parts.push_back(fo4HeadPart(*hs.fo4Head, model.evaluate(rf.fitted, rf.presets), hs.frame));
            r << "Head mesh         : Fallout 4 - its head sculpted with the preset fitted to V's face\n";
            r << "Fit               : " << rf.fitLine << "\n";
        }
    } else {
        // ---- Fallout 4 preset or save
        if (opt.preset.empty()) throw std::runtime_error("a LooksMenu preset or a Fallout 4 save is required");
        const bool fromSave = isFo4Save(opt.preset);
        Log::info(std::string(fromSave ? "Reading save     " : "Reading preset   ") + u8(opt.preset));
        Fo4SaveInfo saveInfo;
        LooksMenuPreset preset = loadFo4Character(opt.preset, &saveInfo);
        for (const auto& w : preset.parseWarnings) Log::warn(w);
        std::string gender = toLower(opt.gender);
        if (gender.empty()) gender = preset.gender == 0 ? "male" : "female";
        name = sanitizeId(fromSave && !saveInfo.playerName.empty() ? saveInfo.playerName : opt.preset.stem().u8string());
        r << "Source            : Fallout 4 " << (fromSave ? "save " : "preset ") << u8(opt.preset) << " (" << gender
          << ", " << preset.regions.size() << " sculpts, " << preset.presetWeights.size() << " face presets / sliders)\n";
        HeadSetup hs = setupHead(withEfsDetected(opt, preset), mappingPath, dataDir, gender, opt.preset.parent_path(), false);
        if (head == "cp") {
            ForwardSculpt fsc = sculptVHead(opt, preset, hs, dataDir);
            parts = partsFromGlb(hs.headGlb, "V_head");
            r << "Head mesh         : Cyberpunk 2077 - V's head with the face baked in\n";
            r << "Sculpt method     : "
              << (fsc.nativeLine.empty() ? "approximate (bones on V's head) - set the Fallout 4 folder for the exact method"
                                         : fsc.nativeLine)
              << "\n";
        } else {
            needFo4Head(hs);
            Fo4FaceModel model(*hs.fo4Head, *hs.fo4Skeleton, hs.mapping, *hs.calibration);
            const bool morphs = attachFaceMorphs(model, hs.fo4ChargenTri, dataDir, hs.gender);
            size_t unknown = 0;
            const auto presets = morphs ? knownFacePresets(model, preset, &unknown) : std::map<uint32_t, double>{};
            std::vector<RegionSculpt> regions = preset.regions;
            if (opt.strength != 1.0)
                for (auto& rs : regions)
                    for (auto& v : rs.values) v *= opt.strength;
            parts.push_back(fo4HeadPart(*hs.fo4Head, model.evaluate(regions, presets), hs.frame));
            r << "Head mesh         : Fallout 4 - its own head sculpted with the preset (as in the game)\n";
            r << "Applied           : " << regions.size() << " sculpts, " << presets.size() << " face presets / sliders";
            if (!morphs && !preset.presetWeights.empty()) r << " (face presets need the chargen .tri in the Fallout 4 folder)";
            if (unknown) r << " (" << unknown << " without a shape skipped)";
            r << "\n";
        }
    }

    // ---- write
    fs::path out = outIn;
    std::error_code ec;
    const std::string ext = toLower(out.extension().u8string());
    if (out.empty() || fs::is_directory(out, ec) || (ext != ".glb" && ext != ".obj"))
        out = (out.empty() ? opt.workDir : out) / (name + (head == "cp" ? "_cyberpunk_head" : "_fallout4_head") + ".glb");
    fs::create_directories(longPath(out.parent_path()), ec);
    writeModel(parts, out);
    size_t verts = 0, tris = 0;
    for (const auto& p : parts) verts += p.pos.size(), tris += p.idx.size() / 3;
    r << "Model             : " << u8(out) << "\n";
    r << "                    " << parts.size() << (parts.size() == 1 ? " mesh, " : " meshes, ") << verts << " vertices, "
      << tris << " triangles (metres, Y up, both heads in the same place)\n";
    Log::info("Model            " + u8(out));
    res.file = out;
    res.report = r.str();
    writeFileText(opt.workDir / "report.txt", res.report);
    return res;
}

} // namespace ptk
