#include "Gltf.h"

#include "Util.h"

#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

using nlohmann::json;

namespace ptk {

namespace {

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A; // "JSON"
constexpr uint32_t kChunkBin = 0x004E4942;  // "BIN\0"

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; } // glb is little-endian
void wr32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

Affine nodeLocal(const json& node) {
    Affine a;
    if (node.contains("matrix")) {
        const auto& m = node["matrix"]; // column-major 4x4
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) a.l(r, c) = m[c * 4 + r].get<double>();
        a.t = {m[12].get<double>(), m[13].get<double>(), m[14].get<double>()};
        return a;
    }
    Vec3 t, s{1, 1, 1};
    double q[4] = {0, 0, 0, 1};
    if (node.contains("translation")) { auto& v = node["translation"]; t = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}; }
    if (node.contains("scale")) { auto& v = node["scale"]; s = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}; }
    if (node.contains("rotation")) { auto& v = node["rotation"]; for (int i = 0; i < 4; ++i) q[i] = v[i].get<double>(); }
    double x = q[0], y = q[1], z = q[2], w = q[3];
    Mat3 R;
    R(0,0) = 1 - 2*(y*y + z*z); R(0,1) = 2*(x*y - z*w);     R(0,2) = 2*(x*z + y*w);
    R(1,0) = 2*(x*y + z*w);     R(1,1) = 1 - 2*(x*x + z*z); R(1,2) = 2*(y*z - x*w);
    R(2,0) = 2*(x*z - y*w);     R(2,1) = 2*(y*z + x*w);     R(2,2) = 1 - 2*(x*x + y*y);
    Mat3 S; S(0,0) = s.x; S(1,1) = s.y; S(2,2) = s.z;
    a.l = R * S;
    a.t = t;
    return a;
}

} // namespace

Glb loadGlb(const std::filesystem::path& path) {
    auto data = readFileBytes(path);
    if (data.size() < 20 || rd32(data.data()) != kGlbMagic)
        throw std::runtime_error("not a binary glTF (.glb) file: " + path.u8string() +
                                 " (export the head from WolvenKit as GLB)");
    uint32_t total = rd32(data.data() + 8);
    if (total > data.size()) throw std::runtime_error("truncated glb: " + path.u8string());

    Glb g;
    size_t off = 12;
    bool haveJson = false;
    while (off + 8 <= total) {
        uint32_t len = rd32(data.data() + off);
        uint32_t type = rd32(data.data() + off + 4);
        off += 8;
        if (off + len > total) throw std::runtime_error("corrupt glb chunk in " + path.u8string());
        if (type == kChunkJson) {
            g.doc = json::parse(data.begin() + off, data.begin() + off + len);
            haveJson = true;
        } else if (type == kChunkBin && g.bin.empty()) {
            g.bin.assign(data.begin() + off, data.begin() + off + len);
        }
        off += len;
    }
    if (!haveJson) throw std::runtime_error("glb has no JSON chunk: " + path.u8string());
    if (g.doc.contains("buffers")) {
        const auto& bufs = g.doc["buffers"];
        if (bufs.size() > 1 || (bufs.size() == 1 && bufs[0].contains("uri")))
            throw std::runtime_error("glb references external buffers; only self-contained .glb files are supported");
    }
    return g;
}

void saveGlb(const Glb& glbIn, const std::filesystem::path& path) {
    Glb glb = glbIn;
    std::vector<uint8_t> bin = glb.bin;
    while (bin.size() % 4) bin.push_back(0);
    if (!glb.doc.contains("buffers") || glb.doc["buffers"].empty())
        glb.doc["buffers"] = json::array({json::object()});
    glb.doc["buffers"][0]["byteLength"] = glbIn.bin.size();

    std::string js = glb.doc.dump();
    while (js.size() % 4) js.push_back(' ');

    std::vector<uint8_t> out;
    uint32_t total = 12 + 8 + (uint32_t)js.size() + (bin.empty() ? 0 : 8 + (uint32_t)bin.size());
    wr32(out, kGlbMagic);
    wr32(out, 2);
    wr32(out, total);
    wr32(out, (uint32_t)js.size());
    wr32(out, kChunkJson);
    out.insert(out.end(), js.begin(), js.end());
    if (!bin.empty()) {
        wr32(out, (uint32_t)bin.size());
        wr32(out, kChunkBin);
        out.insert(out.end(), bin.begin(), bin.end());
    }
    writeFileBytes(path, out);
}

