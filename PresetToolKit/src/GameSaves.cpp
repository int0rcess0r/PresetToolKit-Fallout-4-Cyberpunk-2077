#include "GameSaves.h"
#include "Util.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <array>
#include <set>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ptk {

namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    size_t o = 0;
    explicit Reader(const std::vector<uint8_t>& data, size_t start = 0) : d(data), o(start) {}
    void need(size_t n) const {
        if (o + n > d.size()) throw std::runtime_error("unexpected end of save data");
    }
    uint8_t u8() { need(1); return d[o++]; }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, &d[o], 2); o += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, &d[o], 4); o += 4; return v; }
    uint64_t u64() { need(8); uint64_t v; std::memcpy(&v, &d[o], 8); o += 8; return v; }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    void skip(size_t n) { need(n); o += n; }
    std::string bytes(size_t n) { need(n); std::string s(reinterpret_cast<const char*>(&d[o]), n); o += n; return s; }
    // Bethesda wstring: u16 length + chars
    std::string wstr() { uint16_t n = u16(); return bytes(n); }
    // CDPR VLQ: first byte sign (bit 7), continue (bit 6), 6 value bits; then 7 bits + continue (bit 7)
    int32_t vlq() {
        uint8_t b = u8();
        const bool neg = (b & 0x80) != 0;
        uint32_t v = b & 0x3F;
        if (b & 0x40) {
            int shift = 6;
            do {
                b = u8();
                v |= uint32_t(b & 0x7F) << shift;
                shift += 7;
            } while ((b & 0x80) && shift < 35);
        }
        return neg ? -int32_t(v) : int32_t(v);
    }
    // CDPR length-prefixed string: VLQ length, negative = 8-bit chars, positive = UTF-16
    std::string lpstr() {
        int32_t p = vlq();
        size_t n = size_t(p < 0 ? -p : p);
        if (n > 4096) throw std::runtime_error("bad string in save data");
        if (p < 0) return bytes(n);
        std::string s;
        for (size_t i = 0; i < n; ++i) {
            uint16_t c = u16();
            s += c < 128 ? char(c) : '?';
        }
        return s;
    }
};

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    uint32_t v;
    std::memcpy(&v, &d[o], 4);
    return v;
}
float rdf(const std::vector<uint8_t>& d, size_t o) {
    float f;
    std::memcpy(&f, &d[o], 4);
    return f;
}
bool sane(float f, float lim) { return std::isfinite(f) && std::fabs(f) <= lim; }

std::string lowerExt(const fs::path& p) { return toLower(p.extension().u8string()); }

// ------------------------------------------------------------------ FO4 face block
struct Fo4Face {
    std::vector<RegionSculpt> regions;
    std::map<uint32_t, double> presets;
    std::map<uint32_t, double> values;
    std::vector<double> weight;
    bool found = false;
};

// The player's NPC_ change form ends with the face data:
//   u32 n + n x (u32 presetKey, f32 weight)                  Morphs.Presets
//   u32 n + n x (u32 regionId, 8 x f32, f32 unused)           Morphs.Regions
//   ... tints / head parts ...
//   u32 n + n x f32                                           Morphs.Values
//   3 x f32                                                   body weight (thin / muscular / large)
Fo4Face parseFo4Face(const std::vector<uint8_t>& b) {
    Fo4Face face;
    const size_t len = b.size();

    // --- regions: the longest valid "count + 40-byte records" run, preferably right after the presets list
    size_t bestPos = 0, bestN = 0;
    bool bestHasPresets = false;
    size_t bestPresetPos = 0, bestPresetN = 0;
    for (size_t o = 0; o + 4 + 40 <= len; ++o) {
        uint32_t n = rd32(b, o);
        if (n < 1 || n > 300 || o + 4 + size_t(n) * 40 > len) continue;
        bool ok = true;
        std::vector<uint32_t> ids;
        for (uint32_t k = 0; k < n && ok; ++k) {
            size_t r = o + 4 + size_t(k) * 40;
            for (int f = 0; f < 9 && ok; ++f) ok = sane(rdf(b, r + 4 + 4 * f), 20.f);
            ids.push_back(rd32(b, r));
        }
        if (!ok) continue;
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) continue;
        // presets list ending exactly here?
        bool hasPresets = false;
        size_t pPos = 0, pN = 0;
        for (size_t m = 0; m <= 300 && 4 + 8 * m <= o; ++m) {
            size_t p = o - 4 - 8 * m;
            if (rd32(b, p) != m) continue;
            bool pv = true;
            for (size_t k = 0; k < m && pv; ++k) pv = sane(rdf(b, p + 4 + 8 * k + 4), 20.f);
            if (pv) { hasPresets = true; pPos = p; pN = m; }
        }
        if ((hasPresets && !bestHasPresets) || (hasPresets == bestHasPresets && n > bestN)) {
            bestPos = o; bestN = n; bestHasPresets = hasPresets; bestPresetPos = pPos; bestPresetN = pN;
        }
    }
    if (bestN > 0) {
        face.found = true;
        for (uint32_t k = 0; k < bestN; ++k) {
            size_t r = bestPos + 4 + size_t(k) * 40;
            RegionSculpt s;
            s.id = rd32(b, r);
            for (int f = 0; f < 8; ++f) s.values.push_back(rdf(b, r + 4 + 4 * f));
            face.regions.push_back(s);
        }
        if (bestHasPresets)
            for (size_t k = 0; k < bestPresetN; ++k)
                face.presets[rd32(b, bestPresetPos + 4 + 8 * k)] = rdf(b, bestPresetPos + 4 + 8 * k + 4);
    }

    // --- tail: Values + body weight
    if (len >= 16) {
        bool wOk = true;
        std::vector<double> w;
        for (int i = 0; i < 3; ++i) {
            float f = rdf(b, len - 12 + 4 * i);
            wOk &= std::isfinite(f) && f >= -0.01f && f <= 1.01f;
            w.push_back(f);
        }
        if (wOk) {
            face.weight = w;
            for (size_t k = 64; k-- > 0;) {
                if (len < 12 + 4 * k + 4) continue;
                size_t p = len - 12 - 4 * k - 4;
                if (rd32(b, p) != k) continue;
                bool ok = true;
                for (size_t i = 0; i < k && ok; ++i) ok = sane(rdf(b, p + 4 + 4 * i), 10.f);
                if (!ok) continue;
                for (size_t i = 0; i < k; ++i) face.values[uint32_t(i)] = rdf(b, p + 4 + 4 * i);
                face.found = true;
                break;
            }
        }
    }
    return face;
}

} // namespace

