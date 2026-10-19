// Shared by fsrd_prepass.hlsl and fsrd_compose.hlsl (FSRDFeatureDx12, FSR Ray Regeneration in place of DLSS-RR).
//
// DLSS-RR only receives the final noisy color, so it's split into the two signals FSR Ray Regeneration denoises:
//   diffuse  = color * wd / diffuseModulation
//   specular = color * (1 - wd) / specularModulation
// Compose multiplies the denoised signals back by the same modulations (which only depend on the albedos), so a pixel
// the denoiser leaves untouched comes back exactly as the input. How wd is picked depends on SplitMode:
//   SPLIT_ALBEDO:    wd = luma(diffuseAlbedo) / (luma(diffuseAlbedo) + luma(specularAlbedo)), specular / specularAlbedo
//   SPLIT_SPECULAR:  wd = 0, everything goes through the specular denoiser (roughness aware), / total albedo
//   SPLIT_ROUGHNESS: smooth surfaces all specular (sharp reflections), rough ones like SPLIT_ALBEDO, / total albedo
// Sky and albedo-less pixels (emissive, black) are skipped and keep the game's color.

cbuffer Params : register(b0)
{
    float ProjZZ;             // ViewToClip[2][2]
    float ProjWZ;             // ViewToClip[3][2]
    float DiffuseHitDistance; // DLSS-RR has no diffuse hit distance, constant fallback
    uint Flags;
    uint RenderWidth;
    uint RenderHeight;
    float SkyDepth;           // value written to linear depth for sky, outside the denoiser's depth bounds
    uint SplitMode;

    float2 MvScale;           // game MV -> PreviousUV - CurrentUV
    float2 MvTexelScale;      // render pixel -> MV texel (MVs can be display sized)
    float2 InvProj;           // 1 / ViewToClip[0][0], 1 / ViewToClip[1][1]
    uint HasPrevView;
    float MaxStaticMvError;   // in UV, above this the pixel is treated as moving (no depth delta)

    // Current view space -> previous view space (row-vector), columns x, y, z: prev.c = dot(float4(v, 1), PrevView[c])
    float4 PrevViewX;
    float4 PrevViewY;
    float4 PrevViewZ;

    float ReflectionReactive; // compose: reactive mask strength for smooth reflections, 0 = game mask only
    float GameMaskBias;       // compose: same scale OptiScaler applies to DLSS.Input.Bias.Current.Color.Mask
    float SpecHitScale;       // prepass: multiplier for the game's specular hit distance
    float RoughnessOverride;  // prepass: >= 0 replaces the game's roughness (diagnostic)

    float3 CameraForward;     // world space, for sentinel texels denoised as diffuse
    uint SentinelMode;        // SENTINEL_PASSTHROUGH or SENTINEL_DIFFUSE

    uint CounterSlot;         // prepass: SentinelCounter texel counted this frame, the other one is cleared
    float PreExposure;        // the game's color is multiplied by this (DLSS.Pre.Exposure), signals are divided by it
    float2 Pad3;
};

#define SENTINEL_PASSTHROUGH 0
#define SENTINEL_DIFFUSE 1

#define SPLIT_ALBEDO 0
#define SPLIT_SPECULAR 1
#define SPLIT_ROUGHNESS 2

#define FLAG_SEPARATE_ROUGHNESS 0x1
#define FLAG_HAS_SPEC_HIT_DISTANCE 0x2
#define FLAG_DEPTH_INVERTED 0x4
#define FLAG_PARTICLE_LAYER 0x8     // particle layer (premultiplied, alpha = coverage) goes over the result
#define FLAG_COUNT_SENTINELS 0x10   // prepass: count sentinel texels (only while the probe samples)
#define FLAG_LINEAR_DEPTH 0x20      // the game's depth is already linear view Z (DLSS.Use.HW.Depth 0)
#define FLAG_TRANSPARENCY_LAYER 0x40 // prepass got DLSSD.ColorBeforeTransparency, compose adds Color - that back
#define FLAG_NORMALS_UNORM 0x80     // the game's normals are UNORM: xyz = 2 * v - 1
#define FLAG_SSS_GUIDE 0x100        // prepass: DLSSD.ScreenSpaceSubsurfaceScatteringGuide marks skin
#define FLAG_LAYER_OPACITY 0x200    // compose: the layer's coverage comes from DLSS.TransparencyLayerOpacity
#define FLAG_DIFFUSE_HIT 0x400      // prepass: the game sends a diffuse hit distance (DiffuseHitDistance texture)
#define FLAG_DIFFUSE_HIT_IN_W 0x800 // ... packed in .w (DLSSD.DiffuseRayDirectionHitDistance), else in .r

