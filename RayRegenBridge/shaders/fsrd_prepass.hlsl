// FSRD prepass: builds FSR Ray Regeneration's inputs from what DLSS-RR receives (see fsrd_common.hlsli).
// Game resources are only read, every output is an FSRD scratch texture.

#include "fsrd_common.hlsli"

Texture2D<float4> Color : register(t0);
Texture2D<float4> DiffuseAlbedo : register(t1);
Texture2D<float4> SpecularAlbedo : register(t2);
Texture2D<float4> Normals : register(t3);
Texture2D<float> Roughness : register(t4);
Texture2D<float> Depth : register(t5);
Texture2D<float> SpecularHitDistance : register(t6);
Texture2D<float4> MotionVectors : register(t7);
Texture2D<float4> SssGuide : register(t8); // DLSSD.ScreenSpaceSubsurfaceScatteringGuide, used with FLAG_SSS_GUIDE
Texture2D<float4> DiffuseHit : register(t9); // DLSSD.DiffuseHitDistance (.r) or DiffuseRayDirectionHitDistance (.w)
Texture2D<float4> Emissive : register(t10);  // GBuffer.Emissive, null reads 0: kept out of the denoiser

// The game's diffuse hit distance when it sends one, FarHitDistance and up = miss, else the constant fallback
float GameDiffuseHit(int3 p)
{
    if ((Flags & FLAG_DIFFUSE_HIT) == 0)
        return DiffuseHitDistance;

    const float4 texel = DiffuseHit.Load(p);
    const float hit = (Flags & FLAG_DIFFUSE_HIT_IN_W) ? texel.w : texel.r;

    if (!(hit > 0.0f))
        return DiffuseHitDistance;

    return hit >= FarHitDistance ? MaxHitDistance : hit;
}

RWTexture2D<float4> DiffuseSignal : register(u0);
RWTexture2D<float4> SpecularSignal : register(u1);
RWTexture2D<float> LinearDepthOut : register(u2);
RWTexture2D<float4> PackedNormals : register(u3);
RWTexture2D<float4> MotionVectorsOut : register(u4);
RWTexture2D<uint> SentinelCounter : register(u5); // 2x1, [CounterSlot] counts this frame, the other is cleared

