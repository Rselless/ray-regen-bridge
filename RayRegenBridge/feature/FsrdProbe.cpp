#include "pch.h"
#include "FsrdProbe.h"
#include "FsrdUtil.h"

#include <Config.h>
#include <RayRegenBridge/shaders/fsrd_probe_Shader.h>

// Screen points (fractions of the render size, same as fsrd_probe.hlsl) and the RGBA32F texels per point that
// shader writes: (hit, depth, roughness, material), game normals, diffuse albedo, specular albedo
constexpr float ProbePoints[][2] = { { 0.5f, 0.5f }, { 0.5f, 0.8f }, { 0.3f, 0.7f }, { 0.7f, 0.7f }, { 0.5f, 0.95f } };
constexpr UINT ProbePointCount = 5;
constexpr UINT ProbeTexelsPerPoint = 4;
constexpr DXGI_FORMAT ProbeTexelFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr UINT ProbeRowPitch = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // 512 >= 20 texels * 16 bytes

// Per readback slot: the probe texels row, then one texel with the frame's sentinel count
constexpr UINT ProbeCounterOffset = ProbeRowPitch;
constexpr UINT ProbeSlotSize = ProbeCounterOffset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

// Frames between rounds: periodic (~2 s) and during a capture (~7 samples/s at 60 fps, still several frames for
// the GPU copy)
constexpr uint32_t ProbeInterval = 120;
constexpr uint32_t ProbeCaptureInterval = 8;

FsrdProbe::FsrdProbe(ID3D12Device* device) : _device(device)
{
    _pass = std::make_unique<FsrdPass_Dx12>("FSRD Probe", device, 6, 1, fsrd_probe_cso, sizeof(fsrd_probe_cso));
}

FsrdProbe::~FsrdProbe()
{
    if (State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_readback);
    SAFE_RELEASE(_texels);
}

bool FsrdProbe::Sampling() const
{
    return _captureActive || CapturePending() || Config::Instance()->FsrdDiagnostics.value_or_default();
}

bool FsrdProbe::CreateResources()
{
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(ProbeSlotSize * 2);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    if (SUCCEEDED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&_readback))) &&
        Fsrd::CreateScratchTexture(_device, ProbePointCount * ProbeTexelsPerPoint, 1, ProbeTexelFormat,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"FSRD_ProbeTexels", &_texels))
    {
        return true;
    }

    LOG_ERROR("Can't create the probe resources");
    SAFE_RELEASE(_readback);
    SAFE_RELEASE(_texels);
    return false;
}

void FsrdProbe::UpdateCapture()
{
    const int64_t nowNs = NowNs();

    // A requested capture starts after the menu delay
    int request = -1;

    if (_captureRequest.load() >= 0 && nowNs >= _captureStartNs.load())
        request = _captureRequest.exchange(-1);

    if (request >= 0 && request < CaptureLabelCount)
    {
        if (_captureActive)
            FinishCapture();

        _captureLabel = request;
        _captureId.fetch_add(1);

        for (auto& st : _captureStats)
            st = Stats {};

        _sentinelMin = 1e30f;
        _sentinelMax = -1e30f;
        _sentinelSum = 0.0f;
        _sentinelCount = 0;

        _captureActive = true;
        _frame = 0;
        _captureDoneUntilNs.store(0);
        _captureEndNs.store(nowNs + SecondsToNs(CaptureSeconds));

        LOG_INFO("=== Probe capture #{} '{}' start ({} s) ===", _captureId.load(), CaptureLabels[_captureLabel],
                 CaptureSeconds);
    }

    if (_captureActive && nowNs >= _captureEndNs.load())
        FinishCapture();
}