namespace {

struct AccessorLayout {
    size_t count = 0, base = 0, stride = 0;
    int comps = 0;
    bool hasView = false;
};

AccessorLayout layoutOf(const Glb& glb, int accessorIndex) {
    const auto& acc = glb.doc.at("accessors").at(accessorIndex);
    if (acc.contains("sparse")) throw std::runtime_error("sparse accessors are not supported");
    if (acc.value("componentType", 0) != 5126)
        throw std::runtime_error("accessor " + std::to_string(accessorIndex) + " is not float");
    std::string type = acc.value("type", "");
    AccessorLayout L;
    L.comps = type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "VEC2" ? 2 : type == "SCALAR" ? 1 : 0;
    if (!L.comps) throw std::runtime_error("accessor " + std::to_string(accessorIndex) + " has unsupported type " + type);
    L.count = acc.at("count").get<size_t>();
    if (!acc.contains("bufferView")) return L;
    L.hasView = true;
    const auto& bv = glb.doc.at("bufferViews").at(acc["bufferView"].get<int>());
    if (bv.value("buffer", 0) != 0) throw std::runtime_error("accessor uses a buffer other than the GLB BIN chunk");
    L.base = bv.value("byteOffset", (size_t)0) + acc.value("byteOffset", (size_t)0);
    L.stride = bv.value("byteStride", (size_t)0);
    if (L.stride == 0) L.stride = 4 * (size_t)L.comps;
    if (L.count && L.base + L.stride * (L.count - 1) + 4 * L.comps > glb.bin.size())
        throw std::runtime_error("accessor " + std::to_string(accessorIndex) + " reads past the BIN chunk");
    return L;
}

} // namespace

std::vector<float> readFloatAccessor(const Glb& glb, int accessorIndex, int& components) {
    AccessorLayout L = layoutOf(glb, accessorIndex);
    components = L.comps;
    std::vector<float> out(L.count * L.comps, 0.0f);
    if (!L.hasView) return out;
    for (size_t i = 0; i < L.count; ++i)
        std::memcpy(out.data() + i * L.comps, glb.bin.data() + L.base + i * L.stride, 4 * L.comps);
    return out;
}

std::vector<uint32_t> readIndexAccessor(const Glb& glb, int accessorIndex) {
    const auto& acc = glb.doc.at("accessors").at(accessorIndex);
    const int ct = acc.value("componentType", 0);
    const size_t size = ct == 5121 ? 1 : ct == 5123 ? 2 : ct == 5125 ? 4 : 0;
    if (!size) throw std::runtime_error("accessor " + std::to_string(accessorIndex) + " is not an index accessor");
    const size_t count = acc.value("count", size_t(0));
    std::vector<uint32_t> out(count, 0);
    if (!acc.contains("bufferView")) return out;
    const auto& bv = glb.doc.at("bufferViews").at(acc["bufferView"].get<int>());
    const size_t base = bv.value("byteOffset", size_t(0)) + acc.value("byteOffset", size_t(0));
    const size_t stride = bv.value("byteStride", size);
    for (size_t i = 0; i < count; ++i) {
        const size_t o = base + i * stride;
        if (o + size > glb.bin.size()) throw std::runtime_error("index accessor past end of buffer");
        uint32_t v = 0;
        std::memcpy(&v, glb.bin.data() + o, size);
        out[i] = v;
    }
    return out;
}

bool writeFloatAccessor(Glb& glb, int accessorIndex, const std::vector<float>& data, bool forceMinMax) {
    AccessorLayout L = layoutOf(glb, accessorIndex);
    if (data.size() != L.count * L.comps) throw std::runtime_error("writeFloatAccessor: size mismatch");
    if (!L.hasView) return false;
    for (size_t i = 0; i < L.count; ++i)
        std::memcpy(glb.bin.data() + L.base + i * L.stride, data.data() + i * L.comps, 4 * L.comps);
    auto& acc = glb.doc["accessors"][accessorIndex];
    if (forceMinMax || acc.contains("min") || acc.contains("max")) {
        std::vector<double> mn(L.comps, 0.0), mx(L.comps, 0.0);
        for (size_t i = 0; i < L.count; ++i)
            for (int k = 0; k < L.comps; ++k) {
                double v = data[i * L.comps + k];
                if (i == 0 || v < mn[k]) mn[k] = v;
                if (i == 0 || v > mx[k]) mx[k] = v;
            }
        acc["min"] = mn;
        acc["max"] = mx;
    }
    return true;
}

