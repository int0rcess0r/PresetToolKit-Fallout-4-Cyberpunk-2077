#include "Util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ptk {

bool Log::verbose = false;
std::function<void(int, const std::string&)> Log::sink;
namespace {
std::mutex g_logMutex;
std::unique_ptr<std::ofstream> g_logFile;
void toFile(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile && *g_logFile) { *g_logFile << s << "\n"; g_logFile->flush(); }
}
} // namespace
void Log::setLogFile(const fs::path& p) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logFile.reset();
    if (p.empty()) return;
    if (p.has_parent_path()) fs::create_directories(longPath(p.parent_path()));
    g_logFile = std::make_unique<std::ofstream>(longPath(p), std::ios::binary | std::ios::trunc);
}
void Log::info(const std::string& s) { toFile(s); if (sink) sink(0, s); else std::cout << s << "\n"; }
void Log::warn(const std::string& s) { toFile("warning: " + s); if (sink) sink(1, s); else std::cerr << "warning: " << s << "\n"; }
void Log::debug(const std::string& s) {
    if (!verbose) return;
    toFile("  [debug] " + s);
    if (sink) sink(2, s); else std::cout << "  [debug] " << s << "\n";
}

fs::path longPath(const fs::path& p) {
#ifdef _WIN32
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    std::wstring w = a.wstring();
    for (auto& c : w) if (c == L'/') c = L'\\';
    if (w.size() < 240 || w.rfind(L"\\\\?\\", 0) == 0) return fs::path(w);
    if (w.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + w.substr(2));   // \\server\share
    return fs::path(L"\\\\?\\" + w);
#else
    return p;
#endif
}

std::vector<uint8_t> readFileBytes(const fs::path& p) {
    std::ifstream f(longPath(p), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + p.u8string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void copyFileReplace(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(longPath(to.parent_path()), ec);
    if (fs::exists(longPath(to), ec)) {
        fs::permissions(longPath(to), fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove(longPath(to), ec);
    }
    fs::copy_file(longPath(from), longPath(to), fs::copy_options::overwrite_existing);
    fs::permissions(longPath(to), fs::perms::owner_write, fs::perm_options::add, ec);
}

std::string readFileText(const fs::path& p) {
    std::ifstream f(longPath(p), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + p.u8string());
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // strip UTF-8 BOM
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

void writeFileBytes(const fs::path& p, const std::vector<uint8_t>& data) {
    if (p.has_parent_path()) fs::create_directories(longPath(p.parent_path()));
    std::ofstream f(longPath(p), std::ios::binary);
    if (!f) throw std::runtime_error("cannot write file: " + p.u8string());
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
}

void writeFileText(const fs::path& p, const std::string& text) {
    if (p.has_parent_path()) fs::create_directories(longPath(p.parent_path()));
    std::ofstream f(longPath(p), std::ios::binary);
    if (!f) throw std::runtime_error("cannot write file: " + p.u8string());
    f << text;
}

std::optional<uint32_t> parseHexId(const std::string& in) {
    std::string s = in;
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() > 8) return std::nullopt;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return std::nullopt;
    return (uint32_t)std::stoul(s, nullptr, 16);
}

std::string toHex(uint32_t v, int width) {
    std::ostringstream ss;
    ss << std::uppercase << std::hex;
    if (width > 0) { ss.width(width); ss.fill('0'); }
    ss << v;
    return ss.str();
}

std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string sanitizeId(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += (char)std::tolower(c);
        else if (c == '_' || c == '-' || c == ' ' || c == '.') {
            if (!out.empty() && out.back() != '_') out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "preset";
    return out;
}

std::string toDepotPath(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    while (!s.empty() && s.front() == '\\') s.erase(0, 1);
    return s;
}

fs::path depotToFs(const std::string& depot) {
    fs::path p;
    std::string part;
    for (char c : depot) {
        if (c == '\\' || c == '/') {
            if (!part.empty()) p /= part;
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) p /= part;
    return p;
}

int runCommand(const std::string& cmd, bool verbose, const std::vector<std::pair<std::string, std::string>>& env) {
    if (verbose) {
        std::string pre;
        for (const auto& [k, v] : env) pre += k + "=" + v + " ";
        Log::info("  $ " + pre + cmd);
    }
    // set the variables for the child (inherited), restore afterwards
    struct EnvGuard {
        std::vector<std::pair<std::string, std::optional<std::string>>> saved;
        explicit EnvGuard(const std::vector<std::pair<std::string, std::string>>& e) {
            for (const auto& [k, v] : e) {
                const char* old = std::getenv(k.c_str());
                saved.push_back({k, old ? std::optional<std::string>(old) : std::nullopt});
#ifdef _WIN32
                _putenv_s(k.c_str(), v.c_str());
                SetEnvironmentVariableA(k.c_str(), v.c_str());
#else
                setenv(k.c_str(), v.c_str(), 1);
#endif
            }
        }
        ~EnvGuard() {
            for (const auto& [k, old] : saved) {
#ifdef _WIN32
                _putenv_s(k.c_str(), old ? old->c_str() : "");
                SetEnvironmentVariableA(k.c_str(), old ? old->c_str() : nullptr);
#else
                if (old) setenv(k.c_str(), old->c_str(), 1); else unsetenv(k.c_str());
#endif
            }
        }
    } guard(env);
#ifdef _WIN32
    auto widen = [](const std::string& u) {
        if (u.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), n);
        return w;
    };
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring line = widen(cmd);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        Log::warn("could not start: " + cmd + " (Windows error " + std::to_string(GetLastError()) + ")");
        return -1;
    }
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string l = pending.substr(0, nl);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) Log::info("    " + l);
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) Log::info("    " + pending);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    std::cout.flush();
    int rc = std::system(cmd.c_str());
    if (rc != -1 && (rc & 0x7f) == 0) rc = (rc >> 8) & 0xff; // WEXITSTATUS
    return rc;
#endif
}

} // namespace ptk
