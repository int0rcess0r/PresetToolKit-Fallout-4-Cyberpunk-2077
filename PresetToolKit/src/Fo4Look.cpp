#include "Fo4Look.h"
#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace ptk {

namespace {

struct Word { const char* word; uint8_t r, g, b; };

// General colour words (hair, eyes, makeup) - Cyberpunk names them like "06_black_carbon".
const Word kColorWords[] = {
    {"black", 20, 18, 18},     {"carbon", 24, 22, 22},    {"onyx", 18, 18, 20},      {"raven", 22, 20, 24},
    {"liquorice", 30, 22, 20}, {"licorice", 30, 22, 20},  {"espresso", 44, 30, 24},  {"chocolate", 62, 40, 28},
    {"brown", 96, 62, 40},     {"brunette", 80, 52, 34},  {"chestnut", 104, 58, 34}, {"hazel", 130, 96, 52},
    {"caramel", 160, 110, 60}, {"honey", 190, 140, 70},   {"auburn", 122, 52, 30},   {"mahogany", 100, 40, 30},
    {"copper", 170, 80, 40},   {"ginger", 190, 90, 40},   {"rust", 150, 60, 30},     {"red", 170, 40, 30},
    {"cherry", 150, 20, 40},   {"wine", 110, 30, 50},     {"burgundy", 100, 20, 40}, {"crimson", 160, 20, 40},
    {"strawberry", 210, 120, 90}, {"blond", 205, 168, 104}, {"blonde", 205, 168, 104}, {"golden", 212, 170, 88},
    {"gold", 212, 170, 88},    {"sand", 196, 168, 120},   {"ash", 160, 150, 135},    {"platinum", 232, 222, 192},
    {"bleach", 236, 226, 202}, {"white", 242, 242, 238},  {"snow", 240, 240, 240},   {"silver", 190, 190, 196},
    {"grey", 140, 140, 140},   {"gray", 140, 140, 140},   {"steel", 108, 114, 126},  {"blue", 50, 90, 190},
    {"navy", 30, 40, 100},     {"cyan", 40, 180, 200},    {"teal", 32, 150, 150},    {"turquoise", 40, 190, 180},
    {"aqua", 60, 200, 200},    {"green", 50, 150, 70},    {"lime", 150, 210, 60},    {"mint", 150, 220, 180},
    {"toxic", 160, 220, 40},   {"yellow", 220, 190, 50},  {"amber", 200, 130, 40},   {"orange", 232, 122, 32},
    {"peach", 240, 170, 120},  {"pink", 240, 150, 190},   {"rose", 220, 120, 150},   {"magenta", 200, 40, 140},
    {"fuchsia", 210, 50, 160}, {"purple", 112, 42, 162},  {"violet", 130, 60, 180},  {"lilac", 190, 160, 220},
    {"lavender", 180, 160, 220}, {"indigo", 70, 40, 130}, {"plum", 100, 40, 80},     {"nude", 200, 150, 130},
    {"beige", 200, 165, 140},  {"coral", 240, 120, 100},  {"berry", 110, 20, 60},
};

// Skin words (Cyberpunk skin tones: "01_ca_pale_00_warm_ivory", "03_ca_senna", "05_bl_espresso", ...)
const Word kSkinWords[] = {
    {"porcelain", 246, 230, 220}, {"pale", 242, 222, 206},   {"ivory", 240, 222, 200}, {"fair", 236, 210, 190},
    {"light", 230, 200, 175},     {"limestone", 226, 198, 172}, {"beige", 220, 186, 156}, {"sand", 216, 182, 148},
    {"senna", 204, 160, 124},     {"sienna", 190, 130, 95},  {"almond", 196, 150, 115}, {"honey", 200, 150, 100},
    {"golden", 205, 160, 110},    {"olive", 182, 146, 108},  {"tan", 188, 140, 100},    {"amber", 180, 125, 80},
    {"caramel", 172, 118, 80},    {"cinnamon", 150, 96, 62}, {"bronze", 150, 100, 66},  {"chestnut", 126, 82, 56},
    {"brown", 120, 80, 56},       {"mocha", 108, 72, 52},    {"coffee", 100, 66, 46},   {"cocoa", 90, 60, 44},
    {"umber", 94, 62, 42},        {"espresso", 74, 50, 38},  {"ebony", 62, 42, 32},     {"sable", 68, 46, 36},
    {"onyx", 46, 34, 30},         {"dark", 80, 56, 42},      {"deep", 70, 48, 36},
};

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : toLower(s)) {
        if (std::isalpha((unsigned char)c)) cur += c;
        else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
int numberIn(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (std::isdigit((unsigned char)s[i])) {
            size_t j = i;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
            return std::stoi(s.substr(i, j - i));
        }
    return 0;
}
// the colour part of a definition: after the last "__" ("...pimples_01__brown_01" -> "brown_01")
std::string colorPart(const std::string& def) {
    size_t p = def.rfind("__");
    return p == std::string::npos ? def : def.substr(p + 2);
}
template <size_t N>
bool average(const std::vector<std::string>& w, const Word (&table)[N], double rgb[3]) {
    double s[3] = {0, 0, 0};
    int n = 0;
    for (const auto& x : w)
        for (const auto& t : table)
            if (x == t.word) { s[0] += t.r; s[1] += t.g; s[2] += t.b; ++n; break; }
    if (!n) return false;
    for (int k = 0; k < 3; ++k) rgb[k] = s[k] / n;
    return true;
}
double colorDistance(const double a[3], double r, double g, double b) {
    double rm = (a[0] + r) / 2, dr = a[0] - r, dg = a[1] - g, db = a[2] - b;
    return (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db;
}
double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }
// LooksMenu / save colours are the game's (r, g, b, a) bytes read as a little-endian u32: 0x00BBGGRR
uint32_t bgra(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t(b) << 16) | (uint32_t(g) << 8) | r; }

