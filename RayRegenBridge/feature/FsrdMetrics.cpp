#include "pch.h"
#include "FsrdMetrics.h"
#include "FsrdRemote.h"
#include "FsrdUtil.h"

#include <Util.h>
#include <RayRegenBridge/shaders/fsrd_metrics_Shader.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

constexpr UINT MetricsTileSize = 64;
constexpr UINT MetricsTexelsPerTile = 3;
constexpr UINT MetricsBytesPerTexel = 16; // RGBA32F
constexpr double Luma[3] = { 0.2126, 0.7152, 0.0722 };

FsrdMetrics::FsrdMetrics(ID3D12Device* device) : _device(device)
{
    _pass = std::make_unique<FsrdPass_Dx12>("FSRD Metrics", device, 5, 1, fsrd_metrics_cso, sizeof(fsrd_metrics_cso));
}

FsrdMetrics::~FsrdMetrics()
{
    if (State::Instance().isShuttingDown)
        return;

    ReleaseResources();
}

void FsrdMetrics::ReleaseResources()
{
    for (auto& [resource, frame] : _retired)
        resource->Release();

    _retired.clear();
    SAFE_RELEASE(_tiles);

    for (int i = 0; i < Slots; i++)
    {
        SAFE_RELEASE(_readback[i]);
        _slotWritten[i] = false;
    }

    _tilesX = _tilesY = 0;
}

bool FsrdMetrics::EnsureResources(UINT renderWidth, UINT renderHeight)
{
    const UINT tilesX = (renderWidth + MetricsTileSize - 1) / MetricsTileSize;
    const UINT tilesY = (renderHeight + MetricsTileSize - 1) / MetricsTileSize;

    if (_tiles != nullptr && tilesX == _tilesX && tilesY == _tilesY)
        return true;

    // A new render size: frames still in the ring were taken at the old one, the old resources are released once
    // the GPU is done with them
    for (auto* resource : { _tiles, _readback[0], _readback[1], _readback[2], _readback[3] })
    {
        if (resource != nullptr)
            _retired.emplace_back(resource, _frame + Slots * 2);
    }

    _tiles = nullptr;

    for (int i = 0; i < Slots; i++)
    {
        _readback[i] = nullptr;
        _slotWritten[i] = false;
    }

    _series.assign((size_t) tilesX * tilesY, TileSeries {});

    _rowPitch = (tilesX * MetricsTexelsPerTile * MetricsBytesPerTexel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    auto desc = CD3DX12_RESOURCE_DESC::Buffer((UINT64) _rowPitch * tilesY);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    for (int i = 0; i < Slots; i++)
    {
        if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&_readback[i]))))
        {
            ReleaseResources();
            return false;
        }
    }

    if (!Fsrd::CreateScratchTexture(_device, tilesX * MetricsTexelsPerTile, tilesY, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"FSRD_MetricsTiles", &_tiles))
    {
        ReleaseResources();
        return false;
    }

    _tilesX = tilesX;
    _tilesY = tilesY;
    return true;
}

