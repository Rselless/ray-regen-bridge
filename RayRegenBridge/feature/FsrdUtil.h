#pragma once
// Small D3D12 helpers shared by FSRDFeatureDx12 and FsrdProbe (include after pch.h)
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <initializer_list>
#include <iterator>

namespace Fsrd
{
struct BarrierDesc
{
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES before;
    D3D12_RESOURCE_STATES after;
};

// One ResourceBarrier call for a group of transitions, null resources are skipped
inline void Barriers(ID3D12GraphicsCommandList* cmdList, std::initializer_list<BarrierDesc> transitions)
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

inline bool CreateScratchTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
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
} // namespace Fsrd
