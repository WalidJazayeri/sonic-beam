// SPDX-License-Identifier: MIT
// What Sonic Beam's D3D11 and D3D12 renderers share: the HLSL, the per-draw constants,
// the flat (shadow / HUD text) vertex and the text-to-quads builder.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "font.hpp"
#include "game.hpp"

namespace draw {

inline const char kShader[] = R"(
cbuffer Draw : register(b0) { row_major float4x4 modelView; row_major float4x4 proj; float4 tint; float4 params; };
cbuffer Bones : register(b1) { float4 bones[768]; };
Texture2D tex : register(t0);
SamplerState wrapS : register(s0);
SamplerState clampS : register(s1);
SamplerState pointS : register(s2);
struct VSIn { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; uint4 j : BLENDINDICES; float4 w : BLENDWEIGHT; };
struct PSIn { float4 pos : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD0; float3 v : TEXCOORD1; };
float3 skinP(float4 p, uint j) { return float3(dot(bones[j * 3], p), dot(bones[j * 3 + 1], p), dot(bones[j * 3 + 2], p)); }
float3 skinN(float3 n, uint j) { return float3(dot(bones[j * 3].xyz, n), dot(bones[j * 3 + 1].xyz, n), dot(bones[j * 3 + 2].xyz, n)); }
float3 output(float3 rgb) {
    if (params.w > 1.5) return pow(max(rgb, 0), 2.2);             // sRGB target: the view encodes
    if (params.w > 0.5) return pow(max(rgb, 0), 2.2) * 2.5;       // scRGB HDR target
    return rgb;
}
PSIn vsMesh(VSIn i) {
    float4 p = float4(i.p, 1); float3 sp = 0, sn = 0;
    [unroll] for (int k = 0; k < 4; k++) { float w = i.w[k]; if (w > 0) { sp += w * skinP(p, i.j[k]); sn += w * skinN(i.n, i.j[k]); } }
    float4 vp = mul(float4(sp, 1), modelView);
    PSIn o; o.v = vp.xyz; o.pos = mul(vp, proj); o.n = mul(float4(sn, 0), modelView).xyz; o.uv = i.uv; return o;
}
float4 psMesh(PSIn i) : SV_Target {
    float4 c = params.y > 0.5 ? tex.Sample(clampS, i.uv) : tex.Sample(wrapS, i.uv);
    clip(c.a - 0.5);
    float3 n = normalize(i.n + 1e-6);
    float3 L = normalize(float3(0.35, 0.85, -0.4));
    float ndl = dot(n, L);
    float3 V = normalize(-i.v);
    float rim = pow(1 - saturate(abs(dot(n, V))), 3) * 0.3;
    float light = 0.55 + 0.6 * saturate(ndl) + rim + params.z;
    return float4(output(c.rgb * tint.rgb * light), tint.a);
}
struct FIn { float3 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR; };
struct FOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR; };
FOut vsFlat(FIn i) { FOut o; o.pos = mul(mul(float4(i.p, 1), modelView), proj); o.uv = i.uv; o.c = i.c; return o; }
float4 psFlat(FOut i) : SV_Target {
    float a;
    if (params.x > 1.5) {                       // boost flash (2) / shock wave (3), drawn additive
        float r = length(i.uv * 2 - 1);
        float shape = params.x > 2.5 ? pow(saturate(1 - abs(r - 0.72) / 0.2), 2) : pow(saturate(1 - r), 2.5);
        float4 t = tex.Sample(wrapS, i.uv * 2);   // Frontiers' effect texture gives the detail
        float detail = 0.55 + 0.9 * max(t.r, max(t.g, t.b)) * t.a;
        return float4(output(i.c.rgb * (1 + 0.5 * shape)), saturate(i.c.a * shape * detail));
    }
    if (params.x > 0.5) { float d = saturate(1 - length(i.uv * 2 - 1)); a = d * d; }
    else a = tex.Sample(pointS, i.uv).r;
    return float4(output(i.c.rgb), i.c.a * a);
}
// Boost aura, after Frontiers' SonicAura material: Sonic's aura shell on his own pose, brighter
// toward the silhouette (falloff) with the aura texture streaming over it. Drawn additive.
// params.x: push along the normal (model units), params.y: time; tint: colour and brightness.
PSIn vsAura(VSIn i) {
    float4 p = float4(i.p, 1); float3 sp = 0, sn = 0;
    [unroll] for (int k = 0; k < 4; k++) { float w = i.w[k]; if (w > 0) { sp += w * skinP(p, i.j[k]); sn += w * skinN(i.n, i.j[k]); } }
    sp += normalize(sn + 1e-6) * params.x;
    float4 vp = mul(float4(sp, 1), modelView);
    PSIn o; o.v = vp.xyz; o.pos = mul(vp, proj); o.n = mul(float4(sn, 0), modelView).xyz; o.uv = i.uv; return o;
}
float4 psAura(PSIn i) : SV_Target {
    float3 n = normalize(i.n + 1e-6);
    float3 V = normalize(-i.v);
    float falloff = pow(1 - saturate(abs(dot(n, V))), 1.6);
    float4 a1 = tex.Sample(wrapS, i.uv * 1.0 + float2(0.0, -params.y * 1.3));
    float4 a2 = tex.Sample(wrapS, i.uv * 1.7 + float2(params.y * 0.4, -params.y * 2.1));
    float energy = max(a1.r, max(a1.g, a1.b)) * a1.a * 0.6 + max(a2.r, max(a2.g, a2.b)) * a2.a * 0.6;
    float a = saturate(falloff * (0.25 + energy)) * tint.a;
    return float4(output(tint.rgb * (0.8 + 0.7 * energy)), a);
}
// Speed blur (Frontiers' boost): the finished BeamNG frame streaks toward Sonic. tint.xy: centre
// (0..1), tint.z: longest streak. The centre stays sharp; BeamNG's minimap corner is kept sharp.
struct POut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
POut vsPost(uint id : SV_VertexID) {
    POut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
float4 psPost(POut i) : SV_Target {
    float2 d = i.uv - tint.xy;
    float amount = tint.z * smoothstep(0.08, 0.55, length(d));
    // BeamNG's own UI stays sharp: minimap (bottom left) and toolbar (top right)
    float map = min(0.19 - i.uv.x, i.uv.y - 0.64), bar = min(i.uv.x - 0.76, 0.08 - i.uv.y);
    amount *= (1 - smoothstep(0.0, 0.03, map) * step(0, map)) * (1 - smoothstep(0.0, 0.02, bar) * step(0, bar));
    float3 acc = 0;
    [unroll] for (int k = 0; k < 16; k++) acc += tex.SampleLevel(clampS, i.uv - d * amount * (k / 15.0), 0).rgb;
    return float4(acc / 16, 1);
}
)";

