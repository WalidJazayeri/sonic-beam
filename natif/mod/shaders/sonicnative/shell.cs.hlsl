// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: the boost aura's shell, the overlay's default aura shape (PostEffect
// sonicnative_shell, made by lua/ge/extensions/sonicnative/shell.lua). The overlay drew a bullet mesh
// lying along Sonic's run in three nested layers, additive, brighter toward each layer's silhouette,
// with Frontiers' aura pattern streaming back over it (beamng/src/assets.cpp dome, game.cpp 650-668,
// shared_draw.hpp vsAura / psAura and kDomeScale / kDomeAlpha, v0.12.3). A BeamNG material has no view
// dependent term, so here each pixel's eye ray meets the three bullets themselves: a nose (half an
// ellipsoid, radius R across, N along the run) and a tail flaring from R to 1.15 R over its length,
// open at the back; front and back faces both add, as the overlay drew them without culling. Run after
// the tonemapper, so the colour adds onto the finished image as the overlay's did. As in the overlay,
// Sonic's body hides the shell behind him (a pixel inside sonicShellBody's capsule is his); the rest of
// the scene hides a hit only when it is nearer by more than sonicShellTail.z (walls, cars), so the
// ground under him never cuts the shell's lower half. Laid out like speedBlur.cs.hlsl; the distance
// comes from the depth as motionBlurPrepare.cs.hlsl does.
//   texture 0        $backBuffer
//   texture 1        #prepass[Depth]
//   texture 2        Frontiers' aura pattern (the converter's tex/sonicnative_fx_aura.dds), wrapped
//   sonicShellBase   xyz the bullets' base point (world), w the aura's brightness (0: nothing drawn)
//   sonicShellDir    xyz along the run, w N (the nose's length, layer 0)
//   sonicShellUp     xyz up, square to the run, w R (the radius, layer 0)
//   sonicShellTail   x the tail's length (layer 0), y the pattern's clock, z hiding margin (m), w add mode
//                    (0: onto the image as it is; 1: the image is linear, add in gamma)
//   sonicShellBound  xyz centre, w radius of a sphere holding the three layers (pixels outside: unchanged)
//   sonicShellTint   rgb the aura's colour (gamma), w unused
//   sonicShellGlow   xyz the jump ball's middle, w its glow's alpha (0: none); the overlay's soft round blue
//                    glow round the ball (game.cpp 422-424: a camera-facing quad, psFlat mode 1, (1 - r)^2)
//   sonicShellGlowTint rgb its colour (gamma), w its radius (m)
//   sonicShellBody   xyz the bottom of a capsule around Sonic's body (world), w its radius
//   sonicShellBodyUp xyz the capsule's axis (his up), w its length
// The layer numbers are the overlay's (shared_draw.hpp kDomeScale, kDomeAlpha; render11.cpp: the tail
// x (1 + 0.12 layer), the clock + 0.37 layer). The rest comes from lua/common/sonicnative/effects.lua (shell).
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(inputTex, 0);
uniform_sampler2D(depth, 1);
uniform_sampler2D(aura, 2);

cbuffer perDraw : register(b0)
{
    uniform float3 eyePosWorld;
    uniform float4 sonicShellBase;
    uniform float4 sonicShellDir;
    uniform float4 sonicShellUp;
    uniform float4 sonicShellTail;
    uniform float4 sonicShellBound;
    uniform float4 sonicShellTint;
    uniform float4 sonicShellGlow;
    uniform float4 sonicShellGlowTint;
    uniform float4 sonicShellBody;
    uniform float4 sonicShellBodyUp;
    POSTFX_UNIFORMS
};

#include "shaders/common/postFx/postFx.hlsl"
#include "shaders/common/postFx/postFxCompute.h.hlsl"

static const float LAYER_SCALE[3] = {1.0, 1.13, 1.27};
static const float LAYER_ALPHA[3] = {0.8, 0.5, 0.3};
static const float TAIL_FLARE = 0.15;
static const float FAR_DISTANCE = 60000.0;

int2 texelOf(float2 uv, uint2 size)
{
    return clamp(int2(uv * float2(size)), int2(0, 0), int2(size) - int2(1, 1));
}

// psAura on one hit: falloff toward the silhouette, the pattern sampled twice, the nose bright and
// the tail fading out (v from 0 at the tip to 2 at the tail's end)
float3 shade(float3 n, float3 d, float2 uv, float tau, float strength)
{
    float falloff = pow(1.0 - saturate(abs(dot(n, -d))), 1.6);
    float4 a1 = auraTex.SampleLevel(aura_sampler_, uv + float2(0.0, -tau * 1.3), 0);
    float4 a2 = auraTex.SampleLevel(aura_sampler_, uv * 1.7 + float2(tau * 0.4, -tau * 2.1), 0);
    float energy = max(a1.r, max(a1.g, a1.b)) * a1.a * 0.6 + max(a2.r, max(a2.g, a2.b)) * a2.a * 0.6;
    float a = saturate(falloff * (0.25 + energy)) * strength * (1.0 - smoothstep(0.35, 2.0, uv.y));
    return a * sonicShellTint.rgb * (0.75 + 0.45 * energy);
}

// the pattern's u around the run: two wraps, as the overlay's mesh (u = 2 j / 48)
float aroundU(float x, float y)
{
    float th = atan2(y, x);
    return (th < 0.0 ? th + 2.0 * PI : th) / PI;
}

