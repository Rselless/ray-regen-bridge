// FSRD exposure tracking, pass 1: the game applies its auto exposure to the color before Ray Reconstruction and
// Cyberpunk doesn't report it (DLSS.Pre.Exposure = 1), so it's measured: each surface's brightness this frame against
// the same surface last frame (reprojected with the game's motion vectors, same depth). One thread group per
// 64x64 tile sums both over the texels that match; fsrd_exposure_reduce.hlsl turns the tiles into one ratio.
// Also stores this frame's luma and depth for the next frame (ping-ponged history, so no texel is read and written).

#include "fsrd_common.hlsli"

Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> MotionVectors : register(t2);
Texture2D<float4> Emissive : register(t3);
Texture2D<float2> PrevHistory : register(t4); // last frame: luma, linear depth (0 = none)

RWTexture2D<float4> Tiles : register(u0);     // current luma sum, previous luma sum, texels, unused
RWTexture2D<float2> History : register(u1);   // this frame: luma, linear depth

static const uint TileSize = 64;
static const uint Threads = 256;
static const float MaxLuma = 64.0f; // keeps rare fireflies from dominating a tile

groupshared float3 Sums[Threads];

float SignalLuma(int3 p)
{
    const float3 c = Color.Load(p).rgb - max(Emissive.Load(p).rgb, 0.0f);
    return IsFinite3(c) ? min(max(dot(c, LumaWeights), 0.0f) / PreExposure, MaxLuma) : -1.0f;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    float3 sum = 0.0f;

    for (uint y = 0; y < 4; y++)
    {
        for (uint x = 0; x < 4; x++)
        {
            const uint2 pixel = groupId.xy * TileSize + threadId.xy * 4 + uint2(x, y);

            if (pixel.x >= RenderWidth || pixel.y >= RenderHeight)
                continue;

            const int3 p = int3(pixel, 0);
            const float hwDepth = Depth.Load(p);
            const bool sky = IsSky(hwDepth);
            const float viewZ = sky ? 0.0f : LinearDepth(hwDepth);
            const float luma = SignalLuma(p);

            History[pixel] = float2(max(luma, 0.0f), luma >= 0.0f ? viewZ : 0.0f);

            if (sky || luma < 0.0f || (Flags & FLAG_EXPOSURE_RESET))
                continue;

            // Where this surface was last frame (game MV -> PreviousUV - CurrentUV, as in the prepass)
            const float2 mv = MotionVectors.Load(int3(uint2(float2(pixel) * MvTexelScale), 0)).xy * MvScale;
            const float2 prevUv = (float2(pixel) + 0.5f) / float2(RenderWidth, RenderHeight) + mv;

            if (any(isnan(mv)) || any(prevUv <= 0.0f) || any(prevUv >= 1.0f))
                continue;

            const float2 prev = PrevHistory.Load(int3(uint2(prevUv * float2(RenderWidth, RenderHeight)), 0));

            // Same surface: depth within 5% (static or slow geometry; fast movers and disocclusions drop out)
            if (prev.y <= 0.0f || abs(prev.y - viewZ) > 0.05f * viewZ)
                continue;

            sum += float3(luma, prev.x, 1.0f);
        }
    }

    Sums[index] = sum;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = Threads / 2; stride > 0; stride >>= 1)
    {
        if (index < stride)
            Sums[index] += Sums[index + stride];

        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0)
        Tiles[groupId.xy] = float4(Sums[0], 0.0f);
}
