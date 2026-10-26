// SPDX-License-Identifier: MIT
// Synthetic test rig for --fixture, built in Frontiers conventions (Y up, +Z front) so it goes through
// exactly the same axis change, writer and self-check as Sonic. No Frontiers data is involved.
//   joints  root at 0.5 m, chest at 1.0 m (child of root)
//   mesh    a 0.3 m square column, 1 m tall, flat sides; bottom on root, middle 50/50, top on chest
//   clips   idle (loop): chest swings +-20 deg about X; lean (one-shot): root rolls 30 deg about Z and back
#pragma once
#include <cmath>
#include <string>
#include <vector>
#include "skin_math.hpp"

namespace sonicnative {

namespace fixture {

inline void translate(float *m, float x, float y, float z) { std::memcpy(m, kIdentity, 64); m[3] = x; m[7] = y; m[11] = z; }
inline void rotate_x(float *m, float a) { std::memcpy(m, kIdentity, 64); m[5] = std::cos(a); m[6] = -std::sin(a); m[9] = std::sin(a); m[10] = std::cos(a); }
inline void rotate_z(float *m, float a) { std::memcpy(m, kIdentity, 64); m[0] = std::cos(a); m[1] = -std::sin(a); m[4] = std::sin(a); m[5] = std::cos(a); }

// Skin matrices (G * IB, 3x4) of both joints for one frame, from their globals.
inline void store(frontiers::Clip &clip, uint32_t frame, const frontiers::Model &model, const float *root, const float *chest)
{
    const float *g[2] = {root, chest};
    for (int n = 0; n < 2; ++n) {
        float s[16];
        mul(g[n], model.nodes[n].inverse_bind, s);
        std::memcpy(clip.skin.data() + (size_t(frame) * 2 + n) * 12, s, 48);
    }
}

} // namespace fixture

inline void build_fixture(frontiers::Model &model, std::vector<ClipSource> &clips)
{
    const float pi = 3.14159265358979f;
    frontiers::Node root{"root", -1, {}}, chest{"chest", 0, {}};
    fixture::translate(root.inverse_bind, 0, -0.5f, 0);
    fixture::translate(chest.inverse_bind, 0, -1.0f, 0);
    model.nodes = {root, chest};

    // Four flat sides, each 3 levels x 2 corners, CCW seen from outside (right = up x normal).
    const float h = 0.15f, levels[3] = {0, 0.5f, 1.0f};
    const float sides[4][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}};
    frontiers::Mesh mesh;
    mesh.material = "fixture";
    for (auto &n : sides) {
        const float r[3] = {n[2], 0, -n[0]};
        const uint16_t base = uint16_t(model.vertices.size());
        for (int l = 0; l < 3; ++l) {
            for (int c = 0; c < 2; ++c) {
                const float side = c ? h : -h;
                frontiers::Vertex v{};
                v.pos[0] = n[0] * h + r[0] * side; v.pos[1] = levels[l]; v.pos[2] = n[2] * h + r[2] * side;
                v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
                v.uv[0] = float(c); v.uv[1] = 1.0f - levels[l];
                if (l == 0) { v.joints[0] = 0; v.weights[0] = 255; }
                else if (l == 2) { v.joints[0] = 1; v.weights[0] = 255; }
                else { v.joints[0] = 0; v.weights[0] = 128; v.joints[1] = 1; v.weights[1] = 127; }
                model.vertices.push_back(v);
            }
        }
        for (int l = 0; l < 2; ++l) {
            const uint16_t bl = uint16_t(base + l * 2), br = uint16_t(bl + 1), tl = uint16_t(bl + 2), tr = uint16_t(bl + 3);
            for (uint16_t i : {bl, br, tr, bl, tr, tl}) model.indices.push_back(i);
        }
    }
    mesh.first_index = 0;
    mesh.index_count = uint32_t(model.indices.size());
    mesh.base_vertex = 0;
    model.meshes = {mesh};
    for (auto &v : model.vertices) for (int a = 0; a < 3; ++a) { model.min[a] = std::min(model.min[a], v.pos[a]); model.max[a] = std::max(model.max[a], v.pos[a]); }

    float bind_root[16], chest_local[16];
    fixture::translate(bind_root, 0, 0.5f, 0);
    fixture::translate(chest_local, 0, 0.5f, 0);
    {   // idle: 24-frame loop stored the Frontiers way, with the first pose repeated at the end
        ClipSource c;
        c.name = "idle"; c.loop = true;
        c.clip.name = "idle"; c.clip.frames = 25; c.clip.nodes = 2; c.clip.duration = 24 / 30.0f;
        c.clip.skin.resize(size_t(c.clip.frames) * 2 * 12);
        for (uint32_t f = 0; f < c.clip.frames; ++f) {
            float rot[16], local[16], g_chest[16];
            fixture::rotate_x(rot, 20 * pi / 180 * std::sin(2 * pi * float(f % 24) / 24));
            mul(chest_local, rot, local);
            mul(bind_root, local, g_chest);
            fixture::store(c.clip, f, model, bind_root, g_chest);
        }
        clips.push_back(std::move(c));
    }
    {   // lean: one second, 0 -> 30 -> 0 deg
        ClipSource c;
        c.name = "lean"; c.loop = false;
        c.clip.name = "lean"; c.clip.frames = 31; c.clip.nodes = 2; c.clip.duration = 1.0f;
        c.clip.skin.resize(size_t(c.clip.frames) * 2 * 12);
        for (uint32_t f = 0; f < c.clip.frames; ++f) {
            float rot[16], g_root[16], g_chest[16];
            fixture::rotate_z(rot, 30 * pi / 180 * std::sin(pi * float(f) / 30));
            mul(bind_root, rot, g_root);
            mul(g_root, chest_local, g_chest);
            fixture::store(c.clip, f, model, g_root, g_chest);
        }
        clips.push_back(std::move(c));
    }
}

} // namespace sonicnative