PFX_NUMTHREADS
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 storePos;
    if (!postFxComputeThread(dispatchThreadID, storePos))
    {
        return;
    }

    PFXVertToPix IN = processPostFxCompute(dispatchThreadID);
    float4 base = tex2D(inputTex, IN.uv0);
    float strength = sonicShellBase.w;
    float3 d = normalize(IN.wsEyeRay);
    float3 o = eyePosWorld;

    // outside the sphere holding the shell: nothing to add
    float3 oc = o - sonicShellBound.xyz;
    float bb = dot(oc, d);
    float bc = dot(oc, oc) - sonicShellBound.w * sonicShellBound.w;
    if ((strength <= 0.0 && sonicShellGlow.w <= 0.0) || (bc > 0.0 && (bb > 0.0 || bb * bb - bc < 0.0)))
    {
        pfxOut[storePos] = base;
        return;
    }

    // the scene's distance along this ray (the sky is far away)
    uint2 depthSize;
    depthTex.GetDimensions(depthSize.x, depthSize.y);
    float ndc = depthTex.Load(int3(texelOf(IN.uv1, depthSize), 0)).r;
    float sceneDistance = length(IN.wsEyeRay * realToEyeLinearDepth(ndcToRealDepth(ndc, projParams.zw), projParams));
    if (!(sceneDistance < FAR_DISTANCE))
    {
        sceneDistance = FAR_DISTANCE;
    }
    // Sonic's own pixel: the shell behind him is hidden at once; elsewhere only well in front of it
    float3 sp = o + d * sceneDistance - sonicShellBody.xyz;
    float onAxis = clamp(dot(sp, sonicShellBodyUp.xyz), 0.0, sonicShellBodyUp.w);
    bool isSonic = length(sp - sonicShellBodyUp.xyz * onAxis) < sonicShellBody.w;
    float limit = isSonic ? sceneDistance : sceneDistance + sonicShellTail.z;

    float3 dir = sonicShellDir.xyz;
    float3 up = sonicShellUp.xyz;
    float3 side = cross(dir, up);
    float3 ob = o - sonicShellBase.xyz;
    float3 ol = float3(dot(ob, side), dot(ob, up), dot(ob, dir));
    float3 dl = float3(dot(d, side), dot(d, up), dot(d, dir));

    float3 add = 0;

    // the jump ball's glow, over everything as the overlay drew its sprites
    float3 toBall = sonicShellGlow.xyz - o;
    float along = dot(toBall, d);
    if (sonicShellGlow.w > 0.0 && along > 0.0)
    {
        float g = saturate(1.0 - length(toBall - d * along) / max(sonicShellGlowTint.w, 0.01));
        add += sonicShellGlowTint.rgb * (g * g * sonicShellGlow.w);
    }

    [unroll] for (int layer = 0; layer < 3; layer++)
    {
        if (strength <= 0.0)
        {
            break;
        }
        float s = LAYER_SCALE[layer];
        float R = s * sonicShellUp.w;
        float N = s * sonicShellDir.w;
        float T = max(0.05, s * sonicShellTail.x * (1.0 + 0.12 * layer));
        float tau = sonicShellTail.y + 0.37 * layer;
        float k = strength * LAYER_ALPHA[layer];

        // the nose: the unit hemisphere scaled to R, R, N (z >= 0 along the run)
        float3 os = ol / float3(R, R, N);
        float3 ds = dl / float3(R, R, N);
        float qa = dot(ds, ds);
        float qb = dot(os, ds);
        float qc = dot(os, os) - 1.0;
        float disc = qb * qb - qa * qc;
        if (disc > 0.0)
        {
            float root = sqrt(disc);
            [unroll] for (int h = 0; h < 2; h++)
            {
                float t = (-qb + (h == 0 ? -root : root)) / qa;
                float3 m = os + ds * t;
                if (t > 0.0 && m.z >= 0.0 && t < limit)
                {
                    // the overlay's normal: the unit sphere's, turned by the bullet's matrix (not its inverse)
                    float3 n = normalize(side * (R * m.x) + up * (R * m.y) + dir * (N * m.z));
                    add += shade(n, d, float2(aroundU(m.x, m.y), 0.5 * (1.0 - m.z)), tau, k);
                }
            }
        }

        // the tail: radius R - (TAIL_FLARE R / T) z for z from 0 back to -T, open at its end
        float kk = TAIL_FLARE * R / T;
        float A = R - kk * ol.z;
        float B = kk * dl.z;
        float ta = dl.x * dl.x + dl.y * dl.y - B * B;
        float tb = ol.x * dl.x + ol.y * dl.y + A * B;
        float tc = ol.x * ol.x + ol.y * ol.y - A * A;
        float tdisc = tb * tb - ta * tc;
        if (abs(ta) > 1e-6 && tdisc > 0.0)
        {
            float root = sqrt(tdisc);
            [unroll] for (int h = 0; h < 2; h++)
            {
                float t = (-tb + (h == 0 ? -root : root)) / ta;
                float3 p = ol + dl * t;
                if (t > 0.0 && p.z < 0.0 && p.z >= -T && t < limit)
                {
                    float3 n = normalize(side * p.x + up * p.y);
                    add += shade(n, d, float2(aroundU(p.x, p.y), 0.5 + 1.5 * (-p.z / T)), tau, k);
                }
            }
        }
    }

    // a pixel nothing was added to stays exactly as it was (HDR output: values above 1 too)
    if (all(add <= 0.0))
    {
        pfxOut[storePos] = base;
        return;
    }
    float3 colour = base.rgb;
    if (sonicShellTail.w > 0.5)
    {
        // a linear image: the overlay added in display values (not clamped: HDR output goes past 1)
        colour = pow(max(pow(max(colour, 0.0), 1.0 / 2.2) + add, 0.0), 2.2);
    }
    else
    {
        colour = colour + add;
    }
    pfxOut[storePos] = float4(colour, base.a);
}