// ======================================================================== Fallout 4
bool isFo4Save(const fs::path& p) {
    if (lowerExt(p) == ".fos") return true;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    auto d = readFileBytes(p);
    return d.size() > 12 && std::memcmp(d.data(), "FO4_SAVEGAME", 12) == 0;
}

LooksMenuPreset loadFo4Save(const fs::path& path, Fo4SaveInfo* infoOut) {
    const std::vector<uint8_t> d = readFileBytes(path);
    if (d.size() < 16 || std::memcmp(d.data(), "FO4_SAVEGAME", 12) != 0)
        throw std::runtime_error(path.filename().u8string() + " is not a Fallout 4 save");
    Fo4SaveInfo info;
    LooksMenuPreset preset;
    preset.name = path.stem().u8string();

    Reader r(d, 12);
    const uint32_t headerSize = r.u32();
    const size_t headerEnd = r.o + headerSize;
    r.u32();                     // version
    r.u32();                     // save number
    info.playerName = r.wstr();
    info.level = int(r.u32());
    r.wstr();                    // location
    r.wstr();                    // play time
    info.race = r.wstr();
    const uint16_t sex = r.u16();
    preset.gender = sex == 1 ? 1 : 0;
    preset.race = info.race;
    r.o = headerEnd - 8;
    const uint32_t shotW = r.u32(), shotH = r.u32();
    r.o = headerEnd + size_t(shotW) * shotH * 4;   // RGBA screenshot

    r.u8();                      // form version
    r.wstr();                    // game version
    const uint32_t pluginInfoSize = r.u32();
    r.skip(pluginInfoSize);
    // file location table
    r.u32(); r.u32(); r.u32(); r.u32();
    const uint32_t changeFormsOffset = r.u32();
    r.u32(); r.u32(); r.u32(); r.u32();
    const uint32_t changeFormCount = r.u32();

    // The player's face is on the player's base NPC_ (0x000007) - or, in saves from the prologue's
    // mirror scene, on the spouse NPC of the same sex (MQ101PlayerSpouseMale 0x0A7D34 / ...Female 0x0A7D35).
    constexpr uint32_t kPlayer = 0x400007, kSpouseMale = 0x4A7D34, kSpouseFemale = 0x4A7D35;
    constexpr uint32_t CHANGE_NPC_FACE = 0x800;
    std::map<uint32_t, std::vector<uint8_t>> forms;
    std::map<uint32_t, uint32_t> formFlags;
    Reader c(d, changeFormsOffset);
    for (uint32_t i = 0; i < changeFormCount; ++i) {
        const uint32_t refId = (uint32_t(c.u8()) << 16) | (uint32_t(c.u8()) << 8) | c.u8();
        const uint32_t flags = c.u32();
        const uint8_t type = c.u8();
        c.u8();  // version
        uint32_t len1 = 0, len2 = 0;
        switch (type >> 6) {
        case 0: len1 = c.u8(); len2 = c.u8(); break;
        case 1: len1 = c.u16(); len2 = c.u16(); break;
        default: len1 = c.u32(); len2 = c.u32(); break;
        }
        c.need(len1);
        if (refId == kPlayer || refId == kSpouseMale || refId == kSpouseFemale) {
            std::vector<uint8_t> body;
            if (len2 > 0) {
                body.resize(len2);
                mz_ulong outLen = len2;
                if (mz_uncompress(body.data(), &outLen, &d[c.o], len1) == MZ_OK) body.resize(outLen);
                else body.clear();
            } else {
                body.assign(d.begin() + c.o, d.begin() + c.o + len1);
            }
            forms[refId] = std::move(body);
            formFlags[refId] = flags;
        }
        c.o += len1;
    }
    if (!forms.count(kPlayer))
        throw std::runtime_error("the player's character data was not found in " + path.filename().u8string());

    // female sculpt regions are 0x186A0.. and 0x274E/F; male ones are the rest
    auto femaleIds = [](const Fo4Face& f) {
        int fem = 0;
        for (const auto& r : f.regions) fem += (r.id >= 0x186A0 && r.id < 0x18800) || r.id == 0x274E || r.id == 0x274F;
        return fem * 2 > int(f.regions.size());
    };
    const bool female = preset.gender == 1;
    std::optional<Fo4Face> chosen;
    std::string from;
    for (uint32_t id : {kPlayer, female ? kSpouseFemale : kSpouseMale, female ? kSpouseMale : kSpouseFemale}) {
        auto it = forms.find(id);
        if (it == forms.end() || !(formFlags[id] & CHANGE_NPC_FACE)) continue;
        Fo4Face f = parseFo4Face(it->second);
        if (!f.found || (f.regions.empty() && f.presets.empty())) continue;
        if (id != kPlayer && !f.regions.empty() && femaleIds(f) != female) continue;   // the other spouse
        chosen = f;
        from = id == kPlayer ? "player" : "spouse NPC (prologue)";
        break;
    }
    if (!chosen) {
        preset.parseWarnings.push_back("this save has no face changes (the player still has the default face)");
    } else {
        info.hasFaceData = true;
        info.faceSource = from;
        preset.regions = chosen->regions;
        preset.presetWeights = chosen->presets;
        preset.morphValues = chosen->values;
        info.weight = chosen->weight;
    }
    if (!info.playerName.empty()) preset.name = info.playerName;
    if (infoOut) *infoOut = info;
    return preset;
}

