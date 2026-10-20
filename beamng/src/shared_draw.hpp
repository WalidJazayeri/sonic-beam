// SPDX-License-Identifier: MIT
// What Sonic Beam's D3D11 and D3D12 renderers share: the HLSL, the per-draw constants,
// the flat (shadow / HUD text) vertex and the text-to-quads builder.
#pragma once
#include <algorithm>
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
    if (params.x > 0.5) { float d = saturate(1 - length(i.uv * 2 - 1)); a = d * d; }
    else a = tex.Sample(pointS, i.uv).r;
    return float4(output(i.c.rgb), i.c.a * a);
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
