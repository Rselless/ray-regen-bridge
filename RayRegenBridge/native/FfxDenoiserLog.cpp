#include "pch.h"
#include "FfxDenoiserLog.h"

#include <proxies/FfxApi_Proxy.h>
#include <RayRegenBridge/feature/ffx_denoiser_compat.h>

#include <magic_enum.hpp>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>

namespace
{
constexpr uint32_t EffectMask = 0x00ff0000u;
constexpr uint32_t RadianceCacheEffect = 0x00060000u;
constexpr size_t MaxConfigureLogs = 64;
constexpr size_t MaxDispatchShapes = 16;
constexpr uint32_t SampleDispatches[] = { 300, 3000, 20000 };

std::mutex _mutex;
uint32_t _dispatchCount = 0;
std::set<std::string> _configured;
std::unordered_set<uint64_t> _queried;
std::unordered_set<std::string> _dispatchShapes;

const char* DescName(uint64_t type)
{
    switch (type)
    {
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER:
        return "create";
    case FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE:
        return "configure key/value";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER:
        return "dispatch";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW:
        return "debug view";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION:
        return "ambient occlusion";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE:
        return "direct diffuse";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR:
        return "direct specular";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT:
        return "dominant light";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE:
        return "indirect diffuse";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR:
        return "indirect specular";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION:
        return "specular occlusion";
    case FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE:
        return "query default key/value";
    case FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE:
        return "query GPU memory";
    default:
        return (type & EffectMask) == RadianceCacheEffect ? "radiance cache" : "other";
    }
}

std::string DescChain(const ffxApiHeader* header)
{
    std::string chain;

    for (; header != nullptr; header = header->pNext)
    {
        if (!chain.empty())
            chain += " + ";

        chain += std::format("{} ({:#x})", DescName(header->type), header->type);
    }

    return chain;
}

std::string SignalNames(uint32_t flags)
{
    static constexpr std::pair<uint32_t, const char*> names[] = {
        { FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION, "ambient occlusion" },
        { FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE, "direct diffuse" },
        { FFX_DENOISER_SIGNAL_DIRECT_SPECULAR, "direct specular" },
        { FFX_DENOISER_SIGNAL_DOMINANT_LIGHT_VISIBILITY, "dominant light visibility" },
        { FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE, "indirect diffuse" },
        { FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR, "indirect specular" },
        { FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION, "specular occlusion" },
    };

    std::string text;

    for (const auto& [flag, name] : names)
    {
        if ((flags & flag) == 0)
            continue;

        if (!text.empty())
            text += ", ";

        text += name;
    }

    return text.empty() ? "none" : text;
}

// Size and DXGI format from the D3D12 resource itself, comparable with the DLSSD input log
std::string ResourceText(const FfxApiResource& res)
{
    if (res.resource == nullptr)
        return "null";

    const auto desc = static_cast<ID3D12Resource*>(res.resource)->GetDesc();
    const auto formatName = magic_enum::enum_name(desc.Format);

    return std::format("{}x{} format {} {} (ffx format {}, state {:#x})", desc.Width, desc.Height, (int) desc.Format,
                       formatName.empty() ? "" : std::string(formatName), res.description.format, res.state);
}

std::string ResourceShape(const FfxApiResource& res)
{
    if (res.resource == nullptr)
        return "null";

    const auto desc = static_cast<ID3D12Resource*>(res.resource)->GetDesc();
    return std::format("{}x{}:{}", desc.Width, desc.Height, (int) desc.Format);
}

void LogMatrix(const char* name, const FfxApiMatrix4x4& m)
{
    LOG_INFO("  {}:", name);

    for (const auto& row : m.rows)
        LOG_INFO("    [{:.4f} {:.4f} {:.4f} {:.4f}]", row.x, row.y, row.z, row.w);
}

void LogSignal(const char* name, const FfxApiDenoiserSignal& signal)
{
    LOG_INFO("  {} input: {}", name, ResourceText(signal.input));
    LOG_INFO("  {} output: {}", name, ResourceText(signal.output));

    if (signal.checkerboardOrigin != 0)
        LOG_INFO("  {} checkerboard origin: {}", name, signal.checkerboardOrigin);
}

const FfxApiDenoiserSignal* SignalOf(const ffxApiHeader* header)
{
    switch (header->type)
    {
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION:
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE:
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR:
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT:
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE:
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR:
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION:
        // Every signal desc is the header followed by one FfxApiDenoiserSignal
        return &reinterpret_cast<const ffxDispatchDescDenoiserIndirectDiffuse*>(header)->signal;

    default:
        return nullptr;
    }
}

// Signals, flags, sizes and formats: a dispatch is logged in full again only when one of these changes
std::string DispatchShape(const ffxApiHeader* header)
{
    std::string shape;

    for (; header != nullptr; header = header->pNext)
    {
        shape += std::format("{:#x};", header->type);

        if (header->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER)
        {
            const auto d = reinterpret_cast<const ffxDispatchDescDenoiser*>(header);
            shape += std::format("{}x{};{:#x};{};{};{};{};{};", d->renderSize.width, d->renderSize.height,
                                 d->flags & ~FFX_DENOISER_DISPATCH_RESET, ResourceShape(d->linearDepth),
                                 ResourceShape(d->motionVectors), ResourceShape(d->normals),
                                 ResourceShape(d->diffuseAlbedo), ResourceShape(d->specularAlbedo));
        }
        else if (const auto signal = SignalOf(header); signal != nullptr)
        {
            shape += std::format("{};{};", ResourceShape(signal->input), ResourceShape(signal->output));
        }
    }

    return shape;
}
} // namespace

