// SPDX-License-Identifier: MIT
// Sonic Beam, native mode: the motion blur that keeps Sonic sharp, pass 4 of 5
// (sonicnative_sharpBlurNeighbor). Each tile keeps the longest half blur among itself and its eight
// neighbours: a fast object near a tile's edge blurs over the next tile too (McGuire et al. 2012,
// "A Reconstruction Filter for Plausible Motion Blur": NeighborMax).
//   texture 0  #sonicnative_sharpTile (xy: the tile's longest half blur, pixels)
//   target     #sonicnative_sharpNeighbor (rgba16f, same size: xy the longest around)
#define PFX_COMPUTE 1

#include "shaders/common/postFx/postFx.h.hlsl"

uniform_sampler2D(tile, 0);

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

    uint2 size;
    tileTex.GetDimensions(size.x, size.y);
    int2 here = min(int2(dispatchThreadID.xy), int2(size) - int2(1, 1));

    float2 longest = float2(0.0, 0.0);
    float longestSq = 0.0;
    [unroll] for (int dy = -1; dy <= 1; dy++)
    {
        [unroll] for (int dx = -1; dx <= 1; dx++)
        {
            int2 at = clamp(here + int2(dx, dy), int2(0, 0), int2(size) - int2(1, 1));
            float2 v = tileTex.Load(int3(at, 0)).xy;
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
