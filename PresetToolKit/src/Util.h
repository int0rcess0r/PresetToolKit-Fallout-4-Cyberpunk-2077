#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ptk {

namespace fs = std::filesystem;

// On Windows, returns a "\\?\" extended-length form of long paths (> ~240 chars) so file APIs
// are not limited to 260 characters. Elsewhere returns the path unchanged.
fs::path longPath(const fs::path& p);

std::vector<uint8_t> readFileBytes(const fs::path& p);
std::string readFileText(const fs::path& p);
void writeFileBytes(const fs::path& p, const std::vector<uint8_t>& data);
// Copies a file, replacing the destination even if it exists or is read-only; the copy is left writable.
void copyFileReplace(const fs::path& from, const fs::path& to);
void writeFileText(const fs::path& p, const std::string& text);

// Parses "7A31E", "0x7A31E", "0007A31E" -> value. Returns nullopt if not hex.
std::optional<uint32_t> parseHexId(const std::string& s);
std::string toHex(uint32_t v, int width = 0);

std::string toLower(std::string s);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
// Make a safe identifier (letters, digits, underscore), lower-case.
std::string sanitizeId(const std::string& s);
// Convert '/' to '\\' (Cyberpunk depot paths use backslashes)
std::string toDepotPath(std::string s);
// Convert depot path (backslashes) into a relative filesystem path
fs::path depotToFs(const std::string& depot);

// Runs a command line, returns its exit code. On Windows it is started directly (no console
// window) and its output is forwarded to the log; elsewhere it goes through the shell.
// `env` adds environment variables for that one process only.
int runCommand(const std::string& cmd, bool verbose,
               const std::vector<std::pair<std::string, std::string>>& env = {});

struct Log {
    // Optional sink (the GUI installs one). level: 0 info, 1 warning, 2 debug.
    static std::function<void(int level, const std::string&)> sink;
    // Also append every message to this file (empty = off).
    static void setLogFile(const fs::path& p);
    static bool verbose;
    static void info(const std::string& s);
    static void warn(const std::string& s);
    static void debug(const std::string& s);
};

} // namespace ptk
