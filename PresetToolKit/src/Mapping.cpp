#include "Mapping.h"

#include "Util.h"

#include <nlohmann/json.hpp>
#include <cctype>
#include <stdexcept>

using nlohmann::json;

namespace ptk {

namespace {

Vec3 vec3(const json& j, const Vec3& def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

uint32_t hexOrThrow(const json& j, const std::string& ctx) {
    if (j.is_number()) return j.get<uint32_t>();
    if (j.is_string())
        if (auto v = parseHexId(j.get<std::string>())) return *v;
    throw std::runtime_error("mapping: bad hex id in " + ctx);
}

} // namespace

const RegionDef* Mapping::findRegion(const std::string& name) const {
    for (const auto& r : regions)
        if (r.name == name) return &r;
    return nullptr;
}

std::optional<std::string> Mapping::regionForId(uint32_t id) const {
    if (auto it = idOverrides.find(id); it != idOverrides.end()) return it->second;
    if (auto it = fo4RegionNames.find(id); it != fo4RegionNames.end())
        if (auto r = regionForName(it->second)) return r;
    for (const auto& r : ranges)
        if (id >= r.from && id <= r.to) return r.region;
    return std::nullopt;
}

void Mapping::selectGender(const std::string& gender) {
    const BoneSet* set = nullptr;
    if (auto it = boneSets.find(gender); it != boneSets.end() && it->second.present) set = &it->second;
    else if (auto g = boneSets.find(""); g != boneSets.end() && g->second.present) set = &g->second;
    if (!set) return;
    boneLocations = set->locations;
    // the generic set fills in bones the gender set does not have
    if (auto g = boneSets.find(""); g != boneSets.end() && set != &g->second)
        for (const auto& [k, v] : g->second.locations) boneLocations.emplace(k, v);
    fo4UnitMeters = set->metersPerUnit;
    fo4RotationScale = set->rotationScale;
    fo4ScaleScale = set->scaleScale;
    fo4Strength = set->strength;
}

const BoneLocation* Mapping::findBone(char side, const std::string& key, bool* mirrored) const {
    if (mirrored) *mirrored = false;
    std::string full = std::string(1, (char)std::tolower((unsigned char)side)) + "_" + key;
    if (auto it = boneLocations.find(full); it != boneLocations.end()) return &it->second;
    if (auto it = boneLocations.find(key); it != boneLocations.end()) {
        if (mirrored) *mirrored = (side == 'L');
        return &it->second;
    }
    if (side == 'L') // calibrated right side reused for the left
        if (auto it = boneLocations.find("r_" + key); it != boneLocations.end()) {
            if (mirrored) *mirrored = true;
            return &it->second;
        }
    return nullptr;
}

std::optional<std::string> Mapping::regionForName(const std::string& fo4Name) const {
    const std::string n = toLower(fo4Name);
    for (const auto& rule : nameRules) {
        const std::string m = toLower(rule.match);
        for (size_t pos = n.find(m); pos != std::string::npos; pos = n.find(m, pos + 1)) {
            bool wordStart = pos == 0 || !std::isalpha((unsigned char)n[pos - 1]);
            if (wordStart) return rule.region;
        }
    }
    return std::nullopt;
}

std::string Mapping::fo4NameOf(uint32_t id) const {
    auto it = fo4RegionNames.find(id);
    return it == fo4RegionNames.end() ? std::string() : it->second;
}

void splitBoneName(const std::string& bone, char& side, std::string& key) {
    std::string b = bone;
    side = 'C';
    if (toLower(b).rfind("bone_", 0) == 0) b = b.substr(5);
    if (b.size() > 2 && b[1] == '_' && (b[0] == 'L' || b[0] == 'R' || b[0] == 'C' || b[0] == 'l' || b[0] == 'r' || b[0] == 'c')) {
        side = (char)std::toupper((unsigned char)b[0]);
        b = b.substr(2);
    }
    key = toLower(b);
}

std::map<uint32_t, Fo4Region> loadFo4Regions(const std::filesystem::path& path) {
    std::map<uint32_t, Fo4Region> out;
    json j;
    try {
        j = json::parse(readFileText(path), nullptr, true, true);
    } catch (const json::exception&) {
        return out;
    }
    auto v3 = [](const json& o, const char* k) {
        Vec3 r;
        if (o.is_object() && o.contains(k) && o[k].is_object()) {
            const auto& t = o[k];
            r = {t.value("x", 0.0), t.value("y", 0.0), t.value("z", 0.0)};
        }
        return r;
    };
    const json* arr = &j;
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_array()) { arr = &it.value(); break; }
    if (!arr->is_array()) return out;
    for (const auto& r : *arr) {
        if (!r.is_object() || !r.contains("ID") || !r["ID"].is_number()) continue;
        Fo4Region reg;
        reg.id = r["ID"].get<uint32_t>();
        reg.name = r.value("Name", std::string());
        reg.group = r.value("AssociatedMorphGroup", r.value("AssociatedTintGroup", std::string()));
        if (r.contains("BonesA") && r["BonesA"].is_array())
            for (const auto& b : r["BonesA"]) {
                Fo4Bone bone;
                bone.name = b.value("Bone", std::string());
                const json mx = b.value("Maxima", json::object()), mn = b.value("Minima", json::object());
                bone.posMax = v3(mx, "Position"); bone.posMin = v3(mn, "Position");
                bone.rotMax = v3(mx, "Rotation"); bone.rotMin = v3(mn, "Rotation");
                bone.sclMax = v3(mx, "Scale");    bone.sclMin = v3(mn, "Scale");
                if (!bone.name.empty()) reg.bones.push_back(bone);
            }
        out[reg.id] = reg;
    }
    return out;
}

std::map<uint32_t, std::string> loadFo4RegionNames(const std::filesystem::path& path) {
    const std::string text = readFileText(path);
    std::map<uint32_t, std::string> out;
    // Find every  "ID" : <number>  and take the name-like string field from the same
    // innermost { ... } block. Works whether or not the file is strictly valid JSON.
    std::vector<size_t> open;
    std::vector<std::pair<size_t, size_t>> blocks; // innermost-first order not needed
    bool inStr = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
        if (c == '"') inStr = true;
        else if (c == '{') open.push_back(i);
        else if (c == '}' && !open.empty()) { blocks.push_back({open.back(), i}); open.pop_back(); }
    }
    auto stringField = [&](const std::string& blk, const std::string& keyLower) -> std::string {
        // returns the first "key" : "value" whose key contains keyLower
        size_t p = 0;
        while ((p = blk.find('"', p)) != std::string::npos) {
            size_t e = blk.find('"', p + 1);
            if (e == std::string::npos) break;
            std::string key = toLower(blk.substr(p + 1, e - p - 1));
            size_t c = blk.find_first_not_of(" \t\r\n", e + 1);
            if (c != std::string::npos && blk[c] == ':') {
                size_t v = blk.find_first_not_of(" \t\r\n", c + 1);
                if (v != std::string::npos && blk[v] == '"' && key.find(keyLower) != std::string::npos) {
                    size_t ve = blk.find('"', v + 1);
                    if (ve != std::string::npos) return blk.substr(v + 1, ve - v - 1);
                }
            }
            p = e + 1;
        }
        return {};
    };
    for (const auto& [b, e] : blocks) {
        std::string blk = text.substr(b, e - b + 1);
        // only use blocks whose own nesting level holds the ID (skip parents of nested blocks)
        size_t idPos = std::string::npos, p = 0;
        while ((p = blk.find('"', p)) != std::string::npos) {
            size_t q = blk.find('"', p + 1);
            if (q == std::string::npos) break;
            if (toLower(blk.substr(p + 1, q - p - 1)) == "id") { idPos = q; break; }
            p = q + 1;
        }
        if (idPos == std::string::npos) continue;
        if (blk.find('{', 1) != std::string::npos && blk.find('{', 1) < idPos) continue;
        size_t colon = blk.find(':', idPos);
        if (colon == std::string::npos) continue;
        size_t ns = blk.find_first_of("0123456789", colon);
        if (ns == std::string::npos) continue;
        uint32_t id = (uint32_t)std::stoul(blk.substr(ns));
        std::string name = stringField(blk, "name");
        if (name.empty()) name = stringField(blk, "label");
        if (!name.empty() && !out.count(id)) out[id] = name;
    }
    return out;
}