LooksMenuPreset loadFo4Character(const fs::path& p, Fo4SaveInfo* info) {
    if (isFo4Save(p)) return loadFo4Save(p, info);
    return loadLooksMenuPreset(p);
}

// ======================================================================== LZ4
std::vector<uint8_t> lz4DecompressBlock(const uint8_t* src, size_t srcLen, size_t rawLen) {
    std::vector<uint8_t> out;
    out.reserve(rawLen);
    size_t i = 0;
    auto bad = []() { throw std::runtime_error("corrupt LZ4 data in the save"); };
    while (i < srcLen) {
        const uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= srcLen) bad();
                b = src[i++];
                lit += b;
            } while (b == 255);
        }
        if (i + lit > srcLen) bad();
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i >= srcLen) break;  // last sequence has no match
        if (i + 2 > srcLen) bad();
        const size_t off = size_t(src[i]) | (size_t(src[i + 1]) << 8);
        i += 2;
        if (off == 0 || off > out.size()) bad();
        size_t mlen = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (i >= srcLen) bad();
                b = src[i++];
                mlen += b;
            } while (b == 255);
        }
        size_t from = out.size() - off;
        for (size_t k = 0; k < mlen; ++k) out.push_back(out[from + k]);
        if (out.size() > rawLen) bad();
    }
    if (out.size() != rawLen) bad();
    return out;
}

// ======================================================================== Cyberpunk 2077
namespace {

fs::path savDatPath(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p / "sav.dat";
    return p;
}

bool isMorphRegion(const std::string& s) {
    return s == "eyes" || s == "nose" || s == "mouth" || s == "jaw" || s == "ear" || s == "ears";
}
bool isMorphTarget(const std::string& s) {
    return s.size() == 4 && s[0] == 'h' && std::isdigit((unsigned char)s[1]) && std::isdigit((unsigned char)s[2]) &&
           std::isdigit((unsigned char)s[3]);
}

struct CpGroupMorphs {
    std::string group;
    std::map<std::string, std::string> morphs;
    std::vector<std::pair<std::string, std::string>> apps;   // (name, definition)
};

// gameuiCharacterCustomizationPresetWrapper (game 2.x), as read by WolvenKit's save parser
bool parseAppearanceNode(const std::vector<uint8_t>& d, size_t start, size_t end, CpAppearance& out) {
    Reader r(d, start);
    r.u32();  // node id
    if (!r.u8()) {
        out.notes.push_back("the save holds no character customization data");
        return true;
    }
    r.u32();                       // unknown (cooking platform?)
    r.u32();                       // preset version
    out.isMale = r.u8() != 0;
    out.genderKnown = true;
    r.u8();                        // brain gender (voice)
    std::vector<CpGroupMorphs> groups;
    auto readGroups = [&](bool keep) {
        uint32_t n = r.u32();
        if (n > 1000) throw std::runtime_error("bad group count");
        for (uint32_t g = 0; g < n; ++g) {
            CpGroupMorphs gm;
            gm.group = r.lpstr();
            int32_t na = int32_t(r.u32());
            if (na < 0 || na > 10000) throw std::runtime_error("bad appearance count");
            for (int32_t i = 0; i < na; ++i) {
                r.u64();      // resource
                std::string def = r.lpstr();
                std::string name = r.lpstr();
                r.u32();      // censor flag
                r.u32();      // censor action
                gm.apps.push_back({name, def});
            }
            int32_t nm = int32_t(r.u32());
            if (nm < 0 || nm > 10000) throw std::runtime_error("bad morph count");
            for (int32_t i = 0; i < nm; ++i) {
                std::string region = r.lpstr(), target = r.lpstr();
                r.u32();
                r.u32();
                if (!region.empty() && !target.empty()) gm.morphs[region] = target;
            }
            if (keep) groups.push_back(gm);
            if (r.o > end) throw std::runtime_error("appearance data overruns its node");
        }
    };
    readGroups(true);  // head groups (arms/body are not needed)
    for (const char* name : {"character_customization", "TPP", "TPP_photomode"})
        for (const auto& g : groups)
            if (g.group == name && !g.morphs.empty() && out.morphs.empty()) out.morphs = g.morphs;
    if (out.morphs.empty())
        for (const auto& g : groups)
            if (!g.morphs.empty() && out.morphs.empty()) out.morphs = g.morphs;
    // hair: "hair_colorNN" -> colour definition; eyebrows: "eyebrows_colorN"
    for (const char* name : {"hairs", "character_customization"})
        for (const auto& g : groups)
            if (g.group == name)
                for (const auto& [n, def] : g.apps) {
                    if (out.hairColor.empty() && n.rfind("hair_color", 0) == 0 && n.find("fpp") == std::string::npos) {
                        out.hairColor = def;
                        try { out.hairStyle = std::stoi(n.substr(10)); } catch (...) {}
                    }
                    if (out.eyebrowColor.empty() && n.rfind("eyebrows_color", 0) == 0) out.eyebrowColor = def;
                }
    for (const auto& g : groups)
        for (const auto& [n, def] : g.apps)
            if (out.eyebrowColor.empty() && n.rfind("eyebrows_color", 0) == 0) out.eyebrowColor = def;
    // all head appearance choices: the character creator group, plus anything only in TPP / face
    for (const char* name : {"character_customization", "TPP", "face"})
        for (const auto& g : groups)
            if (g.group == name)
                for (const auto& a : g.apps)
                    if (std::find_if(out.appearances.begin(), out.appearances.end(),
                                     [&](const auto& x) { return x.first == a.first; }) == out.appearances.end())
                        out.appearances.push_back(a);
    return true;
}

// Fallback for layouts this parser does not know: pick region / target string pairs out of the bytes.
void scanAppearanceStrings(const std::vector<uint8_t>& d, size_t start, size_t end, CpAppearance& out) {
    std::string prevRegion;
    for (size_t o = start; o + 2 < end; ++o) {
        uint8_t b = d[o];
        if (!(b & 0x80) || (b & 0x40)) continue;  // short negative VLQ = 8-bit string
        size_t n = b & 0x3F;
        if (n < 3 || n > 40 || o + 1 + n > end) continue;
        std::string s(reinterpret_cast<const char*>(&d[o + 1]), n);
        if (!std::all_of(s.begin(), s.end(), [](char c) { return c >= 32 && c < 127; })) continue;
        if (isMorphRegion(s)) {
            prevRegion = s;
        } else if (isMorphTarget(s) && !prevRegion.empty()) {
            if (!out.morphs.count(prevRegion)) out.morphs[prevRegion] = s;
            prevRegion.clear();
        } else if (!out.genderKnown && s.find("_pma_") != std::string::npos) {
            out.isMale = true;
            out.genderKnown = true;
        } else if (!out.genderKnown && s.find("_pwa_") != std::string::npos) {
            out.isMale = false;
            out.genderKnown = true;
        }
    }
}

} // namespace