namespace FfxDenoiserLog
{
bool IsDenoiserChain(const ffxApiHeader* header)
{
    for (; header != nullptr; header = header->pNext)
    {
        const auto effect = header->type & EffectMask;

        if (effect == FFX_API_EFFECT_ID_DENOISER || effect == RadianceCacheEffect)
            return true;
    }

    return false;
}

void LogCreate(const ffxApiHeader* header, ffxReturnCode_t result, ffxContext context)
{
    LOG_INFO("Game denoiser context: {}, result: {} ({}), context: {:X}", DescChain(header),
             FfxApiProxy::ReturnCodeToString(result), (uint32_t) result, (size_t) context);

    for (; header != nullptr; header = header->pNext)
    {
        if (header->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER)
            continue;

        const auto d = reinterpret_cast<const ffxCreateContextDescDenoiser*>(header);
        LOG_INFO("  version: {}.{}.{}, max render size: {}x{}, create flags: {:#x}", d->version >> 22,
                 (d->version >> 12) & 0x3ff, d->version & 0xfff, d->maxRenderSize.width, d->maxRenderSize.height,
                 d->flags);
        LOG_INFO("  signals: {} ({:#x})", SignalNames(d->signalFlags), d->signalFlags);

        if (d->checkerboardSignalFlags != 0)
            LOG_INFO("  checkerboard signals: {} ({:#x})", SignalNames(d->checkerboardSignalFlags),
                     d->checkerboardSignalFlags);
    }
}

void LogConfigure(const ffxApiHeader* header, ffxReturnCode_t result)
{
    if (header->type != FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE)
    {
        std::scoped_lock lock(_mutex);

        if (_configured.size() < MaxConfigureLogs && _configured.insert(DescChain(header)).second)
            LOG_INFO("Game denoiser configure: {}, result: {}", DescChain(header),
                     FfxApiProxy::ReturnCodeToString(result));

        return;
    }

    const auto d = reinterpret_cast<const ffxConfigureDescDenoiserKeyValue*>(header);
    std::string values;

    if (d->data != nullptr)
    {
        const auto floats = static_cast<const float*>(d->data);

        for (uint64_t i = 0; i < d->count && i < 4; i++)
            values += std::format("{}{}", i == 0 ? "" : ", ", floats[i]);
    }

    auto key = magic_enum::enum_name((FfxApiConfigureDenoiserKey) d->key);
    auto text = std::format("key {} ({}) = {}", d->key, key.empty() ? "unknown" : std::string(key),
                            values.empty() ? "null" : values);

    std::scoped_lock lock(_mutex);

    if (_configured.size() < MaxConfigureLogs && _configured.insert(text).second)
        LOG_INFO("Game denoiser configure: {}, result: {}", text, FfxApiProxy::ReturnCodeToString(result));
}

void LogQuery(const ffxApiHeader* header, ffxReturnCode_t result)
{
    {
        std::scoped_lock lock(_mutex);

        if (!_queried.insert(header->type).second)
            return;
    }

    LOG_INFO("Game denoiser query: {}, result: {}", DescChain(header), FfxApiProxy::ReturnCodeToString(result));
}

void LogDispatch(const ffxApiHeader* header, ffxReturnCode_t result)
{
    uint32_t count = 0;

    {
        const auto shape = DispatchShape(header);
        std::scoped_lock lock(_mutex);
        count = ++_dispatchCount;

        // A few samples well after the first frame too: the first one has no camera movement or history yet
        const bool sample = std::find(std::begin(SampleDispatches), std::end(SampleDispatches), count) !=
                            std::end(SampleDispatches);
        const bool newShape = _dispatchShapes.size() < MaxDispatchShapes && _dispatchShapes.insert(shape).second;

        if (!sample && !newShape)
            return;
    }

    LOG_INFO("Game denoiser dispatch #{}: {}, result: {}", count, DescChain(header),
             FfxApiProxy::ReturnCodeToString(result));

    for (; header != nullptr; header = header->pNext)
    {
        if (header->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER)
        {
            const auto d = reinterpret_cast<const ffxDispatchDescDenoiser*>(header);
            LOG_INFO("  render size: {}x{}, frame index: {}, flags: {:#x}", d->renderSize.width,
                     d->renderSize.height, d->frameIndex, d->flags);
            LOG_INFO("  linear depth: {}", ResourceText(d->linearDepth));
            LOG_INFO("  motion vectors: {}", ResourceText(d->motionVectors));
            LOG_INFO("  normals: {}", ResourceText(d->normals));
            LOG_INFO("  diffuse albedo: {}", ResourceText(d->diffuseAlbedo));
            LOG_INFO("  specular albedo: {}", ResourceText(d->specularAlbedo));
            LOG_INFO("  MV scale: ({}, {}, {}), jitter: ({}, {}), camera delta: ({}, {}, {})",
                     d->motionVectorScale.x, d->motionVectorScale.y, d->motionVectorScale.z, d->jitterOffsets.x,
                     d->jitterOffsets.y, d->cameraPositionDelta.x, d->cameraPositionDelta.y,
                     d->cameraPositionDelta.z);
            LOG_INFO("  linear depth bounds: [{}, {}]", d->linearDepthBounds.min, d->linearDepthBounds.max);
            LogMatrix("view", d->view);
            LogMatrix("projection", d->projection);
        }
        else if (header->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT)
        {
            const auto d = reinterpret_cast<const ffxDispatchDescDenoiserDominantLight*>(header);
            LogSignal(DescName(header->type), d->signal);
            LOG_INFO("  dominant light direction: ({}, {}, {}), emission: ({}, {}, {}), angular radius: {}",
                     d->direction.x, d->direction.y, d->direction.z, d->emission.x, d->emission.y, d->emission.z,
                     d->angularRadius);
        }
        else if (header->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW)
        {
            const auto d = reinterpret_cast<const ffxDispatchDescDenoiserDebugView*>(header);
            LOG_INFO("  debug view: mode {}, viewport {}, output {}", d->mode, d->viewportIndex,
                     ResourceText(d->output));
        }
        else if (const auto signal = SignalOf(header); signal != nullptr)
        {
            LogSignal(DescName(header->type), *signal);
        }
    }
}
} // namespace FfxDenoiserLog
