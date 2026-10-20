// SPDX-License-Identifier: MIT
// Developer check: read Sonic and the ring from an installed Sonic Frontiers and
// bake the clips listed in the animations sheet. Prints counts and bounds only.
#include <cstdio>
#include "../../src/frontiers/pac.hpp"
#include "../../src/frontiers/model.hpp"
#include "../../src/frontiers/anim.hpp"
#include "../../src/frontiers/acb.hpp"
#include "../../src/gen/sheets.hpp"

static void skinned_bounds(const frontiers::Model &m, const float *skin, float lo[3], float hi[3])
{
    for (int a = 0; a < 3; ++a) { lo[a] = 1e9f; hi[a] = -1e9f; }
    for (auto &v : m.vertices) {
        float p[3] = {};
        for (int k = 0; k < 4; ++k) {
            if (!v.weights[k]) continue;
            const float *s = skin + v.joints[k] * 12, w = v.weights[k] / 255.0f;
            for (int r = 0; r < 3; ++r) p[r] += w * (s[r * 4] * v.pos[0] + s[r * 4 + 1] * v.pos[1] + s[r * 4 + 2] * v.pos[2] + s[r * 4 + 3]);
        }
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
    }
}

int wmain(int argc, wchar_t **argv)
{
    std::wstring root = argc > 1 ? argv[1] : L"C:\\games\\Sonic Frontiers";
    std::string error;
    frontiers::Pac sonic, common;
    if (!sonic.load(root + L"\\" + sheets::sonic_model.archive, error)) { std::printf("sonic.pac: %s\n", error.c_str()); return 1; }
    if (!common.load(root + L"\\" + sheets::ring_model.archive, error)) { std::printf("CommonObject.pac: %s\n", error.c_str()); return 1; }
    frontiers::Model model, ring;
    if (!frontiers::parse_model(sonic, sheets::sonic_model.entry, model, error)) { std::printf("model: %s\n", error.c_str()); return 1; }
    std::printf("sonic: %zu nodes, %zu vertices, %zu triangles, %zu meshes, %zu textures, bounds y %.3f..%.3f x %.3f..%.3f z %.3f..%.3f\n",
                model.nodes.size(), model.vertices.size(), model.indices.size() / 3, model.meshes.size(), model.textures.size(),
                model.min[1], model.max[1], model.min[0], model.max[0], model.min[2], model.max[2]);
    for (auto &m : model.meshes)
        std::printf("  mesh %-24s idx %6u tex %d (%s) alpha %d wrap %d/%d\n", m.material.c_str(), m.index_count, m.texture,
                    m.texture >= 0 ? model.texture_names[m.texture].c_str() : "-", m.alpha_test, m.wrap_u, m.wrap_v);
    for (size_t t = 0; t < model.textures.size(); ++t)
        std::printf("  texture %s %ux%u dxgi %u mips %u\n", model.texture_names[t].c_str(), model.textures[t].width, model.textures[t].height, model.textures[t].dxgi, model.textures[t].mips);
    frontiers::Skeleton skeleton;
    const frontiers::PacEntry *skl = sonic.find(sheets::sonic_skeleton.entry);
    if (!skl || !frontiers::parse_skeleton(*skl, skeleton, error)) { std::printf("skeleton: %s\n", error.c_str()); return 1; }
    std::printf("skeleton: %zu bones\n", skeleton.bones.size());
    for (auto &row : sheets::animations) {
        const frontiers::PacEntry *e = sonic.find(row.clip);
        frontiers::Clip clip;
        if (!e || !frontiers::bake_clip(*e, skeleton, model, clip, error)) { std::printf("clip %s: %s\n", row.clip, e ? error.c_str() : "missing"); return 1; }
        float lo[3], hi[3];
        skinned_bounds(model, clip.frame(clip.frames / 2), lo, hi);
        std::printf("clip %-12s %3u frames @ %.0f fps (%.2fs) mid-frame bounds y %.3f..%.3f z %.3f..%.3f\n", row.id, clip.frames, clip.rate, clip.duration, lo[1], hi[1], lo[2], hi[2]);
    }
    if (!frontiers::parse_model(common, sheets::ring_model.entry, ring, error)) { std::printf("ring: %s\n", error.c_str()); return 1; }
    std::printf("ring: %zu nodes, %zu vertices, %zu meshes, %zu textures, bounds x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f\n",
                ring.nodes.size(), ring.vertices.size(), ring.meshes.size(), ring.textures.size(), ring.min[0], ring.max[0], ring.min[1], ring.max[1], ring.min[2], ring.max[2]);
    for (auto &row : sheets::sounds) {
        frontiers::AcbBank bank;
        frontiers::Pcm pcm;
        if (!bank.load(root + L"\\image\\x64\\raw\\sound\\" + row.bank, error) || !bank.decode_cue(row.cue, pcm, error)) {
            std::printf("sound %s (%s): %s\n", row.id, row.cue, error.c_str()); return 1;
        }
        std::printf("sound %-6s %-14s %u Hz x%u, %.2fs\n", row.id, row.cue, pcm.rate, pcm.channels, pcm.samples.size() / double(pcm.channels * pcm.rate));
    }
    return 0;
}