bool isCpSave(const fs::path& p) {
    const fs::path f = savDatPath(p);
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) return false;
    std::vector<uint8_t> head = readFileBytes(f);
    return head.size() > 4 && std::memcmp(head.data(), "VASC", 4) == 0;
}

CpAppearance loadCpSave(const fs::path& p) {
    const fs::path file = savDatPath(p);
    const std::vector<uint8_t> f = readFileBytes(file);
    CpAppearance out;
    out.saveName = file.parent_path().filename().u8string();
    if (f.size() < 32 || std::memcmp(f.data(), "VASC", 4) != 0)
        throw std::runtime_error(file.u8string() + " is not a Cyberpunk 2077 save (sav.dat)");

    // ---- chunk table ("FZLC") and LZ4 chunks ("4ZLX") -> the decompressed layout node offsets refer to
    size_t table = std::string::npos;
    for (size_t o = 4; o + 8 < std::min<size_t>(f.size(), 4096); ++o)
        if (std::memcmp(&f[o], "FZLC", 4) == 0) { table = o; break; }
    if (table == std::string::npos) throw std::runtime_error("save has no chunk table - unsupported save version");
    const uint32_t chunkCount = rd32(f, table + 4);
    if (chunkCount == 0 || table + 8 + size_t(chunkCount) * 12 > f.size())
        throw std::runtime_error("bad chunk table in the save");
    struct Chunk { uint32_t offset, csize, dsize; };
    std::vector<Chunk> chunks;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        size_t e = table + 8 + size_t(i) * 12;
        chunks.push_back({rd32(f, e), rd32(f, e + 4), rd32(f, e + 8)});
    }
    std::vector<uint8_t> data(f.begin(), f.begin() + chunks[0].offset);
    for (const auto& ch : chunks) {
        if (size_t(ch.offset) + ch.csize > f.size()) throw std::runtime_error("save chunk past end of file");
        const uint8_t* src = &f[ch.offset];
        if (ch.csize >= 8 && std::memcmp(src, "4ZLX", 4) == 0) {
            auto raw = lz4DecompressBlock(src + 8, ch.csize - 8, ch.dsize);
            data.insert(data.end(), raw.begin(), raw.end());
        } else {
            data.insert(data.end(), src, src + ch.csize);
        }
    }

    // ---- node table at the end of the file
    std::optional<std::pair<size_t, size_t>> node;
    if (f.size() >= 8 && (std::memcmp(&f[f.size() - 4], "ENOD", 4) == 0 || std::memcmp(&f[f.size() - 4], "DONE", 4) == 0)) {
        try {
            Reader r(f, rd32(f, f.size() - 8));
            std::string magic = r.bytes(4);
            if (magic == "EDON" || magic == "NODE") {
                int32_t n = r.vlq();
                for (int32_t i = 0; i < n; ++i) {
                    std::string name = r.lpstr();
                    r.u32();  // next
                    r.u32();  // child
                    uint32_t off = r.u32(), size = r.u32();
                    if (name == "CharacetrCustomization_Appearances" || name == "CharacterCustomization_Appearances")
                        node = std::make_pair(size_t(off), size_t(size));
                }
            }
        } catch (const std::exception&) {
            out.notes.push_back("could not read the save's node table - searching the whole save");
        }
    }

    bool parsed = false;
    if (node && node->first + node->second <= data.size()) {
        try {
            parsed = parseAppearanceNode(data, node->first, node->first + node->second, out);
        } catch (const std::exception& e) {
            out.notes.push_back(std::string("appearance data not in the expected layout (") + e.what() +
                                ") - picking the choices out of it instead");
            out.morphs.clear();
        }
        if (out.morphs.empty()) scanAppearanceStrings(data, node->first, node->first + node->second, out);
    } else {
        out.notes.push_back("no CharacetrCustomization_Appearances node - searching the whole save");
    }
    if (!parsed && out.morphs.empty()) scanAppearanceStrings(data, 0, data.size(), out);
    if (out.morphs.empty() && !out.genderKnown)
        throw std::runtime_error("no character creator data found in " + file.u8string());
    if (out.morphs.empty())
        out.notes.push_back("no face shape choices in the save (default V) - using V's base face");
    return out;
}