// FFX wants the signed linear depth delta in MV.z, which DLSS doesn't have. It's computed by reprojecting the pixel
// as static geometry with the previous view matrix; pixels whose game MV disagrees with that (characters, first
// person arms) are moving and get 0, which is right for anything attached to the camera.
float4 BuildMotionVector(uint2 pixel, float viewZ, bool sky)
{
    float2 mv = MotionVectors.Load(int3(uint2(float2(pixel) * MvTexelScale), 0)).xy * MvScale;

    if (sky || HasPrevView == 0 || !all(!isnan(mv)))
        return float4(isnan(mv.x) ? 0.0f : mv.x, isnan(mv.y) ? 0.0f : mv.y, 0.0f, 0.0f);

    float2 uv = (float2(pixel) + 0.5f) / float2(RenderWidth, RenderHeight);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 v = float4(ndc.x * viewZ * InvProj.x, ndc.y * viewZ * InvProj.y, viewZ, 1.0f);

    float3 prev = float3(dot(v, PrevViewX), dot(v, PrevViewY), dot(v, PrevViewZ));

    if (prev.z <= 1e-4f)
        return float4(mv, 0.0f, 0.0f);

    float2 prevNdc = float2(prev.x / (prev.z * InvProj.x), prev.y / (prev.z * InvProj.y));
    float2 prevUv = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);

    float depthDelta = length((prevUv - uv) - mv) <= MaxStaticMvError ? prev.z - viewZ : 0.0f;
    return float4(mv, depthDelta, 0.0f);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= RenderWidth || DTid.y >= RenderHeight)
        return;

    int3 p = int3(DTid.xy, 0);

    // Signals are in scene units: the game's color comes multiplied by its pre-exposure, compose multiplies back.
    // Emissive light (neons, screens) isn't noisy: it stays out of the denoiser and compose adds it back.
    float3 color = (Color.Load(p).rgb - max(Emissive.Load(p).rgb, 0.0f)) / PreExposure;
    const float diffuseHit = GameDiffuseHit(p);
    float hwDepth = Depth.Load(p);
    float4 n = DecodeNormals(Normals.Load(p));
    float3 diffuseAlbedo = DiffuseAlbedo.Load(p).rgb;
    float3 specularAlbedo = SpecularAlbedo.Load(p).rgb;

    const bool sentinel = IsSentinel(n, diffuseAlbedo, specularAlbedo);

    // Count sentinel texels for the probe log, one atomic per wave
    if (Flags & FLAG_COUNT_SENTINELS)
    {
        if (all(DTid.xy == 0))
            SentinelCounter[uint2(1 - CounterSlot, 0)] = 0;

        const uint waveSentinels = WaveActiveCountBits(sentinel);

        if (WaveIsFirstLane() && waveSentinels > 0)
            InterlockedAdd(SentinelCounter[uint2(CounterSlot, 0)], waveSentinels);
    }

    // World space normal, roughness in normals.w when packed (DLSS.Roughness.Mode 1)
    float3 normal = n.xyz;
    float len = length(normal);
    normal = len > 1e-5f ? normal / len : float3(0.0f, 0.0f, 1.0f);
    float roughness = (Flags & FLAG_SEPARATE_ROUGHNESS) ? Roughness.Load(p) : n.w;

    if (RoughnessOverride >= 0.0f)
        roughness = RoughnessOverride;

    // Sentinel texels denoised as diffuse: no real normal, so face the camera and be fully rough
    if (sentinel && SentinelMode == SENTINEL_DIFFUSE)
    {
        normal = -normalize(CameraForward);
        roughness = 1.0f;
    }

    float viewZ = LinearDepth(hwDepth);

    // Passthrough sentinels are inactive for the denoiser, so there the material only tells compose what they are.
    // Skin (marked by the game's SSS guide) is its own material, so it isn't filtered together with clothes, hair and
    // the background.
    const bool skin = (Flags & FLAG_SSS_GUIDE) && any(SssGuide.Load(p).rgb > 0.01f);
    const float material = sentinel ? MATERIAL_SENTINEL : skin ? MATERIAL_SKIN : MATERIAL_SURFACE;
    PackedNormals[DTid.xy] = float4(NormalToOctahedronUv(normal), saturate(roughness), material);
    LinearDepthOut[DTid.xy] = viewZ;
    MotionVectorsOut[DTid.xy] = BuildMotionVector(DTid.xy, viewZ, IsSky(hwDepth) || viewZ >= SkyDepth);

    if (sentinel)
    {
        // Passthrough: inactive for the denoiser, compose keeps the game's color. Diffuse: albedo is white, so
        // the color goes undemodulated through the diffuse signal only.
        const bool diffuse = SentinelMode == SENTINEL_DIFFUSE && IsFinite3(color);
        DiffuseSignal[DTid.xy] = diffuse ? float4(max(color, 0.0f), diffuseHit) : float4(0.0f, 0.0f, 0.0f, -1.0f);
        SpecularSignal[DTid.xy] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return;
    }

    float3 dMod, sMod;
    float albedoRatio;

    if (!GetModulation(diffuseAlbedo, specularAlbedo, hwDepth, dMod, sMod, albedoRatio) || !IsFinite3(color))
    {
        // Negative hit distance marks the texel as inactive for the denoiser
        DiffuseSignal[DTid.xy] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        SpecularSignal[DTid.xy] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return;
    }

    color = max(color, 0.0f);

    // Game's specular hit distance: FarHitDistance and up = ray missed (far), 0 or invalid = no specular ray, the
    // specular signal is inactive and the whole color goes through the diffuse one
    float specHit = DiffuseHitDistance;
    bool specActive = true;

    if (Flags & FLAG_HAS_SPEC_HIT_DISTANCE)
    {
        const float rawHit = SpecularHitDistance.Load(p);

        if (rawHit >= FarHitDistance)
            specHit = MaxHitDistance;
        else if (rawHit > 0.0f)
            specHit = min(rawHit * SpecHitScale, MaxHitDistance);
        else
            specActive = false;
    }

    const float wd = specActive ? DiffuseWeight(albedoRatio, saturate(roughness)) : 1.0f;

    DiffuseSignal[DTid.xy] = float4(color * wd / dMod, diffuseHit);
    SpecularSignal[DTid.xy] = specActive ? float4(color * (1.0f - wd) / sMod, specHit) : float4(0.0f, 0.0f, 0.0f, -1.0f);
}
