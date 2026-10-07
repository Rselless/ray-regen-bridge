#include "pch.h"
#include "FSRDFeatureDx12.h"

#include <Config.h>
#include <Util.h>
#include <proxies/FfxApi_Proxy.h>

#include <shaders/fsrd/precompile/fsrd_prepass_Shader.h>
#include <shaders/fsrd/precompile/fsrd_compose_Shader.h>
#include <shaders/fsrd/precompile/fsrd_restore_Shader.h>
#include <shaders/fsrd/precompile/fsrd_probe_Shader.h>

// FSR Ray Regeneration API (ffx_denoiser.h 1.2.0), wrapped so it builds on the same ffx_api.h OptiScaler already
// uses -- see ffx_denoiser_compat.h for why the real header can't be included directly. ffxApiGetResourceDX12 comes
// from <dx12/ffx_api_dx12.h>, already included through FfxApi_Proxy.h.
#include "ffx_denoiser_compat.h"

// DLSS Ray Reconstruction NGX keys, from NVIDIA's public nvsdk_ngx_defs_dlssd.h
// (NGX_D3D12_EVALUATE_DLSSD_EXT in nvsdk_ngx_helpers_dlssd_d3d.h sets these).
namespace DlssdKeys
{
constexpr const char* DiffuseAlbedo = "DLSS.Input.DiffuseAlbedo";
constexpr const char* SpecularAlbedo = "DLSS.Input.SpecularAlbedo";
constexpr const char* Normals = "GBuffer.Normals";
constexpr const char* Roughness = "GBuffer.Roughness";
constexpr const char* DiffuseHitDistance = "DLSSD.DiffuseHitDistance";
constexpr const char* SpecularHitDistance = "DLSSD.SpecularHitDistance";
constexpr const char* WorldToViewMatrix = "WorldToViewMatrix"; // float[16]
constexpr const char* ViewToClipMatrix = "ViewToClipMatrix";   // float[16]
constexpr const char* RoughnessMode = "DLSS.Roughness.Mode";   // 0 = separate texture, 1 = normals.w
constexpr const char* UseHWDepth = "DLSS.Use.HW.Depth";        // 0 = linear, 1 = HW depth
constexpr const char* DenoiseMode = "DLSS.Denoise.Mode";
constexpr const char* ColorBeforeParticles = "DLSSD.ColorBeforeParticles";
} // namespace DlssdKeys

// Must match the defines in fsrd_common.hlsli
constexpr uint32_t FlagSeparateRoughness = 0x1;
constexpr uint32_t FlagHasSpecHitDistance = 0x2;
constexpr uint32_t FlagDepthInverted = 0x4;
constexpr uint32_t FlagParticleLayer = 0x8;
constexpr uint32_t FlagParticleDeferred = 0x10;

// Scratch format for the denoiser signals and the packed normals (FFX's preferred signal format)
constexpr DXGI_FORMAT ScratchFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Linear depth written for sky texels, the denoiser passes through anything beyond SkyDepthBound
constexpr float SkyDepth = 60000.0f;
constexpr float SkyDepthBound = 50000.0f;

// Input probe: screen points (fractions of the render size, same as fsrd_probe.hlsl), RGBA32F texels per point
// written by that shader, frames between rounds
constexpr float ProbePoints[][2] = { { 0.5f, 0.5f }, { 0.5f, 0.8f }, { 0.3f, 0.7f }, { 0.7f, 0.7f }, { 0.5f, 0.95f } };
constexpr UINT ProbePointCount = 5;
constexpr UINT ProbeTexelsPerPoint = 4; // (hit, depth, roughness, material), game normals, diffuse, specular albedo
constexpr DXGI_FORMAT ProbeTexelFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr UINT ProbeRowPitch = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // 512 >= 20 texels * 16 bytes
// Per readback slot: the probe texels row, then one texel with the frame's sentinel count
constexpr UINT ProbeCounterOffset = ProbeRowPitch;
constexpr UINT ProbeSlotSize = ProbeCounterOffset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t ProbeInterval = 120;
constexpr uint32_t ProbeCaptureInterval = 8; // ~7 samples/s at 60 fps, still several frames for the GPU copy

// Disable the denoiser for the session after this many failures, upscaling keeps running
constexpr int MaxDenoiserFailures = 3;

// Frames a replaced resource or denoiser context is kept alive, the GPU can still be running work that uses it
constexpr uint32_t RetireFrames = 8;

struct BarrierDesc
{
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES before;
    D3D12_RESOURCE_STATES after;
};

// One ResourceBarrier call for a group of transitions, null resources are skipped
static void Barriers(ID3D12GraphicsCommandList* cmdList, std::initializer_list<BarrierDesc> transitions)
{
    D3D12_RESOURCE_BARRIER barriers[16];
    UINT count = 0;

    for (const auto& t : transitions)
    {
        if (t.resource != nullptr && t.before != t.after && count < std::size(barriers))
            barriers[count++] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource, t.before, t.after);
    }

    if (count > 0)
        cmdList->ResourceBarrier(count, barriers);
}

static ID3D12Resource* GetResource(NVSDK_NGX_Parameter* InParameters, const char* key)
{
    ID3D12Resource* resource = nullptr;

    if (InParameters->Get(key, &resource) != NVSDK_NGX_Result_Success)
        InParameters->Get(key, (void**) &resource);

    return resource;
}

static bool CreateScratchTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                 D3D12_RESOURCE_STATES initialState, const wchar_t* name, ID3D12Resource** target)
{
    auto desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    auto hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                              IID_PPV_ARGS(target));

    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create {}, hr: {:X}", wstring_to_string(name), (UINT) hr);
        return false;
    }

    (*target)->SetName(name);
    return true;
}

// Row-vector rigid view matrix (rotation rows 0-2, translation row 3) -> camera world position
static void CameraPosition(const float view[4][4], float out[3])
{
    for (int j = 0; j < 3; j++)
        out[j] = -(view[j][0] * view[3][0] + view[j][1] * view[3][1] + view[j][2] * view[3][2]);
}

// Current view space -> previous view space: inverse(view) * prevView, both row-vector rigid transforms
static void ViewToPrevView(const float view[4][4], const float prevView[4][4], float out[4][4])
{
    float inverse[4][4] {};

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            inverse[i][j] = view[j][i];

    CameraPosition(view, inverse[3]);
    inverse[3][3] = 1.0f;

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = inverse[i][0] * prevView[0][j] + inverse[i][1] * prevView[1][j] +
                        inverse[i][2] * prevView[2][j] + inverse[i][3] * prevView[3][j];
}

