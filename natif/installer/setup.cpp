// SPDX-License-Identifier: MIT
// SonicBeam-Setup.exe, the native mode's installer: wWinMain, the command line, the window and its worker thread.
// Finding folders is detect.hpp's job, installing install.hpp's. stdout is the log file
// (%TEMP%\SonicBeamNative-setup.log): install.hpp and the converter print there, and the window shows the file as it grows.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS    // _wfreopen: stdout itself becomes the log
#endif
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "version.h"     // natif/build/gen, from natif/VERSION: SN_VERSION_STR, SN_VERSION_WSTR
#include "detect.hpp"
#include "install.hpp"

namespace {

enum ExitCode : int { kOk = 0, kFailed = 1, kBadArgs = 2, kBeamngRunning = 3, kCancelled = 4 };
enum class Action { none, install, uninstall };
enum class Tone { plain, good, bad, warn };

const wchar_t *const kTitle = L"Sonic Beam setup";
const wchar_t *const kClassName = L"SonicBeamNativeSetup";
const wchar_t *const kMutexName = L"Local\\SonicBeamNativeSetup";   // one setup at a time: same files, same log
constexpr UINT kMsgDone = WM_APP + 1;        // worker -> window: wParam exit code, lParam Action
constexpr UINT kMsgAutostart = WM_APP + 2;   // lParam: the Action named on the command line

// What the player reads
const wchar_t *const kIntro = L"Play as Sonic from Sonic Frontiers in BeamNG.drive. Sonic's model, animations, effects and sounds are "
                              L"converted from your own copy of Sonic Frontiers, on this PC. Nothing from the game is downloaded or shared.";
const wchar_t *const kUserMissing = L"Click Browse and choose the BeamNG.drive user folder (the one that holds 'current').";
const wchar_t *const kFrontiersMissing = L"Not found automatically. Click Browse and choose the folder that holds SonicFrontiers.exe.";
const wchar_t *const kRunningWarning = L"BeamNG.drive is running: close it before installing.";
const wchar_t *const kDoneInstall = L"Sonic Beam is installed. Start BeamNG.drive and load any map in Freeroam: you are Sonic. "
                                    L"Press O for the Sonic Beam menu. To uninstall it later, run this setup again and click Uninstall.";
const wchar_t *const kDoneUninstall = L"Sonic Beam was removed.";
const wchar_t *const kNotInstalledHere = L"Sonic Beam is not installed in this BeamNG.drive user folder.";
const wchar_t *const kOverlayRemove = L"Remove it first with uninstall.bat from its package: the two modes cannot be installed together.";
const wchar_t *const kAlreadyRunning = L"The Sonic Beam setup is already open or running: finish or close it, then try again.";
const wchar_t *const kRetryRunning = L"BeamNG.drive is running: close it and try again.";
const wchar_t *const kSomethingWrong = L"Something went wrong: see the log above (also saved in %TEMP%\\SonicBeamNative-setup.log).";
const wchar_t *const kUsage =
    L"SonicBeam-Setup.exe [--install | --uninstall] [--quiet] [--userpath <folder>] [--frontiers <folder>] [--remove-settings]\n\n"
    L"--install: copy the mod and convert Sonic (what --quiet does when no action is given). Run it again to update or repair.\n"
    L"--uninstall: remove Sonic Beam from the BeamNG.drive user folder (without --quiet: the window opens on its questions).\n"
    L"--quiet: no window; the exit code tells how it went: 0 ok, 1 failed (or another setup is running), 2 bad folders or arguments, "
    L"3 BeamNG.drive is running, 4 cancelled.\n"
    L"--userpath: the BeamNG.drive user folder (the one that holds 'current').\n"
    L"--frontiers: the Sonic Frontiers folder (the one that holds SonicFrontiers.exe).\n"
    L"--remove-settings: with --uninstall --quiet, also remove the Sonic Beam Lab settings (kept otherwise).\n\n"
    L"Everything is logged to %TEMP%\\SonicBeamNative-setup.log.";

// ---- command line ----

struct Args {
    Action action = Action::none;
    bool quiet = false, remove_settings = false, help = false;
    bool has_userpath = false, has_frontiers = false;
    std::wstring userpath, frontiers;
    std::wstring error;   // for the player; empty when the command line is fine
};

bool parse_args(Args &a)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { a.error = L"The command line cannot be read."; return false; }
    for (int i = 1; i < argc && a.error.empty(); ++i) {
        const std::wstring s = argv[i];
        const bool more = i + 1 < argc;
        auto set_action = [&](Action act) {
            if (a.action != Action::none && a.action != act) a.error = L"Choose only one of --install and --uninstall.";
            else a.action = act;
        };
        if (s == L"--quiet") a.quiet = true;
        else if (s == L"--install") set_action(Action::install);
        else if (s == L"--uninstall") set_action(Action::uninstall);
        else if (s == L"--remove-settings") a.remove_settings = true;
        else if (s == L"--userpath" && more) { a.userpath = argv[++i]; a.has_userpath = true; }
        else if (s == L"--frontiers" && more) { a.frontiers = argv[++i]; a.has_frontiers = true; }
        else if (s == L"--help" || s == L"-h" || s == L"/?") a.help = true;
        else a.error = L"Unknown or incomplete option: " + s;
    }
    LocalFree(argv);
    return a.error.empty();
}

// ---- the log ----

std::wstring log_path()
{
    std::wstring dir = setup::env_folder(L"TEMP");
    if (dir.empty()) {
        wchar_t buf[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
        if (n && n <= MAX_PATH) dir.assign(buf, n);
        while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    }
    return dir + L"\\SonicBeamNative-setup.log";
}

// Unbuffered, so the window's tail sees each line as soon as it is printed.
bool redirect_stdout(const std::wstring &path, bool append)
{
    if (!_wfreopen(path.c_str(), append ? L"a" : L"w", stdout)) return false;
    setvbuf(stdout, nullptr, _IONBF, 0);
    return true;
}

void log_header()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    setup::log("Sonic Beam setup %s (native mode), %04u-%02u-%02u %02u:%02u:%02u", SN_VERSION_STR, unsigned(t.wYear), unsigned(t.wMonth),
               unsigned(t.wDay), unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond));
    setup::logw(std::wstring(L"command line: ") + GetCommandLineW());
}

