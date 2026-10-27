// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: Sonic's shadow (PostEffect sonicnative_shadow, made by
// lua/ge/extensions/sonicnative/shadow.lua). A StaticShape casts none, so this pass darkens the lit scene
// before the translucent objects are drawn (the trail, the aura and the sprites stay over it, as in the
// overlay), where his shadow falls (beamng/src/game.cpp 446-481, v0.12.3):
//   - in the sun: his silhouette along the sun's direction, from rounded cones on his bones (the same
//     as the sharp motion blur's), onto whatever lies under him (the overlay flattened his mesh onto
//     the ground's plane);
//   - with a low sun or at night: a soft round blob under him (overlay: (1 - r)^2, 0.42 m).
// Both fade as he rises. The overlay darkened display values by its alpha; here the frame is linear,
// so a pixel is scaled by (1 - alpha)^2.2, the same darkening. His own pixels are left as they are.
// Laid out like motionBlurPrepare.cs.hlsl (world position from the depth, the cones).
//   texture 0        $backBuffer
//   texture 1        #prepass[Depth]
//   sonicSun         xyz towards the sun, w the silhouette's alpha (0: none)
//   sonicGround      xyz the ground under him, w the blob's alpha (0: none)
//   sonicGroundN     xyz the ground's normal, w the blob's radius (m)
//   sonicShadowBound xyz centre, w radius of a sphere outside which nothing is darkened
//   sonicShadowSoft  x cone radius scale, y penumbra (m), z penumbra growth per metre from him, w gamma
//   sonicSegA/B0..19 the cones: xyz their ends (world), w the radius there (negative at A: unused)
//   sonicShadowFeet  xyz his feet, w how far above the plane through them (square to his up) a pixel must
//                    be to be his: the ground around his shoes stays shadowed (-1000: no plane, the ball)
//   sonicShadowUp    xyz his up, w the margin around the cones within which a pixel is his (his mesh
//                    pokes out of the bare cones: shoes, gloves, ears)
// The numbers come from lua/common/sonicnative/effects.lua (shadow).
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(inputTex, 0);
uniform_sampler2D(depth, 1);

cbuffer perDraw : register(b0)
{
    uniform float3 eyePosWorld;
    uniform float4 sonicSun;
    uniform float4 sonicGround;
    uniform float4 sonicGroundN;
    uniform float4 sonicShadowBound;
    uniform float4 sonicShadowSoft;
    uniform float4 sonicShadowFeet;
    uniform float4 sonicShadowUp;
    uniform float4 sonicSegA0;  uniform float4 sonicSegB0;
    uniform float4 sonicSegA1;  uniform float4 sonicSegB1;
    uniform float4 sonicSegA2;  uniform float4 sonicSegB2;
    uniform float4 sonicSegA3;  uniform float4 sonicSegB3;
    uniform float4 sonicSegA4;  uniform float4 sonicSegB4;
    uniform float4 sonicSegA5;  uniform float4 sonicSegB5;
    uniform float4 sonicSegA6;  uniform float4 sonicSegB6;
    uniform float4 sonicSegA7;  uniform float4 sonicSegB7;
    uniform float4 sonicSegA8;  uniform float4 sonicSegB8;
    uniform float4 sonicSegA9;  uniform float4 sonicSegB9;
    uniform float4 sonicSegA10; uniform float4 sonicSegB10;
    uniform float4 sonicSegA11; uniform float4 sonicSegB11;
    uniform float4 sonicSegA12; uniform float4 sonicSegB12;
    uniform float4 sonicSegA13; uniform float4 sonicSegB13;
    uniform float4 sonicSegA14; uniform float4 sonicSegB14;
    uniform float4 sonicSegA15; uniform float4 sonicSegB15;
    uniform float4 sonicSegA16; uniform float4 sonicSegB16;
    uniform float4 sonicSegA17; uniform float4 sonicSegB17;
    uniform float4 sonicSegA18; uniform float4 sonicSegB18;
    uniform float4 sonicSegA19; uniform float4 sonicSegB19;
    POSTFX_UNIFORMS
};

#include "shaders/common/postFx/postFx.hlsl"
#include "shaders/common/postFx/postFxCompute.h.hlsl"

static const float FAR_DISTANCE = 60000.0;