static FfxApiMatrix4x4 IdentityMatrix()
{
    FfxApiMatrix4x4 m {};
    m.rows[0].x = 1.0f;
    m.rows[1].y = 1.0f;
    m.rows[2].z = 1.0f;
    m.rows[3].w = 1.0f;
    return m;
}

static FfxApiMatrix4x4 ReadMatrix(NVSDK_NGX_Parameter* InParameters, const char* key, bool* found)
{
    float* values = nullptr;
    InParameters->Get(key, (void**) &values);
    *found = values != nullptr;

    if (values == nullptr)
        return IdentityMatrix();

    // Cyberpunk's matrices are row-vector (translation in the last row), which FFX takes as a direct copy
    FfxApiMatrix4x4 m {};
    memcpy(&m, values, sizeof(m));
    return m;
}

FSRDFeatureDx12::FSRDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), FFXFeatureDx12(InHandleId, InParameters)
{
    // IFeature is a virtual base, so FFXFeatureDx12's SetParameters() call isn't run from here
    InParameters->Set("OptiScaler.SupportsUpscaleSize", true);

    // _moduleLoaded is the FFX upscaler's (set by FFXFeatureDx12), a missing denoiser only disables denoising
    if (!FfxApiProxy::InitFfxDx12_Denoiser() || FfxApiProxy::Dx12Module_Denoiser() == nullptr)
    {
        LOG_WARN("amd_fidelityfx_denoiser_dx12.dll not loaded, FSRD will only upscale");
        _denoiserDisabled = true;
    }
}

FSRDFeatureDx12::~FSRDFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    // The game can release the feature right after its last frame was submitted: everything the GPU may still use
    // (denoiser contexts, scratch textures, the passes' heaps and constant buffers) is released a bit later, like
    // Util::DelayedDestroy does
    RetireDenoiserContext();

    for (auto resource : ScratchResources())
        Retire(*resource);

    std::thread(
        [retired = std::move(_retired), prepass = std::move(_prepass), compose = std::move(_compose),
         restore = std::move(_restore), probe = std::move(_probePass)]() mutable
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            for (auto& item : retired)
                ReleaseRetiredObject(item);
        })
        .detach();
}

void FSRDFeatureDx12::ReleaseRetiredObject(RetiredObject& item)
{
    if (item.resource != nullptr)
        item.resource->Release();

    if (item.context != nullptr)
        FfxApiProxy::D3D12_DestroyContext(&item.context, nullptr);

    item = {};
}

void FSRDFeatureDx12::Retire(ID3D12Resource*& resource)
{
    if (resource == nullptr)
        return;

    _retired.push_back({ resource, nullptr, _evalFrame });
    resource = nullptr;
}

void FSRDFeatureDx12::RetireDenoiserContext()
{
    if (_denoiserContextCreated && _denoiserContext != nullptr)
        _retired.push_back({ nullptr, _denoiserContext, _evalFrame });

    _denoiserContext = nullptr;
    _denoiserContextCreated = false;
}

void FSRDFeatureDx12::ReleaseRetired()
{
    std::erase_if(_retired,
                  [this](RetiredObject& item)
                  {
                      if (_evalFrame - item.frame < RetireFrames)
                          return false;

                      ReleaseRetiredObject(item);
                      return true;
                  });
}

void FSRDFeatureDx12::ProcessInitParams(NVSDK_NGX_Parameter* InParameters)
{
    int paramValue = -1;

    if (InParameters->Get(DlssdKeys::UseHWDepth, &paramValue) == NVSDK_NGX_Result_Success)
    {
        _hwDepth = paramValue == 1;
        LOG_INFO("{}: {}", DlssdKeys::UseHWDepth, paramValue);
    }

    paramValue = -1;
    if (InParameters->Get(DlssdKeys::DenoiseMode, &paramValue) == NVSDK_NGX_Result_Success)
        LOG_INFO("{}: {}", DlssdKeys::DenoiseMode, paramValue);

    paramValue = -1;
    if (InParameters->Get(DlssdKeys::RoughnessMode, &paramValue) == NVSDK_NGX_Result_Success)
    {
        _roughnessPacked = paramValue == 1;
        LOG_INFO("{}: {}", DlssdKeys::RoughnessMode, paramValue);
    }
}

