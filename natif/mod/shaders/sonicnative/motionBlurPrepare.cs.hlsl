// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: the motion blur that keeps Sonic sharp, pass 1 of 5 (PostEffect
// sonicnative_sharpBlur, made by lua/ge/extensions/sonicnative/fx.lua, which pauses BeamNG's own
// PostFxMotionBlur meanwhile). BeamNG's velocity buffer becomes this blur's: each pixel's half blur in
// pixels, its distance from the eye, and whether it is Sonic's. A pixel whose world position lies in
// a capsule around his bones is his and gets no motion, as BeamNG gives none to the player's car:
// the camera follows him, so he hardly moves on screen. Laid out like speedBlur.cs.hlsl; the world
// position is rebuilt from the depth as shaders/common/postFx/caustics/caustics.cs.hlsl does.
//   texture 0        #VelocityBuffer: the uv this pixel had in the last frame minus its uv
//   texture 1        #prepass[Depth]
//   target           #sonicnative_sharpVel (rgba16f): xy half blur (px), z distance (m), w 1 on Sonic
//   sonicMotion      x half blur per frame's motion, y longest half blur (share of the height),
//                    z tile size in pixels (no blur may be longer)
//   sonicBound       xyz his middle (world), w the radius beyond which no pixel is his (0: no Sonic)
//   sonicSegA/B0..19 rounded cones around his bones: xyz their ends (world), w the radius there
//                    (m; a negative radius at A: unused)
//   sonicGround      xyz his feet (world), w how far above the plane through them, square to
//                    sonicUp.xyz (his up), a pixel must be to be his: the ground or the wall under his
//                    feet stays blurred (w -1000: no plane, the ball shell)
// The numbers come from lua/common/sonicnative/effects.lua (sharp).
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(velocity, 0);
uniform_sampler2D(depth, 1);

cbuffer perDraw : register(b0)
{
    uniform float3 eyePosWorld;
    uniform float4 sonicMotion;
    uniform float4 sonicBound;
    uniform float4 sonicGround;
    uniform float4 sonicUp;
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

// the farthest distance kept (rgba16f holds up to 65504)
static const float FAR_DISTANCE = 60000.0;

int2 texelOf(float2 uv, uint2 size)
{
    return clamp(int2(uv * float2(size)), int2(0, 0), int2(size) - int2(1, 1));
}

// how far p lies outside the rounded cone from a (radius a.w) to b (radius b.w): negative inside
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

#define SONIC_SEGMENT(n) inside = inside || outsideSegment(p, sonicSegA##n, sonicSegB##n) < 0.0;

PFX_NUMTHREADS
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 storePos;
    if (!postFxComputeThread(dispatchThreadID, storePos))
    {
        return;
    }

    PFXVertToPix IN = processPostFxCompute(dispatchThreadID);

    uint2 velocitySize, depthSize;
    velocityTex.GetDimensions(velocitySize.x, velocitySize.y);
    depthTex.GetDimensions(depthSize.x, depthSize.y);

    // the half blur in pixels: BeamNG's per-frame motion times the strength, no longer than the
    // longest blur nor a tile (the gather only looks one tile away)
    float2 motion = velocityTex.Load(int3(texelOf(IN.uv0, velocitySize), 0)).xy;
    float2 halfBlur = motion * float2(velocitySize) * sonicMotion.x;
    float longest = min(sonicMotion.y * float(velocitySize.y), sonicMotion.z);
    float halfLength = length(halfBlur);
    if (halfLength > longest)
    {
        halfBlur *= longest / halfLength;
    }

    // this pixel in the world (caustics.cs.hlsl: eye + eye ray x linear depth); the sky is far away
    float ndc = depthTex.Load(int3(texelOf(IN.uv1, depthSize), 0)).r;
    float3 ray = IN.wsEyeRay * realToEyeLinearDepth(ndcToRealDepth(ndc, projParams.zw), projParams);
    float eyeDistance = length(ray);
    bool isFar = !(eyeDistance < FAR_DISTANCE);
    float3 p = eyePosWorld + ray;

    bool inside = false;
    bool aboveGround = dot(p - sonicGround.xyz, sonicUp.xyz) >= sonicGround.w;
    if (!isFar && aboveGround && sonicBound.w > 0.0 && length(p - sonicBound.xyz) < sonicBound.w)
    {
        SONIC_SEGMENT(0)  SONIC_SEGMENT(1)  SONIC_SEGMENT(2)  SONIC_SEGMENT(3)
        SONIC_SEGMENT(4)  SONIC_SEGMENT(5)  SONIC_SEGMENT(6)  SONIC_SEGMENT(7)
        SONIC_SEGMENT(8)  SONIC_SEGMENT(9)  SONIC_SEGMENT(10) SONIC_SEGMENT(11)
        SONIC_SEGMENT(12) SONIC_SEGMENT(13) SONIC_SEGMENT(14) SONIC_SEGMENT(15)
        SONIC_SEGMENT(16) SONIC_SEGMENT(17) SONIC_SEGMENT(18) SONIC_SEGMENT(19)
    }
    if (inside)
    {
        halfBlur = float2(0.0, 0.0);
    }

    pfxOut[storePos] = float4(halfBlur, isFar ? FAR_DISTANCE : eyeDistance, inside ? 1.0 : 0.0);
}
