// SPDX-License-Identifier: MIT
// Sonic Frontiers PXD skeleton and ACL-compressed animation clips, baked into
// per-frame skinning matrices for the model. PXD layout evidence from
// WistfulHopes' PXDAnimationTools; decoding through Nicholas Frechette's ACL/RTM (MIT).
#pragma once
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include "bytes.hpp"
#include "model.hpp"

namespace frontiers {

struct Bone { std::string name; int parent; };

struct Skeleton {
    std::vector<Bone> bones;   // authoritative PXD order
};

// Skinning matrices for every model node, 3 rows x 4 columns each, per frame.
struct Clip {
    std::string name;
    float rate = 30, duration = 0;
    uint32_t frames = 0, nodes = 0;
    std::vector<float> skin;   // frames * nodes * 12
    const float *frame(uint32_t f) const { return skin.data() + size_t(f) * nodes * 12; }
};

inline bool parse_skeleton(const PacEntry &entry, Skeleton &out, std::string &error)
{
    Bytes d{entry.data, entry.size};
    if (entry.size < 168 || std::memcmp(entry.data + 64, "KSXP", 4) || d.u32(68) != 512) { error = "unsupported PXD skeleton"; return false; }
    const uint64_t parents = d.u64(72) + 64, names = d.u64(104) + 64;
    const uint64_t count = d.u64(88);
    if (!count || count > 1024 || d.u64(120) != count || d.u64(152) != count) { error = "PXD skeleton sizes"; return false; }
    for (uint64_t i = 0; i < count; ++i) {
        Bone b;
        b.parent = d.i16(parents + 2 * i);
        b.name = d.cstr(d.u64(names + 16 * i) + 64);
        if (b.parent >= int(i) || b.parent < -1) { error = "PXD parent order"; return false; }
        out.bones.push_back(b);
    }
    return d.ok || (error = "PXD skeleton out of bounds", false);
}

namespace math4 {
// 4x4 row-major matrices acting on column vectors.
inline void mul(const float *a, const float *b, float *o)
{
    float r[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
        r[i * 4 + j] = s;
    }
    std::memcpy(o, r, sizeof(r));
}
inline bool inverse_affine(const float *m, float *o)
{
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
    const double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                           (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                           (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) o[r * 4 + k] = float(inv[r * 3 + k]);
        o[r * 4 + 3] = float(-(inv[r * 3] * m[3] + inv[r * 3 + 1] * m[7] + inv[r * 3 + 2] * m[11]));
    }
    o[12] = o[13] = o[14] = 0; o[15] = 1;
    return true;
}
inline void tr(const float *t, const float *q_in, float *o)
{
    float q[4] = {q_in[0], q_in[1], q_in[2], q_in[3]};
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-12f) for (float &v : q) v /= n; else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0],
                         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1],
                         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2],
                         0, 0, 0, 1};
    std::memcpy(o, m, sizeof(m));
}
} // namespace math4

// Implemented in anim_acl.cpp (ACL is a large header-only library).
// Writes `tracks` x 10 floats (translation xyz, rotation xyzw, scale xyz).
bool acl_clip_info(const uint8_t *blob, uint32_t size, uint32_t &tracks, uint32_t &samples, float &rate, float &duration);
bool acl_sample(const uint8_t *blob, uint32_t size, float seconds, float *out, uint32_t capacity);

inline bool bake_clip(const PacEntry &entry, const Skeleton &skeleton, const Model &model, Clip &clip, std::string &error)
{
    Bytes d{entry.data, entry.size};
    if (entry.size < 128 || std::memcmp(entry.data, "BINA210L", 8) || std::memcmp(entry.data + 64, "NAXP", 4) || d.u32(68) != 512) {
        error = "unsupported PXD clip"; return false;
    }
    if (d.u8(72)) { error = "additive clip"; return false; }
    if (d.u8(73) != 8) { error = "clip is not ACL-compressed"; return false; }
    const uint64_t tracks = d.u64(96), main = d.u64(104);
    const uint64_t pos = main + 64;
    const uint32_t size = d.u32(pos);
    if (!d.ok || size < 32 || !d.has(pos, size) || d.u32(pos + 8) != 0xac11ac11) { error = "ACL chunk out of bounds"; return false; }
    // ACL wants 16-byte aligned input.
    std::vector<uint8_t> aligned(size + 16);
    uint8_t *blob = aligned.data() + ((16 - (reinterpret_cast<uintptr_t>(aligned.data()) & 15)) & 15);
    std::memcpy(blob, entry.data + pos, size);
    uint32_t acl_tracks = 0, samples = 0;
    if (!acl_clip_info(blob, size, acl_tracks, samples, clip.rate, clip.duration)) { error = "ACL clip invalid"; return false; }
    if (acl_tracks != tracks || acl_tracks != skeleton.bones.size() || !samples || samples > 10000) { error = "clip does not match skeleton"; return false; }
    // Map PXD bones to model nodes by name.
    std::unordered_map<std::string, int> by_name;
    for (size_t n = 0; n < model.nodes.size(); ++n) by_name[model.nodes[n].name] = int(n);
    std::vector<int> to_node(acl_tracks, -1);
    for (uint32_t b = 0; b < acl_tracks; ++b) {
        auto it = by_name.find(skeleton.bones[b].name);
        if (it == by_name.end()) { error = "bone missing from model: " + skeleton.bones[b].name; return false; }
        to_node[b] = it->second;
    }
    const uint32_t nodes = uint32_t(model.nodes.size());
    std::vector<float> bind_global(nodes * 16);
    for (uint32_t n = 0; n < nodes; ++n)
        if (!math4::inverse_affine(model.nodes[n].inverse_bind, &bind_global[n * 16])) { error = "singular bind matrix"; return false; }
    clip.frames = samples; clip.nodes = nodes;
    clip.skin.resize(size_t(samples) * nodes * 12);
    std::vector<float> local(acl_tracks * 10), global_tr(acl_tracks * 16), scale(acl_tracks * 3), globals(nodes * 16);
    for (uint32_t f = 0; f < samples; ++f) {
        const float t = std::min(clip.duration, f / clip.rate);
        if (!acl_sample(blob, size, t, local.data(), uint32_t(local.size()))) { error = "ACL sampling failed"; return false; }
        globals = bind_global;
        for (uint32_t b = 0; b < acl_tracks; ++b) {
            const float *L = &local[b * 10];
            float m[16];
            math4::tr(L, L + 3, m);
            float s[3] = {L[7], L[8], L[9]};
            const int parent = skeleton.bones[b].parent;
            if (parent >= 0) {
                math4::mul(&global_tr[parent * 16], m, &global_tr[b * 16]);
                for (int k = 0; k < 3; ++k) s[k] *= scale[parent * 3 + k];
            } else std::memcpy(&global_tr[b * 16], m, sizeof(m));
            std::memcpy(&scale[b * 3], s, sizeof(s));
            float *g = &globals[to_node[b] * 16];
            std::memcpy(g, &global_tr[b * 16], 64);
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) g[r * 4 + c] *= s[c];
        }
        float *out = clip.skin.data() + size_t(f) * nodes * 12;
        for (uint32_t n = 0; n < nodes; ++n) {
            float skin[16];
            math4::mul(&globals[n * 16], model.nodes[n].inverse_bind, skin);
            std::memcpy(out + n * 12, skin, 48);
            for (int k = 0; k < 12; ++k) if (!std::isfinite(out[n * 12 + k])) { error = "non-finite pose"; return false; }
        }
    }
    return true;
}

} // namespace frontiers