std::vector<Vec3> readVec3Accessor(const Glb& glb, int accessorIndex) {
    int comps = 0;
    auto f = readFloatAccessor(glb, accessorIndex, comps);
    if (comps != 3) throw std::runtime_error("accessor " + std::to_string(accessorIndex) + " is not VEC3");
    std::vector<Vec3> out(f.size() / 3);
    for (size_t i = 0; i < out.size(); ++i) out[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
    return out;
}

std::vector<MeshInstance> collectMeshInstances(const Glb& glb) {
    const auto& doc = glb.doc;
    size_t meshCount = doc.contains("meshes") ? doc["meshes"].size() : 0;
    std::vector<bool> seen(meshCount, false);
    std::vector<MeshInstance> out;

    if (doc.contains("nodes")) {
        const auto& nodes = doc["nodes"];
        std::function<void(int, const Affine&, int)> visit = [&](int ni, const Affine& parent, int depth) {
            if (depth > 256 || ni < 0 || ni >= (int)nodes.size()) return;
            const auto& n = nodes[ni];
            Affine w = parent * nodeLocal(n);
            if (n.contains("mesh")) {
                int m = n["mesh"].get<int>();
                if (m >= 0 && m < (int)meshCount && !seen[m]) {
                    seen[m] = true;
                    MeshInstance inst;
                    inst.mesh = m;
                    inst.world = w;
                    inst.name = doc["meshes"][m].value("name", n.value("name", "mesh" + std::to_string(m)));
                    out.push_back(inst);
                }
            }
            if (n.contains("children"))
                for (const auto& c : n["children"]) visit(c.get<int>(), w, depth + 1);
        };
        std::vector<int> roots;
        int sceneIdx = doc.value("scene", 0);
        if (doc.contains("scenes") && sceneIdx < (int)doc["scenes"].size() && doc["scenes"][sceneIdx].contains("nodes")) {
            for (const auto& r : doc["scenes"][sceneIdx]["nodes"]) roots.push_back(r.get<int>());
        } else {
            std::vector<bool> isChild(nodes.size(), false);
            for (const auto& n : nodes)
                if (n.contains("children"))
                    for (const auto& c : n["children"]) isChild[c.get<int>()] = true;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (!isChild[i]) roots.push_back((int)i);
        }
        for (int r : roots) visit(r, Affine{}, 0);
    }
    // Meshes not referenced by any node: identity transform
    for (size_t m = 0; m < meshCount; ++m)
        if (!seen[m]) {
            MeshInstance inst;
            inst.mesh = (int)m;
            inst.name = doc["meshes"][m].value("name", "mesh" + std::to_string(m));
            out.push_back(inst);
        }
    return out;
}

std::map<std::string, Vec3> nodeWorldPositions(const Glb& glb) {
    std::map<std::string, Vec3> out;
    const auto& doc = glb.doc;
    if (!doc.contains("nodes")) return out;
    const auto& nodes = doc["nodes"];
    std::vector<bool> isChild(nodes.size(), false);
    for (const auto& n : nodes)
        if (n.contains("children"))
            for (const auto& c : n["children"])
                if (c.get<size_t>() < nodes.size()) isChild[c.get<size_t>()] = true;
    std::function<void(int, const Affine&, int)> visit = [&](int ni, const Affine& parent, int depth) {
        if (depth > 256 || ni < 0 || ni >= (int)nodes.size()) return;
        const auto& n = nodes[ni];
        Affine w = parent * nodeLocal(n);
        if (n.contains("name")) out[n["name"].get<std::string>()] = w.t;
        if (n.contains("children"))
            for (const auto& c : n["children"]) visit(c.get<int>(), w, depth + 1);
    };
    for (size_t i = 0; i < nodes.size(); ++i)
        if (!isChild[i]) visit((int)i, Affine{}, 0);
    return out;
}

} // namespace ptk