struct DrawConstants { float model_view[16], proj[16], tint[4], params[4]; };
struct FlatVertex { float p[3], uv[2], c[4]; };

inline const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

inline void mul(const float *a, const float *b, float *o)
{
    float t[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
        t[i * 4 + j] = s;
    }
    std::memcpy(o, t, sizeof(t));
}

inline std::vector<float> identity_bones()
{
    std::vector<float> b(768 * 4, 0.0f);
    for (int i = 0; i < 256; ++i) { b[i * 12] = 1; b[i * 12 + 5] = 1; b[i * 12 + 10] = 1; }
    return b;
}

inline std::vector<FlatVertex> shadow_vertices(const game::Scene &scene)
{
    std::vector<FlatVertex> v;
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k : {0, 1, 2, 0, 2, 3}) {
        const float *p = scene.shadow_corners[k];
        v.push_back({{p[0], p[1], p[2]}, {uv[k][0], uv[k][1]}, {0, 0, 0, 0.45f}});
    }
    return v;
}

// Camera-facing quads for the boost flash and shock wave (world space, drawn with the view).
inline std::vector<FlatVertex> sprite_vertices(const game::Scene &scene, int kind)
{
    std::vector<FlatVertex> v;
    // view rows hold the camera axes: column 0 is right, column 1 is up (row-vector matrices)
    const float right[3] = {scene.view[0], scene.view[4], scene.view[8]}, up[3] = {scene.view[1], scene.view[5], scene.view[9]};
    for (auto &s : scene.sprites) {
        if (s.kind != kind) continue;
        const float c = std::cos(s.spin) * s.size * 0.5f, sn = std::sin(s.spin) * s.size * 0.5f;
        const float color[4] = {0.45f, 0.75f, 1.0f, s.alpha};
        const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        FlatVertex q[4];
        for (int k = 0; k < 4; ++k) {
            const float x = corner[k][0] * c - corner[k][1] * sn, y = corner[k][0] * sn + corner[k][1] * c;
            for (int a = 0; a < 3; ++a) q[k].p[a] = s.pos[a] + right[a] * x + up[a] * y;
            q[k].uv[0] = corner[k][0] * 0.5f + 0.5f; q[k].uv[1] = 0.5f - corner[k][1] * 0.5f;
            std::memcpy(q[k].c, color, sizeof(color));
        }
        for (int k : {0, 1, 2, 0, 2, 3}) v.push_back(q[k]);
    }
    return v;
}

inline std::vector<FlatVertex> text_vertices(const game::Scene &scene, float aspect)
{
    std::vector<FlatVertex> v;
    for (auto &t : scene.texts) {
        const float h = t.size, w = h * 5.0f / 7.0f / std::max(0.5f, aspect), advance = w * 1.25f;
        float x = t.x;
        for (char ch : t.text) {
            const int g = font::index_of(ch);
            if (g >= 0) {
                const float u0 = float((g % font::columns) * font::cell_w) / font::atlas_w, v0 = float((g / font::columns) * font::cell_h) / font::atlas_h;
                const float u1 = u0 + 5.0f / font::atlas_w, v1 = v0 + 7.0f / font::atlas_h;
                const FlatVertex q[4] = {{{x, t.y, 0.5f}, {u0, v0}, {t.color[0], t.color[1], t.color[2], t.color[3]}},
                                         {{x + w, t.y, 0.5f}, {u1, v0}, {t.color[0], t.color[1], t.color[2], t.color[3]}},
                                         {{x + w, t.y - h, 0.5f}, {u1, v1}, {t.color[0], t.color[1], t.color[2], t.color[3]}},
                                         {{x, t.y - h, 0.5f}, {u0, v1}, {t.color[0], t.color[1], t.color[2], t.color[3]}}};
                for (int k : {0, 1, 2, 0, 2, 3}) v.push_back(q[k]);
            }
            x += advance;
        }
    }
    return v;
}

} // namespace draw