void FsrdMetrics::Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                      ID3D12Resource* color, ID3D12Resource* composed, ID3D12Resource* diffuseIn,
                      ID3D12Resource* specularIn, ID3D12Resource* packedNormals)
{
    // Resources replaced earlier, once the GPU can't be using them anymore
    for (auto it = _retired.begin(); it != _retired.end();)
    {
        if (_frame < it->second)
        {
            ++it;
            continue;
        }

        it->first->Release();
        it = _retired.erase(it);
    }

    // Off: resources stay (the GPU may still be copying), the next session starts its clock again
    if (!_enabled.load())
    {
        _sessionStart = {};
        _frame++;
        return;
    }

    if (_pass == nullptr || !_pass->CanRender() || !EnsureResources(constants.RenderWidth, constants.RenderHeight))
        return;

    const auto now = std::chrono::steady_clock::now();

    if (_sessionStart == std::chrono::steady_clock::time_point {})
    {
        // New session: nothing from before it is summarized
        _sessionStart = now;
        _secondStart = now;
        std::fill(std::begin(_slotWritten), std::end(_slotWritten), false);
        std::fill(_series.begin(), _series.end(), TileSeries {});
    }

    // The slot written Slots - 1 frames ago is done on the GPU
    const int slot = (int) (_frame % Slots);
    const int readSlot = (int) ((_frame + 1) % Slots);

    if (_slotWritten[readSlot])
    {
        void* mapped = nullptr;
        D3D12_RANGE range { 0, (SIZE_T) _rowPitch * _tilesY };

        if (SUCCEEDED(_readback[readSlot]->Map(0, &range, &mapped)))
        {
            Accumulate(static_cast<const uint8_t*>(mapped));
            D3D12_RANGE noWrite { 0, 0 };
            _readback[readSlot]->Unmap(0, &noWrite);
        }

        _slotWritten[readSlot] = false;
    }

    if (now - _secondStart >= std::chrono::seconds(1))
    {
        Summarize();
        _secondStart = now;
    }

    // One group per tile: the pass dispatches width / 16 groups
    if (!_pass->Dispatch(cmdList, { color, composed, diffuseIn, specularIn, packedNormals }, { _tiles }, constants,
                         _tilesX * 16, _tilesY * 16))
    {
        return;
    }

    Fsrd::Barriers(cmdList, { { _tiles, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE } });

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    footprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, _tilesX * MetricsTexelsPerTile, _tilesY, 1, _rowPitch };
    CD3DX12_TEXTURE_COPY_LOCATION dst(_readback[slot], footprint);
    CD3DX12_TEXTURE_COPY_LOCATION src(_tiles, 0);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Fsrd::Barriers(cmdList, { { _tiles, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });

    _slotWritten[slot] = true;
    _frame++;
}

void FsrdMetrics::Accumulate(const uint8_t* data)
{
    for (UINT ty = 0; ty < _tilesY; ty++)
    {
        const float* row = reinterpret_cast<const float*>(data + (size_t) ty * _rowPitch);

        for (UINT tx = 0; tx < _tilesX; tx++)
        {
            const float* c = row + tx * 12;
            const float* o = c + 4;
            const float* m = c + 8;
            const double n = m[3];

            if (n < 1.0)
                continue;

            auto& s = _series[(size_t) ty * _tilesX + tx];
            const double lc = (c[0] * Luma[0] + c[1] * Luma[1] + c[2] * Luma[2]) / n;
            const double lo = (o[0] * Luma[0] + o[1] * Luma[1] + o[2] * Luma[2]) / n;

            s.colorLuma += lc;
            s.composedLuma += lo;
            s.colorLumaSq += lc * lc;
            s.composedLumaSq += lo * lo;
            s.colorVar += std::max(c[3] / n - lc * lc, 0.0);
            s.composedVar += std::max(o[3] / n - lo * lo, 0.0);

            for (int i = 0; i < 3; i++)
            {
                s.colorRgb[i] += c[i] / n;
                s.composedRgb[i] += o[i] / n;
            }

            s.clamped += m[0];
            s.active += m[1];
            s.skin += m[2];
            s.texels += n;
            s.frames++;
        }
    }
}

