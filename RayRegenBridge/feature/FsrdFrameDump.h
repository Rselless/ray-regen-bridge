#pragma once
// FsrdFrameDump -- diagnostics for FSRDFeatureDx12: saves whole frames of every stage (game inputs, the prepass
// signals, the denoised signals, the composed color) as RGBA16F .dds files plus capture.json, so they can be
// compared pixel by pixel offline (RayRegenBridge/tools/analyze_capture.py). fsrd_dump.hlsl copies each texture
// through an SRV into one FSRD texture, which is copied into a readback buffer per frame; each frame's files are
// written a few frames later, when its GPU work is done.
//
// A request takes one frame, or a sequence: count frames, one every interval seconds (0 = consecutive frames,
// a burst). Reduced requests only keep the textures the temporal analysis needs, so bursts stay small.
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
    static constexpr float MenuDelaySeconds = 2.0f; // time to close the menu before the frame is taken
    static constexpr int MaxCount = 64;

    struct Source
    {
        const char* name;
        ID3D12Resource* resource; // null = not sent this frame, skipped
    };

    struct Request
    {
        std::string label = "frame";
        float delaySeconds = 0.0f;
        int count = 1;
        float intervalSeconds = 0.0f;
        bool reduced = false;
    };

    explicit FsrdFrameDump(ID3D12Device* device);
    ~FsrdFrameDump();

    // Any thread: queues a request; refused while another one is waiting or running
    bool Start(const Request& request);

    // Waiting for the delay, taking frames or writing files
    bool Busy() const { return _busy.load(); }

    // For the overlay and menu: seconds until the first frame, frames taken / requested
    float Countdown() const;
    int Taken() const { return _taken.load(); }
    int Requested() const { return _requested.load(); }

    // Folder of the last request, once all its files are written
    std::string LastFolder() const
    {
        std::lock_guard lock(_status->mutex);
        return _status->lastFolder;
    }

    // True when Run records this frame (the caller only builds the metadata then)
    bool FrameDue();

    // Every frame while Busy(), after compose: writes finished frames, and records this frame when the caller found
    // it due (FrameDue) and passes its capture.json body as metadata (empty = not due). All sources must be
    // readable as SRVs (NON_PIXEL_SHADER_RESOURCE).
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

    // One recorded frame waiting for the GPU
    struct Job
    {
        ID3D12Resource* readback = nullptr;
        std::vector<Entry> entries;
        std::string metadata;
        std::string folder;
        int waitFrames = 0;
        bool last = false; // last frame of its request: publishes the folder once written
    };

    // Shared with the writer threads, which can outlive this object
    struct Status
    {
        std::mutex mutex;
        std::string lastFolder;
        std::atomic<int> writing { 0 };
    };

    ID3D12Device* _device = nullptr;
    std::unique_ptr<FsrdPass_Dx12> _pass;
    ID3D12Resource* _target = nullptr; // RGBA16F, UNORDERED_ACCESS between copies
    UINT _targetWidth = 0, _targetHeight = 0;

    mutable std::mutex _requestMutex;
    Request _pending;
    bool _hasPending = false;
    int64_t _startNs = 0;

    // Running request (render thread)
    Request _active;
    bool _running = false;
    int64_t _nextNs = 0;
    int64_t _firstNs = 0;
    std::string _folder;
    std::vector<Job> _jobs;

    std::atomic<bool> _busy { false };
    std::atomic<int> _taken { 0 };
    std::atomic<int> _requested { 0 };
    std::shared_ptr<Status> _status = std::make_shared<Status>();

    bool Record(ID3D12GraphicsCommandList* cmdList, const FsrdPass_Dx12::Constants& constants,
                const std::vector<Source>& sources, Job& job);
    void WriteJob(Job& job);
    void UpdateBusy();

    static int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static int64_t SecondsToNs(float seconds) { return (int64_t) ((double) seconds * 1e9); }
    static float NsToSeconds(int64_t ns) { return ns > 0 ? (float) ((double) ns / 1e9) : 0.0f; }
};
