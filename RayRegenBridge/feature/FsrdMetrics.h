#pragma once
// FsrdMetrics -- live diagnostics for FSRDFeatureDx12: every frame fsrd_metrics.hlsl sums each 64x64 tile of the
// game color, the composed color and the denoiser signals into a small texture, which is read back a few frames
// later. Once per second a summary goes to RRB_Captures\status.txt, metrics.csv and the log: brightness gain and
// color shift of the composed image against the game's (tile means, so noise averages out), clamped signals,
// noise left in the tiles and how much the tiles flicker over the second (meaningful while the camera holds still).
#include "FsrdPass_Dx12.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

class FsrdMetrics
{
  public:
    explicit FsrdMetrics(ID3D12Device* device);
    ~FsrdMetrics();

    static void SetEnabled(bool enabled) { _enabled.store(enabled); }
    static bool Enabled() { return _enabled.load(); }

    // Every frame after compose; composed, diffuseIn, specularIn and packedNormals in NON_PIXEL_SHADER_RESOURCE
    void Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants, ID3D12Resource* color,
             ID3D12Resource* composed, ID3D12Resource* diffuseIn, ID3D12Resource* specularIn,
             ID3D12Resource* packedNormals);

  private:
    static constexpr int Slots = 4; // readback ring, a slot is read Slots - 1 frames after its copy

    // One tile over the current second
    struct TileSeries
    {
        double colorLuma = 0, composedLuma = 0;        // sums of the per frame tile means
        double colorLumaSq = 0, composedLumaSq = 0;    // for the flicker over frames
        double colorRgb[3] {}, composedRgb[3] {};
        double colorVar = 0, composedVar = 0;          // sums of the per frame spatial variances
        double clamped = 0, active = 0, skin = 0, texels = 0;
        int frames = 0;
    };

    ID3D12Device* _device = nullptr;
    std::unique_ptr<FsrdPass_Dx12> _pass;
    ID3D12Resource* _tiles = nullptr;
    ID3D12Resource* _readback[Slots] {};
    UINT _tilesX = 0, _tilesY = 0;
    UINT _rowPitch = 0;
    bool _slotWritten[Slots] {};
    uint32_t _frame = 0;
    std::vector<std::pair<ID3D12Resource*, uint32_t>> _retired; // released at that frame

    static inline std::atomic<bool> _enabled { false }; // process wide: survives the feature being recreated
    std::vector<TileSeries> _series;
    std::chrono::steady_clock::time_point _secondStart {};
    std::chrono::steady_clock::time_point _sessionStart {};
    bool _csvHeader = false;

    bool EnsureResources(UINT renderWidth, UINT renderHeight);
    void ReleaseResources();
    void Accumulate(const uint8_t* data);
    void Summarize();
};
