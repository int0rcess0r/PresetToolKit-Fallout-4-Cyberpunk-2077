#include "Nif.h"

#include <cstring>
#include <functional>
#include <stdexcept>

namespace ptk {

namespace {

struct Reader {
    const std::vector<uint8_t>& b;
    size_t o = 0;
    std::string label;
    void need(size_t n) const {
        if (o + n > b.size()) throw std::runtime_error(label + ": unexpected end of file");
    }
    template <class T> T get() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, b.data() + o, sizeof(T));
        o += sizeof(T);
        return v;
    }
    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    int32_t i32() { return get<int32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }
    float f32() { return get<float>(); }
    std::string str(size_t n) {
        need(n);
        std::string s(reinterpret_cast<const char*>(b.data() + o), n);
        o += n;
        return s;
    }
};

float halfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) f = sign << 31;
        else { // subnormal
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; --exp; }
            mant &= 0x3FF;
            f = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

Mat3 readMat3(Reader& r) {
    Mat3 m;
    for (int i = 0; i < 9; ++i) m.m[i] = r.f32();
    return m;
}

} // namespace

const NifNode* NifFile::node(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : it->second;
}

std::vector<std::string> NifFile::descendants(const std::string& name) const {
    std::vector<std::string> out;
    const NifNode* n = node(name);
    if (!n) return out;
    std::function<void(const NifNode&, int)> rec = [&](const NifNode& nd, int depth) {
        if (depth > 64) return;
        for (int c : nd.children) {
            if (c < 0 || c >= (int)nodes_.size() || !isNode_[c]) continue;
            if (!nodes_[c].name.empty()) out.push_back(nodes_[c].name);
            rec(nodes_[c], depth + 1);
        }
    };
    rec(*n, 0);
    return out;
}

void NifFile::computeWorld(int idx, const Mat3& R, const Vec3& T, double S, int depth) {
    if (idx < 0 || idx >= (int)nodes_.size() || !isNode_[idx] || depth > 128) return;
    NifNode& n = nodes_[idx];
    // world = parentR * localR as stored; the resulting columns are the node's axes in world
    // space (verified against the BSSkin bone data of the vanilla FO4 head).
    n.worldT = T + (R * n.localT) * S;
    n.worldR = R * n.localR;
    n.worldS = S * n.localS;
    n.hasWorld = true;
    for (int c : n.children) computeWorld(c, n.worldR, n.worldT, n.worldS, depth + 1);
}

