#include "LooksMenuPreset.h"

#include "Util.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

namespace ptk {

namespace {

// LooksMenu stores IDs as hex strings, but be tolerant of decimal numbers too.
bool idFromJson(const json& j, uint32_t& out) {
    if (j.is_number_unsigned() || j.is_number_integer()) { out = j.get<uint32_t>(); return true; }
    if (j.is_string()) {
        if (auto v = parseHexId(j.get<std::string>())) { out = *v; return true; }
    }
    return false;
}

const json* findKeyCI(const json& obj, std::initializer_list<const char*> names) {
    if (!obj.is_object()) return nullptr;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        std::string k = toLower(it.key());
        for (const char* n : names)
            if (k == toLower(n)) return &it.value();
    }
    return nullptr;
}

std::vector<double> numberArray(const json& j) {
    std::vector<double> v;
    if (!j.is_array()) return v;
    for (const auto& e : j)
        if (e.is_number()) v.push_back(e.get<double>());
    return v;
}

void parseRegions(const json& j, LooksMenuPreset& p) {
    if (j.is_null()) return;
    if (j.is_object()) {
        // { "7BC78": [x,y,z, rx,ry,rz, s, ...], ... }
        for (auto it = j.begin(); it != j.end(); ++it) {
            auto id = parseHexId(it.key());
            if (!id) { p.parseWarnings.push_back("region key is not hex: " + it.key()); continue; }
            RegionSculpt r;
            r.id = *id;
            if (it.value().is_array()) r.values = numberArray(it.value());
            else if (it.value().is_object()) {
                if (const json* v = findKeyCI(it.value(), {"values", "value", "data", "transform"}))
                    r.values = numberArray(*v);
            }
            if (r.values.empty()) { p.parseWarnings.push_back("region " + it.key() + " has no numeric data"); continue; }
            p.regions.push_back(std::move(r));
        }
        return;
    }
    if (j.is_array()) {
        size_t idx = 0;
        for (const auto& e : j) {
            RegionSculpt r;
            if (e.is_object()) {
                const json* idj = findKeyCI(e, {"id", "key", "index", "region"});
                const json* vj = findKeyCI(e, {"values", "value", "data", "transform"});
                if (!idj || !idFromJson(*idj, r.id) || !vj) {
                    p.parseWarnings.push_back("unrecognised region entry #" + std::to_string(idx));
                    ++idx;
                    continue;
                }
                r.values = numberArray(*vj);
            } else if (e.is_array() && !e.empty()) {
                // [id, x,y,z, rx,ry,rz, s, ...] with id as hex string or number
                if (!idFromJson(e[0], r.id)) {
                    p.parseWarnings.push_back("region entry #" + std::to_string(idx) + " has no id in slot 0");
                    ++idx;
                    continue;
                }
                for (size_t i = 1; i < e.size(); ++i)
                    if (e[i].is_number()) r.values.push_back(e[i].get<double>());
            }
            if (!r.values.empty()) p.regions.push_back(std::move(r));
            ++idx;
        }
    }
}

void parseKeyedValues(const json& j, std::map<uint32_t, double>& out, LooksMenuPreset& p, const char* what) {
    if (j.is_null()) return;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            auto id = parseHexId(it.key());
            if (!id) { p.parseWarnings.push_back(std::string(what) + " key is not hex: " + it.key()); continue; }
            if (it.value().is_number()) out[*id] = it.value().get<double>();
        }
    } else if (j.is_array()) {
        size_t i = 0;
        for (const auto& e : j) {
            if (e.is_number()) {
                // plain list of values (e.g. "Values": [0.41, 0, 0, 0, 0]) -> keyed by position
                if (e.get<double>() != 0.0) out[(uint32_t)i] = e.get<double>();
            } else if (e.is_object()) {
                const json* k = findKeyCI(e, {"key", "id", "index"});
                const json* v = findKeyCI(e, {"value", "weight"});
                uint32_t id;
                if (k && idFromJson(*k, id)) out[id] = (v && v->is_number()) ? v->get<double>() : 1.0;
            } else if (e.is_array() && e.size() >= 2) {
                uint32_t id;
                if (idFromJson(e[0], id) && e[1].is_number()) out[id] = e[1].get<double>();
            } else {
                p.parseWarnings.push_back(std::string(what) + " entry #" + std::to_string(i) + " not understood");
            }
            ++i;
        }
    }
}

} // namespace

