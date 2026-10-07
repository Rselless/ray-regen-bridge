#include "pch.h"
#include "FSRDFeatureDx12.h"

#include <Config.h>
#include <Util.h>
#include <proxies/FfxApi_Proxy.h>

#include <shaders/fsrd/precompile/fsrd_prepass_Shader.h>
#include <shaders/fsrd/precompile/fsrd_compose_Shader.h>
#include <shaders/fsrd/precompile/fsrd_particles_Shader.h>

// FSR Ray Regeneration API (ffx_denoiser.h 1.2.0), wrapped so it builds on the same ffx_api.h OptiScaler already
// uses -- see ffx_denoiser_compat.h for why the real header can't be included directly. ffxApiGetResourceDX12 comes
// from <dx12/ffx_api_dx12.h>, already included through FfxApi_Proxy.h.
#include "ffx_denoiser_compat.h"
#include "FsrdUtil.h"

#include <mutex>
#include <unordered_set>

using Fsrd::Barriers;
using Fsrd::CreateScratchTexture;

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
constexpr uint32_t FlagCountSentinels = 0x10;

// Scratch format for the denoiser signals and the packed normals (FFX's preferred signal format)
constexpr DXGI_FORMAT ScratchFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Linear depth written for sky texels, the denoiser passes through anything beyond SkyDepthBound
constexpr float SkyDepth = 60000.0f;
constexpr float SkyDepthBound = 50000.0f;

// Disable the denoiser for the session after this many failures, upscaling keeps running
constexpr int MaxDenoiserFailures = 3;

// Frames a replaced resource or denoiser context is kept alive, the GPU can still be running work that uses it
constexpr uint32_t RetireFrames = 8;