bool FSRDFeatureDx12::CreateDenoiserContext()
{
    if (_denoiserContextCreated)
        return true;

    ffxCreateBackendDX12Desc backendDesc {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = Device;

    ffxCreateContextDescDenoiser createDesc {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    createDesc.header.pNext = &backendDesc.header;
    createDesc.version = FFX_DENOISER_VERSION;
    createDesc.maxRenderSize = { _maxRenderWidth, _maxRenderHeight };
    createDesc.signalFlags = FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
    createDesc.checkerboardSignalFlags = 0;
    createDesc.flags = _debugView ? FFX_DENOISER_ENABLE_DEBUGGING : 0;

    ffxReturnCode_t result;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        ScopedSkipHeapCapture skipHeapCapture {};
        result = FfxApiProxy::D3D12_CreateContext(&_denoiserContext, &createDesc.header, nullptr);
    }

    if (result != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Denoiser ffxCreateContext failed: {}", FfxApiProxy::ReturnCodeToString(result));
        _denoiserContext = nullptr;
        return false;
    }

    LOG_INFO("FSR Ray Regeneration context created, max render size: {}x{}, debug view: {}", _maxRenderWidth,
             _maxRenderHeight, _debugView);

    if (_debugView)
    {
        // Linear depth view normalized to the first 100 m, the default range shows mostly white in a city
        FfxApiFloatBounds bounds { 0.0f, 100.0f };

        ffxConfigureDescDenoiserKeyValue configureDesc {};
        configureDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
        configureDesc.key = FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS;
        configureDesc.count = 1;
        configureDesc.data = &bounds;
        FfxApiProxy::D3D12_Configure(&_denoiserContext, &configureDesc.header);
    }

    // Show the denoiser's defaults in the menu for the values the user didn't set
    struct
    {
        CustomOptional<float, NoDefault>* option;
        FfxApiConfigureDenoiserKey key;
    } tunables[] = { { &Config::Instance()->FsrdStabilityBias, FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS },
                     { &Config::Instance()->FsrdRadianceClipStdK, FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K },
                     { &Config::Instance()->FsrdKernelRelaxation,
                       FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION },
                     { &Config::Instance()->FsrdMaxRadiance, FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE } };

    for (auto& tunable : tunables)
    {
        float value = 0.0f;

        ffxQueryDescDenoiserGetDefaultKeyValue queryDesc {};
        queryDesc.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
        queryDesc.key = tunable.key;
        queryDesc.count = 1;
        queryDesc.data = &value;

        if (FfxApiProxy::D3D12_Query(&_denoiserContext, &queryDesc.header) == FFX_API_RETURN_OK)
        {
            LOG_INFO("Denoiser default for key {}: {}", (int) tunable.key, value);

            if (!tunable.option->has_value())
                tunable.option->set_volatile_value(value);
        }
    }

    _denoiserContextCreated = true;
    _resetDenoiserHistory = true;
    return true;
}

void FSRDFeatureDx12::UpdateDebugView()
{
    // The debug view needs a context created with FFX_DENOISER_ENABLE_DEBUGGING, so switching it from the menu
    // creates a new context; the old one is kept until the GPU is done with it
    const bool debugView = Config::Instance()->FsrdDebugView.value_or_default();

    if (debugView == _debugView || !_denoiserContextCreated || _denoiserDisabled)
        return;

    RetireDenoiserContext();
    _debugView = debugView;

    for (auto& sent : _tuningSent)
        sent = false;

    if (!CreateDenoiserContext())
    {
        _denoiserFailures = MaxDenoiserFailures;
        DisableDenoiserOnFailure("Can't recreate the denoiser context for the debug view");
    }
}

bool FSRDFeatureDx12::CreateScratchResources()
{
    // Between frames: denoiser inputs rest in NON_PIXEL_SHADER_RESOURCE, denoiser outputs in UNORDERED_ACCESS
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const UINT w = _maxRenderWidth;
    const UINT h = _maxRenderHeight;

    return CreateScratchTexture(Device, w, h, ScratchFormat, read, L"FSRD_DiffuseIn", &_diffuseIn) &&
           CreateScratchTexture(Device, w, h, ScratchFormat, write, L"FSRD_DiffuseOut", &_diffuseOut) &&
           CreateScratchTexture(Device, w, h, ScratchFormat, read, L"FSRD_SpecularIn", &_specularIn) &&
           CreateScratchTexture(Device, w, h, ScratchFormat, write, L"FSRD_SpecularOut", &_specularOut) &&
           CreateScratchTexture(Device, w, h, DXGI_FORMAT_R32_FLOAT, read, L"FSRD_LinearDepth", &_linearDepth) &&
           CreateScratchTexture(Device, w, h, ScratchFormat, read, L"FSRD_PackedNormals", &_packedNormals) &&
           CreateScratchTexture(Device, w, h, ScratchFormat, read, L"FSRD_MotionVectors", &_motionVectors) &&
           CreateScratchTexture(Device, 2, 1, DXGI_FORMAT_R32_UINT, write, L"FSRD_SentinelCounter",
                                &_sentinelCounter);
}

std::array<ID3D12Resource**, 13> FSRDFeatureDx12::ScratchResources()
{
    return { &_diffuseIn,      &_diffuseOut,    &_specularIn,   &_specularOut,    &_linearDepth,
             &_packedNormals,  &_motionVectors, &_sentinelCounter, &_composedColor, &_reactiveMask,
             &_restoredColor,  &_probeReadback, &_probeTexels };
}

void FSRDFeatureDx12::ReleaseScratchResources()
{
    // Only for resources no recorded GPU work uses yet (init failure)
    for (auto resource : ScratchResources())
        SAFE_RELEASE(*resource);
}

bool FSRDFeatureDx12::PrepareComposedColor(ID3D12Resource* color)
{
    // Same size as the game's color on purpose: FSR 4's auto exposure reads the whole texture, not just renderSize
    auto colorDesc = color->GetDesc();

    if (_composedColor != nullptr && _reactiveMask != nullptr && _restoredColor != nullptr)
    {
        auto desc = _composedColor->GetDesc();

        if (desc.Width == colorDesc.Width && desc.Height == colorDesc.Height && desc.Format == colorDesc.Format)
            return true;
    }

    // Can still be in flight from the previous frames
    Retire(_composedColor);
    Retire(_reactiveMask);
    Retire(_restoredColor);

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    return CreateScratchTexture(Device, (UINT) colorDesc.Width, colorDesc.Height, colorDesc.Format, read,
                                L"FSRD_ComposedColor", &_composedColor) &&
           CreateScratchTexture(Device, (UINT) colorDesc.Width, colorDesc.Height, DXGI_FORMAT_R8_UNORM, read,
                                L"FSRD_ReactiveMask", &_reactiveMask) &&
           CreateScratchTexture(Device, (UINT) colorDesc.Width, colorDesc.Height, colorDesc.Format, read,
                                L"FSRD_RestoredColor", &_restoredColor);
}

void FSRDFeatureDx12::DisableDenoiserOnFailure(const char* reason)
{
    LOG_ERROR("{}", reason);

    if (++_denoiserFailures < MaxDenoiserFailures)
        return;

    LOG_ERROR("Disabling FSR Ray Regeneration after {} failures, only upscaling from now on", _denoiserFailures);
    RetireDenoiserContext();
    _denoiserDisabled = true;
}

void FSRDFeatureDx12::ApplyTuning()
{
    auto* config = Config::Instance();

    const std::pair<const CustomOptional<float, NoDefault>*, FfxApiConfigureDenoiserKey> tunables[] = {
        { &config->FsrdStabilityBias, FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS },
        { &config->FsrdRadianceClipStdK, FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K },
        { &config->FsrdKernelRelaxation, FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION },
        { &config->FsrdMaxRadiance, FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE },
    };

    for (size_t i = 0; i < std::size(tunables); i++)
    {
        if (!tunables[i].first->has_value())
            continue;

        float value = tunables[i].first->value();

        if (_tuningSent[i] && value == _appliedTuning[i])
            continue;

        ffxConfigureDescDenoiserKeyValue configureDesc {};
        configureDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
        configureDesc.key = tunables[i].second;
        configureDesc.count = 1;
        configureDesc.data = &value;

        auto result = FfxApiProxy::D3D12_Configure(&_denoiserContext, &configureDesc.header);
        LOG_INFO("Denoiser key {} set to {}, result: {}", (int) tunables[i].second, value,
                 FfxApiProxy::ReturnCodeToString(result));

        _appliedTuning[i] = value;
        _tuningSent[i] = true;
    }
}

void FSRDFeatureDx12::FinishProbeCapture()
{
    const int id = _captureId.load();
    const char* label = ProbeCaptureLabels[_captureLabel];

    LOG_INFO("=== Probe capture #{} '{}' summary ===", id, label);

    for (int i = 0; i < ProbePointCount; i++)
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

    if (_captureSentinelCount > 0)
    {
        LOG_INFO("[cap #{} {}] sentinel pixels: min {:.2f}%, avg {:.2f}%, max {:.2f}% over {} samples", id, label,
                 _captureSentinelMin, _captureSentinelSum / _captureSentinelCount, _captureSentinelMax,
                 _captureSentinelCount);
    }

    LOG_INFO("=== Probe capture #{} '{}' end ===", id, label);

    _captureActive = false;
    _captureEndNs.store(0);
    _captureDoneUntilNs.store(NowNs() + SecondsToNs(ProbeCaptureDoneSeconds));
}

void FSRDFeatureDx12::ProbeInputs(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                                  ID3D12Resource* specularHitDistance, ID3D12Resource* normals,
                                  ID3D12Resource* diffuseAlbedo, ID3D12Resource* specularAlbedo,
                                  const float view[4][4])
{
    const int64_t nowNs = NowNs();

    // A requested capture starts after the menu delay
    int request = -1;

    if (_captureRequest.load() >= 0 && nowNs >= _captureStartNs.load())
        request = _captureRequest.exchange(-1);

    if (request >= 0 && request < ProbeCaptureLabelCount)
    {
        if (_captureActive)
            FinishProbeCapture();

        _captureLabel = request;
        _captureId.fetch_add(1);

        for (auto& st : _captureStats)
            st = ProbeStats {};

        _captureSentinelMin = 1e30f;
        _captureSentinelMax = -1e30f;
        _captureSentinelSum = 0.0f;
        _captureSentinelCount = 0;

        _captureActive = true;
        _probeFrame = 0;
        _captureDoneUntilNs.store(0);
        _captureEndNs.store(nowNs + SecondsToNs(ProbeCaptureSeconds));

        LOG_INFO("=== Probe capture #{} '{}' start ({} s) ===", _captureId.load(), ProbeCaptureLabels[_captureLabel],
                 ProbeCaptureSeconds);
    }

    if (_captureActive && nowNs >= _captureEndNs.load())
        FinishProbeCapture();

    // Outside a capture the periodic probe log only runs with the Diagnostics option
    if (!_captureActive && !Config::Instance()->FsrdDiagnostics.value_or_default())
    {
        _probeSlotWritten[0] = _probeSlotWritten[1] = false;
        _probeFrame = 0;
        return;
    }

    const uint32_t interval = _captureActive ? ProbeCaptureInterval : ProbeInterval;

    if (++_probeFrame % interval != 0 || _probePass == nullptr || !_probePass->CanRender())
        return;

    const int slot = _probeRound++ % 2;
    const int otherSlot = 1 - slot;

    if (_probeReadback == nullptr || _probeTexels == nullptr)
    {
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(ProbeSlotSize * 2);
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

        if (FAILED(Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&_probeReadback))) ||
            !CreateScratchTexture(Device, ProbePointCount * ProbeTexelsPerPoint, 1, ProbeTexelFormat,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"FSRD_ProbeTexels", &_probeTexels))
        {
            LOG_ERROR("Can't create the probe resources");
            SAFE_RELEASE(_probeReadback);
            SAFE_RELEASE(_probeTexels);
            _probeFrame = 0x7FFFFFFF; // don't try again for a long time
            return;
        }
    }

    // The other slot was copied a full interval ago, so that frame's GPU work is done
    if (_probeSlotWritten[otherSlot])
    {
        D3D12_RANGE range { (SIZE_T) otherSlot * ProbeSlotSize, (SIZE_T) (otherSlot + 1) * ProbeSlotSize };
        uint8_t* data = nullptr;

        if (SUCCEEDED(_probeReadback->Map(0, &range, (void**) &data)))
        {
            LogProbeSlot(data + otherSlot * ProbeSlotSize, otherSlot);

            D3D12_RANGE noWrite { 0, 0 };
            _probeReadback->Unmap(0, &noWrite);
        }
    }

    // This round: the probe shader reads the inputs at the points into _probeTexels (game resources only through
    // SRVs, their state is never changed), then _probeTexels and this frame's sentinel count go to the readback
    if (!_probePass->Dispatch(cmdList,
                              { specularHitDistance, normals, diffuseAlbedo, specularAlbedo, _linearDepth,
                                _packedNormals },
                              { _probeTexels }, constants, ProbePointCount, 1))
    {
        return;
    }

    const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto copy = D3D12_RESOURCE_STATE_COPY_SOURCE;

    Barriers(cmdList, { { _probeTexels, uav, copy }, { _sentinelCounter, uav, copy } });

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = _probeReadback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = slot * ProbeSlotSize;
    dst.PlacedFootprint.Footprint = { ProbeTexelFormat, ProbePointCount * ProbeTexelsPerPoint, 1, 1,
                                      ProbeRowPitch };

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = _probeTexels;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    dst.PlacedFootprint.Offset = slot * ProbeSlotSize + ProbeCounterOffset;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
    src.pResource = _sentinelCounter;

    D3D12_BOX box { _probeCounterSlot, 0, 0, _probeCounterSlot + 1, 1, 1 };
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    Barriers(cmdList, { { _probeTexels, copy, uav }, { _sentinelCounter, copy, uav } });

    _probePixels[slot] = constants.RenderWidth * constants.RenderHeight;
    _probeSlotWritten[slot] = true;
    memcpy(_probeView[slot], view, sizeof(_probeView[slot]));
    _probeSlotCapture[slot] = _captureActive ? _captureId.load() : 0;
}

