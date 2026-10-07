// FSRD particles: puts the game's particle layer (DLSSD.ColorBeforeParticles) over a color. Used when the denoiser
// is bypassed, so the game's noisy color shows steam and smoke like the denoised one (compose does it otherwise).

#include "fsrd_common.hlsli"

Texture2D<float4> Color : register(t0);     // game's color
Texture2D<float4> Particles : register(t1); // particle layer, premultiplied over black, alpha = coverage

RWTexture2D<float4> Output : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height)
        return;

    const int3 p = int3(DTid.xy, 0);
    const float4 color = Color.Load(p);

    if (DTid.x >= RenderWidth || DTid.y >= RenderHeight || !IsFinite3(color.rgb))
    {
        Output[DTid.xy] = color;
        return;
    }

    float unusedReactive = 0.0f;
    Output[DTid.xy] = float4(OverParticleLayer(color.rgb, Particles.Load(p), unusedReactive), color.a);
}