void FsrdProbe::FinishCapture()
{
    const int id = _captureId.load();
    const char* label = CaptureLabels[_captureLabel];

    LOG_INFO("=== Probe capture #{} '{}' summary ===", id, label);

    for (UINT i = 0; i < ProbePointCount; i++)
    {
        const auto& st = _captureStats[i];

        if (st.count == 0)
        {
            LOG_INFO("[cap #{} {}] point ({:.2f}, {:.2f}): no samples", id, label, ProbePoints[i][0], ProbePoints[i][1]);
            continue;
        }

        const float c = (float) st.count;
        LOG_INFO("[cap #{} {}] point ({:.2f}, {:.2f}) {} samples: hit distance {:.3f}..{:.3f}, depth {:.3f}..{:.3f}, "
                 "roughness used {:.3f}..{:.3f}, game normal w {:.3f}..{:.3f}, mean normal world ({:.3f}, {:.3f}, "
                 "{:.3f}) view ({:.3f}, {:.3f}, {:.3f})",
                 id, label, ProbePoints[i][0], ProbePoints[i][1], st.count, st.hitMin, st.hitMax, st.depthMin,
                 st.depthMax, st.roughMin, st.roughMax, st.alphaMin, st.alphaMax, st.normalSum[0] / c,
                 st.normalSum[1] / c, st.normalSum[2] / c, st.viewNormalSum[0] / c, st.viewNormalSum[1] / c,
                 st.viewNormalSum[2] / c);
    }

    if (_sentinelCount > 0)
    {
        LOG_INFO("[cap #{} {}] sentinel pixels: min {:.2f}%, avg {:.2f}%, max {:.2f}% over {} samples", id, label,
                 _sentinelMin, _sentinelSum / _sentinelCount, _sentinelMax, _sentinelCount);
    }

    LOG_INFO("=== Probe capture #{} '{}' end ===", id, label);

    _captureActive = false;
    _captureEndNs.store(0);
    _captureDoneUntilNs.store(NowNs() + SecondsToNs(CaptureDoneSeconds));
}

void FsrdProbe::Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                    const Inputs& inputs, const float view[4][4])
{
    UpdateCapture();

    // Outside a capture the periodic log only runs with the Diagnostics option
    if (!_captureActive && !Config::Instance()->FsrdDiagnostics.value_or_default())
    {
        _slotWritten[0] = _slotWritten[1] = false;
        _frame = 0;
        return;
    }

    const uint32_t interval = _captureActive ? ProbeCaptureInterval : ProbeInterval;

    if (++_frame % interval != 0 || _pass == nullptr || !_pass->CanRender() || inputs.sentinelCounter == nullptr)
        return;

    if ((_readback == nullptr || _texels == nullptr) && !CreateResources())
    {
        _frame = 0x7FFFFFFF; // don't try again for a long time
        return;
    }

    const int slot = _round++ % 2;
    const int otherSlot = 1 - slot;

    // The other slot was copied a full interval ago, so that frame's GPU work is done
    if (_slotWritten[otherSlot])
    {
        D3D12_RANGE range { (SIZE_T) otherSlot * ProbeSlotSize, (SIZE_T) (otherSlot + 1) * ProbeSlotSize };
        uint8_t* data = nullptr;

        if (SUCCEEDED(_readback->Map(0, &range, (void**) &data)))
        {
            LogSlot(data + otherSlot * ProbeSlotSize, otherSlot);

            D3D12_RANGE noWrite { 0, 0 };
            _readback->Unmap(0, &noWrite);
        }
    }

    // This round: the shader reads the inputs at the points into _texels, then _texels and this frame's sentinel
    // count go to the readback buffer
    if (!_pass->Dispatch(cmdList,
                         { inputs.specularHitDistance, inputs.normals, inputs.diffuseAlbedo, inputs.specularAlbedo,
                           inputs.linearDepth, inputs.packedNormals },
                         { _texels }, constants, ProbePointCount, 1))
    {
        return;
    }

    const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto copy = D3D12_RESOURCE_STATE_COPY_SOURCE;

    Fsrd::Barriers(cmdList, { { _texels, uav, copy }, { inputs.sentinelCounter, uav, copy } });

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = _readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = slot * ProbeSlotSize;
    dst.PlacedFootprint.Footprint = { ProbeTexelFormat, ProbePointCount * ProbeTexelsPerPoint, 1, 1, ProbeRowPitch };

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = _texels;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    dst.PlacedFootprint.Offset = slot * ProbeSlotSize + ProbeCounterOffset;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
    src.pResource = inputs.sentinelCounter;

    D3D12_BOX box { inputs.counterSlot, 0, 0, inputs.counterSlot + 1, 1, 1 };
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    Fsrd::Barriers(cmdList, { { _texels, copy, uav }, { inputs.sentinelCounter, copy, uav } });

    _slotPixels[slot] = constants.RenderWidth * constants.RenderHeight;
    _slotWritten[slot] = true;
    memcpy(_slotView[slot], view, sizeof(_slotView[slot]));
    _slotCapture[slot] = _captureActive ? _captureId.load() : 0;
}

