// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: the motion blur that keeps Sonic sharp, pass 5 of 5
// (sonicnative_sharpBlurGather). Each pixel gathers the frame along the longest half blur around it,
// weighing every tap by depth and by how far each one's own blur reaches (McGuire et al. 2012,
// "A Reconstruction Filter for Plausible Motion Blur"): a pixel with no motion, like Sonic's, keeps
// its own colour, and its colour reaches no pixel around it either.
//   texture 0    $backBuffer
//   texture 1    #sonicnative_sharpVel: xy half blur (px), z distance (m), w 1 on Sonic
//   texture 2    #sonicnative_sharpNeighbor: xy the longest half blur around this pixel's tile (px)
//   target       $backBuffer
//   sonicGather  x the depth difference that still counts as one surface (share of the distance),
//                y 1: Sonic's pixels tinted (developer view of the mask), z developer views instead of
//                the frame: 1 each pixel's half blur, 2 the longest around its tile (white: 64 px),
//                3 the distance (white: 100 m)
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(inputColor, 0);
uniform_sampler2D(motion, 1);
uniform_sampler2D(neighbor, 2);

cbuffer perDraw : register(b0)
{
    uniform float4 sonicGather;
    POSTFX_UNIFORMS
};

#include "shaders/common/postFx/postFx.hlsl"
#include "shaders/common/postFx/postFxCompute.h.hlsl"

// taps per pixel at most (one per pixel of the longest blur, at least 4)
static const int MAX_TAPS = 16;
// pixels between a tap and the second depth read that gives its surface's slope
static const float SLOPE_STEP = 4.0;

int2 texelOf(float2 uv, uint2 size)
{
    return clamp(int2(uv * float2(size)), int2(0, 0), int2(size) - int2(1, 1));
}

// interleaved gradient noise (Jimenez 2014): a per-pixel offset of the taps, so too few of them
// read as grain rather than bands
float tapJitter(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// a blur of half length `reach` covers a point `dist` away, fading toward its end
float cone(float dist, float reach)
{
    return saturate(1.0 - dist / max(reach, 1e-4));
}

// the same, with a sharp end: both pixels' blurs reach each other
float cylinder(float dist, float reach)
{
    reach = max(reach, 1e-3);
    return 1.0 - smoothstep(0.95 * reach, 1.05 * reach, dist);
}

PFX_NUMTHREADS
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 storePos;
    if (!postFxComputeThread(dispatchThreadID, storePos))
    {
        return;
    }

    float2 uv = postFxComputeUv(dispatchThreadID);
    float4 base = tex2D(inputColor, viewportCoordToRenderTarget(uv, rtParams0));

    uint2 motionSize, neighborSize;
    motionTex.GetDimensions(motionSize.x, motionSize.y);
    neighborTex.GetDimensions(neighborSize.x, neighborSize.y);
    float4 centre = motionTex.Load(int3(texelOf(viewportCoordToRenderTarget(uv, rtParams1), motionSize), 0));
    float2 around = neighborTex.Load(int3(texelOf(viewportCoordToRenderTarget(uv, rtParams2), neighborSize), 0)).xy;
    float aroundLength = length(around);

    float3 colour = base.rgb;
    if (aroundLength >= 0.5)
    {
        float centreLength = length(centre.xy);
        int taps = clamp((int)ceil(aroundLength), 4, MAX_TAPS);
        float jitter = tapJitter(float2(storePos)) - 0.5;
        float2 stepUv = around / float2(motionSize);

        // the pixel itself counts more the less it moves (McGuire: 1 / its blur's length)
        float total = rcp(max(centreLength, 1.0));
        float3 sum = base.rgb * total;
        [loop] for (int i = 0; i < taps; i++)
        {
            float t = lerp(-1.0, 1.0, (float(i) + 0.5 + jitter) / float(taps));
            float2 tapUv = saturate(uv + stepUv * t);
            float4 tap = motionTex.Load(int3(texelOf(viewportCoordToRenderTarget(tapUv, rtParams1), motionSize), 0));
            float tapLength = length(tap.xy);
            float dist = abs(t) * aroundLength;

            // the tap's surface carried on to this pixel along its slope (read SLOPE_STEP pixels
            // farther out), never nearer than at the tap: the ground seen at a grazing angle lies
            // nearer than his shoe at its own pixel but behind it at the shoe's, and compared as it
            // is it would count as in front of the shoe and smear over it
            float2 outUv = saturate(tapUv + around * (sign(t) * SLOPE_STEP / aroundLength) / float2(motionSize));
            float outZ = motionTex.Load(int3(texelOf(viewportCoordToRenderTarget(outUv, rtParams1), motionSize), 0)).z;
            float tapZ = max(tap.z, tap.z + (tap.z - outZ) * (dist / SLOPE_STEP));

            // the tap in front of this pixel or behind it, softly within a share of the distance
            float soft = max(0.05, sonicGather.x * min(centre.z, tapZ));
            float front = saturate(1.0 - (tapZ - centre.z) / soft);
            float behind = saturate(1.0 - (centre.z - tapZ) / soft);

            // a tap in front blurs over this pixel as far as its own blur reaches; this pixel's own
            // blur shows what is behind it; on one surface both blurs must reach
            float w = front * cone(dist, tapLength) + behind * cone(dist, centreLength)
                + 2.0 * cylinder(dist, tapLength) * cylinder(dist, centreLength);
            sum += tex2D(inputColor, viewportCoordToRenderTarget(tapUv, rtParams0)).rgb * w;
            total += w;
        }
        colour = sum / total;
    }

    if (sonicGather.y > 0.5 && centre.w > 0.5)
    {
        colour = lerp(colour, float3(1.0, 0.1, 0.4), 0.5);
    }
    if (sonicGather.z > 0.5)
    {
        float shown = sonicGather.z < 1.5 ? length(centre.xy) / 64.0 : (sonicGather.z < 2.5 ? aroundLength / 64.0 : centre.z / 100.0);
        colour = saturate(shown).xxx;
    }
    pfxOut[storePos] = float4(colour, base.a);
}
