#include "pch.h"
#include "FsrdFrameDump.h"
#include "FsrdUtil.h"

#include <Util.h>
#include <RayRegenBridge/shaders/fsrd_dump_Shader.h>

#include <filesystem>
#include <fstream>
#include <thread>

constexpr DXGI_FORMAT DumpFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr UINT DumpBytesPerTexel = 8;
constexpr UINT DumpMaxSources = 32; // one descriptor heap per copy recorded in the frame
constexpr int DumpWaitFrames = 4;   // frames until the copies are surely done on the GPU

FsrdFrameDump::FsrdFrameDump(ID3D12Device* device) : _device(device)
{
    _pass = std::make_unique<FsrdPass_Dx12>("FSRD Dump", device, 1, 1, fsrd_dump_cso, sizeof(fsrd_dump_cso),
                                            DumpMaxSources);
}

FsrdFrameDump::~FsrdFrameDump()
{
    if (State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_target);
    ReleaseReadback();
}

void FsrdFrameDump::ReleaseReadback()
{
    SAFE_RELEASE(_readback);
    _entries.clear();
}

void FsrdFrameDump::Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                        const std::vector<Source>& sources, const std::string& metadata)
{
    if (_waitFrames.load() > 0)
    {
        if (_waitFrames.fetch_sub(1) == 1)
            WriteFiles();

        return;
    }

    const int64_t requestNs = _requestNs.load();

    if (requestNs == 0 || NowNs() < requestNs)
        return;

    _requestNs.store(0);
    _metadata = metadata;

    if (Record(cmdList, constants, sources))
        _waitFrames.store(DumpWaitFrames);
    else
        ReleaseReadback();
}

bool FsrdFrameDump::Record(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                           const std::vector<Source>& sources)
{
    if (_pass == nullptr || !_pass->CanRender())
        return false;

    // Readback layout: one tightly sized, 512 aligned footprint per source
    ReleaseReadback();
    UINT maxWidth = 0, maxHeight = 0;
    UINT64 total = 0;

    for (const auto& source : sources)
    {
        if (source.resource == nullptr || _entries.size() >= DumpMaxSources)
            continue;

        const auto desc = source.resource->GetDesc();
        Entry entry;
        entry.name = source.name;
        entry.width = (UINT) desc.Width;
        entry.height = desc.Height;
        entry.rowPitch = (entry.width * DumpBytesPerTexel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                         ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        entry.offset = total;
        entry.sourceFormat = desc.Format;
        total += ((UINT64) entry.rowPitch * entry.height + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                 ~(UINT64) (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);

        maxWidth = std::max(maxWidth, entry.width);
        maxHeight = std::max(maxHeight, entry.height);
        _entries.push_back(entry);
    }

    if (_entries.empty())
        return false;

    if (_target == nullptr || _targetWidth < maxWidth || _targetHeight < maxHeight)
    {
        SAFE_RELEASE(_target);

        if (!Fsrd::CreateScratchTexture(_device, maxWidth, maxHeight, DumpFormat,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"FSRD_DumpTarget", &_target))
        {
            return false;
        }

        _targetWidth = maxWidth;
        _targetHeight = maxHeight;
    }

    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_readback))))
    {
        LOG_ERROR("Frame dump: can't create a {} MB readback buffer", total >> 20);
        return false;
    }

    size_t entryIndex = 0;

    for (const auto& source : sources)
    {
        if (source.resource == nullptr || entryIndex >= _entries.size())
            continue;

        const auto& entry = _entries[entryIndex++];

        auto copyConstants = constants;
        copyConstants.RenderWidth = entry.width;
        copyConstants.RenderHeight = entry.height;

        if (!_pass->Dispatch(cmdList, { source.resource }, { _target }, copyConstants, entry.width, entry.height))
        {
            LOG_ERROR("Frame dump: copy of {} failed", entry.name);
            return false;
        }

        Fsrd::Barriers(cmdList,
                       { { _target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE } });

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        footprint.Offset = entry.offset;
        footprint.Footprint = { DumpFormat, entry.width, entry.height, 1, entry.rowPitch };

        CD3DX12_TEXTURE_COPY_LOCATION dst(_readback, footprint);
        CD3DX12_TEXTURE_COPY_LOCATION src(_target, 0);
        D3D12_BOX box { 0, 0, 0, entry.width, entry.height, 1 };
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

        Fsrd::Barriers(cmdList,
                       { { _target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
    }

    LOG_INFO("Frame dump: {} textures recorded ({} MB)", _entries.size(), total >> 20);
    return true;
}

// .dds with a DX10 header, RGBA16F, rows without padding
static bool WriteDds(const std::filesystem::path& path, const uint8_t* data, UINT width, UINT height, UINT rowPitch)
{
    std::ofstream file(path, std::ios::binary);

    if (!file)
        return false;

    uint32_t header[32] {};
    header[0] = 0x20534444; // "DDS "
    header[1] = 124;        // header size
    header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8; // caps, height, width, pixel format, pitch
    header[3] = height;
    header[4] = width;
    header[5] = width * DumpBytesPerTexel;
    header[7] = 1;          // mip count
    header[19] = 32;        // pixel format size
    header[20] = 0x4;       // FourCC
    header[21] = 0x30315844; // "DX10"
    header[27] = 0x1000;    // texture caps

    uint32_t dx10[5] { (uint32_t) DumpFormat, 3 /* TEXTURE2D */, 0, 1, 0 };

    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));

    for (UINT y = 0; y < height; y++)
        file.write(reinterpret_cast<const char*>(data + (size_t) y * rowPitch), (std::streamsize) width * DumpBytesPerTexel);

    return file.good();
}

