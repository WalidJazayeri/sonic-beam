// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: the motion blur that keeps Sonic sharp, passes 2 and 3 of 5
// (sonicnative_sharpBlurTileX, then sonicnative_sharpBlurTile). Each texel of the smaller target
// keeps the longest half blur of the texels of the input it covers: first along x (target scale
// 1/tile by 1 of the full-size input), then along y (1 by 1/tile of the x pass's output). Two
// steps keep each thread to one tile's row.
//   texture 0  #sonicnative_sharpVel, then #sonicnative_sharpTileX (xy: half blur in pixels)
//   target     #sonicnative_sharpTileX, then #sonicnative_sharpTile (rgba16f: xy the longest)
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(motion, 0);

cbuffer perDraw : register(b0)
{
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

    // the input texels this one covers: [first, last) on each axis, at least one
    uint2 inSize;
    motionTex.GetDimensions(inSize.x, inSize.y);
    uint2 outSize = max(uint2(dispatchRect.zw), uint2(1, 1));
    uint2 first = min(dispatchThreadID.xy * inSize / outSize, inSize - 1);
    uint2 last = max(min(((dispatchThreadID.xy + 1) * inSize + outSize - 1) / outSize, inSize), first + 1);

    float2 longest = float2(0.0, 0.0);
    float longestSq = 0.0;
    for (uint y = first.y; y < last.y; y++)
    {
        for (uint x = first.x; x < last.x; x++)
        {
            float2 v = motionTex.Load(int3(x, y, 0)).xy;
            float sq = dot(v, v);
            if (sq > longestSq)
            {
                longestSq = sq;
                longest = v;
            }
        }
    }

    pfxOut[storePos] = float4(longest, 0.0, 0.0);
}