// ---- the work (the worker thread's, and --quiet's) ----

// Install (and reinstall): the mod files, the conversion, then setup.ini.
int run_install(const std::wstring &user, const std::wstring &frontiers)
{
    setup::log("");
    setup::log("== Install Sonic Beam %s ==", SN_VERSION_STR);
    setup::logw(L"BeamNG.drive user folder: " + user);
    setup::logw(L"Sonic Frontiers folder: " + frontiers);
    std::wstring why;
    if (!setup::check_user_folder(user, why)) { setup::logw(L"error: " + why); return kBadArgs; }
    if (!setup::check_frontiers(frontiers, why)) { setup::logw(L"error: " + why); return kBadArgs; }
    if (setup::beamng_running()) { setup::log("error: BeamNG.drive is running: close it and try again."); return kBeamngRunning; }
    std::wstring what;
    if (setup::overlay_active(user, what)) {   // both modes at once would put two Sonics in the game
        setup::logw(L"error: " + (what.empty() ? std::wstring(L"The Sonic Beam overlay mode is installed.") : what) + L" " + kOverlayRemove);
        return kBadArgs;
    }
    std::wstring error;
    setup::log("[1/2] Copying the mod files");
    if (!setup::install_mod_files(user, error)) { setup::logw(L"error: " + error); return kFailed; }
    setup::log("[2/2] Converting Sonic from your Sonic Frontiers");
    const int converted = setup::convert(user, frontiers);
    if (converted != 0) {
        setup::log("error: the conversion stopped (exit code %d)", converted);
        return converted == kBadArgs || converted == kBeamngRunning ? converted : kFailed;
    }
    if (!setup::save_setup_ini(user, frontiers, error)) { setup::logw(L"error: " + error); return kFailed; }
    setup::log("Done: Sonic Beam %s is installed.", SN_VERSION_STR);
    return kOk;
}

// uninstall() logs its own errors; its codes are ours (0 removed, 1 failed, 2 bad folder or nothing of ours there,
// 3 BeamNG running).
int run_uninstall(const std::wstring &user, bool remove_settings)
{
    setup::log("");
    setup::log("== Uninstall Sonic Beam ==");
    setup::logw(L"BeamNG.drive user folder: " + user);
    setup::log(remove_settings ? "The Sonic Beam Lab settings are removed too." : "The Sonic Beam Lab settings are kept.");
    if (setup::beamng_running()) { setup::log("error: BeamNG.drive is running: close it and try again."); return kBeamngRunning; }
    std::wstring error;
    const int code = setup::uninstall(user, remove_settings, error);
    if (code != kOk) return code == kBadArgs || code == kBeamngRunning ? code : kFailed;
    setup::logw(L"Done: Sonic Beam was removed from " + user + L".");
    return kOk;
}

struct Job { Action action; std::wstring user, frontiers; bool remove_settings; HWND notify; };

DWORD WINAPI worker_main(void *param)
{
    Job *job = static_cast<Job *>(param);
    const int code = job->action == Action::uninstall ? run_uninstall(job->user, job->remove_settings) : run_install(job->user, job->frontiers);
    PostMessageW(job->notify, kMsgDone, WPARAM(code), static_cast<LPARAM>(job->action));   // fails only once the window is gone
    delete job;
    return DWORD(code);
}

// --quiet: no window, the folders from the command line or found as the window would find them.
int run_quiet(const Args &a)
{
    const Action action = a.action == Action::none ? Action::install : a.action;
    std::wstring user;
    if (a.has_userpath) {
        user = setup::user_base(setup::clean_folder(a.userpath));
        if (user.empty()) { setup::log("error: --userpath is empty"); return kBadArgs; }
    } else {
        const setup::Found found = setup::find_beamng_user();
        user = found.path;
        setup::logw(L"BeamNG.drive user folder found in: " + found.source);
    }
    if (action == Action::uninstall) return run_uninstall(user, a.remove_settings);
    std::wstring frontiers;
    if (a.has_frontiers) {
        frontiers = setup::clean_folder(a.frontiers);
        if (frontiers.empty()) { setup::log("error: --frontiers is empty"); return kBadArgs; }
    } else {
        const setup::Found found = setup::find_frontiers();
        if (found.path.empty()) {
            setup::log("error: Sonic Frontiers was not found automatically: pass --frontiers <the folder that holds SonicFrontiers.exe>");
            return kBadArgs;
        }
        frontiers = found.path;
        setup::logw(L"Sonic Frontiers folder found in: " + found.source);
    }
    return run_install(user, frontiers);
}

// ---- the window ----

constexpr int kIdStatic = 99, kIdUserEdit = 100, kIdUserBrowse = 101, kIdFrontiersEdit = 102, kIdFrontiersBrowse = 103, kIdLog = 104;
constexpr int kIdInstall = 110, kIdUninstall = 112, kIdClose = 113;
constexpr UINT_PTR kTimerTail = 1, kTimerIdle = 2, kTimerUser = 3, kTimerFrontiers = 4;
// Layout, in pixels at 96 DPI (scaled by px())
constexpr int kMargin = 16, kGap = 12, kTight = 4, kColumn = 8, kMinLog = 96;

std::wstring window_text(HWND h)
{
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(size_t(n) + 1, L'\0');
    const int got = GetWindowTextW(h, &s[0], n + 1);
    s.resize(size_t(std::max(got, 0)));
    return s;
}