// Closest colour of a palette layer to a name; name words win over plain RGB distance.
const Fo4TintColor* pickColor(const Fo4TintLayer& layer, const std::string& name) {
    if (layer.colors.empty()) return nullptr;
    auto w = words(name);
    double rgb[3];
    const bool have = average(w, kColorWords, rgb);
    std::vector<const Fo4TintColor*> cand;
    for (const auto& c : layer.colors) {
        const std::string n = toLower(c.name);
        for (auto x : w) {
            if (x.size() > 4 && x.back() == 'e') x.pop_back();
            if (x.size() >= 3 && n.find(x) != std::string::npos) { cand.push_back(&c); break; }
        }
    }
    if (cand.empty())
        for (const auto& c : layer.colors) cand.push_back(&c);
    if (!have) return cand.front();
    const Fo4TintColor* best = cand.front();
    double bd = 1e18;
    for (const auto* c : cand) {
        double dd = colorDistance(rgb, c->r, c->g, c->b);
        if (dd < bd) { bd = dd; best = c; }
    }
    return best;
}

const Fo4TintLayer* firstOf(const std::vector<Fo4TintLayer>& layers, const std::string& kind) {
    for (const auto& l : layers)
        if (l.kind == kind) return &l;
    return nullptr;
}
std::vector<const Fo4TintLayer*> allOf(const std::vector<Fo4TintLayer>& layers, const std::string& kind) {
    std::vector<const Fo4TintLayer*> out;
    for (const auto& l : layers)
        if (l.kind == kind) out.push_back(&l);
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->number != b->number ? a->number < b->number : a->index < b->index; });
    return out;
}

// Fallout 4 eye colours by name.
struct EyeRgb { const char* key; uint8_t r, g, b; };
const EyeRgb kEyeRgb[] = {
    {"amber", 196, 132, 44},  {"darkgrey", 78, 84, 90},   {"greenaqua", 78, 168, 158}, {"greenlight", 128, 178, 100},
    {"hazelblue", 118, 128, 112}, {"hazelgreen", 128, 128, 70}, {"lightbrown", 150, 100, 60}, {"lightgrey", 160, 166, 172},
    {"steel", 108, 124, 140}, {"bluehazy", 112, 140, 170}, {"bluepale", 150, 180, 210}, {"green", 70, 130, 70},
    {"brown", 96, 58, 30},    {"blue", 60, 100, 170},     {"hazel", 140, 108, 60},
};

} // namespace

bool colorFromName(const std::string& name, uint8_t out[3]) {
    double rgb[3];
    if (!average(words(name), kColorWords, rgb)) return false;
    for (int k = 0; k < 3; ++k) out[k] = uint8_t(std::lround(rgb[k]));
    return true;
}

