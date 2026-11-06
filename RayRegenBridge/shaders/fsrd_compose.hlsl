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
Texture2D<float4> PackedNormals : register(t7);  // prepass output, roughness in B, material in A
Texture2D<float4> Particles : register(t8); // DLSSD.ColorBeforeParticles: steam/smoke/glows over black, alpha is
                                            // coverage, used with FLAG_PARTICLE_LAYER
Texture2D<float4> BeforeTransparency : register(t9); // DLSSD.ColorBeforeTransparency, used with
                                                     // FLAG_TRANSPARENCY_LAYER
Texture2D<float> LayerOpacity : register(t10); // DLSS.TransparencyLayerOpacity, coverage of the layer in t8 with
                                               // FLAG_LAYER_OPACITY
Texture2D<float4> Emissive : register(t11);    // GBuffer.Emissive, the prepass kept it out of the signals
Texture2D<float> GhostMask : register(t12);    // DLSS.Input.Reduce.Ghost.Mask, goes into the reactive mask
Texture2D<float> Exposure : register(t13);       // tracked scene exposure (FLAG_EXPOSURE)

RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> Reactive : register(u1);

// The prepass denoised the color before transparencies (fog, glass, particles), so their contribution is put back
// undenoised: Color - BeforeTransparency. It also carries a bit of the noisy background where the transparency
// dims it, but never filters away fog the way the denoiser does.
float3 AddTransparency(float3 denoised, float3 color, int3 p)
{
    if ((Flags & FLAG_TRANSPARENCY_LAYER) == 0)
        return denoised;

    const float3 before = BeforeTransparency.Load(p).rgb;

    if (!IsFinite3(before))
        return denoised;

    return max(denoised + (color - before), 0.0f);
}

// Puts the game's particle / transparency layer over a color: color * transmittance + layer (premultiplied).
// Coverage is the layer's alpha, or the separate opacity texture games send with DLSS.TransparencyLayer.
float3 OverParticles(float3 c, int3 p, inout float reactive)
{
    if ((Flags & FLAG_PARTICLE_LAYER) == 0)
        return c;

    float4 layer = Particles.Load(p);

    if (Flags & FLAG_LAYER_OPACITY)
        layer.a = LayerOpacity.Load(p);

    return OverParticleLayer(c, layer, reactive);
}

// The denoiser blends the inactive sky texels into the surface texels next to them, which come out ~40% darker: a
// dark outline on every silhouette against the sky. Those texels take the average signals of the surface
// SilhouetteRepair texels away, on the surface side (no sky in between) and at the same depth within 10%.
// Measured on captures (BuryGer, Corpo Plaza): the outline goes from -25..-45% to -4..-11% with 4 texels; 3 left
// some dark texels on distant roofs. The darkening fades over a few texels.
// Thin structures (antennas, wires, twigs) have no surface that far in and came out ~75% darker, black lines: false
// is returned for them and they keep the game's color, noisy but at the right brightness.
bool DenoisedSignals(int3 p, out float3 diffuse, out float3 specular)
{
    diffuse = DiffuseDenoised.Load(p).rgb;
    specular = SpecularDenoised.Load(p).rgb;

    if (SilhouetteRepair == 0)
        return true;

    static const int2 Offsets[8] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
                                     int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
    const float viewZ = LinearDepth(Depth.Load(p));
    const int2 last = int2(RenderWidth, RenderHeight) - 1;
    const int reach = (int) min(SilhouetteRepair, 4u);
    bool silhouette = false;

    [unroll]
    for (int i = 0; i < 8; i++)
        silhouette = silhouette || IsSky(Depth.Load(int3(clamp(p.xy + Offsets[i], 0, last), 0)));

    if (!silhouette)
        return true;

    float3 diffuseSum = 0.0f;
    float3 specularSum = 0.0f;
    float count = 0.0f;

    for (int j = 0; j < 8; j++)
    {
        const int3 q = int3(clamp(p.xy + reach * Offsets[j], 0, last), 0);
        const float hwDepth = Depth.Load(q);
        bool blocked = IsSky(hwDepth) || abs(LinearDepth(hwDepth) - viewZ) > 0.1f * viewZ;

        for (int t = 1; t < reach && !blocked; t++)
            blocked = IsSky(Depth.Load(int3(clamp(p.xy + t * Offsets[j], 0, last), 0)));

        if (blocked)
            continue;

        diffuseSum += DiffuseDenoised.Load(q).rgb;
        specularSum += SpecularDenoised.Load(q).rgb;
        count += 1.0f;
    }

    if (count == 0.0f)
        return false;

    diffuse = diffuseSum / count;
    specular = specularSum / count;
    return true;
}

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

    // Null textures read 0, so games that send neither mask are unaffected
    float gameReactive = max(saturate(GameReactive.Load(p) * GameMaskBias), saturate(GhostMask.Load(p)));
    const float3 emissive = max(Emissive.Load(p).rgb, 0.0f);
    const float scale = PreExposure * SceneExposure(Exposure); // what the prepass divided the signals by

    const float3 diffuseAlbedo = DiffuseAlbedo.Load(p).rgb;
    const float3 specularAlbedo = SpecularAlbedo.Load(p).rgb;

    const float4 packedNormals = PackedNormals.Load(p);

    // The prepass marked sentinel texels in the material channel
    if (packedNormals.w > 0.5f * MATERIAL_SENTINEL)
    {
        // Same choice as the prepass: game color, or the denoised diffuse signal (white albedo, no remodulation)
        const float3 denoised = DiffuseDenoised.Load(p).rgb;
        const bool diffuse = SentinelMode == SENTINEL_DIFFUSE && IsFinite3(denoised) && IsFinite3(color.rgb);

        const float3 sentinelColor =
            diffuse ? AddTransparency(max(denoised, 0.0f) * scale + emissive, color.rgb, p) : color.rgb;
        Output[DTid.xy] = float4(OverParticles(sentinelColor, p, gameReactive), color.a);
        Reactive[DTid.xy] = gameReactive;
        return;
    }

    float3 dMod, sMod;
    float albedoRatio;

    if (!GetModulation(diffuseAlbedo, specularAlbedo, Depth.Load(p), dMod, sMod, albedoRatio) || !IsFinite3(color.rgb))
    {
        Output[DTid.xy] = float4(OverParticles(color.rgb, p, gameReactive), color.a);
        Reactive[DTid.xy] = gameReactive;
        return;
    }

    float3 diffuseDenoised, specularDenoised;
    const bool repaired = DenoisedSignals(p, diffuseDenoised, specularDenoised);

    // Back to the game's pre-exposed units
    float3 specular = max(specularDenoised * sMod, 0.0f) * scale;

    float3 composed = max(diffuseDenoised * dMod, 0.0f) * scale + specular;

    if (!repaired || !IsFinite3(composed))
    {
        Output[DTid.xy] = float4(OverParticles(color.rgb, p, gameReactive), color.a);
        Reactive[DTid.xy] = gameReactive;
        return;
    }

    float specularShare = saturate(dot(specular, LumaWeights) / max(dot(composed, LumaWeights), 1e-4f));
    float smoothness = 1.0f - smoothstep(0.1f, 0.5f, packedNormals.z);
    composed = AddTransparency(composed + emissive, color.rgb, p);

    float reactive = max(gameReactive, saturate(ReflectionReactive * specularShare * smoothness));
    Output[DTid.xy] = float4(OverParticles(composed, p, reactive), color.a);
    Reactive[DTid.xy] = reactive;
}