std::wstring utf8_to_wide(const char *s, size_t n)
{
    if (!n || n > 0x7fffffff) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, int(n), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(size_t(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, int(n), &w[0], len);
    return w;
}

// Bytes at the end of s that start a UTF-8 sequence not complete yet (kept for the next read).
size_t incomplete_utf8(const std::string &s)
{
    const size_t n = s.size();
    for (size_t back = 1; back <= 4 && back <= n; ++back) {
        const unsigned char c = static_cast<unsigned char>(s[n - back]);
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte: its lead is further back
        const size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return len > back ? back : 0;
    }
    return 0;
}

bool is_high_contrast()
{
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

// The read-only log box would keep Tab, Enter and Escape for itself: the window's keyboard navigation gets them.
LRESULT CALLBACK log_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_GETDLGCODE) return DefSubclassProc(h, msg, wp, lp) & ~LRESULT(DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_HASSETSEL);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, log_subclass, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

// One folder: label, edit box, Browse..., status line.
struct Field {
    HWND label = nullptr, edit = nullptr, browse = nullptr, status = nullptr;
    UINT_PTR timer = 0;          // the debounce timer of its edit box
    std::wstring found_in;       // where the folder was found; empty: the player chose it
    std::wstring value;          // the folder checked last (cleaned)
    std::wstring text;           // the status line
    Tone tone = Tone::plain;
    bool ok = false, pending = false;
};

struct Window {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool ready = false, high_contrast = false;
    HFONT font = nullptr, bold = nullptr, strong = nullptr, mono = nullptr;
    int line_h = 16, button_w = 100, min_client_w = 540, min_client_h = 480;

    HWND title = nullptr, intro = nullptr, warning = nullptr, progress = nullptr, log_box = nullptr, final_line = nullptr;
    HWND install = nullptr, uninstall = nullptr, close = nullptr;
    HWND focus = nullptr;   // given back when the window is activated again
    Field user, frontiers;
    const std::wstring title_text = L"Sonic Beam - native mode " SN_VERSION_WSTR;
    std::wstring warning_text, final_text, install_label = L"Install";
    Tone final_tone = Tone::plain;

    bool busy = false, running = false;
    bool installed = false;       // the mod folder is in the user folder: Install reads "Reinstall"
    bool leftovers = false;       // anything uninstall() removes is in the user folder (the mod folder or not)
    bool overlay = false;         // the overlay mode is installed: the two modes never go in together
    bool setting_text = false;    // an edit box is filled by the program, not typed in
    bool sizing = false, growing = false;   // the player drags the frame; grow_to_min() is resizing the window
    Action working = Action::none;
    HANDLE thread = nullptr;
    int exit_code = kOk;

    std::wstring log_file;        // empty: no log to show
    HANDLE log_handle = INVALID_HANDLE_VALUE;
    unsigned long long log_offset = 0;
    std::string log_pending;      // the start of a UTF-8 character cut by the last read

    int px(int v) const { return MulDiv(v, int(dpi), 96); }

    bool can_install() const { return !busy && user.ok && frontiers.ok && !running && !overlay; }
    bool can_uninstall() const { return !busy && (installed || leftovers); }

    bool create(HINSTANCE instance, const Args &args, int show)
    {
        inst = instance;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = window_proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));   // icon 1 when setup.rc carries one
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        if (!CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             800, 640, nullptr, nullptr, inst, this) || !hwnd)
            return false;
        dpi = GetDpiForWindow(hwnd);
        if (!dpi) dpi = 96;
        high_contrast = is_high_contrast();
        if (!make_fonts() || !create_controls()) { DestroyWindow(hwnd); return false; }
        ready = true;
        place_initial();
        init_fields(args);
        layout();
        if (!SetTimer(hwnd, kTimerTail, 150, nullptr) || !SetTimer(hwnd, kTimerIdle, 2000, nullptr))
            setup::log("warning: a window timer did not start (error %lu)", static_cast<unsigned long>(GetLastError()));
        tail_log();
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
        if (args.action != Action::none) PostMessageW(hwnd, kMsgAutostart, 0, static_cast<LPARAM>(args.action));
        return true;
    }

    void release()
    {
        for (HFONT f : {font, bold, strong, mono}) if (f) DeleteObject(f);
        font = bold = strong = mono = nullptr;
        if (log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
        log_handle = INVALID_HANDLE_VALUE;
    }

    HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id)
    {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    }

    bool create_controls()
    {
        // SS_EDITCONTROL: long paths without spaces wrap too; text_height() measures the same way
        const DWORD text = SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL;
        auto field = [&](Field &f, const wchar_t *label, int edit_id, int browse_id, UINT_PTR timer) {
            f.timer = timer;
            f.label = child(L"Static", label, text, 0, kIdStatic);
            f.edit = child(L"Edit", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, edit_id);
            f.browse = child(L"Button", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 0, browse_id);
            f.status = child(L"Static", L"", text, 0, kIdStatic);
            return f.label && f.edit && f.browse && f.status;
        };
        title = child(L"Static", title_text.c_str(), text, 0, kIdStatic);
        intro = child(L"Static", kIntro, text, 0, kIdStatic);
        const bool fields = field(user, L"BeamNG.drive user folder", kIdUserEdit, kIdUserBrowse, kTimerUser) &&
                            field(frontiers, L"Sonic Frontiers folder", kIdFrontiersEdit, kIdFrontiersBrowse, kTimerFrontiers);
        warning = child(L"Static", L"", text, 0, kIdStatic);
        progress = child(PROGRESS_CLASSW, L"", 0, 0, kIdStatic);
        log_box = child(L"Edit", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, kIdLog);
        final_line = child(L"Static", L"", text, 0, kIdStatic);
        install = child(L"Button", L"Install", WS_TABSTOP | BS_PUSHBUTTON, 0, kIdInstall);
        uninstall = child(L"Button", L"Uninstall", WS_TABSTOP | BS_PUSHBUTTON, 0, kIdUninstall);
        close = child(L"Button", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 0, kIdClose);
        if (!title || !intro || !fields || !warning || !progress || !log_box || !final_line || !install || !uninstall || !close)
            return false;
        SendMessageW(log_box, EM_SETLIMITTEXT, 0, 0);   // the converter writes far more than the default 32K characters
        if (!SetWindowSubclass(log_box, log_subclass, 1, 0)) setup::log("warning: the log box keeps Tab for itself");
        apply_fonts();
        return true;
    }

    // Segoe UI (the system's message font) at this monitor's DPI, a bold one for the title, a semibold one for
    // the final status line and Consolas for the log.
    bool make_fonts()
    {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        LOGFONTW lf{};
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) lf = ncm.lfMessageFont;
        else {
            lf.lfHeight = -MulDiv(9, int(dpi), 72);
            lf.lfWeight = FW_NORMAL;
            lf.lfCharSet = DEFAULT_CHARSET;
            lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
        }
        LOGFONTW title_lf = lf;
        title_lf.lfWeight = FW_BOLD;
        title_lf.lfHeight = MulDiv(lf.lfHeight, 4, 3);
        LOGFONTW strong_lf = lf;
        strong_lf.lfWeight = FW_SEMIBOLD;
        LOGFONTW mono_lf{};
        mono_lf.lfHeight = lf.lfHeight;
        mono_lf.lfWeight = FW_NORMAL;
        mono_lf.lfCharSet = DEFAULT_CHARSET;
        mono_lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpynW(mono_lf.lfFaceName, L"Consolas", LF_FACESIZE);
        HFONT made[4] = {CreateFontIndirectW(&lf), CreateFontIndirectW(&title_lf), CreateFontIndirectW(&strong_lf), CreateFontIndirectW(&mono_lf)};
        if (!made[0] || !made[1] || !made[2] || !made[3]) {
            for (HFONT f : made) if (f) DeleteObject(f);
            return font != nullptr;   // keep the fonts there are
        }
        HFONT old[4] = {font, bold, strong, mono};
        font = made[0]; bold = made[1]; strong = made[2]; mono = made[3];
        measure();
        apply_fonts();
        for (HFONT f : old) if (f) DeleteObject(f);
        return true;
    }

    // Line height and one width for every button (the widest label decides).
    void measure()
    {
        HDC dc = GetDC(hwnd);
        if (!dc) return;
        HGDIOBJ old = SelectObject(dc, font);
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) line_h = tm.tmHeight;
        int widest = 0;
        for (const wchar_t *t : {L"Install", L"Reinstall", L"Uninstall", L"Close", L"Browse..."}) {
            SIZE s{};
            if (GetTextExtentPoint32W(dc, t, int(std::wcslen(t)), &s)) widest = std::max(widest, int(s.cx));
        }
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        button_w = std::max(px(96), widest + px(32));
    }

    void apply_fonts()
    {
        auto set = [](HWND h, HFONT f) { if (h && f) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE); };
        set(title, bold);
        set(intro, font);
        for (Field *f : {&user, &frontiers}) { set(f->label, font); set(f->edit, font); set(f->browse, font); set(f->status, font); }
        set(warning, font);
        set(final_line, strong);
        set(log_box, mono);
        for (HWND b : {install, uninstall, close}) set(b, font);
        if (log_box) SendMessageW(log_box, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(4), px(4)));
    }

    void adjust(RECT &r) const
    {
        const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)), ex = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        if (!AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi)) AdjustWindowRectEx(&r, style, FALSE, ex);
    }

    // 720 x 620 at 96 DPI, centred on the work area of the monitor the window opened on.
    void place_initial()
    {
        RECT r{0, 0, px(720), px(620)};
        adjust(r);
        int w = int(r.right - r.left), h = int(r.bottom - r.top);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT work{0, 0, w, h};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
        w = std::min(w, int(work.right - work.left));
        h = std::min(h, int(work.bottom - work.top));
        SetWindowPos(hwnd, nullptr, int(work.left) + (int(work.right - work.left) - w) / 2, int(work.top) + (int(work.bottom - work.top) - h) / 2,
                     w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int text_height(HFONT f, const std::wstring &text, int width) const
    {
        if (text.empty() || width <= 0) return 0;
        HDC dc = GetDC(hwnd);
        if (!dc) return line_h;
        HGDIOBJ old = SelectObject(dc, f);
        RECT r{0, 0, width, 0};
        DrawTextW(dc, text.c_str(), int(text.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_EXPANDTABS | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        return std::max(int(r.bottom - r.top), line_h);
    }

    // Top down: title, intro, the two folders, the warning, the progress bar; bottom up: the buttons
    // (right-aligned) and the final status line; the log box takes what is left.
    void layout()
    {
        if (!ready || !hwnd) return;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;   // minimized
        const int width = int(rc.right), height = int(rc.bottom);
        const int m = px(kMargin), gap = px(kGap), tight = px(kTight), col = px(kColumn);
        const int inner = std::max(width - 2 * m, px(200));
        const int edit_h = line_h + px(8), button_h = line_h + px(14), bar_h = px(14);
        struct Pos { HWND window; int x, y, w, h; bool show; };
        std::vector<Pos> list;
        auto put = [&](HWND window, int x, int y, int w, int h, bool show) { list.push_back({window, x, y, std::max(w, 0), std::max(h, 0), show}); };

        int y = m;
        const int title_h = text_height(bold, title_text, inner);
        put(title, m, y, inner, title_h, true);
        y += title_h + px(6);
        const int intro_h = text_height(font, kIntro, inner);
        put(intro, m, y, inner, intro_h, true);
        y += intro_h + px(16);
        for (Field *f : {&user, &frontiers}) {
            put(f->label, m, y, inner, line_h, true);
            y += line_h + tight;
            const int edit_w = std::max(inner - button_w - col, px(80));
            put(f->edit, m, y, edit_w, edit_h, true);
            put(f->browse, m + edit_w + col, y, button_w, edit_h, true);
            y += edit_h + tight;
            const int status_h = std::max(line_h, text_height(font, f->text, inner));
            put(f->status, m, y, inner, status_h, true);
            y += status_h + gap;
        }
        if (warning_text.empty()) put(warning, m, y, inner, 0, false);
        else {
            const int warning_h = text_height(font, warning_text, inner);
            put(warning, m, y, inner, warning_h, true);
            y += warning_h + gap;
        }
        put(progress, m, y, inner, bar_h, true);
        y += bar_h + px(8);
        const int log_top = y;

        const int buttons_y = height - m - button_h;
        int x = width - m - button_w;
        for (HWND b : {close, uninstall, install}) { put(b, x, buttons_y, button_w, button_h, true); x -= button_w + col; }
        int log_bottom = buttons_y - gap;
        if (final_text.empty()) put(final_line, m, log_bottom, inner, 0, false);
        else {
            const int final_h = text_height(strong, final_text, inner);
            log_bottom -= final_h;
            put(final_line, m, log_bottom, inner, final_h, true);
            log_bottom -= gap;
        }
        put(log_box, m, log_top, inner, std::max(log_bottom - log_top, px(40)), true);
        min_client_w = std::max(px(540), 2 * m + 3 * button_w + 2 * col);
        min_client_h = log_top + px(kMinLog) + (height - log_bottom);

        HDWP batch = BeginDeferWindowPos(int(list.size()));
        bool deferred = batch != nullptr;
        for (const Pos &p : list) {
            if (!deferred) break;
            const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (p.show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
            batch = DeferWindowPos(batch, p.window, nullptr, p.x, p.y, p.w, p.h, flags);
            deferred = batch != nullptr;
        }
        if (deferred) EndDeferWindowPos(batch);
        else
            for (const Pos &p : list)
                SetWindowPos(p.window, nullptr, p.x, p.y, p.w, p.h, SWP_NOZORDER | SWP_NOACTIVATE | (p.show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // wrapped texts are drawn again at their new width
        for (HWND h : {title, intro, user.status, frontiers.status, warning, final_line}) InvalidateRect(h, nullptr, TRUE);
        // A line that just appeared (the warning, the final status, a wrapped folder status) may need more room
        // than the window has: grow it, or the log box would cover the lines under it. Not during a drag: the
        // frame is the player's until WM_EXITSIZEMOVE.
        if (!growing && !sizing && (width < min_client_w || height < min_client_h) && !IsZoomed(hwnd) && !IsIconic(hwnd))
            grow_to_min(width, height);
    }

    // The window at least min_client_w x min_client_h of client area, kept on its monitor's work area when it fits.
    // Its WM_SIZE lays the controls out again (growing stops that layout() from coming back here).
    void grow_to_min(int width, int height)
    {
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr)) return;
        const int w = int(wr.right - wr.left) + std::max(min_client_w - width, 0);
        const int h = int(wr.bottom - wr.top) + std::max(min_client_h - height, 0);
        int x = int(wr.left), y = int(wr.top);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (x + w > int(mi.rcWork.right)) x = std::max(int(mi.rcWork.left), int(mi.rcWork.right) - w);
            if (y + h > int(mi.rcWork.bottom)) y = std::max(int(mi.rcWork.top), int(mi.rcWork.bottom) - h);
        }
        growing = true;
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        growing = false;
    }

    COLORREF color_of(HWND control) const
    {
        Tone tone = Tone::plain;
        if (control == user.status) tone = user.tone;
        else if (control == frontiers.status) tone = frontiers.tone;
        else if (control == warning) tone = Tone::warn;
        else if (control == final_line) tone = final_tone;
        if (high_contrast || tone == Tone::plain) return GetSysColor(COLOR_WINDOWTEXT);
        return tone == Tone::good ? RGB(16, 124, 16) : tone == Tone::warn ? RGB(178, 74, 0) : RGB(196, 43, 28);
    }

    // ---- folders and state ----

    void set_edit(Field &f, const std::wstring &text)
    {
        setting_text = true;
        SetWindowTextW(f.edit, text.c_str());
        setting_text = false;
    }

    static std::wstring ok_text(const Field &f)
    {
        return f.found_in.empty() ? std::wstring(L"OK - chosen by you") : L"OK - found in: " + f.found_in;
    }

    void set_status(Field &f, bool ok, const std::wstring &text)
    {
        const Tone tone = ok ? Tone::good : Tone::bad;
        f.ok = ok;
        if (tone != f.tone) { f.tone = tone; InvalidateRect(f.status, nullptr, TRUE); }
        if (text != f.text) { f.text = text; SetWindowTextW(f.status, text.c_str()); layout(); }
    }

    void check_user()
    {
        user.pending = false;
        const std::wstring dir = setup::user_base(setup::clean_folder(window_text(user.edit)));   // a picked ...\current means its parent
        user.value = dir;
        std::wstring why;
        if (dir.empty()) set_status(user, false, kUserMissing);
        else if (setup::check_user_folder(dir, why)) set_status(user, true, ok_text(user));
        else set_status(user, false, why.empty() ? std::wstring(L"This is not a BeamNG.drive user folder.") : why);
    }

    void check_frontiers()
    {
        frontiers.pending = false;
        const std::wstring dir = setup::clean_folder(window_text(frontiers.edit));
        frontiers.value = dir;
        std::wstring why;
        if (dir.empty()) set_status(frontiers, false, kFrontiersMissing);
        else if (setup::check_frontiers(dir, why)) set_status(frontiers, true, ok_text(frontiers));
        else set_status(frontiers, false, why.empty() ? std::wstring(L"This is not the Sonic Frontiers folder.") : why);
    }

    // A check still waiting for its timer runs now (the player may click right after typing).
    void flush_checks()
    {
        if (user.pending) { KillTimer(hwnd, user.timer); check_user(); }
        if (frontiers.pending) { KillTimer(hwnd, frontiers.timer); check_frontiers(); }
    }

    // BeamNG running, the old overlay mode, what is installed: the warning line and the buttons.
    void refresh()
    {
        running = setup::beamng_running();
        installed = user.ok && setup::installed_here(user.value);
        leftovers = user.ok && setup::has_ours(user.value);
        std::wstring text, what;
        auto add = [&](const std::wstring &line) { if (!line.empty()) text += (text.empty() ? L"" : L"\r\n") + line; };
        if (running) add(kRunningWarning);
        overlay = user.ok && setup::overlay_active(user.value, what);
        if (overlay) add((what.empty() ? std::wstring(L"The Sonic Beam overlay mode is installed.") : what) + L" " + kOverlayRemove);
        if (text != warning_text) { warning_text = text; SetWindowTextW(warning, text.c_str()); layout(); }
        update_buttons();
    }

    void update_buttons()
    {
        const wchar_t *label = installed ? L"Reinstall" : L"Install";
        if (install_label != label) { install_label = label; SetWindowTextW(install, label); }
        EnableWindow(install, can_install());
        EnableWindow(uninstall, can_uninstall());
        for (Field *f : {&user, &frontiers}) { EnableWindow(f->edit, !busy); EnableWindow(f->browse, !busy); }
        if (GetActiveWindow() == hwnd) {   // a control just disabled cannot keep the keyboard
            HWND focused = GetFocus();
            if (!focused || focused == hwnd || !IsWindowEnabled(focused)) SetFocus(default_focus());
        }
    }

    HWND default_focus() const
    {
        if (can_install()) return install;
        if (!busy && !user.ok) return user.browse;
        if (!busy && !frontiers.ok) return frontiers.browse;
        return close;
    }

    void set_final(Tone tone, const std::wstring &text)
    {
        final_tone = tone;
        if (text != final_text) { final_text = text; SetWindowTextW(final_line, text.c_str()); layout(); }
        InvalidateRect(final_line, nullptr, TRUE);
    }

    void set_marquee(bool on)
    {
        const LONG_PTR style = GetWindowLongPtrW(progress, GWL_STYLE);
        if (on) {
            SetWindowLongPtrW(progress, GWL_STYLE, style | PBS_MARQUEE);
            SendMessageW(progress, PBM_SETMARQUEE, TRUE, 30);
        } else {   // without the marquee style the bar is drawn empty
            SendMessageW(progress, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(progress, GWL_STYLE, style & ~LONG_PTR(PBS_MARQUEE));
            SendMessageW(progress, PBM_SETPOS, 0, 0);
        }
        InvalidateRect(progress, nullptr, TRUE);
    }

    void init_fields(const Args &args)
    {
        setup::Found u = setup::find_beamng_user();
        if (args.has_userpath) { u.path = setup::user_base(setup::clean_folder(args.userpath)); u.source.clear(); }
        setup::Found f;
        if (args.has_frontiers) f.path = setup::clean_folder(args.frontiers);
        else f = setup::find_frontiers();
        auto from = [](const setup::Found &found) { return found.source.empty() ? std::wstring(L" (command line)") : L" (" + found.source + L")"; };
        setup::logw(L"BeamNG.drive user folder: " + u.path + from(u));
        if (f.path.empty()) setup::log("Sonic Frontiers folder: not found automatically");
        else setup::logw(L"Sonic Frontiers folder: " + f.path + from(f));
        user.found_in = u.source;
        frontiers.found_in = f.source;
        set_edit(user, u.path);
        set_edit(frontiers, f.path);
        check_user();
        check_frontiers();
        refresh();
    }

    void browse(Field &f, bool user_field)
    {
        IFileOpenDialog *dialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void **>(&dialog));
        if (FAILED(hr) || !dialog) { setup::log("error: the folder picker cannot open (0x%08lx)", static_cast<unsigned long>(hr)); return; }
        FILEOPENDIALOGOPTIONS options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(user_field ? L"Choose the BeamNG.drive user folder (the one that holds 'current')"
                                    : L"Choose the Sonic Frontiers folder (the one that holds SonicFrontiers.exe)");
        const std::wstring current = setup::clean_folder(window_text(f.edit));
        const DWORD attributes = current.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            IShellItem *start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void **>(&start))) && start) {
                dialog->SetFolder(start);
                start->Release();
            }
        }
        std::wstring picked;
        hr = dialog->Show(hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem *result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)) && result) {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) { picked = path; CoTaskMemFree(path); }
                result->Release();
            }
        } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            setup::log("error: the folder picker failed (0x%08lx)", static_cast<unsigned long>(hr));
        }
        dialog->Release();
        picked = setup::clean_folder(picked);
        if (picked.empty()) return;
        if (user_field) picked = setup::user_base(picked);
        KillTimer(hwnd, f.timer);
        set_edit(f, picked);
        f.found_in.clear();
        if (user_field) check_user();
        else check_frontiers();
        refresh();
    }

    // ---- the work ----

    void start(Action action, const std::wstring &user_dir, const std::wstring &frontiers_dir, bool remove_settings)
    {
        if (busy) return;
        busy = true;
        working = action;
        set_final(Tone::plain, action == Action::uninstall ? L"Removing Sonic Beam..." : L"Installing... The log above shows each step.");
        if (GetActiveWindow() == hwnd) SetFocus(close);
        update_buttons();
        set_marquee(true);
        Job *job = new Job{action, user_dir, frontiers_dir, remove_settings, hwnd};
        thread = CreateThread(nullptr, 0, worker_main, job, 0, nullptr);
        if (!thread) {
            const DWORD e = GetLastError();
            delete job;
            setup::log("error: the work cannot start (error %lu)", static_cast<unsigned long>(e));
            on_done(kFailed, action);
        }
    }

    void on_done(int code, Action action)
    {
        if (thread) { WaitForSingleObject(thread, 5000); CloseHandle(thread); thread = nullptr; }
        busy = false;
        working = Action::none;
        exit_code = code;
        set_marquee(false);
        tail_log();   // the worker's last lines
        if (code == kOk) set_final(Tone::good, action == Action::uninstall ? kDoneUninstall : kDoneInstall);
        else if (code == kBeamngRunning) set_final(Tone::bad, kRetryRunning);
        else set_final(Tone::bad, action == Action::uninstall && code == kBadArgs ? kNotInstalledHere : kSomethingWrong);
        check_user();
        check_frontiers();
        refresh();
    }

    void cancelled()
    {
        exit_code = kCancelled;
        setup::log("Uninstall cancelled.");
    }

    // The Uninstall button and --uninstall without --quiet: only the user folder on screen, after two questions.
    void ask_uninstall()
    {
        flush_checks();
        refresh();
        if (busy) return;
        const std::wstring target = user.value;
        if (!can_uninstall()) {
            setup::logw(std::wstring(L"Uninstall: ") + kNotInstalledHere);
            exit_code = kBadArgs;
            set_final(Tone::plain, kNotInstalledHere);
            return;
        }
        if (running) {
            setup::log("Uninstall: BeamNG.drive is running: close it and try again.");
            exit_code = kBeamngRunning;
            set_final(Tone::bad, kRetryRunning);
            return;
        }
        const std::wstring question = L"Remove Sonic Beam from " + target +
                                      L"?\n\nThis deletes the mod and Sonic's converted files. Sonic Frontiers and BeamNG.drive are not touched.";
        if (MessageBoxW(hwnd, question.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) { cancelled(); return; }
        const int keep = MessageBoxW(hwnd, L"Keep your Sonic Beam Lab settings?", kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (keep != IDYES && keep != IDNO) { cancelled(); return; }
        start(Action::uninstall, target, std::wstring(), keep == IDNO);
    }

    // --install / --uninstall without --quiet: the window opens and starts at once (Install when the folders allow
    // it; Uninstall on its questions).
    void autostart(Action action)
    {
        if (action == Action::uninstall) { ask_uninstall(); return; }
        flush_checks();
        refresh();
        if (can_install()) start(Action::install, user.value, frontiers.value, false);
        else setup::log("--install: not started: see the folders and the warning line above the log (BeamNG.drive running, the overlay mode)");
    }

    void on_close()
    {
        if (busy) {
            const wchar_t *question = working == Action::uninstall ? L"Sonic Beam is being removed. Quit anyway?" : L"Sonic Beam is being installed. Quit anyway?";
            if (MessageBoxW(hwnd, question, kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            // The work may end while the question is open (its message then came in already) or just after:
            // keep its result instead of dropping its message.
            if (busy && thread && WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0) {
                DWORD code = kFailed;
                if (!GetExitCodeThread(thread, &code)) code = kFailed;
                on_done(int(code), working);
            }
            if (busy) {
                setup::log("Quit by the player while the work was running.");
                exit_code = kCancelled;
            }
        }
        DestroyWindow(hwnd);
    }

    // ---- the log box: the log file, read as it grows ----

    void append_log(const std::wstring &text)
    {
        const int len = GetWindowTextLengthW(log_box);
        SendMessageW(log_box, EM_SETSEL, WPARAM(len), LPARAM(len));
        SendMessageW(log_box, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(log_box, EM_SCROLLCARET, 0, 0);
    }

    void tail_log()
    {
        if (log_file.empty()) return;
        if (log_handle == INVALID_HANDLE_VALUE) {
            log_handle = CreateFileW(log_file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log_handle == INVALID_HANDLE_VALUE) return;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(log_handle, &size) || size.QuadPart < 0) return;
        const unsigned long long end = static_cast<unsigned long long>(size.QuadPart);
        if (end < log_offset) {   // the file was started again: show it from the top
            log_offset = 0;
            log_pending.clear();
            SetWindowTextW(log_box, L"");
        }
        if (end == log_offset) return;
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(log_offset);
        if (!SetFilePointerEx(log_handle, at, nullptr, FILE_BEGIN)) return;
        char buf[16384];
        while (log_offset < end) {
            const DWORD want = DWORD(std::min<unsigned long long>(sizeof(buf), end - log_offset));
            DWORD got = 0;
            if (!ReadFile(log_handle, buf, want, &got, nullptr) || got == 0) break;
            log_pending.append(buf, got);
            log_offset += got;
        }
        const size_t keep = incomplete_utf8(log_pending);
        if (log_pending.size() <= keep) return;
        const std::wstring wide = utf8_to_wide(log_pending.data(), log_pending.size() - keep);
        log_pending.erase(0, log_pending.size() - keep);
        std::wstring text;
        text.reserve(wide.size() + wide.size() / 16);
        for (wchar_t c : wide) {   // the file has "\r\n" (text mode) or "\n": the edit box wants "\r\n"
            if (c == L'\r') continue;
            if (c == L'\n') text += L"\r\n";
            else text += c ? c : L' ';
        }
        if (!text.empty()) append_log(text);
    }

    // ---- messages ----

    bool command(int id, int code)
    {
        if (id == kIdUserEdit || id == kIdFrontiersEdit) {
            if (code == EN_CHANGE && !setting_text) {   // typed: check again once the player stops for 300 ms
                Field &f = id == kIdUserEdit ? user : frontiers;
                f.found_in.clear();
                f.pending = true;
                SetTimer(hwnd, f.timer, 300, nullptr);
            }
            return true;
        }
        if (code != BN_CLICKED) return false;
        switch (id) {
        case kIdUserBrowse: browse(user, true); return true;
        case kIdFrontiersBrowse: browse(frontiers, false); return true;
        case kIdInstall:
            flush_checks();
            refresh();
            if (can_install()) start(Action::install, user.value, frontiers.value, false);
            return true;
        case kIdUninstall: ask_uninstall(); return true;
        case kIdClose:
        case IDCANCEL: on_close(); return true;   // IDCANCEL: Escape
        case IDOK: return true;                   // Enter starts nothing by accident
        }
        return false;
    }

    void timer(UINT_PTR id)
    {
        if (id == kTimerTail) tail_log();
        else if (id == kTimerIdle) {
            if (busy) return;
            // A folder that was not OK is checked again: BeamNG's first start makes 'current', a drive comes back.
            if (!user.ok && !user.pending) check_user();
            if (!frontiers.ok && !frontiers.pending) check_frontiers();
            refresh();
        }
        else if (id == kTimerUser || id == kTimerFrontiers) {
            KillTimer(hwnd, id);
            if (id == kTimerUser) check_user();
            else check_frontiers();
            refresh();
        }
    }

    LRESULT proc(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout();
            return 0;
        case WM_ENTERSIZEMOVE:
            sizing = true;
            return 0;
        case WM_EXITSIZEMOVE:   // the drag is over: a status that wrapped onto more lines may need a taller window
            sizing = false;
            layout();
            return 0;
        case WM_GETMINMAXINFO:
            if (ready) {
                RECT r{0, 0, min_client_w, min_client_h};
                adjust(r);
                MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(lp);
                mmi->ptMinTrackSize.x = r.right - r.left;
                mmi->ptMinTrackSize.y = r.bottom - r.top;
                return 0;
            }
            break;
        case WM_DPICHANGED: {   // moved to a monitor with another scale: new fonts, the size Windows suggests
            const UINT old_dpi = dpi;
            dpi = HIWORD(wp) ? HIWORD(wp) : 96;
            min_client_w = MulDiv(min_client_w, int(dpi), int(old_dpi));
            min_client_h = MulDiv(min_client_h, int(dpi), int(old_dpi));
            make_fonts();
            const RECT *r = reinterpret_cast<const RECT *>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HWND control = reinterpret_cast<HWND>(lp);
            if (control == log_box || control == user.edit || control == frontiers.edit) break;   // read-only or disabled edits: Windows' look
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, color_of(control));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_COMMAND:
            if (command(LOWORD(wp), HIWORD(wp))) return 0;
            break;
        case WM_TIMER:
            timer(wp);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND f = GetFocus();
                if (f && IsChild(hwnd, f)) focus = f;
            } else if (!HIWORD(wp) && ready) {
                SetFocus(focus && IsWindow(focus) && IsWindowEnabled(focus) && IsWindowVisible(focus) ? focus : default_focus());
            }
            return 0;
        case WM_SETTINGCHANGE: {
            const UINT what = UINT(wp);
            if (ready && (what == SPI_SETNONCLIENTMETRICS || what == SPI_SETHIGHCONTRAST)) {
                high_contrast = is_high_contrast();
                make_fonts();
                layout();
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
            break;
        }
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case kMsgDone:
            on_done(int(wp), static_cast<Action>(lp));
            return 0;
        case kMsgAutostart:
            autostart(static_cast<Action>(lp));
            return 0;
        case WM_CLOSE:
            on_close();
            return 0;
        case WM_DESTROY:
            for (UINT_PTR t : {kTimerTail, kTimerIdle, kTimerUser, kTimerFrontiers}) KillTimer(hwnd, t);
            if (ready) PostQuitMessage(0);   // not when create() fails: a WM_QUIT would close its error box at once
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        Window *created = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        if (created) created->hwnd = h;
    }
    Window *w = reinterpret_cast<Window *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!w || w->hwnd != h) return DefWindowProcW(h, msg, wp, lp);   // WM_GETMINMAXINFO comes before WM_NCCREATE
    if (msg == WM_NCDESTROY) {
        w->hwnd = nullptr;
        w->ready = false;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return DefWindowProcW(h, msg, wp, lp);
    }
    return w->proc(msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    Args args;
    const bool args_ok = parse_args(args);
    // A second setup would write the same files and start the log again under the first one's window (so it
    // writes nothing to the log). An elevated setup's mutex denies us access instead of opening: another setup too.
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    const DWORD mutex_error = GetLastError();
    if ((mutex && mutex_error == ERROR_ALREADY_EXISTS) || (!mutex && mutex_error == ERROR_ACCESS_DENIED)) {
        if (!args.quiet) {   // the box first: it gets the foreground this launch has, then the open setup comes forward
            MessageBoxW(nullptr, kAlreadyRunning, kTitle, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            HWND other = FindWindowW(kClassName, nullptr);   // none: the other one runs --quiet
            if (other) {
                if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            }
        }
        if (mutex) CloseHandle(mutex);
        return kFailed;
    }
    const std::wstring log_file = log_path();
    const bool logging = redirect_stdout(log_file, false);
    log_header();
    if (!mutex) setup::log("warning: no single-setup guard (CreateMutexW error %lu): running anyway", static_cast<unsigned long>(mutex_error));

    if (!args_ok || args.help) {
        if (args_ok) setup::log("usage shown (--help)");
        else setup::logw(L"error: " + args.error);
        if (!args.quiet) {
            const std::wstring text = (args_ok ? std::wstring() : args.error + L"\n\n") + kUsage;
            MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | (args_ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        }
        if (mutex) CloseHandle(mutex);
        return args_ok ? kOk : kBadArgs;
    }

    if (args.quiet) {
        const int code = run_quiet(args);
        setup::log("exit code %d", code);
        if (mutex) CloseHandle(mutex);
        return code;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // the folder picker
    if (FAILED(com)) setup::log("warning: COM did not start (0x%08lx): Browse may not work", static_cast<unsigned long>(com));
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    if (!InitCommonControlsEx(&icc)) setup::log("warning: InitCommonControlsEx failed");

    Window window;
    window.log_file = logging ? log_file : std::wstring();
    if (!window.create(inst, args, show)) {
        setup::log("error: the window cannot open (error %lu)", static_cast<unsigned long>(GetLastError()));
        MessageBoxW(nullptr, L"The Sonic Beam setup window cannot open. The log is in %TEMP%\\SonicBeamNative-setup.log.", kTitle, MB_OK | MB_ICONERROR);
        window.release();
        if (SUCCEEDED(com)) CoUninitialize();
        if (mutex) CloseHandle(mutex);
        return kFailed;
    }
    if (!logging) window.append_log(L"The log file cannot be written: " + log_file + L"\r\n");

    MSG msg{};
    for (;;) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) break;
        if (window.hwnd && IsDialogMessageW(window.hwnd, &msg)) continue;   // Tab, Escape
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (window.thread) {
        DWORD code = kCancelled;
        if (WaitForSingleObject(window.thread, 0) == WAIT_OBJECT_0 && GetExitCodeThread(window.thread, &code)) {
            window.exit_code = int(code);   // it ended just as the window closed: its message was lost with the window
            CloseHandle(window.thread);
            window.thread = nullptr;
        } else {
            // Quit while working: stop the work where it is (stdout is unbuffered, nothing to flush). The converter
            // removes its manifest first, so the mod asks for a conversion next time.
            setup::log("exit code %d (quit while working)", kCancelled);
            TerminateProcess(GetCurrentProcess(), kCancelled);
        }
    }
    setup::log("exit code %d", window.exit_code);
    window.release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return window.exit_code;
}
