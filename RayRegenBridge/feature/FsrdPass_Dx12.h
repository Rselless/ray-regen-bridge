#pragma once
// One compute pass of FSRDFeatureDx12 (fsrd_prepass / fsrd_compose / fsrd_particles / fsrd_probe). Same pattern as Bias_Dx12, but with a
// variable number of SRVs/UAVs and one constant buffer per descriptor heap, since constants are written at record
// time and the GPU can still be reading the previous frame's.
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define FSRDPASS_NUM_OF_HEAPS 3

class FsrdPass_Dx12 : public Shader_Dx12
{
  public:
    // Must match cbuffer Params in fsrd_common.hlsli
    struct alignas(256) Constants
    {
        float ProjZZ;
        float ProjWZ;
        float DiffuseHitDistance;
        uint32_t Flags;
        uint32_t RenderWidth;
        uint32_t RenderHeight;
        float SkyDepth;
        uint32_t SplitMode;

        float MvScale[2];
        float MvTexelScale[2];
        float InvProj[2];
        uint32_t HasPrevView;
        float MaxStaticMvError;

        float PrevViewX[4];
        float PrevViewY[4];
        float PrevViewZ[4];

        float ReflectionReactive;
        float GameMaskBias;
        float SpecHitScale;
        float RoughnessOverride;

        float CameraForward[3];
        uint32_t SentinelMode;

        uint32_t CounterSlot;
        float PreExposure; // DLSS.Pre.Exposure the game's color is multiplied by, 1 when not sent
        float MirrorRoughness;
        float Demodulation;

        float DemodFadeDistance;
        float Pad4[3];
    };

  private:
    // One heap and constant buffer per dispatch that can be in flight: FSRDPASS_NUM_OF_HEAPS for once per frame
    // passes, more for passes dispatched several times in one command list (frame dump)
    std::vector<FrameDescriptorHeap> _frameHeaps;
    std::vector<ID3D12Resource*> _constantBuffers;

    UINT _srvSlots = 0;
    UINT _uavSlots = 0;

    UINT InNumThreadsX = 16;
    UINT InNumThreadsY = 16;

  public:
    // Null SRVs get a null descriptor (reads return 0), UAVs must all be valid
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, const std::vector<ID3D12Resource*>& InSrvs,
                  const std::vector<ID3D12Resource*>& InUavs, const Constants& InConstants, UINT InWidth,
                  UINT InHeight);

    bool CanRender() const { return _init; }

    FsrdPass_Dx12(std::string InName, ID3D12Device* InDevice, UINT InSrvCount, UINT InUavCount,
                  const void* InBytecode, size_t InBytecodeSize, UINT InHeapCount = FSRDPASS_NUM_OF_HEAPS);

    ~FsrdPass_Dx12();
};