NifFile::NifFile(const std::vector<uint8_t>& bytes, const std::string& label) {
    Reader r{bytes, 0, label};
    // header line
    size_t nl = 0;
    while (nl < bytes.size() && bytes[nl] != '\n') ++nl;
    if (nl >= bytes.size() || std::string(bytes.begin(), bytes.begin() + std::min<size_t>(nl, 20)).find("Gamebryo") == std::string::npos)
        throw std::runtime_error(label + ": not a NIF file");
    r.o = nl + 1;
    uint32_t version = r.u32();
    r.u8();             // endian
    r.u32();            // user version
    uint32_t nblocks = r.u32();
    uint32_t bsver = r.u32();
    if (version != 0x14020007) throw std::runtime_error(label + ": unsupported NIF version (Fallout 4 files expected)");
    for (int i = 0; i < (bsver >= 130 ? 4 : 3); ++i) r.str(r.u8());   // export info strings
    uint16_t ntypes = r.u16();
    std::vector<std::string> types(ntypes);
    for (auto& t : types) t = r.str(r.u32());
    std::vector<uint16_t> tidx(nblocks);
    for (auto& t : tidx) t = r.u16();
    std::vector<uint32_t> sizes(nblocks);
    for (auto& s : sizes) s = r.u32();
    uint32_t nstr = r.u32();
    r.u32();
    std::vector<std::string> strings(nstr);
    for (auto& s : strings) s = r.str(r.u32());
    uint32_t ngroups = r.u32();
    for (uint32_t i = 0; i < ngroups; ++i) r.u32();

    std::vector<size_t> starts(nblocks);
    size_t off = r.o;
    for (uint32_t i = 0; i < nblocks; ++i) { starts[i] = off; off += sizes[i]; }
    if (off > bytes.size()) throw std::runtime_error(label + ": truncated file");

    nodes_.resize(nblocks);
    isNode_.assign(nblocks, false);
    auto strAt = [&](int32_t i) { return (i >= 0 && i < (int32_t)strings.size()) ? strings[i] : std::string(); };

    struct PendingShape { int block; NifShape shape; int32_t skinRef; };
    std::vector<PendingShape> pending;
    std::map<int, std::pair<int32_t, std::vector<int32_t>>> skinInstances;    // block -> (boneData, bones)
    std::map<int, std::vector<std::tuple<Mat3, Vec3, double>>> boneDatas;

    for (uint32_t i = 0; i < nblocks; ++i) {
        const std::string& t = types[tidx[i] & 0x7FFF];
        r.o = starts[i];
        const bool isNi = (t == "NiNode" || t == "BSFadeNode" || t == "BSLeafAnimNode");
        const bool isShape = (t == "BSTriShape" || t == "BSSubIndexTriShape" || t == "BSDynamicTriShape");
        if (isNi || isShape) {
            NifNode& n = nodes_[i];
            n.type = t;
            n.name = strAt(r.i32());
            uint32_t ne = r.u32();
            for (uint32_t k = 0; k < ne; ++k) r.i32();
            r.i32();                  // controller
            r.u32();                  // flags
            n.localT = {r.f32(), r.f32(), r.f32()};
            n.localR = readMat3(r);
            n.localS = r.f32();
            r.i32();                  // collision
            isNode_[i] = true;
            if (isNi) {
                uint32_t nc = r.u32();
                for (uint32_t k = 0; k < nc; ++k) n.children.push_back(r.i32());
            } else {
                for (int k = 0; k < 4; ++k) r.f32();  // bounding sphere
                int32_t skinRef = r.i32();
                r.i32();                              // shader property
                r.i32();                              // alpha
                uint64_t desc = r.u64();
                const uint32_t ntris = r.u32();
                uint16_t nv = r.u16();
                uint32_t dataSize = r.u32();
                const uint32_t vsize = (uint32_t)(desc & 0xF) * 4;
                const uint32_t skinOff = (uint32_t)((desc >> 28) & 0xF) * 4;
                const uint64_t flags = desc >> 44;
                const bool full = (flags & (1u << 10)) != 0;
                const bool skinned = (flags & (1u << 6)) != 0;
                PendingShape ps{(int)i, {}, skinRef};
                const uint32_t uvOff = (uint32_t)((desc >> 8) & 0xF) * 4;
                const bool hasUv = (flags & (1u << 1)) != 0;
                ps.shape.name = n.name;
                if (dataSize > 0 && vsize > 0) {
                    r.need((size_t)vsize * nv);
                    const uint8_t* base = bytes.data() + r.o;
                    ps.shape.verts.resize(nv);
                    for (uint16_t v = 0; v < nv; ++v) {
                        const uint8_t* q = base + (size_t)v * vsize;
                        NifSkinnedVertex& sv = ps.shape.verts[v];
                        if (full) {
                            float p[3];
                            std::memcpy(p, q, 12);
                            sv.pos = {p[0], p[1], p[2]};
                        } else {
                            uint16_t h[3];
                            std::memcpy(h, q, 6);
                            sv.pos = {halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2])};
                        }
                        if (hasUv && uvOff + 4 <= vsize) {
                            uint16_t hu[2];
                            std::memcpy(hu, q + uvOff, 4);
                            ps.shape.uv.push_back({halfToFloat(hu[0]), halfToFloat(hu[1])});
                        }
                        if (skinned && skinOff + 12 <= vsize) {
                            uint16_t hw[4];
                            std::memcpy(hw, q + skinOff, 8);
                            for (int k = 0; k < 4; ++k) {
                                sv.weight[k] = halfToFloat(hw[k]);
                                sv.bone[k] = q[skinOff + 8 + k];
                            }
                        }
                    }
                }
                if (dataSize > 0 && vsize > 0) {
                    // the triangles follow the vertex data
                    r.o += (size_t)vsize * nv;
                    r.need((size_t)ntris * 6);
                    for (uint32_t t3 = 0; t3 < ntris; ++t3) {
                        std::array<uint16_t, 3> tri{r.u16(), r.u16(), r.u16()};
                        ps.shape.tris.push_back(tri);
                    }
                }
                pending.push_back(std::move(ps));
            }
        } else if (t == "BSSkin::Instance") {
            int32_t root = r.i32();
            (void)root;
            int32_t boneData = r.i32();
            uint32_t nb = r.u32();
            std::vector<int32_t> bones(nb);
            for (auto& bref : bones) bref = r.i32();
            skinInstances[(int)i] = {boneData, bones};
        } else if (t == "BSSkin::BoneData") {
            uint32_t nb = r.u32();
            auto& v = boneDatas[(int)i];
            for (uint32_t k = 0; k < nb; ++k) {
                for (int s = 0; s < 4; ++s) r.f32();  // bounding sphere
                Mat3 R = readMat3(r);
                Vec3 T{r.f32(), r.f32(), r.f32()};
                double S = r.f32();
                v.emplace_back(R, T, S);
            }
        }
    }

    for (uint32_t i = 0; i < nblocks; ++i)
        if (isNode_[i] && !nodes_[i].name.empty()) byName_[nodes_[i].name] = &nodes_[i];
    computeWorld(0, Mat3(), Vec3(), 1.0, 0);

    for (auto& ps : pending) {
        auto si = skinInstances.find(ps.skinRef);
        if (si != skinInstances.end()) {
            for (int32_t b : si->second.second)
                ps.shape.skinBones.push_back((b >= 0 && b < (int)nblocks && isNode_[b]) ? nodes_[b].name : std::string());
            auto bd = boneDatas.find(si->second.first);
            if (bd != boneDatas.end())
                for (auto& [R, T, S] : bd->second) {
                    ps.shape.skinR.push_back(R);
                    ps.shape.skinT.push_back(T);
                    ps.shape.skinS.push_back(S);
                }
        }
        shapes_.push_back(std::move(ps.shape));
    }
}

} // namespace ptk