Fo4Chargen loadFo4Chargen(const std::filesystem::path& path) {
    Fo4Chargen d;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return d;
    auto j = nlohmann::json::parse(readFileText(path));
    for (const char* sex : {"female", "male"}) {
        for (const auto& t : j["tints"][sex]) {
            Fo4TintLayer l;
            l.index = std::stoul(t["index"].get<std::string>(), nullptr, 16);
            l.kind = t.value("kind", "");
            l.type = t.value("type", 0);
            l.number = t.value("number", 0);
            l.texture = t.value("texture", "");
            for (const auto& c : t["colors"]) {
                Fo4TintColor fc;
                fc.id = c["id"].get<int>();
                fc.name = c.value("name", "");
                fc.r = uint8_t(c["rgb"][0].get<int>());
                fc.g = uint8_t(c["rgb"][1].get<int>());
                fc.b = uint8_t(c["rgb"][2].get<int>());
                l.colors.push_back(fc);
            }
            d.tints[sex].push_back(l);
        }
        for (const auto& e : j["eyes"][sex]) d.eyes[sex].push_back({e["form"].get<std::string>(), e["name"].get<std::string>()});
        if (j.contains("faceMorphs") && j["faceMorphs"].contains(sex)) {
            const auto& fm = j["faceMorphs"][sex];
            auto& t = d.faceMorphs[sex];
            for (auto it = fm["presets"].begin(); it != fm["presets"].end(); ++it)
                t.presets[uint32_t(std::stoul(it.key(), nullptr, 16))] = it.value().get<std::string>();
            for (auto it = fm["sliders"].begin(); it != fm["sliders"].end(); ++it)
                t.sliders[uint32_t(std::stoul(it.key(), nullptr, 16))] = {it.value()[0].get<std::string>(), it.value()[1].get<std::string>()};
            if (fm.contains("groups"))
                for (auto it = fm["groups"].begin(); it != fm["groups"].end(); ++it)
                    t.groups[uint32_t(std::stoul(it.key(), nullptr, 16))] = it.value().get<std::string>();
        }
    }
    d.loaded = true;
    return d;
}