void FSRDFeatureDx12::LogProbeSlot(const uint8_t* data, int slot)
{
    // Only data copied during the running capture counts for its summary and gets its tag
    const int slotCapture = _probeSlotCapture[slot];
    const bool inCapture = _captureActive && slotCapture == _captureId.load();
    const std::string tag = inCapture ? std::format("[cap #{} {}] ", slotCapture, ProbeCaptureLabels[_captureLabel]) : "";

    // Camera axes in world space are the columns of the row-vector view rotation
    const auto& v = _probeView[slot];
    LOG_INFO("{}Probe camera: right ({:.3f}, {:.3f}, {:.3f}), up ({:.3f}, {:.3f}, {:.3f}), "
             "forward ({:.3f}, {:.3f}, {:.3f})",
             tag, v[0][0], v[1][0], v[2][0], v[0][1], v[1][1], v[2][1], v[0][2], v[1][2], v[2][2]);

    for (int i = 0; i < ProbePointCount; i++)
    {
        // Layout written by fsrd_probe.hlsl: (hit, depth, roughness, material), game normals, diffuse, specular
        float t[ProbeTexelsPerPoint][4] {};
        memcpy(t, data + i * ProbeTexelsPerPoint * sizeof(t[0]), sizeof(t));

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

        if (inCapture)
        {
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
    }

    uint32_t sentinels = 0;
    memcpy(&sentinels, data + ProbeCounterOffset, sizeof(sentinels));

    if (_probePixels[slot] > 0)
    {
        const float percent = 100.0f * (float) sentinels / (float) _probePixels[slot];
        LOG_INFO("{}Probe sentinel pixels: {:.2f}% ({} of {})", tag, percent, sentinels, _probePixels[slot]);

        if (inCapture)
        {
            _captureSentinelMin = std::min(_captureSentinelMin, percent);
            _captureSentinelMax = std::max(_captureSentinelMax, percent);
            _captureSentinelSum += percent;
            _captureSentinelCount++;
        }
    }
}

void FSRDFeatureDx12::LogInputsOnce(NVSDK_NGX_Parameter* InParameters)
{
    if (_loggedInputsOnce)
        return;

    _loggedInputsOnce = true;

    // Everything DLSS-RR can receive, so the log shows what the game actually provides and in which format
    static const char* resourceKeys[] = { NVSDK_NGX_Parameter_Color,
                                          NVSDK_NGX_Parameter_Output,
                                          NVSDK_NGX_Parameter_MotionVectors,
                                          NVSDK_NGX_Parameter_Depth,
                                          DlssdKeys::Normals,
                                          DlssdKeys::Roughness,
                                          DlssdKeys::DiffuseAlbedo,
                                          DlssdKeys::SpecularAlbedo,
                                          DlssdKeys::DiffuseHitDistance,
                                          DlssdKeys::SpecularHitDistance,
                                          "DLSSD.DiffuseRayDirection",
                                          "DLSSD.SpecularRayDirection",
                                          "DLSSD.DiffuseRayDirectionHitDistance",
                                          "DLSSD.SpecularRayDirectionHitDistance",
                                          "DLSSD.ReflectedAlbedo",
                                          "DLSSD.ColorBeforeParticles",
                                          "DLSSD.ColorAfterParticles",
                                          "DLSSD.ColorBeforeTransparency",
                                          "DLSSD.ColorAfterTransparency",
                                          "DLSSD.ColorBeforeFog",
                                          "DLSSD.ColorAfterFog",
                                          "DLSSD.ResponsivityMask",
                                          "DLSSD.Alpha",
                                          "GBuffer.Albedo",
                                          "GBuffer.Emissive",
                                          "GBuffer.SpecularMvec",
                                          "GBuffer.DisocclusionMask",
                                          NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask };

    LOG_INFO("FSRD input resources (first frame):");

    for (auto key : resourceKeys)
    {
        auto resource = GetResource(InParameters, key);

        if (resource == nullptr)
        {
            LOG_INFO("  {}: null", key);
            continue;
        }

        auto desc = resource->GetDesc();
        LOG_INFO("  {}: {}x{} format {} ({})", key, desc.Width, desc.Height, (UINT) desc.Format, (void*) resource);
    }

    bool found = false;
    auto view = ReadMatrix(InParameters, DlssdKeys::WorldToViewMatrix, &found);
    LOG_INFO("  {}: {}", DlssdKeys::WorldToViewMatrix, found ? "present" : "null");

    if (found)
    {
        for (int i = 0; i < 4; i++)
            LOG_INFO("    [{:.4f} {:.4f} {:.4f} {:.4f}]", view.rows[i].x, view.rows[i].y, view.rows[i].z,
                     view.rows[i].w);
    }

    auto proj = ReadMatrix(InParameters, DlssdKeys::ViewToClipMatrix, &found);
    LOG_INFO("  {}: {}", DlssdKeys::ViewToClipMatrix, found ? "present" : "null");

    if (found)
    {
        for (int i = 0; i < 4; i++)
            LOG_INFO("    [{:.4f} {:.4f} {:.4f} {:.4f}]", proj.rows[i].x, proj.rows[i].y, proj.rows[i].z,
                     proj.rows[i].w);
    }

    float mvScaleX = 0.0f, mvScaleY = 0.0f;
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    LOG_INFO("  MV scale: {}x{}, LowResMV: {}, DepthInverted: {}, HW depth: {}, roughness packed: {}", mvScaleX,
             mvScaleY, LowResMV(), DepthInverted(), _hwDepth, _roughnessPacked);
}

bool FSRDFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    ProcessInitParams(InParameters);

    // Upscaler first: this is what produces the image, the denoiser is optional
    if (!FFXFeatureDx12::InitInternal(InCommandList, InParameters))
        return false;

    if (!Config::Instance()->FsrdEnableDenoiser.value_or_default())
    {
        LOG_INFO("FSR Ray Regeneration disabled by config ([FSRD] EnableDenoiser), only upscaling");
        _denoiserDisabled = true;
    }

    if (!_hwDepth)
    {
        // The prepass only knows how to linearize HW depth
        LOG_WARN("Game sends linear depth (DLSS.Use.HW.Depth != 1), FSR Ray Regeneration not supported, only upscaling");
        _denoiserDisabled = true;
    }

    if (_denoiserDisabled)
        return true;

    _debugView = Config::Instance()->FsrdDebugView.value_or_default();

    // Size scratch for the largest render size we can get, so dynamic resolution doesn't reallocate
    _maxRenderWidth = std::max(RenderWidth(), DisplayWidth());
    _maxRenderHeight = std::max(RenderHeight(), DisplayHeight());

    _prepass = std::make_unique<FsrdPass_Dx12>("FSRD Prepass", Device, 8, 6, fsrd_prepass_cso, sizeof(fsrd_prepass_cso));
    _compose = std::make_unique<FsrdPass_Dx12>("FSRD Compose", Device, 10, 2, fsrd_compose_cso, sizeof(fsrd_compose_cso));
    _restore = std::make_unique<FsrdPass_Dx12>("FSRD Restore", Device, 4, 1, fsrd_restore_cso, sizeof(fsrd_restore_cso));
    // Diagnostics only, the denoiser works without it
    _probePass = std::make_unique<FsrdPass_Dx12>("FSRD Probe", Device, 6, 1, fsrd_probe_cso, sizeof(fsrd_probe_cso));

    if (!_prepass->CanRender() || !_compose->CanRender() || !_restore->CanRender() || !CreateScratchResources() ||
        !CreateDenoiserContext())
    {
        LOG_ERROR("FSR Ray Regeneration init failed, only upscaling");
        ReleaseScratchResources();
        _prepass.reset();
        _compose.reset();
        _restore.reset();
        _probePass.reset();
        _denoiserDisabled = true;
    }

    return true;
}

