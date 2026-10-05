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
    float RestoreStrength;    // restore: share of the blurred residual (game - composed) added back, 0 = off
    float RestoreStride;      // restore: pixels between blur taps
    float Pad3;
};

#define SENTINEL_PASSTHROUGH 0
#define SENTINEL_DIFFUSE 1

#define SPLIT_ALBEDO 0
#define SPLIT_SPECULAR 1
#define SPLIT_ROUGHNESS 2

#define FLAG_SEPARATE_ROUGHNESS 0x1
#define FLAG_HAS_SPEC_HIT_DISTANCE 0x2
#define FLAG_DEPTH_INVERTED 0x4
#define FLAG_PARTICLE_LAYER 0x8 // prepass got the color before particles, compose adds the particles back

static const float3 LumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
static const float MinAlbedo = 0.02f;     // per channel, keeps the demodulated signal bounded
static const float MinTotalAlbedo = 0.004f;

bool IsSky(float hwDepth)
{
    return (Flags & FLAG_DEPTH_INVERTED) ? hwDepth <= 0.0f : hwDepth >= 1.0f;
}

// HW depth -> view space Z for a row-vector projection (clip.w = view.z): d = ProjZZ + ProjWZ / viewZ
float LinearDepth(float hwDepth)
{
    float denom = hwDepth - ProjZZ;

    if (IsSky(hwDepth) || abs(denom) < 1e-7f)
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
