// FSRD frame dump: copies one texture (game input or FSRD scratch, read through an SRV so game resources keep their
// state) into an FSRD RGBA16F texture the CPU reads back. RenderWidth/RenderHeight hold the source size here.

#include "fsrd_common.hlsli"

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= RenderWidth || DTid.y >= RenderHeight)
        return;

    Target[DTid.xy] = Source.Load(int3(DTid.xy, 0));
}
