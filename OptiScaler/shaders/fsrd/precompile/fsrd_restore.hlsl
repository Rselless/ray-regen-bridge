// FSRD restore: puts back the low frequency part of the game's color the denoiser removed (fog, haze, other
// additive media that isn't surface lighting). The residual (game color - composed) holds that part plus the
// per pixel noise; a wide depth aware blur averages the noise out and keeps the smooth fog, without crossing
// depth edges so near objects don't get far fog (halos).
//
// Also the final particle pass: with RestoreStrength 0 it only puts the particle layer over Composed (used with the
// low frequency restore on, and to add the particles to the game's color when the denoiser is bypassed).

#include "fsrd_common.hlsli"

Texture2D<float4> Color : register(t0);       // game's noisy color
Texture2D<float4> Composed : register(t1);    // denoised, remodulated color from fsrd_compose
Texture2D<float> LinearDepthIn : register(t2); // prepass linear depth, render size
Texture2D<float4> Particles : register(t3);    // particle layer, put over the result with FLAG_PARTICLE_LAYER

RWTexture2D<float4> Output : register(u0);

static const int RestoreTaps = 4; // (2 * 4 + 1)^2 = 81 taps
static const float RestoreDepthTolerance = 0.05f; // relative depth difference that still counts as the same surface

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height)
        return;

    int3 p = int3(DTid.xy, 0);
    float4 composed = Composed.Load(p);

    if (DTid.x >= RenderWidth || DTid.y >= RenderHeight)
    {
        Output[DTid.xy] = composed;
        return;
    }

    float3 restored = composed.rgb;

    if (RestoreStrength > 0.0f)
    {
        const float centerDepth = LinearDepthIn.Load(p);
        const int stride = max((int) RestoreStride, 1);

        float3 residualSum = 0.0f;
        float weightSum = 0.0f;

        [loop]
        for (int y = -RestoreTaps; y <= RestoreTaps; y++)
        {
            [loop]
            for (int x = -RestoreTaps; x <= RestoreTaps; x++)
            {
                int2 q = int2(DTid.xy) + int2(x, y) * stride;

                if (q.x < 0 || q.y < 0 || q.x >= (int) RenderWidth || q.y >= (int) RenderHeight)
                    continue;

                const float depth = LinearDepthIn.Load(int3(q, 0));
                const float relative = abs(depth - centerDepth) / max(centerDepth, 1e-3f);
                const float weight = saturate(1.0f - relative / RestoreDepthTolerance);

                if (weight <= 0.0f)
                    continue;

                const float3 residual = Color.Load(int3(q, 0)).rgb - Composed.Load(int3(q, 0)).rgb;

                if (!IsFinite3(residual))
                    continue;

                residualSum += residual * weight;
                weightSum += weight;
            }
        }

        if (weightSum > 0.0f)
            restored += RestoreStrength * residualSum / weightSum;
    }

    if (!IsFinite3(restored))
        restored = composed.rgb;

    restored = max(restored, 0.0f);

    if (Flags & FLAG_PARTICLE_LAYER)
    {
        float unusedReactive = 0.0f;
        restored = OverParticleLayer(restored, Particles.Load(p), unusedReactive);
    }

    Output[DTid.xy] = float4(restored, composed.a);
}
