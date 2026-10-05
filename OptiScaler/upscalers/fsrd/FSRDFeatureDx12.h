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
#include <atomic>
#include <chrono>

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
    ID3D12Resource* _sentinelCounter = nullptr; // 2x1 R32_UINT, sentinel texels per frame, ping-ponged
    uint32_t _sentinelFrame = 0;
    int _lastSentinelMode = -1;
    bool _wasBypassed = false;

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
    uint32_t _probeRound = 0;
    int _probeSlotCapture[2] {};   // capture id each slot was copied for, 0 = regular probe
    uint32_t _probePixels[2] {};   // render area of each round, for the sentinel percentage
    uint32_t _probeCounterSlot = 0; // SentinelCounter texel the current frame counted into
    float _captureSentinelMin = 0.0f, _captureSentinelMax = 0.0f, _captureSentinelSum = 0.0f;
    int _captureSentinelCount = 0;

    // Timed capture started from the menu: faster sampling, tagged log lines and a per point summary at the end
    struct ProbeStats
    {
        int count = 0;
        float hitMin = 1e30f, hitMax = -1e30f;
        float depthMin = 1e30f, depthMax = -1e30f;
        float roughMin = 1e30f, roughMax = -1e30f;
        float alphaMin = 1e30f, alphaMax = -1e30f;
        float normalSum[3] {};
        float viewNormalSum[3] {};
    };

    std::atomic<int> _captureRequest { -1 }; // label index, set by the menu thread
    std::atomic<int64_t> _captureStartNs { 0 };
    std::atomic<int64_t> _captureEndNs { 0 };
    std::atomic<int64_t> _captureDoneUntilNs { 0 };
    std::atomic<int> _captureShownLabel { 0 }; // label of the pending/running/last capture, for the overlay
    std::atomic<int> _captureId { 0 };
    int _captureLabel = 0;
    bool _captureActive = false;
    ProbeStats _captureStats[5];
    void FinishProbeCapture();
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

    // Diagnostic capture of the probe for ProbeCaptureSeconds, started from the menu
    static constexpr const char* ProbeCaptureLabels[] = { "still", "floor", "horizon", "walking", "free" };
    static constexpr int ProbeCaptureLabelCount = 5;
    static constexpr float ProbeCaptureSeconds = 10.0f;
    static constexpr float ProbeCaptureDelaySeconds = 2.0f; // time to close the menu before sampling starts
    static constexpr float ProbeCaptureDoneSeconds = 3.0f;  // how long the overlay says the capture is done

    // Starts ProbeCaptureDelaySeconds from now, so the menu can be closed first
    void StartProbeCapture(int labelIndex)
    {
        _captureShownLabel.store(labelIndex);
        _captureStartNs.store(NowNs() + SecondsToNs(ProbeCaptureDelaySeconds));
        _captureRequest.store(labelIndex);
    }

    int ProbeCaptureCount() const { return _captureId.load(); }
    const char* ProbeCaptureLabel() const { return ProbeCaptureLabels[_captureShownLabel.load()]; }

    // Seconds until a requested capture starts sampling, 0 when none is pending
    float ProbeCaptureCountdown() const
    {
        return _captureRequest.load() >= 0 ? NsToSeconds(_captureStartNs.load() - NowNs()) : 0.0f;
    }

    bool ProbeCapturePending() const { return _captureRequest.load() >= 0; }

    // Seconds left in the running capture, 0 when none is running
    float ProbeCaptureRemaining() const
    {
        return ProbeCapturePending() ? 0.0f : NsToSeconds(_captureEndNs.load() - NowNs());
    }

    // True for a few seconds after a capture ends
    bool ProbeCaptureJustDone() const { return NowNs() < _captureDoneUntilNs.load(); }

  private:
    static int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static int64_t SecondsToNs(float seconds) { return (int64_t) ((double) seconds * 1e9); }
    static float NsToSeconds(int64_t ns) { return ns > 0 ? (float) ((double) ns / 1e9) : 0.0f; }

  public:

    FSRDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~FSRDFeatureDx12();
};