// ======================================================================== hair colours
namespace {
// Vanilla Fallout 4 hair colours (Fallout4.esm CLFM). Their CNAM is a gradient index, so the RGB
// here is the look of each colour as named in the game.
const Fo4HairColor kFo4Hair[] = {
    {0x19EE60, "HairColor13JetBlack", 18, 17, 18},       {0x19EE5E, "HairColor11WarmBlack", 36, 28, 24},
    {0x19EE61, "HairColor14DarkBrown", 56, 38, 28},      {0x19EE65, "HairColor18DeepBrown", 74, 46, 30},
    {0x19EE5F, "HairColor12CoolBrown", 84, 66, 54},      {0x19EE67, "HairColor20ChestnutBrown", 104, 58, 34},
    {0x19EE68, "HairColor21Brown", 112, 76, 50},         {0x0A042D, "HairColor02LightBrown", 150, 110, 74},
    {0x19EE69, "HairColor22Auburn", 122, 52, 30},        {0x19EE62, "HairColor15DeepRed", 112, 32, 22},
    {0x0A0431, "HairColor06Red", 160, 54, 30},           {0x0A0439, "HairColor01LightRed", 196, 98, 60},
    {0x19EE63, "HairColor16Blond", 204, 168, 104},       {0x0A042F, "HairColor03GoldenBlond", 212, 170, 88},
    {0x19EE66, "HairColor19DirtyBlond", 170, 140, 92},   {0x19EE64, "HairColor17PlatinumBlond", 232, 222, 192},
    {0x19EE5B, "HairColor08Bleached", 236, 226, 202},    {0x19EE5A, "HairColor07White", 242, 242, 238},
    {0x0A042E, "HairColor04Silver", 190, 190, 196},      {0x19EE5C, "HairColor09Gray", 140, 140, 140},
    {0x19EE5D, "HairColor10SteelGray", 108, 114, 126},   {0x0A042C, "HairColor05Graying", 122, 112, 102},
    {0x24A058, "HairColor32_DLC04Pink", 240, 150, 190},  {0x24A057, "HairColor31_DLC04RedViolet", 160, 40, 92},
    {0x24A056, "HairColor30_DLC04Red", 204, 32, 32},     {0x24A055, "HairColor29_DLC04Orange", 232, 122, 32},
    {0x24A054, "HairColor28_DLC04LightPink", 250, 192, 212}, {0x24A053, "HairColor27_DLC04YellowGreen", 172, 202, 62},
    {0x24A052, "HairColor26_DLC04Green", 42, 160, 62},   {0x24A051, "HairColor25_DLC04BlueGreen", 32, 150, 150},
    {0x24A050, "HairColor24_DLC04Blue", 42, 82, 200},    {0x24A04E, "HairColor23_DLC04Purple", 112, 42, 162},
};

// Words used in Cyberpunk's hair colour names -> colour.
struct ColorWord { const char* word; uint8_t r, g, b; };
const ColorWord kWords[] = {
    {"black", 20, 18, 18},   {"carbon", 24, 22, 22},  {"liquorice", 30, 22, 20}, {"licorice", 30, 22, 20},  {"onyx", 18, 18, 20},    {"raven", 22, 20, 24},
    {"espresso", 44, 30, 24}, {"chocolate", 62, 40, 28}, {"brown", 90, 60, 40}, {"brunette", 80, 52, 34},
    {"chestnut", 104, 58, 34}, {"hazel", 120, 84, 50}, {"caramel", 160, 110, 60}, {"honey", 190, 140, 70},
    {"auburn", 122, 52, 30}, {"mahogany", 100, 40, 30}, {"copper", 170, 80, 40},  {"ginger", 190, 90, 40},
    {"rust", 150, 60, 30},   {"red", 170, 40, 30},    {"cherry", 150, 20, 40},   {"wine", 110, 30, 50},
    {"strawberry", 210, 120, 90}, {"blond", 205, 168, 104}, {"blonde", 205, 168, 104}, {"golden", 212, 170, 88},
    {"gold", 212, 170, 88},  {"sand", 196, 168, 120}, {"ash", 160, 150, 135},    {"platinum", 232, 222, 192},
    {"bleach", 236, 226, 202}, {"white", 242, 242, 238}, {"snow", 240, 240, 240}, {"silver", 190, 190, 196},
    {"grey", 140, 140, 140}, {"gray", 140, 140, 140}, {"steel", 108, 114, 126}, {"blue", 42, 82, 200},
    {"navy", 30, 40, 100},   {"cyan", 40, 180, 200},  {"teal", 32, 150, 150},    {"turquoise", 40, 190, 180},
    {"aqua", 60, 200, 200},  {"green", 42, 160, 62},  {"lime", 150, 210, 60},    {"mint", 150, 220, 180},
    {"toxic", 160, 220, 40}, {"yellow", 230, 210, 60}, {"orange", 232, 122, 32}, {"pink", 240, 150, 190},
    {"rose", 220, 120, 150}, {"magenta", 200, 40, 140}, {"fuchsia", 210, 50, 160}, {"purple", 112, 42, 162},
    {"violet", 130, 60, 180}, {"lilac", 190, 160, 220}, {"lavender", 180, 160, 220}, {"indigo", 70, 40, 130},
};
} // namespace

std::string fo4FormRef(uint32_t formId) { return "Fallout4.esm|" + toHex(formId, 6); }

