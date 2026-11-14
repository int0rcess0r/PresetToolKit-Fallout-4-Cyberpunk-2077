// PresetToolKit - simple Windows front end: Fallout 4 character -> Cyberpunk 2077 head replacement, and
// Cyberpunk 2077 save -> Fallout 4 LooksMenu preset.
// Native Win32 + common item dialogs; no third-party GUI libraries.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <knownfolders.h>

#include "Converter.h"
#include "GameSaves.h"
#include "Util.h"

#include <atomic>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace ptk;

namespace {

// ------------------------------------------------------------------ ids / messages
enum : int {
    ID_EDIT_BASE = 100,  // + field index
    ID_BROWSE_BASE = 200,
    ID_NAME = 300,
    ID_STRENGTH,
    ID_CONVERT,
    ID_OPEN_OUT,
    ID_OPEN_WORK,
    ID_LOG,
    ID_ZIP,
    ID_ARCHIVE,
    ID_FROM,
    ID_TO,
    ID_CCOFF,
    ID_APPROX,
    ID_EFS,
    ID_EFS_REDUX,
    ID_TAB,
    ID_X_SRC,
    ID_X_HEAD,
    ID_X_FMT,
    ID_X_EXPORT,
    ID_X_OPEN,
};
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr UINT WM_APP_EXPORT_DONE = WM_APP + 3;

enum Field { F_PRESET, F_CPSAVE, F_FO4, F_CP, F_WK, F_HEAD, F_MESH, F_TPL_MORPH, F_TPL_MESH, F_DEST, F_PRESETDEST, F_FO4SAVE, F_MODEL_OUT, F_COUNT };

struct FieldDef {
    const wchar_t* label;
    const wchar_t* iniKey;
    bool folder;
    const wchar_t* filterName;  // file dialogs
    const wchar_t* filterSpec;
    bool required;
};

const FieldDef kFields[F_COUNT] = {
    {L"LooksMenu preset or FO4 save *", L"preset", false, L"LooksMenu preset or Fallout 4 save", L"*.json;*.fos", true},
    {L"Cyberpunk 2077 save (sav.dat) *", L"cpsave", false, L"Cyberpunk 2077 save", L"sav.dat;*.dat", true},
    {L"Fallout 4 folder", L"fo4", true, nullptr, nullptr, false},
    {L"Cyberpunk 2077 folder", L"cp", true, nullptr, nullptr, false},
    {L"WolvenKit.CLI.exe", L"wolvenkit", false, L"WolvenKit CLI", L"*.exe", false},
    {L"V head .morphtarget GLB", L"head", false, L"glTF binary", L"*.glb", false},
    {L"V head .mesh GLB (unused)", L"mesh", false, L"glTF binary", L"*.glb", false},
    {L"Vanilla .morphtarget", L"tplMorph", false, L"Morph target", L"*.morphtarget", false},
    {L"Vanilla .mesh", L"tplMesh", false, L"Mesh", L"*.mesh", false},
    {L"Destination folder", L"dest", true, nullptr, nullptr, false},
    {L"Preset folder", L"presetDest", true, nullptr, nullptr, false},
    {L"Also put face in FO4 save", L"fo4save", false, L"Fallout 4 save (a copy is edited)", L"*.fos", false},
    {L"Output folder *", L"modelOut", true, nullptr, nullptr, true},
};

// Which way the conversion goes (the two dropdowns): 0 Fallout 4, 1 Cyberpunk 2077.
enum Game { G_FO4 = 0, G_CP = 1 };
int g_fromG = G_FO4, g_toG = G_CP;
int g_page = 0;                        // tabs: 0 = faces (convert), 1 = export model
int g_xSrc = G_FO4, g_xHead = G_CP;    // export model: where the face comes from, whose head mesh
int g_xFmt = 0;                        // 0 = .glb, 1 = .obj
const wchar_t* kModelFormats[2] = {L"glTF binary (.glb)", L"Wavefront (.obj)"};
const wchar_t* kModelExts[2] = {L".glb", L".obj"};
bool g_cpToFo4 = false;   // output is a preset (not V's head)
const int kGames = 2;
const wchar_t* kGameNames[kGames] = {L"Fallout 4", L"Cyberpunk 2077"};
const wchar_t* kGameIds[kGames] = {L"fo4", L"cp"};
int inField() { return (g_page == 1 ? g_xSrc : g_fromG) == G_FO4 ? F_PRESET : F_CPSAVE; }
int outField() { return g_toG == G_CP ? F_DEST : F_PRESETDEST; }
// per output kind ini keys: name, zip, standalone file
const wchar_t* outKey(int which) {
    static const wchar_t* k[kGames][3] = {{L"presetName", L"presetZip", L"presetJson"}, {L"name", L"zip", L"archive"}};
    return k[g_toG][which];
}
bool fieldVisible(int f) {
    if (g_page == 1) {
        switch (f) {
        case F_PRESET: case F_CPSAVE: return f == inField();
        case F_FO4: case F_CP: case F_WK: case F_HEAD: case F_MODEL_OUT: return true;
        default: return false;
        }
    }
    switch (f) {
    case F_MODEL_OUT: return false;
    case F_PRESET: case F_CPSAVE: return f == inField();
    case F_DEST: case F_PRESETDEST: return f == outField();
    case F_FO4SAVE: return g_toG == G_FO4;
    default: return true;
    }
}

struct DoneMsg {
    bool ok = false;
    std::wstring message;
    ConvertResult result;
};

HWND g_wnd = nullptr;
HWND g_label[F_COUNT], g_edit[F_COUNT], g_browse[F_COUNT];
HWND g_zip, g_archive, g_ccOff, g_approx, g_efs, g_efsRedux;
HWND g_fromLabel, g_from, g_toLabel, g_to, g_groupDir;
HWND g_nameLabel, g_name, g_strengthLabel, g_strength, g_convert, g_openOut, g_openWork, g_log, g_hint;
HWND g_groupIn, g_groupGames, g_groupWk, g_groupOut;
HWND g_tab, g_groupExp, g_xSrcLabel, g_xSrcC, g_xHeadLabel, g_xHeadC, g_xFmtLabel, g_xFmtC, g_xExport, g_xOpen, g_xNote;
std::vector<HWND> g_faceCtls, g_exportCtls;   // shown only on their own tab
HFONT g_font = nullptr;
double g_scale = 1.0;
std::atomic<bool> g_busy{false};
fs::path g_exeDir, g_ini, g_lastWorkDir;

int S(int v) { return (int)(v * g_scale + 0.5); }

// ------------------------------------------------------------------ string helpers
std::wstring widen(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), u.data(), n, nullptr, nullptr);
    return u;
}
std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    // trim spaces and surrounding quotes (pasted paths)
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'"')) ++a;
    return s.substr(a);
}

