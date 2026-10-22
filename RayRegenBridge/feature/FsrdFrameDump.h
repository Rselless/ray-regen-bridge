#pragma once
// FsrdFrameDump -- diagnostics for FSRDFeatureDx12: saves one whole frame of every stage (game inputs, the prepass
// signals, the denoised signals, the composed color) as RGBA16F .dds files plus capture.json, so they can be
// compared pixel by pixel offline (RayRegenBridge/tools/analyze_capture.py). fsrd_dump.hlsl copies each texture
// through an SRV into one FSRD texture, which is copied into a readback buffer; the files are written a few frames
// later, when that frame's GPU work is done.
#include "FsrdPass_Dx12.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class FsrdFrameDump
{
  public:
    static constexpr float DelaySeconds = 2.0f; // time to close the menu before the frame is taken

    struct Source
    {
        const char* name;
        ID3D12Resource* resource; // null = not sent this frame, skipped
    };

    explicit FsrdFrameDump(ID3D12Device* device);
    ~FsrdFrameDump();

    // Menu (other thread): takes a frame DelaySeconds from now, label is the folder name suffix (static string)
    void Request(const char* label)
    {
        _label.store(label);
        _requestNs.store(NowNs() + SecondsToNs(DelaySeconds));
    }

    bool Pending() const { return _requestNs.load() != 0 || _waitFrames > 0; }
    float Countdown() const
    {
        const int64_t ns = _requestNs.load();
        return ns != 0 ? NsToSeconds(ns - NowNs()) : 0.0f;
    }
    // Folder of the last capture, empty until one is written
    std::string LastFolder() const
    {
        std::lock_guard lock(_status->mutex);
        return _status->lastFolder;
    }

    // Every frame after compose: records the copies when a request is due, writes the files when they're done.
    // All sources must be readable as SRVs (NON_PIXEL_SHADER_RESOURCE); metadata is capture.json's body.
    void Run(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
             const std::vector<Source>& sources, const std::string& metadata);

  private:
    struct Entry
    {
        std::string name;
        UINT width = 0, height = 0;
        UINT rowPitch = 0;
        UINT64 offset = 0;
        DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    };

    ID3D12Device* _device = nullptr;
    std::unique_ptr<FsrdPass_Dx12> _pass;
    ID3D12Resource* _target = nullptr; // RGBA16F, UNORDERED_ACCESS between copies
    ID3D12Resource* _readback = nullptr;
    UINT _targetWidth = 0, _targetHeight = 0;

    std::atomic<int64_t> _requestNs { 0 };
    std::atomic<const char*> _label { "frame" };
    std::string _metadata;
    std::vector<Entry> _entries;
    std::atomic<int> _waitFrames { 0 }; // frames left until the readback can be mapped
    // Shared with the writer thread, which can outlive this object
    struct Status
    {
        std::mutex mutex;
        std::string lastFolder;
    };
    std::shared_ptr<Status> _status = std::make_shared<Status>();

    bool Record(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                const std::vector<Source>& sources);
    void WriteFiles();
    void ReleaseReadback();

    static int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static int64_t SecondsToNs(float seconds) { return (int64_t) ((double) seconds * 1e9); }
    static float NsToSeconds(int64_t ns) { return ns > 0 ? (float) ((double) ns / 1e9) : 0.0f; }
};
