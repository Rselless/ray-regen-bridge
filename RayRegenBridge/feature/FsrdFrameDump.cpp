#include "pch.h"
#include "FsrdFrameDump.h"
#include "FsrdUtil.h"

#include <Util.h>
#include <RayRegenBridge/shaders/fsrd_dump_Shader.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>

constexpr DXGI_FORMAT DumpFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr UINT DumpBytesPerTexel = 8;
constexpr UINT DumpMaxSources = 32; // copies recorded in one frame; heaps rotate over a few frames of copies
constexpr int DumpWaitFrames = 4;   // frames until a frame's copies are surely done on the GPU

// What a reduced request keeps: enough to reproject one frame onto the next and compare
constexpr const char* ReducedSources[] = { "color", "composed", "ffx_motion_vectors", "linear_depth", "packed_normals" };

FsrdFrameDump::FsrdFrameDump(ID3D12Device* device) : _device(device)
{
    _pass = std::make_unique<FsrdPass_Dx12>("FSRD Dump", device, 1, 1, fsrd_dump_cso, sizeof(fsrd_dump_cso),
                                            DumpMaxSources * 2);
}

FsrdFrameDump::~FsrdFrameDump()
{
    if (State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_target);

    for (auto& job : _jobs)
        SAFE_RELEASE(job.readback);
}

bool FsrdFrameDump::Start(const Request& request)
{
    std::lock_guard lock(_requestMutex);

    if (_hasPending || _busy.load())
        return false;

    _pending = request;
    _pending.count = std::clamp(_pending.count, 1, MaxCount);
    _pending.label.erase(std::remove_if(_pending.label.begin(), _pending.label.end(),
                                        [](char c) { return !std::isalnum((unsigned char) c) && c != '_'; }),
                         _pending.label.end());

    if (_pending.label.empty())
        _pending.label = "frame";

    _startNs = NowNs() + SecondsToNs(std::max(request.delaySeconds, 0.0f));
    _hasPending = true;
    _taken.store(0);
    _requested.store(_pending.count);
    _busy.store(true);
    return true;
}

float FsrdFrameDump::Countdown() const
{
    std::lock_guard lock(_requestMutex);
    return _hasPending ? NsToSeconds(_startNs - NowNs()) : 0.0f;
}

bool FsrdFrameDump::FrameDue()
{
    if (!_running)
    {
        std::lock_guard lock(_requestMutex);

        if (!_hasPending || NowNs() < _startNs)
            return false;

        // The request starts: one folder for all its frames
        _active = _pending;
        _hasPending = false;
        _running = true;
        _nextNs = 0;
        _firstNs = NowNs();

        char stamp[32];
        const auto now = std::time(nullptr);
        std::tm local {};
        localtime_s(&local, &now);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        _folder = (Util::DllPath().parent_path() / "RRB_Captures" / (std::string(stamp) + "_" + _active.label))
                      .string();
    }

    return NowNs() >= _nextNs;
}

void FsrdFrameDump::UpdateBusy()
{
    std::lock_guard lock(_requestMutex);
    _busy.store(_hasPending || _running || !_jobs.empty() || _status->writing.load() > 0);
}

void FsrdFrameDump::Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                        const std::vector<Source>& sources, const std::string& metadata)
{
    // Frames recorded earlier whose GPU work is done
    for (auto it = _jobs.begin(); it != _jobs.end();)
    {
        if (--it->waitFrames > 0)
        {
            ++it;
            continue;
        }

        WriteJob(*it);
        it = _jobs.erase(it);
    }

    if (_running && !metadata.empty())
    {
        const int index = _taken.load();
        Job job;
        job.folder = _active.count > 1 ? std::format("{}\\frame_{:03}", _folder, index) : _folder;
        job.last = index + 1 >= _active.count;
        job.metadata = std::format("{},\n  \"label\": \"{}\",\n  \"frameIndex\": {},\n  \"frameCount\": {},\n"
                                   "  \"timeMs\": {:.2f},\n  \"reduced\": {}",
                                   metadata, _active.label, index, _active.count,
                                   (double) (NowNs() - _firstNs) / 1e6, _active.reduced);

        std::vector<Source> selected;

        for (const auto& source : sources)
        {
            if (!_active.reduced ||
                std::any_of(std::begin(ReducedSources), std::end(ReducedSources),
                            [&](const char* name) { return std::strcmp(name, source.name) == 0; }))
            {
                selected.push_back(source);
            }
        }

        if (Record(cmdList, constants, selected, job))
        {
            job.waitFrames = DumpWaitFrames;
            _jobs.push_back(std::move(job));
            _taken.fetch_add(1);
        }
        else
        {
            SAFE_RELEASE(job.readback);
            LOG_ERROR("Frame dump '{}': frame {} not recorded, request stopped", _active.label, index);
            _taken.store(_active.count);
        }

        if (_taken.load() >= _active.count)
            _running = false;
        else
            _nextNs = NowNs() + SecondsToNs(_active.intervalSeconds);
    }

    UpdateBusy();
}

