// SPDX-License-Identifier: MIT
// Sonic Beam setup (native mode): finds and checks the BeamNG.drive user folder and the Sonic Frontiers folder.
// Only reads (files, the registry, the process list). Every file read is capped: a missing, huge or garbled
// file only means "not found there".
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>   // after windows.h
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <vector>

namespace setup {

constexpr const wchar_t *kInstallFolderName = L"SonicBeamNative";      // %LOCALAPPDATA%\<this>\setup.ini
constexpr const wchar_t *kModFolder        = L"sonicnative";          // <user>\current\mods\unpacked\<this>
constexpr const wchar_t *kDataFolder        = L"sonicnative_data";     // <user>\current\mods\<this> (the converter's)
constexpr const wchar_t *kSettingsFile      = L"sonicnative.json";     // <user>\current\settings\<this> (the mod's Lab settings)
constexpr const wchar_t *kBeamngAppId       = L"284160";
constexpr const wchar_t *kFrontiersAppId    = L"1237320";
// the three files the converter reads, relative to the Frontiers folder
constexpr const wchar_t *kFrontiersFiles[3] = {L"image\\x64\\raw\\character\\sonic.pac", L"image\\x64\\raw\\EffectCommon.pac",
                                               L"image\\x64\\raw\\sound\\sonic_sound\\se_rangers_player_sonic.acb"};

struct Found { std::wstring path; std::wstring source; };   // source: short English text for the window
// setup.ini: the BeamNG.drive user folder and the Sonic Frontiers folder of the last install
struct SetupIni { std::wstring userpath, frontiers; bool present = false; };

inline std::wstring clean_folder(std::wstring dir);
inline std::wstring steam_path();

namespace detail {

constexpr size_t kMaxRead = size_t(8) << 20;   // Steam's localconfig.vdf is the biggest file read here (usually < 1 MB)
constexpr int kMaxDepth = 32;                  // folder and VDF nesting: deeper is garbage
constexpr size_t kMaxVdfNodes = size_t(1) << 19;
constexpr uint64_t kSteamId64Base = 76561197960265728ull;   // SteamID64 of account 0 (individual, public universe)

inline std::string utf8(const std::wstring &w)
{
    if (w.empty() || w.size() > 0x7fffffff) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// Invalid UTF-8 becomes U+FFFD: a garbled file gives a path that does not exist, never a crash.
inline std::wstring wide(const std::string &s)
{
    if (s.empty() || s.size() > 0x7fffffff) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

inline char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
inline wchar_t lower_ascii(wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; }
inline std::wstring lower(std::wstring s) { for (auto &c : s) c = lower_ascii(c); return s; }

inline bool iequal(const std::string &a, const char *b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return i == a.size() && !b[i];
}

// Windows' own case-insensitive comparison of names (ordinal, like the file system).
inline bool iequal(const std::wstring &a, const std::wstring &b)
{
    if (a.size() != b.size() || a.size() > 0x7fffffff) return false;
    return a.empty() || CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}

inline bool contains_ci(const std::string &text, const char *needle)
{
    std::string a = text, b = needle;
    for (auto &c : a) c = lower_ascii(c);
    for (auto &c : b) c = lower_ascii(c);
    return a.find(b) != std::string::npos;
}

inline std::string trim(const std::string &s)
{
    const char *ws = " \t\r\n\f\v";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// "C:\..." or "\\server\share\..." (also "\\?\..."): never a path that depends on the current directory.
inline bool is_absolute(const std::wstring &p)
{
    if (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\') return true;
    return p.size() >= 3 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'\\';
}

inline std::wstring join(const std::wstring &dir, const std::wstring &name)
{
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

inline std::wstring name_of(const std::wstring &p)
{
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

// Long paths need the \\?\ form for the W functions (the manifest does not opt into long paths).
// Only for absolute, already normalized paths: \\?\ turns off "..", "." and '/' handling.
inline std::wstring lp(const std::wstring &p)
{
    if (p.size() < 240 || p.compare(0, 4, L"\\\\?\\") == 0 || !is_absolute(p)) return p;
    if (p[0] == L'\\') return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

inline DWORD attributes(const std::wstring &p) { return p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(lp(p).c_str()); }
inline bool is_dir(const std::wstring &p) { const DWORD a = attributes(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
inline bool is_file(const std::wstring &p) { const DWORD a = attributes(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

// Resolves ".", ".." and doubled separators; a relative path is resolved against the current directory,
// so callers check is_absolute first.
inline std::wstring full_path(const std::wstring &p)
{
    if (p.empty()) return p;
    const DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n || n > 32768) return p;
    std::wstring out(n, L'\0');
    const DWORD got = GetFullPathNameW(p.c_str(), n, &out[0], nullptr);
    if (!got || got >= n) return p;
    out.resize(got);
    return out;
}

// A whole small file (no device, no pipe, nothing above cap).
inline bool read_capped(const std::wstring &path, std::string &out, size_t cap = kMaxRead)
{
    out.clear();
    if (path.empty()) return false;
    HANDLE f = CreateFileW(lp(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileType(f) == FILE_TYPE_DISK && GetFileSizeEx(f, &size) && size.QuadPart >= 0 && uint64_t(size.QuadPart) <= cap;
    if (ok) {
        out.resize(size_t(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD got = 0;
            const DWORD want = DWORD((std::min)(out.size() - done, size_t(1) << 20));
            ok = ReadFile(f, &out[done], want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}

// Text lines without the UTF-8 BOM and the '\r' of CRLF.
inline std::vector<std::string> lines(const std::string &text)
{
    std::vector<std::string> out;
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        at = end + 1;
    }
    return out;
}

// The value of "key = value" (key case-insensitive, spaces around '=' allowed), in [section] when one is
// given (any section otherwise); ';' and '#' start comment lines. Empty when missing.
inline std::string ini_value(const std::string &text, const char *key, const char *section = nullptr)
{
    std::string current;
    for (const std::string &raw : lines(text)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            current = trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            continue;
        }
        if (section && !iequal(current, section)) continue;
        const size_t eq = line.find('=');
        if (eq != std::string::npos && iequal(trim(line.substr(0, eq)), key)) return trim(line.substr(eq + 1));
    }
    return {};
}

inline bool parse_u64(const std::string &s, uint64_t &v)
{
    v = 0;
    if (s.empty() || s.size() > 20) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = uint64_t(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    return true;
}

// Valve KeyValues text (loginusers.vdf, localconfig.vdf, libraryfolders.vdf, appmanifest_*.acf): quoted or bare
// keys and values, nested {}, \\ \" \n \t escapes, // comments, [$CONDITION] tags skipped.
struct Vdf {
    std::string key, value;   // UTF-8, escapes undone
    bool block = false;
    std::vector<Vdf> children;
    const Vdf *get(const char *name) const   // first child with this key (case-insensitive)
    {
        for (const Vdf &c : children) if (iequal(c.key, name)) return &c;
        return nullptr;
    }
    std::string str(const char *name) const   // its value, empty when missing or a block
    {
        const Vdf *c = get(name);
        return c && !c->block ? c->value : std::string();
    }
};

// Never loops: every token takes at least one byte. Garbage only gives odd keys; nesting and size are capped.
inline void parse_vdf(const std::string &s, Vdf &root)
{
    root = Vdf();
    root.block = true;
    enum Tok { End, Open, Close, Text };
    size_t pos = s.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::string text;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == '\0'; };
    auto next = [&]() -> Tok {
        for (;;) {
            text.clear();
            while (pos < s.size()) {
                if (space(s[pos])) ++pos;
                else if (s[pos] == '/' && pos + 1 < s.size() && s[pos + 1] == '/') {
                    const size_t nl = s.find('\n', pos);
                    pos = nl == std::string::npos ? s.size() : nl + 1;
                } else break;
            }
            if (pos >= s.size()) return End;
            const char c = s[pos];
            if (c == '{') { ++pos; return Open; }
            if (c == '}') { ++pos; return Close; }
            if (c == '"') {
                ++pos;
                while (pos < s.size() && s[pos] != '"') {
                    if (s[pos] == '\\' && pos + 1 < s.size()) {
                        const char e = s[pos + 1];
                        if (e == '\\' || e == '"') { text += e; pos += 2; continue; }
                        if (e == 'n') { text += '\n'; pos += 2; continue; }
                        if (e == 't') { text += '\t'; pos += 2; continue; }
                    }
                    text += s[pos++];
                }
                if (pos < s.size()) ++pos;   // the closing quote (an unclosed one runs to the end)
                return Text;
            }
            const size_t start = pos;
            while (pos < s.size() && !space(s[pos]) && s[pos] != '{' && s[pos] != '}' && s[pos] != '"') ++pos;
            text.assign(s, start, pos - start);
            if (text.size() >= 2 && text.front() == '[' && text.back() == ']') continue;   // [$WIN32] and the like
            return Text;
        }
    };
    // Only the top of the stack gets children, and every pointer below it is one of its ancestors,
    // so no push_back moves a node the stack points to.
    std::vector<Vdf *> stack{&root};
    std::string key;
    bool have_key = false;
    size_t nodes = 0;
    for (Tok t = next(); t != End && nodes < kMaxVdfNodes; t = next()) {
        Vdf &cur = *stack.back();
        if (t == Text) {
            if (!have_key) { key = text; have_key = true; continue; }
            Vdf n;
            n.key = std::move(key);
            n.value = text;
            cur.children.push_back(std::move(n));
            ++nodes;
            have_key = false;
        } else if (t == Open) {
            if (stack.size() > size_t(kMaxDepth)) {   // too deep: skip this block whole
                for (int depth = 1; depth > 0;) {
                    const Tok k = next();
                    if (k == End) break;
                    depth += k == Open ? 1 : k == Close ? -1 : 0;
                }
                have_key = false;
                continue;
            }
            Vdf n;
            if (have_key) n.key = std::move(key);
            n.block = true;
            cur.children.push_back(std::move(n));
            ++nodes;
            stack.push_back(&cur.children.back());
            have_key = false;
        } else {
            have_key = false;   // a key with no value is dropped
            if (stack.size() > 1) stack.pop_back();
        }
    }
}

inline bool read_vdf(const std::wstring &path, Vdf &root)
{
    std::string text;
    root = Vdf();
    if (!read_capped(path, text)) return false;
    parse_vdf(text, root);
    return true;
}

inline const Vdf *vdf_at(const Vdf *node, std::initializer_list<const char *> keys)
{
    for (const char *k : keys) {
        if (!node) return nullptr;
        node = node->get(k);
    }
    return node;
}

inline void append_utf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

inline bool hex4(const std::string &s, size_t at, uint32_t &v)
{
    v = 0;
    if (at + 4 > s.size()) return false;
    for (size_t i = at; i < at + 4; ++i) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + uint32_t(d);
    }
    return true;
}

// The string value of the first "key": "..." pair of a JSON text (at any depth), escapes undone. Not a JSON
// parser: enough for Epic's manifests, and strings are skipped whole so a value never passes for a key.
inline bool json_string(const std::string &s, const char *key, std::string &out)
{
    size_t pos = 0;
    auto read = [&](std::string &dst) -> bool {   // at '"': one string; false when unterminated
        dst.clear();
        ++pos;
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') return true;
            if (c != '\\') { dst += c; continue; }
            if (pos >= s.size()) return false;
            const char e = s[pos++];
            uint32_t cp = 0, low = 0;
            switch (e) {
            case 'b': dst += '\b'; break;
            case 'f': dst += '\f'; break;
            case 'n': dst += '\n'; break;
            case 'r': dst += '\r'; break;
            case 't': dst += '\t'; break;
            case 'u':
                if (!hex4(s, pos, cp)) { dst += 'u'; break; }
                pos += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && s.compare(pos, 2, "\\u") == 0 && hex4(s, pos + 2, low) && low >= 0xDC00 && low < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    pos += 6;
                }
                if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                append_utf8(dst, cp);
                break;
            default: dst += e;   // \" \\ \/ (and anything else as itself)
            }
        }
        return false;
    };
    std::string tok;
    while (pos < s.size()) {
        if (s[pos] != '"') { ++pos; continue; }
        if (!read(tok)) return false;
        size_t p = pos;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
        if (p >= s.size() || s[p] != ':' || tok != key) continue;
        ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
        if (p >= s.size() || s[p] != '"') return false;
        pos = p;
        return read(out);
    }
    return false;
}

inline std::wstring reg_string(HKEY root, const wchar_t *subkey, const wchar_t *name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    std::wstring out;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
        size > 0 && size <= 65536) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
        DWORD got = DWORD(buf.size() * sizeof(wchar_t));
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(buf.data()), &got) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
            out.assign(buf.data(), wcsnlen(buf.data(), (std::min)(buf.size(), size_t(got / sizeof(wchar_t)))));
    }
    RegCloseKey(key);
    return out;
}

inline DWORD reg_dword(HKEY root, const wchar_t *subkey, const wchar_t *name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return 0;
    DWORD type = 0, value = 0, size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&value), &size) != ERROR_SUCCESS || type != REG_DWORD) value = 0;
    RegCloseKey(key);
    return value;
}

} // namespace detail

inline std::wstring env_folder(const wchar_t *name)
{
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n || n > 32768) return {};
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, &v[0], n);
    if (!got || got >= n) return {};
    v.resize(got);
    while (!v.empty() && v.back() == L'\\' && !(v.size() == 3 && v[1] == L':')) v.pop_back();
    return v;
}

// %LOCALAPPDATA%\SonicBeamNative, setup.ini's folder. Empty when %LOCALAPPDATA% is unset or relative: nothing is
// ever written or deleted relative to the current directory.
inline std::wstring install_dir()
{
    const std::wstring local = env_folder(L"LOCALAPPDATA");
    return detail::is_absolute(local) ? detail::join(local, kInstallFolderName) : std::wstring();
}

inline std::wstring setup_ini_path()
{
    const std::wstring dir = install_dir();
    return dir.empty() ? dir : dir + L"\\setup.ini";
}

inline std::wstring clean_folder(std::wstring dir)
{
    for (auto &c : dir) if (c == L'/') c = L'\\';
    auto junk = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"'; };
    size_t a = 0;
    while (a < dir.size() && junk(dir[a])) ++a;
    dir.erase(0, a);
    while (!dir.empty()) {
        if (junk(dir.back())) dir.pop_back();
        else if (dir.back() == L'\\' && !(dir.size() == 3 && dir[1] == L':')) dir.pop_back();
        else break;
    }
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    return dir;
}

// setup.ini (UTF-8): userpath= and frontiers=, written by save_setup_ini() after each successful install.
inline SetupIni read_setup_ini()
{
    SetupIni ini;
    std::string text;
    const std::wstring path = setup_ini_path();
    if (path.empty() || !detail::read_capped(path, text, 64 * 1024)) return ini;
    ini.present = true;
    ini.userpath = clean_folder(detail::wide(detail::ini_value(text, "userpath")));
    ini.frontiers = clean_folder(detail::wide(detail::ini_value(text, "frontiers")));
    return ini;
}

// BeamNG runs in <base>\current: a picked "current" folder means its parent. A base folder that happens
// to be named "current" (it holds its own "current") stays as it is.
inline std::wstring user_base(std::wstring picked)
{
    picked = clean_folder(std::move(picked));
    const size_t slash = picked.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= picked.size()) return picked;
    if (!detail::iequal(picked.substr(slash + 1), std::wstring(L"current"))) return picked;
    if (!detail::is_dir(picked) || detail::is_dir(picked + L"\\current")) return picked;
    std::wstring parent = picked.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    return parent.empty() ? picked : parent;
}

inline bool check_user_folder(const std::wstring &base, std::wstring &why)
{
    why.clear();
    if (base.empty()) { why = L"Choose the BeamNG.drive user folder."; return false; }
    if (!detail::is_absolute(base)) { why = L"Use a full path, starting with a drive letter (C:\\...)."; return false; }
    if (!detail::is_dir(base)) { why = L"This folder does not exist."; return false; }
    if (detail::is_dir(detail::join(base, L"current"))) return true;
    if (detail::is_file(detail::join(base, L"BeamNG.drive.exe")) || detail::is_dir(detail::join(base, L"Bin64")))
        why = L"This is the BeamNG.drive game folder, not its user folder (by default %LOCALAPPDATA%\\BeamNG\\BeamNG.drive).";
    else if (detail::iequal(detail::name_of(base), std::wstring(L"current")))
        why = L"Choose the folder that holds this 'current' folder.";
    else
        why = L"Not a BeamNG.drive user folder: it has no 'current' folder. Start BeamNG.drive once, or choose the folder that holds 'current'.";
    return false;
}

inline bool check_frontiers(const std::wstring &dir, std::wstring &why)
{
    why.clear();
    if (dir.empty()) { why = L"Choose the Sonic Frontiers folder (the one that holds SonicFrontiers.exe)."; return false; }
    if (!detail::is_absolute(dir)) { why = L"Use a full path, starting with a drive letter (C:\\...)."; return false; }
    if (!detail::is_dir(dir)) { why = L"This folder does not exist."; return false; }
    for (const wchar_t *file : kFrontiersFiles) {
        if (detail::is_file(detail::join(dir, file))) continue;
        if (detail::is_file(detail::join(dir, L"SonicFrontiers.exe")))
            why = std::wstring(L"Sonic Frontiers is incomplete here: ") + file + L" is missing (verify the game's files in its launcher).";
        else
            why = std::wstring(L"Not the Sonic Frontiers folder (") + file + L" is missing): choose the folder that holds SonicFrontiers.exe.";
        return false;
    }
    return true;
}

inline std::wstring steam_path()
{
    std::wstring p = clean_folder(detail::reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    if (detail::is_absolute(p) && detail::is_dir(p)) return p;
    p = clean_folder(detail::reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    return detail::is_absolute(p) && detail::is_dir(p) ? p : std::wstring();
}

namespace detail {

// The -userpath value of a launch options line: quoted up to the closing quote, else up to the next " -".
inline std::wstring userpath_option(const std::wstring &opts)
{
    const std::wstring flag = L"-userpath";
    const std::wstring low = lower(opts);
    for (size_t at = low.find(flag); at != std::wstring::npos; at = low.find(flag, at + 1)) {
        if (at > 0 && opts[at - 1] != L' ' && opts[at - 1] != L'\t') continue;   // inside another word
        size_t p = at + flag.size();
        if (p < opts.size() && opts[p] != L' ' && opts[p] != L'\t' && opts[p] != L'"') continue;   // -userpathfoo
        while (p < opts.size() && (opts[p] == L' ' || opts[p] == L'\t')) ++p;
        std::wstring value;
        if (p < opts.size() && opts[p] == L'"') {
            const size_t close = opts.find(L'"', p + 1);
            value = opts.substr(p + 1, close == std::wstring::npos ? std::wstring::npos : close - p - 1);
        } else if (p < opts.size()) {
            const size_t end = (std::min)(opts.find(L" -", p), opts.find(L"\t-", p));
            value = opts.substr(p, end == std::wstring::npos ? std::wstring::npos : end - p);
        }
        value = clean_folder(value);
        if (!value.empty()) return value;
    }
    return {};
}

// BeamNG's Steam launch options (-userpath) of the Steam account in use: the one Steam runs now, else
// loginusers.vdf's MostRecent, else its newest "timestamp" (recent Steam writes no MostRecent).
inline std::wstring steam_userpath()
{
    const std::wstring steam = steam_path();
    if (steam.empty()) return {};
    const uint64_t active = reg_dword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser");   // 0 when Steam is closed
    uint64_t account = 0;
    Vdf users;
    if (read_vdf(steam + L"\\config\\loginusers.vdf", users)) {
        int best = -1;
        uint64_t best_time = 0;
        if (const Vdf *list = users.get("users"))
            for (const Vdf &u : list->children) {
                uint64_t id = 0, time = 0;
                if (!u.block || !parse_u64(u.key, id) || id <= kSteamId64Base || id - kSteamId64Base > 0xffffffffull) continue;
                parse_u64(u.str("Timestamp"), time);
                const uint64_t acc = id - kSteamId64Base;
                const int rank = (active && acc == active) ? 2 : (u.str("MostRecent") == "1" ? 1 : 0);
                if (rank > best || (rank == best && time > best_time)) { best = rank; best_time = time; account = acc; }
            }
    }
    if (!account) account = active;
    if (!account) return {};
    Vdf config;
    if (!read_vdf(steam + L"\\userdata\\" + std::to_wstring(account) + L"\\config\\localconfig.vdf", config)) return {};
    const std::string app = utf8(kBeamngAppId);
    const Vdf *node = vdf_at(&config, {"UserLocalConfigStore", "Software", "Valve", "Steam", "apps", app.c_str()});
    return node && node->block ? userpath_option(wide(node->str("LaunchOptions"))) : std::wstring();
}

// Every Steam library: Steam's own folder, then libraryfolders.vdf's (both the current and the old layout).
inline std::vector<std::wstring> steam_libraries()
{
    std::vector<std::wstring> libs;
    const std::wstring steam = steam_path();
    if (steam.empty()) return libs;
    auto add = [&](const std::wstring &raw) {
        const std::wstring dir = clean_folder(raw);
        if (!is_absolute(dir) || libs.size() >= 64) return;
        for (const std::wstring &l : libs) if (iequal(l, dir)) return;
        libs.push_back(dir);
    };
    add(steam);
    for (const wchar_t *file : {L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf"}) {
        Vdf root;
        if (!read_vdf(join(steam, file), root)) continue;
        const Vdf *list = root.get("libraryfolders");
        if (!list) continue;
        uint64_t number = 0;
        for (const Vdf &c : list->children) {
            if (c.block) add(wide(c.str("path")));
            else if (parse_u64(c.key, number)) add(wide(c.value));   // old layout: "1" "D:\\SteamLibrary"
        }
    }
    return libs;
}

// <library>\steamapps\common\<installdir> of every library whose appmanifest_<appid>.acf names an existing folder.
inline std::vector<std::wstring> steam_app_dirs(const wchar_t *appid)
{
    std::vector<std::wstring> dirs;
    for (const std::wstring &lib : steam_libraries()) {
        Vdf acf;
        if (!read_vdf(join(lib, std::wstring(L"steamapps\\appmanifest_") + appid + L".acf"), acf)) continue;
        const Vdf *state = acf.get("AppState");
        const std::wstring name = clean_folder(wide(state ? state->str("installdir") : std::string()));
        // one folder name under steamapps\common, nothing that climbs out of it
        if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\:") != std::wstring::npos) continue;
        const std::wstring dir = join(lib, L"steamapps\\common\\" + name);
        if (is_dir(dir)) dirs.push_back(dir);
    }
    return dirs;
}

inline std::wstring default_user()
{
    std::wstring local = env_folder(L"LOCALAPPDATA");
    if (local.empty()) {
        const std::wstring home = env_folder(L"USERPROFILE");
        if (!home.empty()) local = join(home, L"AppData\\Local");
    }
    return local.empty() ? std::wstring(L"%LOCALAPPDATA%\\BeamNG\\BeamNG.drive") : join(local, L"BeamNG\\BeamNG.drive");
}

} // namespace detail

// The launcher's installPath, else the Steam library that holds BeamNG.drive.
inline std::wstring beamng_install_dir()
{
    const std::wstring local = env_folder(L"LOCALAPPDATA");
    std::string ini;
    if (!local.empty() && detail::read_capped(detail::join(local, L"BeamNG\\BeamNG.drive.ini"), ini)) {
        const std::wstring dir = clean_folder(detail::wide(detail::ini_value(ini, "installPath")));
        if (detail::is_absolute(dir) && detail::is_dir(dir)) return dir;
    }
    const std::vector<std::wstring> dirs = detail::steam_app_dirs(kBeamngAppId);
    return dirs.empty() ? std::wstring() : dirs.front();
}

inline Found find_beamng_user()
{
    std::wstring why;
    auto ok = [&](const std::wstring &dir) { return !dir.empty() && check_user_folder(dir, why); };
    const SetupIni ini = read_setup_ini();
    if (ok(ini.userpath)) return {ini.userpath, L"previous install"};
    // The value of -userpath is the base: BeamNG runs in <value>\current
    const std::wstring steam = detail::steam_userpath();
    if (ok(steam)) return {steam, L"Steam launch options (-userpath)"};
    const std::wstring local = env_folder(L"LOCALAPPDATA");
    std::string text;
    if (!local.empty() && detail::read_capped(detail::join(local, L"BeamNG\\BeamNG.drive.ini"), text)) {
        const std::wstring dir = user_base(detail::wide(detail::ini_value(text, "userFolder")));
        if (ok(dir)) return {dir, L"BeamNG launcher settings"};
    }
    // startup.ini's UserPath: empty means unset; a relative one is relative to the folder holding startup.ini
    // (its own comments also show one relative to %LOCALAPPDATA%\BeamNG: that spelling is tried second)
    const std::wstring install = beamng_install_dir();
    if (!install.empty() && detail::read_capped(detail::join(install, L"startup.ini"), text)) {
        const std::wstring value = clean_folder(detail::wide(detail::ini_value(text, "UserPath", "filesystem")));
        std::vector<std::wstring> dirs;
        if (detail::is_absolute(value)) dirs.push_back(value);
        else if (!value.empty()) {
            dirs.push_back(clean_folder(detail::full_path(detail::join(install, value))));
            if (detail::is_absolute(local)) dirs.push_back(clean_folder(detail::full_path(detail::join(local, L"BeamNG\\" + value))));
        }
        for (const std::wstring &dir : dirs) if (ok(dir)) return {dir, L"BeamNG startup.ini"};
    }
    return {detail::default_user(), L"default location"};
}

inline Found find_frontiers()
{
    std::wstring why;
    const SetupIni ini = read_setup_ini();
    if (!ini.frontiers.empty() && check_frontiers(ini.frontiers, why)) return {ini.frontiers, L"previous install"};
    for (const std::wstring &dir : detail::steam_app_dirs(kFrontiersAppId))
        if (check_frontiers(dir, why)) return {dir, L"Steam library"};
    // Epic: one JSON manifest per installed game
    const std::wstring data = env_folder(L"ProgramData");
    if (!data.empty()) {
        const std::wstring manifests = detail::join(data, L"Epic\\EpicGamesLauncher\\Data\\Manifests");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((manifests + L"\\*.item").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            Found found;
            int seen = 0;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string text, name, location;
                if (!detail::read_capped(manifests + L"\\" + fd.cFileName, text, size_t(1) << 20)) continue;
                if (!detail::json_string(text, "DisplayName", name) || !detail::contains_ci(name, "sonic frontiers")) continue;
                if (!detail::json_string(text, "InstallLocation", location)) continue;
                const std::wstring dir = clean_folder(detail::wide(location));
                if (check_frontiers(dir, why)) { found = {dir, L"Epic Games launcher"}; break; }
            } while (++seen < 256 && FindNextFileW(h, &fd));
            FindClose(h);
            if (!found.path.empty()) return found;
        }
    }
    return {};
}

// The overlay mode (beamng/) in this user folder or in BeamNG's Bin64. Only looks; never touches its files.
inline bool overlay_active(const std::wstring &user, std::wstring &what)
{
    what.clear();
    if (detail::is_absolute(user)) {
        const std::wstring mods = detail::join(user, L"current\\mods");
        if (detail::is_dir(mods + L"\\unpacked\\sonicfrontiers_sonic")) {
            what = L"The Sonic Beam overlay mode is installed in this user folder (mods\\unpacked\\sonicfrontiers_sonic).";
            return true;
        }
        if (detail::is_file(mods + L"\\sonicfrontiers_sonic.zip")) {
            what = L"The Sonic Beam overlay mode is installed in this user folder (mods\\sonicfrontiers_sonic.zip).";
            return true;
        }
    }
    const std::wstring install = beamng_install_dir();
    if (!install.empty() && detail::is_file(detail::join(install, L"Bin64\\version.dll")) && detail::is_dir(detail::join(install, L"Bin64\\SonicBeam"))) {
        what = L"The Sonic Beam overlay mode is active in BeamNG.drive (Bin64\\version.dll).";
        return true;
    }
    return false;
}

inline bool beamng_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsicmp(pe.szExeFile, L"BeamNG.drive.x64.exe") || !_wcsicmp(pe.szExeFile, L"BeamNG.drive.arm64.exe");
    CloseHandle(snap);
    return found;
}

} // namespace setup
