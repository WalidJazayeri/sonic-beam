// SPDX-License-Identifier: MIT
// Frontiers model + baked clips -> a DAE-ready rig in BeamNG object space (Z up, +Y forward, meters).
// Matrices are row-major acting on column vectors, like frontiers::Node::inverse_bind and COLLADA.
//   bind global   B = inverse(IB)            bind local  Lb = inv(B_parent) * B
//   frame global  G = S * B (S = baked skin)  frame local L  = inv(G_parent) * G
//   axis change   X' = C X C, p' = C p, n' = C n, uv' = (u, 1 - v)
// The self-check chains the locals exactly as written and compares S' = G' * IB' with C S C.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../src/frontiers/anim.hpp"
#include "text.hpp"

namespace sonicnative {

using frontiers::math4::inverse_affine;
using frontiers::math4::mul;

// Frontiers (Y up, +Z = Sonic's front, +X = his left) -> BeamNG object (Z up, +Y front, +X right):
// (x, y, z) -> (-x, z, y). Proper rotation (det +1), C == C^-1, the overlay's own mapping (game.cpp).
constexpr float kAxis[16] = {-1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr float kSelfCheckLimit = 1e-4f;   // abort above this (skin matrix entries, meters for translations)
constexpr float kStaticLimit = 1e-6f;      // a joint whose local never moves more than this keeps its bind matrix

inline void conj(const float *m, float *o) { float t[16]; mul(kAxis, m, t); mul(t, kAxis, o); }
inline void from_3x4(const float *s, float *m) { std::memcpy(m, s, 48); m[12] = m[13] = m[14] = 0; m[15] = 1; }
inline float max_diff(const float *a, const float *b, int n = 16)
{
    float d = 0;
    for (int i = 0; i < n; ++i) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}
inline void quantize16(float *m) { for (int i = 0; i < 16; ++i) m[i] = quantize(m[i]); }

struct RigJoint {
    std::string name, sid;     // node name (TSShape node name) and COLLADA sid ([A-Za-z0-9_])
    int parent = -1;
    float bind_local[16];
    float inverse_bind[16];
    bool animated = false;
};

struct RigMesh {
    std::string object;        // TSShape object; the DAE node is <object>2 (trailing number = detail size)
    std::string material;      // game material name = mapTo
    std::vector<float> positions, normals, uvs;   // 3, 3, 2 per vertex
    std::vector<uint32_t> triangles;              // CCW around the outward normal
    std::vector<int> joints;                      // Name_array: rig joint index per slot
    std::vector<uint8_t> vcount;                  // influences per vertex
    std::vector<uint16_t> influences;             // (slot, weight byte) pairs, weight > 0
    size_t flipped = 0, agreeing = 0, degenerate = 0, uv_clamped = 0;
    bool reversed = false, clamp_u = false, clamp_v = false;
};

struct RigClip {
    std::string name;
    uint32_t first = 0, frames = 0;   // range on the concatenated timeline
    bool loop = false;
    float ref_kmh = 0, min_rate = 1, max_rate = 1;
    std::vector<float> steps;         // toe-down phases in 0..1 of the cycle (walk, run), ascending
    // Diagnostics
    uint32_t source_frames = 0;
    float seam = 0, max_scale = 0, max_shear = 0, check = 0, root_motion = 0, min_det = 1;
};

struct Rig {
    float fps = 30;
    std::vector<RigJoint> joints;
    std::vector<RigMesh> meshes;
    std::vector<RigClip> clips;
    uint32_t total_frames = 0;
    std::vector<float> locals;        // total_frames x joints x 16
    float bind_check = 0;
    bool flat = false;
    const float *local(uint32_t frame, size_t joint) const { return locals.data() + (size_t(frame) * joints.size() + joint) * 16; }
};

// A baked Frontiers clip plus the sheet row it came from.
struct ClipSource {
    std::string name;
    bool loop = false;
    float ref_kmh = 0, min_rate = 1, max_rate = 1;
    std::vector<float> steps;
    frontiers::Clip clip;
};

// How one Frontiers mesh appears in the DAE. clamp_u/v: its diffuse texture is clamp-addressed.
struct MeshNaming { std::string object, material; bool clamp_u = false, clamp_v = false; };

inline std::string sanitize_sid(const std::string &in)
{
    std::string out;
    for (char c : in) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

// Names Torque's COLLADA importer treats specially (LOD, collision, bounds, sequence markers).
inline bool reserved_name(const std::string &name)
{
    static const char *prefixes[] = {"base", "start", "detail", "nulldetail", "bounds", "collision", "loscol",
                                     "colmesh", "dummy", "sequence", "bb_", "bbz_", "sonicnative_armature"};
    std::string lower;
    for (char c : name) lower += char(std::tolower(static_cast<unsigned char>(c)));
    for (const char *p : prefixes) if (lower.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

// Joint tree, bind matrices and per-mesh geometry (shared by the real model and the fixture).
// flat: every joint hangs off the armature and its local is its model-space global, so Frontiers squash and
// stretch (non-uniform scale on the legs) never turns into shear in a child's local matrix.
inline bool build_bind(const frontiers::Model &model, const std::vector<MeshNaming> &naming, bool flat, Rig &rig, std::string &error)
{
    const size_t n = model.nodes.size();
    if (!n) { error = "model has no nodes"; return false; }
    if (naming.size() != model.meshes.size()) { error = "mesh naming does not match the model"; return false; }
    std::vector<float> bind(n * 16);
    std::set<std::string> names, sids;
    rig.flat = flat;
    rig.joints.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const frontiers::Node &node = model.nodes[i];
        if (node.parent >= int(i)) { error = "node " + node.name + " comes before its parent"; return false; }
        if (!inverse_affine(node.inverse_bind, &bind[i * 16])) { error = "singular inverse bind on " + node.name; return false; }
        RigJoint &j = rig.joints[i];
        j.parent = flat ? -1 : node.parent;
        j.name = node.name.empty() ? "joint" : node.name;
        if (reserved_name(j.name)) { std::printf("  note: node '%s' renamed sn_%s (reserved prefix)\n", j.name.c_str(), j.name.c_str()); j.name = "sn_" + j.name; }
        const std::string base = j.name;
        for (int k = 2; !names.insert(j.name).second; ++k) j.name = base + "_" + std::to_string(k);
        j.sid = sanitize_sid(j.name);
        for (int k = 2; !sids.insert(j.sid).second; ++k) j.sid = sanitize_sid(j.name) + "_" + std::to_string(k);
        float local[16];
        if (j.parent >= 0) {
            float inv_parent[16];
            inverse_affine(&bind[j.parent * 16], inv_parent);
            mul(inv_parent, &bind[i * 16], local);
        } else std::memcpy(local, &bind[i * 16], sizeof(local));
        conj(local, j.bind_local); quantize16(j.bind_local);
        conj(node.inverse_bind, j.inverse_bind); quantize16(j.inverse_bind);
    }
    // Bind check: chaining the written locals must invert the written inverse binds.
    std::vector<float> global(n * 16);
    for (size_t i = 0; i < n; ++i) {
        const RigJoint &j = rig.joints[i];
        if (j.parent >= 0) mul(&global[j.parent * 16], j.bind_local, &global[i * 16]);
        else std::memcpy(&global[i * 16], j.bind_local, 64);
        float id[16];
        mul(&global[i * 16], j.inverse_bind, id);
        rig.bind_check = std::max(rig.bind_check, max_diff(id, kIdentity));
    }
    if (rig.bind_check > kSelfCheckLimit) { char b[96]; std::snprintf(b, sizeof(b), "bind self-check failed (%.3g)", rig.bind_check); error = b; return false; }

    for (size_t m = 0; m < model.meshes.size(); ++m) {
        const frontiers::Mesh &src = model.meshes[m];
        const uint32_t first = src.base_vertex;
        const uint32_t end = m + 1 < model.meshes.size() ? model.meshes[m + 1].base_vertex : uint32_t(model.vertices.size());
        if (end <= first || end - first > 65535) { error = "mesh " + src.material + " has a bad vertex range"; return false; }
        RigMesh out;
        out.object = naming[m].object;
        out.material = naming[m].material;
        out.clamp_u = naming[m].clamp_u;
        out.clamp_v = naming[m].clamp_v;
        const uint32_t count = end - first;
        std::map<int, int> slot;
        for (uint32_t v = 0; v < count; ++v) {
            const frontiers::Vertex &vx = model.vertices[first + v];
            for (int k = 0; k < 4; ++k) if (vx.weights[k]) slot[vx.joints[k]] = 0;
        }
        for (auto &s : slot) { s.second = int(out.joints.size()); out.joints.push_back(s.first); }
        for (uint32_t v = 0; v < count; ++v) {
            const frontiers::Vertex &vx = model.vertices[first + v];
            const float p[3] = {-vx.pos[0], vx.pos[2], vx.pos[1]};
            float nr[3] = {-vx.normal[0], vx.normal[2], vx.normal[1]};
            const float len = std::sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
            if (len > 1e-6f) for (float &c : nr) c /= len; else { nr[0] = nr[1] = 0; nr[2] = 1; }
            for (int a = 0; a < 3; ++a) { out.positions.push_back(quantize(p[a])); out.normals.push_back(quantize(nr[a])); }
            // BeamNG materials sample with wrap: a clamp-addressed texture gets its UVs clamped instead
            // (exact for triangles on one side of an edge, close for the few that straddle it).
            float uv[2] = {vx.uv[0], vx.uv[1]};
            if (out.clamp_u) uv[0] = std::clamp(uv[0], 0.0f, 1.0f);
            if (out.clamp_v) uv[1] = std::clamp(uv[1], 0.0f, 1.0f);
            if (uv[0] != vx.uv[0] || uv[1] != vx.uv[1]) ++out.uv_clamped;
            out.uvs.push_back(quantize(uv[0]));
            out.uvs.push_back(quantize(1.0f - uv[1]));
            uint8_t c = 0;
            for (int k = 0; k < 4; ++k) {
                if (!vx.weights[k]) continue;
                if (vx.joints[k] >= n) { error = "skin joint out of range in " + src.material; return false; }
                out.influences.push_back(uint16_t(slot[vx.joints[k]]));
                out.influences.push_back(vx.weights[k]);
                ++c;
            }
            if (!c) { error = "vertex without weights in " + src.material; return false; }
            out.vcount.push_back(c);
        }
        // Winding: COLLADA wants CCW around the outward normal. Compare each face's geometric normal
        // with its vertex normals (the axis change has det +1, so Frontiers space gives the same answer).
        if (src.index_count % 3 || src.first_index + src.index_count > model.indices.size()) { error = "mesh " + src.material + " index range"; return false; }
        const uint16_t *idx = model.indices.data() + src.first_index;
        for (uint32_t t = 0; t + 2 < src.index_count; t += 3) {
            const frontiers::Vertex &a = model.vertices[first + idx[t]], &b = model.vertices[first + idx[t + 1]], &c = model.vertices[first + idx[t + 2]];
            const float e1[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
            const float e2[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
            const float g[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float d = g[0] * (a.normal[0] + b.normal[0] + c.normal[0]) + g[1] * (a.normal[1] + b.normal[1] + c.normal[1]) +
                            g[2] * (a.normal[2] + b.normal[2] + c.normal[2]);
            if (std::fabs(d) < 1e-14f) ++out.degenerate; else if (d > 0) ++out.agreeing; else ++out.flipped;
        }
        out.reversed = out.flipped > out.agreeing;
        for (uint32_t t = 0; t + 2 < src.index_count; t += 3) {
            out.triangles.push_back(idx[t]);
            out.triangles.push_back(idx[out.reversed ? t + 2 : t + 1]);
            out.triangles.push_back(idx[out.reversed ? t + 1 : t + 2]);
        }
        rig.meshes.push_back(std::move(out));
    }
    return true;
}

// Bakes every clip's joint locals onto one timeline and self-checks them.
inline bool build_animation(const frontiers::Model &model, const std::vector<ClipSource> &clips, Rig &rig, std::string &error)
{
    const size_t n = rig.joints.size();
    std::vector<float> bind(n * 16);
    for (size_t i = 0; i < n; ++i) inverse_affine(model.nodes[i].inverse_bind, &bind[i * 16]);
    uint32_t total = 0;
    for (auto &c : clips) {
        if (c.clip.nodes != n || !c.clip.frames) { error = "clip " + c.name + " does not match the model"; return false; }
        // A Frontiers loop ends on its first pose; Torque wraps the last key to the first by itself.
        const uint32_t kept = c.loop && c.clip.frames > 2 ? c.clip.frames - 1 : c.clip.frames;
        if (kept < 2) { error = "clip " + c.name + " is too short"; return false; }
        if (std::fabs(c.clip.rate - rig.fps) > 1e-3f) { error = "clip " + c.name + " is not at the rig's frame rate"; return false; }
        total += kept;
    }
    rig.total_frames = total;
    rig.locals.assign(size_t(total) * n * 16, 0.0f);
    // Root-motion probe: the first joint that is not a model-only root (PXD 'Reference').
    const size_t probe = n > 1 && rig.joints[0].parent < 0 ? 1 : 0;
    uint32_t first = 0;
    std::vector<float> global(n * 16);
    for (auto &c : clips) {
        RigClip out;
        out.name = c.name; out.loop = c.loop; out.ref_kmh = c.ref_kmh; out.min_rate = c.min_rate; out.max_rate = c.max_rate;
        out.steps = c.steps;
        out.first = first; out.source_frames = c.clip.frames;
        out.frames = c.loop && c.clip.frames > 2 ? c.clip.frames - 1 : c.clip.frames;
        if (c.loop) out.seam = max_diff(c.clip.frame(0), c.clip.frame(c.clip.frames - 1), int(n * 12));
        for (uint32_t f = 0; f < out.frames; ++f) {
            const float *skin = c.clip.frame(f);
            for (size_t i = 0; i < n; ++i) {
                float s4[16];
                from_3x4(skin + i * 12, s4);
                mul(s4, &bind[i * 16], &global[i * 16]);
            }
            for (size_t i = 0; i < n; ++i) {
                float local[16];
                const int p = rig.joints[i].parent;
                if (p >= 0) {
                    float inv_parent[16];
                    if (!inverse_affine(&global[p * 16], inv_parent)) { error = "singular pose in " + c.name; return false; }
                    mul(inv_parent, &global[i * 16], local);
                } else std::memcpy(local, &global[i * 16], sizeof(local));
                float *dst = &rig.locals[(size_t(first + f) * n + i) * 16];
                conj(local, dst);
                quantize16(dst);
                // Torque decomposes node matrices (rotation, translation, scale): log how far they are from rigid.
                const float det = dst[0] * (dst[5] * dst[10] - dst[6] * dst[9]) - dst[1] * (dst[4] * dst[10] - dst[6] * dst[8]) +
                                  dst[2] * (dst[4] * dst[9] - dst[5] * dst[8]);
                out.min_det = std::min(out.min_det, det);
                float col[3][3];
                for (int a = 0; a < 3; ++a) for (int r = 0; r < 3; ++r) col[a][r] = dst[r * 4 + a];
                for (int a = 0; a < 3; ++a) {
                    const float len = std::sqrt(col[a][0] * col[a][0] + col[a][1] * col[a][1] + col[a][2] * col[a][2]);
                    out.max_scale = std::max(out.max_scale, std::fabs(len - 1));
                    for (int b = a + 1; b < 3; ++b) {
                        const float lb = std::sqrt(col[b][0] * col[b][0] + col[b][1] * col[b][1] + col[b][2] * col[b][2]);
                        const float dot = col[a][0] * col[b][0] + col[a][1] * col[b][1] + col[a][2] * col[b][2];
                        if (len > 1e-6f && lb > 1e-6f) out.max_shear = std::max(out.max_shear, std::fabs(dot) / (len * lb));
                    }
                }
            }
            const float *g = &global[probe * 16];
            const float *b = &bind[probe * 16];
            out.root_motion = std::max(out.root_motion, std::sqrt((g[3] - b[3]) * (g[3] - b[3]) + (g[11] - b[11]) * (g[11] - b[11])));
        }
        first += out.frames;
        rig.clips.push_back(out);
    }
    // A joint is animated when any frame moves it off its bind local; the others hold <matrix>.
    for (size_t i = 0; i < n; ++i)
        for (uint32_t f = 0; f < total && !rig.joints[i].animated; ++f)
            rig.joints[i].animated = max_diff(rig.local(f, i), rig.joints[i].bind_local) > kStaticLimit;
    // Self-check on the numbers as written: chain the locals, rebuild the skin, compare with C S C.
    for (size_t k = 0; k < clips.size(); ++k) {
        RigClip &out = rig.clips[k];
        for (uint32_t f = 0; f < out.frames; ++f) {
            const float *skin = clips[k].clip.frame(f);
            for (size_t i = 0; i < n; ++i) {
                const RigJoint &j = rig.joints[i];
                const float *local = j.animated ? rig.local(out.first + f, i) : j.bind_local;
                if (j.parent >= 0) mul(&global[j.parent * 16], local, &global[i * 16]);
                else std::memcpy(&global[i * 16], local, 64);
                float s4[16], expect[16], got[16];
                from_3x4(skin + i * 12, s4);
                conj(s4, expect);
                mul(&global[i * 16], j.inverse_bind, got);
                out.check = std::max(out.check, max_diff(got, expect));
            }
        }
        if (!(out.check <= kSelfCheckLimit)) {
            char b[128]; std::snprintf(b, sizeof(b), "self-check failed on clip %s (max error %.3g)", out.name.c_str(), out.check);
            error = b; return false;
        }
    }
    return true;
}

} // namespace sonicnative