static ID3D12Resource* GetResource(NVSDK_NGX_Parameter* InParameters, const char* key)
{
    ID3D12Resource* resource = nullptr;

    if (InParameters->Get(key, &resource) != NVSDK_NGX_Result_Success)
        InParameters->Get(key, (void**) &resource);

    return resource;
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

    SAFE_RELEASE(_adapter);

    std::thread(
        [retired = std::move(_retired), prepass = std::move(_prepass), compose = std::move(_compose),
         particles = std::move(_particlesPass), probe = std::move(_probe)]() mutable
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
    createDesc.signalFlags = _directSignals
                                 ? FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_DIRECT_SPECULAR
                                 : FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
    createDesc.checkerboardSignalFlags = 0;
    createDesc.flags = (_debugView ? FFX_DENOISER_ENABLE_DEBUGGING : 0) | (_validation ? FFX_DENOISER_ENABLE_VALIDATION : 0);

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

    LOG_INFO("FSR Ray Regeneration context created, max render size: {}x{}, signals: {}, debug view: {}, "
             "validation: {}",
             _maxRenderWidth, _maxRenderHeight, _directSignals ? "direct" : "indirect", _debugView, _validation);

    {
        FfxApiEffectMemoryUsage usage {};
        ffxQueryDescDenoiserGetGPUMemoryUsage memoryDesc {};
        memoryDesc.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;
        memoryDesc.device = Device;
        memoryDesc.maxRenderSize = createDesc.maxRenderSize;
        memoryDesc.signalFlags = createDesc.signalFlags;
        memoryDesc.checkerboardSignalFlags = createDesc.checkerboardSignalFlags;
        memoryDesc.flags = createDesc.flags;
        memoryDesc.gpuMemoryUsage = &usage;

        if (FfxApiProxy::D3D12_Query(&_denoiserContext, &memoryDesc.header) == FFX_API_RETURN_OK)
        {
            LOG_INFO("Denoiser GPU memory: {:.1f} MB total, {:.1f} MB aliasable",
                     usage.totalUsageInBytes / (1024.0 * 1024.0), usage.aliasableUsageInBytes / (1024.0 * 1024.0));
        }

        LogVideoMemory("denoiser context created");
    }

    if (_validation)
    {
        // Validation messages come through the global debug callback
        ffxConfigureDescGlobalDebug1 debugDesc {};
        debugDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        debugDesc.fpMessage = &ValidationMessage;
        debugDesc.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;

        auto debugResult = FfxApiProxy::D3D12_Configure(&_denoiserContext, &debugDesc.header);
        LOG_INFO("Denoiser validation messages hooked: {}", FfxApiProxy::ReturnCodeToString(debugResult));
    }

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

void FSRDFeatureDx12::LogVideoMemory(const char* when)
{
    // Local video memory in use by the process vs the budget the OS gives it; over budget the game's own memory
    // manager starts evicting and recreating its heaps
    if (_adapter == nullptr)
    {
        IDXGIFactory4* factory = nullptr;

        {
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return;
        }

        factory->EnumAdapterByLuid(Device->GetAdapterLuid(), IID_PPV_ARGS(&_adapter));
        factory->Release();

        if (_adapter == nullptr)
            return;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO info {};

    if (SUCCEEDED(_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    {
        LOG_INFO("VRAM ({}): {:.0f} MB used of {:.0f} MB budget ({:.0f}%)", when, info.CurrentUsage / (1024.0 * 1024.0),
                 info.Budget / (1024.0 * 1024.0), info.Budget > 0 ? 100.0 * info.CurrentUsage / info.Budget : 0.0);
    }
}

void FSRDFeatureDx12::ValidationMessage(uint32_t type, const wchar_t* message)
{
    // Validation can repeat the same message every frame: each distinct one is logged once, up to a limit
    static std::mutex mutex;
    static std::unordered_set<std::wstring> seen;
    constexpr size_t MaxMessages = 200;

    std::scoped_lock lock(mutex);

    if (message == nullptr || seen.size() >= MaxMessages || !seen.insert(message).second)
        return;

    const auto text = wstring_to_string(std::wstring(message));

    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        LOG_ERROR("FSRD validation: {}", text);
    else if (type == FFX_API_MESSAGE_TYPE_WARNING)
        LOG_WARN("FSRD validation: {}", text);
    else
        LOG_INFO("FSRD validation: {}", text);
}

void FSRDFeatureDx12::UpdateContextOptions()
{
    // Debug view, validation and the signal types are all fixed at context creation, so changing one from the menu
    // creates a new context; the old one is kept until the GPU is done with it
    auto* config = Config::Instance();
    const bool debugView = config->FsrdDebugView.value_or_default();
    const bool validation = config->FsrdValidation.value_or_default();
    const bool directSignals = config->FsrdSignalType.value_or_default() == 1;

    if ((debugView == _debugView && validation == _validation && directSignals == _directSignals) ||
        !_denoiserContextCreated || _denoiserDisabled)
    {
        return;
    }

    RetireDenoiserContext();
    _debugView = debugView;
    _validation = validation;
    _directSignals = directSignals;

    for (auto& sent : _tuningSent)
        sent = false;

    if (!CreateDenoiserContext())
    {
        _denoiserFailures = MaxDenoiserFailures;
        DisableDenoiserOnFailure("Can't recreate the denoiser context for the new options");
    }
}

bool FSRDFeatureDx12::ResizeForRenderSize(unsigned int renderWidth, unsigned int renderHeight)
{
    LOG_INFO("Render size {}x{} is larger than the denoiser's {}x{}, recreating its scratch and context", renderWidth,
             renderHeight, _maxRenderWidth, _maxRenderHeight);

    for (auto resource : { &_diffuseIn, &_diffuseOut, &_specularIn, &_specularOut, &_linearDepth, &_packedNormals,
                           &_motionVectors })
    {
        Retire(*resource);
    }

    RetireDenoiserContext();

    _maxRenderWidth = std::max(_maxRenderWidth, renderWidth);
    _maxRenderHeight = std::max(_maxRenderHeight, renderHeight);

    for (auto& sent : _tuningSent)
        sent = false;

    if (CreateScratchResources() && CreateDenoiserContext())
        return true;

    _denoiserFailures = MaxDenoiserFailures;
    DisableDenoiserOnFailure("Can't recreate the denoiser for the new render size");
    return false;
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
           (_sentinelCounter != nullptr || CreateScratchTexture(Device, 2, 1, DXGI_FORMAT_R32_UINT, write,
                                                                L"FSRD_SentinelCounter", &_sentinelCounter));
}

std::array<ID3D12Resource**, 11> FSRDFeatureDx12::ScratchResources()
{
    return { &_diffuseIn,     &_diffuseOut,      &_specularIn,    &_specularOut,  &_linearDepth,  &_packedNormals,
             &_motionVectors, &_sentinelCounter, &_composedColor, &_reactiveMask, &_bypassColor };
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

    if (_composedColor != nullptr && _reactiveMask != nullptr)
    {
        auto desc = _composedColor->GetDesc();

        if (desc.Width == colorDesc.Width && desc.Height == colorDesc.Height && desc.Format == colorDesc.Format)
            return true;
    }

    // Can still be in flight from the previous frames
    Retire(_composedColor);
    Retire(_reactiveMask);

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    return CreateScratchTexture(Device, (UINT) colorDesc.Width, colorDesc.Height, colorDesc.Format, read,
                                L"FSRD_ComposedColor", &_composedColor) &&
           CreateScratchTexture(Device, (UINT) colorDesc.Width, colorDesc.Height, DXGI_FORMAT_R8_UNORM, read,
                                L"FSRD_ReactiveMask", &_reactiveMask);
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
    _validation = Config::Instance()->FsrdValidation.value_or_default();
    _directSignals = Config::Instance()->FsrdSignalType.value_or_default() == 1;

    // Scratch and the denoiser context start at the render size the feature was created with (it follows the
    // game's output resolution and upscaling mode); a frame that renders larger grows them (ResizeForRenderSize)
    _maxRenderWidth = RenderWidth() > 0 ? RenderWidth() : DisplayWidth();
    _maxRenderHeight = RenderHeight() > 0 ? RenderHeight() : DisplayHeight();

    _prepass = std::make_unique<FsrdPass_Dx12>("FSRD Prepass", Device, 8, 6, fsrd_prepass_cso, sizeof(fsrd_prepass_cso));
    _compose = std::make_unique<FsrdPass_Dx12>("FSRD Compose", Device, 9, 2, fsrd_compose_cso, sizeof(fsrd_compose_cso));
    _particlesPass = std::make_unique<FsrdPass_Dx12>("FSRD Particles", Device, 2, 1, fsrd_particles_cso,
                                                     sizeof(fsrd_particles_cso));
    // Diagnostics only, the denoiser works without it
    _probe = std::make_unique<FsrdProbe>(Device);

    if (!_prepass->CanRender() || !_compose->CanRender() || !_particlesPass->CanRender() || !CreateScratchResources() ||
        !CreateDenoiserContext())
    {
        LOG_ERROR("FSR Ray Regeneration init failed, only upscaling");
        ReleaseScratchResources();
        _prepass.reset();
        _compose.reset();
        _particlesPass.reset();
        _probe.reset();
        _denoiserDisabled = true;
    }

    return true;
}

bool FSRDFeatureDx12::GatherInputs(NVSDK_NGX_Parameter* InParameters, FrameInputs& in)
{
    in.color = GetResource(InParameters, NVSDK_NGX_Parameter_Color);
    in.motionVectors = GetResource(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    in.depth = GetResource(InParameters, NVSDK_NGX_Parameter_Depth);
    in.normals = GetResource(InParameters, DlssdKeys::Normals);
    in.roughness = GetResource(InParameters, DlssdKeys::Roughness);
    in.diffuseAlbedo = GetResource(InParameters, DlssdKeys::DiffuseAlbedo);
    in.specularAlbedo = GetResource(InParameters, DlssdKeys::SpecularAlbedo);
    in.specularHitDistance = GetResource(InParameters, DlssdKeys::SpecularHitDistance);
    in.particles = GetParticleLayer(InParameters, in.color);

    if (in.color == nullptr || in.motionVectors == nullptr || in.depth == nullptr || in.normals == nullptr ||
        in.diffuseAlbedo == nullptr || in.specularAlbedo == nullptr)
    {
        LOG_WARN("Skipping denoise, missing input (color: {}, mv: {}, depth: {}, normals: {}, diffuseAlbedo: {}, "
                 "specularAlbedo: {})",
                 (void*) in.color, (void*) in.motionVectors, (void*) in.depth, (void*) in.normals,
                 (void*) in.diffuseAlbedo, (void*) in.specularAlbedo);
        return false;
    }

    GetRenderResolution(InParameters, &in.renderWidth, &in.renderHeight);

    if (in.renderWidth == 0 || in.renderHeight == 0)
        return false;

    // Never larger than the game's color texture, which holds the render area
    auto colorDesc = in.color->GetDesc();
    in.renderWidth = std::min(in.renderWidth, (unsigned int) colorDesc.Width);
    in.renderHeight = std::min(in.renderHeight, (unsigned int) colorDesc.Height);

    if ((in.renderWidth > _maxRenderWidth || in.renderHeight > _maxRenderHeight) &&
        !ResizeForRenderSize(in.renderWidth, in.renderHeight))
    {
        return false;
    }

    if (!PrepareComposedColor(in.color))
    {
        DisableDenoiserOnFailure("Can't create the composed color texture");
        return false;
    }

    bool found = false;
    auto view = ReadMatrix(InParameters, DlssdKeys::WorldToViewMatrix, &found);
    in.hasView = found;
    auto projection = ReadMatrix(InParameters, DlssdKeys::ViewToClipMatrix, &found);

    if (!found || projection.rows[3].z == 0.0f)
    {
        LOG_WARN("Skipping denoise, no usable ViewToClipMatrix to linearize depth");
        return false;
    }

    memcpy(in.view, &view, sizeof(in.view));
    memcpy(in.projection, &projection, sizeof(in.projection));
    in.separateRoughness = !_roughnessPacked && in.roughness != nullptr;
    return true;
}

FsrdPass_Dx12::Constants FSRDFeatureDx12::BuildConstants(const FrameInputs& in, NVSDK_NGX_Parameter* InParameters)
{
    auto* config = Config::Instance();

    // Modes are read every frame so the menu applies immediately; the history doesn't fit another mode
    const int splitMode = std::clamp(config->FsrdSplitMode.value_or_default(), 0, 2);

    if (splitMode != _lastSplitMode)
    {
        LOG_INFO("Signal split mode: {}", splitMode);
        _lastSplitMode = splitMode;
        _resetDenoiserHistory = true;
    }

    const int sentinelMode = std::clamp(config->FsrdSentinelMode.value_or_default(), 0, 1);

    if (sentinelMode != _lastSentinelMode)
    {
        LOG_INFO("Sentinel pixels mode: {}", sentinelMode == 0 ? "passthrough" : "diffuse");
        _lastSentinelMode = sentinelMode;
        _resetDenoiserHistory = true;
    }

    // DLSS motion vectors point to the previous frame and turn into pixels with MV scale, FFX wants
    // PreviousUV - CurrentUV, the prepass converts them and adds the linear depth delta
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    const float mvWidth = (float) (LowResMV() ? in.renderWidth : DisplayWidth());
    const float mvHeight = (float) (LowResMV() ? in.renderHeight : DisplayHeight());

    FsrdPass_Dx12::Constants constants {};
    constants.ProjZZ = in.projection[2][2];
    constants.ProjWZ = in.projection[3][2];
    constants.DiffuseHitDistance = config->FsrdDiffuseHitDistance.value_or_default();
    constants.Flags = (in.separateRoughness ? FlagSeparateRoughness : 0) |
                      (in.specularHitDistance != nullptr ? FlagHasSpecHitDistance : 0) |
                      (DepthInverted() ? FlagDepthInverted : 0) | (in.particles != nullptr ? FlagParticleLayer : 0);
    constants.RenderWidth = in.renderWidth;
    constants.RenderHeight = in.renderHeight;
    constants.SkyDepth = SkyDepth;
    constants.SplitMode = (uint32_t) splitMode;

    constants.MvScale[0] = mvScaleX / mvWidth;
    constants.MvScale[1] = mvScaleY / mvHeight;
    constants.MvTexelScale[0] = LowResMV() ? 1.0f : mvWidth / (float) in.renderWidth;
    constants.MvTexelScale[1] = LowResMV() ? 1.0f : mvHeight / (float) in.renderHeight;
    constants.InvProj[0] = in.projection[0][0] != 0.0f ? 1.0f / in.projection[0][0] : 1.0f;
    constants.InvProj[1] = in.projection[1][1] != 0.0f ? 1.0f / in.projection[1][1] : 1.0f;
    constants.HasPrevView = (in.hasView && _hasPrevView) ? 1 : 0;
    constants.MaxStaticMvError = 2.0f / (float) in.renderWidth; // ~2 render pixels

    if (constants.HasPrevView)
    {
        float toPrev[4][4];
        ViewToPrevView(in.view, _prevView, toPrev);

        for (int i = 0; i < 4; i++)
        {
            constants.PrevViewX[i] = toPrev[i][0];
            constants.PrevViewY[i] = toPrev[i][1];
            constants.PrevViewZ[i] = toPrev[i][2];
        }
    }

    constants.ReflectionReactive = std::clamp(config->FsrdReflectionReactive.value_or_default(), 0.0f, 1.0f);
    constants.GameMaskBias = config->DlssReactiveMaskBias.value_or_default();
    constants.SpecHitScale = std::max(config->FsrdSpecHitDistanceScale.value_or_default(), 0.0001f);
    constants.RoughnessOverride = config->FsrdRoughnessOverrideEnabled.value_or_default()
                                      ? std::clamp(config->FsrdRoughnessOverride.value_or_default(), 0.0f, 1.0f)
                                      : -1.0f;

    // Camera forward in world space: third column of the row-vector view rotation
    constants.CameraForward[0] = in.view[0][2];
    constants.CameraForward[1] = in.view[1][2];
    constants.CameraForward[2] = in.view[2][2];
    constants.SentinelMode = (uint32_t) sentinelMode;

    _sentinelSlot = _sentinelFrame++ % 2;
    constants.CounterSlot = _sentinelSlot;

    // The sentinel count (atomics in the prepass) is only for the probe log
    if (_probe != nullptr && _probe->Sampling())
        constants.Flags |= FlagCountSentinels;

    return constants;
}

bool FSRDFeatureDx12::RunPrepass(ID3D12GraphicsCommandList* InCommandList, const FrameInputs& in,
                                 const FsrdPass_Dx12::Constants& constants)
{
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    Barriers(InCommandList, { { _diffuseIn, read, write },
                              { _specularIn, read, write },
                              { _linearDepth, read, write },
                              { _packedNormals, read, write },
                              { _motionVectors, read, write } });

    const bool ok = _prepass->Dispatch(InCommandList,
                                       { in.color, in.diffuseAlbedo, in.specularAlbedo, in.normals,
                                         in.separateRoughness ? in.roughness : nullptr, in.depth,
                                         in.specularHitDistance, in.motionVectors },
                                       { _diffuseIn, _specularIn, _linearDepth, _packedNormals, _motionVectors,
                                         _sentinelCounter },
                                       constants, in.renderWidth, in.renderHeight);

    Barriers(InCommandList, { { _diffuseIn, write, read },
                              { _specularIn, write, read },
                              { _linearDepth, write, read },
                              { _packedNormals, write, read },
                              { _motionVectors, write, read } });

    if (!ok)
        DisableDenoiserOnFailure("FSRD prepass dispatch failed");

    return ok;
}

bool FSRDFeatureDx12::RunDenoiser(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                                  const FrameInputs& in)
{
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Same signal resources either way; the context was created for indirect (hit distance in A) or direct (A
    // ignored) signals, and the matching descs must be chained
    FfxApiDenoiserSignal diffuseSignal {};
    diffuseSignal.input = ffxApiGetResourceDX12(_diffuseIn, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    diffuseSignal.output = ffxApiGetResourceDX12(_diffuseOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxApiDenoiserSignal specularSignal {};
    specularSignal.input = ffxApiGetResourceDX12(_specularIn, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    specularSignal.output = ffxApiGetResourceDX12(_specularOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescDenoiserIndirectDiffuse indirectDiffuseDesc {};
    indirectDiffuseDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
    indirectDiffuseDesc.signal = diffuseSignal;

    ffxDispatchDescDenoiserIndirectSpecular indirectSpecularDesc {};
    indirectSpecularDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
    indirectSpecularDesc.signal = specularSignal;

    ffxDispatchDescDenoiserDirectDiffuse directDiffuseDesc {};
    directDiffuseDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
    directDiffuseDesc.signal = diffuseSignal;

    ffxDispatchDescDenoiserDirectSpecular directSpecularDesc {};
    directSpecularDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
    directSpecularDesc.signal = specularSignal;

    ffxDispatchDescHeader* diffuseHeader = _directSignals ? &directDiffuseDesc.header : &indirectDiffuseDesc.header;
    ffxDispatchDescHeader* specularHeader = _directSignals ? &directSpecularDesc.header : &indirectSpecularDesc.header;
    diffuseHeader->pNext = specularHeader;

    ffxDispatchDescDenoiser dispatchDesc {};
    dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    dispatchDesc.header.pNext = diffuseHeader;
    dispatchDesc.commandList = InCommandList;

    dispatchDesc.linearDepth = ffxApiGetResourceDX12(_linearDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.normals = ffxApiGetResourceDX12(_packedNormals, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxApiGetResourceDX12(_motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.specularAlbedo = ffxApiGetResourceDX12(in.specularAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.diffuseAlbedo = ffxApiGetResourceDX12(in.diffuseAlbedo, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    // Already PreviousUV - CurrentUV with the depth delta in B (prepass)
    dispatchDesc.motionVectorScale = { 1.0f, 1.0f, 1.0f };

    float jitterX = 0.0f, jitterY = 0.0f;
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);
    dispatchDesc.jitterOffsets = { jitterX, jitterY };

    dispatchDesc.cameraPositionDelta = { 0.0f, 0.0f, 0.0f };

    if (in.hasView)
    {
        if (_hasPrevView)
        {
            float cameraPos[3], prevCameraPos[3];
            CameraPosition(in.view, cameraPos);
            CameraPosition(_prevView, prevCameraPos);

            dispatchDesc.cameraPositionDelta = { prevCameraPos[0] - cameraPos[0], prevCameraPos[1] - cameraPos[1],
                                                 prevCameraPos[2] - cameraPos[2] };
        }

        memcpy(_prevView, in.view, sizeof(_prevView));
        _hasPrevView = true;
    }

    memcpy(&dispatchDesc.view, in.view, sizeof(dispatchDesc.view));
    memcpy(&dispatchDesc.projection, in.projection, sizeof(dispatchDesc.projection));
    dispatchDesc.linearDepthBounds = { 0.0f, SkyDepthBound };
    dispatchDesc.renderSize = { in.renderWidth, in.renderHeight };
    dispatchDesc.frameIndex = _denoiserFrameIndex++;

    unsigned int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    // DLSS-RR albedos are linear, not sqrt encoded
    dispatchDesc.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO;

    if (_resetDenoiserHistory || reset == 1)
        dispatchDesc.flags |= FFX_DENOISER_DISPATCH_RESET;

    // Debug view: the overview is drawn into the composed color, which then replaces the image
    ffxDispatchDescDenoiserDebugView debugDesc {};

    if (_debugView)
    {
        auto composedDesc = _composedColor->GetDesc();
        debugDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW;
        debugDesc.output = ffxApiGetResourceDX12(_composedColor, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        debugDesc.outputSize = { (uint32_t) composedDesc.Width, composedDesc.Height };
        debugDesc.mode = FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW;
        specularHeader->pNext = &debugDesc.header;

        Barriers(InCommandList, { { _composedColor, read, write } });
    }

    auto result = FfxApiProxy::D3D12_Dispatch(&_denoiserContext, &dispatchDesc.header);

    if (_debugView)
        Barriers(InCommandList, { { _composedColor, write, read } });

    if (result != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Denoiser ffxDispatch failed: {}", FfxApiProxy::ReturnCodeToString(result));
        DisableDenoiserOnFailure("FSR Ray Regeneration dispatch failed");
        return false;
    }

    _resetDenoiserHistory = false;
    return true;
}

bool FSRDFeatureDx12::RunCompose(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                                 const FrameInputs& in, const FsrdPass_Dx12::Constants& constants)
{
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    Barriers(InCommandList, { { _diffuseOut, write, read },
                              { _specularOut, write, read },
                              { _composedColor, read, write },
                              { _reactiveMask, read, write } });

    auto gameReactive = GetResource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
    auto composedDesc = _composedColor->GetDesc();

    const bool ok = _compose->Dispatch(InCommandList,
                                       { in.color, in.diffuseAlbedo, in.specularAlbedo, in.depth, _diffuseOut,
                                         _specularOut, gameReactive, _packedNormals, in.particles },
                                       { _composedColor, _reactiveMask }, constants, (UINT) composedDesc.Width,
                                       composedDesc.Height);

    Barriers(InCommandList, { { _diffuseOut, read, write },
                              { _specularOut, read, write },
                              { _composedColor, write, read },
                              { _reactiveMask, write, read } });

    if (!ok)
        DisableDenoiserOnFailure("FSRD compose dispatch failed");

    return ok;
}

void FSRDFeatureDx12::RunProbe(ID3D12GraphicsCommandList* InCommandList, const FrameInputs& in,
                               const FsrdPass_Dx12::Constants& constants)
{
    if (_probe == nullptr)
        return;

    _probe->Run(InCommandList, constants,
                { in.specularHitDistance, in.normals, in.diffuseAlbedo, in.specularAlbedo, _linearDepth,
                  _packedNormals, _sentinelCounter, _sentinelSlot },
                in.view);
}

ID3D12Resource* FSRDFeatureDx12::Denoise(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    FrameInputs in {};

    if (!GatherInputs(InParameters, in))
        return nullptr;

    const auto constants = BuildConstants(in, InParameters);
    ApplyTuning();

    // 1. Prepass: game resources -> demodulated signals, linear depth, packed normals, MVs with depth delta
    if (!RunPrepass(InCommandList, in, constants))
        return nullptr;

    // 2. FSR Ray Regeneration
    if (!RunDenoiser(InCommandList, InParameters, in))
        return nullptr;

    if (_debugView)
    {
        // The probe reads the inputs, which are the same in debug view
        RunProbe(InCommandList, in, constants);
        return _composedColor;
    }

    // 3. Compose: denoised signals * albedo (+ particle layer) -> composed color and reactive mask
    if (!RunCompose(InCommandList, InParameters, in, constants))
        return nullptr;

    RunProbe(InCommandList, in, constants);
    return _composedColor;
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
    if (_particlesPass == nullptr || !_particlesPass->CanRender())
        return nullptr;

    auto color = GetResource(InParameters, NVSDK_NGX_Parameter_Color);
    auto particles = GetParticleLayer(InParameters, color);

    if (particles == nullptr)
        return nullptr;

    unsigned int renderWidth = 0, renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);

    if (renderWidth == 0 || renderHeight == 0)
        return nullptr;

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto colorDesc = color->GetDesc();

    // Only created once the bypass is used, same size/format as the game's color
    if (_bypassColor != nullptr)
    {
        auto desc = _bypassColor->GetDesc();

        if (desc.Width != colorDesc.Width || desc.Height != colorDesc.Height || desc.Format != colorDesc.Format)
            Retire(_bypassColor);
    }

    if (_bypassColor == nullptr && !CreateScratchTexture(Device, (UINT) colorDesc.Width, colorDesc.Height,
                                                         colorDesc.Format, read, L"FSRD_BypassColor", &_bypassColor))
    {
        return nullptr;
    }

    FsrdPass_Dx12::Constants constants {};
    constants.RenderWidth = std::min(renderWidth, (unsigned int) colorDesc.Width);
    constants.RenderHeight = std::min(renderHeight, (unsigned int) colorDesc.Height);

    Barriers(InCommandList, { { _bypassColor, read, write } });

    const bool ok = _particlesPass->Dispatch(InCommandList, { color, particles }, { _bypassColor }, constants,
                                             (UINT) colorDesc.Width, colorDesc.Height);

    Barriers(InCommandList, { { _bypassColor, write, read } });

    return ok ? _bypassColor : nullptr;
}

bool FSRDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LogInputsOnce(InParameters);

    _evalFrame++;
    ReleaseRetired();

    // ~30 s at 60 fps, only with the Diagnostics option
    if (_evalFrame % 1800 == 0 && Config::Instance()->FsrdDiagnostics.value_or_default())
        LogVideoMemory("periodic");

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

    UpdateContextOptions();

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
