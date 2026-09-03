#pragma once

#include "Engine/_Module/API.h"
#include "Engine/Render/Device/DeviceResizeBuffer.h"
#include "Engine/Render/Shaders/EngineShader.h"
#include "Base/Render/RHI.h"

namespace EE::Render
{
    class RenderSystem;

    // GPU radix sort using AMD FidelityFX Parallel Sort.
    // Sorts unsigned integer keys over the numSortBits lowest bits ( 1-32 ), with optional 32-bit payload.
    //-------------------------------------------------------------------------

    class EE_ENGINE_API DeviceRadixSort final
    {
    public:

        void Initialize( RHI::Context* pContextRHI, RenderSystem* pRenderSystem );
        void Shutdown( RHI::Context* pContextRHI );

        void UpdateDeviceResources( uint32_t numKeys );

        void DispatchSorting
        (
            RHI::CommandBuffer* pCommandBuffer,
            RHI::Buffer* pSrcKeys, RHI::Buffer* pDstKeys, RHI::Buffer* pNumKeysBuffer, uint32_t maxNumKeys, uint32_t numSortBits
        );

        void DispatchSorting
        (
            RHI::CommandBuffer* pCommandBuffer,
            RHI::Buffer* pSrcKeys, RHI::Buffer* pDstKeys,
            RHI::Buffer* pSrcPayload, RHI::Buffer* pDstPayload,
            RHI::Buffer* pNumKeysBuffer, uint32_t maxNumKeys, uint32_t numSortBits
        );

    private:

        RHI::Context*                                   m_pContextRHI = nullptr;
        RenderSystem*                                   m_pRenderSystem = nullptr;

        ComputeShader const*                            m_pSetupIndirectShader = nullptr;
        ComputeShader const*                            m_pCountShader = nullptr;
        ComputeShader const*                            m_pReduceShader = nullptr;
        ComputeShader const*                            m_pScanShader = nullptr;
        ComputeShader const*                            m_pScanAddShader = nullptr;
        ComputeShader const*                            m_pScatterShader = nullptr;
        ComputeShader const*                            m_pScatterPayloadShader = nullptr;

        DeviceResizeBuffer                              m_scratchBuffer = {};
        DeviceResizeBuffer                              m_reducedScratchBuffer = {};

        RHI::Buffer*                                    m_pConstantBuffer = nullptr;
        RHI::Buffer*                                    m_pCountArgumentsBuffer = nullptr;
        RHI::Buffer*                                    m_pReduceArgumentsBuffer = nullptr;
        RHI::Buffer*                                    m_pScanAddArgumentsBuffer = nullptr;
        RHI::Buffer*                                    m_pScatterArgumentsBuffer = nullptr;
        RHI::Buffer*                                    m_pScatterPayloadArgumentsBuffer = nullptr;

        uint32_t                                        m_maxNumKeys = 0;
        uint32_t                                        m_maxThreadGroups = 800;
    };
}
