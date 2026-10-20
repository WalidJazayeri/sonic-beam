// SPDX-License-Identifier: MIT
// Logging and settings for Sonic Beam's renderer.
#include "common.hpp"
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace mod {

namespace {
SRWLOCK log_lock = SRWLOCK_INIT;
HANDLE log_file = INVALID_HANDLE_VALUE;
LARGE_INTEGER qpc_frequency{};

std::wstring module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\') + 1);
}
}

std::wstring data_dir()
{
    static std::wstring dir = [] {
        wchar_t base[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        std::wstring d = (n && n < MAX_PATH) ? std::wstring(base) + L"\\SonicBeam" : module_dir() + L"SonicBeam";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

double now_seconds()
{
    if (!qpc_frequency.QuadPart) QueryPerformanceFrequency(&qpc_frequency);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(qpc_frequency.QuadPart);
}

void log(const char *format, ...)
{
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, format);
    n += std::vsnprintf(line + n, sizeof(line) - n - 3, format, args);
    va_end(args);
    if (n < 0 || n > int(sizeof(line)) - 3) n = int(sizeof(line)) - 3;
    line[n++] = '\r'; line[n++] = '\n';
    AcquireSRWLockExclusive(&log_lock);
    if (log_file == INVALID_HANDLE_VALUE)
        log_file = CreateFileW((data_dir() + L"\\sonic-beam.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(log_file, line, DWORD(n), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&log_lock);
}

namespace {
std::vector<std::wstring> read_lines(const std::wstring &path)
{
    std::vector<std::wstring> lines;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return lines;
    std::string bytes(64 * 1024, '\0');
    DWORD got = 0;
    ReadFile(f, bytes.data(), DWORD(bytes.size()), &got, nullptr);
    CloseHandle(f);
    bytes.resize(got);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF) bytes.erase(0, 3);
    const int wn = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), wn);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::wstring lookup(const std::wstring &path, const wchar_t *key)
{
    const std::wstring prefix = std::wstring(key) + L"=";
    for (auto &line : read_lines(path)) if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    return {};
}
}

std::wstring setting(const wchar_t *key)
{
    std::wstring value = lookup(data_dir() + L"\\settings.ini", key);
    if (value.empty()) value = lookup(module_dir() + L"settings.ini", key);   // developer fallback beside the DLL
    return value;
}

void save_setting(const wchar_t *key, const std::wstring &value)
{
    const std::wstring path = data_dir() + L"\\settings.ini", prefix = std::wstring(key) + L"=";
    auto lines = read_lines(path);
    bool replaced = false;
    for (auto &line : lines) if (line.rfind(prefix, 0) == 0) { line = prefix + value; replaced = true; }
    if (!replaced) lines.push_back(prefix + value);
    std::wstring text;
    for (auto &line : lines) text += line + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    CloseHandle(f);
}

} // namespace mod