// Packed normals alpha: material type 0-3 / 3, the denoiser doesn't filter across different materials
#define MATERIAL_SURFACE 0.0f            // type 0
#define MATERIAL_SKIN (1.0f / 3.0f)      // type 1, texels the game's SSS guide marks (skin), kept apart
#define MATERIAL_SENTINEL 1.0f           // type 3, sentinel texels, kept apart from real surfaces; compose reads it back

// Game's specular hit distance when the ray missed: 2000 with Cyberpunk's default path tracing, 10000 with ReSTIR
// DI presets. Anything from FarHitDistance up is sent as a miss (real hits that far reproject like a miss anyway).
static const float FarHitDistance = 1999.0f;
static const float MaxHitDistance = 65504.0f;

static const float3 LumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
static const float MinAlbedo = 0.02f;     // per channel, keeps the demodulated signal bounded
static const float MinTotalAlbedo = 0.004f;

// hwDepth is the game's depth texel: HW depth, or linear view Z with FLAG_LINEAR_DEPTH
bool IsSky(float hwDepth)
{
    if (Flags & FLAG_LINEAR_DEPTH)
        return hwDepth <= 0.0f || hwDepth >= SkyDepth;

    return (Flags & FLAG_DEPTH_INVERTED) ? hwDepth <= 0.0f : hwDepth >= 1.0f;
}

// HW depth -> view space Z for a row-vector projection (clip.w = view.z): d = ProjZZ + ProjWZ / viewZ
float LinearDepth(float hwDepth)
{
    if (IsSky(hwDepth))
        return SkyDepth;

    if (Flags & FLAG_LINEAR_DEPTH)
        return hwDepth;

    float denom = hwDepth - ProjZZ;

    if (abs(denom) < 1e-7f)
        return SkyDepth;

    return min(ProjWZ / denom, SkyDepth);
}

// Modulations used to demodulate (prepass) and remodulate (compose) each signal, and the diffuse share of the
// albedo. Returns false when the pixel should keep the game's color.
bool GetModulation(float3 diffuseAlbedo, float3 specularAlbedo, float hwDepth, out float3 dMod, out float3 sMod,
                   out float albedoRatio)
{
    dMod = max(diffuseAlbedo, MinAlbedo);
    sMod = SplitMode == SPLIT_ALBEDO ? max(specularAlbedo, MinAlbedo) : max(diffuseAlbedo + specularAlbedo, MinAlbedo);
    albedoRatio = 0.0f;

    if (IsSky(hwDepth))
        return false;

    float lumD = dot(diffuseAlbedo, LumaWeights);
    float lumS = dot(specularAlbedo, LumaWeights);
    float total = lumD + lumS;

    if (total < MinTotalAlbedo)
        return false;

    albedoRatio = saturate(lumD / total);
    return true;
}

// Share of the color sent to the diffuse signal
float DiffuseWeight(float albedoRatio, float roughness)
{
    if (SplitMode == SPLIT_SPECULAR)
        return 0.0f;

    if (SplitMode == SPLIT_ROUGHNESS)
        return albedoRatio * smoothstep(0.15f, 0.6f, roughness);

    return albedoRatio;
}

// Cyberpunk writes these texels without G-buffer data (V's own body, first person arms and weapon, some far
// proxies): normal (-1, -1, -1) normalized, w (roughness) 0 and white diffuse and specular albedo. All three are
// required so a real white, perfectly smooth surface facing that way isn't caught.
bool IsSentinel(float4 normals, float3 diffuseAlbedo, float3 specularAlbedo)
{
    return all(abs(normals.xyz + 0.57735f) < 0.01f) && normals.w <= 1e-4f && all(diffuseAlbedo > 0.99f) &&
           all(specularAlbedo > 0.99f);
}

// The game's normals texel with xyz in [-1, 1]; roughness (w) is left as stored
float4 DecodeNormals(float4 n)
{
    if (Flags & FLAG_NORMALS_UNORM)
        n.xyz = n.xyz * 2.0f - 1.0f;

    return n;
}

float2 NormalToOctahedronUv(float3 N)
{
    N.xy /= abs(N.x) + abs(N.y) + abs(N.z);
    float2 k = sign(N.xy);
    float s = saturate(-N.z);
    N.xy = lerp(N.xy, (1.0 - abs(N.yx)) * k, s);
    return N.xy * 0.5 + 0.5;
}

bool IsFinite3(float3 v)
{
    return all(!isnan(v)) && all(!isinf(v));
}

// Cyberpunk's particle layer (DLSSD.ColorBeforeParticles): steam, smoke and light glows premultiplied over black,
// alpha = coverage. Puts it over c and raises the reactive mask where it covers.
float3 OverParticleLayer(float3 c, float4 layer, inout float reactive)
{
    if (!IsFinite3(layer.rgb) || isnan(layer.a))
        return c;

    const float coverage = saturate(layer.a);
    reactive = max(reactive, coverage);
    return max(c * (1.0f - coverage) + layer.rgb, 0.0f);
}
