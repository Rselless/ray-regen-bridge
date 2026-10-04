#include "pch.h"
#include "FsrdPass_Dx12.h"

bool FsrdPass_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, const std::vector<ID3D12Resource*>& InSrvs,
                             const std::vector<ID3D12Resource*>& InUavs, const Constants& InConstants, UINT InWidth,
                             UINT InHeight)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InSrvs.size() != _srvSlots ||
        InUavs.size() != _uavSlots || InWidth == 0 || InHeight == 0)
        return false;

    for (auto uav : InUavs)
    {
        if (uav == nullptr)
            return false;
    }

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    _counter++;
    _counter = _counter % FSRDPASS_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    for (UINT i = 0; i < _srvSlots; i++)
    {
        if (InSrvs[i] != nullptr)
        {
            CreateShaderResourceView(_device, InSrvs[i], currentHeap.GetSrvCPU(i));
            continue;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc {};
        nullDesc.Format = DXGI_FORMAT_R32_FLOAT;
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(nullptr, &nullDesc, currentHeap.GetSrvCPU(i));
    }

    for (UINT i = 0; i < _uavSlots; i++)
        CreateUnorderedAccessView(_device, InUavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[_counter], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    InCmdList->Dispatch((InWidth + InNumThreadsX - 1) / InNumThreadsX, (InHeight + InNumThreadsY - 1) / InNumThreadsY,
                        1);

    return true;
}

FsrdPass_Dx12::FsrdPass_Dx12(std::string InName, ID3D12Device* InDevice, UINT InSrvCount, UINT InUavCount,
                             const void* InBytecode, size_t InBytecodeSize)
    : Shader_Dx12(InName, InDevice), _srvSlots(InSrvCount), _uavSlots(InUavCount)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, InSrvCount, InUavCount, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (int i = 0; i < FSRDPASS_NUM_OF_HEAPS; i++)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, InBytecode, InBytecodeSize, _name.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, FSRDPASS_NUM_OF_HEAPS);
}

FsrdPass_Dx12::~FsrdPass_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < FSRDPASS_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
        SAFE_RELEASE(_constantBuffers[i]);
    }
}
