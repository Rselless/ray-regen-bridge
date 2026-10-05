#pragma once
// FSRDFeatureDx12 -- serves DLSS Ray Reconstruction (DLSSD/NGX) requests with AMD FSR Ray Regeneration on GPUs
// without real DLSS-D (e.g. AMD), for games that only ship DLSS-RR (Cyberpunk 2077, The Witcher 3 Remastered).
//
// With RR enabled the game turns its own denoisers off and sends the raw noisy color. Per frame:
//   1. Prepass (fsrd_prepass.hlsl): splits that color into demodulated diffuse/specular signals by albedo, and
//      builds linear depth and octahedral normals+roughness, all into scratch resources owned by this class.
//   2. Denoise: FSR Ray Regeneration (amd_fidelityfx_denoiser_dx12.dll, ffx_denoiser.h 1.2.0), scratch in/out only.
//   3. Compose (fsrd_compose.hlsl): remodulates the denoised signals into a scratch color texture.
//   4. Upscale: the regular FFX upscaler path (FFXFeatureDx12::EvaluateInternal) with Color pointed at the composed
//      texture, writing the game's Output. If any step fails the game's own color is upscaled instead.
// No game resource is ever written except Output. [FSRD] EnableDenoiser=false skips steps 1-3.
//
// Resource keys come from NVIDIA's public nvsdk_ngx_defs_dlssd.h / nvsdk_ngx_helpers_dlssd_d3d.h.

#include <upscalers/ffx/FFXFeature_Dx12.h>
#include <shaders/fsrd/FsrdPass_Dx12.h>
#include <memory>

class FSRDFeatureDx12 : public FFXFeatureDx12
{
  private:
    ffxContext _denoiserContext = nullptr;
    bool _denoiserContextCreated = false;
    bool _denoiserDisabled = false; // set by config or after repeated failures
    int _denoiserFailures = 0;
    bool _resetDenoiserHistory = true;
    uint32_t _denoiserFrameIndex = 0;
    unsigned int _maxRenderWidth = 0;
    unsigned int _maxRenderHeight = 0;

    // From creation params (DLSS.Roughness.Mode / DLSS.Use.HW.Depth)
    bool _roughnessPacked = false;
    bool _hwDepth = false;

    // [FSRD] DebugView, needs the context created with debugging so it's only read at init
    bool _debugView = false;

    // Split mode of the last frame, the history is reset when it changes from the menu
    int _lastSplitMode = -1;

    // Previous frame's WorldToViewMatrix, for cameraPositionDelta and the MV depth delta
    bool _hasPrevView = false;
    float _prevView[4][4] {};

    // Tuning values last sent with ffxConfigure (a flag, not NaN: the project builds with /fp:fast)
    float _appliedTuning[3] {};
    bool _tuningSent[3] {};

    // Scratch resources, all owned by this class (no aliasing between inputs and outputs)
    ID3D12Resource* _diffuseIn = nullptr;
    ID3D12Resource* _diffuseOut = nullptr;
    ID3D12Resource* _specularIn = nullptr;
    ID3D12Resource* _specularOut = nullptr;
    ID3D12Resource* _linearDepth = nullptr;
    ID3D12Resource* _packedNormals = nullptr;
    ID3D12Resource* _motionVectors = nullptr; // game MVs in UV + linear depth delta in B
    ID3D12Resource* _composedColor = nullptr; // same size/format as the game's color, recreated when it changes
    ID3D12Resource* _reactiveMask = nullptr;  // R8, same size as the composed color, given to FSR as FSR.reactive

    std::unique_ptr<FsrdPass_Dx12> _prepass;
    std::unique_ptr<FsrdPass_Dx12> _compose;

    bool _loggedInputsOnce = false;

    bool CreateDenoiserContext();
    void DestroyDenoiserContext();
    bool CreateScratchResources();
    void ReleaseScratchResources();
    bool PrepareComposedColor(ID3D12Resource* color);
    void DisableDenoiserOnFailure(const char* reason);
    void ApplyTuning();

    // Logs the game's specular hit distance, our linear depth and roughness at a few screen points every ~2 s,
    // to check the hit distance's units. Copies 1 texel per point into a readback buffer, read 2 rounds later.
    ID3D12Resource* _probeReadback = nullptr;
    uint32_t _probeFrame = 0;
    bool _probeSlotWritten[2] {};
    bool _probeHasHitDistance[2] {};
    float _probeView[2][4][4] {}; // WorldToViewMatrix of each round, to tell world from view space normals
    void ProbeInputs(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* specularHitDistance, ID3D12Resource* normals,
                     ID3D12Resource* diffuseAlbedo, ID3D12Resource* specularAlbedo, const float view[4][4],
                     unsigned int renderWidth, unsigned int renderHeight);

    void LogInputsOnce(NVSDK_NGX_Parameter* InParameters);
    void ProcessInitParams(NVSDK_NGX_Parameter* InParameters);

    // Returns the composed color to upscale, or nullptr to upscale the game's color as is
    ID3D12Resource* Denoise(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    // Version() stays the FFX upscaler's version on purpose, FFXFeatureDx12::EvaluateInternal uses it for FSR4 WARs
    Upscaler GetUpscalerType() const override { return Upscaler::FSRD; }

    // For the menu: false when the denoiser is off (config, missing dll, init or dispatch failures)
    bool DenoiserActive() const { return !_denoiserDisabled && _denoiserContextCreated; }

    FSRDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~FSRDFeatureDx12();
};
