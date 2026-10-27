// SPDX-License-Identifier: MIT
// Boost effects for the native mode (natif plan fx, component A): Frontiers' aura shell as extra skinned meshes
// of Sonic, the effect textures (BC blocks copied as they are, or small uncompressed textures baked here from
// Frontiers' masks or computed), the materials of the aura and of the effect quads Lua draws, and the bone
// tracks the lightning on Sonic's body follows. Nothing here is Frontiers content until it runs on the player's PC.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/frontiers/model.hpp"
#include "../../src/frontiers/anim.hpp"
#include "sonic_data.hpp"
#include "skin_math.hpp"
#include "dds_patch.hpp"
#include "text.hpp"

namespace sonicnative {

// ---- textures ----

// An 8-bit RGBA PNG from w x h RGBA texels in 0..1 (BeamNG makes the mips). Not a DDS: BeamNG 0.40 refuses
// uncompressed DDS ("DDSFile::readHeader - only RGB formats are supported"), so those textures never loaded
// and the quads drawing them showed as plain grey squares. Stored (uncompressed) deflate blocks: no zlib needed.
inline std::vector<uint8_t> png_rgba8(uint32_t w, uint32_t h, const std::vector<float> &rgba)
{
    uint32_t table[256];
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    auto be32 = [](std::vector<uint8_t> &v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(out, uint32_t(data.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t crc = 0xffffffffu;
        for (size_t i = start; i < out.size(); ++i) crc = table[(crc ^ out[i]) & 0xff] ^ (crc >> 8);
        be32(out, crc ^ 0xffffffffu);
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});              // 8 bits, RGBA, deflate, no filter method, no interlace
    chunk("IHDR", ihdr);
    auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    std::vector<uint8_t> raw;                                // each row: filter 0, then the texels
    raw.reserve(size_t(h) * (1 + size_t(w) * 4));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) raw.push_back(byte(rgba[(size_t(y) * w + x) * 4 + c]));
    }
    std::vector<uint8_t> z = {0x78, 0x01};                   // zlib header, then stored blocks of at most 65535 bytes
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        for (size_t i = at; i < at + n; ++i) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; z.push_back(raw[i]); }
        at += n;
        if (n == 0) break;
    }
    be32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

// Mip 0 of a BC5 texture as max(R, G) per texel in 0..1: each 16-byte block is two BC4 blocks (R, then G).
// That is what the overlay's psFlat reads from it (max of rgb, alpha 1).
inline bool bc5_mask(const frontiers::Texture &t, std::vector<float> &mask, std::string &error)
{
    if (t.dxgi != 83 || !t.compressed) { error = "not BC5_UNORM"; return false; }
    const uint32_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
    if (t.data.size() < size_t(bw) * bh * 16) { error = "BC5 mip 0 short"; return false; }
    mask.assign(size_t(t.width) * t.height, 0.0f);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t *b = t.data.data() + (size_t(by) * bw + bx) * 16;
            int r[16], g[16];
            bc4::decode(b, r);
            bc4::decode(b + 8, g);
            for (int k = 0; k < 16; ++k) {
                const uint32_t x = bx * 4 + k % 4, y = by * 4 + k / 4;
                if (x < t.width && y < t.height) mask[size_t(y) * t.width + x] = std::max(r[k], g[k]) / 255.0f;
            }
        }
    return true;
}

// Bilinear sample with wrap addressing, (u, v) in texture space (0..1 covers the image once).
inline float sample_wrap(const std::vector<float> &m, uint32_t w, uint32_t h, float u, float v)
{
    const float x = u * float(w) - 0.5f, y = v * float(h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y), ax = x - fx, ay = y - fy;
    auto at = [&](int i, int j) {
        i %= int(w); if (i < 0) i += int(w);
        j %= int(h); if (j < 0) j += int(h);
        return m[size_t(j) * w + size_t(i)];
    };
    const int i = int(fx), j = int(fy);
    return (at(i, j) * (1 - ax) + at(i + 1, j) * ax) * (1 - ay) + (at(i, j + 1) * (1 - ax) + at(i + 1, j + 1) * ax) * ay;
}

