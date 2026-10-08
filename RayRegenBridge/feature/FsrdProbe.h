#pragma once
// FsrdProbe -- diagnostics for FSRDFeatureDx12: logs the denoiser inputs at a few screen points, during timed
// captures started from the menu or every ~2 s with [FSRD] Diagnostics. fsrd_probe.hlsl reads them through SRVs
// (game resources keep their state) into a small texture, which is copied into a readback buffer and logged two
// rounds later, when that frame's GPU work is done.
#include "FsrdPass_Dx12.h"
#include <atomic>
#include <chrono>
#include <memory>

class FsrdProbe
{
  public:
    static constexpr const char* CaptureLabels[] = { "still", "floor", "horizon", "walking", "free" };
    static constexpr int CaptureLabelCount = 5;
    static constexpr float CaptureSeconds = 10.0f;
    static constexpr float CaptureDelaySeconds = 2.0f; // time to close the menu before sampling starts
    static constexpr float CaptureDoneSeconds = 3.0f;  // how long the overlay says the capture is done

    // What the probe reads this frame. linearDepth and packedNormals are FSRD's, in NON_PIXEL_SHADER_RESOURCE;
    // sentinelCounter is FSRD's, in UNORDERED_ACCESS, counted into texel counterSlot this frame.
    struct Inputs
    {
        ID3D12Resource* specularHitDistance;
        ID3D12Resource* normals;
        ID3D12Resource* diffuseAlbedo;
        ID3D12Resource* specularAlbedo;
        ID3D12Resource* linearDepth;
        ID3D12Resource* packedNormals;
        ID3D12Resource* sentinelCounter;
        uint32_t counterSlot;
    };

    explicit FsrdProbe(ID3D12Device* device);
    ~FsrdProbe();

    // Records this frame's round when one is due (capture running, or Diagnostics on)
    void Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants, const Inputs& inputs,
             const float view[4][4]);

    // Menu (other thread): starts CaptureDelaySeconds from now, so the menu can be closed first
    void StartCapture(int labelIndex)
    {
        _captureShownLabel.store(labelIndex);
        _captureStartNs.store(NowNs() + SecondsToNs(CaptureDelaySeconds));
        _captureRequest.store(labelIndex);
    }

    int CaptureCount() const { return _captureId.load(); }
    const char* CaptureLabel() const { return CaptureLabels[_captureShownLabel.load()]; }
    bool CapturePending() const { return _captureRequest.load() >= 0; }

    // Seconds until a requested capture starts sampling, 0 when none is pending
    float CaptureCountdown() const
    {
        return CapturePending() ? NsToSeconds(_captureStartNs.load() - NowNs()) : 0.0f;
    }

    // Seconds left in the running capture, 0 when none is running
    float CaptureRemaining() const { return CapturePending() ? 0.0f : NsToSeconds(_captureEndNs.load() - NowNs()); }

    // True for a few seconds after a capture ends
    bool CaptureJustDone() const { return NowNs() < _captureDoneUntilNs.load(); }

    // True while samples are taken or about to be (the prepass only counts sentinel texels then)
    bool Sampling() const;

  private:
    struct Stats
    {
        int count = 0;
        float hitMin = 1e30f, hitMax = -1e30f;
        float depthMin = 1e30f, depthMax = -1e30f;
        float roughMin = 1e30f, roughMax = -1e30f;
        float alphaMin = 1e30f, alphaMax = -1e30f;
        float normalSum[3] {};
        float viewNormalSum[3] {};
    };

    ID3D12Device* _device = nullptr;
    std::unique_ptr<FsrdPass_Dx12> _pass;
    ID3D12Resource* _readback = nullptr;
    ID3D12Resource* _texels = nullptr;

    uint32_t _frame = 0;
    uint32_t _round = 0;
    bool _slotWritten[2] {};
    float _slotView[2][4][4] {}; // WorldToViewMatrix of each round, to tell world from view space normals
    int _slotCapture[2] {};      // capture id each slot was copied for, 0 = periodic round
    uint32_t _slotPixels[2] {};  // render area of each round, for the sentinel percentage

    std::atomic<int> _captureRequest { -1 }; // label index, set by the menu thread
    std::atomic<int64_t> _captureStartNs { 0 };
    std::atomic<int64_t> _captureEndNs { 0 };
    std::atomic<int64_t> _captureDoneUntilNs { 0 };
    std::atomic<int> _captureShownLabel { 0 }; // label of the pending/running/last capture, for the overlay
    std::atomic<int> _captureId { 0 };
    int _captureLabel = 0;
    bool _captureActive = false;
    Stats _captureStats[5];
    float _sentinelMin = 0.0f, _sentinelMax = 0.0f, _sentinelSum = 0.0f;
    int _sentinelCount = 0;

    void UpdateCapture();
    void FinishCapture();
    bool CreateResources();
    void LogSlot(const uint8_t* data, int slot);

    static int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static int64_t SecondsToNs(float seconds) { return (int64_t) ((double) seconds * 1e9); }
    static float NsToSeconds(int64_t ns) { return ns > 0 ? (float) ((double) ns / 1e9) : 0.0f; }
};
