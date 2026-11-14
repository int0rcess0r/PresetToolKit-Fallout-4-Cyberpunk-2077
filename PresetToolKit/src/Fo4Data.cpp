#include "Fo4Data.h"

#include "Util.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ptk {

namespace {

std::string normKey(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    while (!s.empty() && s.front() == '\\') s.erase(0, 1);
    return toLower(s);
}

template <class T> bool rd(std::ifstream& f, T& v) { return (bool)f.read(reinterpret_cast<char*>(&v), sizeof(T)); }

} // namespace

Fo4Data::Fo4Data(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    if (fs::is_directory(dir / "Data", ec)) dataDir_ = dir / "Data";
    else dataDir_ = dir;

    std::vector<fs::path> ba2;
    for (auto& e : fs::directory_iterator(dataDir_, ec))
        if (e.is_regular_file(ec) && toLower(e.path().extension().u8string()) == ".ba2") ba2.push_back(e.path());
    // vanilla "Fallout4 - *" first, then mods alphabetically, so later archives override earlier ones
    std::sort(ba2.begin(), ba2.end(), [](const fs::path& a, const fs::path& b) {
        bool va = toLower(a.filename().u8string()).rfind("fallout4 - ", 0) == 0;
        bool vb = toLower(b.filename().u8string()).rfind("fallout4 - ", 0) == 0;
        if (va != vb) return va;
        return a.filename() < b.filename();
    });
    for (const auto& p : ba2) indexArchive(p);
}

void Fo4Data::indexArchive(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return;
    char magic[4], type[4];
    uint32_t version = 0, count = 0;
    uint64_t nameTable = 0;
    if (!f.read(magic, 4) || std::memcmp(magic, "BTDX", 4) != 0) return;
    rd(f, version);
    f.read(type, 4);
    rd(f, count);
    rd(f, nameTable);
    if (std::memcmp(type, "GNRL", 4) != 0) return;   // general archives only (meshes etc., not textures)
    if (version == 2 || version == 3) f.seekg(8, std::ios::cur);  // next-gen header fields
    if (version == 3) f.seekg(4, std::ios::cur);
    struct Rec { uint64_t offset; uint32_t packed, size; };
    std::vector<Rec> recs(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nameHash, dirHash, flags, packed, size, align;
        char ext[4];
        uint64_t offset;
        rd(f, nameHash); f.read(ext, 4); rd(f, dirHash); rd(f, flags);
        rd(f, offset); rd(f, packed); rd(f, size); rd(f, align);
        recs[i] = {offset, packed, size};
    }
    if (!f) return;
    f.seekg((std::streamoff)nameTable);
    size_t ai = archives_.size();
    archives_.push_back(p);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t len = 0;
        if (!rd(f, len)) break;
        std::string name(len, '\0');
        f.read(name.data(), len);
        Entry e;
        e.archive = ai; e.offset = recs[i].offset; e.packed = recs[i].packed; e.size = recs[i].size;
        index_[normKey(name)] = e;
    }
}

std::optional<std::vector<uint8_t>> Fo4Data::read(const std::string& relPath, std::string* source) const {
    if (!valid()) return std::nullopt;
    const std::string key = normKey(relPath);

    // 1. loose file (case-insensitive walk is unnecessary on Windows; try as given)
    std::error_code ec;
    fs::path loose = dataDir_ / depotToFs(key);
    fs::path looseAsGiven = dataDir_ / depotToFs(relPath);
    for (const auto& lp : {looseAsGiven, loose})
        if (fs::is_regular_file(lp, ec)) {
            if (source) *source = lp.u8string();
            return readFileBytes(lp);
        }

    // 2. archives
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    const Entry& e = it->second;
    std::ifstream f(archives_[e.archive], std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg((std::streamoff)e.offset);
    std::vector<uint8_t> out(e.size);
    if (e.packed == 0) {
        f.read(reinterpret_cast<char*>(out.data()), e.size);
    } else {
        std::vector<uint8_t> packed(e.packed);
        f.read(reinterpret_cast<char*>(packed.data()), e.packed);
        mz_ulong len = e.size;
        if (mz_uncompress(out.data(), &len, packed.data(), (mz_ulong)packed.size()) != MZ_OK || len != e.size)
            throw std::runtime_error("could not decompress " + relPath + " from " + archives_[e.archive].u8string());
    }
    if (!f) return std::nullopt;
    if (source) *source = archives_[e.archive].filename().u8string() + " : " + relPath;
    return out;
}

} // namespace ptk
