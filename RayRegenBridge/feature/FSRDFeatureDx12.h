#pragma once
// FSRDFeatureDx12 -- serves DLSS Ray Reconstruction (DLSSD/NGX) requests with AMD FSR Ray Regeneration on GPUs
// without real DLSS-D (e.g. AMD), for games that only ship DLSS-RR (Cyberpunk 2077, The Witcher 3 Remastered).
//
// With RR enabled the game turns its own denoisers off and sends the raw noisy color. Per frame:
//   1. Prepass (fsrd_prepass.hlsl): splits that color into demodulated diffuse/specular signals by albedo, and
//      builds linear depth and octahedral normals+roughness, all into scratch resources owned by this class.
//   2. Denoise: FSR Ray Regeneration (amd_fidelityfx_denoiser_dx12.dll, ffx_denoiser.h 1.2.0), scratch in/out only.
//   3. Compose (fsrd_compose.hlsl): remodulates the denoised signals into a scratch color texture and puts the
//      game's particle layer (DLSSD.ColorBeforeParticles) over it.
//   4. Upscale: the regular FFX upscaler path (FFXFeatureDx12::EvaluateInternal) with Color pointed at the composed
//      texture, writing the game's Output. If any step fails the game's own color is upscaled instead.
// No game resource is ever written except Output. [FSRD] EnableDenoiser=false skips steps 1-3.
//
// Resource keys come from NVIDIA's public nvsdk_ngx_defs_dlssd.h / nvsdk_ngx_helpers_dlssd_d3d.h.

#include <upscalers/ffx/FFXFeature_Dx12.h>
#include "FsrdPass_Dx12.h"
#include "FsrdProbe.h"
#include <array>
#include <memory>
#include <vector>

class FSRDFeatureDx12 : public FFXFeatureDx12
{
  private:
    ffxContext _denoiserContext = nullptr;
    bool _denoiserContextCreated = false;
    bool _denoiserDisabled = false; // set by config or after repeated failures
    int _denoiserFailures = 0;
    bool _resetDenoiserHistory = true;
    uint32_t _denoiserFrameIndex = 0;

    // Render size the scratch textures and the denoiser context are created for, grown by ResizeForRenderSize
    unsigned int _maxRenderWidth = 0;
    unsigned int _maxRenderHeight = 0;

    // From creation params (DLSS.Roughness.Mode / DLSS.Use.HW.Depth)
    bool _roughnessPacked = false;
    bool _hwDepth = false;

    // Context creation options, switching one from the menu recreates the context (UpdateContextOptions)
    bool _debugView = false;     // [FSRD] DebugView: FFX_DENOISER_ENABLE_DEBUGGING
    bool _validation = false;    // FFX_DENOISER_ENABLE_VALIDATION, messages to the log
    bool _directSignals = false; // [FSRD] SignalType 1: direct diffuse/specular instead of indirect

    // Last frame's menu modes, a change resets the history or is logged
    int _lastSplitMode = -1;
    int _lastSentinelMode = -1;
    int _lastParticleLayer = -1;
    int _lastTransparencyLayer = -1;
    float _lastPreExposure = 0.0f;
    bool _wasBypassed = false;

    // Previous frame's WorldToViewMatrix, for cameraPositionDelta and the MV depth delta
    bool _hasPrevView = false;
    float _prevView[4][4] {};

    // Tuning values last sent with ffxConfigure (a flag, not NaN: the project builds with /fp:fast)
    float _appliedTuning[4] {};
    bool _tuningSent[4] {};

    // Scratch resources, all owned by this class (no aliasing between inputs and outputs)
    ID3D12Resource* _diffuseIn = nullptr;
    ID3D12Resource* _diffuseOut = nullptr;
    ID3D12Resource* _specularIn = nullptr;
    ID3D12Resource* _specularOut = nullptr;
    ID3D12Resource* _linearDepth = nullptr;
    ID3D12Resource* _packedNormals = nullptr;
    ID3D12Resource* _motionVectors = nullptr;   // game MVs in UV + linear depth delta in B
    ID3D12Resource* _composedColor = nullptr;   // same size/format as the game's color, recreated when it changes
    ID3D12Resource* _reactiveMask = nullptr;    // R8, same size as the composed color, given to FSR as FSR.reactive
    ID3D12Resource* _bypassColor = nullptr;     // bypass: game color + particle layer, created when first used
    ID3D12Resource* _sentinelCounter = nullptr; // 2x1 R32_UINT, sentinel texels per frame, ping-ponged
    uint32_t _sentinelFrame = 0;
    uint32_t _sentinelSlot = 0; // SentinelCounter texel the current frame counts into

    std::unique_ptr<FsrdPass_Dx12> _prepass;
    std::unique_ptr<FsrdPass_Dx12> _compose;
    std::unique_ptr<FsrdPass_Dx12> _particlesPass; // particle layer over the game's color (bypass)
    std::unique_ptr<FsrdProbe> _probe; // diagnostics, null when the denoiser is off