// The boost flash glow (ring false) or shock ring (ring true) of the overlay's psFlat modes 2 and 3, baked:
//   r = |uv * 2 - 1|, s = (1 - r)^2.5 (glow) or (1 - |r - 0.72| / 0.2)^2 (ring), saturated,
//   detail = 0.55 + 0.9 * mask(uv * 2, wrap), value = (1 + 0.5 s)^2.2 * min(1, s * detail) / kFlashPeak,
// in every channel but alpha (1): the overlay's colour lift (1 + 0.5 s) is inside output(), so linear it is ^2.2.
// The material multiplies kFlashPeak back.
inline std::vector<uint8_t> flash_texture(const std::vector<float> &mask, uint32_t mw, uint32_t mh, bool ring, uint32_t size = 256)
{
    std::vector<float> rgba(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float r = std::sqrt((2 * u - 1) * (2 * u - 1) + (2 * v - 1) * (2 * v - 1));
            const float s = ring ? std::pow(std::clamp(1 - std::fabs(r - 0.72f) / 0.2f, 0.0f, 1.0f), 2.0f)
                                 : std::pow(std::clamp(1 - r, 0.0f, 1.0f), 2.5f);
            const float detail = 0.55f + 0.9f * sample_wrap(mask, mw, mh, u * 2, v * 2);
            const float value = std::pow(1 + 0.5f * s, sonic_data::kGamma) * std::min(1.0f, s * detail) / sonic_data::kFlashPeak;
            float *c = &rgba[(size_t(y) * size + x) * 4];
            c[0] = c[1] = c[2] = value; c[3] = 1;
        }
    return png_rgba8(size, size, rgba);
}

// The boost beam's soft edge (psFlat mode 5): (1 - |2u - 1|)^1.5 across the ribbon (u), the same along it. In every
// channel: BeamNG's opacityMap reads red (defaultMat.hlsl), the overlay read alpha.
inline std::vector<uint8_t> beam_texture(uint32_t w = 256, uint32_t h = 4)
{
    std::vector<float> rgba(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w;
            const float a = std::pow(std::clamp(1 - std::fabs(2 * u - 1), 0.0f, 1.0f), 1.5f);
            float *c = &rgba[(size_t(y) * w + x) * 4];
            c[0] = c[1] = c[2] = c[3] = a;
        }
    return png_rgba8(w, h, rgba);
}

// --locus-fallback: soft vertical streaks standing in for Frontiers' locus01 (BC6H) if BeamNG cannot draw that
// format: a few dozen lines across u, each breaking up along v (whole periods, so it tiles both ways). Computed
// here, no Frontiers content.
inline std::vector<uint8_t> locus_fallback_texture(uint32_t size = 256)
{
    uint32_t seed = 0x51ce7a11u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
    struct Streak { float u, width, amp, periods, phase; };
    std::vector<Streak> streaks(28);
    for (auto &k : streaks) {
        k.u = rnd(); k.width = 0.004f + 0.02f * rnd(); k.amp = 0.35f + 0.65f * rnd();
        k.periods = float(1 + int(rnd() * 3)); k.phase = rnd();
    }
    const float two_pi = 6.28318531f;
    std::vector<float> rgba(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            float value = 0;
            for (auto &k : streaks) {
                float du = std::fabs(u - k.u);
                du = std::min(du, 1 - du);
                const float across = std::exp(-(du / k.width) * (du / k.width));
                const float along = 0.55f + 0.45f * std::sin(two_pi * (k.periods * v + k.phase));
                value += k.amp * across * along;
            }
            float *c = &rgba[(size_t(y) * size + x) * 4];
            c[0] = c[1] = c[2] = std::min(1.0f, value); c[3] = 1;
        }
    return png_rgba8(size, size, rgba);
}

// ---- the aura shell ----

// Frontiers' aura model has Sonic's nodes in the same order with the same inverse binds (node 0 is each model's
// own root, chr_sonic_aura vs chr_sonic), so its joint indices are Sonic's. Node 0 may differ only if no vertex
// is weighted on it.
inline bool same_skeleton(const frontiers::Model &aura, const frontiers::Model &model, std::string &why)
{
    if (aura.nodes.size() != model.nodes.size()) { why = "node count " + std::to_string(aura.nodes.size()) + " vs " + std::to_string(model.nodes.size()); return false; }
    for (size_t i = 0; i < aura.nodes.size(); ++i) {
        const float d = max_diff(aura.nodes[i].inverse_bind, model.nodes[i].inverse_bind);
        if (i && aura.nodes[i].name != model.nodes[i].name) { why = "node " + std::to_string(i) + " is " + aura.nodes[i].name + ", not " + model.nodes[i].name; return false; }
        if (i && !(d < 1e-6f)) { why = "inverse bind of " + aura.nodes[i].name + " differs"; return false; }
        if (!i && !(d < 1e-6f))
            for (auto &v : aura.vertices)
                for (int k = 0; k < 4; ++k)
                    if (v.weights[k] && v.joints[k] == 0) { why = "a vertex is weighted on the differing root node"; return false; }
    }
    return true;
}

