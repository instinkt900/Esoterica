#ifdef __linux__
#include "RHI.h"

//-------------------------------------------------------------------------
// Vulkan RHI backend
//-------------------------------------------------------------------------
// Phase 1 delivers signature-complete stubs so that Esoterica.Base exports the full RHI
// surface and everything downstream links. RHI_Direct3D12.cpp belongs to this project, and
// 55 files across Engine and EngineTools call RHI:: functions, so without these nothing
// downstream links at all. Phase 5 replaces each body with a real implementation.
//
// Every stub halts. A stub that returned quietly would surface as a baffling failure much
// later; one that halts names the function Phase 5 still owes you.
//-------------------------------------------------------------------------

namespace EE::Render::RHI
{
    Context* CreateContext( ContextParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyContext( Context*&& context )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    uint64_t GetTotalAllocatedDeviceMemory( Context* pContext )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    void GetDetailedMemoryStatistics( Context* pContext, uint64_t& localUsageBytes, uint64_t& localAvailableBytes, uint64_t& nonLocalUsageBytes, uint64_t& nonLocalAvailableBytes )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void GetResourceAllocationStatistics( Context* pContext, TVector<ResourceAllocationStatistic>& outBufferStats, TVector<ResourceAllocationStatistic>& outTextureStats )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void BeginFrameCapture( Context* pContext )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void EndFrameCapture( Context* pContext )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    Queue* CreateQueue( Context* pContext, QueueParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyQueue( Context* pContext, Queue*&& pQueue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    uint64_t QueueGetCurrentSemaphore( Queue* pQueue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    uint64_t QueueGetCompletedSemaphore( Queue* pQueue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    void QueueHostWait( Queue* pQueue, uint64_t semaphore )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void QueueDeviceWait( Queue* pQueueThatWaits, Queue* pQueueToWaitFor, uint64_t semaphore )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    uint64_t QueueSubmit( Queue* pQueue, TArrayView<CommandBuffer*> commandBuffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    uint64_t QueuePresent( Queue* pQueue, Swapchain* pSwapchain, uint32_t imageIndex )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    void WaitQueueIdle( Queue* pQueue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    Swapchain* CreateSwapchain( Context* pContext, SwapchainParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroySwapchain( Context* pContext, Swapchain*&& pSwapchain )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    uint32_t AcquireNextImage( Context* pContext, Swapchain* pSwapchain )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    void SetVSync( Swapchain* pSwapchain, bool vsync )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    CommandPool* CreateCommandPool( Context* pContext, CommandPoolParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyCommandPool( Context* pContext, CommandPool*&& pCommandPool )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void ResetCommandPool( Context* pContext, CommandPool* pCommandPool )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    CommandBuffer* CreateCommandBuffer( Context* pContext, CommandBufferParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyCommandBuffer( Context* pContext, CommandBuffer*&& pCommandBuffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void BeginCommandBuffer( CommandBuffer* pCommandBuffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void EndCommandBuffer( CommandBuffer* pCommandBuffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetRenderTargets( CommandBuffer* pCommandBuffer, TArrayView<Texture* const> renderTargets, Texture* pDepthStencil, LoadAction* pLoadAction, TArrayView<uint32_t const> colorArraySlices, TArrayView<uint32_t const> colorMipSlices, uint32_t depthArraySlice, uint32_t depthMipSlice )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetShadingRate( CommandBuffer* pCommandBuffer, ShadingRate shadingRate, Texture* pShadingRateTexture, ShadingRateCombiner postRasterizerCombiner, ShadingRateCombiner finalCombiner )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetViewport( CommandBuffer* pCommandBuffer, float x, float y, float width, float height, float minDepth, float maxDepth )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetScissor( CommandBuffer* pCommandBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetStencilReference( CommandBuffer* pCommandBuffer, uint32_t value )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetPipeline( CommandBuffer* pCommandBuffer, Pipeline* pPipeline )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetRootConstants( CommandBuffer* pCommandBuffer, uint32_t constantIndex, void const* pConstantData, size_t constantSize )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetRootParameter( CommandBuffer* pCommandBuffer, uint32_t parameterIndex, Buffer* pBuffer, size_t bufferOffset )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetIndexBuffer( CommandBuffer* pCommandBuffer, Buffer const* pIndexBuffer, IndexType indexType, uint64_t offset )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDraw( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t firstVertex )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDrawInstanced( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t numInstances, uint32_t firstVertex, uint32_t firstInstance )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDrawIndexed( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t firstIndex )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDrawIndexedInstanced( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t numInstances, uint32_t firstIndex, uint32_t firstInstance )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDispatchCompute( CommandBuffer* pCommandBuffer, uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDispatchMesh( CommandBuffer* pCommandBuffer, uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdDispatchRays( CommandBuffer* pCommandBuffer, RaytracingShaderTable* pShaderTable, AccelerationStructure* pAccelerationStructure, uint32_t width, uint32_t height )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdExecuteIndirect( CommandBuffer* pCommandBuffer, CommandSignature const* pCommandSignature, uint32_t maxNumCommands, Buffer const* pIndirectBuffer, uint64_t indirectBufferOffset, Buffer const* pCounterBuffer, uint64_t counterBufferOffset )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdClearTexture( CommandBuffer* pCommandBuffer, Texture const* pTexture, uint32_t clearValue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdClearBuffer( CommandBuffer* pCommandBuffer, Buffer const* pBuffer, uint32_t clearValue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdBuildAccelerationStructure( CommandBuffer* pCommandBuffer, TArrayView<AccelerationStructure* const> accelerationStructures, TArrayView<uint32_t const> bottomLevelAccelerationStructureIndices )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdBarrier( CommandBuffer* pCommandBuffer, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdBarrier( CommandBuffer* pCommandBuffer, Buffer* pBuffer, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdBarrier( CommandBuffer* pCommandBuffer, Texture* pTexture, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess, TextureState sourceState, TextureState destinationState, TextureBarrierRegion region, TBitFlags<TextureBarrierFlags> flags )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdResetQueryPool( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, uint32_t startQuery, uint32_t numQueries )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdBeginQuery( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, uint32_t queryIndex )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdEndQuery( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, uint32_t queryIndex )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdResolveQuery( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, Buffer const* pReadbackBuffer, uint32_t startQuery, uint32_t numQueries )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdCopyBuffer( CommandBuffer* pCommandBuffer, Buffer const* pDstBuffer, uint64_t dstOffset, Buffer const* pSrcBuffer, uint64_t srcOffset, uint64_t srcSize )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdCopyTexture( CommandBuffer* pCommandBuffer, Texture const* pDstTexture, TextureCopyRegion const& dstRegion, Buffer const* pSrcBuffer, uint64_t srcOffset )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdCopyTexture( CommandBuffer* pCommandBuffer, Buffer const* pDstBuffer, uint64_t dstOffset, Texture const* pSrcTexture, TextureCopyRegion const& srcRegion )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdBeginDebugMarker( CommandBuffer* pCommandBuffer, char const* pName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdEndDebugMarker( CommandBuffer* pCommandBuffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    uint32_t CmdWriteDebugMarker( CommandBuffer* pCommandBuffer, TBitFlags<MarkerTypeFlags> const& markerType, uint32_t markerValue, Buffer* pBuffer, size_t offset, bool useAutoFlags )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    CommandSignature* CreateCommandSignature( Context* pContext, CommandSignatureParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyCommandSignature( Context* pContext, CommandSignature*&& pCommandSignature )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    AccelerationStructure* CreateAccelerationStructure( Context* pContext, AccelerationStructureTopLevelCreateParameters const& topLevelParameters, AccelerationStructureBottomLevelCreateParameters const& bottomLevelParameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    AccelerationStructureHandle GetAccelerationStructureHandle( AccelerationStructure const* pAccelerationStructure )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return AccelerationStructureHandle();
    }

    Buffer* CreateBuffer( Context* pContext, BufferParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyBuffer( Context* pContext, Buffer*&& buffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void MapBuffer( Context* pContext, Buffer* pBuffer, ReadRange range )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void UnmapBuffer( Context* pContext, Buffer* pBuffer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    BufferHandle GetBufferHandle( Buffer const* pBuffer, DescriptorTypeFlags descriptorType )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return BufferHandle();
    }

    BufferSubAllocation BufferSubAllocate( Buffer* pBuffer, uint64_t size, uint64_t alignment )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return BufferSubAllocation();
    }

    void BufferSubDeallocate( Buffer* pBuffer, BufferSubAllocation&& subAllocation )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    Texture* CreateTexture( Context* pContext, TextureParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyTexture( Context* pContext, Texture*&& pTexture )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    uint32_t GetTextureCopyRowStride( Texture const* pTexture, uint32_t mipLevel, uint32_t arrayLayer )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    TextureHandle GetTextureHandle( Texture const* pTexture, DescriptorTypeFlags descriptorType, uint32_t rwTextureMipLevel )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return TextureHandle();
    }

    Sampler* CreateSampler( Context* pContext, SamplerParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroySampler( Context* pContext, Sampler*&& pSampler )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    SamplerStateHandle GetSamplerStateHandle( Sampler const* pSampler )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return SamplerStateHandle();
    }

    Shader* CreateShader( Context* pContext, TInlineVector<ShaderByteCode, 2> const& shaderParameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyShader( Context* pContext, Shader*&& pShader )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    RootSignature* CreateRootSignature( Context* pContext, RootSignatureParameters const& parameter )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyRootSignature( Context* pContext, RootSignature*&& pRootSignature )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    PipelineCache* CreatePipelineCache( Context* pContext, PipelineCacheParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyPipelineCache( Context* pContext, PipelineCache*&& pPipelineCache )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    TArrayView<uint8_t> GetPipelineCacheData( Context* pContext, PipelineCache* pPipelineCache )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return TArrayView<uint8_t>();
    }

    Pipeline* CreatePipeline( Context* pContext, GraphicsPipelineParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    Pipeline* CreatePipeline( Context* pContext, MeshPipelineParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    Pipeline* CreatePipeline( Context* pContext, ComputePipelineParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    Pipeline* CreatePipeline( Context* pContext, RaytracingPipelineParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyPipeline( Context* pContext, Pipeline*&& pPipeline )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    QueryPool* CreateQueryPool( Context* pContext, QueryPoolParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    void DestroyQueryPool( Context* pContext, QueryPool*&& pQueryPool )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    double GetQueryTimestampFrequency( Queue* pQueue )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return 0.0;
    }

    void SetDebugName( Context* pContext, Queue* pQueue, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, QueryPool* pQueryPool, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, Buffer* pBuffer, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, Texture* pTexture, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, RootSignature* pRootSignature, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, CommandSignature* pCommandSignature, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, Pipeline* pPipeline, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, CommandPool* pCommandPool, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void SetDebugName( Context* pContext, CommandBuffer* pCommandBuffer, StringView debugName )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void ReportDeviceMemoryLeaks()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

}
#endif
