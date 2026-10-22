// FSRD live metrics: one thread group per 64x64 tile of the render area sums what FsrdMetrics compares over time
// (game color vs composed color, clamped and active signal texels, skin). Three texels per tile in TileOut:
//   x * 3 + 0: game color RGB sum, luma squared sum
//   x * 3 + 1: composed color RGB sum, luma squared sum
//   x * 3 + 2: texels above MaxRadiance, texels active for the denoiser, skin texels, texels counted

#include "fsrd_common.hlsli"

Texture2D<float4> Color : register(t0);
Texture2D<float4> Composed : register(t1);
Texture2D<float4> DiffuseIn : register(t2);
Texture2D<float4> SpecularIn : register(t3);
Texture2D<float4> PackedNormals : register(t4);

RWTexture2D<float4> TileOut : register(u0);

static const uint TileSize = 64;
static const uint Threads = 256;
static const uint Values = 12;

groupshared float Sums[Values][Threads];

[numthreads(16, 16, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    float v[Values];

    [unroll]
    for (uint i = 0; i < Values; i++)
        v[i] = 0.0f;

    // Each thread sums a 4x4 block of the tile
    for (uint y = 0; y < 4; y++)
    {
        for (uint x = 0; x < 4; x++)
        {
            const uint2 pixel = groupId.xy * TileSize + threadId.xy * 4 + uint2(x, y);

            if (pixel.x >= RenderWidth || pixel.y >= RenderHeight)
                continue;

            const int3 p = int3(pixel, 0);
            const float3 color = Color.Load(p).rgb;
            const float3 composed = Composed.Load(p).rgb;

            if (!IsFinite3(color) || !IsFinite3(composed))
                continue;

            const float lc = dot(color, LumaWeights);
            const float lo = dot(composed, LumaWeights);
            v[0] += color.r;
            v[1] += color.g;
            v[2] += color.b;
            v[3] += lc * lc;
            v[4] += composed.r;
            v[5] += composed.g;
            v[6] += composed.b;
            v[7] += lo * lo;

            const float4 d = DiffuseIn.Load(p);
            const float4 s = SpecularIn.Load(p);
            const bool active = d.w >= 0.0f || s.w >= 0.0f;
            const float peak = max(d.w >= 0.0f ? max(d.r, max(d.g, d.b)) : 0.0f,
                                   s.w >= 0.0f ? max(s.r, max(s.g, s.b)) : 0.0f);

            v[8] += active && peak > MaxRadiance ? 1.0f : 0.0f;
            v[9] += active ? 1.0f : 0.0f;
            v[10] += abs(PackedNormals.Load(p).w - MATERIAL_SKIN) < 0.05f ? 1.0f : 0.0f;
            v[11] += 1.0f;
        }
    }

    [unroll]
    for (uint j = 0; j < Values; j++)
        Sums[j][index] = v[j];

    GroupMemoryBarrierWithGroupSync();

    for (uint stride = Threads / 2; stride > 0; stride >>= 1)
    {
        if (index < stride)
        {
            [unroll]
            for (uint k = 0; k < Values; k++)
                Sums[k][index] += Sums[k][index + stride];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0)
    {
        const uint2 base = uint2(groupId.x * 3, groupId.y);
        TileOut[base] = float4(Sums[0][0], Sums[1][0], Sums[2][0], Sums[3][0]);
        TileOut[base + uint2(1, 0)] = float4(Sums[4][0], Sums[5][0], Sums[6][0], Sums[7][0]);
        TileOut[base + uint2(2, 0)] = float4(Sums[8][0], Sums[9][0], Sums[10][0], Sums[11][0]);
    }
}