// One aura layer as a skinned mesh of Sonic's rig: each vertex pushed along its (normalised) bind normal by
// layer.push model units, the overlay's vsAura push done once in bind space (exact up to the skinning blend),
// and its UVs scaled by layer.uv_scale. Only the shell's first mesh is used (Frontiers' has one).
inline bool build_aura_layer(const frontiers::Model &aura, const sonic_data::AuraLayer &layer, RigMesh &out, std::string &error)
{
    frontiers::Model m = aura;
    for (auto &v : m.vertices) {
        const float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
        if (len > 1e-6f) for (int a = 0; a < 3; ++a) v.pos[a] += v.normal[a] / len * layer.push;
        v.uv[0] *= layer.uv_scale;
        v.uv[1] *= layer.uv_scale;
    }
    return build_mesh(m, 0, MeshNaming{layer.object, layer.material}, true, out, error);
}

// ---- materials ----

inline std::string json_vec(const float *v, int n)
{
    std::string s = "[";
    for (int i = 0; i < n; ++i) s += (i ? ", " : "") + json_num(v[i]);
    return s + "]";
}

// A gamma colour (the overlay's) made linear and scaled.
inline std::string linear_colour(const float *gamma, float scale)
{
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = std::pow(gamma[i], sonic_data::kGamma) * scale;
    return json_vec(c, 3);
}

// An unlit, translucent effect material: black base colour, emissive (display-relative, the same by day and by
// night: emissiveIgnoreExposure with emissiveIntensityNits 1, so emissiveFactor 1 is about white), into the glow
// buffer unless glow is false, no depth write, no shadow. blend: AddAlpha (added onto the scene) or LerpAlpha
// (blended over it).
inline std::string effect_material(const std::string &name, const std::string &stage, const char *blend, bool double_sided, bool glow = true)
{
    std::string s = "  " + json_escape(name) + ": {\n    \"name\": " + json_escape(name) + ", \"mapTo\": " + json_escape(name) +
                    ", \"class\": \"Material\", \"version\": 1.5,\n    \"Stages\": [{\"baseColorFactor\": [0, 0, 0, 1], \"emissive\": true, " +
                    "\"emissiveIntensityNits\": 1, \"emissiveIgnoreExposure\": true, \"glow\": " + (glow ? "true" : "false") + ", " + stage + "}, {}, {}, {}],\n" +
                    "    \"translucent\": true, \"translucentBlendOp\": \"" + blend + "\", \"translucentZWrite\": false, \"castShadows\": false";
    if (double_sided) s += ", \"doubleSided\": true";
    return s + "\n  }";
}

// The aura layer's material at brightness `level` (its colour x its share x level), with its scrolling pattern.
// Single-sided: the shell is inside-out, so only its far half is drawn, and Sonic's body hides it but at the rim.
inline std::string aura_material(const std::string &name, const sonic_data::AuraLayer &layer, float level, const std::string &texture)
{
    const float dir[2] = {layer.scroll_u, layer.scroll_v};
    const std::string stage = "\"emissiveFactor\": " + linear_colour(sonic_data::kAuraColour, layer.alpha * level) +
                              ", \"emissiveMap\": " + json_escape(texture) + ", \"animFlags\": 1, \"scrollDir\": " + json_vec(dir, 2) +
                              ", \"scrollSpeed\": " + json_num(layer.scroll_speed);
    return effect_material(name, stage, "AddAlpha", false);
}

// An aura layer's ladder material name: base_<rest> -> lNN_<rest> (what setSkinName("lNN") asks for).
inline std::string aura_level_name(const char *base, int level)
{
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "l%02d", level);
    const std::string b = base;
    return prefix + (b.compare(0, 4, "base") == 0 ? b.substr(4) : "_" + b);
}