int2 texelOf(float2 uv, uint2 size)
{
    return clamp(int2(uv * float2(size)), int2(0, 0), int2(size) - int2(1, 1));
}

// how far p lies outside the rounded cone from a to b: negative inside
float outsideSegment(float3 p, float4 a, float4 b)
{
    if (a.w < 0.0)
    {
        return 1.0;
    }
    float3 ab = b.xyz - a.xyz;
    float t = saturate(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-8));
    return length(p - (a.xyz + ab * t)) - lerp(a.w, b.w, t);
}

// how much the cone from a to b hides the sun from p: the cone and p seen along the sun's direction
// (both flattened onto the plane square to it), its radius against their distance, softer farther away
float sunCover(float3 p, float3 L, float4 a, float4 b)
{
    if (a.w < 0.0)
    {
        return 0.0;
    }
    float3 pa = a.xyz - p;
    float3 pb = b.xyz - p;
    float ta = dot(pa, L);
    float tb = dot(pb, L);
    if (max(ta, tb) <= 0.0)
    {
        return 0.0;                              // the cone is on the far side of p from the sun
    }
    float3 a2 = pa - L * ta;
    float3 b2 = pb - L * tb;
    float3 ab = b2 - a2;
    float u = saturate(dot(-a2, ab) / max(dot(ab, ab), 1e-8));
    float dist = length(a2 + ab * u);
    float r = lerp(a.w, b.w, u) * sonicShadowSoft.x;
    float pen = sonicShadowSoft.y + sonicShadowSoft.z * max(0.0, lerp(ta, tb, u));
    if (lerp(ta, tb, u) <= 0.0)
    {
        return 0.0;
    }
    return 1.0 - smoothstep(r - pen, r + pen, dist);
}

#define SONIC_COVER(n) if (sun) cover = max(cover, sunCover(p, L, sonicSegA##n, sonicSegB##n)); \
                       inside = inside || (above && outsideSegment(p, sonicSegA##n, sonicSegB##n) < sonicShadowUp.w);

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

    uint2 depthSize;
    depthTex.GetDimensions(depthSize.x, depthSize.y);
    float ndc = depthTex.Load(int3(texelOf(IN.uv1, depthSize), 0)).r;
    float3 ray = IN.wsEyeRay * realToEyeLinearDepth(ndcToRealDepth(ndc, projParams.zw), projParams);
    float3 p = eyePosWorld + ray;
    if (!(length(ray) < FAR_DISTANCE) || length(p - sonicShadowBound.xyz) > sonicShadowBound.w)
    {
        pfxOut[storePos] = base;
        return;
    }

    float3 L = sonicSun.xyz;
    float cover = 0.0;
    bool inside = false;
    bool sun = sonicSun.w > 0.0;                 // with a low sun only the blob shows: no cone covers
    bool above = dot(p - sonicShadowFeet.xyz, sonicShadowUp.xyz) >= sonicShadowFeet.w;
    SONIC_COVER(0)  SONIC_COVER(1)  SONIC_COVER(2)  SONIC_COVER(3)
    SONIC_COVER(4)  SONIC_COVER(5)  SONIC_COVER(6)  SONIC_COVER(7)
    SONIC_COVER(8)  SONIC_COVER(9)  SONIC_COVER(10) SONIC_COVER(11)
    SONIC_COVER(12) SONIC_COVER(13) SONIC_COVER(14) SONIC_COVER(15)
    SONIC_COVER(16) SONIC_COVER(17) SONIC_COVER(18) SONIC_COVER(19)
    if (inside)
    {
        pfxOut[storePos] = base;                 // Sonic himself
        return;
    }

    // the blob: round under him, on what lies near the ground's plane
    float3 g = p - sonicGround.xyz;
    float3 gn = sonicGroundN.xyz;
    float off = dot(g, gn);
    float across = length(g - gn * off);
    float blob = abs(off) < 0.5 ? pow(saturate(1.0 - across / max(sonicGroundN.w, 0.01)), 2.0) : 0.0;

    float dark = saturate(max(sonicSun.w * cover, sonicGround.w * blob));
    pfxOut[storePos] = float4(base.rgb * pow(1.0 - dark, sonicShadowSoft.w), base.a);
}