void FsrdProbe::LogSlot(const uint8_t* data, int slot)
{
    // Only data copied during the running capture counts for its summary and gets its tag
    const int slotCapture = _slotCapture[slot];
    const bool inCapture = _captureActive && slotCapture == _captureId.load();
    const std::string tag = inCapture ? std::format("[cap #{} {}] ", slotCapture, CaptureLabels[_captureLabel]) : "";

    // Camera axes in world space are the columns of the row-vector view rotation
    const auto& v = _slotView[slot];
    LOG_INFO("{}Probe camera: right ({:.3f}, {:.3f}, {:.3f}), up ({:.3f}, {:.3f}, {:.3f}), "
             "forward ({:.3f}, {:.3f}, {:.3f})",
             tag, v[0][0], v[1][0], v[2][0], v[0][1], v[1][1], v[2][1], v[0][2], v[1][2], v[2][2]);

    for (UINT i = 0; i < ProbePointCount; i++)
    {
        float t[ProbeTexelsPerPoint][4] {};
        memcpy(t, data + i * sizeof(t), sizeof(t));

        const float hitDistance = t[0][0], linearDepth = t[0][1], rough = t[0][2];
        const float* n = t[1];
        const float* diffuse = t[2];
        const float* specular = t[3];

        LOG_INFO("{}Probe ({:.2f}, {:.2f}): spec hit distance: {}, linear depth: {:.3f}, roughness used: {:.3f}, "
                 "material: {:.0f}",
                 tag, ProbePoints[i][0], ProbePoints[i][1],
                 hitDistance >= 0.0f ? std::to_string(hitDistance) : std::string("n/a"), linearDepth, rough, t[0][3]);

        // If the game's normals are world space, this is them in view space: n * R (row vector)
        const float nv[3] = { n[0] * v[0][0] + n[1] * v[1][0] + n[2] * v[2][0],
                              n[0] * v[0][1] + n[1] * v[1][1] + n[2] * v[2][1],
                              n[0] * v[0][2] + n[1] * v[1][2] + n[2] * v[2][2] };

        LOG_INFO("{}    game normals: ({:.3f}, {:.3f}, {:.3f}, {:.3f}) len {:.3f}, as view space: "
                 "({:.3f}, {:.3f}, {:.3f}), diffuse albedo: ({:.3f}, {:.3f}, {:.3f}), specular albedo: ({:.3f}, "
                 "{:.3f}, {:.3f})",
                 tag, n[0], n[1], n[2], n[3], sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), nv[0], nv[1], nv[2],
                 diffuse[0], diffuse[1], diffuse[2], specular[0], specular[1], specular[2]);

        if (!inCapture)
            continue;

        auto& st = _captureStats[i];

        st.count++;
        st.hitMin = std::min(st.hitMin, hitDistance);
        st.hitMax = std::max(st.hitMax, hitDistance);
        st.depthMin = std::min(st.depthMin, linearDepth);
        st.depthMax = std::max(st.depthMax, linearDepth);
        st.roughMin = std::min(st.roughMin, rough);
        st.roughMax = std::max(st.roughMax, rough);
        st.alphaMin = std::min(st.alphaMin, n[3]);
        st.alphaMax = std::max(st.alphaMax, n[3]);

        for (int c = 0; c < 3; c++)
        {
            st.normalSum[c] += n[c];
            st.viewNormalSum[c] += nv[c];
        }
    }

    uint32_t sentinels = 0;
    memcpy(&sentinels, data + ProbeCounterOffset, sizeof(sentinels));

    if (_slotPixels[slot] == 0)
        return;

    const float percent = 100.0f * (float) sentinels / (float) _slotPixels[slot];
    LOG_INFO("{}Probe sentinel pixels: {:.2f}% ({} of {})", tag, percent, sentinels, _slotPixels[slot]);

    if (inCapture)
    {
        _sentinelMin = std::min(_sentinelMin, percent);
        _sentinelMax = std::max(_sentinelMax, percent);
        _sentinelSum += percent;
        _sentinelCount++;
    }
}