void FsrdFrameDump::WriteFiles()
{
    if (_readback == nullptr || _entries.empty())
        return;

    const UINT64 size = _readback->GetDesc().Width;
    void* mapped = nullptr;
    D3D12_RANGE readRange { 0, (SIZE_T) size };

    if (FAILED(_readback->Map(0, &readRange, &mapped)))
    {
        LOG_ERROR("Frame dump: can't map the readback buffer");
        ReleaseReadback();
        return;
    }

    // Copy out here, the files are written on another thread so the game doesn't wait for the disk
    auto data = std::make_shared<std::vector<uint8_t>>((uint8_t*) mapped, (uint8_t*) mapped + size);
    D3D12_RANGE noWrite { 0, 0 };
    _readback->Unmap(0, &noWrite);

    auto entries = _entries;
    auto metadata = _metadata;
    ReleaseReadback();

    char stamp[32];
    const auto now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);

    const auto folder = Util::DllPath().parent_path() / "RRB_Captures" / (std::string(stamp) + "_" + _label.load());

    std::thread(
        [status = _status, data, entries, metadata, folder]()
        {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);

            if (ec)
            {
                LOG_ERROR("Frame dump: can't create {}", folder.string());
                return;
            }

            std::string textures;

            for (const auto& entry : entries)
            {
                const auto file = folder / (entry.name + ".dds");

                if (!WriteDds(file, data->data() + entry.offset, entry.width, entry.height, entry.rowPitch))
                    LOG_ERROR("Frame dump: can't write {}", file.string());

                textures += std::format("{}    {{ \"name\": \"{}\", \"width\": {}, \"height\": {}, \"format\": {} }}",
                                        textures.empty() ? "" : ",\n", entry.name, entry.width, entry.height,
                                        (int) entry.sourceFormat);
            }

            std::ofstream json(folder / "capture.json");
            json << "{\n  \"textures\": [\n" << textures << "\n  ],\n" << metadata << "\n}\n";

            LOG_INFO("Frame dump written to {}", folder.string());

            std::lock_guard lock(status->mutex);
            status->lastFolder = folder.string();
        })
        .detach();
}