    // Replaced resources and denoiser contexts, released RetireFrames evaluations later (the GPU can still be
    // running work that uses them)
    struct RetiredObject
    {
        ID3D12Resource* resource = nullptr;
        ffxContext context = nullptr;
        uint32_t frame = 0;
    };

    std::vector<RetiredObject> _retired;
    uint32_t _evalFrame = 0;

    bool _loggedInputsOnce = false;

    // For the VRAM usage / budget log
    IDXGIAdapter3* _adapter = nullptr;
    void LogVideoMemory(const char* when);

    // Everything one Denoise() call works with
    struct FrameInputs
    {
        ID3D12Resource* color = nullptr;
        ID3D12Resource* motionVectors = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* normals = nullptr;
        ID3D12Resource* roughness = nullptr;
        ID3D12Resource* diffuseAlbedo = nullptr;
        ID3D12Resource* specularAlbedo = nullptr;
        ID3D12Resource* specularHitDistance = nullptr;
        ID3D12Resource* particles = nullptr; // DLSSD.ColorBeforeParticles when the particle layer is on
        ID3D12Resource* beforeTransparency = nullptr; // DLSSD.ColorBeforeTransparency when the transparency layer is on
        ID3D12Resource* layerOpacity = nullptr; // DLSS.TransparencyLayerOpacity when particles is DLSS.TransparencyLayer
        ID3D12Resource* sssGuide = nullptr;     // DLSSD.ScreenSpaceSubsurfaceScatteringGuide (skin)
        ID3D12Resource* diffuseHit = nullptr;   // DLSSD.DiffuseHitDistance, or DiffuseRayDirectionHitDistance (.w)
        bool diffuseHitInW = false;
        ID3D12Resource* emissive = nullptr;     // GBuffer.Emissive, kept out of the denoiser
        ID3D12Resource* ghostMask = nullptr;    // DLSS.Input.Reduce.Ghost.Mask, added to the reactive mask
        bool normalsUnorm = false;
        float preExposure = 1.0f;
        unsigned int renderWidth = 0;
        unsigned int renderHeight = 0;
        float view[4][4] {};       // WorldToViewMatrix, row-vector
        float projection[4][4] {}; // ViewToClipMatrix, row-vector
        bool hasView = false;
        bool separateRoughness = false;
    };

    bool CreateDenoiserContext();
    void UpdateContextOptions();
    static void ValidationMessage(uint32_t type, const wchar_t* message);
    bool CreateScratchResources();
    bool ResizeForRenderSize(unsigned int renderWidth, unsigned int renderHeight);
    std::array<ID3D12Resource**, 11> ScratchResources();
    bool EnsureDenoiser(unsigned int renderWidth, unsigned int renderHeight);
    bool PrepareComposedColor(ID3D12Resource* color);
    void DisableDenoiserOnFailure(const char* reason);
    void ApplyTuning();

    void Retire(ID3D12Resource*& resource);
    void RetireDenoiserContext();
    void ReleaseRetired();
    static void ReleaseRetiredObject(RetiredObject& item);

    void LogInputsOnce(NVSDK_NGX_Parameter* InParameters);
    void ProcessInitParams(NVSDK_NGX_Parameter* InParameters);

    ID3D12Resource* GetParticleLayer(NVSDK_NGX_Parameter* InParameters, ID3D12Resource* color);
    ID3D12Resource* GetTransparencyLayer(NVSDK_NGX_Parameter* InParameters, ID3D12Resource* color);
    ID3D12Resource* BypassWithParticles(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

    // Denoise() steps, each returns false (or the fallback) after logging a failure
    bool GatherInputs(NVSDK_NGX_Parameter* InParameters, FrameInputs& in);
    FsrdPass_Dx12::Constants BuildConstants(const FrameInputs& in, NVSDK_NGX_Parameter* InParameters);
    bool RunPrepass(ID3D12GraphicsCommandList* InCommandList, const FrameInputs& in,
                    const FsrdPass_Dx12::Constants& constants);
    bool RunDenoiser(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                     const FrameInputs& in);
    bool RunCompose(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                    const FrameInputs& in, const FsrdPass_Dx12::Constants& constants);
    void RunProbe(ID3D12GraphicsCommandList* InCommandList, const FrameInputs& in,
                  const FsrdPass_Dx12::Constants& constants);

    // Returns the composed color to upscale, or nullptr to upscale the game's color as is
    ID3D12Resource* Denoise(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    // Version() stays the FFX upscaler's version on purpose, FFXFeatureDx12::EvaluateInternal uses it for FSR4 WARs
    Upscaler GetUpscalerType() const override { return Upscaler::FSRD; }

    // For the menu: false when the denoiser is off (config, missing dll, init or dispatch failures)
    bool DenoiserActive() const { return !_denoiserDisabled && _denoiserContextCreated; }

    // For the menu: input probe and timed captures, null when the denoiser is off
    FsrdProbe* Probe() const { return _probe.get(); }

    FSRDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~FSRDFeatureDx12();
};
