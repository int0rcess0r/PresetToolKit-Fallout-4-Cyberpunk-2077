#include "Zip.h"

#include "Util.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace ptk {

namespace {

uint32_t crc32(const std::vector<uint8_t>& d) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(v & 0xFF); o.push_back(v >> 8); }
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((v >> (8 * i)) & 0xFF); }

void zipFiles(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
              const std::filesystem::path& zipPath);

} // namespace

void zipDirectory(const std::filesystem::path& root, const std::filesystem::path& zipPath) {
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::recursive_directory_iterator(longPath(root)))
        if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    zipFiles(root, files, zipPath);
}

namespace {
void zipFiles(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
              const std::filesystem::path& zipPath) {
    std::vector<uint8_t> out, central;
    const uint16_t dosTime = 0, dosDate = (uint16_t)(((2024 - 1980) << 9) | (1 << 5) | 1);
    uint16_t count = 0;
    for (const auto& f : files) {
        std::string name = std::filesystem::relative(f, longPath(root)).generic_u8string();
        auto data = readFileBytes(f);
        if (data.size() >= 0xFFFFFFFFu || out.size() >= 0xFFFFFFFFu)
            throw std::runtime_error("zip: file too large for non-zip64 archive: " + name);
        uint32_t crc = crc32(data);
        uint32_t offset = (uint32_t)out.size();

        put32(out, 0x04034b50); put16(out, 20); put16(out, 0x0800 /*utf-8*/); put16(out, 0 /*store*/);
        put16(out, dosTime); put16(out, dosDate); put32(out, crc);
        put32(out, (uint32_t)data.size()); put32(out, (uint32_t)data.size());
        put16(out, (uint16_t)name.size()); put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());

        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0x0800); put16(central, 0);
        put16(central, dosTime); put16(central, dosDate); put32(central, crc);
        put32(central, (uint32_t)data.size()); put32(central, (uint32_t)data.size());
        put16(central, (uint16_t)name.size()); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0); put32(central, 0); put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
        if (++count == 0xFFFF) throw std::runtime_error("zip: too many files");
    }
    uint32_t cdOffset = (uint32_t)out.size();
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, count); put16(out, count);
    put32(out, (uint32_t)central.size()); put32(out, cdOffset); put16(out, 0);
    writeFileBytes(zipPath, out);
}

} // namespace

} // namespace ptk

