// SPDX-License-Identifier: MIT
#include "assets.hpp"
#include "common.hpp"
#include "testmode.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace assets {

namespace {
Data data;
volatile LONG state = 0;   // 0 idle, 1 loading, 2 ready, 3 failed

std::wstring frontiers_folder()
{
    std::wstring dir = mod::setting(L"frontiers");
    for (auto &c : dir) if (c == L'/') c = L'\\';
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    return dir;
}

bool load_all()
{
    const std::wstring root = frontiers_folder();
    if (root.empty()) { mod::log("assets: no Sonic Frontiers folder in settings.ini (frontiers=...)"); return false; }
    mod::log("assets: reading Sonic Frontiers from %ls", root.c_str());
    const double t0 = mod::now_seconds();
    std::string error;
    frontiers::Pac sonic_pac, common_pac;
    if (!sonic_pac.load(root + L"\\" + sheets::sonic_model.archive, error)) { mod::log("assets: %ls: %s", sheets::sonic_model.archive, error.c_str()); return false; }
    if (!frontiers::parse_model(sonic_pac, sheets::sonic_model.entry, data.sonic, error)) { mod::log("assets: Sonic model: %s", error.c_str()); return false; }
    data.sonic_height = std::max(0.1f, data.sonic.max[1] - data.sonic.min[1]);
    data.bone_rest.assign(data.sonic.nodes.size() * 3, 0.0f);
    for (size_t n = 0; n < data.sonic.nodes.size(); ++n) {
        float bind[16];
        if (!frontiers::math4::inverse_affine(data.sonic.nodes[n].inverse_bind, bind)) continue;
        data.bone_rest[n * 3] = bind[3]; data.bone_rest[n * 3 + 1] = bind[7]; data.bone_rest[n * 3 + 2] = bind[11];
    }
    frontiers::Skeleton skeleton;
    const frontiers::PacEntry *skl = sonic_pac.find(sheets::sonic_skeleton.entry);
    if (!skl || !frontiers::parse_skeleton(*skl, skeleton, error)) { mod::log("assets: skeleton: %s", error.c_str()); return false; }
    for (auto &row : sheets::animations) {
        frontiers::Clip clip;
        clip.name = row.id;
        const frontiers::PacEntry *entry = sonic_pac.find(row.clip);
        if (!entry || !frontiers::bake_clip(*entry, skeleton, data.sonic, clip, error)) {
            mod::log("assets: clip %s: %s", row.clip, entry ? error.c_str() : "missing");
            return false;
        }
        data.clips.push_back(std::move(clip));
    }
    if (!common_pac.load(root + L"\\" + sheets::ring_model.archive, error) ||
        !frontiers::parse_model(common_pac, sheets::ring_model.entry, data.ring, error)) {
        mod::log("assets: ring: %s", error.c_str());
        return false;
    }
    // Boost effect: the aura mesh rides Sonic's skeleton (same bones, same order), the textures are
    // the ones Frontiers uses for its boost aura and boost_impact flash.
    {
        frontiers::Pac effect_pac;
        auto texture = [&](const frontiers::Pac &pac, const sheets::AssetRow &row, frontiers::Texture &out) {
            const frontiers::PacEntry *e = pac.find(row.entry);
            return e && frontiers::parse_dds(*e, out, error);
        };
        bool ok = frontiers::parse_model(sonic_pac, sheets::boost_aura_model.entry, data.aura, error) &&
                  data.aura.nodes.size() == data.sonic.nodes.size() &&
                  texture(sonic_pac, sheets::boost_aura_texture, data.aura_texture) &&
                  effect_pac.load(root + L"\\" + sheets::boost_glow_texture.archive, error) &&
                  texture(effect_pac, sheets::boost_glow_texture, data.glow_texture) &&
                  texture(effect_pac, sheets::boost_wave_texture, data.wave_texture);
        data.boost_fx = ok;
        data.trail_fx = ok && texture(sonic_pac, sheets::trail_texture, data.trail_texture) &&
                        texture(effect_pac, sheets::bolt_texture, data.bolt_texture) &&
                        texture(effect_pac, sheets::arc_texture, data.arc_texture) &&
                        texture(effect_pac, sheets::orb_texture, data.orb_texture);
        if (!data.trail_fx) mod::log("assets: boost trail unavailable (%s) - continuing without it", error.c_str());
        if (ok) mod::log("assets: boost aura %zu vertices, effect textures %ux%u", data.aura.vertices.size(), data.aura_texture.width, data.aura_texture.height);
        else mod::log("assets: boost effect unavailable (%s) - continuing without it", error.c_str());
    }
    // Boost aura shape: a bullet lying along the run, built here (no game file). A round nose (unit
    // hemisphere, tip at z = 1) then a tail flaring slightly back to z = -1 (the renderer stretches it).
    // v runs from 0 at the tip to 2 at the tail's end: the aura fades along it and its energy streams back.
    {
        const int around = 48, nose = 12, tail = 12, rings = nose + tail;
        frontiers::Model &m = data.dome;
        for (int i = 0; i <= rings; ++i) {
            float radius, z, nz, v;
            if (i <= nose) {
                const float phi = 1.5707963f * (1 - float(i) / nose);    // pi/2 at the tip, 0 at the nose's base
                radius = std::cos(phi); z = std::sin(phi); nz = z; v = 0.5f * (1 - z);
            } else {
                const float t = float(i - nose) / tail;
                radius = 1 + 0.15f * t; z = -t; nz = 0; v = 0.5f + 1.5f * t;
            }
            for (int j = 0; j <= around; ++j) {
                const float th = 6.2831853f * float(j) / around;
                frontiers::Vertex vx{};
                vx.pos[0] = radius * std::cos(th); vx.pos[1] = radius * std::sin(th); vx.pos[2] = z;
                const float nr = i <= nose ? radius : 1.0f;
                vx.normal[0] = nr * std::cos(th); vx.normal[1] = nr * std::sin(th); vx.normal[2] = nz;
                vx.uv[0] = 2.0f * float(j) / around; vx.uv[1] = v;
                vx.joints[0] = 0; vx.weights[0] = 255;
                m.vertices.push_back(vx);
            }
        }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < around; ++j) {
                const uint16_t a = uint16_t(i * (around + 1) + j), b = uint16_t(a + around + 1);
                for (uint16_t k : {a, b, uint16_t(a + 1), uint16_t(a + 1), b, uint16_t(b + 1)}) m.indices.push_back(k);
            }
        frontiers::Mesh mesh;
        mesh.index_count = uint32_t(m.indices.size());
        m.meshes.push_back(mesh);
    }
    // Jump ball: Frontiers' spin ball shell (its own small skeleton and loop) around Sonic curled up
    {
        frontiers::Skeleton ball_skeleton;
        const frontiers::PacEntry *skl = sonic_pac.find(sheets::ball_skeleton.entry);
        const frontiers::PacEntry *clip = sonic_pac.find(sheets::ball_clip.entry);
        data.ball_clip.name = "ball_shell";
        data.ball_fx = frontiers::parse_model(sonic_pac, sheets::ball_model.entry, data.ball, error) &&
                       skl && frontiers::parse_skeleton(*skl, ball_skeleton, error) &&
                       clip && frontiers::bake_clip(*clip, ball_skeleton, data.ball, data.ball_clip, error);
        if (data.ball_fx) mod::log("assets: spin ball %zu vertices, %zu nodes, %u frames", data.ball.vertices.size(), data.ball.nodes.size(), data.ball_clip.frames);
        else mod::log("assets: spin ball unavailable (%s) - Sonic still curls up without it", error.c_str());
    }
    // Footsteps: a foot lands when its toe comes down through the bottom fifth of its range.
    {
        int toes[2] = {-1, -1};
        for (size_t n = 0; n < data.sonic.nodes.size(); ++n) {
            if (data.sonic.nodes[n].name == "Toe_L") toes[0] = int(n);
            if (data.sonic.nodes[n].name == "Toe_R") toes[1] = int(n);
        }
        data.step_phases.resize(data.clips.size());
        for (size_t c = 0; c < data.clips.size() && toes[0] >= 0 && toes[1] >= 0; ++c) {
            const frontiers::Clip &clip = data.clips[c];
            if (!sheets::animations[c].loop || clip.frames < 4) continue;
            for (int toe : toes) {
                float bind[16];
                if (!frontiers::math4::inverse_affine(data.sonic.nodes[toe].inverse_bind, bind)) continue;
                const float bx = bind[3], by = bind[7], bz = bind[11];
                std::vector<float> y(clip.frames);
                for (uint32_t f = 0; f < clip.frames; ++f) {
                    const float *s = clip.frame(f) + toe * 12;
                    y[f] = s[4] * bx + s[5] * by + s[6] * bz + s[7];
                }
                const float lo = *std::min_element(y.begin(), y.end()), hi = *std::max_element(y.begin(), y.end());
                if (hi - lo < 1e-3f) continue;
                const float threshold = lo + 0.2f * (hi - lo);
                const uint32_t loop = clip.frames - 1;
                for (uint32_t f = 0; f < loop; ++f)
                    if (y[f] <= threshold && y[(f + loop - 1) % loop] > threshold) data.step_phases[c].push_back(float(f) / loop);
            }
        }
    }
    if (data.step_phases.size() > size_t(sheets::Anim::run))
        mod::log("assets: footsteps at walk %zu, run %zu points of the cycle", data.step_phases[size_t(sheets::Anim::walk)].size(),
                 data.step_phases[size_t(sheets::Anim::run)].size());
    std::map<std::wstring, std::unique_ptr<frontiers::AcbBank>> banks;   // each bank file is read once
    for (auto &row : sheets::sounds) {
        frontiers::Pcm pcm;
        auto found = banks.find(row.bank);
        if (found == banks.end()) {
            auto bank = std::make_unique<frontiers::AcbBank>();
            if (!bank->load(root + L"\\image\\x64\\raw\\sound\\" + row.bank, error)) {
                mod::log("assets: sound bank %ls: %s", row.bank, error.c_str());
                data.sounds.push_back({});
                continue;
            }
            found = banks.emplace(row.bank, std::move(bank)).first;
        }
        if (!found->second->decode_cue(row.cue, pcm, error)) {
            mod::log("assets: sound %s (%s): %s - continuing without it", row.id, row.cue, error.c_str());
            pcm = {};
        } else if (row.maxSeconds > 0) {
            const size_t keep = size_t(row.maxSeconds * pcm.rate) * pcm.channels;
            if (keep < pcm.samples.size()) pcm.samples.resize(keep);
            const size_t fade = std::min(pcm.samples.size(), size_t(0.12 * pcm.rate) * pcm.channels);
            for (size_t i = 0; i < fade; ++i) {
                int16_t &s = pcm.samples[pcm.samples.size() - fade + i];
                s = int16_t(s * (1.0 - double(i) / fade));
            }
        }
        data.sounds.push_back(std::move(pcm));
    }
    mod::log("assets: Sonic %zu vertices / %zu textures, %zu clips, ring %zu vertices, %zu sounds in %.2fs",
             data.sonic.vertices.size(), data.sonic.textures.size(), data.clips.size(), data.ring.vertices.size(),
             data.sounds.size(), mod::now_seconds() - t0);
    return true;
}

DWORD WINAPI loader(void *)
{
    const bool ok = load_all();
    InterlockedExchange(&state, ok ? 2 : 3);
    if (ok) testmode::report_assets_loaded();
    return 0;
}
}

void start_loading()
{
    if (InterlockedCompareExchange(&state, 1, 0) != 0) return;
    if (HANDLE thread = CreateThread(nullptr, 0, loader, nullptr, 0, nullptr)) CloseHandle(thread);
    else InterlockedExchange(&state, 3);
}

const Data *ready() { return InterlockedCompareExchange(&state, 2, 2) == 2 ? &data : nullptr; }
bool failed() { return InterlockedCompareExchange(&state, 3, 3) == 3; }

} // namespace assets
