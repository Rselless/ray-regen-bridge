// FSRD probe: reads the inputs at a few screen points into a small FSRD texture the CPU copies back for the log.
// The game's resources are only read through SRVs, so their state is never touched (no copy barriers on them).
//
// Output, ProbeTexelsPerPoint texels per point (must match FSRDFeatureDx12.cpp):
//   0: game's specular hit distance, our linear depth, our roughness (packed normals B), our material (A)
//   1: game's GBuffer.Normals (xyz, w = packed roughness)
//   2: game's diffuse albedo
//   3: game's specular albedo

#include "fsrd_common.hlsli"

Texture2D<float> SpecularHitDistance : register(t0);
Texture2D<float4> GameNormals : register(t1);
Texture2D<float4> DiffuseAlbedo : register(t2);
Texture2D<float4> SpecularAlbedo : register(t3);
Texture2D<float> LinearDepthIn : register(t4);
Texture2D<float4> PackedNormals : register(t5);

RWTexture2D<float4> ProbeOut : register(u0);

static const uint ProbePointCount = 5;
static const uint ProbeTexelsPerPoint = 4;

// Fractions of the render size, same order as ProbePoints in FSRDFeatureDx12.cpp
static const float2 ProbePoints[ProbePointCount] = { float2(0.5f, 0.5f), float2(0.5f, 0.8f), float2(0.3f, 0.7f),
                                                     float2(0.7f, 0.7f), float2(0.5f, 0.95f) };

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= ProbePointCount || DTid.y != 0)
        return;

    const uint2 pixel = min(uint2(ProbePoints[DTid.x] * float2(RenderWidth, RenderHeight)),
                            uint2(RenderWidth - 1, RenderHeight - 1));
    const int3 p = int3(pixel, 0);
    const uint base = DTid.x * ProbeTexelsPerPoint;

    const float hit = (Flags & FLAG_HAS_SPEC_HIT_DISTANCE) ? SpecularHitDistance.Load(p) : -1.0f;
    const float4 packed = PackedNormals.Load(p);

    ProbeOut[uint2(base + 0, 0)] = float4(hit, LinearDepthIn.Load(p), packed.z, packed.w);
    ProbeOut[uint2(base + 1, 0)] = GameNormals.Load(p);
    ProbeOut[uint2(base + 2, 0)] = DiffuseAlbedo.Load(p);
    ProbeOut[uint2(base + 3, 0)] = SpecularAlbedo.Load(p);
}
