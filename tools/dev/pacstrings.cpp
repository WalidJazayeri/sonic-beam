// SPDX-License-Identifier: MIT
// Developer tool: print the readable strings (texture and mesh names, ...) inside entries of a
// Frontiers PAC archive whose names contain a filter, or save one entry with --save <out>.
// Reads the player's install; prints names only, or writes to a path the developer chooses.
#include <cstdio>
#include <string>
#include "../../src/frontiers/pac.hpp"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: pacstrings <file.pac> <filter> [--save <out>]\n"); return 2; }
    frontiers::Pac pac;
    std::string error, filter;
    if (!pac.load(argv[1], error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }
    for (wchar_t *c = argv[2]; *c; ++c) filter += char(*c);
    const bool save = argc > 4 && std::wstring(argv[3]) == L"--save";
    for (auto &[name, entry] : pac.files()) {
        if (name.find(filter) == std::string::npos) continue;
        if (save) {
            FILE *f = _wfopen(argv[4], L"wb");
            if (!f) return 1;
            std::fwrite(entry.data, 1, entry.size, f); std::fclose(f);
            std::printf("saved %s (%u bytes)\n", name.c_str(), entry.size);
            return 0;
        }
        std::printf("== %s\n", name.c_str());
        std::string run;
        for (uint32_t i = 0; i <= entry.size; ++i) {
            const char ch = i < entry.size ? char(entry.data[i]) : 0;
            if (ch >= 32 && ch < 127) { run += ch; continue; }
            if (run.size() >= 6) std::printf("   %s\n", run.c_str());
            run.clear();
        }
    }
    return 0;
}
