#include "ModelExport.h"
#include "Gltf.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace ptk {

void computeSmoothNormals(ModelPart& p) {
    const size_t n = p.pos.size();
    // weld by position (0.01 mm grid) so seams get one normal
    std::map<std::tuple<long long, long long, long long>, size_t> weld;
    std::vector<size_t> group(n);
    for (size_t i = 0; i < n; ++i) {
        const auto key = std::make_tuple(std::llround(p.pos[i].x * 1e5), std::llround(p.pos[i].y * 1e5),
                                         std::llround(p.pos[i].z * 1e5));
        group[i] = weld.emplace(key, weld.size()).first->second;
    }
    std::vector<Vec3> acc(weld.size());
    for (size_t t = 0; t + 2 < p.idx.size(); t += 3) {
        const uint32_t a = p.idx[t], b = p.idx[t + 1], c = p.idx[t + 2];
        if (a >= n || b >= n || c >= n) continue;
        const Vec3 fn = cross(p.pos[b] - p.pos[a], p.pos[c] - p.pos[a]);   // area-weighted
        acc[group[a]] += fn;
        acc[group[b]] += fn;
        acc[group[c]] += fn;
    }
    p.nrm.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3 v = acc[group[i]];
        const double l = length(v);
        p.nrm[i] = l > 1e-12 ? v * (1.0 / l) : Vec3(0, 1, 0);
    }
}

namespace {

void writeGlb(const std::vector<ModelPart>& parts, const std::filesystem::path& out) {
    Glb g;
    auto& d = g.doc;
    d["asset"] = {{"version", "2.0"}, {"generator", "PresetToolKit"}};
    d["scene"] = 0;
    d["buffers"] = nlohmann::json::array();
    d["bufferViews"] = nlohmann::json::array();
    d["accessors"] = nlohmann::json::array();
    d["materials"] = nlohmann::json::array();
    d["meshes"] = nlohmann::json::array();
    d["nodes"] = nlohmann::json::array();
    nlohmann::json sceneNodes = nlohmann::json::array();

    auto view = [&](const void* data, size_t bytes, int target) {
        while (g.bin.size() % 4) g.bin.push_back(0);
        const size_t off = g.bin.size();
        g.bin.insert(g.bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
        d["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", off}, {"byteLength", bytes}, {"target", target}});
        return int(d["bufferViews"].size() - 1);
    };
    auto accessor = [&](int v, int compType, size_t count, const char* type) {
        d["accessors"].push_back({{"bufferView", v}, {"componentType", compType}, {"count", count}, {"type", type}});
        return int(d["accessors"].size() - 1);
    };

    for (const ModelPart& src : parts) {
        if (src.pos.empty() || src.idx.empty()) continue;
        ModelPart p = src;
        if (p.nrm.size() != p.pos.size()) computeSmoothNormals(p);
        const size_t n = p.pos.size();
        std::vector<float> P, N, T;
        Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
        for (size_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k) {
                P.push_back(float(p.pos[i][k]));
                N.push_back(float(p.nrm[i][k]));
                lo[k] = std::min(lo[k], p.pos[i][k]);
                hi[k] = std::max(hi[k], p.pos[i][k]);
            }
        }
        nlohmann::json attrs;
        const int pa = accessor(view(P.data(), P.size() * 4, 34962), 5126, n, "VEC3");
        d["accessors"][pa]["min"] = {lo.x, lo.y, lo.z};
        d["accessors"][pa]["max"] = {hi.x, hi.y, hi.z};
        attrs["POSITION"] = pa;
        attrs["NORMAL"] = accessor(view(N.data(), N.size() * 4, 34962), 5126, n, "VEC3");
        if (p.uv.size() == n) {
            for (const auto& t : p.uv) { T.push_back(t[0]); T.push_back(t[1]); }
            attrs["TEXCOORD_0"] = accessor(view(T.data(), T.size() * 4, 34962), 5126, n, "VEC2");
        }
        const int ia = accessor(view(p.idx.data(), p.idx.size() * 4, 34963), 5125, p.idx.size(), "SCALAR");
        d["materials"].push_back({{"name", p.name + "_material"},
                                  {"pbrMetallicRoughness", {{"baseColorFactor", {0.8, 0.8, 0.8, 1.0}},
                                                            {"metallicFactor", 0.0}, {"roughnessFactor", 0.7}}}});
        const int mat = int(d["materials"].size() - 1);
        d["meshes"].push_back({{"name", p.name},
                               {"primitives", nlohmann::json::array({{{"attributes", attrs}, {"indices", ia}, {"material", mat}}})}});
        d["nodes"].push_back({{"name", p.name}, {"mesh", int(d["meshes"].size() - 1)}});
        sceneNodes.push_back(int(d["nodes"].size() - 1));
    }
    if (sceneNodes.empty()) throw std::runtime_error("nothing to export (no geometry)");
    while (g.bin.size() % 4) g.bin.push_back(0);
    d["buffers"].push_back({{"byteLength", g.bin.size()}});
    d["scenes"] = nlohmann::json::array({{{"nodes", sceneNodes}}});
    saveGlb(g, out);
}

void writeObj(const std::vector<ModelPart>& parts, const std::filesystem::path& out) {
    std::ostringstream o;
    o << "# PresetToolKit face export (metres, Y up)\n";
    size_t base = 1;
    char buf[160];
    for (const ModelPart& src : parts) {
        if (src.pos.empty() || src.idx.empty()) continue;
        ModelPart p = src;
        if (p.nrm.size() != p.pos.size()) computeSmoothNormals(p);
        const bool uv = p.uv.size() == p.pos.size();
        o << "o " << p.name << "\n";
        for (const auto& v : p.pos) { std::snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", v.x, v.y, v.z); o << buf; }
        if (uv)
            for (const auto& t : p.uv) { std::snprintf(buf, sizeof(buf), "vt %.6f %.6f\n", t[0], 1.0 - t[1]); o << buf; }
        for (const auto& v : p.nrm) { std::snprintf(buf, sizeof(buf), "vn %.5f %.5f %.5f\n", v.x, v.y, v.z); o << buf; }
        o << "s 1\n";
        for (size_t t = 0; t + 2 < p.idx.size(); t += 3) {
            o << "f";
            for (int k = 0; k < 3; ++k) {
                const size_t i = base + p.idx[t + k];
                if (uv) o << " " << i << "/" << i << "/" << i;
                else o << " " << i << "//" << i;
            }
            o << "\n";
        }
        base += p.pos.size();
    }
    writeFileText(out, o.str());
}

} // namespace

void writeModel(const std::vector<ModelPart>& parts, const std::filesystem::path& out) {
    const std::string ext = toLower(out.extension().u8string());
    if (ext == ".obj") writeObj(parts, out);
    else if (ext == ".glb") writeGlb(parts, out);
    else throw std::runtime_error("model format must be .glb or .obj: " + out.u8string());
}

} // namespace ptk
