// SPDX-License-Identifier: MIT
#include "testmode.hpp"
#include "common.hpp"
#include "gen/sheets.hpp"

namespace testmode {

namespace {
SRWLOCK lock = SRWLOCK_INIT;
bool is_active = false;
uint32_t test_seed = 0;
std::wstring checks_path, state_path, test_id;
bool reported[int(sheets::Test::count)] = {};
bool pending[int(sheets::Test::count)] = {};
double next_poll = 0;

std::string json_value(const std::string &text, const char *key)
{
    const std::string pattern = std::string("\"") + key + "\"";
    size_t at = text.find(pattern);
    if (at == std::string::npos) return {};
    at = text.find(':', at + pattern.size());
    if (at == std::string::npos) return {};
    ++at;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
    std::string value;
    if (at < text.size() && text[at] == '"') {
        for (size_t i = at + 1; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) { ++i; value += text[i] == 'n' ? '\n' : text[i]; }
            else value += text[i];
        }
    } else {
        while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ' ') value += text[at++];
    }
    return value;
}

std::wstring widen(const std::string &s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring test_file()
{
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(base) + L"\\Melty\\test\\sonic-beam.json" : std::wstring();
}

void write_pass(int index)
{
    if (checks_path.empty()) return;
    HANDLE f = CreateFileW(checks_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string line = std::string("pass ") + sheets::tests[index].id + "\n";
    DWORD written;
    WriteFile(f, line.data(), DWORD(line.size()), &written, nullptr);
    CloseHandle(f);
    reported[index] = true;
    pending[index] = false;
    mod::log("test: pass %s", sheets::tests[index].id);
}

void report(sheets::Test test)
{
    AcquireSRWLockExclusive(&lock);
    const int i = int(test);
    if (!reported[i]) {
        if (is_active) write_pass(i);
        else pending[i] = true;          // the test file may appear after the event
    }
    ReleaseSRWLockExclusive(&lock);
}
}

void poll(double now)
{
    if (now < next_poll) return;
    next_poll = now + 2.0;
    const std::wstring path = test_file();
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    AcquireSRWLockExclusive(&lock);
    if (f == INVALID_HANDLE_VALUE) {
        is_active = false;
    } else {
        std::string text(16384, '\0');
        DWORD got = 0;
        ReadFile(f, text.data(), DWORD(text.size()), &got, nullptr);
        CloseHandle(f);
        text.resize(got);
        const std::wstring id = widen(json_value(text, "testId") + "#" + json_value(text, "run"));
        const std::wstring checks = widen(json_value(text, "checks"));
        if (!checks.empty()) {
            if (id != test_id) {           // a new run: report everything again
                for (int i = 0; i < int(sheets::Test::count); ++i) { if (reported[i]) pending[i] = true; reported[i] = false; }
                test_id = id;
                mod::log("test: Melty test run %ls", id.c_str());
            }
            is_active = true;
            checks_path = checks;
            state_path = widen(json_value(text, "state"));
            test_seed = uint32_t(std::strtoul(json_value(text, "seed").c_str(), nullptr, 10));
            for (int i = 0; i < int(sheets::Test::count); ++i) if (pending[i]) write_pass(i);
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

bool active()
{
    AcquireSRWLockShared(&lock);
    const bool a = is_active;
    ReleaseSRWLockShared(&lock);
    return a;
}

uint32_t seed() { return test_seed; }

namespace {
DWORD WINAPI watcher(void *)
{
    for (;;) { poll(mod::now_seconds()); Sleep(1000); }
}
}

void start()
{
    if (HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr)) CloseHandle(t);
}

void write_state(const std::string &json)
{
    AcquireSRWLockShared(&lock);
    const std::wstring path = is_active ? state_path : std::wstring();
    ReleaseSRWLockShared(&lock);
    if (path.empty()) return;
    const std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, json.data(), DWORD(json.size()), &written, nullptr);
    CloseHandle(f);
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void report_assets_loaded() { report(sheets::Test::frontiers_sonic_loaded); }
void report_frames_drawn() { report(sheets::Test::sonic_drawn_in_beamng); }
void report_running() { report(sheets::Test::sonic_runs); }
void report_boosting() { report(sheets::Test::sonic_boosts); }
void report_sound_played() { report(sheets::Test::frontiers_sound_played); }

} // namespace testmode
