#include "Cr2w.h"
#include "Util.h"

#include <miniz/miniz.h>
#undef compress   // miniz maps zlib names onto its own
#undef uncompress

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <cctype>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace ptk {

// ======================================================================== Oodle / Kraken
namespace {
typedef intptr_t (*OodleDecompressFn)(const void* comp, intptr_t compLen, void* raw, intptr_t rawLen, int fuzzSafe,
                                      int checkCrc, int verbosity, void* decBufBase, intptr_t decBufSize,
                                      void* fpCallback, void* callbackUserData, void* decoderMemory,
                                      intptr_t decoderMemorySize, int threadPhase);
typedef intptr_t (*OodleCompressFn)(int compressor, const void* raw, intptr_t rawLen, void* comp, int level,
                                    const void* options, const void* dictionaryBase, const void* lrm,
                                    void* scratchMem, intptr_t scratchSize);
typedef int (*OozDecompressFn)(const unsigned char* src, size_t srcLen, unsigned char* dst, size_t dstLen);

constexpr int kOodleKraken = 8;
constexpr int kOodleLevelNormal = 4;
} // namespace

bool Kraken::load(const std::vector<fs::path>& searchDirs) {
#ifdef _WIN32
    for (const auto& dir : searchDirs) {
        if (dir.empty()) continue;
        for (const wchar_t* name : {L"oo2ext_7_win64.dll", L"oo2core_9_win64.dll", L"oo2core_8_win64.dll"}) {
            fs::path dll = dir / name;
            std::error_code ec;
            if (!fs::exists(dll, ec)) continue;
            HMODULE h = LoadLibraryW(dll.wstring().c_str());
            if (!h) continue;
            dec_ = reinterpret_cast<void*>(GetProcAddress(h, "OodleLZ_Decompress"));
            comp_ = reinterpret_cast<void*>(GetProcAddress(h, "OodleLZ_Compress"));
            if (dec_) {
                source_ = dll.u8string();
                return true;
            }
            comp_ = nullptr;
        }
    }
#else
    (void)searchDirs;
#endif
    if (const char* lib = std::getenv("PTK_KRAKEN_LIB")) {
#ifdef _WIN32
        HMODULE h = LoadLibraryA(lib);
        if (h) ooz_ = reinterpret_cast<void*>(GetProcAddress(h, "ooz_decompress"));
#else
        void* h = dlopen(lib, RTLD_NOW);
        if (h) ooz_ = dlsym(h, "ooz_decompress");
#endif
        if (ooz_) {
            source_ = lib;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> Kraken::decompress(const uint8_t* src, size_t srcLen, size_t rawLen) const {
    std::vector<uint8_t> out(rawLen + 64);
    intptr_t got = -1;
    if (dec_)
        got = reinterpret_cast<OodleDecompressFn>(dec_)(src, (intptr_t)srcLen, out.data(), (intptr_t)rawLen, 1, 0, 0,
                                                        nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    else if (ooz_)
        got = reinterpret_cast<OozDecompressFn>(ooz_)(src, srcLen, out.data(), rawLen);
    else
        throw std::runtime_error("no Oodle library loaded");
    if (got != (intptr_t)rawLen)
        throw std::runtime_error("Oodle could not decompress a buffer (" + std::to_string((long long)got) + " of " +
                                 std::to_string(rawLen) + " bytes)");
    out.resize(rawLen);
    return out;
}

std::vector<uint8_t> Kraken::compress(const std::vector<uint8_t>& raw) const {
    if (!comp_ || raw.empty()) return {};
    std::vector<uint8_t> out(raw.size() + 274 * ((raw.size() + 0x3FFFF) / 0x40000) + 1024);
    intptr_t n = reinterpret_cast<OodleCompressFn>(comp_)(kOodleKraken, raw.data(), (intptr_t)raw.size(), out.data(),
                                                         kOodleLevelNormal, nullptr, nullptr, nullptr, nullptr, 0);
    if (n <= 0 || (size_t)n > out.size()) return {};
    out.resize((size_t)n);
    // Verify the round trip before trusting it.
    try {
        if (decompress(out.data(), out.size(), raw.size()) != raw) return {};
    } catch (...) {
        return {};
    }
    return out;
}

// ======================================================================== CR2W reader
namespace {

uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    if (o + 2 > d.size()) throw std::runtime_error("CR2W: read past end of file");
    uint16_t v;
    std::memcpy(&v, d.data() + o, 2);
    return v;
}
uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) throw std::runtime_error("CR2W: read past end of file");
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}
float rdf(const std::vector<uint8_t>& d, size_t o) {
    uint32_t u = rd32(d, o);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void wr32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(d.data() + o, &v, 4); }

uint32_t crc(const uint8_t* p, size_t n) { return (uint32_t)mz_crc32(MZ_CRC32_INIT, p, n); }

struct Prop {
    std::string name, type;
    size_t value = 0;  // offset of the value
    size_t end = 0;    // offset just past the value
};

struct BufferInfo {
    size_t entryOffset = 0;
    uint32_t flags = 0, index = 0, offset = 0, diskSize = 0, memSize = 0, crc32 = 0;
};

struct Cr2w {
    std::vector<uint8_t> d;
    uint32_t tableOff[10]{}, tableCount[10]{};
    std::vector<std::string> names;
    struct Export { std::string cls; uint32_t dataSize, dataOffset; };
    std::vector<Export> exports;
    std::vector<BufferInfo> buffers;

    explicit Cr2w(std::vector<uint8_t> bytes) : d(std::move(bytes)) {
        if (d.size() < 160 || std::memcmp(d.data(), "CR2W", 4) != 0) throw std::runtime_error("not a CR2W file");
        const uint32_t version = rd32(d, 4);
        if (version < 195) throw std::runtime_error("unsupported CR2W version " + std::to_string(version));
        for (int i = 0; i < 10; ++i) {
            tableOff[i] = rd32(d, 40 + 12 * i);
            tableCount[i] = rd32(d, 44 + 12 * i);
        }
        const size_t strings = tableOff[0];
        for (uint32_t i = 0; i < tableCount[1]; ++i) {
            size_t o = strings + rd32(d, tableOff[1] + 8 * i);
            size_t e = o;
            while (e < d.size() && d[e]) ++e;
            names.emplace_back(reinterpret_cast<const char*>(d.data() + o), e - o);
        }
        for (uint32_t i = 0; i < tableCount[4]; ++i) {
            size_t o = tableOff[4] + 24 * i;
            exports.push_back({name(rd16(d, o)), rd32(d, o + 8), rd32(d, o + 12)});
        }
        for (uint32_t i = 0; i < tableCount[5]; ++i) {
            size_t o = tableOff[5] + 24 * i;
            BufferInfo b;
            b.entryOffset = o;
            b.flags = rd32(d, o);
            b.index = rd32(d, o + 4);
            b.offset = rd32(d, o + 8);
            b.diskSize = rd32(d, o + 12);
            b.memSize = rd32(d, o + 16);
            b.crc32 = rd32(d, o + 20);
            buffers.push_back(b);
        }
    }

    const std::string& name(uint16_t i) const {
        if (i >= names.size()) throw std::runtime_error("CR2W: bad name index");
        return names[i];
    }

    // Property list starting at `o` (ends with a zero name index). `*endOut` = just past it.
    std::vector<Prop> props(size_t o, size_t* endOut = nullptr) const {
        std::vector<Prop> out;
        for (;;) {
            uint16_t n = rd16(d, o);
            if (n == 0) {
                if (endOut) *endOut = o + 2;
                return out;
            }
            Prop p;
            p.name = name(n);
            p.type = name(rd16(d, o + 2));
            uint32_t size = rd32(d, o + 4);
            if (size < 4) throw std::runtime_error("CR2W: bad property size");
            p.value = o + 8;
            p.end = o + 4 + size;
            if (p.end > d.size()) throw std::runtime_error("CR2W: property past end of file");
            out.push_back(p);
            o = p.end;
        }
    }
    // A struct value: one zero byte, then a property list.
    std::vector<Prop> structProps(size_t o, size_t* endOut = nullptr) const { return props(o + 1, endOut); }
    // Elements of an array (or static array) of structs.
    std::vector<std::vector<Prop>> structArray(const Prop& p) const {
        std::vector<std::vector<Prop>> out;
        uint32_t n = rd32(d, p.value);
        size_t o = p.value + 4;
        for (uint32_t i = 0; i < n; ++i) out.push_back(structProps(o, &o));
        return out;
    }
    std::vector<uint32_t> u32Array(const Prop& p) const {
        std::vector<uint32_t> out(rd32(d, p.value));
        for (size_t i = 0; i < out.size(); ++i) out[i] = rd32(d, p.value + 4 + 4 * i);
        return out;
    }
    std::vector<uint8_t> u8Array(const Prop& p) const {
        std::vector<uint8_t> out(rd32(d, p.value));
        for (size_t i = 0; i < out.size(); ++i) out[i] = d.at(p.value + 4 + i);
        return out;
    }
    std::string enumValue(const Prop& p) const { return name(rd16(d, p.value)); }
    double vecComponent(const std::vector<Prop>& v, const char* c) const {
        for (auto& p : v)
            if (p.name == c) return rdf(d, p.value);
        return 0.0;
    }

    std::vector<uint8_t> bufferData(size_t i, const Kraken& k) const {
        const BufferInfo& b = buffers.at(i);
        if (size_t(b.offset) + b.diskSize > d.size()) throw std::runtime_error("CR2W: buffer past end of file");
        const uint8_t* p = d.data() + b.offset;
        if (b.diskSize == b.memSize) return std::vector<uint8_t>(p, p + b.diskSize);
        if (b.diskSize < 8 || std::memcmp(p, "KARK", 4) != 0)
            throw std::runtime_error("CR2W: unknown buffer compression");
        if (!k.canDecompress())
            throw std::runtime_error("the head's data is Oodle-compressed and the Oodle library was not found "
                                     "(expected Cyberpunk 2077\\bin\\x64\\oo2ext_7_win64.dll - set the Cyberpunk 2077 folder)");
        return k.decompress(p + 8, b.diskSize - 8, b.memSize);
    }
};

const Prop* find(const std::vector<Prop>& v, const std::string& n) {
    for (auto& p : v)
        if (p.name == n) return &p;
    return nullptr;
}
const Prop& need(const std::vector<Prop>& v, const std::string& n, const char* where) {
    const Prop* p = find(v, n);
    if (!p) throw std::runtime_error(std::string("morphtarget: missing ") + where + "." + n);
    return *p;
}

int packingSize(const std::string& t) {
    static const std::map<std::string, int> sizes = {
        {"PT_Float1", 4},   {"PT_Float2", 8},    {"PT_Float3", 12},   {"PT_Float4", 16},    {"PT_Float16_2", 4},
        {"PT_Float16_4", 8}, {"PT_UShort1", 2},  {"PT_UShort2", 4},   {"PT_UShort4", 8},    {"PT_UShort4N", 8},
        {"PT_Short1", 2},   {"PT_Short2", 4},    {"PT_Short4", 8},    {"PT_Short4N", 8},    {"PT_UInt1", 4},
        {"PT_UInt2", 8},    {"PT_UInt3", 12},    {"PT_UInt4", 16},    {"PT_Int1", 4},       {"PT_Int2", 8},
        {"PT_Int3", 12},    {"PT_Int4", 16},     {"PT_Color", 4},     {"PT_UByte1", 1},     {"PT_UByte1F", 1},
        {"PT_UByte4", 4},   {"PT_UByte4N", 4},   {"PT_Byte4N", 4},    {"PT_Dec4", 4},       {"PT_Index16", 2},
        {"PT_Index32", 4}};
    auto it = sizes.find(t);
    return it == sizes.end() ? -1 : it->second;
}

struct Element {
    std::string type, usage;
    int stream = 0, usageIndex = 0;
};

// unit vector -> Dec4 (10:10:10:2), the inverse of WolvenKit's x / 511.5 - 1 decode
uint32_t encodeDec4(const double in[3], uint32_t keepHigh) {
    uint32_t v = keepHigh & 0xC0000000u;
    for (int i = 0; i < 3; ++i) {
        long q = std::lround((std::clamp(in[i], -1.0, 1.0) + 1.0) * 511.5);
        v |= uint32_t(std::clamp(q, 0L, 1023L)) << (10 * i);
    }
    return v;
}

// glTF (Y up, WolvenKit export) -> Cyberpunk (Z up): (x, y, z) -> (x, -z, y)
void gltfToCp(const float* g, double out[3]) {
    out[0] = g[0];
    out[1] = -double(g[2]);
    out[2] = g[1];
}

} // namespace

// Switches off the character creator's face shapes: for every target of the eyes / nose / mouth /
// jaw / ear regions the position diff scale and offset become 0 (no movement) and the normal /
// tangent diffs become 0 (Dec4 mid-point), so V's face is the base head whatever the save chose.
void neutralizeCcTargets(Cr2w& f, const Kraken& kraken, std::map<size_t, std::vector<uint8_t>>& changed,
                         MorphtargetPatchStats& st) {
    static const std::set<std::string> kRegions = {"eyes", "nose", "mouth", "jaw", "ear", "ears"};
    const auto meshProps = f.props(f.exports[0].dataOffset + 1);
    const Prop* targetsProp = find(meshProps, "targets");
    if (!targetsProp) return;
    std::vector<size_t> sel;
    size_t t = 0;
    for (const auto& tp : f.structArray(*targetsProp)) {
        std::string name, region;
        if (const Prop* p = find(tp, "name")) name = f.enumValue(*p);
        if (const Prop* p = find(tp, "regionName")) region = toLower(f.enumValue(*p));
        const bool ccName = name.size() == 4 && name[0] == 'h' && std::isdigit((unsigned char)name[1]) &&
                            std::isdigit((unsigned char)name[2]) && std::isdigit((unsigned char)name[3]);
        if (kRegions.count(region) || (region.empty() && ccName)) sel.push_back(t);
        ++t;
    }
    if (sel.empty()) return;
    const Cr2w::Export* blob = nullptr;
    for (auto& e : f.exports)
        if (e.cls == "rendRenderMorphTargetMeshBlob") blob = &e;
    if (!blob) return;
    const auto bp = f.props(blob->dataOffset + 1);
    const Prop* hp = find(bp, "header");
    const Prop* db = find(bp, "diffsBuffer");
    if (!hp || !db) return;
    const auto header = f.structProps(hp->value);
    // per-target position scale / offset (Vector4 arrays) -> 0
    for (const char* arr : {"targetPositionDiffScale", "targetPositionDiffOffset"}) {
        const Prop* a = find(header, arr);
        if (!a) continue;
        auto vecs = f.structArray(*a);
        for (size_t i : sel)
            if (i < vecs.size())
                for (const auto& comp : vecs[i])
                    if (comp.type == "Float") wr32(f.d, comp.value, 0u);
    }
    // normal / tangent diffs -> 0
    const Prop* starts = find(header, "targetStartsInVertexDiffs");
    const Prop* counts = find(header, "numVertexDiffsInEachChunk");
    const uint16_t dIndex = rd16(f.d, db->value);
    if (!starts || !counts || dIndex < 1 || dIndex > f.buffers.size()) return;
    const auto st0 = f.u32Array(*starts);
    std::vector<uint64_t> cnt;
    {
        size_t o = counts->value;
        const uint32_t n = rd32(f.d, o);
        o += 4;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t m = rd32(f.d, o);
            o += 4;
            uint64_t sum = 0;
            for (uint32_t k = 0; k < m; ++k, o += 4) sum += rd32(f.d, o);
            cnt.push_back(sum);
        }
    }
    const size_t slot = dIndex - 1;
    if (!changed.count(slot)) changed[slot] = f.bufferData(slot, kraken);
    std::vector<uint8_t>& diffs = changed[slot];
    const uint32_t mid = 512u | (512u << 10) | (512u << 20);
    for (size_t i : sel) {
        if (i >= st0.size() || i >= cnt.size()) continue;
        for (uint64_t k = st0[i]; k < st0[i] + cnt[i]; ++k) {
            const size_t o = size_t(k) * 12;
            if (o + 12 > diffs.size()) break;
            for (int w = 1; w <= 2; ++w) wr32(diffs, o + 4 * w, (rd32(diffs, o + 4 * w) & 0xC0000000u) | mid);
        }
        ++st.neutralized;
    }
}

// ======================================================================== patcher
MorphtargetPatchStats patchMorphtargetGeometry(const fs::path& vanillaMorphtarget, const Glb& glb,
                                               const fs::path& outFile, const Kraken& kraken) {
    MorphtargetPatch p;
    p.glb = &glb;
    return patchMorphtarget(vanillaMorphtarget, outFile, kraken, p);
}

MorphtargetPatchStats patchMorphtarget(const fs::path& vanillaMorphtarget, const fs::path& outFile, const Kraken& kraken,
                                       const MorphtargetPatch& patch) {
    Cr2w f(readFileBytes(vanillaMorphtarget));
    if (f.exports.empty() || f.exports[0].cls != "MorphTargetMesh")
        throw std::runtime_error(vanillaMorphtarget.filename().u8string() + " is not a .morphtarget");

    // ---- the embedded render mesh (rendRenderMeshBlob)
    const Cr2w::Export* blob = nullptr;
    for (auto& e : f.exports)
        if (e.cls == "rendRenderMeshBlob") blob = &e;
    if (!blob) throw std::runtime_error("morphtarget has no rendRenderMeshBlob");
    const auto blobProps = f.props(blob->dataOffset + 1);
    const auto header = f.structProps(need(blobProps, "header", "rendRenderMeshBlob").value);
    const size_t rbValue = need(blobProps, "renderBuffer", "rendRenderMeshBlob").value;
    const uint16_t bufIndex = rd16(f.d, rbValue);  // 1-based
    if (bufIndex < 1 || bufIndex > f.buffers.size()) throw std::runtime_error("morphtarget: bad render buffer index");
    const size_t bufSlot = bufIndex - 1;

    const auto qs = f.structProps(need(header, "quantizationScale", "header").value);
    const auto qo = f.structProps(need(header, "quantizationOffset", "header").value);
    const double scale[3] = {f.vecComponent(qs, "X"), f.vecComponent(qs, "Y"), f.vecComponent(qs, "Z")};
    const double offset[3] = {f.vecComponent(qo, "X"), f.vecComponent(qo, "Y"), f.vecComponent(qo, "Z")};

    struct ChunkLayout {
        uint32_t numVertices = 0;
        size_t posBase = 0, posStride = 0, posOff = 0;
        bool hasNT = false;
        size_t ntBase = 0, ntStride = 0, nOff = 0, tOff = 0;
        bool hasT = false;
    };
    std::vector<ChunkLayout> chunks;
    for (const auto& ch : f.structArray(need(header, "renderChunkInfos", "header"))) {
        ChunkLayout L;
        const Prop* nv = find(ch, "numVertices");
        if (nv) L.numVertices = nv->type == "Uint16" ? rd16(f.d, nv->value)
                              : nv->type == "Uint8" ? f.d.at(nv->value) : rd32(f.d, nv->value);
        const auto cv = f.structProps(need(ch, "chunkVertices", "renderChunkInfos").value);
        const auto byteOffsets = f.u32Array(need(cv, "byteOffsets", "chunkVertices"));
        const auto layout = f.structProps(need(cv, "vertexLayout", "chunkVertices").value);
        const auto strides = f.u8Array(need(layout, "slotStrides", "vertexLayout"));
        std::vector<Element> els;
        for (const auto& e : f.structArray(need(layout, "elements", "vertexLayout"))) {
            Element el;
            if (const Prop* p = find(e, "type")) el.type = f.enumValue(*p);
            if (const Prop* p = find(e, "usage")) el.usage = f.enumValue(*p);
            if (const Prop* p = find(e, "streamIndex")) el.stream = f.d.at(p->value);
            if (const Prop* p = find(e, "usageIndex")) el.usageIndex = f.d.at(p->value);
            if (el.usage.empty() || el.usage == "PS_Invalid") continue;
            els.push_back(el);
        }
        // Offset of an element inside its stream = sum of the sizes of the elements before it.
        auto locate = [&](const std::string& usage, int& stream, size_t& off) {
            for (size_t i = 0; i < els.size(); ++i) {
                if (els[i].usage != usage || els[i].usageIndex != 0) continue;
                stream = els[i].stream;
                size_t o = 0;
                for (size_t j = 0; j < i; ++j)
                    if (els[j].stream == stream) {
                        int s = packingSize(els[j].type);
                        if (s < 0) throw std::runtime_error("morphtarget: unknown vertex packing " + els[j].type);
                        o += size_t(s);
                    }
                off = o;
                return &els[i];
            }
            return (Element*)nullptr;
        };
        auto streamSize = [&](int stream) {
            size_t s = 0;
            for (auto& e : els)
                if (e.stream == stream) s += size_t(std::max(0, packingSize(e.type)));
            return s;
        };
        int ps = 0;
        const Element* pe = locate("PS_Position", ps, L.posOff);
        if (!pe || pe->type != "PT_Short4N") throw std::runtime_error("morphtarget: positions are not Short4N");
        if (ps >= (int)strides.size() || ps >= (int)byteOffsets.size()) throw std::runtime_error("morphtarget: bad stream");
        L.posStride = strides[ps];
        L.posBase = byteOffsets[ps];
        if (streamSize(ps) != L.posStride) throw std::runtime_error("morphtarget: vertex layout does not add up");

        int ns = 0, ts = 0;
        const Element* ne = locate("PS_Normal", ns, L.nOff);
        const Element* te = locate("PS_Tangent", ts, L.tOff);
        if (ne && ne->type == "PT_Dec4" && ns < (int)strides.size() && ns < (int)byteOffsets.size() &&
            streamSize(ns) == strides[ns]) {
            L.hasNT = true;
            L.ntStride = strides[ns];
            L.ntBase = byteOffsets[ns];
            L.hasT = te && te->type == "PT_Dec4" && ts == ns;
        }
        chunks.push_back(L);
    }
    if (chunks.empty()) throw std::runtime_error("morphtarget: no render chunks");

    // ---- the GLB's vertices, in WolvenKit's export order (meshes, then primitives)
    std::vector<float> P, N, T;
    int tComp = 4;
    size_t glbVerts = 0;
    size_t total = 0;
    for (auto& c : chunks) total += c.numVertices;
    const nlohmann::json meshes = patch.glb ? patch.glb->doc.value("meshes", nlohmann::json::array()) : nlohmann::json::array();
    static const Glb kNoGlb;
    const Glb& glb = patch.glb ? *patch.glb : kNoGlb;
    for (const auto& mesh : meshes) {
        const nlohmann::json prims = mesh.value("primitives", nlohmann::json::array());
        for (const auto& prim : prims) {
            const auto& a = prim["attributes"];
            int c = 3;
            auto p = readFloatAccessor(glb, a["POSITION"].get<int>(), c);
            const size_t n = p.size() / 3;
            P.insert(P.end(), p.begin(), p.end());
            if (a.contains("NORMAL")) {
                auto v = readFloatAccessor(glb, a["NORMAL"].get<int>(), c);
                N.insert(N.end(), v.begin(), v.end());
            } else N.insert(N.end(), n * 3, 0.f);
            if (a.contains("TANGENT")) {
                auto v = readFloatAccessor(glb, a["TANGENT"].get<int>(), tComp);
                for (size_t i = 0; i < n; ++i)
                    for (int k = 0; k < 3; ++k) T.push_back(v[i * tComp + k]);
            } else T.insert(T.end(), n * 3, 0.f);
            glbVerts += n;
        }
    }
    if (patch.glb && total != glbVerts)
        throw std::runtime_error("the head GLB has " + std::to_string(glbVerts) + " vertices but " +
                                 vanillaMorphtarget.filename().u8string() + " has " + std::to_string(total) +
                                 " - the GLB must be a WolvenKit export of this exact file");

    // ---- patch the render buffer
    std::map<size_t, std::vector<uint8_t>> changed;   // buffer slot -> new raw data
    MorphtargetPatchStats st;
    st.vertices = total;
    const bool moveVertices = patch.glb || patch.displace;
    std::vector<uint8_t> vb;
    if (moveVertices) vb = f.bufferData(bufSlot, kraken);
    size_t gi = 0;
    for (const auto& L : chunks) {
        if (!moveVertices) break;
        for (uint32_t v = 0; v < L.numVertices; ++v, ++gi) {
            const size_t po = L.posBase + size_t(v) * L.posStride + L.posOff;
            if (po + 8 > vb.size()) throw std::runtime_error("morphtarget: vertex data past end of buffer");
            int16_t q[3];
            std::memcpy(q, vb.data() + po, 6);
            double target[3];
            if (patch.glb) {
                gltfToCp(&P[gi * 3], target);
            } else {
                Vec3 cp;
                for (int k = 0; k < 3; ++k) cp[k] = double(q[k]) / 32767.0 * scale[k] + offset[k];
                const Vec3 g(cp.x, cp.z, -cp.y);             // Cyberpunk -> glTF
                const Vec3 d = patch.displace(g);
                const float gf[3] = {float(g.x + d.x), float(g.y + d.y), float(g.z + d.z)};
                gltfToCp(gf, target);
            }
            int16_t nq[3];
            bool moved = false, clamped = false;
            for (int k = 0; k < 3; ++k) {
                if (scale[k] == 0.0) {
                    nq[k] = q[k];
                    continue;
                }
                double r = std::round((target[k] - offset[k]) / scale[k] * 32767.0);
                if (r > 32767.0 || r < -32767.0) clamped = true;
                r = std::clamp(r, -32767.0, 32767.0);
                nq[k] = int16_t(r);
                // Within one quantization step = the original value (export/import round trip).
                if (std::abs(int(nq[k]) - int(q[k])) > 1) moved = true;
            }
            if (!moved) continue;
            ++st.moved;
            if (clamped) ++st.clamped;
            std::memcpy(vb.data() + po, nq, 6);
            if (L.hasNT && patch.glb) {
                const size_t no = L.ntBase + size_t(v) * L.ntStride + L.nOff;
                if (no + 4 <= vb.size()) {
                    double n[3];
                    gltfToCp(&N[gi * 3], n);
                    double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (len > 1e-6) {
                        for (double& x : n) x /= len;
                        uint32_t old = rd32(vb, no);
                        wr32(vb, no, encodeDec4(n, old));
                    }
                }
                if (L.hasT) {
                    const size_t to = L.ntBase + size_t(v) * L.ntStride + L.tOff;
                    if (to + 4 <= vb.size()) {
                        double t[3];
                        gltfToCp(&T[gi * 3], t);
                        double len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                        if (len > 1e-6) {
                            for (double& x : t) x /= len;
                            uint32_t old = rd32(vb, to);
                            wr32(vb, to, encodeDec4(t, old));
                        }
                    }
                }
            }
        }
    }

    if (moveVertices && st.moved) changed[bufSlot] = std::move(vb);

    // ---- character creator face shapes off (eyes / nose / mouth / jaw / ears targets)
    if (patch.neutralizeCharacterCreator) neutralizeCcTargets(f, kraken, changed, st);

    // ---- write the file back: header + objects (edited in place) kept, buffers re-laid out
    std::vector<std::vector<uint8_t>> disk(f.buffers.size());
    std::vector<uint32_t> memSize(f.buffers.size());
    for (size_t i = 0; i < f.buffers.size(); ++i) {
        const auto& b = f.buffers[i];
        memSize[i] = b.memSize;
        auto it = changed.find(i);
        if (it == changed.end()) {
            disk[i].assign(f.d.begin() + b.offset, f.d.begin() + b.offset + b.diskSize);
            continue;
        }
        const std::vector<uint8_t>& raw = it->second;
        memSize[i] = uint32_t(raw.size());
        std::vector<uint8_t> c = kraken.compress(raw);
        if (!c.empty() && c.size() + 8 < raw.size()) {
            disk[i] = {'K', 'A', 'R', 'K', 0, 0, 0, 0};
            std::memcpy(disk[i].data() + 4, &memSize[i], 4);
            disk[i].insert(disk[i].end(), c.begin(), c.end());
            st.recompressed = true;
        } else {
            disk[i] = raw;  // stored uncompressed (diskSize == memSize)
            st.storedRaw = true;
        }
    }
    size_t firstBuf = f.d.size();
    for (auto& b : f.buffers) firstBuf = std::min<size_t>(firstBuf, b.offset);
    std::vector<uint8_t> out(f.d.begin(), f.d.begin() + firstBuf);
    std::vector<size_t> order(f.buffers.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return f.buffers[a].offset < f.buffers[b].offset; });
    for (size_t i : order) {
        const auto& b = f.buffers[i];
        wr32(out, b.entryOffset + 8, uint32_t(out.size()));
        wr32(out, b.entryOffset + 12, uint32_t(disk[i].size()));
        wr32(out, b.entryOffset + 16, memSize[i]);
        wr32(out, b.entryOffset + 20, crc(disk[i].data(), disk[i].size()));
        out.insert(out.end(), disk[i].begin(), disk[i].end());
    }
    // buffer table CRC, buffersEnd, header CRC (computed with 0xDEADBEEF in the CRC field)
    wr32(out, 40 + 12 * 5 + 8, crc(out.data() + f.tableOff[5], size_t(f.tableCount[5]) * 24));
    wr32(out, 28, uint32_t(out.size()));
    wr32(out, 32, 0xDEADBEEFu);
    wr32(out, 32, crc(out.data(), 160));

    fs::create_directories(longPath(outFile.parent_path()));
    writeFileBytes(outFile, out);
    return st;
}

} // namespace ptk

