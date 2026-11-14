#include "Tri.h"

#include <cstring>
#include <stdexcept>

namespace ptk {

TriFile loadTri(const std::vector<uint8_t>& d) {
    auto need = [&](size_t o, size_t n) {
        if (o + n > d.size()) throw std::runtime_error(".tri file is truncated");
    };
    auto u32 = [&](size_t o) { need(o, 4); uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
    auto f32 = [&](size_t o) { need(o, 4); float v; std::memcpy(&v, &d[o], 4); return v; };
    if (d.size() < 64 || std::memcmp(d.data(), "FRTRI003", 8) != 0) throw std::runtime_error("not a FRTRI003 .tri file");
    const uint32_t nv = u32(8), nt = u32(12), nq = u32(16), nuv = u32(28), flags = u32(32);
    const uint32_t nMorph = u32(36), nMod = u32(40), nModV = u32(44);
    (void)nMod;
    TriFile t;
    size_t o = 64;
    for (uint32_t i = 0; i < nv; ++i) t.verts.emplace_back(f32(o + i * 12), f32(o + i * 12 + 4), f32(o + i * 12 + 8));
    o += size_t(nv + nModV) * 12;
    o += size_t(nt) * 12 + size_t(nq) * 16;
    if (flags & 1) o += size_t(nuv) * 8 + size_t(nt) * 12;
    for (uint32_t m = 0; m < nMorph; ++m) {
        const uint32_t len = u32(o);
        o += 4;
        need(o, len);
        std::string name(reinterpret_cast<const char*>(&d[o]), len);
        while (!name.empty() && name.back() == '\0') name.pop_back();
        o += len;
        const float scale = f32(o);
        o += 4;
        need(o, size_t(nv) * 6);
        std::vector<Vec3> delta(nv);
        for (uint32_t i = 0; i < nv; ++i) {
            int16_t q[3];
            std::memcpy(q, &d[o + size_t(i) * 6], 6);
            delta[i] = Vec3(q[0] * scale, q[1] * scale, q[2] * scale);
        }
        o += size_t(nv) * 6;
        t.morphs[name] = std::move(delta);
    }
    return t;
}

} // namespace ptk