Fo4Look mapCyberpunkLook(const CpAppearance& app, const Fo4Chargen& data, bool female) {
    Fo4Look look;
    const std::string sex = female ? "female" : "male";
    const auto& layers = data.tints.count(sex) ? data.tints.at(sex) : std::vector<Fo4TintLayer>{};
    auto add = [&](const Fo4TintLayer& l, const Fo4TintColor* c, const std::string& line, int percent = 100) {
        for (const auto& t : look.tints)
            if (t.index == l.index) return;
        PresetTint t;
        t.index = l.index;
        t.type = l.type;
        t.kind = l.kind;
        t.percent = percent;
        if (c) { t.color = bgra(c->r, c->g, c->b); t.colorId = c->id; }
        look.tints.push_back(t);
        look.lines.push_back(line);
    };

    for (const auto& [name, def] : app.appearances) {
        const std::string n = toLower(name), dl = toLower(def);
        // ---- eye colour -> eye head part
        if (n == "eyes_color" && data.eyes.count(sex)) {
            std::vector<std::string> w = words(colorPart(def));
            for (auto& x : w)
                if (x == "gray") x = "grey";
            double rgb[3];
            const bool have = average(w, kColorWords, rgb);
            // candidates: eyes whose name contains one of the colour words (if any), never bloodshot
            std::vector<const std::pair<std::string, std::string>*> named, all;
            for (const auto& e : data.eyes.at(sex)) {
                std::string en = toLower(e.second);
                if (en.find("bloodshot") != std::string::npos) continue;
                all.push_back(&e);
                const size_t h = en.find("human");
                const std::string key = h == std::string::npos ? en : en.substr(h + 5);
                for (const auto& x : w)
                    if (x.size() >= 4 && key.find(x) != std::string::npos) { named.push_back(&e); break; }
            }
            const auto& cand = named.empty() ? all : named;
            const std::pair<std::string, std::string>* best = nullptr;
            double bd = 1e18;
            for (const auto* e : cand) {
                std::string en = toLower(e->second);
                const size_t h = en.find("human");
                const std::string key = h == std::string::npos ? en : en.substr(h + 5);
                double dd = 1e12;
                for (const auto& k : kEyeRgb)
                    if (key == k.key) dd = have ? colorDistance(rgb, k.r, k.g, k.b) : 0.0;
                // prefer the plain colour ("Brown") over variants ("LightBrown") when names tie
                dd += double(key.size());
                if (dd < bd) { bd = dd; best = e; }
            }
            if (best) {
                look.eyesForm = "Fallout4.esm|" + best->first;
                look.lines.push_back("eyes " + colorPart(def) + " -> " + best->second);
            }
            continue;
        }
        // ---- skin tone -> skin palette by lightness
        if (n.rfind("skin_type", 0) == 0) {
            const Fo4TintLayer* skin = firstOf(layers, "skin");
            if (!skin || skin->colors.empty()) continue;
            std::string tone = colorPart(def);
            double rgb[3];
            double L;
            if (average(words(tone), kSkinWords, rgb)) L = luma(rgb[0], rgb[1], rgb[2]);
            else L = 228.0 - 18.0 * std::max(0, numberIn(tone) - 1);   // numbered from light to dark
            // Cyberpunk lightness 228 (pale) .. 45 (darkest) -> Fallout 4 skin base 01 .. 13
            double t = std::clamp((228.0 - L) / (228.0 - 45.0), 0.0, 1.0);
            // Fallout 4's skin bases darken quickly past the middle: keep light / medium tones lighter
            t = std::pow(t, 1.5);
            double lo = 1e9, hi = -1e9;
            for (const auto& c : skin->colors) { double l = luma(c.r, c.g, c.b); lo = std::min(lo, l); hi = std::max(hi, l); }
            double want = hi - t * (hi - lo);
            const Fo4TintColor* best = &skin->colors.front();
            for (const auto& c : skin->colors)
                if (std::fabs(luma(c.r, c.g, c.b) - want) < std::fabs(luma(best->r, best->g, best->b) - want)) best = &c;
            add(*skin, best, "skin " + tone + " -> " + best->name);
            continue;
        }
        // ---- eyebrows: style number -> brow texture
        if (n.rfind("eyebrows_color", 0) == 0) {
            auto brows = allOf(layers, "brows");
            if (brows.empty()) continue;
            int style = std::max(1, numberIn(n.substr(14)));
            const Fo4TintLayer* b = brows[size_t(style - 1) % brows.size()];
            add(*b, nullptr, "eyebrows style " + std::to_string(style) + " -> " + b->texture);
            continue;
        }
        if (n.rfind("makeup", 0) != 0) continue;
        const std::string what = n.substr(6);   // Pimples_03, Cheeks_02, Lips_01, Eyes_04, ...
        // the shape number in the definition: "..._pimples_01__..." -> 1
        int shape = 1;
        {
            size_t c = dl.rfind("__");
            std::string head = c == std::string::npos ? dl : dl.substr(0, c);
            size_t u = head.rfind('_');
            if (u != std::string::npos) shape = std::max(1, numberIn(head.substr(u)));
        }
        if (dl.find("freckle") != std::string::npos) {
            auto fr = allOf(layers, "freckles");
            if (!fr.empty()) {
                const Fo4TintLayer* l = fr[size_t(shape - 1) % fr.size()];
                add(*l, nullptr, "freckles " + std::to_string(shape) + " -> " + l->texture);
            }
        } else if (dl.find("pimple") != std::string::npos || dl.find("acne") != std::string::npos ||
                   dl.find("blemish") != std::string::npos) {
            // Cyberpunk pimples sit on the cheeks / chin: use Fallout 4's cheek blemishes
            std::vector<const Fo4TintLayer*> bl;
            for (const auto* l : allOf(layers, "blemish"))
                if (toLower(l->texture).find("cheekblemish") != std::string::npos) bl.push_back(l);
            if (bl.empty()) bl = allOf(layers, "blemish");
            if (!bl.empty()) {
                const Fo4TintLayer* l = bl[size_t(shape - 1) % bl.size()];
                add(*l, nullptr, "blemishes " + std::to_string(shape) + " -> " + l->texture);
            }
        } else if (what.rfind("lips", 0) == 0 || dl.find("lip") != std::string::npos) {
            const Fo4TintLayer* l = firstOf(layers, "lipstick");
            if (l) {
                const Fo4TintColor* c = pickColor(*l, colorPart(def));
                add(*l, c, "lipstick " + colorPart(def) + " -> " + (c ? c->name : std::string("?")));
            }
        } else if (what.rfind("eyes", 0) == 0 || dl.find("eyeshadow") != std::string::npos ||
                   dl.find("eyeliner") != std::string::npos) {
            const bool liner = dl.find("liner") != std::string::npos;
            const Fo4TintLayer* l = firstOf(layers, liner ? "eyeliner" : "eyeshadow");
            if (!l) l = firstOf(layers, liner ? "eyeshadow" : "eyeliner");
            if (l) {
                const Fo4TintColor* c = pickColor(*l, colorPart(def));
                add(*l, c, std::string(liner ? "eyeliner " : "eye makeup ") + colorPart(def) + " -> " +
                               l->texture + " " + (c ? c->name : std::string("?")));
            }
        } else if (what.rfind("cheeks", 0) == 0 || dl.find("blush") != std::string::npos) {
            const Fo4TintLayer* l = firstOf(layers, "blush");
            if (l) {
                const Fo4TintColor* c = pickColor(*l, colorPart(def));
                add(*l, c, "blush " + colorPart(def) + " -> " + (c ? c->name : std::string("?")));
            }
        } else {
            look.lines.push_back("not carried over: " + name + " (" + def + ")");
        }
    }
    return look;
}

} // namespace ptk
