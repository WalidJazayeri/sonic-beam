// SPDX-License-Identifier: MIT
// Developer check: how far each Sonic Frontiers clip moves Sonic away from his origin (root
// motion). Reads the player's installed game; prints numbers only.
#include <algorithm>
#include <cstdio>
#include "../../src/frontiers/pac.hpp"
#include "../../src/frontiers/model.hpp"
#include "../../src/frontiers/anim.hpp"
#include "../src/gen/sheets.hpp"

static void centre(const frontiers::Model &m, const float *skin, float c[3])
{
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (auto &v : m.vertices) {
        float p[3] = {};
        for (int k = 0; k < 4; ++k) {
            if (!v.weights[k]) continue;
            const float *s = skin + v.joints[k] * 12, w = v.weights[k] / 255.0f;
            for (int r = 0; r < 3; ++r) p[r] += w * (s[r * 4] * v.pos[0] + s[r * 4 + 1] * v.pos[1] + s[r * 4 + 2] * v.pos[2] + s[r * 4 + 3]);
        }
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
    }
    for (int a = 0; a < 3; ++a) c[a] = (lo[a] + hi[a]) / 2;
}

int wmain(int argc, wchar_t **argv)
{
    std::wstring root = argc > 1 ? argv[1] : L"C:\games\Sonic Frontiers";
    std::string error;
    frontiers::Pac sonic;
    if (!sonic.load(root + L"\\" + sheets::sonic_model.archive, error)) { std::printf("sonic.pac: %s\n", error.c_str()); return 1; }
    frontiers::Model model;
    if (!frontiers::parse_model(sonic, sheets::sonic_model.entry, model, error)) { std::printf("model: %s\n", error.c_str()); return 1; }
    frontiers::Skeleton skeleton;
    const frontiers::PacEntry *skl = sonic.find(sheets::sonic_skeleton.entry);
    if (!skl || !frontiers::parse_skeleton(*skl, skeleton, error)) { std::printf("skeleton: %s\n", error.c_str()); return 1; }
    std::printf("root bone: %s\n", skeleton.bones[0].name.c_str());
    for (auto &row : sheets::animations) {
        const frontiers::PacEntry *e = sonic.find(row.clip);
        frontiers::Clip clip;
        if (!e || !frontiers::bake_clip(*e, skeleton, model, clip, error)) { std::printf("clip %s: %s\n", row.clip, e ? error.c_str() : "missing"); return 1; }
        float a[3], b[3], c[3];
        centre(model, clip.frame(0), a); centre(model, clip.frame(clip.frames / 2), b); centre(model, clip.frame(clip.frames - 1), c);
        std::printf("%-12s centre x/z first %6.2f %6.2f  mid %6.2f %6.2f  last %6.2f %6.2f  (y %.2f)\n", row.id, a[0], a[2], b[0], b[2], c[0], c[2], b[1]);
    }
    return 0;
}
