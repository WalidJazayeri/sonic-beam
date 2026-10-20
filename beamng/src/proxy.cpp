// SPDX-License-Identifier: MIT
// Sonic Beam is loaded by BeamNG.drive.x64.exe as a version.dll proxy placed in Bin64 (BeamNG's
// Lua sandbox does not let mods load native code). Every version API is forwarded to System32.
// Only the game's main process starts the renderer; the Chromium UI helpers (same exe, with a
// --type= argument) are left alone.
#include "common.hpp"
#include <cwchar>

extern "C" int sb_start();   // render11.cpp

namespace {
HMODULE real_version = nullptr;
INIT_ONCE version_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK load_real_version(PINIT_ONCE, PVOID, PVOID *)
{
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n && n < MAX_PATH - 16) {
        wcscat_s(path, L"\\version.dll");
        real_version = LoadLibraryW(path);
    }
    return TRUE;
}

FARPROC forward_version_api(const char *name)
{
    InitOnceExecuteOnce(&version_once, load_real_version, nullptr, nullptr);
    return real_version ? GetProcAddress(real_version, name) : nullptr;
}

bool main_game_process()
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t *base = std::wcsrchr(path, L'\\');
    if (!n || _wcsicmp(base ? base + 1 : path, L"BeamNG.drive.x64.exe") != 0) return false;
    return std::wcsstr(GetCommandLineW(), L"--type=") == nullptr;
}

DWORD WINAPI starter(void *)
{
    // Give the game a moment to load its graphics modules before probing them.
    Sleep(1500);
    sb_start();
    return 0;
}
} // namespace

#define FORWARD(name, result, params, args)                                              \
    extern "C" result WINAPI sh_##name params                                            \
    {                                                                                    \
        const DWORD error = GetLastError();                                              \
        auto fn = reinterpret_cast<result(WINAPI *) params>(forward_version_api(#name)); \
        if (!fn) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }                       \
        SetLastError(error);                                                             \
        return fn args;                                                                  \
    }
FORWARD(GetFileVersionInfoA, BOOL, (LPCSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
FORWARD(GetFileVersionInfoW, BOOL, (LPCWSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
FORWARD(GetFileVersionInfoSizeA, DWORD, (LPCSTR f, LPDWORD h), (f, h))
FORWARD(GetFileVersionInfoSizeW, DWORD, (LPCWSTR f, LPDWORD h), (f, h))
FORWARD(GetFileVersionInfoExA, BOOL, (DWORD x, LPCSTR f, DWORD h, DWORD l, LPVOID d), (x, f, h, l, d))
FORWARD(GetFileVersionInfoExW, BOOL, (DWORD x, LPCWSTR f, DWORD h, DWORD l, LPVOID d), (x, f, h, l, d))
FORWARD(GetFileVersionInfoSizeExA, DWORD, (DWORD x, LPCSTR f, LPDWORD h), (x, f, h))
FORWARD(GetFileVersionInfoSizeExW, DWORD, (DWORD x, LPCWSTR f, LPDWORD h), (x, f, h))
FORWARD(VerFindFileA, DWORD, (DWORD x, LPSTR a, LPSTR b, LPSTR c, LPSTR d, PUINT e, LPSTR f, PUINT g), (x, a, b, c, d, e, f, g))
FORWARD(VerFindFileW, DWORD, (DWORD x, LPWSTR a, LPWSTR b, LPWSTR c, LPWSTR d, PUINT e, LPWSTR f, PUINT g), (x, a, b, c, d, e, f, g))
FORWARD(VerInstallFileA, DWORD, (DWORD x, LPSTR a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, LPSTR f, PUINT g), (x, a, b, c, d, e, f, g))
FORWARD(VerInstallFileW, DWORD, (DWORD x, LPWSTR a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, LPWSTR f, PUINT g), (x, a, b, c, d, e, f, g))
FORWARD(VerLanguageNameA, DWORD, (DWORD l, LPSTR b, DWORD n), (l, b, n))
FORWARD(VerLanguageNameW, DWORD, (DWORD l, LPWSTR b, DWORD n), (l, b, n))
FORWARD(VerQueryValueA, BOOL, (LPCVOID b, LPCSTR s, LPVOID *o, PUINT l), (b, s, o, l))
FORWARD(VerQueryValueW, BOOL, (LPCVOID b, LPCWSTR s, LPVOID *o, PUINT l), (b, s, o, l))
#undef FORWARD

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (main_game_process())
            if (HANDLE t = CreateThread(nullptr, 0, starter, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
