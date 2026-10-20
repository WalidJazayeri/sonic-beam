// SPDX-License-Identifier: MIT
// Developer tool: list the entries of a Frontiers PAC archive (names and sizes only).
#include <cstdio>
#include <algorithm>
#include "../../src/frontiers/pac.hpp"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: pacls <file.pac> [filter]\n"); return 2; }
    frontiers::Pac pac;
    std::string error;
    if (!pac.load(argv[1], error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }
    std::string filter;
    if (argc > 2) for (wchar_t *c = argv[2]; *c; ++c) filter += char(*c);
    std::vector<std::pair<std::string, uint32_t>> names;
    for (auto &[name, entry] : pac.files()) if (filter.empty() || name.find(filter) != std::string::npos) names.push_back({name, entry.size});
    std::sort(names.begin(), names.end());
    for (auto &[name, size] : names) std::printf("%10u %s\n", size, name.c_str());
    std::printf("%zu of %zu entries\n", names.size(), pac.files().size());
    return 0;
}
