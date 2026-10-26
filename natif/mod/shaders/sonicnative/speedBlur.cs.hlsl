// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: the boost's radial speed blur, a compute post effect (PostEffect
// sonicnative_speedBlur, made by lua/ge/extensions/sonicnative/fx.lua). The finished frame streaks
// toward Sonic's chest; the middle stays sharp. A port of the overlay's psPost
// (beamng/src/shared_draw.hpp, v0.12.3) on the game's compute scaffolding, laid out like
// shaders/common/postFx/passthru.cs.hlsl and passthru.h.hlsl (absolute includes: this file lives
// in the mod's own folder). BeamNG composes its UI after the post effects, so the overlay's
// minimap and toolbar masks are gone.
//   sonicBlur.xy     Sonic's chest on screen: right and up, in units of tan(vertical fov / 2)
//   sonicBlur.z      longest streak, share of the distance to the centre (0: no blur)
//   sonicBlur.w      UV radius left sharp around the centre
//   sonicBlurShape.x UV radius where the streaks reach their full length
//   sonicBlurShape.yz the centre is kept inside this part of the screen (UV)
// The numbers come from lua/common/sonicnative/effects.lua (blur); the 16 taps are its blur.taps.
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(inputTex, 0);

cbuffer perDraw : register(b0)
{
    uniform float4 sonicBlur;
    uniform float4 sonicBlurShape;
    POSTFX_UNIFORMS
};

#include "shaders/common/postFx/postFx.hlsl"
#include "shaders/common/postFx/postFxCompute.h.hlsl"

PFX_NUMTHREADS
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 storePos;
    if (!postFxComputeThread(dispatchThreadID, storePos))
    {
        return;
    }

    // viewport uv, 0..1 from the top left
    float2 uv = postFxComputeUv(dispatchThreadID);
    float aspect = dispatchRect.z / max(dispatchRect.w, 1.0);
    float2 centre = 0.5 + 0.5 * float2(sonicBlur.x / max(aspect, 0.001), -sonicBlur.y);
    centre = clamp(centre, sonicBlurShape.y, max(sonicBlurShape.y, sonicBlurShape.z));

    // every tap lies between this pixel and the centre, inside the viewport
    float2 d = uv - centre;
    float amount = saturate(sonicBlur.z) * smoothstep(sonicBlur.w, max(sonicBlurShape.x, sonicBlur.w + 0.001), length(d));

    float4 base = tex2D(inputTex, viewportCoordToRenderTarget(uv, rtParams0));
    float3 acc = 0;
    [unroll] for (int k = 0; k < 16; k++)
    {
        acc += tex2D(inputTex, viewportCoordToRenderTarget(uv - d * amount * (k / 15.0), rtParams0)).rgb;
    }
    pfxOut[storePos] = float4(acc / 16.0, base.a);
}