bool FsrdFrameDump::Record(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                           const std::vector<Source>& sources, Job& job)
{
    if (_pass == nullptr || !_pass->CanRender())
        return false;

    // Readback layout: one tightly sized, 512 aligned footprint per source
    UINT maxWidth = 0, maxHeight = 0;
    UINT64 total = 0;

    for (const auto& source : sources)
    {
        if (source.resource == nullptr || job.entries.size() >= DumpMaxSources)
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
        job.entries.push_back(entry);
    }

    if (job.entries.empty())
        return false;

    if (_target == nullptr || _targetWidth < maxWidth || _targetHeight < maxHeight)
    {
        // Earlier frames' copies may still use the old target: it's only replaced while none are in flight
        if (_target != nullptr && !_jobs.empty())
            return false;

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
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&job.readback))))
    {
        LOG_ERROR("Frame dump: can't create a {} MB readback buffer", total >> 20);
        return false;
    }

    size_t entryIndex = 0;

    for (const auto& source : sources)
    {
        if (source.resource == nullptr || entryIndex >= job.entries.size())
            continue;

        const auto& entry = job.entries[entryIndex++];

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

        CD3DX12_TEXTURE_COPY_LOCATION dst(job.readback, footprint);
        CD3DX12_TEXTURE_COPY_LOCATION src(_target, 0);
        D3D12_BOX box { 0, 0, 0, entry.width, entry.height, 1 };
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

        Fsrd::Barriers(cmdList,
                       { { _target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
    }

    LOG_DEBUG("Frame dump: {} textures recorded ({} MB)", job.entries.size(), total >> 20);
    return true;
}

// .dds with a DX10 header, RGBA16F, rows without padding
static bool WriteDds(const std::filesystem::path& path, const uint8_t* data, UINT width, UINT height, UINT rowPitch)
{
    std::ofstream file(path, std::ios::binary);

    if (!file)
        return false;

    uint32_t header[32] {};
    header[0] = 0x20534444;                     // "DDS "
    header[1] = 124;                            // header size
    header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8; // caps, height, width, pixel format, pitch
    header[3] = height;
    header[4] = width;
    header[5] = width * DumpBytesPerTexel;
    header[7] = 1;           // mip count
    header[19] = 32;         // pixel format size
    header[20] = 0x4;        // FourCC
    header[21] = 0x30315844; // "DX10"
    header[27] = 0x1000;     // texture caps

    uint32_t dx10[5] { (uint32_t) DumpFormat, 3 /* TEXTURE2D */, 0, 1, 0 };

    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));

    for (UINT y = 0; y < height; y++)
    {
        file.write(reinterpret_cast<const char*>(data + (size_t) y * rowPitch),
                   (std::streamsize) width * DumpBytesPerTexel);
    }

    return file.good();
}

void FsrdFrameDump::WriteJob(Job& job)
{
    const UINT64 size = job.readback->GetDesc().Width;
    void* mapped = nullptr;
    D3D12_RANGE readRange { 0, (SIZE_T) size };

    if (FAILED(job.readback->Map(0, &readRange, &mapped)))
    {
        LOG_ERROR("Frame dump: can't map the readback buffer");
        SAFE_RELEASE(job.readback);
        return;
    }

    // Copy out here, the files are written on another thread so the game doesn't wait for the disk
    auto data = std::make_shared<std::vector<uint8_t>>((uint8_t*) mapped, (uint8_t*) mapped + size);
    D3D12_RANGE noWrite { 0, 0 };
    job.readback->Unmap(0, &noWrite);
    SAFE_RELEASE(job.readback);

    _status->writing.fetch_add(1);

    std::thread(
        [status = _status, data, entries = job.entries, metadata = job.metadata,
         folder = std::filesystem::path(job.folder), last = job.last,
         root = std::filesystem::path(_folder)]()
        {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);

            if (ec)
            {
                LOG_ERROR("Frame dump: can't create {}", folder.string());
            }
            else
            {
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
            }

            if (last)
            {
                LOG_INFO("Frame dump written to {}", root.string());
                std::lock_guard lock(status->mutex);
                status->lastFolder = root.string();
            }

            status->writing.fetch_sub(1);
        })
        .detach();
}