// Every level of one effect kind: sonicnative_fx_<kind>_<k>, k = 1..kFxLevels. Additive kinds: emissive =
// colour x gain x alpha_k through the kind's texture. The beam: its colour blended over the scene by
// opacity min(1, alpha_k) x its edge texture.
inline std::string effect_kind_materials(const sonic_data::FxKind &kind, const std::string &texture)
{
    std::string s;
    for (int k = 1; k <= sonic_data::kFxLevels; ++k) {
        const std::string name = std::string("sonicnative_fx_") + kind.kind + "_" + std::to_string(k);
        const float a = sonic_data::fx_alpha(k);
        std::string stage;
        if (kind.lerp)
            stage = "\"emissiveFactor\": " + linear_colour(kind.colour, kind.gain) + ", \"opacityMap\": " + json_escape(texture) +
                    ", \"opacityFactor\": " + json_num(std::min(1.0f, a));
        else
            stage = "\"emissiveFactor\": " + linear_colour(kind.colour, kind.gain * a) + ", \"emissiveMap\": " + json_escape(texture);
        s += (s.empty() ? "" : ",\n") + effect_material(name, stage, kind.lerp ? "LerpAlpha" : "AddAlpha", true, kind.glow);
    }
    return s;
}

// ---- bone tracks ----

// A position in model units, 0.1 mm steps, no trailing zeros.
inline std::string num4(float v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.4f", double(v));
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

// Model-space positions of some joints at every kept frame of every clip, on the shape's own axes (BeamNG object
// space: Z up, +Y forward) in model units: p = C (S_f o), S_f the baked skin of the frame, o the joint's bind
// position, C the axis change (skin_math.hpp). Frames as the DAE keeps them (a loop drops its closing key), so
// frame = phase * (frames - 1) lines up with the shape's own sequence. found: the joints written, in order.
inline std::string bones_json(const frontiers::Model &model, const std::vector<ClipSource> &clips, const Rig &rig,
                              std::vector<std::string> &found, std::vector<std::string> &missing)
{
    std::vector<size_t> node;
    std::vector<float> origin;
    for (const char *name : sonic_data::kFxBones) {
        int at = -1;
        for (size_t n = 0; n < model.nodes.size(); ++n) if (model.nodes[n].name == name) at = int(n);
        float bind[16];
        if (at < 0 || !frontiers::math4::inverse_affine(model.nodes[size_t(at)].inverse_bind, bind)) { missing.push_back(name); continue; }
        found.push_back(name);
        node.push_back(size_t(at));
        origin.insert(origin.end(), {bind[3], bind[7], bind[11]});
    }
    std::string s = "{\n  \"version\": 1,\n";
    s += "  \"about\": \"Written by sonicnative_convert. Model-space joint positions (the shape's axes: Z up, +Y forward; model units, "
         "x the object's scale for meters) at every key of every clip. Bone b at frame f of a clip: pos[(f * bones + b) * 3 + 0..2]; "
         "frame = phase * (frames - 1).\",\n";
    s += "  \"bones\": [";
    for (size_t b = 0; b < found.size(); ++b) s += (b ? ", " : "") + json_escape(found[b]);
    s += "],\n  \"clips\": {";
    for (size_t k = 0; k < clips.size() && k < rig.clips.size(); ++k) {
        const frontiers::Clip &clip = clips[k].clip;
        const RigClip &rc = rig.clips[k];
        s += k ? ",\n" : "\n";
        s += "    " + json_escape(rc.name) + ": {\"frames\": " + std::to_string(rc.frames) + ", \"loop\": " + (rc.loop ? "true" : "false") + ", \"pos\": [";
        for (uint32_t f = 0; f < rc.frames && f < clip.frames; ++f) {
            const float *skin = clip.frame(f);
            s += f ? ",\n      " : "\n      ";
            for (size_t b = 0; b < node.size(); ++b) {
                const float *m = skin + node[b] * 12, *o = &origin[b * 3];
                float p[3];
                for (int r = 0; r < 3; ++r) p[r] = m[r * 4] * o[0] + m[r * 4 + 1] * o[1] + m[r * 4 + 2] * o[2] + m[r * 4 + 3];
                s += (b ? ", " : "") + num4(-p[0]) + ", " + num4(p[2]) + ", " + num4(p[1]);
            }
        }
        s += "]}";
    }
    s += "\n  }\n}\n";
    return s;
}

} // namespace sonicnative