static double Median(std::vector<double>& values)
{
    if (values.empty())
        return 0.0;

    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

void FsrdMetrics::Summarize()
{
    std::vector<double> gains, shifts, noise, flicker, flickerIn;
    double clamped = 0, active = 0, skin = 0, texels = 0;
    double worstGain = 0;
    UINT worstX = 0, worstY = 0;
    int frames = 0;

    // Dark tiles have meaningless ratios
    std::vector<double> lumas;

    for (const auto& s : _series)
    {
        if (s.frames > 0)
            lumas.push_back(s.colorLuma / s.frames);
    }

    std::vector<double> sorted = lumas;
    const double darkLimit = sorted.empty() ? 0.0 : (std::sort(sorted.begin(), sorted.end()), sorted[sorted.size() / 20]);

    for (UINT i = 0; i < _series.size(); i++)
    {
        auto& s = _series[i];

        if (s.frames == 0)
            continue;

        frames = std::max(frames, s.frames);
        clamped += s.clamped;
        active += s.active;
        skin += s.skin;
        texels += s.texels;

        const double f = s.frames;
        const double lc = s.colorLuma / f, lo = s.composedLuma / f;

        if (lc > std::max(darkLimit, 1e-5))
        {
            const double gain = std::log2(std::max(lo, 1e-6) / lc);
            gains.push_back(gain);

            if (std::abs(gain) > std::abs(worstGain))
            {
                worstGain = gain;
                worstX = (i % _tilesX) * MetricsTileSize;
                worstY = (i / _tilesX) * MetricsTileSize;
            }

            const double sc = s.colorRgb[0] + s.colorRgb[1] + s.colorRgb[2];
            const double so = s.composedRgb[0] + s.composedRgb[1] + s.composedRgb[2];
            double shift = 0;

            for (int k = 0; k < 3; k++)
                shift += std::abs(s.composedRgb[k] / std::max(so, 1e-9) - s.colorRgb[k] / std::max(sc, 1e-9));

            shifts.push_back(shift);
            noise.push_back(std::sqrt(s.composedVar / std::max(s.colorVar, 1e-12)));

            if (s.frames > 2)
            {
                flicker.push_back(std::sqrt(std::max(s.composedLumaSq / f - lo * lo, 0.0)) / std::max(lo, 1e-6));
                flickerIn.push_back(std::sqrt(std::max(s.colorLumaSq / f - lc * lc, 0.0)) / std::max(lc, 1e-6));
            }
        }

        s = TileSeries {};
    }

    if (gains.empty())
        return;

    const size_t n = gains.size();
    const double bright = (double) std::count_if(gains.begin(), gains.end(), [](double g) { return g > std::log2(1.15); }) / n;
    const double dark = (double) std::count_if(gains.begin(), gains.end(), [](double g) { return g < std::log2(0.85); }) / n;
    const double shiftsHigh = (double) std::count_if(shifts.begin(), shifts.end(), [](double s) { return s > 0.05; }) / n;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _sessionStart).count();

    const double medianGain = std::exp2(Median(gains)) - 1.0;
    const double medianShift = Median(shifts);
    const double medianNoise = Median(noise);
    const double medianFlicker = Median(flicker);
    const double medianFlickerIn = Median(flickerIn);
    const double clampShare = active > 0 ? clamped / active : 0.0;
    const double skinShare = texels > 0 ? skin / texels : 0.0;

    // Shares as percentages (std::format has no % presentation type)
    FsrdRemote::Status(std::format(
        "metrics t={:.0f}s frames={} gain {:+.1f}% (tiles >+15% {:.0f}%, <-15% {:.0f}%, worst {:+.0f}% at {},{}) "
        "color shift {:.3f} (tiles >0.05 {:.0f}%) clamped {:.2f}% noise {:.2f} flicker {:.3f} (input {:.3f}) "
        "skin {:.1f}%",
        seconds, frames, medianGain * 100.0, bright * 100.0, dark * 100.0, (std::exp2(worstGain) - 1.0) * 100.0,
        worstX, worstY, medianShift, shiftsHigh * 100.0, clampShare * 100.0, medianNoise, medianFlicker,
        medianFlickerIn, skinShare * 100.0));

    const auto csv = FsrdRemote::Folder() / "metrics.csv";
    std::ofstream out(csv, std::ios::app);

    if (!_csvHeader)
    {
        out << "time_s,frames,gain,tiles_brighter_15,tiles_darker_15,worst_gain,worst_x,worst_y,color_shift,"
               "tiles_shift_005,clamped,noise,flicker,flicker_input,skin\n";
        _csvHeader = true;
    }

    out << std::format("{:.1f},{},{:.4f},{:.4f},{:.4f},{:.4f},{},{},{:.4f},{:.4f},{:.5f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                       seconds, frames, medianGain, bright, dark, std::exp2(worstGain) - 1.0, worstX, worstY,
                       medianShift, shiftsHigh, clampShare, medianNoise, medianFlicker, medianFlickerIn, skinShare);
}
