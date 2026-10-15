#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include <cassert>
#include <stdexcept>
#include "GpuMemoryBlock.h"
#include "TransientHeap.h"
#include "UploadHeap.h"
#include "BindlessDescriptorHeap.h"

using Microsoft::WRL::ComPtr;

//先想想 创建resource要干什么 我们直接做显存复用吧 placed memory + heap
//不能给在resource类里维护heap

//底层物理资源：ComPtr<ID3D12Resource>。   
// 状态机追踪（State Tracking）：记录当前处于什么状态（m_currentState），使得后续在录制屏障时能够准确获知 stateBefore。Bindless 
// 槽位持有：在创建完成后，直接向已经写好的 GPUDescriptorAllocator 索取全局槽位并写入 SRV，内部持久化保存一个 uint32_t m_bindlessIndex。   
// 元数据：保存宽高、格式、Mip 级数。

//独立维护一个Texture 方便我们在语义上去调用
class Texture2D {
public:
    // 基于底层显存切片构建 Placed Resource
    static Texture2D CreatePlaced(
        ID3D12Device* device,
        const D3D12_RESOURCE_DESC& desc,
        const GpuMemoryBlock& memoryBlock,
        D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* clearValue = nullptr);
    
    static Texture2D LoadFromFilePlaced(
        const std::string& path,
        TransientHeap& defaultHeap,
        UploadHeap& uploadHeap,
        GPUDescriptorAllocator& bindlessAllocator    
    );
    uint32_t GetBindlessIndex() const noexcept { return m_bindlessIndex; }

private:
    ComPtr<ID3D12Resource> m_resource;
    GpuMemoryBlock         m_memoryBlock; // 持有物理显存切片信息
    D3D12_RESOURCE_STATES  m_currentState;
    uint32_t               m_bindlessIndex = UINT32_MAX;

};