LooksMenuPreset loadLooksMenuPreset(const std::filesystem::path& path) {
    json j;
    try {
        j = json::parse(readFileText(path), nullptr, true, /*ignore_comments*/ true);
    } catch (const json::exception& e) {
        throw std::runtime_error("preset is not valid JSON (" + path.u8string() + "): " + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("preset root is not a JSON object: " + path.u8string());

    LooksMenuPreset p;
    p.name = path.stem().u8string();

    if (const json* g = findKeyCI(j, {"Gender", "Sex"})) {
        if (g->is_number()) p.gender = g->get<int>();
        else if (g->is_boolean()) p.gender = g->get<bool>() ? 1 : 0;
        else if (g->is_string()) {
            std::string s = toLower(g->get<std::string>());
            if (s == "female" || s == "f" || s == "1") p.gender = 1;
            else if (s == "male" || s == "m" || s == "0") p.gender = 0;
        }
    }
    if (const json* r = findKeyCI(j, {"Race"}); r && r->is_string()) p.race = r->get<std::string>();

    const json* morphs = findKeyCI(j, {"Morphs"});
    if (!morphs || morphs->is_null()) {
        p.parseWarnings.push_back("preset has no \"Morphs\" section - no face sculpt data");
        return p;
    }
    if (const json* r = findKeyCI(*morphs, {"Regions"})) parseRegions(*r, p);
    if (const json* v = findKeyCI(*morphs, {"Values"})) parseKeyedValues(*v, p.morphValues, p, "Values");
    if (const json* pr = findKeyCI(*morphs, {"Presets"})) parseKeyedValues(*pr, p.presetWeights, p, "Presets");
    return p;
}

std::string looksMenuPresetJson(const LooksMenuPreset& preset, const std::vector<double>& weight,
                                const std::filesystem::path& basePresetJson) {
    nlohmann::ordered_json out = nlohmann::ordered_json::object();
    if (!basePresetJson.empty()) {
        out = nlohmann::ordered_json::parse(readFileText(basePresetJson), nullptr, true, true);
        if (!out.is_object()) throw std::runtime_error(basePresetJson.u8string() + " is not a LooksMenu preset");
        // head parts and tint layers are per sex: a base preset of the other sex only lends its body weight
        if (preset.gender >= 0 && out.contains("Gender") && out["Gender"].is_number() && out["Gender"].get<int>() != preset.gender) {
            Log::warn("the base preset is for the other sex - only its body weight is used");
            for (const char* k : {"HeadParts", "Tints", "TintOrder", "HairColor", "BodyMorphs", "Overlays"}) out.erase(k);
        }
    }
    if (preset.gender >= 0) out["Gender"] = preset.gender;
    if (!preset.hairColor.empty()) out["HairColor"] = preset.hairColor;
    nlohmann::ordered_json morphs = nlohmann::ordered_json::object();
    nlohmann::ordered_json presets = nlohmann::ordered_json::object();
    for (const auto& [k, v] : preset.presetWeights) presets[toHex(k)] = v;
    morphs["Presets"] = presets;
    nlohmann::ordered_json regions = nlohmann::ordered_json::object();
    for (const auto& r : preset.regions) {
        nlohmann::ordered_json a = nlohmann::ordered_json::array();
        for (size_t i = 0; i < 8; ++i) a.push_back(i < r.values.size() ? r.values[i] : 0.0);
        regions[toHex(r.id)] = a;
    }
    morphs["Regions"] = regions;
    nlohmann::ordered_json values = nlohmann::ordered_json::array();
    if (!preset.morphValues.empty()) {
        uint32_t last = preset.morphValues.rbegin()->first;
        for (uint32_t i = 0; i <= last && i < 256; ++i) {
            auto it = preset.morphValues.find(i);
            values.push_back(it == preset.morphValues.end() ? 0.0 : it->second);
        }
    }
    // LooksMenu always allocates 5 slider values and only fills as many as the preset has: the
    // rest would be uninitialised memory (the face explodes). Always write all 5.
    while (values.size() < 5) values.push_back(0.0);
    morphs["Values"] = values;
    out["Morphs"] = morphs;
    if (weight.size() == 3) out["Weight"] = weight;
    // LooksMenu reads a missing Weight as 0/0/0 (thinnest body): always write one
    if (!out.contains("Weight") || !out["Weight"].is_array() || out["Weight"].size() != 3)
        out["Weight"] = std::vector<double>{1.0 / 3, 1.0 / 3, 1.0 / 3};

    // head parts: LooksMenu resets to the race defaults, then swaps in each listed part by type
    if (!preset.headParts.empty()) {
        nlohmann::ordered_json parts = nlohmann::ordered_json::array();
        if (out.contains("HeadParts") && out["HeadParts"].is_array())
            for (const auto& p : out["HeadParts"]) {
                const std::string s2 = p.is_string() ? p.get<std::string>() : std::string();
                bool replaced = false;
                for (const auto& r : preset.replaceHeadParts)
                    if (toLower(s2) == toLower(r)) replaced = true;
                if (!replaced && !s2.empty()) parts.push_back(s2);
            }
        for (const auto& h : preset.headParts) parts.push_back(h);
        out["HeadParts"] = parts;
    }
    // tints: LooksMenu replaces the whole list, so keep the base preset's other tints
    if (!preset.tints.empty()) {
        nlohmann::ordered_json tints = nlohmann::ordered_json::object();
        nlohmann::ordered_json order = nlohmann::ordered_json::array();
        auto isReplaced = [&](const std::string& key) {
            uint32_t k = 0;
            try { k = uint32_t(std::stoul(key, nullptr, 16)); } catch (...) { return false; }
            for (uint32_t r : preset.replaceTints)
                if (r == k) return true;
            for (const auto& t : preset.tints)
                if (t.index == k) return true;
            return false;
        };
        if (out.contains("Tints") && out["Tints"].is_object())
            for (auto it = out["Tints"].begin(); it != out["Tints"].end(); ++it)
                if (!isReplaced(it.key())) tints[it.key()] = it.value();
        if (out.contains("TintOrder") && out["TintOrder"].is_array())
            for (const auto& k : out["TintOrder"])
                if (k.is_string() && tints.contains(k.get<std::string>())) order.push_back(k);
        for (auto it = tints.begin(); it != tints.end(); ++it) {
            bool listed = false;
            for (const auto& k : order)
                if (k == it.key()) listed = true;
            if (!listed) order.push_back(it.key());
        }
        // skin tone first (it is the base layer), then the rest in the given order
        std::vector<const PresetTint*> mine;
        for (const auto& t : preset.tints) mine.push_back(&t);
        std::stable_sort(mine.begin(), mine.end(), [](auto* a, auto* b) { return (a->kind == "skin") > (b->kind == "skin"); });
        nlohmann::ordered_json ordered = nlohmann::ordered_json::array();
        for (const auto* t : mine)
            if (t->kind == "skin") ordered.push_back(toHex(t->index));
        for (const auto& k : order) ordered.push_back(k);
        for (const auto* t : mine) {
            nlohmann::ordered_json e = nlohmann::ordered_json::object();
            if (t->type == 1) {
                e["Color"] = t->color;
                e["ColorID"] = t->colorId;
            }
            e["Percent"] = t->percent;
            e["Type"] = t->type;
            tints[toHex(t->index)] = e;
            if (t->kind != "skin") ordered.push_back(toHex(t->index));
        }
        out["Tints"] = tints;
        out["TintOrder"] = ordered;
    }
    return out.dump(3);
}

} // namespace ptk