ID3D12Resource* FSRDFeatureDx12::Denoise(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    auto color = GetResource(InParameters, NVSDK_NGX_Parameter_Color);
    auto motionVectors = GetResource(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    auto depth = GetResource(InParameters, NVSDK_NGX_Parameter_Depth);
    auto normals = GetResource(InParameters, DlssdKeys::Normals);
    auto roughness = GetResource(InParameters, DlssdKeys::Roughness);
    auto diffuseAlbedo = GetResource(InParameters, DlssdKeys::DiffuseAlbedo);
    auto specularAlbedo = GetResource(InParameters, DlssdKeys::SpecularAlbedo);
    auto particles = GetParticleLayer(InParameters, color);
    auto specularHitDistance = GetResource(InParameters, DlssdKeys::SpecularHitDistance);

    if (color == nullptr || motionVectors == nullptr || depth == nullptr || normals == nullptr ||
        diffuseAlbedo == nullptr || specularAlbedo == nullptr)
    {
        LOG_WARN("Skipping denoise, missing input (color: {}, mv: {}, depth: {}, normals: {}, diffuseAlbedo: {}, "
                 "specularAlbedo: {})",
                 (void*) color, (void*) motionVectors, (void*) depth, (void*) normals, (void*) diffuseAlbedo,
                 (void*) specularAlbedo);
        return nullptr;
    }

    unsigned int renderWidth = 0, renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);
    renderWidth = std::min(renderWidth, _maxRenderWidth);
    renderHeight = std::min(renderHeight, _maxRenderHeight);

    if (renderWidth == 0 || renderHeight == 0)
        return nullptr;

    if (!PrepareComposedColor(color))
    {
        DisableDenoiserOnFailure("Can't create the composed color texture");
        return nullptr;
    }

    bool found = false;
    auto view = ReadMatrix(InParameters, DlssdKeys::WorldToViewMatrix, &found);
    const bool hasView = found;
    auto projection = ReadMatrix(InParameters, DlssdKeys::ViewToClipMatrix, &found);

    if (!found || projection.rows[3].z == 0.0f)
    {
        LOG_WARN("Skipping denoise, no usable ViewToClipMatrix to linearize depth");
        return nullptr;
    }

    const bool separateRoughness = !_roughnessPacked && roughness != nullptr;

    // Read every frame so the in-game menu applies immediately
    const int splitMode = std::clamp(Config::Instance()->FsrdSplitMode.value_or_default(), 0, 2);

    if (splitMode != _lastSplitMode)
    {
        LOG_INFO("Signal split mode: {}", splitMode);
        _lastSplitMode = splitMode;
        _resetDenoiserHistory = true;
    }

    ApplyTuning();

    float viewRows[4][4] {};
    memcpy(viewRows, &view, sizeof(viewRows));

    // DLSS motion vectors point to the previous frame and turn into pixels with MV scale, FFX wants
    // PreviousUV - CurrentUV, the prepass converts them and adds the linear depth delta
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    const float mvWidth = (float) (LowResMV() ? renderWidth : DisplayWidth());
    const float mvHeight = (float) (LowResMV() ? renderHeight : DisplayHeight());

    FsrdPass_Dx12::Constants constants {};
    constants.MvScale[0] = mvScaleX / mvWidth;
    constants.MvScale[1] = mvScaleY / mvHeight;
    constants.MvTexelScale[0] = LowResMV() ? 1.0f : mvWidth / (float) renderWidth;
    constants.MvTexelScale[1] = LowResMV() ? 1.0f : mvHeight / (float) renderHeight;
    constants.InvProj[0] = projection.rows[0].x != 0.0f ? 1.0f / projection.rows[0].x : 1.0f;
    constants.InvProj[1] = projection.rows[1].y != 0.0f ? 1.0f / projection.rows[1].y : 1.0f;
    constants.MaxStaticMvError = 2.0f / (float) renderWidth; // ~2 render pixels
    constants.HasPrevView = (hasView && _hasPrevView) ? 1 : 0;
    constants.ReflectionReactive = std::clamp(Config::Instance()->FsrdReflectionReactive.value_or_default(), 0.0f, 1.0f);
    constants.GameMaskBias = Config::Instance()->DlssReactiveMaskBias.value_or_default();
    constants.SpecHitScale = std::max(Config::Instance()->FsrdSpecHitDistanceScale.value_or_default(), 0.0001f);
    // Read every frame so the menu applies immediately; the history doesn't fit the other mode
    const int sentinelMode = std::clamp(Config::Instance()->FsrdSentinelMode.value_or_default(), 0, 1);

    if (sentinelMode != _lastSentinelMode)
    {
        LOG_INFO("Sentinel pixels mode: {}", sentinelMode == 0 ? "passthrough" : "diffuse");
        _lastSentinelMode = sentinelMode;
        _resetDenoiserHistory = true;
    }

    // Camera forward in world space: third column of the row-vector view rotation
    constants.CameraForward[0] = viewRows[0][2];
    constants.CameraForward[1] = viewRows[1][2];
    constants.CameraForward[2] = viewRows[2][2];
    constants.SentinelMode = (uint32_t) sentinelMode;
    _probeCounterSlot = _sentinelFrame++ % 2;
    constants.CounterSlot = _probeCounterSlot;
    constants.RestoreStrength = std::clamp(Config::Instance()->FsrdLowFreqRestore.value_or_default(), 0.0f, 1.0f);
    // 4 taps each side, so the stride is a quarter of the radius
    constants.RestoreStride = std::max(Config::Instance()->FsrdLowFreqRadius.value_or_default() / 4.0f, 1.0f);

    constants.RoughnessOverride = Config::Instance()->FsrdRoughnessOverrideEnabled.value_or_default()
                                      ? std::clamp(Config::Instance()->FsrdRoughnessOverride.value_or_default(), 0.0f, 1.0f)
                                      : -1.0f;

    if (constants.HasPrevView)
    {
        float toPrev[4][4];
        ViewToPrevView(viewRows, _prevView, toPrev);

        for (int i = 0; i < 4; i++)
        {
            constants.PrevViewX[i] = toPrev[i][0];
            constants.PrevViewY[i] = toPrev[i][1];
            constants.PrevViewZ[i] = toPrev[i][2];
        }
    }

    constants.ProjZZ = projection.rows[2].z;
    constants.ProjWZ = projection.rows[3].z;
    constants.DiffuseHitDistance = Config::Instance()->FsrdDiffuseHitDistance.value_or_default();
    constants.Flags = (separateRoughness ? FlagSeparateRoughness : 0) |
                      (specularHitDistance != nullptr ? FlagHasSpecHitDistance : 0) |
                      (DepthInverted() ? FlagDepthInverted : 0) |
                      (particles != nullptr ? FlagParticleLayer : 0);
    constants.RenderWidth = renderWidth;
    constants.RenderHeight = renderHeight;
    constants.SkyDepth = SkyDepth;
    constants.SplitMode = (uint32_t) splitMode;

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // 1. Prepass: game resources -> denoiser inputs
    ID3D12Resource* prepassOutputs[] = { _diffuseIn, _specularIn, _linearDepth, _packedNormals, _motionVectors };

    for (auto resource : prepassOutputs)
        ResourceBarrier(InCommandList, resource, read, write);

    const bool prepassOk = _prepass->Dispatch(
        InCommandList,
        { color, diffuseAlbedo, specularAlbedo, normals,
          separateRoughness ? roughness : nullptr, depth,
          specularHitDistance, motionVectors },
        { _diffuseIn, _specularIn, _linearDepth, _packedNormals, _motionVectors, _sentinelCounter }, constants,
        renderWidth, renderHeight);

    for (auto resource : prepassOutputs)
        ResourceBarrier(InCommandList, resource, write, read);

    if (!prepassOk)
    {
        DisableDenoiserOnFailure("FSRD prepass dispatch failed");
        return nullptr;
    }

    // 2. Denoise
    ffxDispatchDescDenoiserIndirectSpecular specularDesc {};
    specularDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
    specularDesc.signal.input = ffxApiGetResourceDX12(_specularIn, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    specularDesc.signal.output = ffxApiGetResourceDX12(_specularOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescDenoiserIndirectDiffuse diffuseDesc {};
    diffuseDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
    diffuseDesc.header.pNext = &specularDesc.header;
    diffuseDesc.signal.input = ffxApiGetResourceDX12(_diffuseIn, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    diffuseDesc.signal.output = ffxApiGetResourceDX12(_diffuseOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescDenoiser dispatchDesc {};
    dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    dispatchDesc.header.pNext = &diffuseDesc.header;
    dispatchDesc.commandList = InCommandList;

    dispatchDesc.linearDepth = ffxApiGetResourceDX12(_linearDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.normals = ffxApiGetResourceDX12(_packedNormals, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxApiGetResourceDX12(_motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.specularAlbedo = ffxApiGetResourceDX12(specularAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.diffuseAlbedo = ffxApiGetResourceDX12(diffuseAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    // Already PreviousUV - CurrentUV with the depth delta in B (prepass)
    dispatchDesc.motionVectorScale = { 1.0f, 1.0f, 1.0f };

    float jitterX = 0.0f, jitterY = 0.0f;
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);
    dispatchDesc.jitterOffsets = { jitterX, jitterY };

    dispatchDesc.cameraPositionDelta = { 0.0f, 0.0f, 0.0f };

    if (hasView)
    {
        if (_hasPrevView)
        {
            float cameraPos[3], prevCameraPos[3];
            CameraPosition(viewRows, cameraPos);
            CameraPosition(_prevView, prevCameraPos);

            dispatchDesc.cameraPositionDelta = { prevCameraPos[0] - cameraPos[0], prevCameraPos[1] - cameraPos[1],
                                                 prevCameraPos[2] - cameraPos[2] };
        }

        memcpy(_prevView, viewRows, sizeof(viewRows));
        _hasPrevView = true;
    }

    dispatchDesc.view = view;
    dispatchDesc.projection = projection;
    dispatchDesc.linearDepthBounds = { 0.0f, SkyDepthBound };
    dispatchDesc.renderSize = { renderWidth, renderHeight };
    dispatchDesc.frameIndex = _denoiserFrameIndex++;

    unsigned int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    // DLSS-RR albedos are linear, not sqrt encoded
    dispatchDesc.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO;

    if (_resetDenoiserHistory || reset == 1)
        dispatchDesc.flags |= FFX_DENOISER_DISPATCH_RESET;

    // Debug view: the overview is drawn into the composed color, which then replaces the image
    ffxDispatchDescDenoiserDebugView debugDesc {};
    auto composedDesc = _composedColor->GetDesc();

    if (_debugView)
    {
        debugDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW;
        debugDesc.output = ffxApiGetResourceDX12(_composedColor, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        debugDesc.outputSize = { (uint32_t) composedDesc.Width, composedDesc.Height };
        debugDesc.mode = FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW;
        specularDesc.header.pNext = &debugDesc.header;

        ResourceBarrier(InCommandList, _composedColor, read, write);
    }

    auto result = FfxApiProxy::D3D12_Dispatch(&_denoiserContext, &dispatchDesc.header);

    if (_debugView)
        ResourceBarrier(InCommandList, _composedColor, write, read);

    if (result != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Denoiser ffxDispatch failed: {}", FfxApiProxy::ReturnCodeToString(result));
        DisableDenoiserOnFailure("FSR Ray Regeneration dispatch failed");
        return nullptr;
    }

    _resetDenoiserHistory = false;

    if (_debugView)
    {
        // The probe reads the inputs, which are the same in debug view
        ProbeInputs(InCommandList, constants, specularHitDistance, normals, diffuseAlbedo, specularAlbedo, viewRows);
        return _composedColor;
    }

    // 3. Compose: denoised signals * albedo -> composed color
    ResourceBarrier(InCommandList, _diffuseOut, write, read);
    ResourceBarrier(InCommandList, _specularOut, write, read);
    ResourceBarrier(InCommandList, _composedColor, read, write);
    ResourceBarrier(InCommandList, _reactiveMask, read, write);

    auto gameReactive = GetResource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);

    // Diagnostic: remodulate the denoiser's inputs instead of its outputs, to see what it receives
    const bool showInput = Config::Instance()->FsrdShowDenoiserInput.value_or_default();

    if (showInput != _wasShowingInput)
    {
        LOG_INFO("Show denoiser input: {}", showInput);
        _wasShowingInput = showInput;
    }
    auto diffuseSignal = showInput ? _diffuseIn : _diffuseOut;
    auto specularSignal = showInput ? _specularIn : _specularOut;

    // With the low frequency restore on, its residual (game color - composed) must not see the particles, so
    // compose only marks them in the reactive mask and the restore pass puts them over
    const bool restoreOn = constants.RestoreStrength > 0.0f;
    const uint32_t passFlags = constants.Flags;

    if (restoreOn && particles != nullptr)
        constants.Flags |= FlagParticleDeferred;

    const bool composeOk = _compose->Dispatch(
        InCommandList,
        { color, diffuseAlbedo, specularAlbedo, depth, diffuseSignal, specularSignal, gameReactive, _packedNormals,
          normals, particles },
        { _composedColor, _reactiveMask }, constants, (UINT) composedDesc.Width, composedDesc.Height);

    constants.Flags = passFlags;

    ResourceBarrier(InCommandList, _diffuseOut, read, write);
    ResourceBarrier(InCommandList, _specularOut, read, write);
    ResourceBarrier(InCommandList, _composedColor, write, read);
    ResourceBarrier(InCommandList, _reactiveMask, write, read);

    if (!composeOk)
    {
        DisableDenoiserOnFailure("FSRD compose dispatch failed");
        return nullptr;
    }

    ProbeInputs(InCommandList, constants, specularHitDistance, normals, diffuseAlbedo, specularAlbedo, viewRows);

    if (!restoreOn)
        return _composedColor;

    // 4. Restore the low frequency part the denoiser removed (fog, haze), then the particles go over
    ResourceBarrier(InCommandList, _restoredColor, read, write);

    const bool restoreOk = _restore->Dispatch(InCommandList, { color, _composedColor, _linearDepth, particles },
                                              { _restoredColor }, constants, (UINT) composedDesc.Width,
                                              composedDesc.Height);

    ResourceBarrier(InCommandList, _restoredColor, write, read);

    return restoreOk ? _restoredColor : _composedColor;
}

ID3D12Resource* FSRDFeatureDx12::GetParticleLayer(NVSDK_NGX_Parameter* InParameters, ID3D12Resource* color)
{
    // Cyberpunk sends steam/smoke/light glows in DLSSD.ColorBeforeParticles as a separate premultiplied layer over
    // black (alpha = coverage), not in the main color. The denoiser gets the main color, the layer goes over after.
    ID3D12Resource* particles = nullptr;

    if (Config::Instance()->FsrdParticleLayer.value_or_default() == 1 && color != nullptr)
    {
        particles = GetResource(InParameters, DlssdKeys::ColorBeforeParticles);

        if (particles != nullptr)
        {
            auto colorDesc = color->GetDesc();
            auto particlesDesc = particles->GetDesc();

            if (particlesDesc.Width < colorDesc.Width || particlesDesc.Height < colorDesc.Height)
                particles = nullptr;
        }
    }

    const int mode = particles != nullptr ? 1 : 0;

    if (mode != _lastParticleLayer)
    {
        LOG_INFO("Particle layer: {}", mode == 0 ? "off" : "over");
        _lastParticleLayer = mode;
    }

    return particles;
}

ID3D12Resource* FSRDFeatureDx12::BypassWithParticles(ID3D12GraphicsCommandList* InCommandList,
                                                     NVSDK_NGX_Parameter* InParameters)
{
    if (_restore == nullptr || !_restore->CanRender() || _restoredColor == nullptr)
        return nullptr;

    auto color = GetResource(InParameters, NVSDK_NGX_Parameter_Color);
    auto particles = GetParticleLayer(InParameters, color);

    if (particles == nullptr)
        return nullptr;

    unsigned int renderWidth = 0, renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);

    if (renderWidth == 0 || renderHeight == 0)
        return nullptr;

    // The restore pass with strength 0 only puts the particle layer over its Composed input
    FsrdPass_Dx12::Constants constants {};
    constants.RenderWidth = std::min(renderWidth, _maxRenderWidth);
    constants.RenderHeight = std::min(renderHeight, _maxRenderHeight);
    constants.Flags = FlagParticleLayer;
    constants.RestoreStrength = 0.0f;
    constants.RestoreStride = 1.0f;

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto outDesc = _restoredColor->GetDesc();

    ResourceBarrier(InCommandList, _restoredColor, read, write);

    const bool ok = _restore->Dispatch(InCommandList, { color, color, nullptr, particles }, { _restoredColor },
                                       constants, (UINT) outDesc.Width, outDesc.Height);

    ResourceBarrier(InCommandList, _restoredColor, write, read);

    return ok ? _restoredColor : nullptr;
}

bool FSRDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LogInputsOnce(InParameters);

    _evalFrame++;
    ReleaseRetired();

    ID3D12Resource* composed = nullptr;

    // Menu bypass: upscale the game's color as is; the history is stale when it's turned off again
    const bool bypass = Config::Instance()->FsrdBypassDenoiser.value_or_default();

    if (bypass != _wasBypassed)
    {
        LOG_INFO("Denoiser bypass: {}", bypass);
        _wasBypassed = bypass;
        _resetDenoiserHistory = true;
    }

    // Debug: upscale DLSSD.ColorBeforeParticles as is, to see what the game puts in it. Here and in bypass the
    // game's reactive mask is kept, ours isn't written.
    bool gameReactiveOnly = false;

    if (Config::Instance()->FsrdParticleLayer.value_or_default() == 2)
    {
        auto rawParticles = GetResource(InParameters, DlssdKeys::ColorBeforeParticles);

        if (rawParticles != nullptr)
        {
            composed = rawParticles;
            gameReactiveOnly = true;
            _resetDenoiserHistory = true;
        }
    }

    UpdateDebugView();

    // Bypass: the game's color with the particle layer over it, so an A/B in steam is fair
    if (composed == nullptr && bypass)
    {
        composed = BypassWithParticles(InCommandList, InParameters);
        gameReactiveOnly = composed != nullptr;
    }

    if (composed == nullptr && !bypass && !_denoiserDisabled && _denoiserContextCreated)
        composed = Denoise(InCommandList, InParameters);

    if (composed == nullptr)
        return FFXFeatureDx12::EvaluateInternal(InCommandList, InParameters);

    // Upscale the denoised color with our reactive mask (it already includes the game's), then give the game its own
    // parameters back. The debug view has no reactive mask, it's drawn over the composed color.
    auto gameColor = GetResource(InParameters, NVSDK_NGX_Parameter_Color);
    auto gameFsrReactive = GetResource(InParameters, "FSR.reactive");

    InParameters->Set(NVSDK_NGX_Parameter_Color, composed);

    if (!_debugView && !gameReactiveOnly)
        InParameters->Set("FSR.reactive", _reactiveMask);

    auto result = FFXFeatureDx12::EvaluateInternal(InCommandList, InParameters);

    InParameters->Set(NVSDK_NGX_Parameter_Color, gameColor);

    if (!_debugView && !gameReactiveOnly)
        InParameters->Set("FSR.reactive", gameFsrReactive);

    return result;
}