Mapping loadMapping(const std::filesystem::path& path) {
    json j;
    try {
        j = json::parse(readFileText(path), nullptr, true, /*ignore_comments*/ true);
    } catch (const json::exception& e) {
        throw std::runtime_error("mapping file is not valid JSON (" + path.u8string() + "): " + e.what());
    }
    Mapping m;

    if (j.contains("frame")) {
        const auto& f = j["frame"];
        m.up = f.value("up", m.up);
        m.forward = f.value("forward", m.forward);
    }
    if (j.contains("gains")) {
        const auto& g = j["gains"];
        m.positionGain = g.value("position", m.positionGain);
        m.rotationGainDeg = g.value("rotationDegrees", m.rotationGainDeg);
        m.scaleGain = g.value("scale", m.scaleGain);
        m.globalStrength = g.value("strength", m.globalStrength);
        m.clampValue = g.value("clamp", m.clampValue);
        m.combine = g.value("combine", m.combine);
    }
    if (j.contains("regionValueLayout")) {
        // e.g. ["px","py","pz","rx","ry","rz","s","unused"]
        for (int i = 0; i < 3; ++i) { m.idxPos[i] = -1; m.idxRot[i] = -1; }
        m.idxScale = -1;
        int i = 0;
        for (const auto& e : j["regionValueLayout"]) {
            std::string k = toLower(e.get<std::string>());
            if (k == "px") m.idxPos[0] = i; else if (k == "py") m.idxPos[1] = i; else if (k == "pz") m.idxPos[2] = i;
            else if (k == "rx") m.idxRot[0] = i; else if (k == "ry") m.idxRot[1] = i; else if (k == "rz") m.idxRot[2] = i;
            else if (k == "s" || k == "scale") m.idxScale = i;
            ++i;
        }
    }
    if (j.contains("regions")) {
        for (auto it = j["regions"].begin(); it != j["regions"].end(); ++it) {
            if (it.key().rfind("_", 0) == 0) continue; // "_comment" keys
            RegionDef r;
            r.name = it.key();
            const auto& v = it.value();
            r.center = vec3(v.value("center", json()), r.center);
            r.radius = vec3(v.value("radius", json()), r.radius);
            r.mirror = v.value("mirror", false);
            r.strength = v.value("strength", 1.0);
            m.regions.push_back(r);
        }
    }
    if (j.contains("fo4RegionRanges")) {
        for (const auto& e : j["fo4RegionRanges"]) {
            IdRange r;
            r.from = hexOrThrow(e.at("from"), "fo4RegionRanges.from");
            r.to = e.contains("to") ? hexOrThrow(e["to"], "fo4RegionRanges.to") : r.from;
            r.region = e.at("region").get<std::string>();
            m.ranges.push_back(r);
        }
    }
    if (j.contains("idOverrides")) {
        for (auto it = j["idOverrides"].begin(); it != j["idOverrides"].end(); ++it) {
            if (it.key().rfind("_", 0) == 0) continue;
            auto id = parseHexId(it.key());
            if (!id) throw std::runtime_error("mapping: idOverrides key not hex: " + it.key());
            m.idOverrides[*id] = it.value().get<std::string>();
        }
    }
    auto parseBoneSet = [&](const json& fb, BoneSet& set) {
        set.present = true;
        set.metersPerUnit = fb.value("metersPerUnit", set.metersPerUnit);
        set.rotationScale = fb.value("rotationScale", set.rotationScale);
        set.scaleScale = fb.value("scaleScale", set.scaleScale);
        set.strength = fb.value("strength", set.strength);
        if (!fb.contains("locations")) return;
        for (auto it = fb["locations"].begin(); it != fb["locations"].end(); ++it) {
            if (it.key().rfind("_", 0) == 0) continue;
            const json& v = it.value();
            BoneLocation bl;
            bl.center = vec3(v.value("center", json()), bl.center);
            bl.radius = vec3(v.value("radius", json()), bl.radius);
            bl.pivot = bl.center;
            if (v.contains("pivot")) { bl.pivot = vec3(v["pivot"], bl.center); bl.hasPivot = true; }
            if (v.contains("axes") && v["axes"].is_array() && v["axes"].size() == 3) {
                for (int k = 0; k < 3; ++k) bl.axes[k] = vec3(v["axes"][k], Vec3());
                bl.hasAxes = true;
            }
            set.locations[toLower(it.key())] = bl;
        }
    };
    if (j.contains("fo4Bones")) parseBoneSet(j["fo4Bones"], m.boneSets[""]);
    if (j.contains("fo4BonesFemale")) parseBoneSet(j["fo4BonesFemale"], m.boneSets["female"]);
    if (j.contains("fo4BonesMale")) parseBoneSet(j["fo4BonesMale"], m.boneSets["male"]);
    m.selectGender("female");
    if (j.contains("nameRules")) {
        for (const auto& e : j["nameRules"]) {
            NameRule r;
            r.match = e.at("match").get<std::string>();
            r.region = e.at("region").get<std::string>();
            m.nameRules.push_back(r);
        }
    }
    if (j.contains("valueRules")) {
        for (auto it = j["valueRules"].begin(); it != j["valueRules"].end(); ++it) {
            if (it.key().rfind("_", 0) == 0) continue;
            auto id = parseHexId(it.key());
            if (!id) throw std::runtime_error("mapping: valueRules key not hex: " + it.key());
            const auto& v = it.value();
            ValueRule r;
            r.region = v.at("region").get<std::string>();
            r.translate = vec3(v.value("translate", json()), {});
            r.rotate = vec3(v.value("rotate", json()), {});
            r.scale = v.value("scale", 0.0);
            m.valueRules[*id] = r;
        }
    }
    if (j.contains("headDepotPath"))
        for (auto it = j["headDepotPath"].begin(); it != j["headDepotPath"].end(); ++it)
            if (it.value().is_string()) m.headDepotPath[it.key()] = it.value().get<std::string>();
    if (j.contains("headMeshDepotPath"))
        for (auto it = j["headMeshDepotPath"].begin(); it != j["headMeshDepotPath"].end(); ++it)
            if (it.value().is_string()) m.headMeshDepotPath[it.key()] = it.value().get<std::string>();
    if (j.contains("commands"))
        for (auto it = j["commands"].begin(); it != j["commands"].end(); ++it)
            if (it.value().is_string()) m.commands[it.key()] = it.value().get<std::string>();
    m.glbNaming = j.value("glbNaming", m.glbNaming);
    m.stripSkinForMorphImport = j.value("stripSkinForMorphtargetImport", m.stripSkinForMorphImport);

    // Validate region references
    for (const auto& r : m.ranges)
        if (!m.findRegion(r.region))
            throw std::runtime_error("mapping: range " + toHex(r.from) + " references unknown region '" + r.region + "'");
    for (const auto& [id, reg] : m.idOverrides)
        if (!m.findRegion(reg))
            throw std::runtime_error("mapping: idOverride " + toHex(id) + " references unknown region '" + reg + "'");
    for (const auto& r : m.nameRules)
        if (!m.findRegion(r.region))
            throw std::runtime_error("mapping: nameRule '" + r.match + "' references unknown region '" + r.region + "'");
    for (const auto& [id, rule] : m.valueRules)
        if (!m.findRegion(rule.region))
            throw std::runtime_error("mapping: valueRule " + toHex(id) + " references unknown region '" + rule.region + "'");
    return m;
}

} // namespace ptk