const Fo4HairColor& nearestFo4HairColor(const std::string& cpColor, bool* recognised) {
    // average the colour words in the name ("06_black_carbon", "13_platinum_blonde", ...)
    std::string n = toLower(cpColor);
    for (char& c : n)
        if (!std::isalpha((unsigned char)c)) c = ' ';
    double r = 0, g = 0, b = 0;
    int k = 0;
    size_t pos = 0;
    while (pos < n.size()) {
        while (pos < n.size() && n[pos] == ' ') ++pos;
        size_t e = n.find(' ', pos);
        if (e == std::string::npos) e = n.size();
        std::string w = n.substr(pos, e - pos);
        for (const auto& cw : kWords)
            if (!w.empty() && w == cw.word) { r += cw.r; g += cw.g; b += cw.b; ++k; break; }
        pos = e;
    }
    if (recognised) *recognised = k > 0;
    if (k == 0) return kFo4Hair[2];   // dark brown
    r /= k; g /= k; b /= k;
    // Prefer Fallout 4 colours whose name shares a word with the Cyberpunk one (platinum, auburn, ...)
    std::vector<const Fo4HairColor*> named;
    for (const auto& h : kFo4Hair) {
        const std::string id = toLower(h.editorId);
        size_t p2 = 0;
        while (p2 < n.size()) {
            while (p2 < n.size() && n[p2] == ' ') ++p2;
            size_t e = n.find(' ', p2);
            if (e == std::string::npos) e = n.size();
            std::string w = n.substr(p2, e - p2);
            if (w.size() > 4 && w.back() == 'e') w.pop_back();   // blonde -> blond
            if (w.size() >= 4 && id.find(w) != std::string::npos) { named.push_back(&h); break; }
            p2 = e;
        }
    }
    const Fo4HairColor* best = &kFo4Hair[0];
    double bd = 1e18;
    for (const auto& h : kFo4Hair) {
        if (!named.empty() && std::find(named.begin(), named.end(), &h) == named.end()) continue;
        // weighted RGB distance ("redmean")
        double rm = (r + h.r) / 2, dr = r - h.r, dg = g - h.g, db = b - h.b;
        double d = (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db;
        if (d < bd) { bd = d; best = &h; }
    }
    return *best;
}

} // namespace ptk

// ======================================================================== Fallout 4 save writer
namespace ptk {
namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void putf(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, u); }
void set32(std::vector<uint8_t>& v, size_t o, uint32_t x) { std::memcpy(&v[o], &x, 4); }

// "Fallout4.esm|19EE60" -> save refID (0x400000 | form id), 0 if not a Fallout4.esm form
uint32_t esmRefId(const std::string& ref) {
    const size_t bar = ref.find('|');
    if (bar == std::string::npos || toLower(ref.substr(0, bar)) != "fallout4.esm") return 0;
    const uint32_t id = uint32_t(std::stoul(ref.substr(bar + 1), nullptr, 16)) & 0xFFFFFF;
    return id < 0x400000 ? (0x400000 | id) : 0;
}
uint32_t rd24(const std::vector<uint8_t>& b, size_t o) { return (uint32_t(b[o]) << 16) | (uint32_t(b[o + 1]) << 8) | b[o + 2]; }
void wr24(std::vector<uint8_t>& b, size_t o, uint32_t r) { b[o] = uint8_t(r >> 16); b[o + 1] = uint8_t(r >> 8); b[o + 2] = uint8_t(r); }

struct FaceLayout {
    size_t presets = 0, regions = 0, tints = 0, values = 0, weight = 0;
    size_t headPartsCount = 0;   // 0 = not located
    size_t hairRef = 0;          // 0 = not located
    uint32_t nHeadParts = 0;
};

// [... hair flag + ref, 10 bytes, vsval head part count, refs, 01]
// [presets][regions][tints][values][3 x f32 weight] = end of the form
bool locateFace(const std::vector<uint8_t>& b, FaceLayout& L) {
    const size_t len = b.size();
    size_t best = 0, bestN = 0, bestP = 0;
    for (size_t o = 4; o + 4 + 40 <= len; ++o) {
        const uint32_t n = rd32(b, o);
        if (n < 1 || n > 300 || o + 4 + size_t(n) * 40 > len) continue;
        bool ok = true;
        std::vector<uint32_t> ids;
        for (uint32_t k = 0; k < n && ok; ++k) {
            const size_t r = o + 4 + size_t(k) * 40;
            for (int f = 0; f < 9 && ok; ++f) ok = sane(rdf(b, r + 4 + 4 * f), 20.f);
            ids.push_back(rd32(b, r));
        }
        if (!ok) continue;
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) continue;
        // must chain to the end: tints, values, weight
        size_t p = o + 4 + size_t(n) * 40;
        if (p + 4 > len) continue;
        const size_t nt = rd32(b, p);
        const size_t tp = p;
        p += 4 + nt * 10;
        if (p + 4 > len) continue;
        const size_t vp = p;
        p += 4 + size_t(rd32(b, p)) * 4;
        if (p + 12 != len) continue;
        // presets list ending right before
        size_t pp = SIZE_MAX;
        for (size_t m = 0; m <= 300 && 4 + 8 * m <= o; ++m)
            if (rd32(b, o - 4 - 8 * m) == m) { pp = o - 4 - 8 * m; break; }
        if (pp == SIZE_MAX) continue;
        if (n > bestN) { best = o; bestN = n; bestP = pp; L.tints = tp; L.values = vp; }
    }
    if (!bestN) return false;
    L.regions = best;
    L.presets = bestP;
    L.weight = len - 12;
    // head parts: "<vsval n> n x refID 01" right before the presets
    if (L.presets >= 1 && b[L.presets - 1] == 1)
        for (uint32_t n = 1; n < 64; ++n) {
            if (L.presets < 2 + 3 * size_t(n)) break;
            const size_t c = L.presets - 2 - 3 * size_t(n);
            if (b[c] == (n << 2)) {
                L.headPartsCount = c;
                L.nHeadParts = n;
                if (c >= 14 && b[c - 14] == 1) L.hairRef = c - 13;
                break;
            }
        }
    return true;
}

} // namespace

