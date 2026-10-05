// FSRD compose: remodulates the denoised signals by the albedos into a new color for the upscaler.
// Covers the whole color texture; texels outside the render area and skipped texels keep the game's color.
//
// Also writes the upscaler's reactive mask. Reflections move differently from the surface they're on, but FSR
// reprojects them with the surface's motion vectors, which leaves ghost trails. Smooth, specular dominated texels
// get a higher reactive value so FSR leans on the current (already denoised) frame there. The game's own reactive
// mask is kept, since FSR.reactive replaces it in the FFX path.

#include "fsrd_common.hlsli"

Texture2D<float4> Color : register(t0);
Texture2D<float4> DiffuseAlbedo : register(t1);
Texture2D<float4> SpecularAlbedo : register(t2);
Texture2D<float> Depth : register(t3);
Texture2D<float4> DiffuseDenoised : register(t4);
Texture2D<float4> SpecularDenoised : register(t5);
Texture2D<float> GameReactive : register(t6);    // DLSS.Input.Bias.Current.Color.Mask, null reads 0
Texture2D<float4> PackedNormals : register(t7);  // prepass output, roughness in B
Texture2D<float4> GameNormals : register(t8);    // game's GBuffer.Normals, only for the sentinel test

RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> Reactive : register(u1);

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);

    if (DTid.x >= width || DTid.y >= height)
        return;

    int3 p = int3(DTid.xy, 0);
    float4 color = Color.Load(p);

    if (DTid.x >= RenderWidth || DTid.y >= RenderHeight)
    {
        Output[DTid.xy] = color;
        Reactive[DTid.xy] = 0.0f;
        return;
    }

    float gameReactive = saturate(GameReactive.Load(p) * GameMaskBias);

    const float3 diffuseAlbedo = DiffuseAlbedo.Load(p).rgb;
    const float3 specularAlbedo = SpecularAlbedo.Load(p).rgb;

    if (IsSentinel(GameNormals.Load(p), diffuseAlbedo, specularAlbedo))
    {
        // Same choice as the prepass: game color, or the denoised diffuse signal (white albedo, no remodulation)
        const float3 denoised = DiffuseDenoised.Load(p).rgb;
        const bool diffuse = SentinelMode == SENTINEL_DIFFUSE && IsFinite3(denoised) && IsFinite3(color.rgb);

        Output[DTid.xy] = diffuse ? float4(max(denoised, 0.0f), color.a) : color;
        Reactive[DTid.xy] = gameReactive;
        return;
    }

    float3 dMod, sMod;
    float albedoRatio;

    if (!GetModulation(diffuseAlbedo, specularAlbedo, Depth.Load(p), dMod, sMod, albedoRatio) || !IsFinite3(color.rgb))
    {
        Output[DTid.xy] = color;
        Reactive[DTid.xy] = gameReactive;
        return;
    }

    float3 specular = max(SpecularDenoised.Load(p).rgb * sMod, 0.0f);
    float3 composed = max(DiffuseDenoised.Load(p).rgb * dMod, 0.0f) + specular;

    if (!IsFinite3(composed))
    {
        Output[DTid.xy] = color;
        Reactive[DTid.xy] = gameReactive;
        return;
    }

    float specularShare = saturate(dot(specular, LumaWeights) / max(dot(composed, LumaWeights), 1e-4f));
    float smoothness = 1.0f - smoothstep(0.1f, 0.5f, PackedNormals.Load(p).z);

    Output[DTid.xy] = float4(composed, color.a);
    Reactive[DTid.xy] = max(gameReactive, saturate(ReflectionReactive * specularShare * smoothness));
}
