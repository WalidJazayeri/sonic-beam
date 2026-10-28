// SPDX-License-Identifier: MIT
// Sonic Beam setup (native mode): installs the mod files this exe carries, runs the converter in-process,
// remembers the folders in setup.ini and removes all of it again. Uninstall deletes only a short list of exact
// paths, inside a folder that passed check_user_folder; a link (junction) is removed as a link, never followed,
// and the mod is never written through one.
#pragma once
#include "detect.hpp"
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <set>

// natif/converter/main.cpp, linked into this exe (built with -DSONICNATIVE_NO_WMAIN)
extern int sonicnative_convert_main(int argc, wchar_t **argv);

namespace setup {

struct ModFile { const char *path; const unsigned char *data; size_t size; };   // path: forward slashes, relative to natif/mod
#include "mod_payload.inc"   // kModFiles[], kModFileCount (natif/build-setup.ps1 writes it from natif/mod)

// One whole line per write: the worker and the window may both log.
inline void log(const char *fmt, ...)
{
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string line;
    if (n > 0) {
        line.resize(size_t(n) + 1);
        std::vsnprintf(&line[0], line.size(), fmt, ap);
        line.resize(size_t(n));
    }
    va_end(ap);
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

inline void logw(const std::wstring &line)
{
    const std::string s = detail::utf8(line) + "\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

namespace detail {

inline std::wstring win_error(DWORD code)
{
    std::wstring s = L"Windows error " + std::to_wstring(code);
    wchar_t *msg = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (!n) n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (n && msg) {
        std::wstring text(msg, n);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
        if (!text.empty()) s += L": " + text;
    }
    if (msg) LocalFree(msg);
    return s;
}

// Same folder: both full paths, compared case-insensitively.
inline bool same_folder(const std::wstring &a, const std::wstring &b)
{
    const std::wstring x = clean_folder(a), y = clean_folder(b);
    return is_absolute(x) && is_absolute(y) && iequal(clean_folder(full_path(x)), clean_folder(full_path(y)));
}

// A junction or a symbolic link (a "name surrogate"): it stands for another folder or file. Cloud placeholders
// (OneDrive) are reparse points too, but they are the real thing.
inline bool is_link(DWORD attr, DWORD tag) { return (attr & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag); }

inline bool entry_info(const std::wstring &path, DWORD &attr, DWORD &tag, DWORD &code)
{
    attr = tag = code = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(path).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { code = GetLastError(); return false; }
    FindClose(h);
    attr = fd.dwFileAttributes;
    tag = (attr & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
    return true;
}

inline bool list_dir(const std::wstring &dir, std::vector<WIN32_FIND_DATAW> &out)
{
    out.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
        out.push_back(fd);
    } while (out.size() < 100000 && FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// Creates every missing folder of an absolute path.
inline bool make_dirs(const std::wstring &path, std::wstring &error)
{
    if (is_dir(path)) return true;
    if (!is_absolute(path)) { error = L"not a full path: " + path; return false; }
    for (size_t at = path.find(L'\\', 3);; at = path.find(L'\\', at + 1)) {
        const std::wstring part = at == std::wstring::npos ? path : path.substr(0, at);
        CreateDirectoryW(lp(part).c_str(), nullptr);   // existing folders and share roots fail here: the check below decides
        if (at == std::wstring::npos) break;
    }
    if (is_dir(path)) return true;
    error = L"cannot create " + path + L" (" + win_error(GetLastError()) + L")";
    return false;
}

inline bool write_all(const std::wstring &path, const void *data, size_t size, std::wstring &error)
{
    const std::wstring p = lp(path);
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {   // a read-only copy from an old install
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            SetFileAttributesW(p.c_str(), a & ~DWORD(FILE_ATTRIBUTE_READONLY));
            f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (f == INVALID_HANDLE_VALUE) { error = L"cannot write " + path + L" (" + win_error(GetLastError()) + L")"; return false; }
    const char *bytes = static_cast<const char *>(data);
    size_t done = 0;
    bool ok = true;
    while (ok && done < size) {
        DWORD wrote = 0;
        const DWORD want = DWORD((std::min)(size - done, size_t(1) << 20));
        ok = WriteFile(f, bytes + done, want, &wrote, nullptr) && wrote == want;
        done += wrote;
    }
    const DWORD code = ok ? 0 : GetLastError();
    ok = CloseHandle(f) && ok;
    if (!ok) error = L"cannot write " + path + L" (" + win_error(code ? code : GetLastError()) + L")";
    return ok;
}
inline bool write_all(const std::wstring &path, const std::string &text, std::wstring &error) { return write_all(path, text.data(), text.size(), error); }

inline bool file_equals(const std::wstring &path, const unsigned char *data, size_t size)
{
    std::string have;
    return read_capped(path, have, size) && have.size() == size && (size == 0 || std::memcmp(have.data(), data, size) == 0);
}

// A read-only file cannot be deleted. Not for links: the attribute change could reach what they point to.
inline void clear_readonly(const std::wstring &path)
{
    const std::wstring p = lp(path);
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_READONLY) || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
    const DWORD rest = a & ~DWORD(FILE_ATTRIBUTE_READONLY);
    SetFileAttributesW(p.c_str(), rest ? rest : FILE_ATTRIBUTE_NORMAL);
}

// One delete, retried a moment: an indexer or an antivirus may hold the file briefly.
inline bool delete_one(const std::wstring &path, bool dir)
{
    const std::wstring p = lp(path);
    if (!dir) clear_readonly(path);
    for (int attempt = 0;; ++attempt) {
        if (dir ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str())) return true;
        const DWORD e = GetLastError();
        if (attempt >= 4 || (e != ERROR_DIR_NOT_EMPTY && e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED)) { SetLastError(e); return false; }
        Sleep(100);
    }
}

// Deletes a file or a folder tree. A link inside (or the path itself) goes as a link: what it points to is never
// touched. Not std::filesystem::remove_all, which may walk into a junction (libc++ only treats symlinks as links).
inline bool remove_entry(const std::wstring &path, DWORD attr, DWORD tag, std::wstring &error, int depth)
{
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(lp(path).c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
    const bool dir = attr & FILE_ATTRIBUTE_DIRECTORY;
    if (dir && !is_link(attr, tag)) {
        if (depth >= kMaxDepth) { error = L"cannot remove " + path + L": too many nested folders"; return false; }
        std::vector<WIN32_FIND_DATAW> entries;
        list_dir(path, entries);
        for (const WIN32_FIND_DATAW &e : entries)
            if (!remove_entry(path + L"\\" + e.cFileName, e.dwFileAttributes, e.dwReserved0, error, depth + 1)) return false;
    }
    if (!delete_one(path, dir)) { error = L"cannot remove " + path + L" (" + win_error(GetLastError()) + L")"; return false; }
    return true;
}

// A payload path as a relative Windows path; false for anything that could leave the mod folder.
inline bool payload_rel(const char *path, std::wstring &rel)
{
    rel.clear();
    if (!path || !*path) return false;
    std::wstring w = wide(path);
    for (auto &c : w) if (c == L'/') c = L'\\';
    // no wildcard either: entry_info's FindFirstFileW would take it as a pattern
    if (w.empty() || w[0] == L'\\' || w.find_first_of(L":*?\"<>|") != std::wstring::npos) return false;
    for (size_t at = 0; at <= w.size();) {
        size_t end = w.find(L'\\', at);
        if (end == std::wstring::npos) end = w.size();
        const std::wstring part = w.substr(at, end - at);
        if (part.empty() || part == L"." || part == L"..") return false;
        at = end + 1;
    }
    rel = w;
    return true;
}

// The mirror's second half: files under the mod folder that the payload does not have go, then folders left
// empty. keep: lower-case relative paths of the payload's files and of their folders. Links are left alone.
inline bool prune(const std::wstring &dir, const std::wstring &rel, const std::set<std::wstring> &keep, size_t &removed, std::wstring &error, int depth)
{
    std::vector<WIN32_FIND_DATAW> entries;
    if (!list_dir(dir, entries)) return true;
    for (const WIN32_FIND_DATAW &e : entries) {
        const std::wstring name = e.cFileName, sub = rel.empty() ? name : rel + L"\\" + name, path = dir + L"\\" + name;
        if (is_link(e.dwFileAttributes, e.dwReserved0)) { logw(L"  left alone (a link): " + path); continue; }
        if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth + 1 >= kMaxDepth) continue;
            if (!prune(path, sub, keep, removed, error, depth + 1)) return false;
            if (!keep.count(lower(sub)) && RemoveDirectoryW(lp(path).c_str())) logw(L"  removed old folder " + sub);   // only when empty
        } else if (!keep.count(lower(sub))) {
            if (!remove_entry(path, e.dwFileAttributes, 0, error, depth)) return false;
            ++removed;
            logw(L"  removed old file " + sub);
        }
    }
    return true;
}

// The only things uninstall deletes in a user folder. base: the full path of a folder that passed check_user_folder.
inline std::vector<std::wstring> owned_paths(const std::wstring &base, bool settings)
{
    const std::wstring cur = join(base, L"current");
    std::vector<std::wstring> paths = {cur + L"\\mods\\unpacked\\" + kModFolder, cur + L"\\mods\\" + kDataFolder,
                                       cur + L"\\temp\\mods\\" + kDataFolder, cur + L"\\temp\\mods\\unpacked\\" + kModFolder};
    if (settings) paths.push_back(cur + L"\\settings\\" + kSettingsFile);
    return paths;
}

// Last check before a delete: under <base>\current\, no "." or ".." step below it, named like one of ours.
inline bool owned(const std::wstring &path, const std::wstring &base)
{
    const std::wstring cur = join(base, L"current") + L"\\";
    if (!is_absolute(base) || path.size() <= cur.size() || !iequal(path.substr(0, cur.size()), cur)) return false;
    const std::wstring below = path.substr(cur.size());
    if (below[0] == L'.' || below.find(L"\\.") != std::wstring::npos || below.find(L'/') != std::wstring::npos) return false;
    const std::wstring name = name_of(path);
    return iequal(name, std::wstring(kModFolder)) || iequal(name, std::wstring(kDataFolder)) || iequal(name, std::wstring(kSettingsFile));
}

// Writing through a link would change what it points to (e.g. a developer's working copy of the mod).
inline std::wstring link_refusal(const std::wstring &path, bool dir)
{
    return path + (dir ? L" is a link (junction) to another folder" : L" is a link to another file") +
           L": Sonic Beam does not write through it. Remove the link, then install again.";
}

// The converter trims every trailing '\' (its trim_folder): a drive root "D:\" would reach it as "D:", which
// means the current folder of drive D:. "D:\." names the root and survives the trim.
inline std::wstring converter_folder(const std::wstring &dir)
{
    std::wstring d = clean_folder(dir);
    if (d.size() == 3 && d[1] == L':' && d[2] == L'\\') d += L'.';
    return d;
}

} // namespace detail

inline bool installed_here(const std::wstring &user)
{
    const std::wstring base = clean_folder(user);
    return detail::is_absolute(base) && detail::is_dir(detail::join(base, L"current\\mods\\unpacked\\") + kModFolder);
}

// Anything uninstall() removes (the Lab settings included) is in this user folder.
inline bool has_ours(const std::wstring &user)
{
    const std::wstring base = clean_folder(user);
    if (!detail::is_absolute(base)) return false;
    for (const std::wstring &path : detail::owned_paths(detail::full_path(base), true)) {
        DWORD attr = 0, tag = 0, code = 0;
        if (detail::entry_info(path, attr, tag, code)) return true;
    }
    return false;
}

// Mirror: every payload file is written (unchanged ones are left as they are), then whatever else is in the
// mod folder goes. Files are overwritten in place, never deleted first. Nothing outside the mod folder is touched.
inline bool install_mod_files(const std::wstring &user, std::wstring &error)
{
    error.clear();
    std::wstring why;
    const std::wstring picked = clean_folder(user);
    if (!check_user_folder(picked, why)) { error = why; return false; }
    const std::wstring base = detail::full_path(picked);
    if (!check_user_folder(base, why)) { error = why; return false; }
    const std::wstring target = detail::join(base, L"current\\mods\\unpacked\\") + kModFolder;
    DWORD attr = 0, tag = 0, code = 0;
    if (detail::entry_info(target, attr, tag, code)) {
        if (detail::is_link(attr, tag)) { error = detail::link_refusal(target, true); return false; }   // e.g. a developer's link to a source folder
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { error = target + L" is a file, not a folder."; return false; }
    }
    if (!detail::make_dirs(target, error)) return false;
    logw(L"installing the mod into " + target);
    // A link further down (a folder or one file) is refused too. Missing parts are fine: make_dirs and
    // write_all create real ones.
    auto not_a_link = [&](const std::wstring &path) {
        DWORD a = 0, t = 0, c = 0;
        if (detail::entry_info(path, a, t, c)) {
            if (!detail::is_link(a, t)) return true;
            error = detail::link_refusal(path, (a & FILE_ATTRIBUTE_DIRECTORY) != 0);
            return false;
        }
        if (c == ERROR_FILE_NOT_FOUND || c == ERROR_PATH_NOT_FOUND) return true;
        error = L"cannot read " + path + L" (" + detail::win_error(c) + L")";   // unknown: maybe a link
        return false;
    };
    std::set<std::wstring> keep, checked;   // checked: lower-case relative folders already known not to be links
    size_t written = 0, same = 0, removed = 0;
    for (size_t i = 0; i < kModFileCount; ++i) {
        const ModFile &f = kModFiles[i];
        std::wstring rel;
        if (!detail::payload_rel(f.path, rel)) { error = L"bad file name in this setup: " + detail::wide(f.path ? f.path : ""); return false; }
        const std::wstring dest = target + L"\\" + rel;
        for (size_t at = rel.find(L'\\'); at != std::wstring::npos; at = rel.find(L'\\', at + 1)) {
            const std::wstring sub = rel.substr(0, at), low = detail::lower(sub);
            if (checked.count(low)) continue;
            if (!not_a_link(target + L"\\" + sub)) return false;
            checked.insert(low);
        }
        if (!not_a_link(dest)) return false;
        if (!detail::make_dirs(dest.substr(0, dest.find_last_of(L'\\')), error)) return false;
        if (detail::file_equals(dest, f.data, f.size)) ++same;
        else if (!detail::write_all(dest, f.data, f.size, error)) return false;
        else ++written;
        const std::wstring low = detail::lower(rel);
        keep.insert(low);
        for (size_t at = low.find(L'\\'); at != std::wstring::npos; at = low.find(L'\\', at + 1)) keep.insert(low.substr(0, at));
    }
    if (!detail::prune(target, L"", keep, removed, error, 0)) return false;
    log("mod: %zu files (%zu written, %zu unchanged), %zu old file(s) removed", size_t(kModFileCount), written, same, removed);
    return true;
}

inline int convert(const std::wstring &user, const std::wstring &frontiers)
{
    std::vector<std::wstring> args = {L"sonicnative_convert", L"--userpath", detail::converter_folder(user), L"--frontiers",
                                      detail::converter_folder(frontiers)};
    std::vector<wchar_t *> argv;
    for (std::wstring &a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    logw(L"converting Sonic: --userpath \"" + args[2] + L"\" --frontiers \"" + args[4] + L"\"");
    int code = 1;
    try {
        code = sonicnative_convert_main(int(args.size()), argv.data());
    } catch (const std::exception &e) {   // e.g. out of memory: a failed conversion, not a crashed setup
        log("error: the converter stopped: %s", e.what());
        code = 1;
    } catch (...) {
        log("error: the converter stopped");
        code = 1;
    }
    std::fflush(stdout);
    log("converter exit code %d", code);
    return code;
}

// setup.ini (%LOCALAPPDATA%\SonicBeamNative\setup.ini, UTF-8), written after each successful install: the user
// folder (the next run's first guess) and the Sonic Frontiers folder (the next run's, and the converter's default
// --frontiers).
inline bool save_setup_ini(const std::wstring &user, const std::wstring &frontiers, std::wstring &error)
{
    error.clear();
    const std::wstring picked = clean_folder(user), dir = install_dir(), path = setup_ini_path();
    if (!detail::is_absolute(picked)) { error = L"not a full path: " + picked; return false; }   // run_install checked it already
    if (dir.empty()) { error = L"%LOCALAPPDATA% is not set: cannot write setup.ini."; return false; }
    const std::string text = "userpath=" + detail::utf8(detail::full_path(picked)) + "\r\nfrontiers=" + detail::utf8(clean_folder(frontiers)) + "\r\n";
    if (!detail::make_dirs(dir, error) || !detail::write_all(path, text, error)) return false;
    logw(L"wrote " + path);
    return true;
}

// Removes what Sonic Beam made in one user folder: the mod, the converted data, BeamNG's caches of both and
// (remove_settings) the Lab settings. setup.ini (and its folder, when empty) goes too when it names this user
// folder or no folder. Returns 0 removed, 1 failed, 2 bad folder or nothing of ours there, 3 BeamNG.drive running
// (2 and 3 touch nothing).
inline int uninstall(const std::wstring &user, bool remove_settings, std::wstring &error)
{
    error.clear();
    auto stop = [&](int code, const std::wstring &what) { error = what; logw(L"error: " + what); return code; };
    if (beamng_running()) return stop(3, L"BeamNG.drive is running: close it and try again.");
    const std::wstring picked = clean_folder(user);
    if (picked.empty()) return stop(2, L"No BeamNG.drive user folder was given: nothing was removed.");
    std::wstring why;
    if (!check_user_folder(picked, why) || !check_user_folder(detail::full_path(picked), why))
        return stop(2, L"BeamNG.drive user folder " + picked + L": " + why + L" Nothing was removed.");
    const std::wstring base = detail::full_path(picked);
    if (!has_ours(base)) return stop(2, L"Sonic Beam is not installed in " + base + L".");
    for (const std::wstring &path : detail::owned_paths(base, remove_settings)) {
        if (!detail::owned(path, base)) return stop(1, L"refused to delete " + path);
        DWORD attr = 0, tag = 0, code = 0;
        if (!detail::entry_info(path, attr, tag, code)) {
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) { logw(L"not there: " + path); continue; }
            return stop(1, L"cannot read " + path + L" (" + detail::win_error(code) + L")");
        }
        if (!detail::remove_entry(path, attr, tag, error, 0)) return stop(1, error);
        logw(L"removed " + path);
    }
    const std::wstring settings = detail::join(base, L"current\\settings\\") + kSettingsFile;
    if (!remove_settings && detail::is_file(settings)) logw(L"kept your Lab settings: " + settings);
    log("Sonic Beam removed from this user folder");
    const SetupIni ini = read_setup_ini();
    const std::wstring file = setup_ini_path(), dir = install_dir();
    if (ini.present && !ini.userpath.empty() && !detail::same_folder(ini.userpath, base)) {
        logw(L"setup.ini stays: it names another user folder (" + ini.userpath + L")");
    } else if (ini.present) {
        if (!detail::delete_one(file, false)) return stop(1, L"cannot remove " + file + L" (" + detail::win_error(GetLastError()) + L")");
        logw(L"removed " + file);
        if (RemoveDirectoryW(detail::lp(dir).c_str())) logw(L"removed " + dir);   // only when empty
    }
    return 0;
}

} // namespace setup