Fo4SaveWriteInfo writeFo4SaveFace(const fs::path& in, const fs::path& out, const LooksMenuPreset& face,
                                  const std::vector<double>& weight) {
    Fo4SaveWriteInfo info;
    std::vector<uint8_t> d = readFileBytes(in);
    if (d.size() < 16 || std::memcmp(d.data(), "FO4_SAVEGAME", 12) != 0)
        throw std::runtime_error(in.filename().u8string() + " is not a Fallout 4 save");
    Reader r(d, 12);
    const uint32_t headerSize = r.u32();
    const size_t headerEnd = r.o + headerSize;
    r.u32(); r.u32();
    r.wstr();
    r.u32();
    r.wstr(); r.wstr(); r.wstr();
    const uint16_t sex = r.u16();
    if (face.gender >= 0 && int(sex == 1) != face.gender)
        throw std::runtime_error(std::string("the Fallout 4 save's character is ") + (sex == 1 ? "female" : "male") +
                                 " but the face is " + (face.gender == 1 ? "female" : "male") +
                                 " - use a save of a " + (face.gender == 1 ? "female" : "male") + " character");
    r.o = headerEnd - 8;
    const uint32_t shotW = r.u32(), shotH = r.u32();
    r.o = headerEnd + size_t(shotW) * shotH * 4;
    r.u8();
    r.wstr();
    const uint32_t pluginInfoSize = r.u32();
    r.skip(pluginInfoSize);
    const size_t flt = r.o;
    r.need(40);
    const uint32_t changeFormsOffset = rd32(d, flt + 16), changeFormCount = rd32(d, flt + 36);

    constexpr uint32_t kPlayer = 0x400007, kSpouseMale = 0x4A7D34, kSpouseFemale = 0x4A7D35;
    struct Form { size_t start = 0, data = 0; uint32_t flags = 0; uint8_t type = 0; uint32_t l1 = 0, l2 = 0; };
    std::map<uint32_t, Form> forms;
    Reader c(d, changeFormsOffset);
    for (uint32_t i = 0; i < changeFormCount; ++i) {
        Form f;
        f.start = c.o;
        const uint32_t refId = (uint32_t(c.u8()) << 16) | (uint32_t(c.u8()) << 8) | c.u8();
        f.flags = c.u32();
        f.type = c.u8();
        c.u8();
        switch (f.type >> 6) {
        case 0: f.l1 = c.u8(); f.l2 = c.u8(); break;
        case 1: f.l1 = c.u16(); f.l2 = c.u16(); break;
        default: f.l1 = c.u32(); f.l2 = c.u32(); break;
        }
        f.data = c.o;
        c.need(f.l1);
        if (refId == kPlayer || refId == kSpouseMale || refId == kSpouseFemale) forms[refId] = f;
        c.o += f.l1;
    }
    auto bodyOf = [&](const Form& f) {
        std::vector<uint8_t> body;
        if (f.l2 > 0) {
            body.resize(f.l2);
            mz_ulong n = f.l2;
            if (mz_uncompress(body.data(), &n, &d[f.data], f.l1) != MZ_OK) return std::vector<uint8_t>{};
            body.resize(n);
        } else {
            body.assign(d.begin() + f.data, d.begin() + f.data + f.l1);
        }
        return body;
    };

    // the same face form loadFo4Save reads
    const bool female = sex == 1;
    uint32_t chosen = 0;
    std::vector<uint8_t> b;
    FaceLayout L;
    for (uint32_t id : {kPlayer, female ? kSpouseFemale : kSpouseMale}) {
        auto it = forms.find(id);
        if (it == forms.end() || !(it->second.flags & 0x800)) continue;
        std::vector<uint8_t> body = bodyOf(it->second);
        FaceLayout l;
        if (body.empty() || !locateFace(body, l)) continue;
        chosen = id;
        b = std::move(body);
        L = l;
        break;
    }
    if (!chosen)
        throw std::runtime_error("this save has no face data to replace - in Fallout 4 open the face editor once "
                                 "(or LooksMenu), change anything, save, and use that save");
    info.faceSource = chosen == kPlayer ? "player" : "spouse NPC (prologue)";

    // ---- new face tail
    std::vector<uint8_t> tail;
    put32(tail, uint32_t(face.presetWeights.size()));
    for (const auto& [k, w] : face.presetWeights) { put32(tail, k); putf(tail, float(w)); }
    put32(tail, uint32_t(face.regions.size()));
    for (const auto& rs : face.regions) {
        put32(tail, rs.id);
        for (int i = 0; i < 9; ++i) putf(tail, i < 8 && size_t(i) < rs.values.size() ? float(rs.values[i]) : 0.f);
    }
    // tints: keep the save's, minus replaced ones, plus ours
    {
        std::vector<std::array<uint8_t, 10>> keep;
        std::set<uint32_t> drop(face.replaceTints.begin(), face.replaceTints.end());
        for (const auto& t : face.tints) drop.insert(t.index);
        const uint32_t nt = rd32(b, L.tints);
        for (uint32_t i = 0; i < nt; ++i) {
            std::array<uint8_t, 10> e;
            std::memcpy(e.data(), &b[L.tints + 4 + 10 * i], 10);
            const uint16_t idx = uint16_t(e[0] | (e[1] << 8));
            if (!drop.count(idx)) keep.push_back(e);
        }
        put32(tail, uint32_t(keep.size() + face.tints.size()));
        for (const auto& e : keep) tail.insert(tail.end(), e.begin(), e.end());
        for (const auto& t : face.tints) {
            put16(tail, uint16_t(t.index));
            put32(tail, t.type == 1 ? (t.color & 0xFFFFFF) : 0xFFFFFFFFu);
            putf(tail, float(std::clamp(t.percent, 0, 100)) / 100.f);
        }
        if (!face.tints.empty())
            info.lines.push_back("tints: " + std::to_string(face.tints.size()) + " set, " + std::to_string(nt - keep.size()) +
                                 " replaced, " + std::to_string(keep.size()) + " kept");
    }
    // Morphs.Values: positional, at least 5
    {
        const uint32_t nv = rd32(b, L.values);
        std::vector<float> vals(std::max<size_t>(nv, 5), 0.f);
        for (uint32_t i = 0; i < nv; ++i) vals[i] = rdf(b, L.values + 4 + 4 * i);
        for (const auto& [k, v] : face.morphValues)
            if (k < 64) {
                if (k >= vals.size()) vals.resize(k + 1, 0.f);
                vals[k] = float(v);
            }
        put32(tail, uint32_t(vals.size()));
        for (float v : vals) putf(tail, v);
    }
    for (int i = 0; i < 3; ++i)
        putf(tail, weight.size() == 3 ? float(weight[i]) : rdf(b, L.weight + 4 * i));
    if (weight.size() == 3) info.lines.push_back("body weight set");

    std::vector<uint8_t> nb(b.begin(), b.begin() + L.presets);
    // eyes (head part) and hair colour live before the presets
    if (!face.headParts.empty() && !face.replaceHeadParts.empty()) {
        const uint32_t want = esmRefId(face.headParts.front());
        std::set<uint32_t> repl;
        for (const auto& h : face.replaceHeadParts) if (uint32_t x = esmRefId(h)) repl.insert(x);
        bool done = false;
        if (want && L.headPartsCount)
            for (uint32_t i = 0; i < L.nHeadParts && !done; ++i) {
                const size_t o = L.headPartsCount + 1 + 3 * size_t(i);
                if (repl.count(rd24(nb, o))) { wr24(nb, o, want); done = true; }
            }
        info.lines.push_back(done ? "eyes set to " + face.headParts.front()
                                  : "eyes not changed (the save's eyes are not vanilla Fallout 4 eyes)");
    }
    if (!face.hairColor.empty()) {
        const uint32_t want = esmRefId(face.hairColor);
        if (want && L.hairRef) {
            wr24(nb, L.hairRef, want);
            info.lines.push_back("hair colour set to " + face.hairColor);
        } else {
            info.lines.push_back("hair colour not changed (not found in the save)");
        }
    }
    nb.insert(nb.end(), tail.begin(), tail.end());
    info.lines.insert(info.lines.begin(), std::to_string(face.regions.size()) + " sculpt regions, " +
                                              std::to_string(face.presetWeights.size()) + " face presets written");

    // ---- re-encode the change form
    const Form& f = forms[chosen];
    std::vector<uint8_t> payload;
    uint32_t l1, l2;
    if (f.l2 > 0) {
        mz_ulong cap = mz_compressBound(mz_ulong(nb.size()));
        payload.resize(cap);
        if (mz_compress2(payload.data(), &cap, nb.data(), mz_ulong(nb.size()), 6) != MZ_OK)
            throw std::runtime_error("could not compress the face data");
        payload.resize(cap);
        l1 = uint32_t(payload.size());
        l2 = uint32_t(nb.size());
    } else {
        payload = nb;
        l1 = uint32_t(nb.size());
        l2 = 0;
    }
    const uint32_t big = std::max(l1, l2);
    int cls = big < 0x100 ? 0 : big < 0x10000 ? 1 : 2;
    cls = std::max(cls, int(f.type >> 6));
    std::vector<uint8_t> rec(d.begin() + f.start, d.begin() + f.start + 9);
    rec[7] = uint8_t((f.type & 0x3F) | (cls << 6));
    if (cls == 0) { rec.push_back(uint8_t(l1)); rec.push_back(uint8_t(l2)); }
    else if (cls == 1) { put16(rec, uint16_t(l1)); put16(rec, uint16_t(l2)); }
    else { put32(rec, l1); put32(rec, l2); }
    rec.insert(rec.end(), payload.begin(), payload.end());

    const size_t oldEnd = f.data + f.l1;
    const int64_t delta = int64_t(rec.size()) - int64_t(oldEnd - f.start);
    std::vector<uint8_t> o;
    o.reserve(d.size() + rec.size());
    o.insert(o.end(), d.begin(), d.begin() + f.start);
    o.insert(o.end(), rec.begin(), rec.end());
    o.insert(o.end(), d.begin() + oldEnd, d.end());
    // file location table offsets past the form: formIDArrayCount, unknownTable3, globalDataTable3
    for (size_t k : {0, 1, 2, 3, 5}) {
        const uint32_t v = rd32(o, flt + 4 * k);
        if (v >= oldEnd) set32(o, flt + 4 * k, uint32_t(int64_t(v) + delta));
    }
    std::error_code eq;
    if (fs::exists(out, eq) && fs::equivalent(in, out, eq)) throw std::runtime_error("the output save must be a new file, not the input save");
    writeFileBytes(out, o);

    // F4SE co-save
    fs::path co = in;
    co.replace_extension(".f4se");
    std::error_code ec;
    if (fs::exists(co, ec)) {
        fs::path coOut = out;
        coOut.replace_extension(".f4se");
        copyFileReplace(co, coOut);
        info.coSaveCopied = true;
    }
    return info;
}

} // namespace ptk