// ------------------------------------------------------------------ settings (ini next to the exe)
std::wstring iniGet(const wchar_t* key) {
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"PresetToolKit", key, L"", buf, 2048, g_ini.c_str());
    return buf;
}
void iniSet(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"PresetToolKit", key, v.c_str(), g_ini.c_str());
}
void saveSettings() {
    for (int i = 0; i < F_COUNT; ++i) iniSet(kFields[i].iniKey, getText(g_edit[i]));
    iniSet(outKey(0), getText(g_name));
    iniSet(L"from", kGameIds[g_fromG]);
    iniSet(L"to", kGameIds[g_toG]);
    iniSet(L"ccOff", SendMessageW(g_ccOff, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"approx", SendMessageW(g_approx, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"efs", SendMessageW(g_efs, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"efsRedux", SendMessageW(g_efsRedux, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    iniSet(L"strength", getText(g_strength));
    iniSet(L"tab", g_page == 1 ? L"export" : L"faces");
    iniSet(L"modelFrom", kGameIds[g_xSrc]);
    iniSet(L"modelHead", kGameIds[g_xHead]);
    iniSet(L"modelFormat", g_xFmt == 1 ? L"obj" : L"glb");
    iniSet(outKey(1), SendMessageW(g_zip, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    iniSet(outKey(2), SendMessageW(g_archive, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
}
void loadSettings() {
    for (int i = 0; i < F_COUNT; ++i) SetWindowTextW(g_edit[i], iniGet(kFields[i].iniKey).c_str());
    auto gameOf = [](const std::wstring& id, int def) {
        for (int g = 0; g < kGames; ++g) if (id == kGameIds[g]) return g;
        return def;
    };
    const bool old = iniGet(L"direction") == L"cp2fo4";
    g_fromG = gameOf(iniGet(L"from"), old ? G_CP : G_FO4);
    g_toG = gameOf(iniGet(L"to"), old ? G_FO4 : G_CP);
    if (g_toG == g_fromG) g_toG = g_fromG == G_CP ? G_FO4 : G_CP;
    g_cpToFo4 = g_toG != G_CP;
    SendMessageW(g_ccOff, BM_SETCHECK, iniGet(L"ccOff") == L"0" ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(g_approx, BM_SETCHECK, iniGet(L"approx") == L"0" ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(g_efs, BM_SETCHECK, iniGet(L"efs") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_efsRedux, BM_SETCHECK, iniGet(L"efsRedux") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    if (iniGet(L"efsRedux") == L"1") SendMessageW(g_efs, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(g_from, CB_SETCURSEL, g_fromG, 0);
    SendMessageW(g_to, CB_SETCURSEL, g_toG, 0);
    g_page = iniGet(L"tab") == L"export" ? 1 : 0;
    TabCtrl_SetCurSel(g_tab, g_page);
    g_xSrc = gameOf(iniGet(L"modelFrom"), G_FO4);
    g_xHead = gameOf(iniGet(L"modelHead"), G_CP);
    g_xFmt = iniGet(L"modelFormat") == L"obj" ? 1 : 0;
    SendMessageW(g_xSrcC, CB_SETCURSEL, g_xSrc, 0);
    SendMessageW(g_xHeadC, CB_SETCURSEL, g_xHead, 0);
    SendMessageW(g_xFmtC, CB_SETCURSEL, g_xFmt, 0);
    SetWindowTextW(g_name, iniGet(outKey(0)).c_str());
    std::wstring st = iniGet(L"strength");
    SetWindowTextW(g_strength, st.empty() ? L"1.0" : st.c_str());
    SendMessageW(g_zip, BM_SETCHECK, iniGet(outKey(1)) == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_archive, BM_SETCHECK, iniGet(outKey(2)) == L"0" ? BST_UNCHECKED : BST_CHECKED, 0);
}

// ------------------------------------------------------------------ log panel
void appendLog(const std::wstring& line) {
    std::wstring t = line;
    // normalise to CRLF for the edit control
    std::wstring out;
    for (wchar_t c : t) {
        if (c == L'\n') out += L"\r\n";
        else if (c != L'\r') out += c;
    }
    out += L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)out.c_str());
}
void postLog(int level, const std::string& s) {
    std::wstring w = widen(s);
    if (level == 1) w = L"warning: " + w;
    PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(std::move(w)));
}

// ------------------------------------------------------------------ dialogs
std::wstring pickPath(HWND owner, const FieldDef& f, const std::wstring& current) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (f.folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!f.folder && f.filterSpec) {
        COMDLG_FILTERSPEC spec[2] = {{f.filterName, f.filterSpec}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, spec);
    }
    std::wstring title = std::wstring(L"Select ") + f.label;
    if (!title.empty() && title.back() == L'*') title.resize(title.size() - 2);
    dlg->SetTitle(title.c_str());
    // start in the folder of the current value, or where the game keeps these files
    fs::path startDir;
    if (current.empty()) {
        PWSTR kp = nullptr;
        if (f.iniKey == std::wstring(L"cpsave") && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &kp)) && kp) {
            startDir = fs::path(kp) / L"CD Projekt Red" / L"Cyberpunk 2077";
        } else if (f.iniKey == std::wstring(L"preset") && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &kp)) && kp) {
            startDir = fs::path(kp) / L"My Games" / L"Fallout4" / L"Saves";
        }
        if (kp) CoTaskMemFree(kp);
        std::error_code ec;
        if (!startDir.empty() && fs::is_directory(startDir, ec)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
    }
    if (!current.empty()) {
        fs::path p(current);
        fs::path dir = f.folder ? p : p.parent_path();
        std::error_code ec;
        if (!dir.empty() && fs::is_directory(dir, ec)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// When the head .morphtarget GLB comes from a WolvenKit project (...\source\raw\...), fill in the
// matching .mesh GLB and the vanilla .morphtarget / .mesh from ...\source\archive\... if they exist.
void autoFillFromProject(const std::wstring& headGlb) {
    fs::path glb(headGlb);
    std::wstring s = glb.wstring();
    std::wstring lower = s;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    const std::wstring rawKey = L"\\source\\raw\\";
    size_t pos = lower.rfind(rawKey);
    if (pos == std::wstring::npos) return;
    std::wstring archiveRoot = s.substr(0, pos) + L"\\source\\archive\\";
    std::wstring rel = s.substr(pos + rawKey.size());                     // ...\h0_000_pwa__morphs.morphtarget.glb
    std::error_code ec;
    auto fillIfEmpty = [&](Field f, const fs::path& p) {
        if (getText(g_edit[f]).empty() && fs::exists(p, ec)) {
            SetWindowTextW(g_edit[f], p.c_str());
            appendLog(L"Auto-filled " + std::wstring(kFields[f].label) + L": " + p.wstring());
        }
    };
    if (rel.size() > 4 && _wcsicmp(rel.c_str() + rel.size() - 4, L".glb") == 0) {
        fillIfEmpty(F_TPL_MORPH, fs::path(archiveRoot + rel.substr(0, rel.size() - 4)));
    }
    // h0_000_pwa__morphs  ->  h0_000_pwa_c__basehead\h0_000_pwa_c__basehead
    std::wstring stem = glb.filename().wstring();
    size_t m = stem.find(L"__morphs");
    if (m != std::wstring::npos) {
        std::wstring base = stem.substr(0, m) + L"_c__basehead";
        fs::path relDir = fs::path(rel).parent_path();
        (void)relDir; (void)base;   // the .mesh is left vanilla - nothing to fill in
    }
}

// Work files go to %LOCALAPPDATA%\\PresetToolKit\\work (short path - Windows limits paths to 260 chars).
fs::path workRoot() {
    PWSTR p = nullptr;
    fs::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
        root = fs::path(p) / L"PresetToolKit" / L"work";
        CoTaskMemFree(p);
    } else {
        root = g_exeDir / L"PresetToolKit_work";
    }
    return root;
}

void openFolder(const fs::path& p) {
    if (p.empty()) return;
    ShellExecuteW(g_wnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ------------------------------------------------------------------ layout
HWND makeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

void layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = S(10), labelW = S(170), btnW = S(80), rowH = S(24), gap = S(6);
    const int editX = m + S(10) + labelW, editW = std::max(S(120), W - editX - btnW - m - S(16));
    int y = m;
    MoveWindow(g_tab, m, y, W - 2 * m, S(26), TRUE);
    y += S(32);

    auto group = [&](HWND g, int rows) {
        int h = S(22) + rows * (rowH + gap) + S(4);
        MoveWindow(g, m, y, W - 2 * m, h, TRUE);
        y += S(20);
        return h;
    };
    auto row = [&](int i) {
        MoveWindow(g_label[i], m + S(10), y + S(4), labelW, rowH, TRUE);
        MoveWindow(g_edit[i], editX, y, editW, rowH, TRUE);
        MoveWindow(g_browse[i], editX + editW + S(6), y, btnW, rowH, TRUE);
        y += rowH + gap;
    };

    for (int i = 0; i < F_COUNT; ++i) {
        const int sw = fieldVisible(i) ? SW_SHOW : SW_HIDE;
        ShowWindow(g_label[i], sw); ShowWindow(g_edit[i], sw); ShowWindow(g_browse[i], sw);
    }
    for (HWND h : g_faceCtls) ShowWindow(h, g_page == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : g_exportCtls) ShowWindow(h, g_page == 1 ? SW_SHOW : SW_HIDE);
    const int bh = S(30);

    if (g_page == 1) {
        // ---- Export model: the face as a 3D model on Cyberpunk's or Fallout 4's head mesh
        int gy = y, gh = group(g_groupExp, 4);
        MoveWindow(g_xSrcLabel, m + S(10), y + S(4), labelW, rowH, TRUE);
        MoveWindow(g_xSrcC, editX, y, S(220), S(200), TRUE);
        MoveWindow(g_xHeadLabel, editX + S(235), y + S(4), S(75), rowH, TRUE);
        MoveWindow(g_xHeadC, editX + S(310), y, std::max(S(120), std::min(S(230), editX + editW - (editX + S(310)))), S(200), TRUE);
        y += rowH + gap;
        row(inField());
        row(F_MODEL_OUT);
        MoveWindow(g_xFmtLabel, m + S(10), y + S(4), labelW, rowH, TRUE);
        MoveWindow(g_xFmtC, editX, y, S(220), S(200), TRUE);
        y += rowH + gap;
        y = gy + gh + gap;

        gy = y; gh = group(g_groupGames, 5);
        row(F_FO4); row(F_CP); row(F_WK);
        MoveWindow(g_efs, editX, y, editW, rowH, TRUE);
        y += rowH + gap;
        MoveWindow(g_efsRedux, editX, y, editW, rowH, TRUE);
        y += rowH + gap;
        y = gy + gh + gap;

        gy = y; gh = group(g_groupWk, 1);
        row(F_HEAD);
        y = gy + gh + gap;

        MoveWindow(g_xExport, m, y, S(140), bh, TRUE);
        MoveWindow(g_xOpen, m + S(150), y, S(170), bh, TRUE);
        MoveWindow(g_hint, m + S(330), y + S(7), S(90), rowH, TRUE);
        y += bh + gap;
        MoveWindow(g_xNote, m, y, W - 2 * m, S(48), TRUE);
        y += S(48) + gap;
        MoveWindow(g_log, m, y, W - 2 * m, std::max(S(60), H - y - m), TRUE);
        return;
    }

    int gy = y, gh = group(g_groupDir, 1);
    MoveWindow(g_fromLabel, m + S(10), y + S(4), S(60), rowH, TRUE);
    MoveWindow(g_from, m + S(70), y, S(170), S(200), TRUE);
    MoveWindow(g_toLabel, m + S(260), y + S(4), S(30), rowH, TRUE);
    MoveWindow(g_to, m + S(290), y, S(170), S(200), TRUE);
    y = gy + gh + gap;

    gy = y; gh = group(g_groupIn, 1);
    row(inField());
    y = gy + gh + gap;

    gy = y; gh = group(g_groupGames, 5);
    row(F_FO4); row(F_CP); row(F_WK);
    MoveWindow(g_efs, editX, y, editW, rowH, TRUE);
    y += rowH + gap;
    MoveWindow(g_efsRedux, editX, y, editW, rowH, TRUE);
    y += rowH + gap;
    y = gy + gh + gap;

    gy = y; gh = group(g_groupWk, 4);
    row(F_HEAD); row(F_MESH); row(F_TPL_MORPH); row(F_TPL_MESH);
    y = gy + gh + gap;

    const bool extraOut = g_toG == G_FO4;
    gy = y; gh = group(g_groupOut, extraOut ? 5 : 4);
    row(outField());
    if (g_toG == G_FO4) row(F_FO4SAVE);
    MoveWindow(g_nameLabel, m + S(10), y + S(4), labelW, rowH, TRUE);
    int nameW = std::max(S(100), editW - S(150));
    MoveWindow(g_name, editX, y, nameW, rowH, TRUE);
    MoveWindow(g_strengthLabel, editX + nameW + S(10), y + S(4), S(60), rowH, TRUE);
    MoveWindow(g_strength, editX + nameW + S(70), y, editX + editW - (editX + nameW + S(70)), rowH, TRUE);
    y += rowH + gap;
    {
        int half = editW / 2;
        MoveWindow(g_archive, editX, y, half - S(5), rowH, TRUE);
        MoveWindow(g_zip, editX + half, y, editW - half, rowH, TRUE);
    }
    const bool showCc = g_toG == G_CP, showApprox = g_toG == G_FO4;
    ShowWindow(g_ccOff, showCc ? SW_SHOW : SW_HIDE);
    ShowWindow(g_approx, showApprox ? SW_SHOW : SW_HIDE);
    y += rowH + gap;
    MoveWindow(showApprox ? g_approx : g_ccOff, editX, y, editW, rowH, TRUE);
    y = gy + gh + gap;

    MoveWindow(g_convert, m, y, S(140), bh, TRUE);
    MoveWindow(g_openOut, m + S(150), y, S(170), bh, TRUE);
    MoveWindow(g_openWork, m + S(330), y, S(150), bh, TRUE);
    MoveWindow(g_hint, m + S(490), y + S(7), std::max(S(50), W - m - S(490) - m), rowH, TRUE);
    y += bh + gap;

    MoveWindow(g_log, m, y, W - 2 * m, std::max(S(60), H - y - m), TRUE);
}

void updateModeTexts();

void createControls() {
    g_tab = makeCtl(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, ID_TAB);
    {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(L"Faces");
        TabCtrl_InsertItem(g_tab, 0, &ti);
        ti.pszText = const_cast<wchar_t*>(L"Export model");
        TabCtrl_InsertItem(g_tab, 1, &ti);
    }
    g_groupDir = makeCtl(L"BUTTON", L"Convert", BS_GROUPBOX, -1);
    g_fromLabel = makeCtl(L"STATIC", L"From", SS_LEFT, -1);
    g_from = makeCtl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_FROM);
    g_toLabel = makeCtl(L"STATIC", L"to", SS_LEFT, -1);
    g_to = makeCtl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_TO);
    for (HWND c : {g_from, g_to}) {
        for (int g = 0; g < kGames; ++g) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)kGameNames[g]);
        SendMessageW(c, CB_SETCURSEL, c == g_from ? G_FO4 : G_CP, 0);
    }
    g_groupIn = makeCtl(L"BUTTON", L"Fallout 4 character", BS_GROUPBOX, -1);
    g_groupGames = makeCtl(L"BUTTON", L"Game folders (files are read from the games' archives - nothing to extract)", BS_GROUPBOX, -1);
    g_groupWk = makeCtl(L"BUTTON", L"V's head from a WolvenKit project (optional - overrides the Cyberpunk folder)", BS_GROUPBOX, -1);
    g_groupOut = makeCtl(L"BUTTON", L"Output", BS_GROUPBOX, -1);
    for (int i = 0; i < F_COUNT; ++i) {
        g_label[i] = makeCtl(L"STATIC", kFields[i].label, SS_LEFT, -1);
        g_edit[i] = makeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_EDIT_BASE + i, WS_EX_CLIENTEDGE);
        g_browse[i] = makeCtl(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, ID_BROWSE_BASE + i);
    }
    g_nameLabel = makeCtl(L"STATIC", L"Mod name", SS_LEFT, -1);
    g_name = makeCtl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_NAME, WS_EX_CLIENTEDGE);
    g_strengthLabel = makeCtl(L"STATIC", L"Strength", SS_LEFT, -1);
    g_strength = makeCtl(L"EDIT", L"1.0", WS_TABSTOP | ES_AUTOHSCROLL, ID_STRENGTH, WS_EX_CLIENTEDGE);
    g_archive = makeCtl(L"BUTTON", L"Standalone .archive (drop-in for archive\\pc\\mod)",
                        WS_TABSTOP | BS_AUTOCHECKBOX, ID_ARCHIVE);
    g_zip = makeCtl(L"BUTTON", L"Mod manager .zip (Vortex / MO2)",
                    WS_TABSTOP | BS_AUTOCHECKBOX, ID_ZIP);
    g_ccOff = makeCtl(L"BUTTON", L"Remove V's character creator face shapes (eyes, nose, mouth, jaw, ears) so the face is exactly the preset",
                      WS_TABSTOP | BS_AUTOCHECKBOX, ID_CCOFF);
    g_approx = makeCtl(L"BUTTON", L"Approximate customization (hair colour, eyes, skin tone, brows, freckles, blemishes, makeup from the save)",
                       WS_TABSTOP | BS_AUTOCHECKBOX, ID_APPROX);
    g_efsRedux = makeCtl(L"BUTTON", L"Extended Facial Sculpting REDUX (Fallout 4 mod): more sliders, the closest presets",
                         WS_TABSTOP | BS_AUTOCHECKBOX, ID_EFS_REDUX);
    g_efs = makeCtl(L"BUTTON", L"Extended Facial Sculpting (Fallout 4 mod): use its extra sculpt sliders",
                    WS_TABSTOP | BS_AUTOCHECKBOX, ID_EFS);
    g_convert = makeCtl(L"BUTTON", L"Convert", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_CONVERT);
    g_openOut = makeCtl(L"BUTTON", L"Open destination folder", WS_TABSTOP | BS_PUSHBUTTON, ID_OPEN_OUT);
    g_openWork = makeCtl(L"BUTTON", L"Open work folder", WS_TABSTOP | BS_PUSHBUTTON, ID_OPEN_WORK);
    g_hint = makeCtl(L"STATIC", L"* required", SS_LEFT, -1);
    g_log = makeCtl(L"EDIT", L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                    ID_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
    EnableWindow(g_openWork, FALSE);

    // ---- Export model tab
    g_groupExp = makeCtl(L"BUTTON", L"Export model: the generated face as a 3D model", BS_GROUPBOX, -1);
    g_xSrcLabel = makeCtl(L"STATIC", L"Face from", SS_LEFT, -1);
    g_xSrcC = makeCtl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_X_SRC);
    SendMessageW(g_xSrcC, CB_ADDSTRING, 0, (LPARAM)L"Fallout 4 preset or save");
    SendMessageW(g_xSrcC, CB_ADDSTRING, 0, (LPARAM)L"Cyberpunk 2077 save");
    g_xHeadLabel = makeCtl(L"STATIC", L"Head mesh", SS_LEFT, -1);
    g_xHeadC = makeCtl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_X_HEAD);
    SendMessageW(g_xHeadC, CB_ADDSTRING, 0, (LPARAM)L"Fallout 4");
    SendMessageW(g_xHeadC, CB_ADDSTRING, 0, (LPARAM)L"Cyberpunk 2077 (V)");
    g_xFmtLabel = makeCtl(L"STATIC", L"Format", SS_LEFT, -1);
    g_xFmtC = makeCtl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_X_FMT);
    for (const wchar_t* f : kModelFormats) SendMessageW(g_xFmtC, CB_ADDSTRING, 0, (LPARAM)f);
    SendMessageW(g_xSrcC, CB_SETCURSEL, g_xSrc, 0);
    SendMessageW(g_xHeadC, CB_SETCURSEL, g_xHead, 0);
    SendMessageW(g_xFmtC, CB_SETCURSEL, g_xFmt, 0);
    g_xExport = makeCtl(L"BUTTON", L"Export model", WS_TABSTOP | BS_PUSHBUTTON, ID_X_EXPORT);
    g_xOpen = makeCtl(L"BUTTON", L"Open output folder", WS_TABSTOP | BS_PUSHBUTTON, ID_X_OPEN);
    g_xNote = makeCtl(L"STATIC", L"The face is built as it is for the mod / preset, then saved as a model (metres, Y up) that opens in "
                                 L"Blender and other 3D tools. Cyberpunk 2077's head needs the Cyberpunk folder + WolvenKit (or V's head "
                                 L"GLB); Fallout 4's head needs the Fallout 4 folder. Both heads are saved in the same place, so they line up.",
                      SS_LEFT, -1);

    g_faceCtls = {g_groupDir, g_fromLabel, g_from, g_toLabel, g_to, g_groupIn, g_groupOut, g_nameLabel, g_name,
                  g_strengthLabel, g_strength, g_archive, g_zip, g_ccOff, g_approx, g_convert, g_openOut, g_openWork};
    g_exportCtls = {g_groupExp, g_xSrcLabel, g_xSrcC, g_xHeadLabel, g_xHeadC, g_xFmtLabel, g_xFmtC, g_xExport, g_xOpen, g_xNote};
}

void updateModeTexts() {
    SetWindowTextW(g_groupIn, (std::wstring(kGameNames[g_fromG]) + L" character").c_str());
    const wchar_t* out = g_toG == G_CP ? L"Output: V's head for Cyberpunk 2077" : L"Output: LooksMenu preset for Fallout 4";
    SetWindowTextW(g_groupOut, out);
    SetWindowTextW(g_nameLabel, g_toG == G_CP ? L"Mod name" : L"Preset name");
    SetWindowTextW(g_openOut, g_toG == G_CP ? L"Open destination folder" : L"Open preset folder");
    SetWindowTextW(g_archive, g_toG == G_CP ? L"Standalone .archive (drop-in for archive\\pc\\mod)" : L"Standalone preset .json");
    SetWindowTextW(g_zip, L"Mod manager .zip (Vortex / MO2)");
    SetWindowTextW(g_convert, L"Convert");
}

// Switches direction. The two dropdowns never show the same game.
void setDirection(int fromG, int toG) {
    if (fromG == toG) {
        // picked the game on the other side: swap
        if (fromG != g_fromG) toG = g_fromG;
        else fromG = g_toG;
    }
    if (fromG == g_fromG && toG == g_toG) {
        SendMessageW(g_from, CB_SETCURSEL, g_fromG, 0);
        SendMessageW(g_to, CB_SETCURSEL, g_toG, 0);
        return;
    }
    iniSet(outKey(0), getText(g_name));
    iniSet(outKey(1), SendMessageW(g_zip, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    iniSet(outKey(2), SendMessageW(g_archive, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    g_fromG = fromG;
    g_toG = toG;
    g_cpToFo4 = toG != G_CP;
    SendMessageW(g_zip, BM_SETCHECK, iniGet(outKey(1)) == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_archive, BM_SETCHECK, iniGet(outKey(2)) == L"0" ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(g_from, CB_SETCURSEL, g_fromG, 0);
    SendMessageW(g_to, CB_SETCURSEL, g_toG, 0);
    SetWindowTextW(g_name, iniGet(outKey(0)).c_str());
    if (toG == G_FO4 && getText(g_edit[F_PRESETDEST]).empty() && !getText(g_edit[F_FO4]).empty()) {
        fs::path d = fs::path(getText(g_edit[F_FO4])) / L"Data" / L"F4SE" / L"Plugins" / L"F4EE" / L"Presets";
        SetWindowTextW(g_edit[F_PRESETDEST], d.c_str());
    }
    updateModeTexts();
    layout();
    InvalidateRect(g_wnd, nullptr, TRUE);
}

void setBusy(bool busy) {
    g_busy = busy;
    for (int i = 0; i < F_COUNT; ++i) { EnableWindow(g_edit[i], !busy); EnableWindow(g_browse[i], !busy); }
    EnableWindow(g_name, !busy);
    EnableWindow(g_strength, !busy);
    EnableWindow(g_zip, !busy);
    EnableWindow(g_archive, !busy);
    EnableWindow(g_from, !busy);
    EnableWindow(g_ccOff, !busy);
    EnableWindow(g_approx, !busy);
    EnableWindow(g_efs, !busy);
    EnableWindow(g_efsRedux, !busy);
    EnableWindow(g_to, !busy);
    EnableWindow(g_convert, !busy);
    SetWindowTextW(g_convert, busy ? L"Working..." : L"Convert");
    for (HWND h : {g_xSrcC, g_xHeadC, g_xFmtC, g_xExport, g_tab}) EnableWindow(h, !busy);
    SetWindowTextW(g_xExport, busy ? L"Working..." : L"Export model");
}

// ------------------------------------------------------------------ conversion
void startConversion() {
    if (g_busy) return;
    std::wstring v[F_COUNT];
    for (int i = 0; i < F_COUNT; ++i) v[i] = getText(g_edit[i]);
    for (int i = 0; i < F_COUNT; ++i) {
        if (!fieldVisible(i)) continue;
        if (kFields[i].required && v[i].empty()) {
            std::wstring msg = std::wstring(L"Please select: ") + kFields[i].label;
            MessageBoxW(g_wnd, msg.c_str(), L"PresetToolKit", MB_ICONWARNING);
            return;
        }
        std::error_code ec;
        if (!v[i].empty() && i != F_DEST && i != F_PRESETDEST && !fs::exists(fs::path(v[i]), ec)) {
            std::wstring msg = std::wstring(L"Not found:\n") + v[i];
            MessageBoxW(g_wnd, msg.c_str(), L"PresetToolKit", MB_ICONWARNING);
            return;
        }
    }
    double strength = 1.0;
    try {
        std::wstring st = getText(g_strength);
        strength = st.empty() ? 1.0 : std::stod(st);
    } catch (...) {
        MessageBoxW(g_wnd, L"Strength must be a number, e.g. 1.0", L"PresetToolKit", MB_ICONWARNING);
        return;
    }
    if (v[F_HEAD].empty() && v[F_CP].empty()) {
        MessageBoxW(g_wnd, L"Select the Cyberpunk 2077 folder (V's head is read from the game), or V's head .morphtarget GLB "
                           L"from a WolvenKit project.", L"PresetToolKit", MB_ICONWARNING);
        return;
    }
    if (v[F_HEAD].empty() && v[F_WK].empty()) {
        MessageBoxW(g_wnd, L"Reading V's head from the Cyberpunk 2077 folder needs WolvenKit.CLI.exe.", L"PresetToolKit", MB_ICONWARNING);
        return;
    }
    bool wantArchive = SendMessageW(g_archive, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool wantZip = SendMessageW(g_zip, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_cpToFo4) {
        if (!wantArchive && !wantZip && v[F_FO4SAVE].empty()) {
            MessageBoxW(g_wnd, L"Tick at least one output: the preset .json and/or the mod manager .zip, or choose a Fallout 4 save to put the face in.", L"PresetToolKit", MB_ICONWARNING);
            return;
        }
        if ((wantArchive || wantZip) && v[F_PRESETDEST].empty()) {
            MessageBoxW(g_wnd, L"Choose the preset folder (Fallout 4\\Data\\F4SE\\Plugins\\F4EE\\Presets).",
                        L"PresetToolKit", MB_ICONWARNING);
            return;
        }
        if (v[F_FO4].empty() &&
            MessageBoxW(g_wnd, L"Without the Fallout 4 folder the built-in face data is used. Continue?", L"PresetToolKit",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    } else if (!v[F_WK].empty() && !wantArchive && !wantZip) {
        MessageBoxW(g_wnd, L"Tick at least one output: the standalone .archive and/or the mod manager .zip.",
                    L"PresetToolKit", MB_ICONWARNING);
        return;
    }
    if (!g_cpToFo4 && !v[F_WK].empty() && v[F_DEST].empty()) {
        MessageBoxW(g_wnd, L"Choose a destination folder (for example Cyberpunk 2077\\archive\\pc\\mod).",
                    L"PresetToolKit", MB_ICONWARNING);
        return;
    }
    saveSettings();

    ConvertOptions o;
    o.preset = fs::path(v[F_PRESET]);
    o.headGlb = fs::path(v[F_HEAD]);
    o.fo4Dir = fs::path(v[F_FO4]);
    o.cpGameDir = fs::path(v[F_CP]);
    o.cacheDir = g_exeDir / "PresetToolKit_cache";
    o.headMeshGlb = fs::path(v[F_MESH]);
    o.wolvenkit = fs::path(v[F_WK]);
    o.templateMorphtarget = fs::path(v[F_TPL_MORPH]);
    o.templateMesh = fs::path(v[F_TPL_MESH]);
    o.archiveDest = fs::path(v[F_DEST]);
    o.modName = narrow(getText(g_name));
    o.strength = strength;
    o.archiveToDest = wantArchive;
    o.zipToDest = wantZip;
    o.dataDir = defaultDataDir(g_exeDir);
    o.cpToFo4 = g_fromG == G_CP && g_toG == G_FO4;
    o.keepCharacterCreatorShapes = SendMessageW(g_ccOff, BM_GETCHECK, 0, 0) != BST_CHECKED;
    o.cpSave = fs::path(v[F_CPSAVE]);
    o.presetOut = fs::path(v[F_PRESETDEST]);
    o.approximateCustomization = SendMessageW(g_approx, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.extendedSculpting = SendMessageW(g_efs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.extendedSculptingRedux = SendMessageW(g_efsRedux, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_toG == G_FO4) o.fo4SaveTarget = fs::path(v[F_FO4SAVE]);
    std::string workName;
    if (g_fromG == G_CP) {
        fs::path sp(v[F_CPSAVE]);
        std::string saveName = narrow((sp.filename() == L"sav.dat" ? sp.parent_path().filename() : sp.stem()).wstring());
        workName = o.modName.empty() ? "CP_" + sanitizeId(saveName) : o.modName;
        if (o.modName.empty()) o.modName = workName;
    } else {
        workName = o.modName.empty() ? "LM_" + sanitizeId(narrow(o.preset.stem().wstring())) + "_Head" : o.modName;
    }
    o.workDir = workRoot() / fs::path(widen(workName));
    g_lastWorkDir = o.workDir;

    SetWindowTextW(g_log, L"");
    setBusy(true);
    std::thread([o]() {
        auto* done = new DoneMsg;
        try {
            done->result = runConversion(o);
            done->ok = true;
        } catch (const std::exception& e) {
            done->message = widen(e.what());
        }
        PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)done);
    }).detach();
}

// ------------------------------------------------------------------ export model
struct ExportDoneMsg {
    bool ok = false;
    std::wstring message;
    ModelExportResult result;
};

void startExport() {
    if (g_busy) return;
    std::wstring v[F_COUNT];
    for (int i = 0; i < F_COUNT; ++i) v[i] = getText(g_edit[i]);
    for (int i = 0; i < F_COUNT; ++i) {
        if (!fieldVisible(i)) continue;
        if (kFields[i].required && v[i].empty()) {
            MessageBoxW(g_wnd, (std::wstring(L"Please select: ") + kFields[i].label).c_str(), L"PresetToolKit", MB_ICONWARNING);
            return;
        }
        std::error_code ec;
        if (!v[i].empty() && i != F_MODEL_OUT && !fs::exists(fs::path(v[i]), ec)) {
            MessageBoxW(g_wnd, (std::wstring(L"Not found:\n") + v[i]).c_str(), L"PresetToolKit", MB_ICONWARNING);
            return;
        }
    }
    if (v[F_HEAD].empty() && (v[F_CP].empty() || v[F_WK].empty())) {
        MessageBoxW(g_wnd, L"V's head is needed for both head meshes (it is what the faces are lined up with): select the "
                           L"Cyberpunk 2077 folder and WolvenKit.CLI.exe, or V's head .morphtarget GLB.",
                    L"PresetToolKit", MB_ICONWARNING);
        return;
    }
    if (g_xHead == G_FO4 && v[F_FO4].empty()) {
        MessageBoxW(g_wnd, L"Fallout 4's head mesh is read from the game: select the Fallout 4 folder.", L"PresetToolKit",
                    MB_ICONWARNING);
        return;
    }
    saveSettings();

    ConvertOptions o;
    o.cpToFo4 = g_xSrc == G_CP;
    if (o.cpToFo4) o.cpSave = fs::path(v[F_CPSAVE]);
    else o.preset = fs::path(v[F_PRESET]);
    o.headGlb = fs::path(v[F_HEAD]);
    o.fo4Dir = fs::path(v[F_FO4]);
    o.cpGameDir = fs::path(v[F_CP]);
    o.wolvenkit = fs::path(v[F_WK]);
    o.cacheDir = g_exeDir / "PresetToolKit_cache";
    o.dataDir = defaultDataDir(g_exeDir);
    o.extendedSculpting = SendMessageW(g_efs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.extendedSculptingRedux = SendMessageW(g_efsRedux, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::string name;
    if (o.cpToFo4) {
        fs::path sp(v[F_CPSAVE]);
        name = sanitizeId(narrow((sp.filename() == L"sav.dat" ? sp.parent_path().filename() : sp.stem()).wstring()));
    } else {
        name = sanitizeId(narrow(o.preset.stem().wstring()));
    }
    const std::string headName = g_xHead == G_CP ? "_cyberpunk_head" : "_fallout4_head";
    o.workDir = workRoot() / fs::path(widen("Export_" + name));
    g_lastWorkDir = o.workDir;
    const fs::path out = fs::path(v[F_MODEL_OUT]) / fs::path(widen(name + headName) + kModelExts[g_xFmt]);
    const std::string head = g_xHead == G_CP ? "cp" : "fo4";

    SetWindowTextW(g_log, L"");
    setBusy(true);
    std::thread([o, head, out]() {
        auto* done = new ExportDoneMsg;
        try {
            done->result = exportFaceModel(o, head, out);
            done->ok = true;
        } catch (const std::exception& e) {
            done->message = widen(e.what());
        }
        PostMessageW(g_wnd, WM_APP_EXPORT_DONE, 0, (LPARAM)done);
    }).detach();
}

void finishExport(ExportDoneMsg* d) {
    setBusy(false);
    if (!d->ok) {
        appendLog(L"error: " + d->message);
        MessageBoxW(g_wnd, (L"Export failed:\n\n" + d->message).c_str(), L"PresetToolKit", MB_ICONERROR);
        return;
    }
    appendLog(L"");
    appendLog(widen(d->result.report));
    MessageBoxW(g_wnd, (L"Model saved:\n\n" + d->result.file.wstring()).c_str(), L"PresetToolKit", MB_ICONINFORMATION);
}

void finishConversion(DoneMsg* d) {
    setBusy(false);
    EnableWindow(g_openWork, TRUE);
    if (!d->ok) {
        appendLog(L"error: " + d->message);
        MessageBoxW(g_wnd, (L"Conversion failed:\n\n" + d->message).c_str(), L"PresetToolKit", MB_ICONERROR);
        return;
    }
    const ConvertResult& r = d->result;
    appendLog(L"");
    appendLog(widen(r.report));
    if (!r.presetFile.empty()) {
        std::wstring msg = L"LooksMenu preset created.";
        if (r.archiveCopied) {
            appendLog(L"Preset: " + r.presetFile.wstring());
            msg += L"\n\nPreset .json:\n" + r.presetFile.wstring();
        }
        if (r.zipCopied) {
            appendLog(L"Zip:    " + r.zip.wstring());
            msg += L"\n\nMod manager .zip (installs to Data\\F4SE\\Plugins\\F4EE\\Presets):\n" + r.zip.wstring();
        }
        msg += L"\n\nIn Fallout 4 open LooksMenu and load it from the preset list.";
        if (!r.saveFile.empty()) {
            appendLog(L"Save:   " + r.saveFile.wstring());
            msg += L"\n\nFallout 4 save with the face (load it from the Load menu):\n" + r.saveFile.wstring();
        }
        MessageBoxW(g_wnd, msg.c_str(), L"PresetToolKit", MB_ICONINFORMATION);
        return;
    }
    switch (r.exitCode) {
    case 0:
    {
        std::wstring msg = L"Head replacement built.";
        if (r.archiveCopied) {
            appendLog(L"Archive: " + r.archive.wstring());
            msg += L"\n\nStandalone .archive:\n" + r.archive.wstring();
        }
        if (r.zipCopied) {
            appendLog(L"Zip:     " + r.zip.wstring());
            msg += L"\n\nMod manager .zip:\n" + r.zip.wstring();
        }
        if (!r.archiveCopied && !r.zipCopied) {
            appendLog(L"Done (work folder): " + r.archive.wstring());
            msg += L"\n\nOutputs are in the work folder:\n" + g_lastWorkDir.wstring();
        }
        MessageBoxW(g_wnd, msg.c_str(), L"PresetToolKit", MB_ICONINFORMATION);
    }
        break;
    case 2:
        MessageBoxW(g_wnd, L"Nothing in the preset could be applied - see the log.", L"PresetToolKit", MB_ICONWARNING);
        break;
    case 3:
        MessageBoxW(g_wnd,
                    (L"The head was sculpted, but the .archive was not built (WolvenKit.CLI or the vanilla files "
                     L"were not set, or WolvenKit failed).\n\nA ready WolvenKit project and MANUAL_STEPS.txt are in:\n" +
                     g_lastWorkDir.wstring()).c_str(),
                    L"PresetToolKit", MB_ICONINFORMATION);
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------ window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        createControls();
        loadSettings();
        updateModeTexts();
        layout();
        appendLog(L"Fallout 4 -> Cyberpunk 2077: V gets your Fallout 4 face as a head replacement mod.");
        appendLog(L"Cyberpunk 2077 -> Fallout 4: V's face from a Cyberpunk save becomes a LooksMenu preset.");
        appendLog(L"");
        appendLog(L"1. Pick a LooksMenu preset (.json) or a Fallout 4 save (.fos) - or, the other way, a Cyberpunk save (sav.dat).");
        appendLog(L"2. Set the Fallout 4 folder, the Cyberpunk 2077 folder and WolvenKit.CLI.exe. Fallout 4's .nif face");
        appendLog(L"   bones and sculpt regions are read from its .ba2 archives (loose/modded files win), and V's head is");
        appendLog(L"   pulled from Cyberpunk's archives by WolvenKit (first run only - it is cached afterwards).");
        appendLog(L"3. Choose the destination (Cyberpunk 2077\\archive\\pc\\mod, or for presets");
        appendLog(L"   Fallout 4\\Data\\F4SE\\Plugins\\F4EE\\Presets) and press Convert.");
        appendLog(L"Without the Fallout 4 folder, built-in data calibrated from the vanilla game is used.");
        return 0;
    case WM_SIZE:
        if (g_log) layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(700);
        mm->ptMinTrackSize.y = S(850);
        return 0;
    }
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) {
            g_page = TabCtrl_GetCurSel(g_tab);
            layout();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if ((id == ID_X_SRC || id == ID_X_HEAD || id == ID_X_FMT) && HIWORD(wp) == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (id == ID_X_SRC) g_xSrc = sel;
            else if (id == ID_X_HEAD) g_xHead = sel;
            else g_xFmt = sel;
            layout();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        if ((id == ID_EFS || id == ID_EFS_REDUX) && HIWORD(wp) == BN_CLICKED) {
            // the two mods replace the same files: one or the other
            HWND other = id == ID_EFS ? g_efsRedux : g_efs;
            if (SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED) SendMessageW(other, BM_SETCHECK, BST_UNCHECKED, 0);
            return 0;
        }
        if (id == ID_X_EXPORT) {
            startExport();
            return 0;
        }
        if (id == ID_X_OPEN) {
            const std::wstring d = getText(g_edit[F_MODEL_OUT]);
            if (d.empty()) MessageBoxW(hwnd, L"No output folder selected yet.", L"PresetToolKit", MB_ICONINFORMATION);
            else openFolder(fs::path(d));
            return 0;
        }
        if ((id == ID_FROM || id == ID_TO) && HIWORD(wp) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (id == ID_FROM) setDirection(sel, g_toG);
            else setDirection(g_fromG, sel);
            return 0;
        }
        if (id >= ID_BROWSE_BASE && id < ID_BROWSE_BASE + F_COUNT) {
            int i = id - ID_BROWSE_BASE;
            FieldDef fd = kFields[i];
            std::wstring p = pickPath(hwnd, fd, getText(g_edit[i]));
            if (!p.empty()) {
                SetWindowTextW(g_edit[i], p.c_str());
                if (i == F_HEAD) autoFillFromProject(p);
                if (i == F_CPSAVE && getText(g_name).empty()) {
                    fs::path sp(p);
                    fs::path folder = sp.filename() == L"sav.dat" ? sp.parent_path().filename() : sp.stem();
                    SetWindowTextW(g_name, widen("CP_" + sanitizeId(narrow(folder.wstring()))).c_str());
                }
                if (i == F_PRESET && getText(g_name).empty()) {
                    std::string n = "LM_" + sanitizeId(narrow(fs::path(p).stem().wstring())) + "_Head";
                    SetWindowTextW(g_name, widen(n).c_str());
                }
            }
        } else if (id == ID_CONVERT) {
            startConversion();
        } else if (id == ID_OPEN_OUT) {
            std::wstring d = getText(g_edit[outField()]);
            if (d.empty()) MessageBoxW(hwnd, L"No destination folder selected yet.", L"PresetToolKit", MB_ICONINFORMATION);
            else openFolder(fs::path(d));
        } else if (id == ID_OPEN_WORK) {
            openFolder(g_lastWorkDir);
        }
        return 0;
    }
    case WM_APP_LOG: {
        auto* s = (std::wstring*)lp;
        appendLog(*s);
        delete s;
        return 0;
    }
    case WM_APP_EXPORT_DONE: {
        auto* d = (ExportDoneMsg*)lp;
        finishExport(d);
        delete d;
        return 0;
    }
    case WM_APP_DONE: {
        auto* d = (DoneMsg*)lp;
        finishConversion(d);
        delete d;
        return 0;
    }
    case WM_CLOSE:
        if (g_busy && MessageBoxW(hwnd, L"A conversion is still running. Quit anyway?", L"PresetToolKit",
                                  MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        saveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    wchar_t exe[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    g_exeDir = fs::path(exe).parent_path();
    g_ini = g_exeDir / L"PresetToolKit.ini";
    {
        // carry over the settings and head cache of the program's old name (lm2cp)
        std::error_code ec;
        const fs::path oldIni = g_exeDir / L"lm2cp_gui.ini";
        if (!fs::exists(g_ini, ec) && fs::exists(oldIni, ec)) {
            std::string text = readFileText(oldIni);
            text = replaceAll(text, "[lm2cp]", "[PresetToolKit]");
            writeFileText(g_ini, text);
        }
        const fs::path oldCache = g_exeDir / L"lm2cp_cache", newCache = g_exeDir / L"PresetToolKit_cache";
        if (!fs::exists(newCache, ec) && fs::exists(oldCache, ec)) fs::rename(oldCache, newCache, ec);
    }

    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, dc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    // Core log messages go to the log panel (from any thread)
    Log::sink = [](int level, const std::string& s) { postLog(level, s); };

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PresetToolKit";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"PresetToolKit", L"PresetToolKit - Fallout 4 / Cyberpunk 2077 faces",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(860), S(1010), nullptr, nullptr,
                                inst, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    Log::sink = nullptr;
    DeleteObject(g_font);
    CoUninitialize();
    return 0;
}
