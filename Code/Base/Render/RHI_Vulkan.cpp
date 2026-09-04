#ifdef __linux__
#include "Base/Esoterica.h"

#include "RHI.h"

#include "Base/Math/Math.h"
#include "Base/Types/HashMap.h"
#include "Base/Render/HandleAllocator.h"
#include "Base/Encoding/Embed.h"
#include "Base/Platform/PlatformUtils_Linux.h"

#include "EASTL/algorithm.h"

#include <vulkan/vulkan.h>
#include <dlfcn.h>

// SPIRV-Reflect replaces ID3D12ShaderReflection.
#include "spirv_reflect.h"

#include "renderdoc_app.h"

// VMA's implementation belongs in exactly one translation unit, and this is the only Vulkan one.
// The pragma silences the header's nullability warnings, which no first-party code raises.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

//-------------------------------------------------------------------------
// Vulkan RHI backend
//-------------------------------------------------------------------------
// The Linux counterpart of RHI_Direct3D12.cpp, which is the reference for behaviour.
//-------------------------------------------------------------------------

namespace eastl
{
    template <>
    struct hash<EE::TBitFlags<EE::Render::RHI::DescriptorTypeFlags>>
    {
        size_t operator()( EE::TBitFlags<EE::Render::RHI::DescriptorTypeFlags> const& s ) const { return hash<uint32_t>{}( s.Get() ); }
    };
}

//-------------------------------------------------------------------------

namespace EE::Memory::Allocators
{
    static MemoryAllocator g_Vulkan( "Vulkan" );

    // One backend is compiled per platform, so this has to be defined here as well as in the D3D12 backend.
    MemoryAllocator g_RHI( "RHI" );
}

//-------------------------------------------------------------------------

namespace EE::Render::RHI
{
    //-------------------------------------------------------------------------
    // Device requirements
    //-------------------------------------------------------------------------
    // Fixed by the bindless binding model. The shaders are compiled for it and there is no
    // fallback path, so a device that cannot meet these is refused rather than worked around.

    static char const* const g_requiredDeviceExtensions[] =
    {
        // Set 1 binding 0 aliases six descriptor types, which is what DXC's emulated heap emits.
        VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
        // CmdSetRootConstants and CmdSetRootParameter are push descriptor writes into set 0.
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        // Enabled up front because the device is created once, long before the swapchain.
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // The D3D12 backend's counts, kept identical so a handle means the same thing on both backends.
    // Both fit under UINT16_MAX, which keeps InvalidResourceHandle outside either heap.
    static constexpr uint32_t g_resourceHeapSize = 64 * 1023;
    static constexpr uint32_t g_samplerHeapSize = 2048;

    // The only push constant range the backend uses. No Vulkan indirect draw rebinds a descriptor
    // per command, so the shader reads its own command out of the argument buffer instead.
    // Mirrored in RHI.esh as EE_IndirectRootPushConstants; the two must agree.
    struct IndirectRootPushConstants
    {
        uint64_t                                            m_argumentBufferAddress = 0;
        uint32_t                                            m_stride = 0;
        uint32_t                                            m_commandIndexBase = 0;
        uint32_t                                            m_rootConstantOffset = 0;
        uint32_t                                            m_rootCbvOffset = 0;
    };

    static constexpr uint32_t g_rootParameterSet = 0;
    static constexpr uint32_t g_heapSet = 1;
    static constexpr uint32_t g_resourceHeapBinding = 0;
    static constexpr uint32_t g_samplerHeapBinding = 1;

    // DXC's emulated heap emits one OpTypeRuntimeArray per HLSL resource type, all on this one binding.
    static constexpr VkDescriptorType g_resourceHeapMutableTypes[] =
    {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,           // Texture2D, Texture2DArray, Texture3D, TextureCube
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           // RWTexture2D
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,    // Buffer<T>
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,    // RWBuffer<T>
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          // StructuredBuffer<T>, RWStructuredBuffer<T>, ByteAddressBuffer
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          // no shader today, but GetBufferHandle accepts ConstantBuffer
    };

    // Vulkan 1.3 is the baseline, so only the bits that stay optional within core need asking for.
    struct RequiredFeatures
    {
        VkPhysicalDeviceVulkan13Features                    m_vulkan13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features                    m_vulkan12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan11Features                    m_vulkan11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT    m_mutableDescriptorType = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
        VkPhysicalDeviceFeatures2                           m_features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

        // Call after any copy: copying leaves every pNext pointing at the original.
        void Chain()
        {
            m_features2.pNext = &m_vulkan11;
            m_vulkan11.pNext = &m_vulkan12;
            m_vulkan12.pNext = &m_vulkan13;
            m_vulkan13.pNext = &m_mutableDescriptorType;
            m_mutableDescriptorType.pNext = nullptr;
        }
    };

    // Emits the vtable and typeinfo that VulkanContext needs. One backend is compiled per
    // platform, so this has to be defined here as well as in the D3D12 backend.
    EE_BASE_API GenericResource::~GenericResource() = default;

    struct VulkanContext;
    static void SetVulkanObjectName( VulkanContext* pVulkanContext, VkObjectType objectType, uint64_t objectHandle, StringView debugName );

    struct VulkanCommandBuffer;
    static void FlushRendering( VulkanCommandBuffer* pVulkanCommandBuffer );
    static void SuspendRendering( VulkanCommandBuffer* pVulkanCommandBuffer );
    static void FlushBarriers( VulkanCommandBuffer* pVulkanCommandBuffer );

    static VkFormat VulkanFormat( DataFormat format );
    static VkCompareOp VulkanCompareOp( CompareMode mode );

    //-------------------------------------------------------------------------

    struct ResourceAllocStats
    {
        uint64_t                                            m_numAllocations = 0;
        uint64_t                                            m_numBytes = 0;
    };

    // Vulkan has no equivalent of DXGI's live-object report. ReportDeviceMemoryLeaks runs after
    // DestroyContext has freed the VMA allocator, so DestroyContext records what it saw here.
    static uint64_t g_leakedDeviceAllocations = 0;
    static uint64_t g_leakedDeviceAllocationBytes = 0;

    // VulkanPipelineStage is handed flags and no Context, but the task and mesh stage bits are
    // only legal once VK_EXT_mesh_shader is enabled. Set by CreateContext, cleared by DestroyContext.
    static bool g_meshShaderEnabled = false;

    // Said once, not once per dropped draw. See CmdSetPipeline.
    static bool g_warnedAboutDroppedMeshDraws = false;

    // Linux surfaces offer only the BGRA spelling of the 8-bit formats, while the engine hardcodes
    // RGBA for its present-path render targets. Dynamic rendering demands the pipeline's attachment
    // format match the image exactly, so the two spellings have to be made to agree.
    //  - A relabel, not a swizzle. A shader's red output lands in the format's red component, so
    //    the picture is identical.
    //  - Render targets only. A sampled texture really is in the order the engine says, and
    //    relabelling one would swap its channels on screen.
    //  - Unconditional, because pipelines are built before there is a window to make a surface
    //    from. CreateSwapchain asks for the same spelling, so the two cannot disagree.
    static DataFormat SubstituteSwapchainColorFormat( DataFormat format )
    {
        switch ( format )
        {
            case DataFormat::RGBA8_sRGB:  return DataFormat::BGRA8_sRGB;
            case DataFormat::RGBA8_UNorm: return DataFormat::BGRA8_UNorm;
            default: return format;
        }
    }

    // Declaring a dynamic state from a disabled extension is a validation error, and
    // CreateGraphicsOrMeshPipeline has no Context to ask. Set alongside g_meshShaderEnabled.
    static bool g_fragmentShadingRateEnabled = false;

    // Naming a usage bit from a disabled extension is a validation error, and CreateBuffer has no
    // Context to ask. Raytracing builds read and write ordinary buffers, so every buffer needs
    // the acceleration structure usage bits once the extension is on.
    static bool g_raytracingEnabled = false;

    struct VulkanContext : Context
    {
        VkInstance                                                      m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT                                        m_debugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice                                                m_physicalDevice = VK_NULL_HANDLE;
        VkDevice                                                        m_device = VK_NULL_HANDLE;
        VmaAllocator                                                    m_resourceAllocator = VK_NULL_HANDLE;

        // Stored without its pNext chain, which points at FillDeviceCapabilities' stack.
        VkPhysicalDeviceProperties                                      m_physicalDeviceProperties = {};
        VkPhysicalDeviceMemoryProperties                                m_memoryProperties = {};

        // VMA holds this by pointer for the allocator's whole life, so it cannot be a local.
        VkAllocationCallbacks                                           m_hostAllocationCallbacks = {};

        // One layout for every pipeline in the engine. CmdSetPipeline rebinds it, because a
        // pipeline with a different set 0 layout disturbs it.
        VkDescriptorSetLayout                                           m_heapSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool                                                m_heapDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet                                                 m_heapDescriptorSet = VK_NULL_HANDLE;

        // The same allocator the D3D12 backend uses, so a handle is allocated the same way on both sides.
        HandleAllocator<GenericResourceHandle>                          m_resourceHeapAllocator;
        HandleAllocator<GenericResourceHandle>                          m_samplerHeapAllocator;

        // Chosen here because vkCreateDevice takes the queue create infos. An index of ~0U means
        // the device exposes no such family, and the graphics family stands in, which is legal.
        uint32_t                                                        m_graphicsQueueFamily = ~0U;
        uint32_t                                                        m_computeQueueFamily = ~0U;
        uint32_t                                                        m_transferQueueFamily = ~0U;

        // A family exposing one queue gives every RHI queue the same VkQueue, which is correct
        // but serialises them.
        struct QueueFamilyAllocation
        {
            uint32_t                                        m_familyIndex = ~0U;
            uint32_t                                        m_numQueues = 0;
            uint32_t                                        m_nextQueueIndex = 0;
        };

        TInlineVector<QueueFamilyAllocation, 3>                         m_queueFamilyAllocations;

        // The distinct families in use, which is what a CONCURRENT resource has to list. See SetSharingMode.
        TInlineVector<uint32_t, 3>                                      m_sharingQueueFamilies;

        // True when every device-local memory type is also host-visible. D3D12 reads the same
        // answer from D3D12MA's IsUMA().
        bool                                                            m_isUnifiedMemory = false;

        template<typename T>
        T* CreateObject()
        {
            void* pObjectMemory = Memory::Allocators::g_RHI.Alloc( sizeof( T ), alignof( T ) );
            return new( pObjectMemory ) T();
        }

        template<typename T>
        void DestroyObject( T*&& pObject )
        {
            if ( pObject != nullptr )
            {
                pObject->~T();
                Memory::Allocators::g_RHI.Free( (void*&) pObject );
            }
        }

        // Extension entry points are not exported by the loader. Null when the extension is absent.
        PFN_vkSetDebugUtilsObjectNameEXT                                m_vkSetDebugUtilsObjectName = nullptr;
        // Core in Vulkan 1.4, but the baseline here is 1.3, so it is the KHR entry point.
        PFN_vkCmdPushDescriptorSetKHR                                   m_vkCmdPushDescriptorSet = nullptr;
        PFN_vkCmdBeginDebugUtilsLabelEXT                                m_vkCmdBeginDebugUtilsLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT                                  m_vkCmdEndDebugUtilsLabel = nullptr;

        // Optional, unlike everything in the required feature list. See CreateQueryPool.
        bool                                                            m_pipelineStatisticsQuery = false;

        // The engine has no capability flag for mesh shaders and no fallback path, so D3D12
        // assumes the hardware has them. Requiring the extension would refuse a device the rest
        // of the engine renders on fine, so it is asked for when present and asserted at use.
        bool                                                            m_meshShader = false;
        PFN_vkCmdDrawMeshTasksEXT                                       m_vkCmdDrawMeshTasks = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectEXT                               m_vkCmdDrawMeshTasksIndirect = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectCountEXT                          m_vkCmdDrawMeshTasksIndirectCount = nullptr;

        // VK_KHR_fragment_shading_rate, optional for the same reason. See CmdSetShadingRate.
        bool                                                            m_fragmentShadingRate = false;
        PFN_vkCmdSetFragmentShadingRateKHR                              m_vkCmdSetFragmentShadingRate = nullptr;

        // VK_KHR_acceleration_structure, VK_KHR_ray_tracing_pipeline and the deferred host
        // operations the first depends on. All three together or none, and optional like the rest.
        bool                                                            m_raytracing = false;
        PFN_vkCreateAccelerationStructureKHR                            m_vkCreateAccelerationStructure = nullptr;
        PFN_vkDestroyAccelerationStructureKHR                           m_vkDestroyAccelerationStructure = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR                     m_vkGetAccelerationStructureBuildSizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR                  m_vkGetAccelerationStructureDeviceAddress = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR                         m_vkCmdBuildAccelerationStructures = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR                              m_vkCreateRayTracingPipelines = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR                        m_vkGetRayTracingShaderGroupHandles = nullptr;
        PFN_vkCmdTraceRaysKHR                                           m_vkCmdTraceRays = nullptr;
        PFN_vkCmdTraceRaysIndirect2KHR                                  m_vkCmdTraceRaysIndirect2 = nullptr;

        void*                                                           m_pRenderDocLibrary = nullptr;
        RENDERDOC_API_1_0_0*                                            m_pRenderDocAPI = nullptr;

        // GetResourceAllocationStatistics reads these, and DestroyContext asserts they are back to zero.
        THashMap<TBitFlags<DescriptorTypeFlags>, ResourceAllocStats>    m_bufferStats{ Memory::Allocators::g_RHI };
        THashMap<TBitFlags<DescriptorTypeFlags>, ResourceAllocStats>    m_textureStats{ Memory::Allocators::g_RHI };
    };

    //-------------------------------------------------------------------------
    // Helpers
    //-------------------------------------------------------------------------

    static TInlineString<MaxVendorNameLength> FormatVendorID( uint32_t id )
    {
        TInlineString<MaxVendorNameLength> buffer;
        buffer.sprintf( "%#x", id );
        return buffer;
    }

    static bool HasExtension( TVector<VkExtensionProperties> const& extensions, char const* pName )
    {
        for ( VkExtensionProperties const& extension : extensions )
        {
            if ( strcmp( extension.extensionName, pName ) == 0 )
            {
                return true;
            }
        }

        return false;
    }

    static TVector<VkExtensionProperties> EnumerateDeviceExtensions( VkPhysicalDevice physicalDevice )
    {
        uint32_t numExtensions = 0;
        vkEnumerateDeviceExtensionProperties( physicalDevice, nullptr, &numExtensions, nullptr );

        TVector<VkExtensionProperties> extensions( numExtensions );
        vkEnumerateDeviceExtensionProperties( physicalDevice, nullptr, &numExtensions, extensions.data() );

        return extensions;
    }

    // Returns the missing feature by name, so the log can say more than "no suitable device".
    static char const* GetDeviceRejectionReason( VkPhysicalDevice physicalDevice )
    {
        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties( physicalDevice, &properties );

        if ( properties.apiVersion < VK_API_VERSION_1_3 )
        {
            return "Vulkan 1.3 is the baseline";
        }

        TVector<VkExtensionProperties> const extensions = EnumerateDeviceExtensions( physicalDevice );

        for ( char const* pRequiredExtension : g_requiredDeviceExtensions )
        {
            if ( !HasExtension( extensions, pRequiredExtension ) )
            {
                return pRequiredExtension;
            }
        }

        RequiredFeatures available = {};
        available.Chain();
        vkGetPhysicalDeviceFeatures2( physicalDevice, &available.m_features2 );

        if ( !available.m_vulkan13.dynamicRendering )               { return "dynamicRendering"; }
        if ( !available.m_vulkan13.synchronization2 )               { return "synchronization2"; }
        if ( !available.m_vulkan12.timelineSemaphore )              { return "timelineSemaphore"; }
        if ( !available.m_vulkan12.bufferDeviceAddress )            { return "bufferDeviceAddress"; }
        if ( !available.m_vulkan12.drawIndirectCount )              { return "drawIndirectCount"; }
        // The Reflector passes -fvk-use-dx-layout, so without this every cbuffer reads at the wrong offsets.
        if ( !available.m_vulkan12.scalarBlockLayout )              { return "scalarBlockLayout"; }
        if ( !available.m_vulkan12.descriptorIndexing )             { return "descriptorIndexing"; }
        if ( !available.m_vulkan12.runtimeDescriptorArray )         { return "runtimeDescriptorArray"; }
        if ( !available.m_vulkan12.descriptorBindingPartiallyBound ){ return "descriptorBindingPartiallyBound"; }
        if ( !available.m_vulkan12.descriptorBindingVariableDescriptorCount ) { return "descriptorBindingVariableDescriptorCount"; }
        // The engine writes handles into the heap while command buffers that reference it are recording.
        if ( !available.m_vulkan12.descriptorBindingSampledImageUpdateAfterBind )  { return "descriptorBindingSampledImageUpdateAfterBind"; }
        if ( !available.m_vulkan12.descriptorBindingStorageImageUpdateAfterBind )  { return "descriptorBindingStorageImageUpdateAfterBind"; }
        if ( !available.m_vulkan12.descriptorBindingStorageBufferUpdateAfterBind ) { return "descriptorBindingStorageBufferUpdateAfterBind"; }
        if ( !available.m_vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind ) { return "descriptorBindingUniformTexelBufferUpdateAfterBind"; }
        // Every type in the heap's mutable list needs update-after-bind, and the list holds a
        // storage texel buffer for RWBuffer<T>.
        if ( !available.m_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind ) { return "descriptorBindingStorageTexelBufferUpdateAfterBind"; }
        // FilterMode::Min and Max. RenderSystem::Initialize creates a Max sampler on the first frame.
        if ( !available.m_vulkan12.samplerFilterMinmax )            { return "samplerFilterMinmax"; }
        // NonUniformResourceIndex in the shaders indexes the heap with a divergent index.
        if ( !available.m_vulkan12.shaderSampledImageArrayNonUniformIndexing )  { return "shaderSampledImageArrayNonUniformIndexing"; }
        if ( !available.m_vulkan12.shaderStorageImageArrayNonUniformIndexing )  { return "shaderStorageImageArrayNonUniformIndexing"; }
        if ( !available.m_vulkan12.shaderStorageBufferArrayNonUniformIndexing ) { return "shaderStorageBufferArrayNonUniformIndexing"; }
        if ( !available.m_vulkan12.shaderUniformTexelBufferArrayNonUniformIndexing ) { return "shaderUniformTexelBufferArrayNonUniformIndexing"; }
        // The same RWBuffer<T> storage texel buffer. It is in the heap, so NonUniformResourceIndex
        // reaches it too.
        if ( !available.m_vulkan12.shaderStorageTexelBufferArrayNonUniformIndexing ) { return "shaderStorageTexelBufferArrayNonUniformIndexing"; }
        if ( !available.m_mutableDescriptorType.mutableDescriptorType ) { return "mutableDescriptorType"; }

        return nullptr;
    }

    // Higher is better. Mirrors DXGI_GPU_PREFERENCE rather than inventing a different notion of "best".
    static uint32_t ScoreDevice( VkPhysicalDevice physicalDevice, DeviceSelectionPreference preference )
    {
        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties( physicalDevice, &properties );

        switch ( properties.deviceType )
        {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return ( preference == DeviceSelectionPreference::PreferPowerEfficiency ) ? 2u : 4u;

            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return ( preference == DeviceSelectionPreference::PreferPowerEfficiency ) ? 4u : 2u;

            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 1u;

            default:
            // CPU and Other. The D3D12 backend skips software adapters, so they are a last resort here too.
            return 0u;
        }
    }

    static void SelectQueueFamilies( VulkanContext* pVulkanContext )
    {
        uint32_t numQueueFamilies = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, nullptr );

        TVector<VkQueueFamilyProperties> queueFamilies( numQueueFamilies );
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, queueFamilies.data() );

        for ( uint32_t familyIndex = 0; familyIndex < numQueueFamilies; ++familyIndex )
        {
            VkQueueFlags const flags = queueFamilies[familyIndex].queueFlags;

            if ( pVulkanContext->m_graphicsQueueFamily == ~0U && ( flags & VK_QUEUE_GRAPHICS_BIT ) )
            {
                pVulkanContext->m_graphicsQueueFamily = familyIndex;
                continue;
            }

            // Async compute and async transfer are only worth a separate family when that
            // family is not the graphics one, which is what "dedicated" means here.
            if ( pVulkanContext->m_computeQueueFamily == ~0U && ( flags & VK_QUEUE_COMPUTE_BIT ) && !( flags & VK_QUEUE_GRAPHICS_BIT ) )
            {
                pVulkanContext->m_computeQueueFamily = familyIndex;
                continue;
            }

            if ( pVulkanContext->m_transferQueueFamily == ~0U && ( flags & VK_QUEUE_TRANSFER_BIT ) && !( flags & ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) )
            {
                pVulkanContext->m_transferQueueFamily = familyIndex;
                continue;
            }
        }

        EE_ASSERT( pVulkanContext->m_graphicsQueueFamily != ~0U );

        // Every graphics-capable family also supports compute and transfer, so standing in is
        // correct rather than a fallback that loses work.
        if ( pVulkanContext->m_computeQueueFamily == ~0U )
        {
            pVulkanContext->m_computeQueueFamily = pVulkanContext->m_graphicsQueueFamily;
        }

        if ( pVulkanContext->m_transferQueueFamily == ~0U )
        {
            pVulkanContext->m_transferQueueFamily = pVulkanContext->m_graphicsQueueFamily;
        }

        // The distinct families, for SetSharingMode.
        uint32_t const families[] = { pVulkanContext->m_graphicsQueueFamily, pVulkanContext->m_computeQueueFamily, pVulkanContext->m_transferQueueFamily };
        for ( uint32_t familyIndex : families )
        {
            if ( eastl::find( pVulkanContext->m_sharingQueueFamilies.begin(), pVulkanContext->m_sharingQueueFamilies.end(), familyIndex ) == pVulkanContext->m_sharingQueueFamilies.end() )
            {
                pVulkanContext->m_sharingQueueFamilies.emplace_back( familyIndex );
            }
        }
    }

    static void FillDeviceCapabilities( VulkanContext* pVulkanContext )
    {
        VkPhysicalDeviceSubgroupProperties subgroupProperties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 properties2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &subgroupProperties;
        vkGetPhysicalDeviceProperties2( pVulkanContext->m_physicalDevice, &properties2 );

        pVulkanContext->m_physicalDeviceProperties = properties2.properties;
        vkGetPhysicalDeviceMemoryProperties( pVulkanContext->m_physicalDevice, &pVulkanContext->m_memoryProperties );

        VkPhysicalDeviceLimits const& limits = properties2.properties.limits;
        DeviceCapabilities& capabilities = pVulkanContext->m_deviceCapabilities;

        // The Vulkan equivalent of DXGI's DedicatedVideoMemory is the size of the device-local
        // heaps. On an integrated GPU that is shared system memory, which is what DXGI reports there too.
        for ( uint32_t heapIndex = 0; heapIndex < pVulkanContext->m_memoryProperties.memoryHeapCount; ++heapIndex )
        {
            VkMemoryHeap const& heap = pVulkanContext->m_memoryProperties.memoryHeaps[heapIndex];
            if ( heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
            {
                capabilities.m_dedicatedVideoMemory += heap.size;
            }
        }

        // The Vulkan equivalent of D3D12MA's IsUMA(): no memory is device-only.
        pVulkanContext->m_isUnifiedMemory = true;
        for ( uint32_t typeIndex = 0; typeIndex < pVulkanContext->m_memoryProperties.memoryTypeCount; ++typeIndex )
        {
            VkMemoryPropertyFlags const flags = pVulkanContext->m_memoryProperties.memoryTypes[typeIndex].propertyFlags;
            if ( ( flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) && !( flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) )
            {
                pVulkanContext->m_isUnifiedMemory = false;
                break;
            }
        }

        capabilities.m_constantBufferAlignment = uint32_t( limits.minUniformBufferOffsetAlignment );
        capabilities.m_uploadBufferTextureAlignment = uint32_t( limits.optimalBufferCopyOffsetAlignment );
        capabilities.m_uploadBufferTextureRowAlignment = uint32_t( limits.optimalBufferCopyRowPitchAlignment );

        // Copied from the D3D12 backend rather than derived. It is an AMD packet-size heuristic
        // with no Vulkan meaning, and it only feeds root signature sizing, which must agree across both.
        capabilities.m_optimalRootSignatureSizeInDWORDs = 13;

        capabilities.m_numWaveLanes = subgroupProperties.subgroupSize;

        VkSubgroupFeatureFlags const subgroupOperations = subgroupProperties.supportedOperations;
        capabilities.m_waveOpsSupportFlags.ClearAllFlags();
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_BASIC_BIT )            { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Basic ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_VOTE_BIT )             { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Vote ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT )       { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Arithmetic ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT )           { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Ballot ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT )          { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Shuffle ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT ) { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::ShuffleRelative ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT )        { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Clustered ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_QUAD_BIT )             { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Quad ); }
        if ( subgroupOperations & VK_SUBGROUP_FEATURE_PARTITIONED_BIT_NV )   { capabilities.m_waveOpsSupportFlags.SetFlag( WaveOpsSupportFlags::Partitioned ); }

        // Matches the D3D12 backend. Reporting a capability the backend cannot honour would make
        // the engine issue calls that halt.
        capabilities.m_shadingRate = ShadingRate::NotSupported;
        capabilities.m_shadingRateCaps = ShadingRateCaps::NotSupported;
        capabilities.m_shadingRateTexelWidth = 0;
        capabilities.m_shadingRateTexelHeight = 0;

        // drawIndirectCount is required above, so multi-draw indirect is always available.
        capabilities.m_multiDrawIndirect = true;
        // D3D12 command signatures set root constants per draw and Vulkan indirect draws cannot.
        capabilities.m_indirectRootConstant = false;
        // VK_EXT_fragment_shader_interlock is the equivalent, and nothing enables it yet.
        capabilities.m_rasterizerOrderViews = false;
        // The equivalent of DRED is VK_AMD_buffer_marker or VK_NV_device_diagnostic_checkpoints.
        // Neither is wired up.
        capabilities.m_breadcrumbs = false;
        // HDR needs a swapchain colour space that nothing selects yet.
        capabilities.m_hdr = false;

        // What the device can do with each DataFormat, mirroring the D3D12 backend's loop:
        //   SHADER_SAMPLE   -> SAMPLED_IMAGE_BIT
        //   UAV_TYPED_STORE -> STORAGE_IMAGE_BIT
        //   RENDER_TARGET   -> COLOR_ATTACHMENT_BIT
        // m_canRenderTargetWriteTo is colour only on both backends, so a depth format reads false
        // here exactly as it does there. Optimal tiling, because CreateTexture uses it for every image.
        for ( uint32_t formatIndex = 0; formatIndex < NumDataFormats; ++formatIndex )
        {
            VkFormat const vulkanFormat = VulkanFormat( DataFormat( formatIndex ) );
            if ( vulkanFormat == VK_FORMAT_UNDEFINED )
            {
                continue;
            }

            VkFormatProperties formatProperties = {};
            vkGetPhysicalDeviceFormatProperties( pVulkanContext->m_physicalDevice, vulkanFormat, &formatProperties );

            VkFormatFeatureFlags const features = formatProperties.optimalTilingFeatures;

            capabilities.m_canShaderReadFrom[formatIndex] = ( features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) != 0;
            capabilities.m_canShaderWriteTo[formatIndex] = ( features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ) != 0;
            capabilities.m_canRenderTargetWriteTo[formatIndex] = ( features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ) != 0;
        }
    }

    //-------------------------------------------------------------------------
    // Debug messenger
    //-------------------------------------------------------------------------

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback
    (
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData,
        void*
    )
    {
        // The layers call this from whichever thread tripped the check, which the engine's
        // allocator may never have seen.
        if ( !Memory::HasInitializedThreadHeap() )
        {
            Memory::InitializeThreadHeap();
        }

        if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT )
        {
            // A validation error is a build break, and halting here is what makes that true.
            EE_LOG_FATAL_ERROR( LogCategory::Render, "RHI/Validation", "%s", pCallbackData->pMessage );
        }
        else if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT )
        {
            EE_LOG_WARNING( LogCategory::Render, "RHI/Validation", "%s", pCallbackData->pMessage );
        }
        else
        {
            EE_LOG_MESSAGE( LogCategory::Render, "RHI/Validation", "%s", pCallbackData->pMessage );
        }

        return VK_FALSE;
    }

    //-------------------------------------------------------------------------
    // Context
    //-------------------------------------------------------------------------

    Context* CreateContext( ContextParameters const& parameters )
    {
        void* pContextMemory = Memory::Allocators::g_RHI.Alloc( sizeof( VulkanContext ), alignof( VulkanContext ) );
        VulkanContext* pVulkanContext = new ( pContextMemory ) VulkanContext();

        // Instance
        //-------------------------------------------------------------------------

        VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        applicationInfo.pApplicationName = parameters.m_pApplicationName;
        applicationInfo.pEngineName = parameters.m_pEngineName;
        applicationInfo.apiVersion = VK_API_VERSION_1_3;

        TInlineVector<char const*, MaxInstanceExtensions> instanceExtensions;
        TInlineVector<char const*, 4> instanceLayers;

        bool const enableValidation = parameters.m_enableHostValidation || parameters.m_enableDeviceValidation;

        uint32_t numAvailableLayers = 0;
        vkEnumerateInstanceLayerProperties( &numAvailableLayers, nullptr );
        TVector<VkLayerProperties> availableLayers( numAvailableLayers );
        vkEnumerateInstanceLayerProperties( &numAvailableLayers, availableLayers.data() );

        bool validationLayerAvailable = false;
        for ( VkLayerProperties const& layer : availableLayers )
        {
            if ( strcmp( layer.layerName, "VK_LAYER_KHRONOS_validation" ) == 0 )
            {
                validationLayerAvailable = true;
                break;
            }
        }

        uint32_t numInstanceExtensions = 0;
        vkEnumerateInstanceExtensionProperties( nullptr, &numInstanceExtensions, nullptr );
        TVector<VkExtensionProperties> availableInstanceExtensions( numInstanceExtensions );
        vkEnumerateInstanceExtensionProperties( nullptr, &numInstanceExtensions, availableInstanceExtensions.data() );

        if ( enableValidation )
        {
            if ( validationLayerAvailable )
            {
                instanceLayers.emplace_back( "VK_LAYER_KHRONOS_validation" );
                instanceExtensions.emplace_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
            }
            else
            {
                // Not fatal, but a missing layer package is the usual reason validation is silently off.
                EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "VK_LAYER_KHRONOS_validation is not installed, so validation is off. Install vulkan-validationlayers." );
            }
        }
        else
        {
            // Object names and command buffer markers are worth having with or without the layer.
            if ( HasExtension( availableInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) )
            {
                instanceExtensions.emplace_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
            }
        }

        // Surface extensions
        //-------------------------------------------------------------------------
        // A VkSurfaceKHR may only be created from an instance that enabled its platform extension,
        // and the instance is created once, before any window exists. So every platform the loader
        // reports is enabled now.
        //
        // Named by string rather than by macro: the macros only exist once VK_USE_PLATFORM_*_KHR
        // is defined, which drags X11 headers into a file with no other reason to see them.
        static char const* const surfaceExtensions[] =
        {
            "VK_KHR_surface",
            "VK_KHR_xlib_surface",
            "VK_KHR_xcb_surface",
            "VK_KHR_wayland_surface",
        };

        for ( char const* pSurfaceExtension : surfaceExtensions )
        {
            if ( HasExtension( availableInstanceExtensions, pSurfaceExtension ) )
            {
                instanceExtensions.emplace_back( pSurfaceExtension );
            }
        }

        VkInstanceCreateInfo instanceCreateInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceCreateInfo.pApplicationInfo = &applicationInfo;
        instanceCreateInfo.enabledLayerCount = uint32_t( instanceLayers.size() );
        instanceCreateInfo.ppEnabledLayerNames = instanceLayers.data();
        instanceCreateInfo.enabledExtensionCount = uint32_t( instanceExtensions.size() );
        instanceCreateInfo.ppEnabledExtensionNames = instanceExtensions.data();

        VkResult result = vkCreateInstance( &instanceCreateInfo, nullptr, &pVulkanContext->m_instance );
        EE_ASSERT( result == VK_SUCCESS );

        if ( result != VK_SUCCESS )
        {
            pVulkanContext->~VulkanContext();
            Memory::Allocators::g_RHI.Free( (void*&) pVulkanContext );
            return nullptr;
        }

        // RenderDoc
        //-------------------------------------------------------------------------
        // After vkCreateInstance, which matters: on Linux RenderDoc arrives as an implicit Vulkan
        // layer, and the loader only maps librenderdoc.so while servicing that call. Probing any
        // earlier always missed it. RTLD_NOLOAD attaches to a loaded RenderDoc and never loads one.

        if ( parameters.m_enableRenderDoc )
        {
            pVulkanContext->m_pRenderDocLibrary = dlopen( "librenderdoc.so", RTLD_NOW | RTLD_NOLOAD );
            if ( pVulkanContext->m_pRenderDocLibrary != nullptr )
            {
                pRENDERDOC_GetAPI renderdoc_GetAPI = reinterpret_cast<pRENDERDOC_GetAPI>( dlsym( pVulkanContext->m_pRenderDocLibrary, "RENDERDOC_GetAPI" ) );
                if ( renderdoc_GetAPI != nullptr && renderdoc_GetAPI( eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>( &pVulkanContext->m_pRenderDocAPI ) ) == 1 )
                {
                    EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "RenderDoc connected" );
                }
            }
        }

        // Debug messenger
        //-------------------------------------------------------------------------

        if ( !instanceExtensions.empty() )
        {
            auto vkCreateDebugUtilsMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkCreateDebugUtilsMessengerEXT" ) );
            if ( vkCreateDebugUtilsMessenger != nullptr && enableValidation )
            {
                VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
                messengerCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                messengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                messengerCreateInfo.pfnUserCallback = DebugMessengerCallback;

                result = vkCreateDebugUtilsMessenger( pVulkanContext->m_instance, &messengerCreateInfo, nullptr, &pVulkanContext->m_debugMessenger );
                EE_ASSERT( result == VK_SUCCESS );
            }
        }

        // Physical device
        //-------------------------------------------------------------------------

        uint32_t numPhysicalDevices = 0;
        vkEnumeratePhysicalDevices( pVulkanContext->m_instance, &numPhysicalDevices, nullptr );
        EE_ASSERT( numPhysicalDevices > 0 );

        TVector<VkPhysicalDevice> physicalDevices( numPhysicalDevices );
        vkEnumeratePhysicalDevices( pVulkanContext->m_instance, &numPhysicalDevices, physicalDevices.data() );

        if ( parameters.m_deviceSelection == DeviceSelectionPreference::UseProvidedIndex )
        {
            EE_ASSERT( parameters.m_deviceIndex < numPhysicalDevices );

            VkPhysicalDevice const requested = physicalDevices[parameters.m_deviceIndex];
            if ( char const* pReason = GetDeviceRejectionReason( requested ) )
            {
                EE_LOG_FATAL_ERROR( LogCategory::Render, "RHI/CreateContext", "Device index %u cannot run this engine: %s is missing", parameters.m_deviceIndex, pReason );
            }
            else
            {
                pVulkanContext->m_physicalDevice = requested;
            }
        }
        else
        {
            uint32_t bestScore = 0;
            for ( VkPhysicalDevice physicalDevice : physicalDevices )
            {
                VkPhysicalDeviceProperties properties = {};
                vkGetPhysicalDeviceProperties( physicalDevice, &properties );

                if ( char const* pReason = GetDeviceRejectionReason( physicalDevice ) )
                {
                    EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "Skipping GPU %s: %s is missing", properties.deviceName, pReason );
                    continue;
                }

                uint32_t const score = ScoreDevice( physicalDevice, parameters.m_deviceSelection );
                if ( pVulkanContext->m_physicalDevice == VK_NULL_HANDLE || score > bestScore )
                {
                    pVulkanContext->m_physicalDevice = physicalDevice;
                    bestScore = score;
                }
            }
        }

        if ( pVulkanContext->m_physicalDevice == VK_NULL_HANDLE )
        {
            // There is no fallback binding model, so this is fatal rather than a degraded mode.
            EE_LOG_FATAL_ERROR( LogCategory::Render, "RHI/CreateContext", "No Vulkan device meets the requirements of the engine's binding model" );

            vkDestroyInstance( pVulkanContext->m_instance, nullptr );
            pVulkanContext->~VulkanContext();
            Memory::Allocators::g_RHI.Free( (void*&) pVulkanContext );
            return nullptr;
        }

        FillDeviceCapabilities( pVulkanContext );
        SelectQueueFamilies( pVulkanContext );

        VkPhysicalDeviceProperties const& deviceProperties = pVulkanContext->m_physicalDeviceProperties;

        pVulkanContext->m_vendorInfo.m_vendorID = FormatVendorID( deviceProperties.vendorID );
        pVulkanContext->m_vendorInfo.m_deviceID = FormatVendorID( deviceProperties.deviceID );
        pVulkanContext->m_vendorInfo.m_revisionID = FormatVendorID( deviceProperties.driverVersion );
        pVulkanContext->m_vendorInfo.m_deviceName = TInlineString<MaxVendorNameLength>( deviceProperties.deviceName );
        // No Vulkan query exposes raytracing core counts, and the D3D12 backend does not fill this either.
        pVulkanContext->m_vendorInfo.m_numRaytracingCores = 0;

        EE_LOG_MESSAGE
        (
            LogCategory::Render, "RHI/CreateContext", "GPU %s %s %s",
            pVulkanContext->m_vendorInfo.m_vendorID.c_str(),
            pVulkanContext->m_vendorInfo.m_deviceID.c_str(),
            pVulkanContext->m_vendorInfo.m_deviceName.c_str()
        );

        // Vulkan has no linked node adapter. Multi-GPU is explicit device groups, which the
        // engine never asks for.
        pVulkanContext->m_numLinkedNodes = 1;
        pVulkanContext->m_unlinkedNodeIndex = 0;
        pVulkanContext->m_deviceMode = DeviceMode::Single;
        pVulkanContext->m_shaderModel = parameters.m_shaderModel;

        // Device
        //-------------------------------------------------------------------------

        // One VkQueue per RHI queue where the family allows it. Two RHI queues sharing a VkQueue
        // makes a cross-queue QueueDeviceWait between them a deadlock, because the value it waits
        // for is only signalled by a later submit on that same VkQueue.
        uint32_t numQueueFamilies = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, nullptr );
        TVector<VkQueueFamilyProperties> queueFamilyProperties( numQueueFamilies );
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, queueFamilyProperties.data() );

        float const queuePriorities[3] = { 1.0f, 1.0f, 1.0f };
        TInlineVector<VkDeviceQueueCreateInfo, 3> queueCreateInfos;

        for ( uint32_t familyIndex : { pVulkanContext->m_graphicsQueueFamily, pVulkanContext->m_computeQueueFamily, pVulkanContext->m_transferQueueFamily } )
        {
            VulkanContext::QueueFamilyAllocation* pAllocation = nullptr;
            for ( VulkanContext::QueueFamilyAllocation& allocation : pVulkanContext->m_queueFamilyAllocations )
            {
                if ( allocation.m_familyIndex == familyIndex )
                {
                    pAllocation = &allocation;
                    break;
                }
            }

            if ( pAllocation == nullptr )
            {
                pVulkanContext->m_queueFamilyAllocations.emplace_back( VulkanContext::QueueFamilyAllocation{ familyIndex, 0, 0 } );
                pAllocation = &pVulkanContext->m_queueFamilyAllocations.back();
            }

            // A family with one queue is normal on integrated parts.
            if ( pAllocation->m_numQueues < queueFamilyProperties[familyIndex].queueCount )
            {
                pAllocation->m_numQueues++;
            }
        }

        for ( VulkanContext::QueueFamilyAllocation const& allocation : pVulkanContext->m_queueFamilyAllocations )
        {
            VkDeviceQueueCreateInfo queueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            queueCreateInfo.queueFamilyIndex = allocation.m_familyIndex;
            queueCreateInfo.queueCount = allocation.m_numQueues;
            queueCreateInfo.pQueuePriorities = queuePriorities;
            queueCreateInfos.emplace_back( queueCreateInfo );
        }

        // Ask only for what GetDeviceRejectionReason verified. Enabling an absent feature is a
        // validation error, and enabling an unused one costs performance on some drivers.
        RequiredFeatures enabledFeatures = {};
        enabledFeatures.Chain();

        enabledFeatures.m_vulkan13.dynamicRendering = VK_TRUE;
        enabledFeatures.m_vulkan13.synchronization2 = VK_TRUE;

        enabledFeatures.m_vulkan12.timelineSemaphore = VK_TRUE;
        enabledFeatures.m_vulkan12.bufferDeviceAddress = VK_TRUE;
        enabledFeatures.m_vulkan12.drawIndirectCount = VK_TRUE;
        enabledFeatures.m_vulkan12.scalarBlockLayout = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.runtimeDescriptorArray = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingPartiallyBound = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
        enabledFeatures.m_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
        enabledFeatures.m_vulkan12.samplerFilterMinmax = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;

        enabledFeatures.m_mutableDescriptorType.mutableDescriptorType = VK_TRUE;

        // Asked for when present, never required. Nothing in the engine creates a
        // PipelineStatistics query pool, so CreateQueryPool asserts on the flag rather than
        // refusing the whole device.
        RequiredFeatures availableFeatures = {};
        availableFeatures.Chain();
        vkGetPhysicalDeviceFeatures2( pVulkanContext->m_physicalDevice, &availableFeatures.m_features2 );

        pVulkanContext->m_pipelineStatisticsQuery = availableFeatures.m_features2.features.pipelineStatisticsQuery == VK_TRUE;
        enabledFeatures.m_features2.features.pipelineStatisticsQuery = availableFeatures.m_features2.features.pipelineStatisticsQuery;

        // 16-bit types
        //-------------------------------------------------------------------------
        // Not optional: the engine's shaders declare float16_t and uint16_t, DXC emits the matching
        // SPIR-V capabilities, and vkCreateShaderModule rejects a module whose capabilities the
        // device did not enable. D3D12 has no equivalent step.
        //
        // Asked for rather than required, and logged when absent, so a device without them fails
        // at the shader that needs one. That names the shader.
        enabledFeatures.m_features2.features.shaderInt16 = availableFeatures.m_features2.features.shaderInt16;
        enabledFeatures.m_vulkan12.shaderFloat16 = availableFeatures.m_vulkan12.shaderFloat16;
        enabledFeatures.m_vulkan11.storageBuffer16BitAccess = availableFeatures.m_vulkan11.storageBuffer16BitAccess;
        enabledFeatures.m_vulkan11.uniformAndStorageBuffer16BitAccess = availableFeatures.m_vulkan11.uniformAndStorageBuffer16BitAccess;
        enabledFeatures.m_vulkan11.storagePushConstant16 = availableFeatures.m_vulkan11.storagePushConstant16;

        // A 16-bit type in an Input or Output variable is gated separately from the buffer accesses
        // above. No shader declares one any more, but it is still enabled when the device has it.
        enabledFeatures.m_vulkan11.storageInputOutput16 = availableFeatures.m_vulkan11.storageInputOutput16;

        if ( availableFeatures.m_features2.features.shaderInt16 != VK_TRUE ||
             availableFeatures.m_vulkan12.shaderFloat16 != VK_TRUE )
        {
            EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "This device is missing 16-bit shader types (shaderInt16 %s, shaderFloat16 %s). Shaders that use them will fail to create.",
                            availableFeatures.m_features2.features.shaderInt16 ? "yes" : "no",
                            availableFeatures.m_vulkan12.shaderFloat16 ? "yes" : "no" );
        }

        if ( availableFeatures.m_vulkan11.storageInputOutput16 != VK_TRUE )
        {
            EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "This device has no storageInputOutput16. No shader in the engine declares it, so nothing depends on it." );
        }

        // 64-bit types and subgroup operations
        //-------------------------------------------------------------------------
        // Two more capabilities the engine's shaders declare and D3D12 does not gate. shaderInt64
        // carries the Int64 capability; shaderSubgroupExtendedTypes lets a wave operation take a
        // 16-, 64- or 8-bit operand, which the culling shaders do.
        enabledFeatures.m_features2.features.shaderInt64 = availableFeatures.m_features2.features.shaderInt64;
        enabledFeatures.m_vulkan12.shaderSubgroupExtendedTypes = availableFeatures.m_vulkan12.shaderSubgroupExtendedTypes;

        // HLSL's `discard` compiles to OpDemoteToHelperInvocation under Shader Model 6.6, which
        // Vulkan gates and D3D12 does not. Every material pixel shader has one.
        enabledFeatures.m_vulkan13.shaderDemoteToHelperInvocation = availableFeatures.m_vulkan13.shaderDemoteToHelperInvocation;

        // A 64-bit InterlockedMin or InterlockedAdd. Buffer and groupshared are separate bits.
        enabledFeatures.m_vulkan12.shaderBufferInt64Atomics = availableFeatures.m_vulkan12.shaderBufferInt64Atomics;
        enabledFeatures.m_vulkan12.shaderSharedInt64Atomics = availableFeatures.m_vulkan12.shaderSharedInt64Atomics;

        // Indirect draws read their command index out of the DrawIndex builtin, which carries the
        // DrawParameters capability. D3D12 pushes the data per draw instead, so it needs no index.
        enabledFeatures.m_vulkan11.shaderDrawParameters = availableFeatures.m_vulkan11.shaderDrawParameters;

        // CreateGraphicsOrMeshPipeline sets depthClampEnable from the engine's rasterizer state,
        // which is the inverse of D3D12's DepthClipEnable. D3D12 has no feature bit for it.
        enabledFeatures.m_features2.features.depthClamp = availableFeatures.m_features2.features.depthClamp;

        if ( availableFeatures.m_features2.features.shaderInt64 != VK_TRUE ||
             availableFeatures.m_vulkan12.shaderSubgroupExtendedTypes != VK_TRUE )
        {
            EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "This device is missing 64-bit or extended subgroup shader types (shaderInt64 %s, shaderSubgroupExtendedTypes %s). Shaders that use them will fail to create.",
                            availableFeatures.m_features2.features.shaderInt64 ? "yes" : "no",
                            availableFeatures.m_vulkan12.shaderSubgroupExtendedTypes ? "yes" : "no" );
        }

        // Optional device extensions
        //-------------------------------------------------------------------------
        // Asked for when present and asserted at the point of use. Refusing a whole device over a
        // feature the engine needs for one pass would be worse than halting in that pass.
        TInlineVector<char const*, MaxDeviceExtensions> deviceExtensions;
        for ( char const* pRequiredExtension : g_requiredDeviceExtensions )
        {
            deviceExtensions.emplace_back( pRequiredExtension );
        }

        TVector<VkExtensionProperties> const availableDeviceExtensions = EnumerateDeviceExtensions( pVulkanContext->m_physicalDevice );

        VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        if ( HasExtension( availableDeviceExtensions, VK_EXT_MESH_SHADER_EXTENSION_NAME ) )
        {
            VkPhysicalDeviceFeatures2 meshShaderQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            meshShaderQuery.pNext = &meshShaderFeatures;
            vkGetPhysicalDeviceFeatures2( pVulkanContext->m_physicalDevice, &meshShaderQuery );

            // Both, because the engine's debug draw uses a task stage as well as a mesh one.
            if ( meshShaderFeatures.meshShader && meshShaderFeatures.taskShader )
            {
                // The query filled every bit the device supports, and vkCreateDevice reads the
                // same struct, so anything left set is being asked for. multiviewMeshShader is
                // the one that bites: it also requires VkPhysicalDeviceMultiviewFeatures::multiview,
                // and vkCreateDevice rejects that pair.
                meshShaderFeatures.multiviewMeshShader = VK_FALSE;
                meshShaderFeatures.primitiveFragmentShadingRateMeshShader = VK_FALSE;
                meshShaderFeatures.meshShaderQueries = VK_FALSE;

                deviceExtensions.emplace_back( VK_EXT_MESH_SHADER_EXTENSION_NAME );
                enabledFeatures.m_mutableDescriptorType.pNext = &meshShaderFeatures;

                pVulkanContext->m_meshShader = true;
                g_meshShaderEnabled = true;
            }
        }

        VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragmentShadingRateFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
        if ( HasExtension( availableDeviceExtensions, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME ) )
        {
            VkPhysicalDeviceFeatures2 fragmentShadingRateQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            fragmentShadingRateQuery.pNext = &fragmentShadingRateFeatures;
            vkGetPhysicalDeviceFeatures2( pVulkanContext->m_physicalDevice, &fragmentShadingRateQuery );

            // Both, because the RHI exposes a per-draw rate and a per-tile image and
            // CmdSetShadingRate takes them in one call.
            if ( fragmentShadingRateFeatures.pipelineFragmentShadingRate && fragmentShadingRateFeatures.attachmentFragmentShadingRate )
            {
                // Clear what the engine cannot exercise, as the mesh shader block does. No shader
                // writes a per-primitive rate.
                fragmentShadingRateFeatures.primitiveFragmentShadingRate = VK_FALSE;

                deviceExtensions.emplace_back( VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME );
                // Chained after the mesh shader features, which may or may not be there.
                fragmentShadingRateFeatures.pNext = enabledFeatures.m_mutableDescriptorType.pNext;
                enabledFeatures.m_mutableDescriptorType.pNext = &fragmentShadingRateFeatures;

                pVulkanContext->m_fragmentShadingRate = true;
                g_fragmentShadingRateEnabled = true;
            }
        }

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
        if ( HasExtension( availableDeviceExtensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME ) &&
             HasExtension( availableDeviceExtensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME ) &&
             HasExtension( availableDeviceExtensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME ) )
        {
            accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;

            VkPhysicalDeviceFeatures2 raytracingQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            raytracingQuery.pNext = &accelerationStructureFeatures;
            vkGetPhysicalDeviceFeatures2( pVulkanContext->m_physicalDevice, &raytracingQuery );

            if ( accelerationStructureFeatures.accelerationStructure && rayTracingPipelineFeatures.rayTracingPipeline )
            {
                // Ask for the two bits the backend uses and clear the rest, as the mesh shader
                // block does. g_resourceHeapMutableTypes has no acceleration structure type, so
                // the update-after-bind bit is not needed either.
                accelerationStructureFeatures.accelerationStructureCaptureReplay = VK_FALSE;
                accelerationStructureFeatures.accelerationStructureIndirectBuild = VK_FALSE;
                accelerationStructureFeatures.accelerationStructureHostCommands = VK_FALSE;
                accelerationStructureFeatures.descriptorBindingAccelerationStructureUpdateAfterBind = VK_FALSE;

                rayTracingPipelineFeatures.rayTracingPipelineShaderGroupHandleCaptureReplay = VK_FALSE;
                rayTracingPipelineFeatures.rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE;
                rayTracingPipelineFeatures.rayTracingPipelineTraceRaysIndirect = VK_FALSE;
                rayTracingPipelineFeatures.rayTraversalPrimitiveCulling = VK_FALSE;

                // VK_KHR_deferred_host_operations carries no feature bit. It is a dependency of
                // VK_KHR_acceleration_structure and has to be enabled for it to load.
                deviceExtensions.emplace_back( VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME );
                deviceExtensions.emplace_back( VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME );
                deviceExtensions.emplace_back( VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME );

                rayTracingPipelineFeatures.pNext = enabledFeatures.m_mutableDescriptorType.pNext;
                enabledFeatures.m_mutableDescriptorType.pNext = &accelerationStructureFeatures;

                pVulkanContext->m_raytracing = true;
                g_raytracingEnabled = true;
            }
        }

        // Two extensions the engine's shaders need, rather than passes the RHI exposes. Neither has
        // an RHI entry point to assert in, so a device without one fails at the shader that
        // declares it. InstancePickingResolve.esf reads an R64_UINT texel buffer; the material
        // shaders read SV_Barycentrics. D3D12 folds both into the shader model.
        VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT shaderImageAtomicInt64Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT };
        if ( HasExtension( availableDeviceExtensions, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME ) )
        {
            VkPhysicalDeviceFeatures2 shaderImageAtomicInt64Query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            shaderImageAtomicInt64Query.pNext = &shaderImageAtomicInt64Features;
            vkGetPhysicalDeviceFeatures2( pVulkanContext->m_physicalDevice, &shaderImageAtomicInt64Query );

            if ( shaderImageAtomicInt64Features.shaderImageInt64Atomics )
            {
                deviceExtensions.emplace_back( VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME );
                // Sparse is a separate bit and nothing here uses it, so it is left as queried.
                shaderImageAtomicInt64Features.sparseImageInt64Atomics = VK_FALSE;
                shaderImageAtomicInt64Features.pNext = enabledFeatures.m_mutableDescriptorType.pNext;
                enabledFeatures.m_mutableDescriptorType.pNext = &shaderImageAtomicInt64Features;
            }
        }

        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragmentShaderBarycentricFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR };
        if ( HasExtension( availableDeviceExtensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME ) )
        {
            VkPhysicalDeviceFeatures2 fragmentShaderBarycentricQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            fragmentShaderBarycentricQuery.pNext = &fragmentShaderBarycentricFeatures;
            vkGetPhysicalDeviceFeatures2( pVulkanContext->m_physicalDevice, &fragmentShaderBarycentricQuery );

            if ( fragmentShaderBarycentricFeatures.fragmentShaderBarycentric )
            {
                deviceExtensions.emplace_back( VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME );
                fragmentShaderBarycentricFeatures.pNext = enabledFeatures.m_mutableDescriptorType.pNext;
                enabledFeatures.m_mutableDescriptorType.pNext = &fragmentShaderBarycentricFeatures;
            }
        }

        // ClusterCompaction.esf calls WaveMatch, which is core SM 6.5 on D3D12 and has no core
        // Vulkan equivalent at all: DXC lowers it to OpGroupNonUniformPartitionNV, so the module
        // declares SPV_NV_shader_subgroup_partitioned and vkCreateComputePipelines fails
        // VUID-VkShaderModuleCreateInfo-pCode-08742 unless this is enabled.
        //
        // A vendor extension, and the only one this backend enables. **That makes the compaction
        // pass NVIDIA-only**, and compaction is what writes the draw arguments, so a device
        // without it renders no geometry rather than losing one effect. Escalated and accepted on
        // 2026-09-03; the alternative was rewriting WaveMatch as a portable ballot loop in a
        // shared shader, which reaches D3D12. See Docs/Linux/Blocked.md.
        //
        // No feature struct to chain: the extension only gates the SPIR-V capability, so there is
        // nothing to query and nothing to clear. Do not add a features query here - P8.5's
        // query-as-enable-request defect was exactly that shape.
        if ( HasExtension( availableDeviceExtensions, VK_NV_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME ) )
        {
            deviceExtensions.emplace_back( VK_NV_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME );
        }
        else
        {
            EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "This device has no VK_NV_shader_subgroup_partitioned. ClusterCompaction.esf uses WaveMatch, so cluster compaction will fail at pipeline creation and no geometry will be drawn." );
        }

        if ( !pVulkanContext->m_meshShader )
        {
            // Loud, because the engine has no fallback and RenderPass_DebugDraw will halt in CmdDispatchMesh.
            EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "This device has no VK_EXT_mesh_shader, so the mesh shader path will halt if the engine reaches it." );
        }

        VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceCreateInfo.pNext = &enabledFeatures.m_features2;
        deviceCreateInfo.queueCreateInfoCount = uint32_t( queueCreateInfos.size() );
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.enabledExtensionCount = uint32_t( deviceExtensions.size() );
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

        result = vkCreateDevice( pVulkanContext->m_physicalDevice, &deviceCreateInfo, nullptr, &pVulkanContext->m_device );
        EE_ASSERT( result == VK_SUCCESS );

        // Resolved now, because everything created from here on names itself.
        pVulkanContext->m_vkSetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkSetDebugUtilsObjectNameEXT" ) );
        pVulkanContext->m_vkCmdBeginDebugUtilsLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkCmdBeginDebugUtilsLabelEXT" ) );
        pVulkanContext->m_vkCmdEndDebugUtilsLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkCmdEndDebugUtilsLabelEXT" ) );

        if ( pVulkanContext->m_raytracing )
        {
            pVulkanContext->m_vkCreateAccelerationStructure = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCreateAccelerationStructureKHR" ) );
            pVulkanContext->m_vkDestroyAccelerationStructure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkDestroyAccelerationStructureKHR" ) );
            pVulkanContext->m_vkGetAccelerationStructureBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkGetAccelerationStructureBuildSizesKHR" ) );
            pVulkanContext->m_vkGetAccelerationStructureDeviceAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkGetAccelerationStructureDeviceAddressKHR" ) );
            pVulkanContext->m_vkCmdBuildAccelerationStructures = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdBuildAccelerationStructuresKHR" ) );
            pVulkanContext->m_vkCreateRayTracingPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCreateRayTracingPipelinesKHR" ) );
            pVulkanContext->m_vkGetRayTracingShaderGroupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkGetRayTracingShaderGroupHandlesKHR" ) );
            pVulkanContext->m_vkCmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdTraceRaysKHR" ) );
            // Core to VK_KHR_ray_tracing_maintenance1 rather than the pipeline extension, so it
            // is allowed to be null. CmdExecuteIndirect on a DispatchRays signature asserts on it.
            pVulkanContext->m_vkCmdTraceRaysIndirect2 = reinterpret_cast<PFN_vkCmdTraceRaysIndirect2KHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdTraceRaysIndirect2KHR" ) );

            EE_ASSERT( pVulkanContext->m_vkCreateAccelerationStructure != nullptr );
            EE_ASSERT( pVulkanContext->m_vkCmdTraceRays != nullptr );
        }

        if ( pVulkanContext->m_fragmentShadingRate )
        {
            pVulkanContext->m_vkCmdSetFragmentShadingRate = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdSetFragmentShadingRateKHR" ) );
            EE_ASSERT( pVulkanContext->m_vkCmdSetFragmentShadingRate != nullptr );
        }

        if ( pVulkanContext->m_meshShader )
        {
            pVulkanContext->m_vkCmdDrawMeshTasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdDrawMeshTasksEXT" ) );
            pVulkanContext->m_vkCmdDrawMeshTasksIndirect = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdDrawMeshTasksIndirectEXT" ) );
            pVulkanContext->m_vkCmdDrawMeshTasksIndirectCount = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectCountEXT>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdDrawMeshTasksIndirectCountEXT" ) );
            EE_ASSERT( pVulkanContext->m_vkCmdDrawMeshTasks != nullptr );
        }
        pVulkanContext->m_vkCmdPushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>( vkGetDeviceProcAddr( pVulkanContext->m_device, "vkCmdPushDescriptorSetKHR" ) );
        EE_ASSERT( pVulkanContext->m_vkCmdPushDescriptorSet != nullptr );

        // VMA
        //-------------------------------------------------------------------------

        VkAllocationCallbacks vulkanAllocationCallbacks = {};
        vulkanAllocationCallbacks.pfnAllocation = [] ( void*, size_t size, size_t alignment, VkSystemAllocationScope ) -> void*
        {
            return Memory::Allocators::g_Vulkan.Alloc( size, alignment );
        };
        vulkanAllocationCallbacks.pfnReallocation = [] ( void*, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope ) -> void*
        {
            return Memory::Allocators::g_Vulkan.Realloc( pOriginal, size, alignment );
        };
        vulkanAllocationCallbacks.pfnFree = [] ( void*, void* pPtr )
        {
            if ( pPtr != nullptr )
            {
                Memory::Allocators::g_Vulkan.Free( pPtr );
            }
        };

        pVulkanContext->m_hostAllocationCallbacks = vulkanAllocationCallbacks;

        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.instance = pVulkanContext->m_instance;
        allocatorCreateInfo.physicalDevice = pVulkanContext->m_physicalDevice;
        allocatorCreateInfo.device = pVulkanContext->m_device;
        allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        // bufferDeviceAddress is enabled above, so VMA has to know: it adds
        // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and the matching allocate flag itself.
        allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocatorCreateInfo.pAllocationCallbacks = &pVulkanContext->m_hostAllocationCallbacks;

        result = vmaCreateAllocator( &allocatorCreateInfo, &pVulkanContext->m_resourceAllocator );
        EE_ASSERT( result == VK_SUCCESS );

        // Descriptor heaps
        //-------------------------------------------------------------------------

        VkMutableDescriptorTypeListEXT mutableTypeLists[2] = {};
        mutableTypeLists[g_resourceHeapBinding].descriptorTypeCount = uint32_t( eastl::size( g_resourceHeapMutableTypes ) );
        mutableTypeLists[g_resourceHeapBinding].pDescriptorTypes = g_resourceHeapMutableTypes;
        // Binding 1 is a plain sampler array, so it has no mutable list.
        mutableTypeLists[g_samplerHeapBinding].descriptorTypeCount = 0;
        mutableTypeLists[g_samplerHeapBinding].pDescriptorTypes = nullptr;

        VkMutableDescriptorTypeCreateInfoEXT mutableTypeCreateInfo = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT };
        mutableTypeCreateInfo.mutableDescriptorTypeListCount = 2;
        mutableTypeCreateInfo.pMutableDescriptorTypeLists = mutableTypeLists;

        VkDescriptorSetLayoutBinding heapBindings[2] = {};
        heapBindings[g_resourceHeapBinding].binding = g_resourceHeapBinding;
        heapBindings[g_resourceHeapBinding].descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
        heapBindings[g_resourceHeapBinding].descriptorCount = g_resourceHeapSize;
        heapBindings[g_resourceHeapBinding].stageFlags = VK_SHADER_STAGE_ALL;

        heapBindings[g_samplerHeapBinding].binding = g_samplerHeapBinding;
        heapBindings[g_samplerHeapBinding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        heapBindings[g_samplerHeapBinding].descriptorCount = g_samplerHeapSize;
        heapBindings[g_samplerHeapBinding].stageFlags = VK_SHADER_STAGE_ALL;

        // PARTIALLY_BOUND because most of the heap is empty at any moment, and UPDATE_AFTER_BIND
        // because the engine writes handles while command buffers that reference them are recording.
        //
        // No VARIABLE_DESCRIPTOR_COUNT: only the highest-numbered binding in a set may carry it,
        // and both heaps are allocated at their full declared size anyway.
        VkDescriptorBindingFlags const heapBindingFlags[2] =
        {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };

        VkDescriptorSetLayoutBindingFlagsCreateInfo heapBindingFlagsCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        heapBindingFlagsCreateInfo.pNext = &mutableTypeCreateInfo;
        heapBindingFlagsCreateInfo.bindingCount = 2;
        heapBindingFlagsCreateInfo.pBindingFlags = heapBindingFlags;

        VkDescriptorSetLayoutCreateInfo heapSetLayoutCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        heapSetLayoutCreateInfo.pNext = &heapBindingFlagsCreateInfo;
        heapSetLayoutCreateInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        heapSetLayoutCreateInfo.bindingCount = 2;
        heapSetLayoutCreateInfo.pBindings = heapBindings;

        result = vkCreateDescriptorSetLayout( pVulkanContext->m_device, &heapSetLayoutCreateInfo, nullptr, &pVulkanContext->m_heapSetLayout );
        EE_ASSERT( result == VK_SUCCESS );

        // One pool holding exactly one set. The mutable pool size covers binding 0 whatever
        // type each slot ends up holding, which is the point of a mutable descriptor.
        VkDescriptorPoolSize heapPoolSizes[2] = {};
        heapPoolSizes[0].type = VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
        heapPoolSizes[0].descriptorCount = g_resourceHeapSize;
        heapPoolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        heapPoolSizes[1].descriptorCount = g_samplerHeapSize;

        VkDescriptorPoolCreateInfo heapPoolCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        heapPoolCreateInfo.pNext = &mutableTypeCreateInfo;
        heapPoolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        heapPoolCreateInfo.maxSets = 1;
        heapPoolCreateInfo.poolSizeCount = 2;
        heapPoolCreateInfo.pPoolSizes = heapPoolSizes;

        result = vkCreateDescriptorPool( pVulkanContext->m_device, &heapPoolCreateInfo, nullptr, &pVulkanContext->m_heapDescriptorPool );
        EE_ASSERT( result == VK_SUCCESS );

        VkDescriptorSetAllocateInfo heapSetAllocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        heapSetAllocateInfo.descriptorPool = pVulkanContext->m_heapDescriptorPool;
        heapSetAllocateInfo.descriptorSetCount = 1;
        heapSetAllocateInfo.pSetLayouts = &pVulkanContext->m_heapSetLayout;

        result = vkAllocateDescriptorSets( pVulkanContext->m_device, &heapSetAllocateInfo, &pVulkanContext->m_heapDescriptorSet );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_DESCRIPTOR_SET, uint64_t( pVulkanContext->m_heapDescriptorSet ), "BindlessHeaps" );

        // HandleAllocator works in 64-slot pages, so the capacities round up to whole pages.
        pVulkanContext->m_resourceHeapAllocator.Initialize( ( g_resourceHeapSize + 63 ) / 64 );
        pVulkanContext->m_samplerHeapAllocator.Initialize( ( g_samplerHeapSize + 63 ) / 64 );

        // A descriptor set is created once with a fixed descriptor count, so the allocator must not
        // grow. Growing would hand back an index the set does not contain, rather than the invalid
        // handle the caller checks for.
        pVulkanContext->m_resourceHeapAllocator.SetIsGrowable( false );
        pVulkanContext->m_samplerHeapAllocator.SetIsGrowable( false );

        EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "ShaderResource descriptor pool size: %i", g_resourceHeapSize );
        EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "Sampler descriptor pool size: %i", g_samplerHeapSize );

        //-------------------------------------------------------------------------

        pVulkanContext->m_hostValidation = parameters.m_enableHostValidation && validationLayerAvailable;
        pVulkanContext->m_deviceValidation = parameters.m_enableDeviceValidation && validationLayerAvailable;
        // DRED has no portable Vulkan equivalent, and nothing here provides one.
        pVulkanContext->m_deviceBreadcrumbs = false;
        pVulkanContext->m_renderDoc = pVulkanContext->m_pRenderDocAPI != nullptr;
        // AGS is D3D12 only by construction.
        pVulkanContext->m_amdAgsEnabled = false;

        return pVulkanContext;
    }

    void DestroyContext( Context*&& context )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( context );

        if ( pVulkanContext == nullptr )
        {
            return;
        }

        // Every per-descriptor-type allocation must have been freed, as the D3D12 backend checks.
        for ( [[maybe_unused]] auto const& pair : pVulkanContext->m_bufferStats )
        {
            EE_ASSERT( pair.second.m_numAllocations == 0 );
            EE_ASSERT( pair.second.m_numBytes == 0 );
        }

        for ( [[maybe_unused]] auto const& pair : pVulkanContext->m_textureStats )
        {
            EE_ASSERT( pair.second.m_numAllocations == 0 );
            EE_ASSERT( pair.second.m_numBytes == 0 );
        }

        pVulkanContext->m_resourceHeapAllocator.Shutdown();
        pVulkanContext->m_samplerHeapAllocator.Shutdown();

        // Neither the barrier mapping nor the dynamic state list outlives a context.
        g_meshShaderEnabled = false;
        g_fragmentShadingRateEnabled = false;
        g_raytracingEnabled = false;

        if ( pVulkanContext->m_heapDescriptorPool != VK_NULL_HANDLE )
        {
            // Destroying the pool frees the set allocated from it.
            vkDestroyDescriptorPool( pVulkanContext->m_device, pVulkanContext->m_heapDescriptorPool, nullptr );
        }

        if ( pVulkanContext->m_heapSetLayout != VK_NULL_HANDLE )
        {
            vkDestroyDescriptorSetLayout( pVulkanContext->m_device, pVulkanContext->m_heapSetLayout, nullptr );
        }

        if ( pVulkanContext->m_resourceAllocator != VK_NULL_HANDLE )
        {
            // Recorded while the allocator still exists, because ReportDeviceMemoryLeaks runs
            // after this point.
            VmaTotalStatistics statistics = {};
            vmaCalculateStatistics( pVulkanContext->m_resourceAllocator, &statistics );

            g_leakedDeviceAllocations += statistics.total.statistics.allocationCount;
            g_leakedDeviceAllocationBytes += statistics.total.statistics.allocationBytes;

            vmaDestroyAllocator( pVulkanContext->m_resourceAllocator );
            pVulkanContext->m_resourceAllocator = VK_NULL_HANDLE;
        }

        if ( pVulkanContext->m_device != VK_NULL_HANDLE )
        {
            vkDestroyDevice( pVulkanContext->m_device, nullptr );
        }

        if ( pVulkanContext->m_debugMessenger != VK_NULL_HANDLE )
        {
            auto vkDestroyDebugUtilsMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkDestroyDebugUtilsMessengerEXT" ) );
            if ( vkDestroyDebugUtilsMessenger != nullptr )
            {
                vkDestroyDebugUtilsMessenger( pVulkanContext->m_instance, pVulkanContext->m_debugMessenger, nullptr );
            }
        }

        if ( pVulkanContext->m_instance != VK_NULL_HANDLE )
        {
            vkDestroyInstance( pVulkanContext->m_instance, nullptr );
        }

        // The RenderDoc API pointer belongs to the library, so it is not freed, only dropped.
        if ( pVulkanContext->m_pRenderDocLibrary != nullptr )
        {
            dlclose( pVulkanContext->m_pRenderDocLibrary );
        }

        pVulkanContext->~VulkanContext();
        Memory::Allocators::g_RHI.Free( (void*&) pVulkanContext );
    }

    uint64_t GetTotalAllocatedDeviceMemory( Context* pContext )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );

        TInlineVector<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets( pVulkanContext->m_memoryProperties.memoryHeapCount );
        vmaGetHeapBudgets( pVulkanContext->m_resourceAllocator, budgets.data() );

        uint64_t totalUsageBytes = 0;
        for ( VmaBudget const& budget : budgets )
        {
            totalUsageBytes += budget.usage;
        }

        return totalUsageBytes;
    }

    void GetDetailedMemoryStatistics( Context* pContext, uint64_t& localUsageBytes, uint64_t& localAvailableBytes, uint64_t& nonLocalUsageBytes, uint64_t& nonLocalAvailableBytes )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );

        TInlineVector<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets( pVulkanContext->m_memoryProperties.memoryHeapCount );
        vmaGetHeapBudgets( pVulkanContext->m_resourceAllocator, budgets.data() );

        localUsageBytes = 0;
        localAvailableBytes = 0;
        nonLocalUsageBytes = 0;
        nonLocalAvailableBytes = 0;

        // "Local" is D3D12's word for device memory. The Vulkan split is the
        // DEVICE_LOCAL heap flag, which is the same distinction.
        for ( uint32_t heapIndex = 0; heapIndex < pVulkanContext->m_memoryProperties.memoryHeapCount; ++heapIndex )
        {
            bool const isDeviceLocal = ( pVulkanContext->m_memoryProperties.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) != 0;

            if ( isDeviceLocal )
            {
                localUsageBytes += budgets[heapIndex].usage;
                localAvailableBytes += budgets[heapIndex].budget;
            }
            else
            {
                nonLocalUsageBytes += budgets[heapIndex].usage;
                nonLocalAvailableBytes += budgets[heapIndex].budget;
            }
        }
    }

    void GetResourceAllocationStatistics( Context* pContext, TVector<ResourceAllocationStatistic>& outBufferStats, TVector<ResourceAllocationStatistic>& outTextureStats )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );

        outBufferStats.clear();
        outTextureStats.clear();

        for ( auto const& pair : pVulkanContext->m_bufferStats )
        {
            outBufferStats.push_back( { pair.first, pair.second.m_numAllocations, pair.second.m_numBytes } );
        }

        for ( auto const& pair : pVulkanContext->m_textureStats )
        {
            outTextureStats.push_back( { pair.first, pair.second.m_numAllocations, pair.second.m_numBytes } );
        }
    }

    void BeginFrameCapture( Context* pContext )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );

        if ( pVulkanContext->m_renderDoc && pVulkanContext->m_pRenderDocAPI != nullptr )
        {
            // For Vulkan this is the dispatch table pointer at the start of the VkInstance, not
            // the instance handle. The macro is what RenderDoc's header provides for it.
            pVulkanContext->m_pRenderDocAPI->StartFrameCapture( RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE( pVulkanContext->m_instance ), nullptr );
        }
    }

    void EndFrameCapture( Context* pContext )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );

        if ( pVulkanContext->m_renderDoc && pVulkanContext->m_pRenderDocAPI != nullptr )
        {
            pVulkanContext->m_pRenderDocAPI->EndFrameCapture( RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE( pVulkanContext->m_instance ), nullptr );
        }
    }

    //-------------------------------------------------------------------------
    // Debug names
    //-------------------------------------------------------------------------
    // The one call underneath all nine SetDebugName overloads.

    static void SetVulkanObjectName( VulkanContext* pVulkanContext, VkObjectType objectType, uint64_t objectHandle, StringView debugName )
    {
        if ( debugName.empty() )
        {
            return;
        }

        if ( pVulkanContext->m_vkSetDebugUtilsObjectName == nullptr )
        {
            return;
        }

        // A resource the device could not accept has a null handle, and naming VK_NULL_HANDLE is
        // a validation error. Guarded here rather than at every call site.
        if ( objectHandle == 0 )
        {
            return;
        }

        // StringView is not null terminated, and Vulkan wants a C string.
        TInlineString<MaxDebugNameLength> name( debugName.data(), debugName.size() );

        VkDebugUtilsObjectNameInfoEXT nameInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        nameInfo.objectType = objectType;
        nameInfo.objectHandle = objectHandle;
        nameInfo.pObjectName = name.c_str();

        pVulkanContext->m_vkSetDebugUtilsObjectName( pVulkanContext->m_device, &nameInfo );
    }

    //-------------------------------------------------------------------------
    // Queues and synchronization
    //-------------------------------------------------------------------------
    // RHI.h exposes a monotonic counter, which is a timeline semaphore here, one per queue.
    // m_nextSemaphoreValue is the value the next submit will signal, so QueueGetCurrentSemaphore
    // returns a value that has not been signalled yet. D3D12 runs its fence the same way, and the
    // engine is written against that.

    struct VulkanQueue final : Queue
    {
        // QueueGetCompletedSemaphore and QueueHostWait take no Context, so the queue has to
        // carry the device it belongs to.
        VkDevice                                            m_device = VK_NULL_HANDLE;
        VkQueue                                             m_queue = VK_NULL_HANDLE;
        VkSemaphore                                         m_timelineSemaphore = VK_NULL_HANDLE;
        uint64_t                                            m_nextSemaphoreValue = 1;
        uint32_t                                            m_queueFamilyIndex = ~0U;

        // GetQueryTimestampFrequency takes a Queue and no Context, and both numbers are per
        // family. Zero valid bits means the family cannot write a timestamp at all.
        float                                               m_timestampPeriod = 0.0F;
        uint32_t                                            m_timestampValidBits = 0;

        // Vulkan has no standalone queue wait. ID3D12CommandQueue::Wait blocks everything submitted
        // after it, and the only Vulkan construct with that meaning is a wait attached to a submit,
        // so QueueDeviceWait records it here and the next QueueSubmit drains it.
        //
        // An empty submit carrying only the wait would be wrong: submits on one queue may overlap,
        // so a wait in submit N does not hold back submit N+1.
        TInlineVector<VkSemaphoreSubmitInfo, 8>             m_pendingWaits;

        TVector<VkCommandBufferSubmitInfo>                  m_submitCommandBuffers{ Memory::Allocators::g_RHI };
    };

    struct VulkanCommandBuffer final : CommandBuffer
    {
        // Vulkan tracks the same lifecycle itself, but an assert names the caller that got it
        // wrong instead of a layer message three frames later.
        enum struct Stage
        {
            Invalid,
            Created,
            Recording,
            Closed
        };

        VkDevice                                            m_device = VK_NULL_HANDLE;
        VkCommandBuffer                                     m_commandBuffer = VK_NULL_HANDLE;
        Stage                                               m_stage = Stage::Invalid;

        // Dynamic rendering, deferred: CmdSetRenderTargets does not begin the pass, the first draw
        // does. The engine records image layout barriers between the two, and a barrier may not
        // run inside dynamic rendering.
        //
        // m_isRendering means a pass is open now; m_needsRenderingBegin means one is configured
        // and not yet open. The attachment configuration outlives both, so a pass left for a
        // barrier can be resumed by the next draw.
        bool                                                m_isRendering = false;
        bool                                                m_needsRenderingBegin = false;

        VkRenderingInfo                                     m_renderingInfo = {};
        TInlineVector<VkRenderingAttachmentInfo, MaxRenderTargets> m_colorAttachments;
        VkRenderingAttachmentInfo                           m_depthAttachment = {};
        VkRenderingAttachmentInfo                           m_stencilAttachment = {};
        bool                                                m_hasDepthAttachment = false;
        bool                                                m_hasStencilAttachment = false;

        // Copied from the pool this buffer came from. FlushBarriers clamps every stage mask to it.
        VkQueueFlags                                        m_queueFlags = 0;

        // One bit per set 0 parameter written since the last CmdSetPipeline. CmdExecuteIndirect
        // binds whatever is still clear, because a shader reached by an indirect draw statically
        // uses bindings no engine call fills. A direct draw gets no such help.
        uint32_t                                            m_boundRootParameterMask = 0;

        // Batched as the D3D12 backend batches them, and flushed at the same points. One
        // vkCmdPipelineBarrier2 also lets the driver see the whole set, and the flush is the
        // single place that has to leave the render pass.
        TVector<VkMemoryBarrier2>                           m_globalBarriers{ Memory::Allocators::g_RHI };
        TVector<VkBufferMemoryBarrier2>                     m_bufferBarriers{ Memory::Allocators::g_RHI };
        TVector<VkImageMemoryBarrier2>                      m_imageBarriers{ Memory::Allocators::g_RHI };

        // The pipeline layout of the currently bound pipeline. Push descriptors need it, and
        // CommandBuffer::m_pBoundPipeline only carries the platform-neutral pointer.
        VkPipelineLayout                                    m_boundPipelineLayout = VK_NULL_HANDLE;
        VkPipelineBindPoint                                 m_boundBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

        // True while the bound pipeline has no VkPipeline, which is a mesh pipeline on a device
        // without VK_EXT_mesh_shader. Every draw against it is dropped. See CmdSetPipeline.
        bool                                                m_boundPipelineIsNull = false;

        // Root constants are not Vulkan push constants; see CmdSetRootConstants. Each set is
        // copied into this ring and a descriptor pushed at the copy. Reset in BeginCommandBuffer,
        // which is safe because Vulkan already requires the previous submission to have completed.
        Buffer*                                             m_pRootConstantRing = nullptr;
        uint64_t                                            m_rootConstantRingOffset = 0;

        // CmdSetRootConstants and CmdSetRootParameter take no Context, so the entry point comes
        // along on the command buffer.
        PFN_vkCmdPushDescriptorSetKHR                       m_vkCmdPushDescriptorSet = nullptr;

        // Debug markers, which take no Context either. Both are null when VK_EXT_debug_utils is
        // absent, and every marker call then does nothing.
        PFN_vkCmdBeginDebugUtilsLabelEXT                    m_vkCmdBeginDebugUtilsLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT                      m_vkCmdEndDebugUtilsLabel = nullptr;

        // Mesh shader draws, same reason. Null when VK_EXT_mesh_shader is absent, and every one
        // of them asserts rather than doing nothing: the engine has no fallback path.
        PFN_vkCmdDrawMeshTasksEXT                           m_vkCmdDrawMeshTasks = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectEXT                   m_vkCmdDrawMeshTasksIndirect = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectCountEXT              m_vkCmdDrawMeshTasksIndirectCount = nullptr;

        // m_shadingRateCaps is copied from DeviceCapabilities, and CmdSetShadingRate guards on it
        // as the D3D12 backend does.
        PFN_vkCmdSetFragmentShadingRateKHR                  m_vkCmdSetFragmentShadingRate = nullptr;
        TBitFlags<ShadingRateCaps>                          m_shadingRateCaps = {};

        // Raytracing, same reason again. Null when the three extensions are absent.
        PFN_vkCmdBuildAccelerationStructuresKHR             m_vkCmdBuildAccelerationStructures = nullptr;
        PFN_vkCmdTraceRaysKHR                               m_vkCmdTraceRays = nullptr;
        PFN_vkCmdTraceRaysIndirect2KHR                      m_vkCmdTraceRaysIndirect2 = nullptr;

        // D3D12 binds the shading rate image with a command and Vulkan makes it a render pass
        // attachment, so CmdSetShadingRate can only record it and BeginRenderingIfPending chains it.
        VkImageView                                         m_shadingRateImageView = VK_NULL_HANDLE;
        // DeviceCapabilities::m_shadingRateTexelWidth and Height, copied at creation.
        VkExtent2D                                          m_shadingRateTexelSize = {};

        // The same starting value the D3D12 backend uses, so a marker gets the same colour on both.
        float                                               m_currentDebugMarkerColorValue = 0.5F;
        int32_t                                             m_debugMarkerScopeCounter = 0;
    };

    //-------------------------------------------------------------------------
    // Resource types
    //-------------------------------------------------------------------------
    // Gathered here because RHI.h declares the draw commands before the buffers, textures and
    // pipelines they act on. The functions keep RHI.h's order; only the types move.

    struct VulkanCommandPool final : CommandPool
    {
        VkCommandPool                                       m_commandPool = VK_NULL_HANDLE;

        // The pool's queue family decides what may be recorded into it. A transfer-only or
        // compute-only family rejects any graphics command, and any barrier naming a graphics
        // stage. D3D12 has no equivalent restriction.
        VkQueueFlags                                        m_queueFlags = 0;

        // Destroying a VkCommandPool frees every buffer allocated from it, and a later
        // vkFreeCommandBuffers on the dead pool is a validation error. D3D12 has no such rule, and
        // Window::DestroySwapchain destroys pools before buffers. So the pool tracks its buffers
        // and clears their handles on the way out.
        TInlineVector<VulkanCommandBuffer*, 8>              m_allocatedCommandBuffers;
    };

    struct VulkanQueryPool final : QueryPool
    {
        VkQueryPool                                         m_queryPool = VK_NULL_HANDLE;
        VkQueryType                                         m_queryType = VK_QUERY_TYPE_TIMESTAMP;
    };

    // One RHI acceleration structure is a bottom level and a top level together. Vulkan splits each
    // into a handle and the buffer that holds it, so both are here.
    struct VulkanAccelerationStructure final : AccelerationStructure
    {
        struct Level
        {
            VkAccelerationStructureKHR                      m_handle = VK_NULL_HANDLE;
            Buffer*                                         m_pStructureBuffer = nullptr;
            VkBuildAccelerationStructureFlagsKHR            m_flags = 0;
        };

        Level                                               m_bottomLevel = {};
        Level                                               m_topLevel = {};

        // These outlive creation, because CmdBuildAccelerationStructure hands the same array to
        // the build.
        TVector<VkAccelerationStructureGeometryKHR>         m_geometries{ Memory::Allocators::g_RHI };
        TVector<VkAccelerationStructureBuildRangeInfoKHR>   m_buildRanges{ Memory::Allocators::g_RHI };

        // Shared by both builds, sized for the larger of the two.
        Buffer*                                             m_pScratchBuffer = nullptr;

        Buffer*                                             m_pInstanceBuffer = nullptr;
        uint64_t                                            m_instanceBufferOffset = 0;
        uint64_t                                            m_numInstances = 0;

        // What VkAccelerationStructureInstanceKHR::accelerationStructureReference wants for a
        // bottom level. D3D12 uses a plain GPU virtual address instead.
        VkDeviceAddress                                     m_bottomLevelDeviceAddress = 0;
    };

    // Nothing creates one of these on either backend: RHI.h declares no factory for a
    // RaytracingShaderTable, so CmdDispatchRays is unreachable. The fields mirror the D3D12 type
    // so both sides have the same shape to fill in.
    struct VulkanRaytracingShaderTable final : RaytracingShaderTable
    {
        Buffer*                                             m_pBuffer = nullptr;
        uint64_t                                            m_maxEntrySize = 0;
        uint64_t                                            m_missRecordSize = 0;
        uint64_t                                            m_hitGroupRecordSize = 0;
    };

    struct VulkanBuffer final : Buffer
    {
        VkBuffer                                            m_buffer = VK_NULL_HANDLE;
        VmaAllocation                                       m_allocation = VK_NULL_HANDLE;
        uint64_t                                            m_allocationSize = 0;

        // Typed buffers, Buffer<T> and RWBuffer<T>, are texel buffers in Vulkan and a texel
        // buffer descriptor takes a view rather than a range. Null for structured and raw
        // buffers, which take the buffer directly.
        VkBufferView                                        m_uniformTexelBufferView = VK_NULL_HANDLE;
        VkBufferView                                        m_storageTexelBufferView = VK_NULL_HANDLE;

        // BufferFlags::SubAllocations, the equivalent of the D3D12 backend's virtual block.
        VmaVirtualBlock                                     m_virtualBlock = VK_NULL_HANDLE;

        // A contiguous run in the resource heap, laid out as D3D12 lays it out: constant buffer
        // first if present, then the read view, then the read-write view.
        HandleAllocator<GenericResourceHandle>::Handle      m_descriptorHandles = {};
        int8_t                                              m_srvDescriptorOffset = -1;
        int8_t                                              m_uavDescriptorOffset = -1;

        ReadRange                                           m_mappedRange = {};
    };

    struct VulkanTexture final : Texture
    {
        VkImage                                             m_image = VK_NULL_HANDLE;
        VmaAllocation                                       m_allocation = VK_NULL_HANDLE;
        uint64_t                                            m_allocationSize = 0;

        // Not m_format: Texture::m_format is the DataFormat the caller asked for, and a member of
        // the same name here would hide it behind a different type.
        VkFormat                                            m_vulkanFormat = VK_FORMAT_UNDEFINED;
        VkExtent3D                                          m_extent = {};

        // Every aspect the image has: colour, or depth and stencil. A view picks a subset.
        VkImageAspectFlags                                  m_aspectMask = 0;

        // False when the image belongs to somebody else: a swapchain image, or the texture this
        // one aliases. The views are still ours, the image is not.
        bool                                                m_ownsImage = true;

        // vkCreateImage only accepts UNDEFINED or PREINITIALIZED, whatever
        // TextureParameters::m_initialState says, so the engine believes the texture is already in
        // m_initialState and the image is not. Barriers transition from what is recorded here
        // rather than from the state the caller passes.
        //
        // One layout per subresource, not one per image: a cubemap capture draws each face in
        // turn, so face 1 is a colour attachment while face 0 is already sampled. Indexed
        // mipLevel * m_arrayLayers + arrayLayer.
        TInlineVector<VkImageLayout, 6>                     m_subresourceLayouts;

        // UNDEFINED when the subresources disagree. Only the barrier path needs the distinction.
        VkImageLayout CurrentLayout( uint32_t mipLevel = 0, uint32_t arrayLayer = 0 ) const
        {
            uint32_t const index = mipLevel * m_arrayLayers + arrayLayer;
            return ( index < m_subresourceLayouts.size() ) ? m_subresourceLayouts[index] : VK_IMAGE_LAYOUT_UNDEFINED;
        }

        void SetLayout( VkImageLayout layout, uint32_t baseMip, uint32_t numMips, uint32_t baseLayer, uint32_t numLayers )
        {
            for ( uint32_t mip = baseMip; mip < baseMip + numMips; ++mip )
            {
                for ( uint32_t layer = baseLayer; layer < baseLayer + numLayers; ++layer )
                {
                    uint32_t const index = mip * m_arrayLayers + layer;
                    if ( index < m_subresourceLayouts.size() )
                    {
                        m_subresourceLayouts[index] = layout;
                    }
                }
            }
        }

        // The layout the sampled-image descriptor was written with, so the layout a barrier has to
        // reach before a shader reads it. GENERAL for a texture that is also an RWTexture, because
        // one image cannot be in two layouts and a storage image descriptor has to say GENERAL.
        VkImageLayout                                       m_shaderReadLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // The view a shader samples, covering every mip and layer. One per texture.
        VkImageView                                         m_shaderResourceView = VK_NULL_HANDLE;

        // One storage view per mip level, because an RWTexture handle names a mip level.
        TVector<VkImageView>                                m_storageViews{ Memory::Allocators::g_RHI };

        // One attachment view per subresource, indexed as D3D12 indexes its render target
        // descriptors: m_mipLevels * arrayLayer + mipLevel.
        TVector<VkImageView>                                m_renderTargetViews{ Memory::Allocators::g_RHI };

        // A contiguous run in the resource heap, in D3D12's order: the read view first if present,
        // then one read-write view per mip level.
        HandleAllocator<GenericResourceHandle>::Handle      m_descriptorHandles = {};
        int8_t                                              m_uavDescriptorOffset = -1;

        // DeviceCapabilities::m_uploadBufferTextureRowAlignment, copied here because
        // GetTextureCopyRowStride takes no Context.
        uint32_t                                            m_copyRowAlignment = 1;

        VkImageView RenderTargetView( uint32_t arrayLayer, uint32_t mipLevel ) const
        {
            uint32_t const viewIndex = m_mipLevels * arrayLayer + mipLevel;
            EE_ASSERT( viewIndex < m_renderTargetViews.size() );
            return m_renderTargetViews[viewIndex];
        }
    };

    struct VulkanSampler final : Sampler
    {
        VkSampler                                           m_sampler = VK_NULL_HANDLE;

        // One slot in the sampler heap, set 1 binding 1.
        HandleAllocator<GenericResourceHandle>::Handle      m_descriptorHandle = {};
    };

    struct VulkanShader final : Shader
    {
        VkDevice                                            m_device = VK_NULL_HANDLE;
        TInlineVector<VkShaderModule, 2>                    m_shaderModules;
    };

    struct VulkanRootSignature final : RootSignature
    {
        VkDevice                                            m_device = VK_NULL_HANDLE;
        VkPipelineLayout                                    m_pipelineLayout = VK_NULL_HANDLE;

        // Set 0 of the binding model: per-pipeline, derived from reflection, and created with
        // PUSH_DESCRIPTOR_BIT so CmdSetRootParameter is a push rather than a set allocation.
        VkDescriptorSetLayout                               m_rootParameterSetLayout = VK_NULL_HANDLE;

        // Kept so CmdExecuteIndirect can bind the ones the engine left alone. Same order as
        // m_descriptorReflections, so a parameter index indexes both.
        TInlineVector<VkDescriptorSetLayoutBinding, 32>     m_rootParameterBindings;

        // Copied from the context so CmdSetPipeline, which takes no Context, can bind it.
        VkDescriptorSet                                     m_heapDescriptorSet = VK_NULL_HANDLE;
    };

    struct VulkanPipeline final : Pipeline
    {
        VkDevice                                            m_device = VK_NULL_HANDLE;
        VkPipeline                                          m_pipeline = VK_NULL_HANDLE;
        VkPipelineBindPoint                                 m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkPrimitiveTopology                                 m_primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    };

    struct VulkanPipelineCache final : PipelineCache
    {
        VkDevice                                            m_device = VK_NULL_HANDLE;
        VkPipelineCache                                     m_pipelineCache = VK_NULL_HANDLE;

        // GetPipelineCacheData hands back a view, so the bytes have to outlive the call.
        TVector<uint8_t>                                    m_cacheData{ Memory::Allocators::g_RHI };
    };

    // Enough for every root constant set in one command buffer, by a wide margin. Asserted rather
    // than wrapped: wrapping would overwrite constants the GPU is still reading, and the failure
    // would look like a shader bug.
    static constexpr uint64_t g_rootConstantRingSize = 64 * 1024;
    static constexpr uint64_t g_rootConstantAlignment = 256;

    //-------------------------------------------------------------------------

    Queue* CreateQueue( Context* pContext, QueueParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanQueue* pVulkanQueue = pVulkanContext->CreateObject<VulkanQueue>();

        // Vulkan has no linked-node adapter, so CreateContext always reports a single node.
        EE_ASSERT( parameters.m_nodeIndex == 0 );

        switch ( parameters.m_queueType )
        {
            case QueueType::Graphics: pVulkanQueue->m_queueFamilyIndex = pVulkanContext->m_graphicsQueueFamily; break;
            case QueueType::Compute:  pVulkanQueue->m_queueFamilyIndex = pVulkanContext->m_computeQueueFamily; break;
            case QueueType::Transfer: pVulkanQueue->m_queueFamilyIndex = pVulkanContext->m_transferQueueFamily; break;
        }

        EE_ASSERT( pVulkanQueue->m_queueFamilyIndex != ~0U );

        // Hand out a distinct VkQueue while the family has one left, then repeat the last. Repeating
        // is legal, and CreateContext already asked for as many as the family allows.
        uint32_t queueIndex = 0;
        for ( VulkanContext::QueueFamilyAllocation& allocation : pVulkanContext->m_queueFamilyAllocations )
        {
            if ( allocation.m_familyIndex != pVulkanQueue->m_queueFamilyIndex )
            {
                continue;
            }

            queueIndex = Math::Min( allocation.m_nextQueueIndex, allocation.m_numQueues - 1 );
            allocation.m_nextQueueIndex++;
            break;
        }

        vkGetDeviceQueue( pVulkanContext->m_device, pVulkanQueue->m_queueFamilyIndex, queueIndex, &pVulkanQueue->m_queue );
        EE_ASSERT( pVulkanQueue->m_queue != VK_NULL_HANDLE );

        // Both numbers belong to the queue family, and GetQueryTimestampFrequency is handed a
        // Queue with no Context to ask.
        pVulkanQueue->m_timestampPeriod = pVulkanContext->m_physicalDeviceProperties.limits.timestampPeriod;

        uint32_t numQueueFamilies = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, nullptr );
        TVector<VkQueueFamilyProperties> queueFamilyProperties( numQueueFamilies );
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, queueFamilyProperties.data() );

        pVulkanQueue->m_timestampValidBits = queueFamilyProperties[pVulkanQueue->m_queueFamilyIndex].timestampValidBits;

        VkSemaphoreTypeCreateInfo semaphoreTypeCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        semaphoreTypeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        semaphoreTypeCreateInfo.initialValue = 0;

        VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        semaphoreCreateInfo.pNext = &semaphoreTypeCreateInfo;

        [[maybe_unused]] VkResult const result = vkCreateSemaphore( pVulkanContext->m_device, &semaphoreCreateInfo, nullptr, &pVulkanQueue->m_timelineSemaphore );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_QUEUE, uint64_t( pVulkanQueue->m_queue ), parameters.m_debugName );

        // QueuePriority is not honoured. Vulkan fixes priorities at vkCreateDevice, so honouring it
        // would need the device recreated, and nothing in the engine asks for anything but Normal.
        // QueueFlags::DisableTimeout has no Vulkan equivalent at all, and nothing sets it either.

        pVulkanQueue->m_device = pVulkanContext->m_device;
        pVulkanQueue->m_queueType = parameters.m_queueType;
        pVulkanQueue->m_nodeIndex = parameters.m_nodeIndex;
        pVulkanQueue->m_unifiedMemory = pVulkanContext->m_isUnifiedMemory;

        return pVulkanQueue;
    }

    void DestroyQueue( Context* pContext, Queue*&& pQueue )
    {
        if ( pQueue != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

            if ( pVulkanQueue->m_timelineSemaphore != VK_NULL_HANDLE )
            {
                vkDestroySemaphore( pVulkanContext->m_device, pVulkanQueue->m_timelineSemaphore, nullptr );
            }

            // The VkQueue itself is owned by the device and is not destroyed.

            pVulkanContext->DestroyObject( eastl::move( pVulkanQueue ) );
            pQueue = nullptr;
        }
    }

    uint64_t QueueGetCurrentSemaphore( Queue* pQueue )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );
        return pVulkanQueue->m_nextSemaphoreValue;
    }

    uint64_t QueueGetCompletedSemaphore( Queue* pQueue )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        uint64_t completedValue = 0;
        [[maybe_unused]] VkResult const result = vkGetSemaphoreCounterValue( pVulkanQueue->m_device, pVulkanQueue->m_timelineSemaphore, &completedValue );
        EE_ASSERT( result == VK_SUCCESS );

        return completedValue;
    }

    void QueueHostWait( Queue* pQueue, uint64_t semaphore )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        // Value 0 is the timeline's initial value, so it is always already satisfied.
        if ( semaphore == 0 )
        {
            return;
        }

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &pVulkanQueue->m_timelineSemaphore;
        waitInfo.pValues = &semaphore;

        [[maybe_unused]] VkResult const result = vkWaitSemaphores( pVulkanQueue->m_device, &waitInfo, UINT64_MAX );
        EE_ASSERT( result == VK_SUCCESS );
    }

    void QueueDeviceWait( Queue* pQueueThatWaits, Queue* pQueueToWaitFor, uint64_t semaphore )
    {
        EE_ASSERT( pQueueThatWaits != pQueueToWaitFor );

        VulkanQueue* pVulkanQueueThatWaits = static_cast<VulkanQueue*>( pQueueThatWaits );
        VulkanQueue* pVulkanQueueToWaitFor = static_cast<VulkanQueue*>( pQueueToWaitFor );

        if ( semaphore == 0 )
        {
            return;
        }

        EE_ASSERT( semaphore < pVulkanQueueToWaitFor->m_nextSemaphoreValue );

        VkSemaphoreSubmitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        waitInfo.semaphore = pVulkanQueueToWaitFor->m_timelineSemaphore;
        waitInfo.value = semaphore;
        // ALL_COMMANDS, because ID3D12CommandQueue::Wait blocks the whole queue and this has to
        // mean the same. Narrowing it needs to know what the waiting submit will do, and the
        // caller does not say.
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        pVulkanQueueThatWaits->m_pendingWaits.emplace_back( waitInfo );
    }

    // A D3D12 queue runs its command lists in submission order and a Vulkan queue does not: two
    // vkQueueSubmit2 calls on one VkQueue may overlap. Nothing in RHI.h can ask for that ordering,
    // because QueueDeviceWait asserts the two queues differ, and the engine relies on it when it
    // submits several graphics command buffers a frame with the barriers recorded across them.
    //
    // So every submit waits on the value the previous submit signalled. That also keeps the
    // swapchain sound: the acquire wait the first submit carries holds back every later one too.
    // ALL_COMMANDS on both ends, because "the previous submit finished" is the whole meaning.
    static void RecordQueueOrderingWait( VulkanQueue* pVulkanQueue )
    {
        uint64_t const previousValue = pVulkanQueue->m_nextSemaphoreValue - 1;

        // Zero is the timeline's initial value, so the first submit has nothing to wait for.
        if ( previousValue == 0 )
        {
            return;
        }

        VkSemaphoreSubmitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        waitInfo.semaphore = pVulkanQueue->m_timelineSemaphore;
        waitInfo.value = previousValue;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        pVulkanQueue->m_pendingWaits.emplace_back( waitInfo );
    }

    // Shared by QueueSubmit and the submit QueuePresent has to make first. VkPresentInfoKHR cannot
    // wait on a timeline, so a present needs a binary semaphore signalled next to the timeline
    // value. VK_NULL_HANDLE for an ordinary submit.
    static uint64_t SubmitToQueue( VulkanQueue* pVulkanQueue, TArrayView<CommandBuffer*> commandBuffers, VkSemaphore binarySignalSemaphore )
    {
        pVulkanQueue->m_submitCommandBuffers.clear();
        pVulkanQueue->m_submitCommandBuffers.reserve( commandBuffers.size() );

        for ( CommandBuffer* pCommandBuffer : commandBuffers )
        {
            VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

            VkCommandBufferSubmitInfo commandBufferSubmitInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            commandBufferSubmitInfo.commandBuffer = pVulkanCommandBuffer->m_commandBuffer;
            pVulkanQueue->m_submitCommandBuffers.emplace_back( commandBufferSubmitInfo );
        }

        RecordQueueOrderingWait( pVulkanQueue );

        uint64_t const signalSemaphore = pVulkanQueue->m_nextSemaphoreValue++;

        TInlineVector<VkSemaphoreSubmitInfo, 2> signalInfos;

        VkSemaphoreSubmitInfo timelineSignalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        timelineSignalInfo.semaphore = pVulkanQueue->m_timelineSemaphore;
        timelineSignalInfo.value = signalSemaphore;
        timelineSignalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signalInfos.emplace_back( timelineSignalInfo );

        if ( binarySignalSemaphore != VK_NULL_HANDLE )
        {
            VkSemaphoreSubmitInfo binarySignalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            binarySignalInfo.semaphore = binarySignalSemaphore;
            // A binary semaphore carries no value.
            binarySignalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            signalInfos.emplace_back( binarySignalInfo );
        }

        VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.waitSemaphoreInfoCount = uint32_t( pVulkanQueue->m_pendingWaits.size() );
        submitInfo.pWaitSemaphoreInfos = pVulkanQueue->m_pendingWaits.data();
        submitInfo.commandBufferInfoCount = uint32_t( pVulkanQueue->m_submitCommandBuffers.size() );
        submitInfo.pCommandBufferInfos = pVulkanQueue->m_submitCommandBuffers.data();
        submitInfo.signalSemaphoreInfoCount = uint32_t( signalInfos.size() );
        submitInfo.pSignalSemaphoreInfos = signalInfos.data();

        // D3D12 skips ExecuteCommandLists on an empty list but still signals, which is what an
        // empty submit does here.
        [[maybe_unused]] VkResult const result = vkQueueSubmit2( pVulkanQueue->m_queue, 1, &submitInfo, VK_NULL_HANDLE );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanQueue->m_pendingWaits.clear();

        return signalSemaphore;
    }

    uint64_t QueueSubmit( Queue* pQueue, TArrayView<CommandBuffer*> commandBuffers )
    {
        return SubmitToQueue( static_cast<VulkanQueue*>( pQueue ), commandBuffers, VK_NULL_HANDLE );
    }

    //-------------------------------------------------------------------------
    // Swapchain and presentation
    //-------------------------------------------------------------------------
    // SwapchainParameters::m_pNativeWindowHandle is an SDL_Window* on Linux, and this file makes
    // the VkSurfaceKHR from it:
    //  - Base/Render must not depend on a window system library, so the surface is made by
    //    Platform::Linux::CreateVulkanSurface and the handle crosses as a void*.
    //  - A null handle means headless. The swapchain is then a ring of offscreen render targets
    //    with no VkSwapchainKHR, and QueuePresent signals its timeline value and presents nothing.
    //  - The engine drives recreation through Window::ResizeSwapchain, so this file tolerates
    //    VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR rather than recreating behind its back.

    struct VulkanSwapchain final : Swapchain
    {
        VkDevice                                            m_device = VK_NULL_HANDLE;

        // Remade with the swapchain, because Window::ResizeSwapchain destroys and recreates around
        // an unchanged window. SDL_Vulkan_CreateSurface is cheap.
        VkSurfaceKHR                                        m_surface = VK_NULL_HANDLE;
        VkSwapchainKHR                                      m_swapchain = VK_NULL_HANDLE;

        // SwapchainParameters::m_presentQueues[0]. AcquireNextImage takes no Queue and has to
        // put its wait somewhere, so the swapchain remembers which queue presents it.
        VulkanQueue*                                        m_pPresentQueue = nullptr;

        uint32_t                                            m_numImages = 0;
        uint32_t                                            m_currentImageIndex = 0;

        // Every present mode the surface supports, kept because SetVSync takes no Context and
        // has to choose from them.
        TInlineVector<VkPresentModeKHR, 8>                  m_supportedPresentModes;
        VkPresentModeKHR                                    m_presentMode = VK_PRESENT_MODE_FIFO_KHR;

        // Binary semaphores, because VkPresentInfoKHR has no timeline path. The acquire semaphores
        // are a ring rather than one per image, because vkAcquireNextImageKHR is told which
        // semaphore to signal before it says which image it gave. The present semaphores are one
        // per image, which is safe because an image is not presented again until it is acquired again.
        TInlineVector<VkSemaphore, MaxPendingFrames>        m_acquireSemaphores;
        TInlineVector<VkSemaphore, MaxPendingFrames>        m_presentSemaphores;
        uint32_t                                            m_nextAcquireSemaphore = 0;

        bool                                                m_isHeadless = false;
    };

    uint64_t QueuePresent( Queue* pQueue, Swapchain* pSwapchain, uint32_t imageIndex )
    {
        VulkanQueue*      pVulkanQueue = static_cast<VulkanQueue*>( pQueue );
        VulkanSwapchain*  pVulkanSwapchain = static_cast<VulkanSwapchain*>( pSwapchain );

        EE_ASSERT( imageIndex == pVulkanSwapchain->m_currentImageIndex );

        if ( pVulkanSwapchain->m_isHeadless )
        {
            // Nothing to present to. D3D12 signals its fence after the present and returns that
            // value, and so does this, so the engine's frame pacing is unchanged.
            return SubmitToQueue( pVulkanQueue, {}, VK_NULL_HANDLE );
        }

        VkSemaphore const presentSemaphore = pVulkanSwapchain->m_presentSemaphores[imageIndex];

        // The submit comes first where D3D12 presents first and signals after, because
        // vkQueuePresentKHR waits on a semaphore only a submit can signal. The returned value
        // still means "the frame is done", which is all the engine reads it for.
        uint64_t const signalSemaphore = SubmitToQueue( pVulkanQueue, {}, presentSemaphore );

        VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &presentSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &pVulkanSwapchain->m_swapchain;
        presentInfo.pImageIndices = &imageIndex;

        [[maybe_unused]] VkResult const result = vkQueuePresentKHR( pVulkanQueue->m_queue, &presentInfo );

        // Neither of these is an error here. The engine resizes the swapchain itself, from the
        // window size, so a stale swapchain is already on its way to being replaced.
        EE_ASSERT( result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR );

        return signalSemaphore;
    }

    void WaitQueueIdle( Queue* pQueue )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        // D3D12 signals its fence and blocks on an event, because it has no queue-idle
        // call. Vulkan has one, and it means precisely this.
        [[maybe_unused]] VkResult const result = vkQueueWaitIdle( pVulkanQueue->m_queue );
        EE_ASSERT( result == VK_SUCCESS );
    }

    // The same textures on both paths. On the real path they wrap images the presentation engine
    // owns, which is what TextureParameters::m_pNativeHandle is for; headless they are ordinary
    // textures this call allocates.
    static void CreateSwapchainRenderTargets( Context* pContext, VulkanSwapchain* pVulkanSwapchain, SwapchainParameters const& parameters, DataFormat renderTargetFormat, VkExtent2D extent, TArrayView<VkImage const> images )
    {
        TextureParameters textureParameters = {};
        // The surface's extent, not the one that was asked for. Wayland usually decides the size
        // itself, and the texture has to describe the image that was actually made.
        textureParameters.m_width = extent.width;
        textureParameters.m_height = extent.height;
        textureParameters.m_depth = 1;
        textureParameters.m_arrayLayers = 1;
        textureParameters.m_format = renderTargetFormat;
        textureParameters.m_clearValue = parameters.m_clearValue;
        textureParameters.m_numSamples = 1;
        textureParameters.m_sampleQuality = 0;
        textureParameters.m_textureFlags = TextureFlags::AllowDisplayTarget;
        textureParameters.m_descriptorTypes = DescriptorTypeFlags::RenderTarget;
        textureParameters.m_initialState = TextureState::Present;

        for ( uint32_t imageIndex = 0; imageIndex < pVulkanSwapchain->m_numImages; ++imageIndex )
        {
            textureParameters.m_pNativeHandle = images.empty() ? nullptr : images[imageIndex];
            textureParameters.m_debugName.sprintf( "Swapchain Render Target %i", imageIndex );

            pVulkanSwapchain->m_renderTargets[imageIndex] = CreateTexture( pContext, textureParameters );
        }
    }

    Swapchain* CreateSwapchain( Context* pContext, SwapchainParameters const& parameters )
    {
        VulkanContext*   pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanSwapchain* pVulkanSwapchain = pVulkanContext->CreateObject<VulkanSwapchain>();

        // Vulkan presents from one queue, and the engine only ever passes one.
        EE_ASSERT( parameters.m_presentQueues.size() == 1 );

        pVulkanSwapchain->m_device = pVulkanContext->m_device;
        pVulkanSwapchain->m_pPresentQueue = static_cast<VulkanQueue*>( parameters.m_presentQueues[0] );
        pVulkanSwapchain->m_surface = static_cast<VkSurfaceKHR>( Platform::Linux::CreateVulkanSurface( pVulkanContext->m_instance, parameters.m_pNativeWindowHandle ) );
        pVulkanSwapchain->m_isHeadless = pVulkanSwapchain->m_surface == VK_NULL_HANDLE;
        pVulkanSwapchain->m_numImages = parameters.m_numImages;

        // The engine holds the render targets in a fixed array of MaxPendingFrames, so this is
        // not a soft limit.
        EE_ASSERT( pVulkanSwapchain->m_numImages <= MaxPendingFrames );

        if ( pVulkanSwapchain->m_isHeadless )
        {
            CreateSwapchainRenderTargets( pContext, pVulkanSwapchain, parameters, parameters.m_renderTargetFormat, { parameters.m_width, parameters.m_height }, {} );

            // The first acquire returns image 0, the way the real path's first acquire does.
            pVulkanSwapchain->m_currentImageIndex = pVulkanSwapchain->m_numImages - 1;

            SetVSync( pVulkanSwapchain, parameters.m_enableVSync );
            return pVulkanSwapchain;
        }

        // Surface format
        //-------------------------------------------------------------------------
        // D3D12 creates the swapchain UNorm and puts an sRGB view on it; Vulkan creates the image
        // sRGB and the view matches. Same conversion, same picture, and no need for
        // VK_KHR_swapchain_mutable_format. So m_renderTargetFormat drives both the image and the
        // views, and m_colorFormat is the fallback when the surface refuses it.
        uint32_t numSurfaceFormats = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &numSurfaceFormats, nullptr );
        TVector<VkSurfaceFormatKHR> surfaceFormats( numSurfaceFormats );
        vkGetPhysicalDeviceSurfaceFormatsKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &numSurfaceFormats, surfaceFormats.data() );

        DataFormat         renderTargetFormat = parameters.m_renderTargetFormat;
        VkSurfaceFormatKHR chosenSurfaceFormat = {};

        // Linux surfaces offer only the BGRA spellings, so those are candidates too, or the assert
        // below fires on every Linux machine. A Vulkan format names its components in memory order,
        // so a BGRA swapchain displays exactly as an RGBA one does. The sRGB spelling of each pair
        // comes first, which preserves the caller's preference.
        for ( DataFormat const candidate : { SubstituteSwapchainColorFormat( parameters.m_renderTargetFormat ),
                                             SubstituteSwapchainColorFormat( parameters.m_colorFormat ),
                                             parameters.m_renderTargetFormat, parameters.m_colorFormat } )
        {
            VkFormat const candidateVulkanFormat = VulkanFormat( candidate );

            for ( VkSurfaceFormatKHR const& surfaceFormat : surfaceFormats )
            {
                if ( surfaceFormat.format == candidateVulkanFormat )
                {
                    renderTargetFormat = candidate;
                    chosenSurfaceFormat = surfaceFormat;
                    break;
                }
            }

            if ( chosenSurfaceFormat.format != VK_FORMAT_UNDEFINED )
            {
                break;
            }
        }

        // The image and its views have to agree on one format, so a surface that offers neither
        // is a real incompatibility rather than something to paper over.
        EE_ASSERT( chosenSurfaceFormat.format != VK_FORMAT_UNDEFINED );

        // Extent, image count and present mode
        //-------------------------------------------------------------------------
        VkSurfaceCapabilitiesKHR surfaceCapabilities = {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &surfaceCapabilities );

        VkExtent2D extent = { parameters.m_width, parameters.m_height };
        if ( surfaceCapabilities.currentExtent.width != UINT32_MAX )
        {
            // The window system has already decided, which is the usual case on Wayland.
            extent = surfaceCapabilities.currentExtent;
        }
        extent.width = Math::Clamp( extent.width, surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width );
        extent.height = Math::Clamp( extent.height, surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height );

        uint32_t numRequestedImages = Math::Max( parameters.m_numImages, surfaceCapabilities.minImageCount );
        if ( surfaceCapabilities.maxImageCount != 0 )
        {
            numRequestedImages = Math::Min( numRequestedImages, surfaceCapabilities.maxImageCount );
        }

        uint32_t numPresentModes = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &numPresentModes, nullptr );
        TVector<VkPresentModeKHR> presentModes( numPresentModes );
        vkGetPhysicalDeviceSurfacePresentModesKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &numPresentModes, presentModes.data() );

        for ( VkPresentModeKHR const presentMode : presentModes )
        {
            pVulkanSwapchain->m_supportedPresentModes.emplace_back( presentMode );
        }

        SetVSync( pVulkanSwapchain, parameters.m_enableVSync );

        // Swapchain
        //-------------------------------------------------------------------------
        VkSwapchainCreateInfoKHR swapchainCreateInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        swapchainCreateInfo.surface = pVulkanSwapchain->m_surface;
        swapchainCreateInfo.minImageCount = numRequestedImages;
        swapchainCreateInfo.imageFormat = chosenSurfaceFormat.format;
        swapchainCreateInfo.imageColorSpace = chosenSurfaceFormat.colorSpace;
        swapchainCreateInfo.imageExtent = extent;
        swapchainCreateInfo.imageArrayLayers = 1;
        // The transfer bits only when the surface allows them, because a copy or a clear can name
        // any texture and a swapchain image is one.
        swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         ( surfaceCapabilities.supportedUsageFlags & ( VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT ) );
        // One queue presents and one queue renders, and they are the same queue. See
        // SetSharingMode for why every other resource is CONCURRENT instead.
        swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
        swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchainCreateInfo.presentMode = pVulkanSwapchain->m_presentMode;
        swapchainCreateInfo.clipped = VK_TRUE;

        [[maybe_unused]] VkResult result = vkCreateSwapchainKHR( pVulkanContext->m_device, &swapchainCreateInfo, nullptr, &pVulkanSwapchain->m_swapchain );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_SWAPCHAIN_KHR, uint64_t( pVulkanSwapchain->m_swapchain ), "Swapchain" );

        // Images
        //-------------------------------------------------------------------------
        // minImageCount is a minimum, so the driver may hand back more images than were asked for,
        // and several Linux drivers want three or four. If this halts, the fix is MaxPendingFrames
        // in RHI.h, which is an upstream change.
        uint32_t numImages = 0;
        vkGetSwapchainImagesKHR( pVulkanContext->m_device, pVulkanSwapchain->m_swapchain, &numImages, nullptr );

        if ( numImages > MaxPendingFrames )
        {
            EE_LOG_ERROR( LogCategory::Render, "RHI/CreateSwapchain", "The surface needs %u swapchain images and RHI::MaxPendingFrames is %u.", numImages, uint32_t( MaxPendingFrames ) );
        }
        EE_ASSERT( numImages <= MaxPendingFrames );

        TInlineVector<VkImage, MaxPendingFrames> images;
        images.resize( numImages );
        vkGetSwapchainImagesKHR( pVulkanContext->m_device, pVulkanSwapchain->m_swapchain, &numImages, images.data() );

        pVulkanSwapchain->m_numImages = numImages;

        CreateSwapchainRenderTargets( pContext, pVulkanSwapchain, parameters, renderTargetFormat, extent, { images.data(), images.size() } );

        // Semaphores
        //-------------------------------------------------------------------------
        VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        for ( uint32_t imageIndex = 0; imageIndex < numImages; ++imageIndex )
        {
            VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
            result = vkCreateSemaphore( pVulkanContext->m_device, &semaphoreCreateInfo, nullptr, &acquireSemaphore );
            EE_ASSERT( result == VK_SUCCESS );
            pVulkanSwapchain->m_acquireSemaphores.emplace_back( acquireSemaphore );

            VkSemaphore presentSemaphore = VK_NULL_HANDLE;
            result = vkCreateSemaphore( pVulkanContext->m_device, &semaphoreCreateInfo, nullptr, &presentSemaphore );
            EE_ASSERT( result == VK_SUCCESS );
            pVulkanSwapchain->m_presentSemaphores.emplace_back( presentSemaphore );
        }

        return pVulkanSwapchain;
    }

    void DestroySwapchain( Context* pContext, Swapchain*&& pSwapchain )
    {
        if ( pSwapchain == nullptr )
        {
            return;
        }

        VulkanContext*   pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanSwapchain* pVulkanSwapchain = static_cast<VulkanSwapchain*>( pSwapchain );

        // Every caller waits the present queue idle first, which is what Vulkan needs before the
        // images go away.
        for ( uint32_t imageIndex = 0; imageIndex < pVulkanSwapchain->m_numImages; ++imageIndex )
        {
            // The texture owns its views and not the image, so this destroys the views only.
            DestroyTexture( pContext, eastl::move( pVulkanSwapchain->m_renderTargets[imageIndex] ) );
        }

        for ( VkSemaphore semaphore : pVulkanSwapchain->m_acquireSemaphores )
        {
            vkDestroySemaphore( pVulkanContext->m_device, semaphore, nullptr );
        }

        for ( VkSemaphore semaphore : pVulkanSwapchain->m_presentSemaphores )
        {
            vkDestroySemaphore( pVulkanContext->m_device, semaphore, nullptr );
        }

        if ( pVulkanSwapchain->m_swapchain != VK_NULL_HANDLE )
        {
            vkDestroySwapchainKHR( pVulkanContext->m_device, pVulkanSwapchain->m_swapchain, nullptr );
        }

        // Destroyed after the VkSwapchainKHR that was created from it, not before.
        Platform::Linux::DestroyVulkanSurface( pVulkanContext->m_instance, pVulkanSwapchain->m_surface );
        pVulkanSwapchain->m_surface = VK_NULL_HANDLE;

        pVulkanContext->DestroyObject( eastl::move( pVulkanSwapchain ) );
        pSwapchain = nullptr;
    }

    uint32_t AcquireNextImage( Context* pContext, Swapchain* pSwapchain )
    {
        VulkanContext*   pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanSwapchain* pVulkanSwapchain = static_cast<VulkanSwapchain*>( pSwapchain );

        if ( pVulkanSwapchain->m_isHeadless )
        {
            pVulkanSwapchain->m_currentImageIndex = ( pVulkanSwapchain->m_currentImageIndex + 1 ) % pVulkanSwapchain->m_numImages;
            return pVulkanSwapchain->m_currentImageIndex;
        }

        VkSemaphore const acquireSemaphore = pVulkanSwapchain->m_acquireSemaphores[pVulkanSwapchain->m_nextAcquireSemaphore];
        pVulkanSwapchain->m_nextAcquireSemaphore = ( pVulkanSwapchain->m_nextAcquireSemaphore + 1 ) % pVulkanSwapchain->m_numImages;

        VkResult const result = vkAcquireNextImageKHR( pVulkanContext->m_device, pVulkanSwapchain->m_swapchain, UINT64_MAX,
                                                       acquireSemaphore, VK_NULL_HANDLE, &pVulkanSwapchain->m_currentImageIndex );

        if ( result == VK_ERROR_OUT_OF_DATE_KHR )
        {
            // No image was acquired and the semaphore was not signalled, so recording a wait on it
            // would hang the queue. The engine recreates the swapchain next frame, so this one
            // renders into the image it already held rather than stopping.
            return pVulkanSwapchain->m_currentImageIndex;
        }

        EE_ASSERT( result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR );

        // D3D12 needs nothing here, because GetCurrentBackBufferIndex answers without waiting.
        // Vulkan hands back an image the presentation engine may still be reading, so the wait goes
        // on the present queue and the next submit drains it. RecordQueueOrderingWait carries the
        // order to every later submit, which matters because the one that writes the image is
        // rarely the first.
        VkSemaphoreSubmitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        waitInfo.semaphore = acquireSemaphore;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        pVulkanSwapchain->m_pPresentQueue->m_pendingWaits.emplace_back( waitInfo );

        return pVulkanSwapchain->m_currentImageIndex;
    }

    void SetVSync( Swapchain* pSwapchain, bool vsync )
    {
        VulkanSwapchain* pVulkanSwapchain = static_cast<VulkanSwapchain*>( pSwapchain );

        pVulkanSwapchain->m_vsync = vsync;

        // FIFO is the one mode every implementation has to support, and it is vsync. Without vsync,
        // MAILBOX if the surface has it, because it tears nothing, and IMMEDIATE otherwise.
        auto HasPresentMode = [pVulkanSwapchain] ( VkPresentModeKHR mode )
        {
            return eastl::find( pVulkanSwapchain->m_supportedPresentModes.begin(), pVulkanSwapchain->m_supportedPresentModes.end(), mode ) != pVulkanSwapchain->m_supportedPresentModes.end();
        };

        pVulkanSwapchain->m_presentMode = VK_PRESENT_MODE_FIFO_KHR;

        if ( !vsync )
        {
            if ( HasPresentMode( VK_PRESENT_MODE_MAILBOX_KHR ) )
            {
                pVulkanSwapchain->m_presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            else if ( HasPresentMode( VK_PRESENT_MODE_IMMEDIATE_KHR ) )
            {
                pVulkanSwapchain->m_presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }

        // A present mode is fixed at swapchain creation, so a later call only takes effect at the
        // next recreation. Nothing calls this outside CreateSwapchain today.
    }

    //-------------------------------------------------------------------------
    // Command pools and buffers
    //-------------------------------------------------------------------------


    //-------------------------------------------------------------------------

    CommandPool* CreateCommandPool( Context* pContext, CommandPoolParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( parameters.m_pQueue );
        VulkanCommandPool* pVulkanCommandPool = pVulkanContext->CreateObject<VulkanCommandPool>();

        uint32_t numQueueFamilies = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, nullptr );
        TVector<VkQueueFamilyProperties> queueFamilyProperties( numQueueFamilies );
        vkGetPhysicalDeviceQueueFamilyProperties( pVulkanContext->m_physicalDevice, &numQueueFamilies, queueFamilyProperties.data() );
        pVulkanCommandPool->m_queueFlags = queueFamilyProperties[pVulkanQueue->m_queueFamilyIndex].queueFlags;

        VkCommandPoolCreateInfo commandPoolCreateInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commandPoolCreateInfo.queueFamilyIndex = pVulkanQueue->m_queueFamilyIndex;
        // The engine begins buffers individually rather than resetting the pool, so each buffer
        // has to reset itself.
        commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        [[maybe_unused]] VkResult const result = vkCreateCommandPool( pVulkanContext->m_device, &commandPoolCreateInfo, nullptr, &pVulkanCommandPool->m_commandPool );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandPool->m_pQueue = parameters.m_pQueue;

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_COMMAND_POOL, uint64_t( pVulkanCommandPool->m_commandPool ), parameters.m_debugName );

        return pVulkanCommandPool;
    }

    void DestroyCommandPool( Context* pContext, CommandPool*&& pCommandPool )
    {
        if ( pCommandPool != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanCommandPool* pVulkanCommandPool = static_cast<VulkanCommandPool*>( pCommandPool );

            if ( pVulkanCommandPool->m_commandPool != VK_NULL_HANDLE )
            {
                // Destroying the pool frees these, so clear the handles. DestroyCommandBuffer then
                // skips its vkFreeCommandBuffers.
                for ( VulkanCommandBuffer* pAllocatedCommandBuffer : pVulkanCommandPool->m_allocatedCommandBuffers )
                {
                    pAllocatedCommandBuffer->m_commandBuffer = VK_NULL_HANDLE;
                }

                vkDestroyCommandPool( pVulkanContext->m_device, pVulkanCommandPool->m_commandPool, nullptr );
            }

            pVulkanCommandPool->m_allocatedCommandBuffers.clear();

            pVulkanContext->DestroyObject( eastl::move( pVulkanCommandPool ) );
            pCommandPool = nullptr;
        }
    }

    void ResetCommandPool( Context* pContext, CommandPool* pCommandPool )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanCommandPool* pVulkanCommandPool = static_cast<VulkanCommandPool*>( pCommandPool );

        // No RELEASE_RESOURCES: this runs once per frame per pool, and the memory is meant to be reused.
        [[maybe_unused]] VkResult const result = vkResetCommandPool( pVulkanContext->m_device, pVulkanCommandPool->m_commandPool, 0 );
        EE_ASSERT( result == VK_SUCCESS );
    }

    CommandBuffer* CreateCommandBuffer( Context* pContext, CommandBufferParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanCommandPool* pVulkanCommandPool = static_cast<VulkanCommandPool*>( parameters.m_pCommandPool );
        VulkanCommandBuffer* pVulkanCommandBuffer = pVulkanContext->CreateObject<VulkanCommandBuffer>();

        VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = pVulkanCommandPool->m_commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;

        [[maybe_unused]] VkResult const result = vkAllocateCommandBuffers( pVulkanContext->m_device, &allocateInfo, &pVulkanCommandBuffer->m_commandBuffer );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandPool->m_allocatedCommandBuffers.emplace_back( pVulkanCommandBuffer );

        pVulkanCommandBuffer->m_device = pVulkanContext->m_device;
        pVulkanCommandBuffer->m_vkCmdPushDescriptorSet = pVulkanContext->m_vkCmdPushDescriptorSet;
        pVulkanCommandBuffer->m_vkCmdBeginDebugUtilsLabel = pVulkanContext->m_vkCmdBeginDebugUtilsLabel;
        pVulkanCommandBuffer->m_vkCmdEndDebugUtilsLabel = pVulkanContext->m_vkCmdEndDebugUtilsLabel;
        pVulkanCommandBuffer->m_vkCmdDrawMeshTasks = pVulkanContext->m_vkCmdDrawMeshTasks;
        pVulkanCommandBuffer->m_vkCmdDrawMeshTasksIndirect = pVulkanContext->m_vkCmdDrawMeshTasksIndirect;
        pVulkanCommandBuffer->m_vkCmdDrawMeshTasksIndirectCount = pVulkanContext->m_vkCmdDrawMeshTasksIndirectCount;
        // The pool's family decides what this buffer may record. FlushBarriers clamps every barrier
        // to it, and the shading rate entry point is gated on it below.
        pVulkanCommandBuffer->m_queueFlags = pVulkanCommandPool->m_queueFlags;

        // vkCmdSetFragmentShadingRateKHR requires VK_QUEUE_GRAPHICS_BIT. Leaving it null on a
        // transfer or compute pool stops BeginCommandBuffer setting a rate there, and makes
        // CmdSetShadingRate's assert name the caller if a pass ever asks for one.
        pVulkanCommandBuffer->m_vkCmdSetFragmentShadingRate = ( pVulkanCommandPool->m_queueFlags & VK_QUEUE_GRAPHICS_BIT )
                                                             ? pVulkanContext->m_vkCmdSetFragmentShadingRate : nullptr;
        pVulkanCommandBuffer->m_vkCmdBuildAccelerationStructures = pVulkanContext->m_vkCmdBuildAccelerationStructures;
        pVulkanCommandBuffer->m_vkCmdTraceRays = pVulkanContext->m_vkCmdTraceRays;
        pVulkanCommandBuffer->m_vkCmdTraceRaysIndirect2 = pVulkanContext->m_vkCmdTraceRaysIndirect2;
        pVulkanCommandBuffer->m_shadingRateCaps = pContext->m_deviceCapabilities.m_shadingRateCaps;
        // CmdSetShadingRate is handed a CommandBuffer and no Context.
        pVulkanCommandBuffer->m_shadingRateTexelSize =
        {
            pContext->m_deviceCapabilities.m_shadingRateTexelWidth,
            pContext->m_deviceCapabilities.m_shadingRateTexelHeight
        };
        pVulkanCommandBuffer->m_pQueue = parameters.m_pCommandPool->m_pQueue;
        pVulkanCommandBuffer->m_pCommandPool = parameters.m_pCommandPool;
        pVulkanCommandBuffer->m_nodeIndex = parameters.m_pCommandPool->m_pQueue->m_nodeIndex;

        // D3D12 creates a command list already recording and closes it to match other APIs.
        // Vulkan already starts in the initial state, so there is nothing to undo.
        pVulkanCommandBuffer->m_stage = VulkanCommandBuffer::Stage::Closed;

        // Written by the CPU, read by the GPU, and never given a descriptor of its own:
        // CmdSetRootConstants pushes a descriptor at an offset into it.
        BufferParameters ringParameters = {};
        ringParameters.m_bufferSize = g_rootConstantRingSize;
        ringParameters.m_memoryType = ResourceMemoryType::HostToDevice;
        ringParameters.m_flags.SetMultipleFlags( BufferFlags::NoDescriptors, BufferFlags::PersistentMap );
        // RWBuffer as well as ConstantBuffer, for the storage buffer usage bit. The ring also
        // stands in for set 0 bindings the engine never wrote, where a root SRV or UAV is a
        // storage buffer. Nothing reads those bytes; see BindUnboundRootParameters.
        ringParameters.m_descriptorTypes.SetMultipleFlags( DescriptorTypeFlags::ConstantBuffer, DescriptorTypeFlags::RWBuffer );
        ringParameters.m_debugName.sprintf( "%s RootConstants", parameters.m_debugName.c_str() );

        pVulkanCommandBuffer->m_pRootConstantRing = CreateBuffer( pContext, ringParameters );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_COMMAND_BUFFER, uint64_t( pVulkanCommandBuffer->m_commandBuffer ), parameters.m_debugName );

        return pVulkanCommandBuffer;
    }

    void DestroyCommandBuffer( Context* pContext, CommandBuffer*&& pCommandBuffer )
    {
        if ( pCommandBuffer != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
            VulkanCommandPool* pVulkanCommandPool = static_cast<VulkanCommandPool*>( pVulkanCommandBuffer->m_pCommandPool );

            // Null when the pool was destroyed first, which frees its buffers for us.
            if ( pVulkanCommandBuffer->m_commandBuffer != VK_NULL_HANDLE )
            {
                pVulkanCommandPool->m_allocatedCommandBuffers.erase_first_unsorted( pVulkanCommandBuffer );
                vkFreeCommandBuffers( pVulkanContext->m_device, pVulkanCommandPool->m_commandPool, 1, &pVulkanCommandBuffer->m_commandBuffer );
            }

            DestroyBuffer( pContext, eastl::move( pVulkanCommandBuffer->m_pRootConstantRing ) );

            pVulkanContext->DestroyObject( eastl::move( pVulkanCommandBuffer ) );
            pCommandBuffer = nullptr;
        }
    }

    void BeginCommandBuffer( CommandBuffer* pCommandBuffer )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        EE_ASSERT( pVulkanCommandBuffer->m_stage == VulkanCommandBuffer::Stage::Closed );

        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        // No ONE_TIME_SUBMIT. D3D12 allows a closed command list to be submitted more than once
        // without re-recording, and nothing here proves the engine never does.
        beginInfo.flags = 0;

        [[maybe_unused]] VkResult const result = vkBeginCommandBuffer( pVulkanCommandBuffer->m_commandBuffer, &beginInfo );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandBuffer->m_stage = VulkanCommandBuffer::Stage::Recording;

        // D3D12 calls SetDescriptorHeaps here. The equivalent binds the heap descriptor set, which
        // happens in CmdSetPipeline instead, because set 1 is disturbed whenever a pipeline with a
        // different set 0 layout is bound.
        pVulkanCommandBuffer->m_pBoundRootSignature = nullptr;
        pVulkanCommandBuffer->m_pBoundPipeline = nullptr;
        pVulkanCommandBuffer->m_boundPipelineLayout = VK_NULL_HANDLE;
        pVulkanCommandBuffer->m_isRendering = false;
        pVulkanCommandBuffer->m_needsRenderingBegin = false;
        pVulkanCommandBuffer->m_rootConstantRingOffset = 0;
        pVulkanCommandBuffer->m_shadingRateImageView = VK_NULL_HANDLE;

        // A declared dynamic state that is never set leaves every draw undefined. Every pipeline
        // declares the shading rate state when the extension is on and nothing in the engine sets
        // it, so the full rate is set once here.
        if ( pVulkanCommandBuffer->m_vkCmdSetFragmentShadingRate != nullptr )
        {
            VkExtent2D const fullRate = { 1, 1 };
            VkFragmentShadingRateCombinerOpKHR const keepBoth[2] = { VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR };
            pVulkanCommandBuffer->m_vkCmdSetFragmentShadingRate( pVulkanCommandBuffer->m_commandBuffer, &fullRate, keepBoth );
        }

        // A barrier that outlived its command buffer would apply to the wrong work.
        EE_ASSERT( pVulkanCommandBuffer->m_globalBarriers.empty() );
        EE_ASSERT( pVulkanCommandBuffer->m_bufferBarriers.empty() );
        EE_ASSERT( pVulkanCommandBuffer->m_imageBarriers.empty() );
    }

    void EndCommandBuffer( CommandBuffer* pCommandBuffer )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        EE_ASSERT( pVulkanCommandBuffer->m_stage == VulkanCommandBuffer::Stage::Recording );

        // Vulkan is stricter than D3D12 here: an unbalanced label is a validation error, not a
        // cosmetic one.
        EE_ASSERT( pVulkanCommandBuffer->m_debugMarkerScopeCounter == 0 );

        // Dynamic rendering has to be closed before the command buffer is. A configuration that
        // never reached a draw is begun and ended here, so the clear it carries still happens.
        FlushRendering( pVulkanCommandBuffer );

        // A barrier recorded and never flushed would not happen at all.
        FlushBarriers( pVulkanCommandBuffer );

        [[maybe_unused]] VkResult const result = vkEndCommandBuffer( pVulkanCommandBuffer->m_commandBuffer );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandBuffer->m_stage = VulkanCommandBuffer::Stage::Closed;
    }

    //-------------------------------------------------------------------------
    // Render pass and draw commands
    //-------------------------------------------------------------------------

    // Begins the pass CmdSetRenderTargets configured, if it has not begun already. Every draw
    // calls this, after flushing barriers, because the begin is deferred to the draw.
    static void BeginRenderingIfPending( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        if ( !pVulkanCommandBuffer->m_needsRenderingBegin )
        {
            return;
        }

        VkRenderingInfo& renderingInfo = pVulkanCommandBuffer->m_renderingInfo;

        // Re-pointed on every begin. The attachment vector may have been refilled since the
        // configuration was recorded, so a pointer taken then could be stale.
        renderingInfo.colorAttachmentCount = uint32_t( pVulkanCommandBuffer->m_colorAttachments.size() );
        renderingInfo.pColorAttachments = pVulkanCommandBuffer->m_colorAttachments.data();
        renderingInfo.pDepthAttachment = pVulkanCommandBuffer->m_hasDepthAttachment ? &pVulkanCommandBuffer->m_depthAttachment : nullptr;
        renderingInfo.pStencilAttachment = pVulkanCommandBuffer->m_hasStencilAttachment ? &pVulkanCommandBuffer->m_stencilAttachment : nullptr;

        // Chained rather than set, so a pass without a shading rate image carries no pNext at all.
        VkRenderingFragmentShadingRateAttachmentInfoKHR shadingRateAttachment = { VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
        if ( pVulkanCommandBuffer->m_shadingRateImageView != VK_NULL_HANDLE )
        {
            shadingRateAttachment.imageView = pVulkanCommandBuffer->m_shadingRateImageView;
            shadingRateAttachment.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
            shadingRateAttachment.shadingRateAttachmentTexelSize = pVulkanCommandBuffer->m_shadingRateTexelSize;

            renderingInfo.pNext = &shadingRateAttachment;
        }

        vkCmdBeginRendering( pVulkanCommandBuffer->m_commandBuffer, &renderingInfo );

        pVulkanCommandBuffer->m_needsRenderingBegin = false;
        pVulkanCommandBuffer->m_isRendering = true;
    }

    // Leaves the render pass so a barrier, dispatch or copy can run, since none may run inside one.
    // The next draw begins it again with every load op forced to LOAD, so the restart keeps what
    // the first half drew. The store ops were fixed when the pass began, so a caller that asked for
    // StoreActionType::None would lose that half. No engine pass does this; it is a safety net.
    static void SuspendRendering( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        if ( !pVulkanCommandBuffer->m_isRendering )
        {
            return;
        }

        vkCmdEndRendering( pVulkanCommandBuffer->m_commandBuffer );

        pVulkanCommandBuffer->m_isRendering = false;
        pVulkanCommandBuffer->m_needsRenderingBegin = true;

        for ( VkRenderingAttachmentInfo& attachment : pVulkanCommandBuffer->m_colorAttachments )
        {
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        pVulkanCommandBuffer->m_depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        pVulkanCommandBuffer->m_stencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    // Finishes the current pass for good. A configuration that never reached a draw is begun and
    // ended anyway, because its load op carries the clear the caller asked for, and a D3D12 render
    // target bound and cleared with no draw still gets cleared.
    static void FlushRendering( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        BeginRenderingIfPending( pVulkanCommandBuffer );

        if ( pVulkanCommandBuffer->m_isRendering )
        {
            vkCmdEndRendering( pVulkanCommandBuffer->m_commandBuffer );
            pVulkanCommandBuffer->m_isRendering = false;
        }
    }

    //-------------------------------------------------------------------------

    // LoadActionType::DontCare means "the caller said nothing", not "discard". LoadAction is zero
    // initialised and DontCare is the zero, so every action the engine leaves alone arrives here as
    // DontCare. Mapping it to DONT_CARE would discard the whole rendered frame at the debug draw
    // pass, which sets only the depth action and binds the frame's final colour target. D3D12 has
    // no load actions at all, and preserves.
    static VkAttachmentLoadOp VulkanLoadOp( LoadActionType action )
    {
        switch ( action )
        {
            case LoadActionType::Load:      return VK_ATTACHMENT_LOAD_OP_LOAD;
            case LoadActionType::Clear:     return VK_ATTACHMENT_LOAD_OP_CLEAR;
            case LoadActionType::DontCare:  return VK_ATTACHMENT_LOAD_OP_LOAD;
        }

        EE_UNREACHABLE_CODE();
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    // The same reasoning, and it matters more: no engine pass sets a store action, so every
    // attachment arrives as DontCare and discarding would throw away every render pass in the
    // frame. StoreActionType::None is untouched, so a caller that means it still has a value for it.
    static VkAttachmentStoreOp VulkanStoreOp( StoreActionType action )
    {
        switch ( action )
        {
            case StoreActionType::Store:    return VK_ATTACHMENT_STORE_OP_STORE;
            case StoreActionType::DontCare: return VK_ATTACHMENT_STORE_OP_STORE;
            // None means the attachment is untouched, which is exactly what this op says.
            case StoreActionType::None:     return VK_ATTACHMENT_STORE_OP_NONE;
        }

        EE_UNREACHABLE_CODE();
        return VK_ATTACHMENT_STORE_OP_STORE;
    }

    // The engine binds render targets it has not transitioned. D3D12 has no image layouts so
    // nothing there is wrong, but Vulkan needs the layout to match the attachment's use. A texture
    // arrives either still UNDEFINED, or in whatever layout its last read left it.
    //
    // Corrected here rather than asserted, because the engine has no barrier to add: the state it
    // tracks is the D3D12 one, and it is already correct in those terms.
    //
    // ALL_COMMANDS and all-access on both sides. Narrowing needs to know what last touched the
    // image and what the pass will do to it, and neither is passed here.
    static void TransitionAttachmentIfNeeded( VulkanCommandBuffer* pVulkanCommandBuffer, VulkanTexture* pVulkanTexture, VkImageLayout attachmentLayout, uint32_t arraySlice, uint32_t mipSlice )
    {
        if ( pVulkanTexture->CurrentLayout( mipSlice, arraySlice ) == attachmentLayout )
        {
            return;
        }

        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        // The truth, so the contents survive unless there were none. Only an image still in
        // UNDEFINED loses anything, and there was nothing there to lose.
        barrier.oldLayout = pVulkanTexture->CurrentLayout( mipSlice, arraySlice );
        barrier.newLayout = attachmentLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pVulkanTexture->m_image;
        // Only the subresource being bound. The rest of the image may legitimately be in another
        // layout: a cubemap capture samples the faces it has already drawn.
        barrier.subresourceRange.aspectMask = pVulkanTexture->m_aspectMask;
        barrier.subresourceRange.baseMipLevel = mipSlice;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = arraySlice;
        barrier.subresourceRange.layerCount = 1;

        pVulkanCommandBuffer->m_imageBarriers.emplace_back( barrier );
        pVulkanTexture->SetLayout( attachmentLayout, mipSlice, 1, arraySlice, 1 );
    }

    void CmdSetRenderTargets( CommandBuffer* pCommandBuffer, TArrayView<Texture* const> renderTargets, Texture* pDepthStencil, LoadAction* pLoadAction, TArrayView<uint32_t const> colorArraySlices, TArrayView<uint32_t const> colorMipSlices, uint32_t depthArraySlice, uint32_t depthMipSlice )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // OMSetRenderTargets replaces what is bound. Dynamic rendering has a begin and an end, so
        // the previous one is finished here.
        FlushRendering( pVulkanCommandBuffer );

        // The barriers that put these textures into their attachment layouts are pending now, and
        // the layouts below read what they leave behind.
        FlushBarriers( pVulkanCommandBuffer );

        pVulkanCommandBuffer->m_colorAttachments.clear();
        pVulkanCommandBuffer->m_hasDepthAttachment = false;
        pVulkanCommandBuffer->m_hasStencilAttachment = false;

        if ( renderTargets.empty() && pDepthStencil == nullptr )
        {
            return;
        }

        // D3D12 clears with a separate ClearRenderTargetView after binding. Here the clear is the
        // load op, which is what a tiler needs.
        VkExtent2D renderArea = {};

        for ( size_t renderTargetIndex = 0; renderTargetIndex < renderTargets.size(); ++renderTargetIndex )
        {
            VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( renderTargets[renderTargetIndex] );

            // Which subresource to draw into. A different VkImageView, created by CreateTexture.
            uint32_t const colorArraySlice = colorArraySlices.empty() ? 0 : colorArraySlices[renderTargetIndex];
            uint32_t const colorMipSlice = colorMipSlices.empty() ? 0 : colorMipSlices[renderTargetIndex];

            VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            attachment.imageView = pVulkanTexture->RenderTargetView( colorArraySlice, colorMipSlice );
            // The layout the texture is actually in, not the one an attachment is usually in.
            TransitionAttachmentIfNeeded( pVulkanCommandBuffer, pVulkanTexture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorArraySlice, colorMipSlice );
            attachment.imageLayout = pVulkanTexture->CurrentLayout( colorMipSlice, colorArraySlice );
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            if ( pLoadAction != nullptr )
            {
                attachment.loadOp = VulkanLoadOp( pLoadAction->m_loadActionsColor[renderTargetIndex] );
                attachment.storeOp = VulkanStoreOp( pLoadAction->m_storeActionsColor[renderTargetIndex] );

                ClearValue const& clearValue = pLoadAction->m_colorClearValues[renderTargetIndex];
                attachment.clearValue.color.float32[0] = clearValue.m_red;
                attachment.clearValue.color.float32[1] = clearValue.m_green;
                attachment.clearValue.color.float32[2] = clearValue.m_blue;
                attachment.clearValue.color.float32[3] = clearValue.m_alpha;
            }

            pVulkanCommandBuffer->m_colorAttachments.push_back( attachment );

            renderArea.width = Math::Max( renderArea.width, Math::Max( pVulkanTexture->m_extent.width >> colorMipSlice, 1U ) );
            renderArea.height = Math::Max( renderArea.height, Math::Max( pVulkanTexture->m_extent.height >> colorMipSlice, 1U ) );
        }

        if ( pDepthStencil != nullptr )
        {
            VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( pDepthStencil );

            pVulkanCommandBuffer->m_hasDepthAttachment = ( pVulkanTexture->m_aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT ) != 0;
            pVulkanCommandBuffer->m_hasStencilAttachment = ( pVulkanTexture->m_aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ) != 0;

            VkRenderingAttachmentInfo depthAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            depthAttachment.imageView = pVulkanTexture->RenderTargetView( depthArraySlice, depthMipSlice );
            // Read from the texture for the same reason as the colour targets, and it matters more
            // here: the debug draw pass binds a depth target it only reads, which is a read-only
            // layout rather than the attachment one.
            TransitionAttachmentIfNeeded( pVulkanCommandBuffer, pVulkanTexture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, depthArraySlice, depthMipSlice );
            depthAttachment.imageLayout = pVulkanTexture->CurrentLayout( depthMipSlice, depthArraySlice );
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo stencilAttachment = depthAttachment;

            if ( pLoadAction != nullptr )
            {
                depthAttachment.loadOp = VulkanLoadOp( pLoadAction->m_loadActionDepth );
                depthAttachment.storeOp = VulkanStoreOp( pLoadAction->m_storeActionsDepth );
                depthAttachment.clearValue.depthStencil.depth = pLoadAction->m_depthClearValue.m_depth;

                stencilAttachment.loadOp = VulkanLoadOp( pLoadAction->m_loadActionStencil );
                stencilAttachment.storeOp = VulkanStoreOp( pLoadAction->m_storeActionStencil );
                stencilAttachment.clearValue.depthStencil.stencil = pLoadAction->m_depthClearValue.m_stencil;
            }

            pVulkanCommandBuffer->m_depthAttachment = depthAttachment;
            pVulkanCommandBuffer->m_stencilAttachment = stencilAttachment;

            renderArea.width = Math::Max( renderArea.width, Math::Max( pVulkanTexture->m_extent.width >> depthMipSlice, 1U ) );
            renderArea.height = Math::Max( renderArea.height, Math::Max( pVulkanTexture->m_extent.height >> depthMipSlice, 1U ) );
        }

        pVulkanCommandBuffer->m_renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        // D3D12 has no render area and draws wherever the viewport and scissor allow. The full
        // extent of the attachments is the same thing, and the viewport still restricts it.
        pVulkanCommandBuffer->m_renderingInfo.renderArea.extent = renderArea;
        pVulkanCommandBuffer->m_renderingInfo.layerCount = 1;

        // Configured, not begun. See VulkanCommandBuffer::m_needsRenderingBegin for why.
        pVulkanCommandBuffer->m_needsRenderingBegin = true;
    }

    // A D3D12 shading rate is a pair of coarse pixel dimensions and so is the Vulkan one.
    static VkExtent2D VulkanFragmentSize( ShadingRate shadingRate )
    {
        switch ( shadingRate )
        {
            case ShadingRate::Full:     return { 1, 1 };
            case ShadingRate::Rate1x2:  return { 1, 2 };
            case ShadingRate::Rate2x1:  return { 2, 1 };
            case ShadingRate::Half:     return { 2, 2 };
            case ShadingRate::Rate2x4:  return { 2, 4 };
            case ShadingRate::Rate4x2:  return { 4, 2 };
            case ShadingRate::Quarter:  return { 4, 4 };
            default: break;
        }

        EE_ASSERT( false );
        return { 1, 1 };
    }

    // Four of the five combiners map. D3D12's SUM adds the two rates and the nearest Vulkan
    // operation multiplies them, so SUM maps to MUL and the two backends would disagree on it.
    // Nothing in the engine calls CmdSetShadingRate, so nothing disagrees today.
    static VkFragmentShadingRateCombinerOpKHR VulkanShadingRateCombiner( ShadingRateCombiner combiner )
    {
        switch ( combiner )
        {
            case ShadingRateCombiner::Passthrough:  return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            case ShadingRateCombiner::Override:     return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
            case ShadingRateCombiner::Min:          return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
            case ShadingRateCombiner::Max:          return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
            case ShadingRateCombiner::Sum:          return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR;
        }

        EE_UNREACHABLE_CODE();
        return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    }

    void CmdSetShadingRate( CommandBuffer* pCommandBuffer, ShadingRate shadingRate, Texture* pShadingRateTexture, ShadingRateCombiner postRasterizerCombiner, ShadingRateCombiner finalCombiner )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // Both guards are false today, because FillDeviceCapabilities reports NotSupported on both
        // backends. Changing that on one and not the other would make the two render the same scene
        // differently. Turning variable rate shading on also needs the two things named in the
        // per-tile branch below.
        if ( pVulkanCommandBuffer->m_shadingRateCaps.IsFlagSet( ShadingRateCaps::PerDraw ) )
        {
            EE_ASSERT( pVulkanCommandBuffer->m_vkCmdSetFragmentShadingRate != nullptr );

            VkExtent2D const fragmentSize = VulkanFragmentSize( shadingRate );
            VkFragmentShadingRateCombinerOpKHR const combiners[2] =
            {
                VulkanShadingRateCombiner( postRasterizerCombiner ),
                VulkanShadingRateCombiner( finalCombiner )
            };

            pVulkanCommandBuffer->m_vkCmdSetFragmentShadingRate( pVulkanCommandBuffer->m_commandBuffer, &fragmentSize, combiners );
        }

        if ( pShadingRateTexture != nullptr && pVulkanCommandBuffer->m_shadingRateCaps.IsFlagSet( ShadingRateCaps::PerTile ) )
        {
            VulkanTexture const* pVulkanTexture = static_cast<VulkanTexture const*>( pShadingRateTexture );

            // Recorded, not bound. Setting a rate image is a command on D3D12 and an attachment of
            // the render pass on Vulkan, so BeginRenderingIfPending is where it reaches the device.
            //
            // Two things this path still needs, both guesswork until something creates a rate
            // image: CreateTexture does not set the shading rate attachment usage bit, and the
            // view below is the render target view, which only exists for a RenderTarget texture.
            pVulkanCommandBuffer->m_shadingRateImageView = pVulkanTexture->RenderTargetView( 0, 0 );
        }
    }

    void CmdSetViewport( CommandBuffer* pCommandBuffer, float x, float y, float width, float height, float minDepth, float maxDepth )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // Clip-space Y is inverted here, and exactly once. D3D12 clip space has +Y up and Vulkan
        // has +Y down, so without this everything renders upside down. The shader compiler does
        // not do it, and -fvk-invert-y must never be added.
        //
        // Do not add a second flip anywhere. The other half of this is the front face in
        // CreatePipeline, which accounts for the winding this reverses.
        VkViewport viewport = {};
        viewport.x = x;
        viewport.y = y + height;
        viewport.width = width;
        viewport.height = -height;
        viewport.minDepth = minDepth;
        viewport.maxDepth = maxDepth;

        vkCmdSetViewport( pVulkanCommandBuffer->m_commandBuffer, 0, 1, &viewport );
    }

    void CmdSetScissor( CommandBuffer* pCommandBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // The scissor is in framebuffer coordinates and is not affected by the viewport flip.
        VkRect2D scissor = {};
        scissor.offset.x = int32_t( x );
        scissor.offset.y = int32_t( y );
        scissor.extent.width = width;
        scissor.extent.height = height;

        vkCmdSetScissor( pVulkanCommandBuffer->m_commandBuffer, 0, 1, &scissor );
    }

    void CmdSetStencilReference( CommandBuffer* pCommandBuffer, uint32_t value )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        vkCmdSetStencilReference( pVulkanCommandBuffer->m_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, value );
    }

    void CmdSetPipeline( CommandBuffer* pCommandBuffer, Pipeline* pPipeline )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanPipeline* pVulkanPipeline = static_cast<VulkanPipeline*>( pPipeline );
        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pVulkanPipeline->m_pRootSignature );

        // A pipeline layout change invalidates the push descriptor set, so whatever set 0 held is
        // gone from here on.
        pVulkanCommandBuffer->m_boundRootParameterMask = 0;

        // Null when CreatePipeline skipped a mesh pipeline on a device without VK_EXT_mesh_shader.
        // Every draw against it is dropped, so the rest of the frame still records and presents.
        // A development convenience for hardware that cannot run the geometry path at all.
        pVulkanCommandBuffer->m_boundPipelineIsNull = ( pVulkanPipeline->m_pipeline == VK_NULL_HANDLE );
        if ( pVulkanCommandBuffer->m_boundPipelineIsNull )
        {
            if ( !g_warnedAboutDroppedMeshDraws )
            {
                g_warnedAboutDroppedMeshDraws = true;
                EE_LOG_WARNING( LogCategory::Render, "RHI/CmdSetPipeline", "This device has no VK_EXT_mesh_shader, so every mesh draw is being dropped. The frame will present without its geometry." );
            }

            pVulkanCommandBuffer->m_pBoundPipeline = pVulkanPipeline;
            pVulkanCommandBuffer->m_pBoundRootSignature = pVulkanRootSignature;
            return;
        }

        vkCmdBindPipeline( pVulkanCommandBuffer->m_commandBuffer, pVulkanPipeline->m_bindPoint, pVulkanPipeline->m_pipeline );

        pVulkanCommandBuffer->m_pBoundPipeline = pVulkanPipeline;
        pVulkanCommandBuffer->m_pBoundRootSignature = pVulkanRootSignature;
        pVulkanCommandBuffer->m_boundPipelineLayout = pVulkanRootSignature->m_pipelineLayout;
        pVulkanCommandBuffer->m_boundBindPoint = pVulkanPipeline->m_bindPoint;

        // A direct draw must not inherit the previous indirect draw's root argument block.
        // EE_IndirectRoot lives in push constants and CmdExecuteIndirect is the only thing that
        // writes it, so it keeps whatever the last indirect call left there. An indirect-capable
        // shader reads its root constants from the argument buffer whenever m_stride is non-zero,
        // so a direct draw after an indirect one would read them at a stale device address.
        //
        // Zeroing on every pipeline bind restores the "direct" meaning. Every draw binds a pipeline
        // first, and CmdExecuteIndirect pushes the real block immediately before its own dispatch.
        IndirectRootPushConstants const directDrawRootPushConstants = {};
        vkCmdPushConstants( pVulkanCommandBuffer->m_commandBuffer, pVulkanRootSignature->m_pipelineLayout,
                            VK_SHADER_STAGE_ALL, 0, sizeof( directDrawRootPushConstants ), &directDrawRootPushConstants );

        // Heap set 1 is bound here rather than once per command buffer, because binding a pipeline
        // whose layout differs from set N onwards disturbs every set from N up, and set 0 varies
        // per shader. The redundant rebind is deliberate and costs almost nothing.
        vkCmdBindDescriptorSets( pVulkanCommandBuffer->m_commandBuffer, pVulkanPipeline->m_bindPoint, pVulkanRootSignature->m_pipelineLayout,
                                 g_heapSet, 1, &pVulkanRootSignature->m_heapDescriptorSet, 0, nullptr );
    }

    void CmdSetRootConstants( CommandBuffer* pCommandBuffer, uint32_t constantIndex, void const* pConstantData, size_t constantSize )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // A dropped pipeline bound no layout, so the write would go against a stale one.
        if ( pVulkanCommandBuffer->m_boundPipelineIsNull )
        {
            return;
        }

        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pVulkanCommandBuffer->m_pBoundRootSignature );

        [[maybe_unused]] DescriptorReflection const& descriptorReflection = pVulkanRootSignature->m_descriptorReflections[constantIndex];
        EE_ASSERT( descriptorReflection.m_descriptorTypeFlags == TBitFlags( DescriptorTypeFlags::RootConstant ) );
        EE_ASSERT( constantSize == sizeof( uint32_t ) * descriptorReflection.m_numConstants );

        if ( pConstantData == nullptr )
        {
            return;
        }

        // Not Vulkan push constants. RHI.esh declares the block as a ConstantBuffer<T> on register
        // b0, so DXC emits a uniform buffer, and turning it into a push constant block would mean
        // editing RHI.esh. So the constants are copied into a per-command-buffer ring and a
        // descriptor is pushed at the copy.
        VulkanBuffer* pRing = static_cast<VulkanBuffer*>( pVulkanCommandBuffer->m_pRootConstantRing );
        EE_ASSERT( pRing != nullptr && pRing->m_pMappedAddress_WriteCombined != nullptr );

        uint64_t const offset = pVulkanCommandBuffer->m_rootConstantRingOffset;
        // Asserted rather than wrapped. Wrapping would overwrite constants the GPU is still reading
        // and surface as a shader reading the wrong values.
        EE_ASSERT( offset + constantSize <= g_rootConstantRingSize );

        memcpy( static_cast<uint8_t*>( pRing->m_pMappedAddress_WriteCombined ) + offset, pConstantData, constantSize );
        pVulkanCommandBuffer->m_rootConstantRingOffset = Math::RoundUpToNearestMultiple64( offset + constantSize, g_rootConstantAlignment );

        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = pRing->m_buffer;
        bufferInfo.offset = offset;
        bufferInfo.range = constantSize;

        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstBinding = pVulkanRootSignature->m_shaderResources[constantIndex].m_registerIndex;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;

        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdPushDescriptorSet != nullptr );
        pVulkanCommandBuffer->m_vkCmdPushDescriptorSet( pVulkanCommandBuffer->m_commandBuffer, pVulkanCommandBuffer->m_boundBindPoint,
                                                        pVulkanCommandBuffer->m_boundPipelineLayout, g_rootParameterSet, 1, &write );

        pVulkanCommandBuffer->m_boundRootParameterMask |= ( 1u << constantIndex );
    }

    void CmdSetRootParameter( CommandBuffer* pCommandBuffer, uint32_t parameterIndex, Buffer* pBuffer, size_t bufferOffset )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // A dropped pipeline bound no layout, so the write would go against a stale one.
        if ( pVulkanCommandBuffer->m_boundPipelineIsNull )
        {
            return;
        }

        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pVulkanCommandBuffer->m_pBoundRootSignature );
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );

        DescriptorReflection const& descriptorReflection = pVulkanRootSignature->m_descriptorReflections[parameterIndex];
        EE_ASSERT( bufferOffset < pVulkanBuffer->m_size );

        VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if ( descriptorReflection.m_descriptorTypeFlags.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) )
        {
            EE_ASSERT( pVulkanBuffer->m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) );
            descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        else if ( descriptorReflection.m_descriptorTypeFlags.IsFlagSet( DescriptorTypeFlags::Buffer ) )
        {
            EE_ASSERT( pVulkanBuffer->m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::Buffer ) );
        }
        else if ( descriptorReflection.m_descriptorTypeFlags.IsFlagSet( DescriptorTypeFlags::RWBuffer ) )
        {
            EE_ASSERT( pVulkanBuffer->m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWBuffer ) );
        }
        else
        {
            EE_UNREACHABLE_CODE();
        }

        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = pVulkanBuffer->m_buffer;
        bufferInfo.offset = bufferOffset;
        // VK_WHOLE_SIZE, because a D3D12 root descriptor is an address with no size.
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstBinding = pVulkanRootSignature->m_shaderResources[parameterIndex].m_registerIndex;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType;
        write.pBufferInfo = &bufferInfo;

        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdPushDescriptorSet != nullptr );
        pVulkanCommandBuffer->m_vkCmdPushDescriptorSet( pVulkanCommandBuffer->m_commandBuffer, pVulkanCommandBuffer->m_boundBindPoint,
                                                        pVulkanCommandBuffer->m_boundPipelineLayout, g_rootParameterSet, 1, &write );

        pVulkanCommandBuffer->m_boundRootParameterMask |= ( 1u << parameterIndex );
    }

    void CmdSetIndexBuffer( CommandBuffer* pCommandBuffer, Buffer const* pIndexBuffer, IndexType indexType, uint64_t offset )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer const* pVulkanBuffer = static_cast<VulkanBuffer const*>( pIndexBuffer );

        VkIndexType const vulkanIndexType = ( indexType == IndexType::Uint16 ) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        vkCmdBindIndexBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, offset, vulkanIndexType );
    }

    // The order matters: barriers may not run inside a render pass, and they are what put the
    // attachments into the layouts the pass names. Then the pass opens, which CmdSetRenderTargets
    // deliberately did not do.
    //
    // Returns false when the draw has to be dropped, which only happens for a mesh pipeline on a
    // device without VK_EXT_mesh_shader. The barriers and the pass are recorded either way.
    static bool PrepareDraw( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        FlushBarriers( pVulkanCommandBuffer );
        BeginRenderingIfPending( pVulkanCommandBuffer );

        return !pVulkanCommandBuffer->m_boundPipelineIsNull;
    }

    void CmdDraw( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t firstVertex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        if ( !PrepareDraw( pVulkanCommandBuffer ) ) { return; }
        vkCmdDraw( pVulkanCommandBuffer->m_commandBuffer, numVertices, 1, firstVertex, 0 );
    }

    void CmdDrawInstanced( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t numInstances, uint32_t firstVertex, uint32_t firstInstance )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        if ( !PrepareDraw( pVulkanCommandBuffer ) ) { return; }
        vkCmdDraw( pVulkanCommandBuffer->m_commandBuffer, numVertices, numInstances, firstVertex, firstInstance );
    }

    void CmdDrawIndexed( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t firstIndex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        if ( !PrepareDraw( pVulkanCommandBuffer ) ) { return; }
        vkCmdDrawIndexed( pVulkanCommandBuffer->m_commandBuffer, numIndices, 1, firstIndex, 0, 0 );
    }

    void CmdDrawIndexedInstanced( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t numInstances, uint32_t firstIndex, uint32_t firstInstance )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        if ( !PrepareDraw( pVulkanCommandBuffer ) ) { return; }
        vkCmdDrawIndexed( pVulkanCommandBuffer->m_commandBuffer, numIndices, numInstances, firstIndex, 0, firstInstance );
    }

    void CmdDispatchCompute( CommandBuffer* pCommandBuffer, uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // A dispatch may not run inside a render pass, and D3D12 has no such rule, so the engine
        // closes nothing before dispatching. The pass is suspended rather than finished, so a draw
        // that follows resumes it.
        FlushBarriers( pVulkanCommandBuffer );
        SuspendRendering( pVulkanCommandBuffer );

        EE_ASSERT( numGroupsX <= MaxDispatchSize && numGroupsY <= MaxDispatchSize && numGroupsZ <= MaxDispatchSize );

        vkCmdDispatch( pVulkanCommandBuffer->m_commandBuffer, numGroupsX, numGroupsY, numGroupsZ );
    }


    void CmdDispatchMesh( CommandBuffer* pCommandBuffer, uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // A mesh dispatch is a draw despite the name: it rasterises and runs inside a render pass.
        // Suspending the pass here, the way CmdDispatchCompute does, would be wrong.
        if ( !PrepareDraw( pVulkanCommandBuffer ) ) { return; }

        // The dropped-pipeline check above returns first on a device with no VK_EXT_mesh_shader,
        // so reaching this means the pipeline was real.
        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdDrawMeshTasks != nullptr );

        EE_ASSERT( numGroupsX <= MaxDispatchSize && numGroupsY <= MaxDispatchSize && numGroupsZ <= MaxDispatchSize );

        pVulkanCommandBuffer->m_vkCmdDrawMeshTasks( pVulkanCommandBuffer->m_commandBuffer, numGroupsX, numGroupsY, numGroupsZ );
    }

    // The acceleration structure parameter is unnamed because it has no use: the structure the
    // trace reads is bound through the heap handle the shader already holds. D3D12 ignores it too.
    void CmdDispatchRays( CommandBuffer* pCommandBuffer, RaytracingShaderTable* pShaderTable, AccelerationStructure*, uint32_t width, uint32_t height )
    {
        VulkanCommandBuffer*           pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanRaytracingShaderTable*   pVulkanShaderTable = static_cast<VulkanRaytracingShaderTable*>( pShaderTable );
        VulkanBuffer const*            pVulkanShaderTableBuffer = static_cast<VulkanBuffer const*>( pVulkanShaderTable->m_pBuffer );

        // Nothing can hand this a valid table, because no factory for one exists on either backend.
        EE_ASSERT( pVulkanShaderTableBuffer != nullptr );
        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdTraceRays != nullptr );

        VkDeviceAddress const tableStart = pVulkanShaderTableBuffer->m_deviceAddress;

        // The same three regions D3D12 builds, in the same order and at the same offsets: the ray
        // generation record first, then the miss records, then the hit groups, all at one stride.
        VkStridedDeviceAddressRegionKHR rayGenRegion = {};
        rayGenRegion.deviceAddress = tableStart;
        rayGenRegion.stride = pVulkanShaderTable->m_maxEntrySize;
        // A ray generation region's size has to equal its stride.
        rayGenRegion.size = pVulkanShaderTable->m_maxEntrySize;

        VkStridedDeviceAddressRegionKHR missRegion = {};
        missRegion.deviceAddress = tableStart + pVulkanShaderTable->m_maxEntrySize;
        missRegion.stride = pVulkanShaderTable->m_maxEntrySize;
        missRegion.size = pVulkanShaderTable->m_missRecordSize;

        VkStridedDeviceAddressRegionKHR hitGroupRegion = {};
        hitGroupRegion.deviceAddress = tableStart + pVulkanShaderTable->m_maxEntrySize + pVulkanShaderTable->m_missRecordSize;
        hitGroupRegion.stride = pVulkanShaderTable->m_maxEntrySize;
        hitGroupRegion.size = pVulkanShaderTable->m_hitGroupRecordSize;

        // Vulkan has a fourth region for callable shaders and the RHI has no concept of one.
        VkStridedDeviceAddressRegionKHR callableRegion = {};

        CmdSetPipeline( pCommandBuffer, pVulkanShaderTable->m_pPipeline );

        // A trace is not a draw and may not run inside a render pass, so it suspends one the way a
        // compute dispatch does.
        FlushBarriers( pVulkanCommandBuffer );
        SuspendRendering( pVulkanCommandBuffer );

        pVulkanCommandBuffer->m_vkCmdTraceRays( pVulkanCommandBuffer->m_commandBuffer, &rayGenRegion, &missRegion, &hitGroupRegion, &callableRegion, width, height, 1 );
    }

    //-------------------------------------------------------------------------
    // Indirect draws and command signatures
    //-------------------------------------------------------------------------
    // A D3D12 command signature can set root constants and bind root descriptors per command.
    // Vulkan indirect draws read draw arguments and nothing else, and every signature the engine
    // builds carries both. One material command is laid out like this:
    //
    //     [ root constants   40 bytes ]   set 0 binding b0, a uniform buffer on Vulkan
    //     [ root CBV address  8 bytes ]   set 0 binding b1, a uniform buffer on Vulkan
    //     [ dispatch args    12 bytes ]   VkDispatchIndirectCommand
    //
    // vkCmdDrawIndirect takes a stride, so it reads the last block out of the fat struct. It
    // cannot rebind the first two, so the push becomes a pull: the shader reads its own command
    // out of the argument buffer, indexed by DrawIndex, and CmdExecuteIndirect says where.

    struct VulkanCommandSignature final : CommandSignature
    {
        // Where the draw or dispatch argument sits inside one command. D3D12 packs the root
        // arguments ahead of it, and Vulkan reads only this part.
        uint32_t                                            m_drawArgumentOffset = 0;

        // True when the signature also sets root constants or binds root descriptors per command.
        bool                                                m_hasRootArguments = false;

        // Byte offsets of the two root blocks inside one command, for the shader to index with.
        // -1 when the signature has no such block. CmdExecuteIndirect pushes both.
        int32_t                                             m_rootConstantOffset = -1;
        int32_t                                             m_rootCbvOffset = -1;
    };

    // An indirect draw's set 0 bindings are declared but never written, and Vulkan still wants them
    // bound. The engine binds neither on the CPU, because a D3D12 command signature would write
    // them per command. The shader loads them from the argument buffer instead, but RHI.esh keeps
    // the ConstantBuffer declarations - they are the direct-bind fallback, and they preserve the
    // reflected layout the signature is built from - so the shader statically uses set 0, and
    // Vulkan requires a statically used binding to be bound.
    //
    // So the gaps are filled with the root constant ring every command buffer already owns. The
    // contents are undefined and unread, because the shader takes the argument buffer branch here.
    //
    // Only the bindings the engine did not write, and only on an indirect draw. A direct draw gets
    // no such help, so a genuinely forgotten binding still fails validation there.
    //
    // No ring space is consumed. Reserving a block per indirect draw would burn a ring that asserts
    // rather than wraps, for bytes nothing reads.
    static void BindUnboundRootParameters( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        VulkanRootSignature const* pVulkanRootSignature = static_cast<VulkanRootSignature const*>( pVulkanCommandBuffer->m_pBoundRootSignature );
        if ( pVulkanRootSignature == nullptr || pVulkanRootSignature->m_rootParameterBindings.empty() )
        {
            return;
        }

        VulkanBuffer const* pRing = static_cast<VulkanBuffer const*>( pVulkanCommandBuffer->m_pRootConstantRing );
        EE_ASSERT( pRing != nullptr );

        TInlineVector<VkDescriptorBufferInfo, 32> bufferInfos;
        TInlineVector<VkWriteDescriptorSet, 32> writes;

        for ( uint32_t parameterIndex = 0; parameterIndex < pVulkanRootSignature->m_rootParameterBindings.size(); ++parameterIndex )
        {
            if ( pVulkanCommandBuffer->m_boundRootParameterMask & ( 1u << parameterIndex ) ) { continue; }

            VkDescriptorSetLayoutBinding const& binding = pVulkanRootSignature->m_rootParameterBindings[parameterIndex];

            // A sampler cannot be pointed at a buffer. Samplers go through the heap, so no shader
            // puts one in set 0, and this is a guard rather than a gap.
            if ( binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                 binding.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER )
            {
                continue;
            }

            VkDescriptorBufferInfo bufferInfo = {};
            bufferInfo.buffer = pRing->m_buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = VK_WHOLE_SIZE;
            bufferInfos.push_back( bufferInfo );

            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.descriptorType;
            writes.push_back( write );
        }

        if ( writes.empty() ) { return; }

        // Fixed up after the fact, because push_back may reallocate the info array and leave
        // every pBufferInfo already stored pointing at freed memory.
        for ( uint32_t writeIndex = 0; writeIndex < writes.size(); ++writeIndex )
        {
            writes[writeIndex].pBufferInfo = &bufferInfos[writeIndex];
        }

        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdPushDescriptorSet != nullptr );
        pVulkanCommandBuffer->m_vkCmdPushDescriptorSet( pVulkanCommandBuffer->m_commandBuffer, pVulkanCommandBuffer->m_boundBindPoint,
                                                        pVulkanCommandBuffer->m_boundPipelineLayout, g_rootParameterSet,
                                                        uint32_t( writes.size() ), writes.data() );
    }

    void CmdExecuteIndirect( CommandBuffer* pCommandBuffer, CommandSignature const* pCommandSignature, uint32_t maxNumCommands, Buffer const* pIndirectBuffer, uint64_t indirectBufferOffset, Buffer const* pCounterBuffer, uint64_t counterBufferOffset )
    {
        VulkanCommandBuffer*            pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanCommandSignature const*   pVulkanCommandSignature = static_cast<VulkanCommandSignature const*>( pCommandSignature );
        VulkanBuffer const*             pVulkanIndirectBuffer = static_cast<VulkanBuffer const*>( pIndirectBuffer );
        VulkanBuffer const*             pVulkanCounterBuffer = static_cast<VulkanBuffer const*>( pCounterBuffer );

        EE_ASSERT( ( pVulkanIndirectBuffer->m_stride % IndirectCommandAlignment ) == 0 );
        EE_ASSERT( pVulkanIndirectBuffer->m_stride == pVulkanCommandSignature->m_stride );

        // Where the shader looks for its own command. Pushed for every signature, not only the ones
        // with root arguments, so an indirect-capable shader reads a defined block either way.
        if ( pVulkanCommandSignature->m_hasRootArguments )
        {
            EE_ASSERT( pVulkanIndirectBuffer->m_deviceAddress != 0 );

            VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pVulkanCommandBuffer->m_pBoundRootSignature );
            EE_ASSERT( pVulkanRootSignature != nullptr );

            IndirectRootPushConstants pushConstants = {};
            pushConstants.m_argumentBufferAddress = pVulkanIndirectBuffer->m_deviceAddress + indirectBufferOffset;
            pushConstants.m_stride = pVulkanCommandSignature->m_stride;
            pushConstants.m_commandIndexBase = 0;
            pushConstants.m_rootConstantOffset = uint32_t( Math::Max( pVulkanCommandSignature->m_rootConstantOffset, 0 ) );
            pushConstants.m_rootCbvOffset = uint32_t( Math::Max( pVulkanCommandSignature->m_rootCbvOffset, 0 ) );

            vkCmdPushConstants( pVulkanCommandBuffer->m_commandBuffer, pVulkanRootSignature->m_pipelineLayout,
                                VK_SHADER_STAGE_ALL, 0, sizeof( pushConstants ), &pushConstants );
        }

        // Nothing is bound when the pipeline was dropped, so there is no layout to push against.
        if ( !pVulkanCommandBuffer->m_boundPipelineIsNull )
        {
            BindUnboundRootParameters( pVulkanCommandBuffer );
        }

        // A draw stays inside the render pass and a dispatch may not, so an indirect draw wants
        // exactly what PrepareDraw gives an ordinary one.
        bool const isDraw = pVulkanCommandSignature->m_argumentType == IndirectArgumentType::Draw ||
                            pVulkanCommandSignature->m_argumentType == IndirectArgumentType::DrawIndexed ||
                            pVulkanCommandSignature->m_argumentType == IndirectArgumentType::DispatchMesh;

        if ( isDraw )
        {
            if ( !PrepareDraw( pVulkanCommandBuffer ) ) { return; }
        }
        else
        {
            FlushBarriers( pVulkanCommandBuffer );
            SuspendRendering( pVulkanCommandBuffer );
        }

        VkDeviceSize const argumentOffset = indirectBufferOffset + pVulkanCommandSignature->m_drawArgumentOffset;

        switch ( pVulkanCommandSignature->m_argumentType )
        {
            case IndirectArgumentType::Draw:
            {
                if ( pVulkanCounterBuffer != nullptr )
                {
                    vkCmdDrawIndirectCount( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset,
                                            pVulkanCounterBuffer->m_buffer, counterBufferOffset, maxNumCommands, pVulkanCommandSignature->m_stride );
                }
                else
                {
                    vkCmdDrawIndirect( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset,
                                       maxNumCommands, pVulkanCommandSignature->m_stride );
                }
            }
            break;

            case IndirectArgumentType::DrawIndexed:
            {
                if ( pVulkanCounterBuffer != nullptr )
                {
                    vkCmdDrawIndexedIndirectCount( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset,
                                                   pVulkanCounterBuffer->m_buffer, counterBufferOffset, maxNumCommands, pVulkanCommandSignature->m_stride );
                }
                else
                {
                    vkCmdDrawIndexedIndirect( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset,
                                              maxNumCommands, pVulkanCommandSignature->m_stride );
                }
            }
            break;

            case IndirectArgumentType::DispatchCompute:
            {
                // vkCmdDispatchIndirect runs exactly one dispatch and reads no count buffer, where
                // D3D12 runs min( maxNumCommands, count ). Vulkan has no indirect dispatch count at
                // all, so the count is spent on the CPU: one dispatch per possible command, each at
                // its own offset with its own command index pushed. A draw gets its index from
                // DrawIndex; a dispatch has no such builtin, which is why only this case loops.
                //
                // A command past the GPU-written count reads a stale slot, because nothing resets
                // the argument buffer between frames. The engine clears it, so an unwritten slot is
                // a (0,0,0) dispatch, which is a legal no-op.
                if ( maxNumCommands <= 1 )
                {
                    vkCmdDispatchIndirect( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset );
                }
                else
                {
                    VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pVulkanCommandBuffer->m_pBoundRootSignature );
                    EE_ASSERT( pVulkanRootSignature != nullptr );

                    IndirectRootPushConstants pushConstants = {};
                    pushConstants.m_argumentBufferAddress = pVulkanIndirectBuffer->m_deviceAddress + indirectBufferOffset;
                    pushConstants.m_stride = pVulkanCommandSignature->m_stride;
                    pushConstants.m_rootConstantOffset = uint32_t( Math::Max( pVulkanCommandSignature->m_rootConstantOffset, 0 ) );
                    pushConstants.m_rootCbvOffset = uint32_t( Math::Max( pVulkanCommandSignature->m_rootCbvOffset, 0 ) );

                    for ( uint32_t commandIndex = 0; commandIndex < maxNumCommands; ++commandIndex )
                    {
                        pushConstants.m_commandIndexBase = commandIndex;

                        vkCmdPushConstants( pVulkanCommandBuffer->m_commandBuffer, pVulkanRootSignature->m_pipelineLayout,
                                            VK_SHADER_STAGE_ALL, 0, sizeof( pushConstants ), &pushConstants );

                        vkCmdDispatchIndirect( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer,
                                               argumentOffset + VkDeviceSize( commandIndex ) * pVulkanCommandSignature->m_stride );
                    }
                }
            }
            break;

            case IndirectArgumentType::DispatchMesh:
            {
                EE_ASSERT( pVulkanCommandBuffer->m_vkCmdDrawMeshTasksIndirect != nullptr );

                if ( pVulkanCounterBuffer != nullptr )
                {
                    EE_ASSERT( pVulkanCommandBuffer->m_vkCmdDrawMeshTasksIndirectCount != nullptr );

                    pVulkanCommandBuffer->m_vkCmdDrawMeshTasksIndirectCount( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset,
                                                                             pVulkanCounterBuffer->m_buffer, counterBufferOffset, maxNumCommands, pVulkanCommandSignature->m_stride );
                }
                else
                {
                    pVulkanCommandBuffer->m_vkCmdDrawMeshTasksIndirect( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_buffer, argumentOffset,
                                                                        maxNumCommands, pVulkanCommandSignature->m_stride );
                }
            }
            break;

            case IndirectArgumentType::DispatchRays:
            {
                // vkCmdTraceRaysIndirect2KHR takes one address and no count, so it runs exactly one
                // trace where D3D12 runs min( maxNumCommands, count ).
                EE_ASSERT( pVulkanCommandBuffer->m_vkCmdTraceRaysIndirect2 != nullptr );
                EE_ASSERT( pVulkanCounterBuffer == nullptr );
                EE_ASSERT( maxNumCommands == 1 );

                pVulkanCommandBuffer->m_vkCmdTraceRaysIndirect2( pVulkanCommandBuffer->m_commandBuffer, pVulkanIndirectBuffer->m_deviceAddress + argumentOffset );
            }
            break;

            default:
            {
                EE_ASSERT( false );
            }
        }
    }

    // A copy and a clear are transfer commands, and neither may run inside dynamic rendering.
    // D3D12 has no such rule, so the engine closes nothing before either. The order matters: a
    // barrier the transfer depends on has to reach the device before the transfer does.
    static void PrepareTransfer( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        FlushBarriers( pVulkanCommandBuffer );
        SuspendRendering( pVulkanCommandBuffer );
    }

    // The engine never barriers a texture into a copy layout, because D3D12 has none: a texture it
    // copies to is created in TextureState::Common, which is already a legal copy target there.
    // Vulkan needs GENERAL or a TRANSFER layout, and the image is still UNDEFINED.
    //
    // GENERAL, not TRANSFER_DST_OPTIMAL, so the engine's belief stays true. The next barrier it
    // records names TextureState::Common as the source, Common is GENERAL, and CmdBarrier asserts
    // the two agree.
    //
    // The texture is const because RHI.h passes it that way to every copy. Its layout is not part
    // of what the caller sees, so recording the change is not a change to it.
    static void TransitionTextureForTransfer( VulkanCommandBuffer* pVulkanCommandBuffer, VulkanTexture const* pVulkanTexture )
    {
        if ( pVulkanTexture->CurrentLayout() == VK_IMAGE_LAYOUT_GENERAL )
        {
            return;
        }

        // Any other layout means the engine moved this texture and then copied it with no barrier
        // in between, which D3D12 would not accept either.
        EE_ASSERT( pVulkanTexture->CurrentLayout() == VK_IMAGE_LAYOUT_UNDEFINED );

        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = pVulkanTexture->CurrentLayout();
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pVulkanTexture->m_image;
        barrier.subresourceRange.aspectMask = pVulkanTexture->m_aspectMask;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = pVulkanTexture->m_mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = pVulkanTexture->m_arrayLayers;

        pVulkanCommandBuffer->m_imageBarriers.emplace_back( barrier );

        const_cast<VulkanTexture*>( pVulkanTexture )->SetLayout( VK_IMAGE_LAYOUT_GENERAL, 0, pVulkanTexture->m_mipLevels, 0, pVulkanTexture->m_arrayLayers );
    }

    // A D3D12 clear is a shader write and a Vulkan clear is a transfer write, and the engine
    // barriers for the first one. Its barrier names a shader storage write as the source, which
    // does not cover vkCmdFillBuffer, so the cleared counters would be read stale. The clear
    // records the transfer half of its own visibility barrier instead.
    //
    // ALL_COMMANDS on the destination, because nothing here knows what reads the resource next.
    static void RecordClearVisibilityBarrier( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        pVulkanCommandBuffer->m_globalBarriers.emplace_back( barrier );
    }

    // One aspect per copy. TextureCopyRegion has no plane index, and D3D12 builds its subresource
    // index with plane 0, which is the depth plane of a depth-stencil texture.
    static VkImageAspectFlags TransferAspectMask( VulkanTexture const* pVulkanTexture )
    {
        if ( ( pVulkanTexture->m_aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT ) != 0 )
        {
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        }

        return pVulkanTexture->m_aspectMask;
    }

    // A buffer image copy takes its row length in texels, and the engine lays its rows out at the
    // byte stride GetTextureCopyRowStride reports.
    static uint32_t CopyRowLengthInTexels( VulkanTexture const* pVulkanTexture, uint32_t mipLevel, uint32_t arrayLayer )
    {
        uint32_t const rowStride = GetTextureCopyRowStride( pVulkanTexture, mipLevel, arrayLayer );
        uint32_t const blockByteSize = FormatBlockBitSize( pVulkanTexture->m_format ) / 8;

        // A row length is a whole number of blocks or it cannot be expressed at all. It is one for
        // every format the engine uploads, because both the block size and the row alignment are
        // powers of two. A 96-bit format would break it, and nothing uses one.
        EE_ASSERT( ( rowStride % blockByteSize ) == 0 );

        return ( rowStride / blockByteSize ) * FormatBlockWidth( pVulkanTexture->m_format );
    }

    void CmdClearTexture( CommandBuffer* pCommandBuffer, Texture const* pTexture, uint32_t clearValue )
    {
        VulkanCommandBuffer*    pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanTexture const*    pVulkanTexture = static_cast<VulkanTexture const*>( pTexture );

        EE_ASSERT( pVulkanTexture->m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWTexture ) );
        EE_ASSERT( pVulkanTexture->m_aspectMask == VK_IMAGE_ASPECT_COLOR_BIT );

        TransitionTextureForTransfer( pVulkanCommandBuffer, pVulkanTexture );
        PrepareTransfer( pVulkanCommandBuffer );

        // The two backends read the clear value differently: D3D12 writes the raw bits through a
        // typed view, and vkCmdClearColorImage converts to the image format. They agree on an
        // integer format and disagree on a normalised one. Nothing calls this today.
        VkClearColorValue vulkanClearValue = {};
        vulkanClearValue.uint32[0] = clearValue;
        vulkanClearValue.uint32[1] = clearValue;
        vulkanClearValue.uint32[2] = clearValue;
        vulkanClearValue.uint32[3] = clearValue;

        // D3D12 clears one view per mip level. One subresource range covers every mip and layer.
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = pVulkanTexture->m_mipLevels;
        range.baseArrayLayer = 0;
        range.layerCount = pVulkanTexture->m_arrayLayers;

        vkCmdClearColorImage( pVulkanCommandBuffer->m_commandBuffer, pVulkanTexture->m_image,
                              VK_IMAGE_LAYOUT_GENERAL, &vulkanClearValue, 1, &range );

        RecordClearVisibilityBarrier( pVulkanCommandBuffer );
    }

    void CmdClearBuffer( CommandBuffer* pCommandBuffer, Buffer const* pBuffer, uint32_t clearValue )
    {
        VulkanCommandBuffer*    pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer const*     pVulkanBuffer = static_cast<VulkanBuffer const*>( pBuffer );

        EE_ASSERT( pVulkanBuffer->m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWBuffer ) );

        PrepareTransfer( pVulkanCommandBuffer );

        // vkCmdFillBuffer repeats one 32-bit value over the whole buffer, which agrees with D3D12
        // for every buffer the engine clears: counters and 32-bit typed buffers. A 16-bit format
        // would disagree. VK_WHOLE_SIZE rounds down to a multiple of 4, which is what a fill needs.
        vkCmdFillBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, 0, VK_WHOLE_SIZE, clearValue );

        RecordClearVisibilityBarrier( pVulkanCommandBuffer );
    }

    void CmdBuildAccelerationStructure( CommandBuffer* pCommandBuffer, TArrayView<AccelerationStructure* const> accelerationStructures, TArrayView<uint32_t const> bottomLevelAccelerationStructureIndices )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdBuildAccelerationStructures != nullptr );

        // A build is not a draw and may not run inside a render pass, so it takes the same pair
        // every copy and clear takes.
        PrepareTransfer( pVulkanCommandBuffer );

        // Bottom levels first, then top levels: a top level reads the bottom levels it references.
        for ( uint32_t const bottomLevelIndex : bottomLevelAccelerationStructureIndices )
        {
            EE_ASSERT( bottomLevelIndex < accelerationStructures.size() );

            VulkanAccelerationStructure* pVulkanAccelerationStructure = static_cast<VulkanAccelerationStructure*>( accelerationStructures[bottomLevelIndex] );
            VulkanBuffer const* pVulkanScratchBuffer = static_cast<VulkanBuffer const*>( pVulkanAccelerationStructure->m_pScratchBuffer );

            VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.flags = pVulkanAccelerationStructure->m_bottomLevel.m_flags;
            buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.dstAccelerationStructure = pVulkanAccelerationStructure->m_bottomLevel.m_handle;
            buildInfo.geometryCount = uint32_t( pVulkanAccelerationStructure->m_geometries.size() );
            buildInfo.pGeometries = pVulkanAccelerationStructure->m_geometries.data();
            buildInfo.scratchData.deviceAddress = pVulkanScratchBuffer->m_deviceAddress;

            VkAccelerationStructureBuildRangeInfoKHR const* pBuildRanges = pVulkanAccelerationStructure->m_buildRanges.data();
            pVulkanCommandBuffer->m_vkCmdBuildAccelerationStructures( pVulkanCommandBuffer->m_commandBuffer, 1, &buildInfo, &pBuildRanges );
        }

        for ( AccelerationStructure* pAccelerationStructure : accelerationStructures )
        {
            VulkanAccelerationStructure* pVulkanAccelerationStructure = static_cast<VulkanAccelerationStructure*>( pAccelerationStructure );
            VulkanBuffer const* pVulkanScratchBuffer = static_cast<VulkanBuffer const*>( pVulkanAccelerationStructure->m_pScratchBuffer );
            VulkanBuffer const* pVulkanInstanceBuffer = static_cast<VulkanBuffer const*>( pVulkanAccelerationStructure->m_pInstanceBuffer );

            // The D3D12 backend crashes here and this does not: it never fills in its instance
            // buffer and then dereferences it. CreateAccelerationStructure records ours.
            EE_ASSERT( pVulkanInstanceBuffer != nullptr );

            VkAccelerationStructureGeometryKHR topLevelGeometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            topLevelGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            topLevelGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            topLevelGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
            topLevelGeometry.geometry.instances.data.deviceAddress = pVulkanInstanceBuffer->m_deviceAddress + pVulkanAccelerationStructure->m_instanceBufferOffset;

            VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            buildInfo.flags = pVulkanAccelerationStructure->m_topLevel.m_flags;
            buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.dstAccelerationStructure = pVulkanAccelerationStructure->m_topLevel.m_handle;
            buildInfo.geometryCount = 1;
            buildInfo.pGeometries = &topLevelGeometry;
            buildInfo.scratchData.deviceAddress = pVulkanScratchBuffer->m_deviceAddress;

            VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
            buildRange.primitiveCount = uint32_t( pVulkanAccelerationStructure->m_numInstances );

            VkAccelerationStructureBuildRangeInfoKHR const* pBuildRange = &buildRange;
            pVulkanCommandBuffer->m_vkCmdBuildAccelerationStructures( pVulkanCommandBuffer->m_commandBuffer, 1, &buildInfo, &pBuildRange );
        }
    }

    //-------------------------------------------------------------------------
    // Barriers
    //-------------------------------------------------------------------------
    // The three CmdBarrier overloads mirror D3D12's enhanced barriers one for one, including the
    // batching: barriers are recorded on the command buffer and the whole set reaches the device in
    // one vkCmdPipelineBarrier2 at the next draw, dispatch or EndCommandBuffer.
    //
    // Two mappings below are deliberately broad, because nothing narrower says the same thing:
    // PipelineStage::All means exactly ALL_COMMANDS, and ResourceAccess::Common is D3D12's
    // "any access".

    static VkPipelineStageFlags2 VulkanPipelineStage( TBitFlags<PipelineStage> pipelineStages )
    {
        // All is not one bit among many, it is the answer.
        if ( pipelineStages.IsFlagSet( PipelineStage::All ) ) { return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; }

        VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_NONE;

        // ALL_GRAPHICS covers the depth test stages, and the engine relies on that: a depth target
        // is transitioned with PipelineStage::Draw, never with a depth stage of its own.
        if ( pipelineStages.IsFlagSet( PipelineStage::Draw ) ) { stageMask |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT; }
        if ( pipelineStages.IsFlagSet( PipelineStage::PixelShader ) ) { stageMask |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; }
        // D3D12's non-pixel shading includes compute, so this does too. The task and mesh bits go
        // in only when the extension is enabled, because naming a stage from a disabled extension
        // is a validation error, and without them a barrier before a mesh draw would not cover the
        // stage that reads the result.
        //
        // No tessellation or geometry stage, for the same reason and permanently: both are optional
        // features that CreateContext does not enable, and no shader in the engine declares either.
        if ( pipelineStages.IsFlagSet( PipelineStage::NonPixelShader ) )
        {
            stageMask |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

            if ( g_meshShaderEnabled )
            {
                stageMask |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
            }
        }
        if ( pipelineStages.IsFlagSet( PipelineStage::ComputeShader ) ) { stageMask |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; }
        if ( pipelineStages.IsFlagSet( PipelineStage::AllShader ) )
        {
            stageMask |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

            if ( g_meshShaderEnabled )
            {
                stageMask |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
            }
        }
        // ALL_TRANSFER rather than COPY, because the RHI has no separate flag for a clear or a
        // resolve, so both arrive here as Copy.
        if ( pipelineStages.IsFlagSet( PipelineStage::Copy ) ) { stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; }
        if ( pipelineStages.IsFlagSet( PipelineStage::ExecuteIndirect ) ) { stageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT; }

        // These name stages that only exist once their extension is enabled, and nothing can reach
        // them today. Mapped rather than left out, so whoever enables an extension finds it correct.
        if ( pipelineStages.IsFlagSet( PipelineStage::Raytracing ) ) { stageMask |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::BuildAccelerationStructure ) ) { stageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::CopyAccelerationStructure ) ) { stageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::VideoDecode ) ) { stageMask |= VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::VideoEncode ) ) { stageMask |= VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR; }
        // Vulkan has no video processing stage at all, so there is nothing to map D3D12's onto.

        return stageMask;
    }

    static VkAccessFlags2 VulkanAccess( TBitFlags<ResourceAccess> resourceAccess )
    {
        VkAccessFlags2 accessMask = VK_ACCESS_2_NONE;

        // D3D12's "any access", which Vulkan spells MEMORY_READ plus MEMORY_WRITE. Broad, but every
        // texture starts at Common, so this is what the first barrier on one uses as its source.
        if ( resourceAccess.IsFlagSet( ResourceAccess::Common ) ) { accessMask |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; }
        // A presentation engine read needs no access bits at all, only the PRESENT_SRC layout.
        if ( resourceAccess.IsFlagSet( ResourceAccess::Present ) ) { accessMask |= VK_ACCESS_2_NONE; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::ConstantBuffer ) ) { accessMask |= VK_ACCESS_2_UNIFORM_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::IndexBuffer ) ) { accessMask |= VK_ACCESS_2_INDEX_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::RenderTarget ) ) { accessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::UnorderedAccess ) ) { accessMask |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::DepthWrite ) ) { accessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::DepthRead ) ) { accessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT; }
        // A D3D12 shader resource view covers a sampled texture and a read-only structured buffer,
        // and Vulkan splits those into two access bits.
        if ( resourceAccess.IsFlagSet( ResourceAccess::ShaderResource ) ) { accessMask |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::IndirectArgument ) ) { accessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::CopyDestination ) ) { accessMask |= VK_ACCESS_2_TRANSFER_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::CopySource ) ) { accessMask |= VK_ACCESS_2_TRANSFER_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::AccelerationStructureRead ) ) { accessMask |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::AccelerationStructureWrite ) ) { accessMask |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::ShadingRateSource ) ) { accessMask |= VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::VideoDecodeRead ) ) { accessMask |= VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::VideoDecodeWrite ) ) { accessMask |= VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::VideoEncodeRead ) ) { accessMask |= VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::VideoEncodeWrite ) ) { accessMask |= VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR; }
        // VideoProcessRead and VideoProcessWrite have no Vulkan equivalent.

        return accessMask;
    }

    // The texture is needed because one state does not answer on its own; see ShaderResource.
    static VkImageLayout VulkanImageLayout( TextureState textureState, VulkanTexture const* pVulkanTexture )
    {
        switch ( textureState )
        {
            case TextureState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
            // D3D12's Common allows any access, and GENERAL is the Vulkan layout that does.
            case TextureState::Common: return VK_IMAGE_LAYOUT_GENERAL;
            // Not always SHADER_READ_ONLY_OPTIMAL: the sampled descriptor was written with
            // m_shaderReadLayout, and a descriptor's layout has to match the image's.
            case TextureState::ShaderResource: return pVulkanTexture->m_shaderReadLayout;
            case TextureState::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
            case TextureState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            case TextureState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case TextureState::DepthWrite: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case TextureState::DepthRead: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            case TextureState::ShadingRateSource: return VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
            case TextureState::VideoDecodeRead: return VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR;
            case TextureState::VideoDecodeWrite: return VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
            case TextureState::VideoEncodeRead: return VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
            case TextureState::VideoEncodeWrite: return VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR;
            // Vulkan has no video processing, so no layout for it either.
            case TextureState::VideoProcessRead:
            case TextureState::VideoProcessWrite: return VK_IMAGE_LAYOUT_GENERAL;
        }

        EE_UNREACHABLE_CODE();
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // A stage mask of NONE may carry no access bits, and the two are built from independent
    // arguments, so a caller can produce an empty sync with a non-empty access.
    static void NormalizeBarrierMasks( VkPipelineStageFlags2 stageMask, VkAccessFlags2& accessMask )
    {
        if ( stageMask == VK_PIPELINE_STAGE_2_NONE )
        {
            accessMask = VK_ACCESS_2_NONE;
        }
    }

    // Copied from the D3D12 backend, TODO comment and all. Vulkan is stricter, so this is not
    // optional here: naming a graphics stage in a barrier on a compute queue is a validation error.
    static void ClearGraphicsOnlyStages( VulkanCommandBuffer const* pVulkanCommandBuffer, TBitFlags<PipelineStage>& sourceSync, TBitFlags<PipelineStage> destinationSync )
    {
        if ( pVulkanCommandBuffer->m_pQueue->m_queueType != QueueType::Graphics )
        {
            sourceSync.ClearFlags( PipelineStageFlags_GraphicsQueueOnly );
            EE_ASSERT( !destinationSync.AreAnyFlagsSet( PipelineStage::Draw, PipelineStage::PixelShader ) );
        }
    }

    // A Vulkan barrier may only name stages its queue family can run, and a D3D12 one has no such
    // rule. The engine transitions a resource on whichever queue owns the work, naming the stage
    // that reads it next, and that reader is often on another queue. Only a device with dedicated
    // families notices, which is why an iGPU with one universal family never sees this.
    //
    // Dropping the stages the queue cannot run is correct, not a workaround. A barrier orders work
    // within one queue; the cross-queue half of the dependency is already carried by the timeline
    // semaphore every submit waits on. See RecordQueueOrderingWait.
    static VkPipelineStageFlags2 ClampStagesToQueue( VkPipelineStageFlags2 stageMask, VkQueueFlags queueFlags )
    {
        // ALL_COMMANDS is every stage the queue supports, so it is legal everywhere already.
        if ( stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT ) { return stageMask; }

        if ( !( queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
        {
            stageMask &= ~( VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT |
                            VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT );
        }

        if ( !( queueFlags & VK_QUEUE_COMPUTE_BIT ) )
        {
            stageMask &= ~( VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR );
        }

        // DRAW_INDIRECT covers the indirect argument fetch for a draw and for a dispatch, so
        // either capability keeps it.
        if ( !( queueFlags & ( VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT ) ) )
        {
            stageMask &= ~VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        }

        // Every named stage was dropped and the access mask is still set. An access bit requires a
        // compatible stage, so NONE would be a fresh validation error rather than a fix.
        // ALL_COMMANDS satisfies any access mask, and over-synchronising is the safe direction.
        if ( stageMask == VK_PIPELINE_STAGE_2_NONE ) { return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; }

        return stageMask;
    }

    static void FlushBarriers( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        if ( pVulkanCommandBuffer->m_globalBarriers.empty() &&
             pVulkanCommandBuffer->m_bufferBarriers.empty() &&
             pVulkanCommandBuffer->m_imageBarriers.empty() )
        {
            return;
        }

        // Clamped here because VulkanPipelineStage is handed no command buffer and cannot know the
        // queue. This is the one place every barrier passes through on its way to the device.
        VkQueueFlags const queueFlags = pVulkanCommandBuffer->m_queueFlags;

        for ( VkMemoryBarrier2& barrier : pVulkanCommandBuffer->m_globalBarriers )
        {
            barrier.srcStageMask = ClampStagesToQueue( barrier.srcStageMask, queueFlags );
            barrier.dstStageMask = ClampStagesToQueue( barrier.dstStageMask, queueFlags );
        }

        for ( VkBufferMemoryBarrier2& barrier : pVulkanCommandBuffer->m_bufferBarriers )
        {
            barrier.srcStageMask = ClampStagesToQueue( barrier.srcStageMask, queueFlags );
            barrier.dstStageMask = ClampStagesToQueue( barrier.dstStageMask, queueFlags );
        }

        for ( VkImageMemoryBarrier2& barrier : pVulkanCommandBuffer->m_imageBarriers )
        {
            barrier.srcStageMask = ClampStagesToQueue( barrier.srcStageMask, queueFlags );
            barrier.dstStageMask = ClampStagesToQueue( barrier.dstStageMask, queueFlags );
        }

        // A barrier may not run inside dynamic rendering. This is the one place that has to know
        // it, which is why every other caller goes through the flush. The pass resumes at the next draw.
        SuspendRendering( pVulkanCommandBuffer );

        VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependencyInfo.memoryBarrierCount = uint32_t( pVulkanCommandBuffer->m_globalBarriers.size() );
        dependencyInfo.pMemoryBarriers = pVulkanCommandBuffer->m_globalBarriers.data();
        dependencyInfo.bufferMemoryBarrierCount = uint32_t( pVulkanCommandBuffer->m_bufferBarriers.size() );
        dependencyInfo.pBufferMemoryBarriers = pVulkanCommandBuffer->m_bufferBarriers.data();
        dependencyInfo.imageMemoryBarrierCount = uint32_t( pVulkanCommandBuffer->m_imageBarriers.size() );
        dependencyInfo.pImageMemoryBarriers = pVulkanCommandBuffer->m_imageBarriers.data();

        vkCmdPipelineBarrier2( pVulkanCommandBuffer->m_commandBuffer, &dependencyInfo );

        pVulkanCommandBuffer->m_globalBarriers.clear();
        pVulkanCommandBuffer->m_bufferBarriers.clear();
        pVulkanCommandBuffer->m_imageBarriers.clear();
    }

    void CmdBarrier( CommandBuffer* pCommandBuffer, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        ClearGraphicsOnlyStages( pVulkanCommandBuffer, sourceSync, destinationSync );

        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VulkanPipelineStage( sourceSync );
        barrier.dstStageMask = VulkanPipelineStage( destinationSync );
        barrier.srcAccessMask = VulkanAccess( sourceAccess );
        barrier.dstAccessMask = VulkanAccess( destinationAccess );

        NormalizeBarrierMasks( barrier.srcStageMask, barrier.srcAccessMask );
        NormalizeBarrierMasks( barrier.dstStageMask, barrier.dstAccessMask );

        pVulkanCommandBuffer->m_globalBarriers.emplace_back( barrier );
    }

    void CmdBarrier( CommandBuffer* pCommandBuffer, Buffer* pBuffer, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );

        ClearGraphicsOnlyStages( pVulkanCommandBuffer, sourceSync, destinationSync );

        VkBufferMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VulkanPipelineStage( sourceSync );
        barrier.dstStageMask = VulkanPipelineStage( destinationSync );
        barrier.srcAccessMask = VulkanAccess( sourceAccess );
        barrier.dstAccessMask = VulkanAccess( destinationAccess );
        // No queue ownership transfer: every resource is created CONCURRENT across the families the
        // context uses, which is what a D3D12 resource effectively has. See SetSharingMode.
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = pVulkanBuffer->m_buffer;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;

        NormalizeBarrierMasks( barrier.srcStageMask, barrier.srcAccessMask );
        NormalizeBarrierMasks( barrier.dstStageMask, barrier.dstAccessMask );

        pVulkanCommandBuffer->m_bufferBarriers.emplace_back( barrier );
    }

    // sourceState is unnamed on purpose: this function reads the old layout from the texture instead,
    // for the reason the comment on barrier.oldLayout below gives. The parameter stays because the
    // signature is RHI.h's and the Direct3D 12 backend does use it.
    void CmdBarrier( CommandBuffer* pCommandBuffer, Texture* pTexture, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess, TextureState /* sourceState */, TextureState destinationState, TextureBarrierRegion region, TBitFlags<TextureBarrierFlags> flags )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( pTexture );

        ClearGraphicsOnlyStages( pVulkanCommandBuffer, sourceSync, destinationSync );

        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VulkanPipelineStage( sourceSync );
        barrier.dstStageMask = VulkanPipelineStage( destinationSync );
        barrier.srcAccessMask = VulkanAccess( sourceAccess );
        barrier.dstAccessMask = VulkanAccess( destinationAccess );

        // The old layout comes from the texture, not from sourceState. A texture the engine believes
        // is already in its m_initialState is in fact in UNDEFINED until the first barrier moves it.
        //
        // sourceState is allowed to disagree, and is not checked. CmdSetRenderTargets transitions
        // attachments the engine never barriered, so the engine's tracked state lags the image
        // routinely. The barrier is correct either way, because oldLayout is read from the texture.
        barrier.oldLayout = pVulkanTexture->CurrentLayout();
        barrier.newLayout = VulkanImageLayout( destinationState, pVulkanTexture );

        // The discard flag says the old contents are not needed, which is what UNDEFINED as an old
        // layout means. It lets the driver skip decompressing what it is about to overwrite.
        if ( flags.IsFlagSet( TextureBarrierFlags::Discard ) )
        {
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pVulkanTexture->m_image;
        // Every aspect the image has. The engine never asks to move depth without stencil.
        barrier.subresourceRange.aspectMask = pVulkanTexture->m_aspectMask;
        barrier.subresourceRange.baseMipLevel = region.m_mipLevel;
        barrier.subresourceRange.levelCount = region.m_numMipLevels ? region.m_numMipLevels : pVulkanTexture->m_mipLevels;
        barrier.subresourceRange.baseArrayLayer = region.m_arraySlice;
        barrier.subresourceRange.layerCount = region.m_numArraySlices ? region.m_numArraySlices : pVulkanTexture->m_arrayLayers;

        NormalizeBarrierMasks( barrier.srcStageMask, barrier.srcAccessMask );
        NormalizeBarrierMasks( barrier.dstStageMask, barrier.dstAccessMask );

        // One barrier per old layout, not one per call. The engine barriers a whole texture, and its
        // subresources can legitimately be in different layouts by then, because CmdSetRenderTargets
        // transitions the one face it is about to draw into. A single barrier naming one old layout
        // would be wrong for every other face, which Vulkan rejects outright.
        uint32_t const baseMip = barrier.subresourceRange.baseMipLevel;
        uint32_t const numMips = barrier.subresourceRange.levelCount;
        uint32_t const baseLayer = barrier.subresourceRange.baseArrayLayer;
        uint32_t const numLayers = barrier.subresourceRange.layerCount;

        bool layoutsAgree = true;
        for ( uint32_t mip = baseMip; mip < baseMip + numMips && layoutsAgree; ++mip )
        {
            for ( uint32_t layer = baseLayer; layer < baseLayer + numLayers; ++layer )
            {
                if ( pVulkanTexture->CurrentLayout( mip, layer ) != barrier.oldLayout )
                {
                    layoutsAgree = false;
                    break;
                }
            }
        }

        if ( layoutsAgree )
        {
            pVulkanCommandBuffer->m_imageBarriers.emplace_back( barrier );
        }
        else
        {
            for ( uint32_t mip = baseMip; mip < baseMip + numMips; ++mip )
            {
                for ( uint32_t layer = baseLayer; layer < baseLayer + numLayers; ++layer )
                {
                    VkImageMemoryBarrier2 subresourceBarrier = barrier;
                    subresourceBarrier.oldLayout = flags.IsFlagSet( TextureBarrierFlags::Discard ) ? VK_IMAGE_LAYOUT_UNDEFINED : pVulkanTexture->CurrentLayout( mip, layer );
                    subresourceBarrier.subresourceRange.baseMipLevel = mip;
                    subresourceBarrier.subresourceRange.levelCount = 1;
                    subresourceBarrier.subresourceRange.baseArrayLayer = layer;
                    subresourceBarrier.subresourceRange.layerCount = 1;

                    if ( subresourceBarrier.oldLayout != subresourceBarrier.newLayout )
                    {
                        pVulkanCommandBuffer->m_imageBarriers.emplace_back( subresourceBarrier );
                    }
                }
            }
        }

        pVulkanTexture->SetLayout( barrier.newLayout, baseMip, numMips, baseLayer, numLayers );
    }

    //-------------------------------------------------------------------------
    // Queries
    //-------------------------------------------------------------------------
    // Nothing in the engine calls any of this; it is API surface written for parity.
    //
    // A timestamp needs a begin and an end on D3D12 and only an end on Vulkan, so CmdBeginQuery
    // does nothing for one. That matches what D3D12 achieves anyway: BeginQuery rejects a timestamp
    // query, so its own begin is a no-op with a debug layer complaint attached.

    void CmdResetQueryPool( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, uint32_t startQuery, uint32_t numQueries )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanQueryPool*     pVulkanQueryPool = static_cast<VulkanQueryPool*>( pQueryPool );

        EE_ASSERT( startQuery + numQueries <= pVulkanQueryPool->m_numQueries );

        // A query is undefined until it has been reset, and a reset may not run inside a render
        // pass. D3D12 does nothing here at all.
        PrepareTransfer( pVulkanCommandBuffer );

        vkCmdResetQueryPool( pVulkanCommandBuffer->m_commandBuffer, pVulkanQueryPool->m_queryPool, startQuery, numQueries );
    }

    void CmdBeginQuery( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, uint32_t queryIndex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanQueryPool*     pVulkanQueryPool = static_cast<VulkanQueryPool*>( pQueryPool );

        EE_ASSERT( queryIndex < pVulkanQueryPool->m_numQueries );

        // A timestamp is written at one point, not over a range, so it has no begin. CmdEndQuery
        // writes it. vkCmdBeginQuery on a timestamp pool is a validation error.
        if ( pVulkanQueryPool->m_queryType == VK_QUERY_TYPE_TIMESTAMP )
        {
            return;
        }

        vkCmdBeginQuery( pVulkanCommandBuffer->m_commandBuffer, pVulkanQueryPool->m_queryPool, queryIndex, 0 );
    }

    void CmdEndQuery( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, uint32_t queryIndex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanQueryPool*     pVulkanQueryPool = static_cast<VulkanQueryPool*>( pQueryPool );

        EE_ASSERT( queryIndex < pVulkanQueryPool->m_numQueries );

        if ( pVulkanQueryPool->m_queryType == VK_QUERY_TYPE_TIMESTAMP )
        {
            // A queue family may report zero valid timestamp bits, meaning it cannot write one at
            // all. D3D12 has no equivalent, so a caller would never think to check.
            EE_ASSERT( pVulkanCommandBuffer->m_pQueue != nullptr );
            EE_ASSERT( static_cast<VulkanQueue const*>( pVulkanCommandBuffer->m_pQueue )->m_timestampValidBits > 0 );

            // BOTTOM_OF_PIPE, because D3D12's EndQuery timestamp is taken after the work the scope
            // covers. Legal inside a render pass, so a profile scope does not tear one.
            vkCmdWriteTimestamp2( pVulkanCommandBuffer->m_commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                  pVulkanQueryPool->m_queryPool, queryIndex );
            return;
        }

        vkCmdEndQuery( pVulkanCommandBuffer->m_commandBuffer, pVulkanQueryPool->m_queryPool, queryIndex );
    }

    void CmdResolveQuery( CommandBuffer* pCommandBuffer, QueryPool* pQueryPool, Buffer const* pReadbackBuffer, uint32_t startQuery, uint32_t numQueries )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanQueryPool*     pVulkanQueryPool = static_cast<VulkanQueryPool*>( pQueryPool );
        VulkanBuffer const*  pVulkanReadbackBuffer = static_cast<VulkanBuffer const*>( pReadbackBuffer );

        EE_ASSERT( startQuery + numQueries <= pVulkanQueryPool->m_numQueries );

        // Eight bytes per query is only right for a timestamp, and it is the offset D3D12 writes.
        // A pipeline statistics query resolves to eleven counters, so this asserts rather than
        // inventing a second layout. Nothing creates a statistics pool.
        EE_ASSERT( pVulkanQueryPool->m_queryType == VK_QUERY_TYPE_TIMESTAMP );

        PrepareTransfer( pVulkanCommandBuffer );

        // WAIT_BIT, because ResolveQueryData reads finished results. Without it the copy could write
        // nothing and report availability separately.
        vkCmdCopyQueryPoolResults( pVulkanCommandBuffer->m_commandBuffer, pVulkanQueryPool->m_queryPool,
                                   startQuery, numQueries,
                                   pVulkanReadbackBuffer->m_buffer, uint64_t( startQuery ) * 8, 8,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );
    }

    void CmdCopyBuffer( CommandBuffer* pCommandBuffer, Buffer const* pDstBuffer, uint64_t dstOffset, Buffer const* pSrcBuffer, uint64_t srcOffset, uint64_t srcSize )
    {
        VulkanCommandBuffer*    pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer const*     pVulkanDstBuffer = static_cast<VulkanBuffer const*>( pDstBuffer );
        VulkanBuffer const*     pVulkanSrcBuffer = static_cast<VulkanBuffer const*>( pSrcBuffer );

        PrepareTransfer( pVulkanCommandBuffer );

        VkBufferCopy region = {};
        region.srcOffset = srcOffset;
        region.dstOffset = dstOffset;
        region.size = srcSize;

        vkCmdCopyBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanSrcBuffer->m_buffer, pVulkanDstBuffer->m_buffer, 1, &region );
    }

    void CmdCopyTexture( CommandBuffer* pCommandBuffer, Texture const* pDstTexture, TextureCopyRegion const& dstRegion, Buffer const* pSrcBuffer, uint64_t srcOffset )
    {
        VulkanCommandBuffer*    pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanTexture const*    pVulkanDstTexture = static_cast<VulkanTexture const*>( pDstTexture );
        VulkanBuffer const*     pVulkanSrcBuffer = static_cast<VulkanBuffer const*>( pSrcBuffer );

        EE_ASSERT( dstRegion.m_mipLevel < pVulkanDstTexture->m_mipLevels );
        EE_ASSERT( dstRegion.m_arrayLayer < pVulkanDstTexture->m_arrayLayers );

        TransitionTextureForTransfer( pVulkanCommandBuffer, pVulkanDstTexture );
        PrepareTransfer( pVulkanCommandBuffer );

        VkBufferImageCopy region = {};
        region.bufferOffset = srcOffset;
        region.bufferRowLength = CopyRowLengthInTexels( pVulkanDstTexture, dstRegion.m_mipLevel, dstRegion.m_arrayLayer );
        // Zero means the rows are packed to imageExtent.height, which is how the engine writes them.
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = TransferAspectMask( pVulkanDstTexture );
        region.imageSubresource.mipLevel = dstRegion.m_mipLevel;
        // One layer per copy, as the D3D12 subresource index names one. A 3D texture has one layer
        // and puts its slices in m_z and m_depth instead.
        region.imageSubresource.baseArrayLayer = dstRegion.m_arrayLayer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { int32_t( dstRegion.m_x ), int32_t( dstRegion.m_y ), int32_t( dstRegion.m_z ) };
        region.imageExtent = { dstRegion.m_width, dstRegion.m_height, dstRegion.m_depth };

        vkCmdCopyBufferToImage( pVulkanCommandBuffer->m_commandBuffer, pVulkanSrcBuffer->m_buffer,
                                pVulkanDstTexture->m_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region );
    }

    void CmdCopyTexture( CommandBuffer* pCommandBuffer, Buffer const* pDstBuffer, uint64_t dstOffset, Texture const* pSrcTexture, TextureCopyRegion const& srcRegion )
    {
        VulkanCommandBuffer*    pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer const*     pVulkanDstBuffer = static_cast<VulkanBuffer const*>( pDstBuffer );
        VulkanTexture const*    pVulkanSrcTexture = static_cast<VulkanTexture const*>( pSrcTexture );

        EE_ASSERT( srcRegion.m_mipLevel < pVulkanSrcTexture->m_mipLevels );
        EE_ASSERT( srcRegion.m_arrayLayer < pVulkanSrcTexture->m_arrayLayers );

        TransitionTextureForTransfer( pVulkanCommandBuffer, pVulkanSrcTexture );
        PrepareTransfer( pVulkanCommandBuffer );

        VkBufferImageCopy region = {};
        region.bufferOffset = dstOffset;
        // The same stride the upload path uses, so a caller sizes its readback buffer with one
        // function for both directions. D3D12 uses the destination buffer's own footprint, which
        // for a buffer resource says nothing about texture rows. Nothing calls this yet.
        region.bufferRowLength = CopyRowLengthInTexels( pVulkanSrcTexture, srcRegion.m_mipLevel, srcRegion.m_arrayLayer );
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = TransferAspectMask( pVulkanSrcTexture );
        region.imageSubresource.mipLevel = srcRegion.m_mipLevel;
        region.imageSubresource.baseArrayLayer = srcRegion.m_arrayLayer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { int32_t( srcRegion.m_x ), int32_t( srcRegion.m_y ), int32_t( srcRegion.m_z ) };
        region.imageExtent = { srcRegion.m_width, srcRegion.m_height, srcRegion.m_depth };

        vkCmdCopyImageToBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanSrcTexture->m_image,
                                VK_IMAGE_LAYOUT_GENERAL, pVulkanDstBuffer->m_buffer, 1, &region );
    }

    //-------------------------------------------------------------------------
    // Debug markers
    //-------------------------------------------------------------------------
    // VK_EXT_debug_utils labels. The extension is enabled whenever the loader has it, with or
    // without the validation layer, so markers are present in a Release build too.

    void CmdBeginDebugMarker( CommandBuffer* pCommandBuffer, char const* pName )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // Counted even when the extension is missing, so the balance assert in EndCommandBuffer
        // still catches an unmatched scope on a machine without it.
        pVulkanCommandBuffer->m_debugMarkerScopeCounter++;

        if ( pVulkanCommandBuffer->m_vkCmdBeginDebugUtilsLabel == nullptr )
        {
            return;
        }

        // Stolen from ImGui::ColorConvertHSVtoRGB, as the D3D12 backend does. Copied rather than
        // shared because RHI.h holds no such helper.
        auto HSVtoRGB = [] ( float h, float s, float v, float& out_r, float& out_g, float& out_b )
        {
            if ( s == 0.0f )
            {
                // gray
                out_r = out_g = out_b = v;
                return;
            }

            h = Math::FModF( h, 1.0f ) / ( 60.0f / 360.0f );
            int   i = (int) h;
            float f = h - (float) i;
            float p = v * ( 1.0f - s );
            float q = v * ( 1.0f - s * f );
            float t = v * ( 1.0f - s * ( 1.0f - f ) );

            switch ( i )
            {
                case 0: out_r = v; out_g = t; out_b = p; break;
                case 1: out_r = q; out_g = v; out_b = p; break;
                case 2: out_r = p; out_g = v; out_b = t; break;
                case 3: out_r = p; out_g = q; out_b = v; break;
                case 4: out_r = t; out_g = p; out_b = v; break;
                case 5: default: out_r = v; out_g = p; out_b = q; break;
            }
        };

        float h = pVulkanCommandBuffer->m_currentDebugMarkerColorValue;
        float s = 0.5F;
        float v = 0.95F;

        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        HSVtoRGB( h, s, v, r, g, b );

        // https://martin.ankerl.com/2009/12/09/how-to-create-random-colors-programmatically/
        // Random colors that are consistent and distinct from each other
        pVulkanCommandBuffer->m_currentDebugMarkerColorValue += 0.618033988749895F;
        pVulkanCommandBuffer->m_currentDebugMarkerColorValue = Math::FModF( pVulkanCommandBuffer->m_currentDebugMarkerColorValue, 1.0F );

        VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
        label.pLabelName = pName;
        // Floats where PIX takes bytes. Same colour, no rounding through 0-255.
        label.color[0] = r;
        label.color[1] = g;
        label.color[2] = b;
        label.color[3] = 1.0F;

        pVulkanCommandBuffer->m_vkCmdBeginDebugUtilsLabel( pVulkanCommandBuffer->m_commandBuffer, &label );
    }

    void CmdEndDebugMarker( CommandBuffer* pCommandBuffer )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        pVulkanCommandBuffer->m_debugMarkerScopeCounter--;
        EE_ASSERT( pVulkanCommandBuffer->m_debugMarkerScopeCounter >= 0 );

        if ( pVulkanCommandBuffer->m_vkCmdEndDebugUtilsLabel == nullptr )
        {
            return;
        }

        pVulkanCommandBuffer->m_vkCmdEndDebugUtilsLabel( pVulkanCommandBuffer->m_commandBuffer );
    }

    // The breadcrumb write, which nothing in the engine calls. D3D12's WriteBufferImmediate has
    // MARKER_IN and MARKER_OUT modes that order the write against everything already submitted;
    // the Vulkan equivalent is VK_AMD_buffer_marker, which is not enabled here.
    //
    // vkCmdFillBuffer writes the same value at the same point in the command stream, so it matches
    // the default mode exactly and approximates the other two. m_breadcrumbs is false on this
    // backend, so nothing asks for the tighter ordering.
    uint32_t CmdWriteDebugMarker( CommandBuffer* pCommandBuffer, TBitFlags<MarkerTypeFlags> const& markerType, uint32_t markerValue, Buffer* pBuffer, size_t offset, bool useAutoFlags )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer*        pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );

        VkDeviceSize const bufferOffset = offset * sizeof( uint32_t );
        EE_ASSERT( bufferOffset + sizeof( uint32_t ) <= pVulkanBuffer->m_size );

        // A fill is a transfer command, so it leaves the render pass the way every copy and clear does.
        PrepareTransfer( pVulkanCommandBuffer );

        if ( markerType == TBitFlags( MarkerTypeFlags::InOut ) )
        {
            // Two writes to one address. The In value does not survive here and it does on D3D12,
            // which writes it at the top of the pipe and the Out value at the bottom, so a crash
            // between the two leaves the In value behind. Two fills run in order and the second
            // overwrites the first. Closing that needs VK_AMD_buffer_marker.
            //
            // The enum values below, not the bit field's, matching what the D3D12 backend does.
            uint32_t const inValue = markerValue | ( useAutoFlags ? ( uint32_t( MarkerTypeFlags::In ) << 30 ) : 0 );
            uint32_t const outValue = markerValue | ( useAutoFlags ? ( uint32_t( MarkerTypeFlags::Out ) << 30 ) : 0 );

            vkCmdFillBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, bufferOffset, sizeof( uint32_t ), inValue );
            vkCmdFillBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, bufferOffset, sizeof( uint32_t ), outValue );
        }
        else
        {
            // The bit field here, again matching the D3D12 backend.
            uint32_t const value = markerValue | ( useAutoFlags ? ( markerType.Get() << 30 ) : 0 );

            vkCmdFillBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, bufferOffset, sizeof( uint32_t ), value );
        }

        // No visibility barrier, unlike the clears. A breadcrumb is read by the host after the fact,
        // and submission plus a host wait is what makes a transfer write visible there.

        return markerValue;
    }

    CommandSignature* CreateCommandSignature( Context* pContext, CommandSignatureParameters const& parameters )
    {
        VulkanContext*          pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanCommandSignature* pVulkanCommandSignature = pVulkanContext->CreateObject<VulkanCommandSignature>();

        // There is no Vulkan object to create. A command signature is a D3D12 concept, and the
        // Vulkan side of it is the byte layout of one command. The sizes accumulated below must
        // match D3D12's exactly, because both backends read one buffer that a shader wrote.
        uint32_t commandStride = 0;

        for ( IndirectArgumentDescriptor const& argument : parameters.m_indirectArgumentParameters )
        {
            DescriptorReflection const* pDescriptorReflection = nullptr;
            if ( argument.m_type != IndirectArgumentType::Draw &&
                 argument.m_type != IndirectArgumentType::DrawIndexed &&
                 argument.m_type != IndirectArgumentType::DispatchCompute &&
                 argument.m_type != IndirectArgumentType::DispatchMesh &&
                 argument.m_type != IndirectArgumentType::DispatchRays )
            {
                EE_ASSERT( parameters.m_pRootSignature != nullptr );
                EE_ASSERT( argument.m_index < parameters.m_pRootSignature->m_descriptorReflections.size() );
                pDescriptorReflection = &parameters.m_pRootSignature->m_descriptorReflections[argument.m_index];
            }

            switch ( argument.m_type )
            {
                // The draw and dispatch arguments end the command, and their byte offset is what
                // CmdExecuteIndirect hands to Vulkan.
                case IndirectArgumentType::Draw:
                {
                    pVulkanCommandSignature->m_argumentType = argument.m_type;
                    pVulkanCommandSignature->m_drawArgumentOffset = commandStride;
                    commandStride += sizeof( IndirectDrawArguments );
                }
                break;

                case IndirectArgumentType::DrawIndexed:
                {
                    pVulkanCommandSignature->m_argumentType = argument.m_type;
                    pVulkanCommandSignature->m_drawArgumentOffset = commandStride;
                    commandStride += sizeof( IndirectDrawIndexedArguments );
                }
                break;

                case IndirectArgumentType::DispatchCompute:
                case IndirectArgumentType::DispatchMesh:
                case IndirectArgumentType::DispatchRays:
                {
                    pVulkanCommandSignature->m_argumentType = argument.m_type;
                    pVulkanCommandSignature->m_drawArgumentOffset = commandStride;
                    commandStride += sizeof( IndirectDispatchArguments );
                }
                break;

                // Everything below is a root argument, which is the half Vulkan cannot execute. The
                // sizes are still counted, because the stride has to match what the shader wrote.
                case IndirectArgumentType::VertexBuffer:
                case IndirectArgumentType::IndexBuffer:
                {
                    // A D3D12 buffer view is an address, a size and a stride or format.
                    commandStride += 16;
                    pVulkanCommandSignature->m_hasRootArguments = true;
                }
                break;

                case IndirectArgumentType::Constant:
                {
                    EE_ASSERT( argument.m_byteSize == sizeof( uint32_t ) * pDescriptorReflection->m_numConstants );

                    // The shader reads this block itself, so where it sits is not just bookkeeping.
                    EE_ASSERT( pVulkanCommandSignature->m_rootConstantOffset == -1 );
                    pVulkanCommandSignature->m_rootConstantOffset = int32_t( commandStride );

                    commandStride += sizeof( uint32_t ) * pDescriptorReflection->m_numConstants;
                    pVulkanCommandSignature->m_hasRootArguments = true;
                }
                break;

                case IndirectArgumentType::ConstantBufferView:
                {
                    // A GPU virtual address. A Vulkan descriptor takes a buffer and an offset, so
                    // there is nothing to turn one back into, but a shader can dereference the
                    // address directly, which is what EE_DECLARE_INDIRECT_ROOT_CBV does.
                    EE_ASSERT( pVulkanCommandSignature->m_rootCbvOffset == -1 );
                    pVulkanCommandSignature->m_rootCbvOffset = int32_t( commandStride );

                    commandStride += 8;
                    pVulkanCommandSignature->m_hasRootArguments = true;
                }
                break;

                case IndirectArgumentType::ShaderResourceView:
                case IndirectArgumentType::UnorderedAccessView:
                {
                    // Also an address, and no shader reads these indirectly. Only root constants and
                    // the root CBV have indirect declarations. A signature can still carry one, so
                    // its stride is counted; indexing it would need a third declaration macro.
                    commandStride += 8;
                    pVulkanCommandSignature->m_hasRootArguments = true;
                }
                break;

                case IndirectArgumentType::Invalid:
                default:
                {
                    EE_ASSERT( false );
                }
            }
        }

        EE_ASSERT( pVulkanCommandSignature->m_argumentType != IndirectArgumentType::Invalid );

        pVulkanCommandSignature->m_stride = Math::RoundUpToNearestMultiple32( commandStride, IndirectCommandAlignment );

        return pVulkanCommandSignature;
    }

    void DestroyCommandSignature( Context* pContext, CommandSignature*&& pCommandSignature )
    {
        if ( pCommandSignature != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanCommandSignature* pVulkanCommandSignature = static_cast<VulkanCommandSignature*>( pCommandSignature );

            pVulkanContext->DestroyObject( eastl::move( pVulkanCommandSignature ) );
            pCommandSignature = nullptr;
        }
    }

    //-------------------------------------------------------------------------
    // Raytracing
    //-------------------------------------------------------------------------
    // Nothing in the engine reaches any of this, on either backend: no code creates an acceleration
    // structure, no shader calls GetRaytracingAccelerationStructure, and RHI.h declares no way to
    // build a RaytracingShaderTable. Written for parity, and unverified.
    //
    // GetAccelerationStructureHandle returns a buffer handle on the structure buffer, as D3D12
    // does, so the acceleration structure descriptor type never has to join the heap's mutable
    // list. That changes the day a shader actually reads one.

    static VkBuildAccelerationStructureFlagsKHR VulkanAccelerationStructureBuildFlags( TBitFlags<AccelerationStructureBuildFlags> flags )
    {
        VkBuildAccelerationStructureFlagsKHR result = 0;

        if ( flags.IsFlagSet( AccelerationStructureBuildFlags::AllowUpdate ) )      { result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR; }
        if ( flags.IsFlagSet( AccelerationStructureBuildFlags::AllowCompaction ) )  { result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR; }
        if ( flags.IsFlagSet( AccelerationStructureBuildFlags::PreferFastTrace ) )  { result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; }
        if ( flags.IsFlagSet( AccelerationStructureBuildFlags::PreferFastBuild ) )  { result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; }
        if ( flags.IsFlagSet( AccelerationStructureBuildFlags::MinimizeMemory ) )   { result |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR; }
        // PerformUpdate is not a creation flag on Vulkan. It is the build mode, UPDATE against
        // BUILD, which CmdBuildAccelerationStructure reads back out of the RHI flags.

        return result;
    }

    static VkGeometryFlagsKHR VulkanGeometryFlags( TBitFlags<AccelerationStructureGeometryFlags> flags )
    {
        VkGeometryFlagsKHR result = 0;

        if ( flags.IsFlagSet( AccelerationStructureGeometryFlags::Opaque ) )                        { result |= VK_GEOMETRY_OPAQUE_BIT_KHR; }
        if ( flags.IsFlagSet( AccelerationStructureGeometryFlags::NoDuplicateAnyhitInvocation ) )   { result |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR; }

        return result;
    }

    // The structure buffer and the scratch buffer, both device local and both sized by the driver.
    static Buffer* CreateAccelerationStructureBuffer( Context* pContext, uint64_t size, bool needsDescriptor, char const* pDebugName )
    {
        BufferParameters bufferParameters = {};
        bufferParameters.m_bufferSize = size;
        bufferParameters.m_bufferStride = sizeof( uint32_t );
        bufferParameters.m_memoryType = ResourceMemoryType::DeviceLocal;
        bufferParameters.m_debugName = pDebugName;

        if ( needsDescriptor )
        {
            // A deliberate divergence: the D3D12 backend creates this buffer without the descriptor
            // GetAccelerationStructureHandle then asks it for, and would assert if anything called
            // it. This buffer gets the descriptor it is about to be asked for.
            bufferParameters.m_descriptorTypes.SetMultipleFlags( DescriptorTypeFlags::Buffer, DescriptorTypeFlags::RWBuffer, DescriptorTypeFlags::Raw );
        }
        else
        {
            bufferParameters.m_descriptorTypes = DescriptorTypeFlags::RWBuffer;
            bufferParameters.m_flags.SetMultipleFlags( BufferFlags::OwnMemory, BufferFlags::NoDescriptors );
        }

        return CreateBuffer( pContext, bufferParameters );
    }

    AccelerationStructure* CreateAccelerationStructure( Context* pContext, AccelerationStructureTopLevelCreateParameters const& topLevelParameters, AccelerationStructureBottomLevelCreateParameters const& bottomLevelParameters )
    {
        VulkanContext*                 pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanAccelerationStructure*   pVulkanAccelerationStructure = pVulkanContext->CreateObject<VulkanAccelerationStructure>();

        EE_ASSERT( pVulkanContext->m_raytracing );

        pVulkanAccelerationStructure->m_bottomLevel.m_flags = VulkanAccelerationStructureBuildFlags( bottomLevelParameters.m_flags );
        pVulkanAccelerationStructure->m_topLevel.m_flags = VulkanAccelerationStructureBuildFlags( topLevelParameters.m_flags );

        // Bottom level
        //-------------------------------------------------------------------------
        TInlineVector<uint32_t, 8> maxPrimitiveCounts;

        pVulkanAccelerationStructure->m_geometries.reserve( bottomLevelParameters.m_geometries.size() );
        pVulkanAccelerationStructure->m_buildRanges.reserve( bottomLevelParameters.m_geometries.size() );

        for ( AccelerationStructureGeometry const& geometry : bottomLevelParameters.m_geometries )
        {
            VulkanBuffer const* pVulkanIndexBuffer = static_cast<VulkanBuffer const*>( geometry.m_pIndexBuffer );
            VulkanBuffer const* pVulkanVertexBuffer = static_cast<VulkanBuffer const*>( geometry.m_pVertexBuffer );

            VkAccelerationStructureGeometryKHR vulkanGeometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            vulkanGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            vulkanGeometry.flags = VulkanGeometryFlags( geometry.m_flags );

            VkAccelerationStructureGeometryTrianglesDataKHR& triangles = vulkanGeometry.geometry.triangles;
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VulkanFormat( geometry.m_vertexFormat );
            triangles.vertexData.deviceAddress = pVulkanVertexBuffer->m_deviceAddress + geometry.m_vertexOffset;
            triangles.vertexStride = pVulkanVertexBuffer->m_stride;
            // The highest index a vertex index may take, so one less than the count. D3D12 takes
            // the count itself.
            triangles.maxVertex = geometry.m_numVertices > 0 ? geometry.m_numVertices - 1 : 0;
            triangles.indexType = ( geometry.m_indexType == IndexType::Uint16 ) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = pVulkanIndexBuffer->m_deviceAddress + geometry.m_indexOffset;

            pVulkanAccelerationStructure->m_geometries.push_back( vulkanGeometry );

            // D3D12 counts indices and Vulkan counts triangles. The offsets are folded into the
            // device addresses above, so the range itself starts at zero.
            uint32_t const numPrimitives = geometry.m_numIndices / 3;

            VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
            buildRange.primitiveCount = numPrimitives;
            pVulkanAccelerationStructure->m_buildRanges.push_back( buildRange );

            maxPrimitiveCounts.push_back( numPrimitives );
        }

        VkAccelerationStructureBuildGeometryInfoKHR bottomLevelBuildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bottomLevelBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bottomLevelBuildInfo.flags = pVulkanAccelerationStructure->m_bottomLevel.m_flags;
        bottomLevelBuildInfo.geometryCount = uint32_t( pVulkanAccelerationStructure->m_geometries.size() );
        bottomLevelBuildInfo.pGeometries = pVulkanAccelerationStructure->m_geometries.data();

        VkAccelerationStructureBuildSizesInfoKHR bottomLevelSizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pVulkanContext->m_vkGetAccelerationStructureBuildSizes( pVulkanContext->m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                                &bottomLevelBuildInfo, maxPrimitiveCounts.data(), &bottomLevelSizes );

        pVulkanAccelerationStructure->m_bottomLevel.m_pStructureBuffer =
            CreateAccelerationStructureBuffer( pContext, bottomLevelSizes.accelerationStructureSize, false, "AccelerationStructure BottomLevel" );

        VkAccelerationStructureCreateInfoKHR bottomLevelCreateInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        bottomLevelCreateInfo.buffer = static_cast<VulkanBuffer*>( pVulkanAccelerationStructure->m_bottomLevel.m_pStructureBuffer )->m_buffer;
        bottomLevelCreateInfo.size = bottomLevelSizes.accelerationStructureSize;
        bottomLevelCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

        [[maybe_unused]] VkResult result = pVulkanContext->m_vkCreateAccelerationStructure( pVulkanContext->m_device, &bottomLevelCreateInfo, nullptr, &pVulkanAccelerationStructure->m_bottomLevel.m_handle );
        EE_ASSERT( result == VK_SUCCESS );

        VkAccelerationStructureDeviceAddressInfoKHR bottomLevelAddressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        bottomLevelAddressInfo.accelerationStructure = pVulkanAccelerationStructure->m_bottomLevel.m_handle;
        pVulkanAccelerationStructure->m_bottomLevelDeviceAddress = pVulkanContext->m_vkGetAccelerationStructureDeviceAddress( pVulkanContext->m_device, &bottomLevelAddressInfo );

        // Top level
        //-------------------------------------------------------------------------
        VulkanBuffer const* pVulkanInstanceBuffer = static_cast<VulkanBuffer const*>( topLevelParameters.m_pInstanceBuffer );
        EE_ASSERT( pVulkanInstanceBuffer != nullptr );

        pVulkanAccelerationStructure->m_pInstanceBuffer = topLevelParameters.m_pInstanceBuffer;
        pVulkanAccelerationStructure->m_instanceBufferOffset = topLevelParameters.m_instanceBufferOffset;
        pVulkanAccelerationStructure->m_numInstances = topLevelParameters.m_numInstances;

        // AccelerationStructureInstance matches VkAccelerationStructureInstanceKHR field for field,
        // so the engine's instance buffer is readable as it stands.
        VkAccelerationStructureGeometryKHR topLevelGeometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        topLevelGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        topLevelGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        topLevelGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
        topLevelGeometry.geometry.instances.data.deviceAddress = pVulkanInstanceBuffer->m_deviceAddress + topLevelParameters.m_instanceBufferOffset;

        uint32_t const numInstances = uint32_t( topLevelParameters.m_numInstances );

        VkAccelerationStructureBuildGeometryInfoKHR topLevelBuildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        topLevelBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        topLevelBuildInfo.flags = pVulkanAccelerationStructure->m_topLevel.m_flags;
        topLevelBuildInfo.geometryCount = 1;
        topLevelBuildInfo.pGeometries = &topLevelGeometry;

        VkAccelerationStructureBuildSizesInfoKHR topLevelSizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pVulkanContext->m_vkGetAccelerationStructureBuildSizes( pVulkanContext->m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                                &topLevelBuildInfo, &numInstances, &topLevelSizes );

        pVulkanAccelerationStructure->m_topLevel.m_pStructureBuffer =
            CreateAccelerationStructureBuffer( pContext, topLevelSizes.accelerationStructureSize, true, "AccelerationStructure TopLevel" );

        VkAccelerationStructureCreateInfoKHR topLevelCreateInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        topLevelCreateInfo.buffer = static_cast<VulkanBuffer*>( pVulkanAccelerationStructure->m_topLevel.m_pStructureBuffer )->m_buffer;
        topLevelCreateInfo.size = topLevelSizes.accelerationStructureSize;
        topLevelCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        result = pVulkanContext->m_vkCreateAccelerationStructure( pVulkanContext->m_device, &topLevelCreateInfo, nullptr, &pVulkanAccelerationStructure->m_topLevel.m_handle );
        EE_ASSERT( result == VK_SUCCESS );

        // Scratch
        //-------------------------------------------------------------------------
        // Sized for the larger of the two builds, which share one buffer because the RHI has one
        // field for it. The D3D12 backend sizes it from the bottom level alone and overruns
        // whenever the top level needs more.
        uint64_t const scratchSize = Math::Max( bottomLevelSizes.buildScratchSize, topLevelSizes.buildScratchSize );
        pVulkanAccelerationStructure->m_pScratchBuffer = CreateAccelerationStructureBuffer( pContext, scratchSize, false, "AccelerationStructure Scratch" );

        return pVulkanAccelerationStructure;
    }

    AccelerationStructureHandle GetAccelerationStructureHandle( AccelerationStructure const* pAccelerationStructure )
    {
        VulkanAccelerationStructure const* pVulkanAccelerationStructure = static_cast<VulkanAccelerationStructure const*>( pAccelerationStructure );

        // The top level structure buffer read as a buffer, which is what D3D12 returns. See the
        // section note above for why this needs no acceleration structure descriptor in the heap.
        return GetBufferHandle( pVulkanAccelerationStructure->m_topLevel.m_pStructureBuffer, DescriptorTypeFlags::Buffer );
    }

    //-------------------------------------------------------------------------
    // Buffers
    //-------------------------------------------------------------------------


    //-------------------------------------------------------------------------

    // The only DataFormat to VkFormat mapping. There must never be a second one: two mappings that
    // disagree corrupt textures in a way that looks like a bug somewhere else.
    //
    // Two places where the backends do not line up one for one:
    //  - Vulkan names packed formats most significant component first and DXGI names them least
    //    significant first, so the component order reverses on every packed entry below. Getting
    //    one backwards swaps red and blue on that format alone.
    //  - RGB565_UNorm and BGR565_UNorm map to the same VkFormat, because D3D12 maps both to one
    //    DXGI format. Vulkan can tell them apart and D3D12 cannot, so distinguishing them here
    //    would make the two backends draw the same asset differently. Nothing uses either.
    static VkFormat VulkanFormat( DataFormat format )
    {
        switch ( format )
        {
            case DataFormat::Undefined: return VK_FORMAT_UNDEFINED;

                // Uncompressed formats
                //
                // R1_UNorm has no Vulkan equivalent. Returning UNDEFINED without asserting mirrors
                // what D3D12 does with the ASTC formats it cannot express.
            case DataFormat::R1_UNorm: return VK_FORMAT_UNDEFINED;
            case DataFormat::RGB565_UNorm: return VK_FORMAT_R5G6B5_UNORM_PACK16;
            case DataFormat::BGR565_UNorm: return VK_FORMAT_R5G6B5_UNORM_PACK16;
            case DataFormat::BGR555_A1_UNorm: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
            case DataFormat::R8_UNorm: return VK_FORMAT_R8_UNORM;
            case DataFormat::R8_SNorm: return VK_FORMAT_R8_SNORM;
            case DataFormat::R8_UInt: return VK_FORMAT_R8_UINT;
            case DataFormat::R8_SInt: return VK_FORMAT_R8_SINT;
            case DataFormat::RG8_UNorm: return VK_FORMAT_R8G8_UNORM;
            case DataFormat::RG8_SNorm: return VK_FORMAT_R8G8_SNORM;
            case DataFormat::RG8_UInt: return VK_FORMAT_R8G8_UINT;
            case DataFormat::RG8_SInt: return VK_FORMAT_R8G8_SINT;
            case DataFormat::BGRA4_UNorm: return VK_FORMAT_A4R4G4B4_UNORM_PACK16;
            case DataFormat::RGBA8_UNorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case DataFormat::RGBA8_SNorm: return VK_FORMAT_R8G8B8A8_SNORM;
            case DataFormat::RGBA8_UInt: return VK_FORMAT_R8G8B8A8_UINT;
            case DataFormat::RGBA8_SInt: return VK_FORMAT_R8G8B8A8_SINT;
            case DataFormat::RGBA8_sRGB: return VK_FORMAT_R8G8B8A8_SRGB;
            case DataFormat::BGRA8_UNorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case DataFormat::BGRA8_sRGB: return VK_FORMAT_B8G8R8A8_SRGB;
            case DataFormat::RGB10_A2_UNorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case DataFormat::RGB10_A2_UInt: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
            case DataFormat::R16_UNorm: return VK_FORMAT_R16_UNORM;
            case DataFormat::R16_SNorm: return VK_FORMAT_R16_SNORM;
            case DataFormat::R16_UInt: return VK_FORMAT_R16_UINT;
            case DataFormat::R16_SInt: return VK_FORMAT_R16_SINT;
            case DataFormat::R16_SFloat: return VK_FORMAT_R16_SFLOAT;
            case DataFormat::RG16_UNorm: return VK_FORMAT_R16G16_UNORM;
            case DataFormat::RG16_SNorm: return VK_FORMAT_R16G16_SNORM;
            case DataFormat::RG16_UInt: return VK_FORMAT_R16G16_UINT;
            case DataFormat::RG16_SInt: return VK_FORMAT_R16G16_SINT;
            case DataFormat::RG16_SFloat: return VK_FORMAT_R16G16_SFLOAT;
            case DataFormat::RGBA16_UNorm: return VK_FORMAT_R16G16B16A16_UNORM;
            case DataFormat::RGBA16_SNorm: return VK_FORMAT_R16G16B16A16_SNORM;
            case DataFormat::RGBA16_UInt: return VK_FORMAT_R16G16B16A16_UINT;
            case DataFormat::RGBA16_SInt: return VK_FORMAT_R16G16B16A16_SINT;
            case DataFormat::RGBA16_SFloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case DataFormat::R32_UInt: return VK_FORMAT_R32_UINT;
            case DataFormat::R32_SInt: return VK_FORMAT_R32_SINT;
            case DataFormat::R32_SFloat: return VK_FORMAT_R32_SFLOAT;
            case DataFormat::RG32_UInt: return VK_FORMAT_R32G32_UINT;
            case DataFormat::RG32_SInt: return VK_FORMAT_R32G32_SINT;
            case DataFormat::RG32_SFloat: return VK_FORMAT_R32G32_SFLOAT;
            case DataFormat::RGB32_UInt: return VK_FORMAT_R32G32B32_UINT;
            case DataFormat::RGB32_SInt: return VK_FORMAT_R32G32B32_SINT;
            case DataFormat::RGB32_SFloat: return VK_FORMAT_R32G32B32_SFLOAT;
            case DataFormat::RGBA32_UInt: return VK_FORMAT_R32G32B32A32_UINT;
            case DataFormat::RGBA32_SInt: return VK_FORMAT_R32G32B32A32_SINT;
            case DataFormat::RGBA32_SFloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case DataFormat::RG11_B10_UFloat: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case DataFormat::RGB9_E5_UFloat: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            case DataFormat::D32_SFloat: return VK_FORMAT_D32_SFLOAT;
            case DataFormat::D32_SFloat_S8_UInt: return VK_FORMAT_D32_SFLOAT_S8_UINT;
                // D3D12 has no stencil-only format and widens this to depth-stencil. Vulkan has the
                // exact format, and support for it is optional, so a device without it reports the
                // format unusable rather than silently getting a depth buffer it never asked for.
            case DataFormat::S8_Uint: return VK_FORMAT_S8_UINT;

                // Compressed DXBC formats
                //
                // Vulkan separates BC1 with and without alpha and DXGI does not, so this mapping is
                // more exact than the D3D12 one.
            case DataFormat::DXBC1_RGB_UNorm: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
            case DataFormat::DXBC1_RGB_sRGB: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
            case DataFormat::DXBC1_RGBA_UNorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case DataFormat::DXBC1_RGBA_sRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case DataFormat::DXBC2_UNorm: return VK_FORMAT_BC2_UNORM_BLOCK;
            case DataFormat::DXBC2_sRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
            case DataFormat::DXBC3_UNorm: return VK_FORMAT_BC3_UNORM_BLOCK;
            case DataFormat::DXBC3_sRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
            case DataFormat::DXBC4_UNorm: return VK_FORMAT_BC4_UNORM_BLOCK;
            case DataFormat::DXBC4_SNorm: return VK_FORMAT_BC4_SNORM_BLOCK;
            case DataFormat::DXBC5_UNorm: return VK_FORMAT_BC5_UNORM_BLOCK;
            case DataFormat::DXBC5_SNorm: return VK_FORMAT_BC5_SNORM_BLOCK;
            case DataFormat::DXBC6H_UFloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            case DataFormat::DXBC6H_SFloat: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
            case DataFormat::DXBC7_UNorm: return VK_FORMAT_BC7_UNORM_BLOCK;
            case DataFormat::DXBC7_sRGB: return VK_FORMAT_BC7_SRGB_BLOCK;

                // Compressed ASTC formats. D3D12 has none of these and returns
                // DXGI_FORMAT_UNKNOWN; Vulkan has all of them, gated on textureCompressionASTC_LDR,
                // which nothing enables. FillDeviceCapabilities reports each one honestly.
            case DataFormat::ASTC_4x4_UNorm: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
            case DataFormat::ASTC_4x4_sRGB: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
            case DataFormat::ASTC_5x4_UNorm: return VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
            case DataFormat::ASTC_5x4_sRGB: return VK_FORMAT_ASTC_5x4_SRGB_BLOCK;
            case DataFormat::ASTC_5x5_UNorm: return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
            case DataFormat::ASTC_5x5_sRGB: return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;
            case DataFormat::ASTC_6x5_UNorm: return VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
            case DataFormat::ASTC_6x5_sRGB: return VK_FORMAT_ASTC_6x5_SRGB_BLOCK;
            case DataFormat::ASTC_6x6_UNorm: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
            case DataFormat::ASTC_6x6_sRGB: return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
            case DataFormat::ASTC_8x5_UNorm: return VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
            case DataFormat::ASTC_8x5_sRGB: return VK_FORMAT_ASTC_8x5_SRGB_BLOCK;
            case DataFormat::ASTC_8x6_UNorm: return VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
            case DataFormat::ASTC_8x6_sRGB: return VK_FORMAT_ASTC_8x6_SRGB_BLOCK;
            case DataFormat::ASTC_8x8_UNorm: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
            case DataFormat::ASTC_8x8_sRGB: return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
            case DataFormat::ASTC_10x5_UNorm: return VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
            case DataFormat::ASTC_10x5_sRGB: return VK_FORMAT_ASTC_10x5_SRGB_BLOCK;
            case DataFormat::ASTC_10x6_UNorm: return VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
            case DataFormat::ASTC_10x6_sRGB: return VK_FORMAT_ASTC_10x6_SRGB_BLOCK;
            case DataFormat::ASTC_10x8_UNorm: return VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
            case DataFormat::ASTC_10x8_sRGB: return VK_FORMAT_ASTC_10x8_SRGB_BLOCK;
            case DataFormat::ASTC_10x10_UNorm: return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
            case DataFormat::ASTC_10x10_sRGB: return VK_FORMAT_ASTC_10x10_SRGB_BLOCK;
            case DataFormat::ASTC_12x10_UNorm: return VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
            case DataFormat::ASTC_12x10_sRGB: return VK_FORMAT_ASTC_12x10_SRGB_BLOCK;
            case DataFormat::ASTC_12x12_UNorm: return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
            case DataFormat::ASTC_12x12_sRGB: return VK_FORMAT_ASTC_12x12_SRGB_BLOCK;

            // Special case - completely invalid format on all platforms
            default:
            {
                EE_ASSERT( false );
                return VK_FORMAT_UNDEFINED;
            }
        };
    }

    // Every aspect the image has. A view picks a subset of it: an attachment view takes all of
    // them, and a sampled view of a depth-stencil image has to take exactly one.
    static VkImageAspectFlags VulkanImageAspect( VkFormat format )
    {
        switch ( format )
        {
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
                return VK_IMAGE_ASPECT_DEPTH_BIT;

            case VK_FORMAT_S8_UINT:
                return VK_IMAGE_ASPECT_STENCIL_BIT;

            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    // The view type a shader reads the whole texture through. Uses the same ViewDimension decision
    // as the D3D12 backend, so both classify a texture identically.
    static VkImageViewType VulkanImageViewType( ViewDimension viewDimension )
    {
        switch ( viewDimension )
        {
            case ViewDimension::Texture1D: return VK_IMAGE_VIEW_TYPE_1D;
            case ViewDimension::Texture1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            case ViewDimension::Texture2D: return VK_IMAGE_VIEW_TYPE_2D;
            case ViewDimension::Texture2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            // Vulkan has no separate multisample view type; the image's sample count carries it.
            case ViewDimension::Texture2DMultisample: return VK_IMAGE_VIEW_TYPE_2D;
            case ViewDimension::Texture2DMultisampleArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case ViewDimension::Texture3D: return VK_IMAGE_VIEW_TYPE_3D;
            case ViewDimension::TextureCube: return VK_IMAGE_VIEW_TYPE_CUBE;
            // VK_IMAGE_VIEW_TYPE_CUBE_ARRAY needs the imageCubeArray feature, which is not
            // enabled because no texture in the engine has more than six cube faces.
            case ViewDimension::TextureCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;

            default:
            {
                EE_ASSERT( false );
                return VK_IMAGE_VIEW_TYPE_2D;
            }
        }
    }

    // Which ViewDimension a texture is, by the same rules the D3D12 backend uses. One function for
    // the same reason the format mapping is: two backends that classify a texture differently
    // disagree about what its views mean.
    // width is unnamed because the classification only ever needs height, depth and the layer count.
    // It stays in the signature so that the argument list matches the texture description's field
    // order, which is how every call site reads.
    static ViewDimension VulkanTextureViewDimension( uint32_t /* width */, uint32_t height, uint32_t depth, uint32_t arrayLayers, uint32_t numSamples, TBitFlags<DescriptorTypeFlags> const& descriptorTypes )
    {
        bool const isCubemap = descriptorTypes.AreAnyFlagsSet( DescriptorTypeFlags::TextureCube );

        if ( numSamples > 1 )
        {
            EE_ASSERT( height > 1 && depth == 1 && !isCubemap );
            return arrayLayers > 1 ? ViewDimension::Texture2DMultisampleArray : ViewDimension::Texture2DMultisample;
        }

        if ( isCubemap )
        {
            EE_ASSERT( ( arrayLayers % 6 ) == 0 );
            return arrayLayers > 6 ? ViewDimension::TextureCubeArray : ViewDimension::TextureCube;
        }

        if ( arrayLayers > 1 )
        {
            EE_ASSERT( depth == 1 ); // Neither API has a 3D texture array
            return height > 1 ? ViewDimension::Texture2DArray : ViewDimension::Texture1DArray;
        }

        if ( depth > 1 )
        {
            return ViewDimension::Texture3D;
        }

        return height > 1 ? ViewDimension::Texture2D : ViewDimension::Texture1D;
    }

    // D3D12 resources have no queue ownership at all, and the engine's async compute path relies
    // on that. Vulkan's EXCLUSIVE sharing would need an ownership transfer, and nothing in RHI.h
    // says which queue last touched a resource, so there is nothing to build one out of.
    //
    // CONCURRENT reproduces the D3D12 semantics. It costs some compression on some hardware, and
    // costs nothing on a device with one queue family, which stays EXCLUSIVE below.
    static void SetSharingMode( VulkanContext const* pVulkanContext, VkSharingMode& sharingMode, uint32_t& queueFamilyIndexCount, uint32_t const*& pQueueFamilyIndices )
    {
        if ( pVulkanContext->m_sharingQueueFamilies.size() < 2 )
        {
            sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            return;
        }

        sharingMode = VK_SHARING_MODE_CONCURRENT;
        queueFamilyIndexCount = uint32_t( pVulkanContext->m_sharingQueueFamilies.size() );
        pQueueFamilyIndices = pVulkanContext->m_sharingQueueFamilies.data();
    }

    static void TrackResourceAllocation( VulkanContext* pVulkanContext, TBitFlags<DescriptorTypeFlags> descriptorTypes, bool isTexture, bool isAllocation, uint64_t numBytes )
    {
        if ( numBytes == 0 )
        {
            return;
        }

        auto& stats = isTexture ? pVulkanContext->m_textureStats : pVulkanContext->m_bufferStats;
        auto it = stats.find( descriptorTypes );
        if ( it == stats.end() )
        {
            ResourceAllocStats entry;
            entry.m_numAllocations = isAllocation ? 1 : 0;
            entry.m_numBytes = isAllocation ? numBytes : 0;
            stats.insert( { descriptorTypes, entry } );
        }
        else
        {
            if ( isAllocation )
            {
                it->second.m_numAllocations++;
                it->second.m_numBytes += numBytes;
            }
            else
            {
                it->second.m_numAllocations--;
                it->second.m_numBytes -= numBytes;
            }
        }
    }

    // Writing a mutable descriptor names the actual type, never VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
    // which only ever appears in the layout.
    static void WriteResourceHeapSlot( VulkanContext* pVulkanContext, uint32_t heapIndex, VkDescriptorType descriptorType, VkDescriptorBufferInfo const* pBufferInfo, VkBufferView const* pTexelBufferView )
    {
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pVulkanContext->m_heapDescriptorSet;
        write.dstBinding = g_resourceHeapBinding;
        write.dstArrayElement = heapIndex;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType;
        write.pBufferInfo = pBufferInfo;
        write.pTexelBufferView = pTexelBufferView;

        vkUpdateDescriptorSets( pVulkanContext->m_device, 1, &write, 0, nullptr );
    }

    // The image form of the same write.
    static void WriteResourceHeapSlot( VulkanContext* pVulkanContext, uint32_t heapIndex, VkDescriptorType descriptorType, VkDescriptorImageInfo const* pImageInfo )
    {
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pVulkanContext->m_heapDescriptorSet;
        write.dstBinding = g_resourceHeapBinding;
        write.dstArrayElement = heapIndex;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType;
        write.pImageInfo = pImageInfo;

        vkUpdateDescriptorSets( pVulkanContext->m_device, 1, &write, 0, nullptr );
    }

    // The sampler heap is the second binding of the same set, and a plain sampler array rather
    // than a mutable one.
    static void WriteSamplerHeapSlot( VulkanContext* pVulkanContext, uint32_t heapIndex, VkSampler sampler )
    {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = sampler;

        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pVulkanContext->m_heapDescriptorSet;
        write.dstBinding = g_samplerHeapBinding;
        write.dstArrayElement = heapIndex;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets( pVulkanContext->m_device, 1, &write, 0, nullptr );
    }

    //-------------------------------------------------------------------------

    Buffer* CreateBuffer( Context* pContext, BufferParameters const& parameters )
    {
        EE_ASSERT( pContext != nullptr );

        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanBuffer* pVulkanBuffer = pVulkanContext->CreateObject<VulkanBuffer>();

        uint64_t allocationSize = parameters.m_bufferSize;
        if ( parameters.m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) )
        {
            allocationSize = Math::RoundUpToNearestMultiple64( allocationSize, pVulkanContext->m_deviceCapabilities.m_constantBufferAlignment );
        }

        EE_ASSERT( allocationSize > 0 );

        uint64_t bufferStride = parameters.m_bufferStride;
        if ( parameters.m_format != DataFormat::Undefined )
        {
            bufferStride = FormatBlockBitSize( parameters.m_format ) / 8;
        }

        if ( parameters.m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::Raw ) )
        {
            EE_ASSERT( parameters.m_format == DataFormat::Undefined ||
                       parameters.m_format == DataFormat::R32_UInt ||
                       parameters.m_format == DataFormat::R32_SInt ||
                       parameters.m_format == DataFormat::R32_SFloat );
            bufferStride = sizeof( uint32_t );
        }

        TBitFlags<DescriptorTypeFlags> descriptorTypes = parameters.m_descriptorTypes;
        if ( parameters.m_flags.IsFlagSet( BufferFlags::NoDescriptors ) )
        {
            descriptorTypes = {};
        }

        // Usage flags
        //-------------------------------------------------------------------------
        // D3D12 needs almost none of this: a buffer is a buffer and the view decides what it is.
        // Vulkan wants the usage up front, so it comes from the descriptor types the caller asked
        // for, plus the transfer bits, which any buffer here can need.
        //
        // Read from parameters, not from descriptorTypes. NoDescriptors means "give this buffer no
        // heap slot", which is all it can mean on D3D12, where usage is not a property of a buffer.
        // On Vulkan it is, and a push descriptor write still needs the matching usage bit. The root
        // constant ring is the case that proves it: NoDescriptors and ConstantBuffer together,
        // correctly, and reading the cleared copy here left it without the uniform buffer bit.
        TBitFlags<DescriptorTypeFlags> const usageTypes = parameters.m_descriptorTypes;

        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if ( usageTypes.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) )         { usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; }
        if ( usageTypes.IsFlagSet( DescriptorTypeFlags::IndexBuffer ) )            { usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; }
        if ( usageTypes.IsFlagSet( DescriptorTypeFlags::IndirectArgumentBuffer ) ) { usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; }

        bool const isTypedBuffer = parameters.m_format != DataFormat::Undefined && !usageTypes.IsFlagSet( DescriptorTypeFlags::Raw );

        if ( usageTypes.IsFlagSet( DescriptorTypeFlags::Buffer ) )
        {
            usage |= isTypedBuffer ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }

        if ( usageTypes.IsFlagSet( DescriptorTypeFlags::RWBuffer ) )
        {
            usage |= isTypedBuffer ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }

        // bufferDeviceAddress is a device feature CreateContext requires, and Buffer holds an
        // m_deviceAddress the engine reads, so every buffer carries the bit.
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        // A raytracing build reads its geometry and instances out of ordinary buffers the caller
        // already made, so every buffer carries these rather than the RHI growing a flag for them.
        // Only when the extension is enabled, because naming a usage bit from a disabled extension
        // is a validation error.
        if ( g_raytracingEnabled )
        {
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
        }

        VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferCreateInfo.size = allocationSize;
        bufferCreateInfo.usage = usage;
        SetSharingMode( pVulkanContext, bufferCreateInfo.sharingMode, bufferCreateInfo.queueFamilyIndexCount, bufferCreateInfo.pQueueFamilyIndices );

        VmaAllocationCreateInfo allocationCreateInfo = {};
        switch ( parameters.m_memoryType )
        {
            case ResourceMemoryType::HostToDevice:
            {
                // D3D12_HEAP_TYPE_UPLOAD. Write-combined host memory the GPU reads.
                allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
                allocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            }
            break;

            case ResourceMemoryType::DeviceToHost:
            {
                // D3D12_HEAP_TYPE_READBACK. Cached host memory the CPU reads back.
                allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
                allocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            }
            break;

            case ResourceMemoryType::DeviceLocal:
            {
                allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
            }
            break;
        }

        if ( parameters.m_flags.IsFlagSet( BufferFlags::OwnMemory ) )
        {
            // D3D12MA::ALLOCATION_FLAG_COMMITTED.
            allocationCreateInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        }

        if ( parameters.m_flags.IsFlagSet( BufferFlags::PersistentMap ) && parameters.m_memoryType != ResourceMemoryType::DeviceLocal )
        {
            allocationCreateInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo allocationInfo = {};
        [[maybe_unused]] VkResult result = vmaCreateBuffer( pVulkanContext->m_resourceAllocator, &bufferCreateInfo, &allocationCreateInfo, &pVulkanBuffer->m_buffer, &pVulkanBuffer->m_allocation, &allocationInfo );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanBuffer->m_allocationSize = allocationInfo.size;
        TrackResourceAllocation( pVulkanContext, descriptorTypes, false, true, allocationInfo.size );

        VkBufferDeviceAddressInfo deviceAddressInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        deviceAddressInfo.buffer = pVulkanBuffer->m_buffer;
        pVulkanBuffer->m_deviceAddress = vkGetBufferDeviceAddress( pVulkanContext->m_device, &deviceAddressInfo );
        EE_ASSERT( pVulkanBuffer->m_deviceAddress != 0 );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_BUFFER, uint64_t( pVulkanBuffer->m_buffer ), parameters.m_debugName );

        // VMA_ALLOCATION_CREATE_MAPPED_BIT already mapped it, so there is no second Map call
        // the way D3D12 needs one.
        if ( parameters.m_flags.IsFlagSet( BufferFlags::PersistentMap ) && parameters.m_memoryType != ResourceMemoryType::DeviceLocal )
        {
            pVulkanBuffer->m_pMappedAddress_WriteCombined = allocationInfo.pMappedData;
            EE_ASSERT( pVulkanBuffer->m_pMappedAddress_WriteCombined != nullptr );
        }

        // Descriptors
        //-------------------------------------------------------------------------
        // One contiguous run in the resource heap, in the same order D3D12 uses, because
        // the handle arithmetic in GetBufferHandle has to match on both backends.

        if ( !parameters.m_flags.IsFlagSet( BufferFlags::NoDescriptors ) && descriptorTypes.AreAnyFlagsSet( DescriptorTypeFlags::ConstantBuffer, DescriptorTypeFlags::Buffer, DescriptorTypeFlags::RWBuffer ) )
        {
            uint16_t numDescriptors = 0;
            if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) ) { numDescriptors++; }
            if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::Buffer ) ) { numDescriptors++; }
            if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWBuffer ) ) { numDescriptors++; }

            pVulkanBuffer->m_descriptorHandles = pVulkanContext->m_resourceHeapAllocator.Allocate( numDescriptors );
            EE_ASSERT( pVulkanBuffer->m_descriptorHandles.IsValid() );

            VkDescriptorBufferInfo descriptorBufferInfo = {};
            descriptorBufferInfo.buffer = pVulkanBuffer->m_buffer;
            descriptorBufferInfo.offset = 0;
            descriptorBufferInfo.range = VK_WHOLE_SIZE;

            if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) )
            {
                pVulkanBuffer->m_srvDescriptorOffset = 1;

                VkDescriptorBufferInfo constantBufferInfo = descriptorBufferInfo;
                constantBufferInfo.range = allocationSize;

                WriteResourceHeapSlot( pVulkanContext, pVulkanBuffer->m_descriptorHandles.m_offset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &constantBufferInfo, nullptr );
            }
            else
            {
                pVulkanBuffer->m_srvDescriptorOffset = 0;
            }

            if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::Buffer ) )
            {
                EE_ASSERT( bufferStride > 0 && ( allocationSize % bufferStride ) == 0 );
                pVulkanBuffer->m_uavDescriptorOffset = int8_t( pVulkanBuffer->m_srvDescriptorOffset + int8_t( 1 ) );

                uint32_t const heapIndex = uint32_t( pVulkanBuffer->m_descriptorHandles.m_offset ) + uint32_t( pVulkanBuffer->m_srvDescriptorOffset );

                if ( isTypedBuffer )
                {
                    VkBufferViewCreateInfo viewCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
                    viewCreateInfo.buffer = pVulkanBuffer->m_buffer;
                    viewCreateInfo.format = VulkanFormat( parameters.m_format );
                    viewCreateInfo.offset = parameters.m_firstElement * bufferStride;
                    viewCreateInfo.range = VK_WHOLE_SIZE;

                    result = vkCreateBufferView( pVulkanContext->m_device, &viewCreateInfo, nullptr, &pVulkanBuffer->m_uniformTexelBufferView );
                    EE_ASSERT( result == VK_SUCCESS );

                    WriteResourceHeapSlot( pVulkanContext, heapIndex, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, nullptr, &pVulkanBuffer->m_uniformTexelBufferView );
                }
                else
                {
                    // StructuredBuffer<T> and ByteAddressBuffer are both storage buffers. The
                    // element offset that D3D12 puts in the SRV becomes the descriptor's
                    // own offset here.
                    VkDescriptorBufferInfo readInfo = descriptorBufferInfo;
                    readInfo.offset = parameters.m_firstElement * bufferStride;

                    WriteResourceHeapSlot( pVulkanContext, heapIndex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &readInfo, nullptr );
                }
            }
            else
            {
                pVulkanBuffer->m_uavDescriptorOffset = pVulkanBuffer->m_srvDescriptorOffset;
            }

            if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWBuffer ) )
            {
                EE_ASSERT( pVulkanBuffer->m_uavDescriptorOffset != -1 );
                EE_ASSERT( bufferStride > 0 && ( allocationSize % bufferStride ) == 0 );

                uint32_t const heapIndex = uint32_t( pVulkanBuffer->m_descriptorHandles.m_offset ) + uint32_t( pVulkanBuffer->m_uavDescriptorOffset );

                if ( isTypedBuffer )
                {
                    VkBufferViewCreateInfo viewCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
                    viewCreateInfo.buffer = pVulkanBuffer->m_buffer;
                    viewCreateInfo.format = VulkanFormat( parameters.m_format );
                    viewCreateInfo.offset = parameters.m_firstElement * bufferStride;
                    viewCreateInfo.range = VK_WHOLE_SIZE;

                    result = vkCreateBufferView( pVulkanContext->m_device, &viewCreateInfo, nullptr, &pVulkanBuffer->m_storageTexelBufferView );
                    EE_ASSERT( result == VK_SUCCESS );

                    WriteResourceHeapSlot( pVulkanContext, heapIndex, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, nullptr, &pVulkanBuffer->m_storageTexelBufferView );
                }
                else
                {
                    VkDescriptorBufferInfo writeInfo = descriptorBufferInfo;
                    writeInfo.offset = parameters.m_firstElement * bufferStride;

                    WriteResourceHeapSlot( pVulkanContext, heapIndex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &writeInfo, nullptr );
                }

                // m_pCounterBuffer has nowhere to go: D3D12 hands the counter resource to
                // CreateUnorderedAccessView and Vulkan has no such thing. The engine does not need
                // it, because AppendBuffer.esh carries its own counter and does its own
                // InterlockedAdd, and no shader uses an append or consume buffer.
                EE_ASSERT( parameters.m_pCounterBuffer == nullptr );
            }
        }

        if ( parameters.m_flags.IsFlagSet( BufferFlags::SubAllocations ) )
        {
            VmaVirtualBlockCreateInfo virtualBlockCreateInfo = {};
            virtualBlockCreateInfo.size = allocationSize;
            virtualBlockCreateInfo.pAllocationCallbacks = &pVulkanContext->m_hostAllocationCallbacks;

            result = vmaCreateVirtualBlock( &virtualBlockCreateInfo, &pVulkanBuffer->m_virtualBlock );
            EE_ASSERT( result == VK_SUCCESS );
        }

        pVulkanBuffer->m_size = allocationSize;
        pVulkanBuffer->m_stride = parameters.m_bufferStride;
        pVulkanBuffer->m_memoryType = parameters.m_memoryType;
        pVulkanBuffer->m_nodeIndex = parameters.m_nodeIndex;
        pVulkanBuffer->m_descriptorTypes = descriptorTypes;

        return pVulkanBuffer;
    }

    void DestroyBuffer( Context* pContext, Buffer*&& pBuffer )
    {
        if ( pBuffer != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );

            if ( pVulkanBuffer->m_virtualBlock != VK_NULL_HANDLE )
            {
                EE_ASSERT( vmaIsVirtualBlockEmpty( pVulkanBuffer->m_virtualBlock ) );
                vmaDestroyVirtualBlock( pVulkanBuffer->m_virtualBlock );
            }

            if ( pVulkanBuffer->m_uniformTexelBufferView != VK_NULL_HANDLE )
            {
                vkDestroyBufferView( pVulkanContext->m_device, pVulkanBuffer->m_uniformTexelBufferView, nullptr );
            }

            if ( pVulkanBuffer->m_storageTexelBufferView != VK_NULL_HANDLE )
            {
                vkDestroyBufferView( pVulkanContext->m_device, pVulkanBuffer->m_storageTexelBufferView, nullptr );
            }

            if ( pVulkanBuffer->m_descriptorHandles.IsValid() )
            {
                // The slots are not cleared. PARTIALLY_BOUND means a stale descriptor is only a
                // problem if a shader reads it, and a shader that reads a freed handle is a bug
                // either way. D3D12 frees its descriptors the same way, without writing
                // over them.
                pVulkanContext->m_resourceHeapAllocator.Deallocate( eastl::move( pVulkanBuffer->m_descriptorHandles ) );
            }

            TrackResourceAllocation( pVulkanContext, pVulkanBuffer->m_descriptorTypes, false, false, pVulkanBuffer->m_allocationSize );

            if ( pVulkanBuffer->m_buffer != VK_NULL_HANDLE )
            {
                vmaDestroyBuffer( pVulkanContext->m_resourceAllocator, pVulkanBuffer->m_buffer, pVulkanBuffer->m_allocation );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanBuffer ) );
            pBuffer = nullptr;
        }
    }

    void MapBuffer( Context* pContext, Buffer* pBuffer, ReadRange range )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );

        pVulkanBuffer->m_mappedRange = { 0, pVulkanBuffer->m_size };
        if ( range.m_offset != 0 || range.m_size != 0 )
        {
            pVulkanBuffer->m_mappedRange.m_offset = range.m_offset;
            pVulkanBuffer->m_mappedRange.m_size = range.m_size;
        }

        EE_ASSERT( pVulkanBuffer->m_mappedRange.m_offset < pVulkanBuffer->m_size );
        EE_ASSERT( pVulkanBuffer->m_mappedRange.m_offset + pVulkanBuffer->m_mappedRange.m_size <= pVulkanBuffer->m_size );

        // vmaMapMemory maps the whole allocation; D3D12's Map takes a read range as a
        // hint about what the CPU will touch. The offset is applied to the pointer so the
        // caller sees the same address either way.
        void* pMappedAddress = nullptr;
        [[maybe_unused]] VkResult const result = vmaMapMemory( pVulkanContext->m_resourceAllocator, pVulkanBuffer->m_allocation, &pMappedAddress );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanBuffer->m_pMappedAddress_WriteCombined = static_cast<uint8_t*>( pMappedAddress ) + pVulkanBuffer->m_mappedRange.m_offset;
    }

    void UnmapBuffer( Context* pContext, Buffer* pBuffer )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );

        vmaUnmapMemory( pVulkanContext->m_resourceAllocator, pVulkanBuffer->m_allocation );
        pVulkanBuffer->m_pMappedAddress_WriteCombined = nullptr;
    }

    BufferHandle GetBufferHandle( Buffer const* pBuffer, DescriptorTypeFlags descriptorType )
    {
        VulkanBuffer const* pVulkanBuffer = static_cast<VulkanBuffer const*>( pBuffer );

        EE_ASSERT( pVulkanBuffer->m_descriptorTypes.IsFlagSet( descriptorType ) );
        EE_ASSERT( pVulkanBuffer->m_descriptorHandles.IsValid() );

        switch ( descriptorType )
        {
            case DescriptorTypeFlags::ConstantBuffer:
            {
                return BufferHandle( pVulkanBuffer->m_descriptorHandles.m_offset );
            }

            case DescriptorTypeFlags::Buffer:
            {
                EE_ASSERT( pVulkanBuffer->m_srvDescriptorOffset != -1 );
                return BufferHandle( pVulkanBuffer->m_descriptorHandles.m_offset + uint8_t( pVulkanBuffer->m_srvDescriptorOffset ) );
            }

            case DescriptorTypeFlags::RWBuffer:
            {
                EE_ASSERT( pVulkanBuffer->m_uavDescriptorOffset != -1 );
                return BufferHandle( pVulkanBuffer->m_descriptorHandles.m_offset + uint8_t( pVulkanBuffer->m_uavDescriptorOffset ) );
            }

            default:
            {
                EE_ASSERT( false );
                return InvalidResourceHandle;
            }
        }
    }

    BufferSubAllocation BufferSubAllocate( Buffer* pBuffer, uint64_t size, uint64_t alignment )
    {
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );
        EE_ASSERT( pVulkanBuffer->m_virtualBlock != VK_NULL_HANDLE ); // Buffer was not created with the SubAllocations flag

        VmaVirtualAllocationCreateInfo allocationCreateInfo = {};
        allocationCreateInfo.size = size;
        allocationCreateInfo.alignment = alignment;

        VmaVirtualAllocation virtualAllocation = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;

        VkResult const result = vmaVirtualAllocate( pVulkanBuffer->m_virtualBlock, &allocationCreateInfo, &virtualAllocation, &offset );
        if ( result == VK_SUCCESS )
        {
            static_assert( sizeof( VmaVirtualAllocation ) <= sizeof( uint64_t ), "BufferSubAllocation::m_internal must be able to hold a VmaVirtualAllocation" );
            return BufferSubAllocation{ offset, uint64_t( virtualAllocation ) };
        }

        // Out of memory is expected and the caller handles it; anything else needs looking at.
        EE_ASSERT( result == VK_ERROR_OUT_OF_DEVICE_MEMORY );
        return {};
    }

    void BufferSubDeallocate( Buffer* pBuffer, BufferSubAllocation&& subAllocation )
    {
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );
        EE_ASSERT( pVulkanBuffer->m_virtualBlock != VK_NULL_HANDLE ); // Buffer was not created with the SubAllocations flag

        EE_ASSERT( subAllocation.IsValid() );

        vmaVirtualFree( pVulkanBuffer->m_virtualBlock, VmaVirtualAllocation( subAllocation.m_internal ) );

        subAllocation = {};
    }


    //-------------------------------------------------------------------------
    // Textures and samplers
    //-------------------------------------------------------------------------

    static VkImageView CreateTextureView( VkDevice device, VkImage image, VkFormat format, VkImageViewType viewType, VkImageAspectFlags aspectMask, uint32_t baseMipLevel, uint32_t numMipLevels, uint32_t baseArrayLayer, uint32_t numArrayLayers )
    {
        VkImageViewCreateInfo viewCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewCreateInfo.image = image;
        viewCreateInfo.viewType = viewType;
        viewCreateInfo.format = format;
        viewCreateInfo.subresourceRange.aspectMask = aspectMask;
        viewCreateInfo.subresourceRange.baseMipLevel = baseMipLevel;
        viewCreateInfo.subresourceRange.levelCount = numMipLevels;
        viewCreateInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
        viewCreateInfo.subresourceRange.layerCount = numArrayLayers;

        VkImageView imageView = VK_NULL_HANDLE;
        [[maybe_unused]] VkResult const result = vkCreateImageView( device, &viewCreateInfo, nullptr, &imageView );
        EE_ASSERT( result == VK_SUCCESS );

        return imageView;
    }

    Texture* CreateTexture( Context* pContext, TextureParameters const& parameters )
    {
        EE_ASSERT( pContext != nullptr );

        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanTexture* pVulkanTexture = pVulkanContext->CreateObject<VulkanTexture>();

        EE_ASSERT( parameters.m_width > 0 && parameters.m_height > 0 && parameters.m_depth > 0 );
        EE_ASSERT( parameters.m_mipLevels > 0 && parameters.m_arrayLayers > 0 );

        // A render target the engine named in the RGBA spelling follows the surface into the
        // BGRA one, so its pipeline and the swapchain agree with it. See
        // SubstituteSwapchainColorFormat; a sampled texture keeps the spelling it was given.
        bool const isRenderTargetFormat = parameters.m_descriptorTypes.IsFlagSet( DescriptorTypeFlags::RenderTarget ) ||
                                          parameters.m_textureFlags.IsFlagSet( TextureFlags::AllowDisplayTarget );
        DataFormat const textureFormat = isRenderTargetFormat ? SubstituteSwapchainColorFormat( parameters.m_format ) : parameters.m_format;

        VkFormat const vulkanFormat = VulkanFormat( textureFormat );
        EE_ASSERT( vulkanFormat != VK_FORMAT_UNDEFINED );

        VkImageAspectFlags const aspectMask = VulkanImageAspect( vulkanFormat );
        bool const isDepthStencil = ( aspectMask & ( VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT ) ) != 0;

        TBitFlags<DescriptorTypeFlags> const descriptorTypes = parameters.m_descriptorTypes;
        bool const isShaderResource = descriptorTypes.AreAnyFlagsSet( DescriptorTypeFlags::Texture, DescriptorTypeFlags::TextureCube );
        bool const isStorage = descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWTexture );
        bool const isRenderTarget = descriptorTypes.IsFlagSet( DescriptorTypeFlags::RenderTarget );

        // The flags below need external memory or a console, and no enabled extension provides any
        // of them. Nothing sets one, so halting names the caller that starts to. DisableCompression
        // and AllowDisplayTarget have no Vulkan control at all and are ignored rather than refused.
        EE_ASSERT( !parameters.m_textureFlags.AreAnyFlagsSet( TextureFlags::ExportHandle, TextureFlags::ExportAdapter, TextureFlags::ImportHandle, TextureFlags::ESRAM, TextureFlags::OnTile ) );

        ViewDimension const viewDimension = VulkanTextureViewDimension( parameters.m_width, parameters.m_height, parameters.m_depth, parameters.m_arrayLayers, parameters.m_numSamples, descriptorTypes );

        // Image
        //-------------------------------------------------------------------------

        VkImageCreateInfo imageCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };

        if ( parameters.m_depth > 1 )
        {
            EE_ASSERT( parameters.m_arrayLayers == 1 ); // No 3D texture array in either API
            imageCreateInfo.imageType = VK_IMAGE_TYPE_3D;
            imageCreateInfo.extent = { parameters.m_width, parameters.m_height, parameters.m_depth };
            imageCreateInfo.arrayLayers = 1;
        }
        else
        {
            imageCreateInfo.imageType = parameters.m_height > 1 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_1D;
            imageCreateInfo.extent = { parameters.m_width, parameters.m_height, 1 };
            imageCreateInfo.arrayLayers = parameters.m_arrayLayers;
        }

        // Vulkan wants the usage up front where D3D12 derives it from the views, as with buffers.
        // Both transfer bits, because a copy or a clear can name any texture.
        imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ( isShaderResource ) { imageCreateInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT; }
        if ( isStorage )        { imageCreateInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT; }
        if ( isRenderTarget )   { imageCreateInfo.usage |= isDepthStencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; }

        if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::TextureCube ) )
        {
            imageCreateInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        imageCreateInfo.format = vulkanFormat;
        imageCreateInfo.mipLevels = parameters.m_mipLevels;
        imageCreateInfo.samples = VkSampleCountFlagBits( parameters.m_numSamples );
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        SetSharingMode( pVulkanContext, imageCreateInfo.sharingMode, imageCreateInfo.queueFamilyIndexCount, imageCreateInfo.pQueueFamilyIndices );
        // vkCreateImage accepts UNDEFINED or PREINITIALIZED and nothing else, and the second is for
        // linear tiling. See VulkanTexture::m_subresourceLayouts for what that costs the barriers.
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if ( parameters.m_pNativeHandle != nullptr )
        {
            // A VkImage somebody else owns and allocated. Swapchain images arrive this way, which
            // is why the views below are still built for it.
            pVulkanTexture->m_image = static_cast<VkImage>( parameters.m_pNativeHandle );
            pVulkanTexture->m_ownsImage = false;
        }
        else if ( parameters.m_pTextureToAlias != nullptr )
        {
            // D3D12 aliases by sharing the resource pointer. Sharing the VkImage is the same thing,
            // and the aliased texture keeps ownership of the memory.
            pVulkanTexture->m_image = static_cast<VulkanTexture*>( parameters.m_pTextureToAlias )->m_image;
            pVulkanTexture->m_ownsImage = false;
        }
        else
        {
            VmaAllocationCreateInfo allocationCreateInfo = {};
            allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

            if ( parameters.m_memoryType != ResourceMemoryType::DeviceLocal )
            {
                // D3D12 puts every texture in device memory and uploads through a staging buffer.
                // The RHI still lets a caller ask for host memory.
                allocationCreateInfo.flags |= parameters.m_memoryType == ResourceMemoryType::HostToDevice
                                            ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            }

            if ( parameters.m_textureFlags.IsFlagSet( TextureFlags::OwnMemory ) )
            {
                // D3D12MA::ALLOCATION_FLAG_COMMITTED.
                allocationCreateInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            }

            VmaAllocationInfo allocationInfo = {};
            [[maybe_unused]] VkResult const result = vmaCreateImage( pVulkanContext->m_resourceAllocator, &imageCreateInfo, &allocationCreateInfo, &pVulkanTexture->m_image, &pVulkanTexture->m_allocation, &allocationInfo );
            EE_ASSERT( result == VK_SUCCESS );

            pVulkanTexture->m_allocationSize = allocationInfo.size;
            TrackResourceAllocation( pVulkanContext, descriptorTypes, true, true, allocationInfo.size );
        }

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_IMAGE, uint64_t( pVulkanTexture->m_image ), parameters.m_debugName );

        pVulkanTexture->m_width = parameters.m_width;
        pVulkanTexture->m_height = parameters.m_height;
        pVulkanTexture->m_depth = parameters.m_depth;
        pVulkanTexture->m_arrayLayers = parameters.m_arrayLayers;
        pVulkanTexture->m_mipLevels = parameters.m_mipLevels;
        // Every subresource starts UNDEFINED, which is the only layout vkCreateImage accepts.
        pVulkanTexture->m_subresourceLayouts.resize( size_t( Math::Max( parameters.m_mipLevels, uint32_t( 1 ) ) ) * size_t( Math::Max( parameters.m_arrayLayers, uint32_t( 1 ) ) ), VK_IMAGE_LAYOUT_UNDEFINED );
        pVulkanTexture->m_format = textureFormat;
        pVulkanTexture->m_numSamples = parameters.m_numSamples;
        pVulkanTexture->m_sampleQuality = parameters.m_sampleQuality;
        pVulkanTexture->m_nodeIndex = parameters.m_nodeIndex;
        pVulkanTexture->m_clearValue = parameters.m_clearValue;
        pVulkanTexture->m_descriptorTypes = descriptorTypes;
        pVulkanTexture->m_initialState = parameters.m_initialState;

        pVulkanTexture->m_vulkanFormat = vulkanFormat;
        pVulkanTexture->m_extent = imageCreateInfo.extent;
        pVulkanTexture->m_aspectMask = aspectMask;
        pVulkanTexture->m_copyRowAlignment = pVulkanContext->m_deviceCapabilities.m_uploadBufferTextureRowAlignment;

        // A texture that is also an RWTexture has to sit in GENERAL, because a storage image
        // descriptor may name no other layout and one image cannot be in two at once. Recorded so
        // barriers reach the layout the descriptor was written with, rather than guessing.
        pVulkanTexture->m_shaderReadLayout = isStorage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Views
        //-------------------------------------------------------------------------
        // D3D12 puts the subresource selection in the descriptor and Vulkan puts it in the view, so
        // every subresource the engine can name needs one built up front.

        if ( isShaderResource )
        {
            // A sampled view of a depth-stencil image must name exactly one aspect, and depth is
            // the one the engine reads. The D3D12 backend makes the same choice.
            VkImageAspectFlags const sampledAspect = ( aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT ) != 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : aspectMask;

            pVulkanTexture->m_shaderResourceView = CreateTextureView( pVulkanContext->m_device, pVulkanTexture->m_image, vulkanFormat,
                                                                      VulkanImageViewType( viewDimension ), sampledAspect,
                                                                      0, parameters.m_mipLevels, 0, parameters.m_arrayLayers );
        }

        if ( isStorage )
        {
            // One view per mip level, because an RWTexture handle names a mip. A storage image
            // has no cube view type, so a cube-compatible image is read as a 2D array.
            VkImageViewType storageViewType = VulkanImageViewType( viewDimension );
            if ( storageViewType == VK_IMAGE_VIEW_TYPE_CUBE || storageViewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY )
            {
                storageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            }

            pVulkanTexture->m_storageViews.reserve( parameters.m_mipLevels );
            for ( uint32_t mipLevel = 0; mipLevel < parameters.m_mipLevels; ++mipLevel )
            {
                pVulkanTexture->m_storageViews.emplace_back( CreateTextureView( pVulkanContext->m_device, pVulkanTexture->m_image, vulkanFormat,
                                                                                storageViewType, aspectMask,
                                                                                mipLevel, 1, 0, imageCreateInfo.arrayLayers ) );
            }
        }

        if ( isRenderTarget )
        {
            // One view per subresource, in the order D3D12 allocates its render target
            // descriptors: array layer outer, mip level inner. VulkanTexture::RenderTargetView
            // indexes them the same way.
            VkImageViewType const attachmentViewType = imageCreateInfo.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                                                     : imageCreateInfo.imageType == VK_IMAGE_TYPE_2D ? VK_IMAGE_VIEW_TYPE_2D
                                                     : VK_IMAGE_VIEW_TYPE_1D;

            pVulkanTexture->m_renderTargetViews.reserve( parameters.m_mipLevels * parameters.m_arrayLayers );
            for ( uint32_t arrayLayer = 0; arrayLayer < parameters.m_arrayLayers; ++arrayLayer )
            {
                for ( uint32_t mipLevel = 0; mipLevel < parameters.m_mipLevels; ++mipLevel )
                {
                    // An attachment view takes every aspect the image has, unlike the sampled
                    // view above: dynamic rendering binds the depth and the stencil from it.
                    pVulkanTexture->m_renderTargetViews.emplace_back( CreateTextureView( pVulkanContext->m_device, pVulkanTexture->m_image, vulkanFormat,
                                                                                          attachmentViewType, aspectMask,
                                                                                          mipLevel, 1, arrayLayer, 1 ) );
                }
            }
        }

        // Descriptors
        //-------------------------------------------------------------------------
        // One contiguous run in the resource heap, in D3D12's order: the read view first if present,
        // then one read-write view per mip level. GetTextureHandle does the same arithmetic on both.

        if ( isShaderResource || isStorage )
        {
            uint16_t numDescriptors = 0;
            if ( isShaderResource ) { numDescriptors++; }
            if ( isStorage ) { numDescriptors += uint16_t( parameters.m_mipLevels ); }

            pVulkanTexture->m_descriptorHandles = pVulkanContext->m_resourceHeapAllocator.Allocate( numDescriptors );
            EE_ASSERT( pVulkanTexture->m_descriptorHandles.IsValid() );

            if ( isShaderResource )
            {
                pVulkanTexture->m_uavDescriptorOffset = 1;

                VkDescriptorImageInfo imageInfo = {};
                imageInfo.imageView = pVulkanTexture->m_shaderResourceView;
                imageInfo.imageLayout = pVulkanTexture->m_shaderReadLayout;

                WriteResourceHeapSlot( pVulkanContext, pVulkanTexture->m_descriptorHandles.m_offset, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &imageInfo );
            }
            else
            {
                pVulkanTexture->m_uavDescriptorOffset = 0;
            }

            if ( isStorage )
            {
                for ( uint32_t mipLevel = 0; mipLevel < parameters.m_mipLevels; ++mipLevel )
                {
                    VkDescriptorImageInfo imageInfo = {};
                    imageInfo.imageView = pVulkanTexture->m_storageViews[mipLevel];
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                    uint32_t const heapIndex = uint32_t( pVulkanTexture->m_descriptorHandles.m_offset ) + uint32_t( pVulkanTexture->m_uavDescriptorOffset ) + mipLevel;
                    WriteResourceHeapSlot( pVulkanContext, heapIndex, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imageInfo );
                }
            }
        }

        return pVulkanTexture;
    }

    void DestroyTexture( Context* pContext, Texture*&& pTexture )
    {
        if ( pTexture != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( pTexture );

            if ( pVulkanTexture->m_shaderResourceView != VK_NULL_HANDLE )
            {
                vkDestroyImageView( pVulkanContext->m_device, pVulkanTexture->m_shaderResourceView, nullptr );
            }

            for ( VkImageView imageView : pVulkanTexture->m_storageViews )
            {
                vkDestroyImageView( pVulkanContext->m_device, imageView, nullptr );
            }

            for ( VkImageView imageView : pVulkanTexture->m_renderTargetViews )
            {
                vkDestroyImageView( pVulkanContext->m_device, imageView, nullptr );
            }

            if ( pVulkanTexture->m_descriptorHandles.IsValid() )
            {
                // Not cleared, for the reason DestroyBuffer gives: PARTIALLY_BOUND makes a stale
                // slot harmless unless a shader reads a freed handle, which is a bug either way.
                pVulkanContext->m_resourceHeapAllocator.Deallocate( eastl::move( pVulkanTexture->m_descriptorHandles ) );
            }

            TrackResourceAllocation( pVulkanContext, pVulkanTexture->m_descriptorTypes, true, false, pVulkanTexture->m_allocationSize );

            if ( pVulkanTexture->m_ownsImage && pVulkanTexture->m_image != VK_NULL_HANDLE )
            {
                vmaDestroyImage( pVulkanContext->m_resourceAllocator, pVulkanTexture->m_image, pVulkanTexture->m_allocation );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanTexture ) );
            pTexture = nullptr;
        }
    }

    uint32_t GetTextureCopyRowStride( Texture const* pTexture, uint32_t mipLevel, uint32_t arrayLayer )
    {
        VulkanTexture const* pVulkanTexture = static_cast<VulkanTexture const*>( pTexture );

        EE_ASSERT( mipLevel < pVulkanTexture->m_mipLevels );
        EE_ASSERT( arrayLayer < pVulkanTexture->m_arrayLayers );

        // Vulkan has no equivalent of GetCopyableFootprints, because the staging buffer's layout is
        // the caller's to choose. The device's optimalBufferCopyRowPitchAlignment is the equivalent
        // alignment. CopyRowLengthInTexels has to derive its row length from this same number.
        uint32_t const mipWidth = Math::Max( pVulkanTexture->m_width >> mipLevel, 1U );
        uint32_t const rowStride = ComputeFormatRowStride( pVulkanTexture->m_format, mipWidth );

        return Math::RoundUpToNearestMultiple32( rowStride, pVulkanTexture->m_copyRowAlignment );
    }

    TextureHandle GetTextureHandle( Texture const* pTexture, DescriptorTypeFlags descriptorType, uint32_t rwTextureMipLevel )
    {
        VulkanTexture const* pVulkanTexture = static_cast<VulkanTexture const*>( pTexture );

        EE_ASSERT( pVulkanTexture->m_descriptorTypes.IsFlagSet( descriptorType ) );
        EE_ASSERT( pVulkanTexture->m_descriptorHandles.IsValid() );

        switch ( descriptorType )
        {
            case DescriptorTypeFlags::Texture:
            case DescriptorTypeFlags::TextureCube:
            {
                return TextureHandle( pVulkanTexture->m_descriptorHandles.m_offset );
            }

            case DescriptorTypeFlags::RWTexture:
            {
                EE_ASSERT( rwTextureMipLevel < pVulkanTexture->m_mipLevels );
                EE_ASSERT( pVulkanTexture->m_uavDescriptorOffset != -1 );

                return TextureHandle( pVulkanTexture->m_descriptorHandles.m_offset + uint8_t( pVulkanTexture->m_uavDescriptorOffset ) + rwTextureMipLevel );
            }

            default:
            {
                EE_ASSERT( false );
                return InvalidResourceHandle;
            }
        }
    }

    //-------------------------------------------------------------------------

    static VkFilter VulkanFilter( FilterType filterType )
    {
        return filterType == FilterType::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    }

    static VkSamplerAddressMode VulkanAddressMode( AddressMode addressMode )
    {
        switch ( addressMode )
        {
            case AddressMode::Wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            case AddressMode::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }

        EE_UNREACHABLE_CODE();
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    // D3D12 takes any border colour; Vulkan takes one of six fixed ones unless
    // VK_EXT_custom_border_color is enabled, which it is not. Every sampler the engine creates
    // leaves the default of transparent black, and only a ClampToBorder address mode reads it.
    static VkBorderColor VulkanBorderColor( float const borderColor[4] )
    {
        bool const isBlack = borderColor[0] == 0.0F && borderColor[1] == 0.0F && borderColor[2] == 0.0F;
        bool const isWhite = borderColor[0] == 1.0F && borderColor[1] == 1.0F && borderColor[2] == 1.0F;

        if ( isBlack && borderColor[3] == 0.0F ) { return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; }
        if ( isBlack && borderColor[3] == 1.0F ) { return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; }
        if ( isWhite && borderColor[3] == 1.0F ) { return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; }

        // A colour Vulkan cannot express without VK_EXT_custom_border_color. Halting here names
        // the sampler that started needing the extension.
        EE_UNIMPLEMENTED_FUNCTION();
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }

    Sampler* CreateSampler( Context* pContext, SamplerParameters const& parameters )
    {
        EE_ASSERT( pContext != nullptr );

        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanSampler* pVulkanSampler = pVulkanContext->CreateObject<VulkanSampler>();

        // The D3D12 backend asserts on the same pairing, where a comparison filter and a comparison
        // function are two halves of one filter value.
        if ( parameters.m_compareMode != CompareMode::Never )
        {
            EE_ASSERT( parameters.m_filterMode == FilterMode::Compare );
        }

        VkSamplerCreateInfo samplerCreateInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerCreateInfo.minFilter = VulkanFilter( parameters.m_minFilter );
        samplerCreateInfo.magFilter = VulkanFilter( parameters.m_magFilter );
        samplerCreateInfo.mipmapMode = parameters.m_mipMapMode == MipMapMode::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerCreateInfo.addressModeU = VulkanAddressMode( parameters.m_addressModeU );
        samplerCreateInfo.addressModeV = VulkanAddressMode( parameters.m_addressModeV );
        samplerCreateInfo.addressModeW = VulkanAddressMode( parameters.m_addressModeW );
        samplerCreateInfo.mipLodBias = parameters.m_mipLODBias;
        samplerCreateInfo.anisotropyEnable = parameters.m_maxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
        samplerCreateInfo.maxAnisotropy = float( parameters.m_maxAnisotropy );
        samplerCreateInfo.compareEnable = parameters.m_compareMode != CompareMode::Never ? VK_TRUE : VK_FALSE;
        samplerCreateInfo.compareOp = VulkanCompareOp( parameters.m_compareMode );
        samplerCreateInfo.minLod = parameters.m_minLOD;
        samplerCreateInfo.maxLod = parameters.m_maxLOD;
        samplerCreateInfo.borderColor = VulkanBorderColor( parameters.m_borderColor );

        // FilterMode::Min and FilterMode::Max are one D3D12_FILTER value there and a separate
        // reduction mode here. samplerFilterMinmax is core in 1.2 and CreateContext requires it,
        // because RenderSystem::Initialize creates COMMON_SAMPLER_LINEAR_CLAMP_MAX.
        VkSamplerReductionModeCreateInfo reductionModeCreateInfo = { VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO };
        if ( parameters.m_filterMode == FilterMode::Min || parameters.m_filterMode == FilterMode::Max )
        {
            reductionModeCreateInfo.reductionMode = parameters.m_filterMode == FilterMode::Min ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
            samplerCreateInfo.pNext = &reductionModeCreateInfo;
        }

        // SamplerParameters::m_setLODRange has no D3D12 use either; CreateSampler there
        // ignores it too.

        [[maybe_unused]] VkResult const result = vkCreateSampler( pVulkanContext->m_device, &samplerCreateInfo, nullptr, &pVulkanSampler->m_sampler );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanSampler->m_nodeIndex = parameters.m_nodeIndex;

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_SAMPLER, uint64_t( pVulkanSampler->m_sampler ), "SamplerState" );

        pVulkanSampler->m_descriptorHandle = pVulkanContext->m_samplerHeapAllocator.Allocate( 1 );
        EE_ASSERT( pVulkanSampler->m_descriptorHandle.IsValid() );

        WriteSamplerHeapSlot( pVulkanContext, pVulkanSampler->m_descriptorHandle.m_offset, pVulkanSampler->m_sampler );

        return pVulkanSampler;
    }

    void DestroySampler( Context* pContext, Sampler*&& pSampler )
    {
        if ( pSampler != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanSampler* pVulkanSampler = static_cast<VulkanSampler*>( pSampler );

            if ( pVulkanSampler->m_descriptorHandle.IsValid() )
            {
                pVulkanContext->m_samplerHeapAllocator.Deallocate( eastl::move( pVulkanSampler->m_descriptorHandle ) );
            }

            if ( pVulkanSampler->m_sampler != VK_NULL_HANDLE )
            {
                vkDestroySampler( pVulkanContext->m_device, pVulkanSampler->m_sampler, nullptr );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanSampler ) );
            pSampler = nullptr;
        }
    }

    SamplerStateHandle GetSamplerStateHandle( Sampler const* pSampler )
    {
        VulkanSampler const* pVulkanSampler = static_cast<VulkanSampler const*>( pSampler );

        EE_ASSERT( pVulkanSampler->m_descriptorHandle.IsValid() );
        return SamplerStateHandle( pVulkanSampler->m_descriptorHandle.m_offset );
    }

    //-------------------------------------------------------------------------
    // Shaders, root signatures and pipelines
    //-------------------------------------------------------------------------





    //-------------------------------------------------------------------------
    // State mapping
    //-------------------------------------------------------------------------

    static VkCompareOp VulkanCompareOp( CompareMode mode )
    {
        switch ( mode )
        {
            case CompareMode::Never:        return VK_COMPARE_OP_NEVER;
            case CompareMode::Less:         return VK_COMPARE_OP_LESS;
            case CompareMode::Equal:        return VK_COMPARE_OP_EQUAL;
            case CompareMode::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case CompareMode::Greater:      return VK_COMPARE_OP_GREATER;
            case CompareMode::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
            case CompareMode::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case CompareMode::Always:       return VK_COMPARE_OP_ALWAYS;
        }

        EE_UNREACHABLE_CODE();
        return VK_COMPARE_OP_ALWAYS;
    }

    static VkStencilOp VulkanStencilOp( StencilOp op )
    {
        switch ( op )
        {
            case StencilOp::Keep:               return VK_STENCIL_OP_KEEP;
            case StencilOp::SetZero:            return VK_STENCIL_OP_ZERO;
            case StencilOp::Replace:            return VK_STENCIL_OP_REPLACE;
            case StencilOp::Invert:             return VK_STENCIL_OP_INVERT;
            case StencilOp::Increment:          return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case StencilOp::Decrement:          return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            case StencilOp::IncrementSaturate:  return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case StencilOp::DecrementSaturate:  return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        }

        EE_UNREACHABLE_CODE();
        return VK_STENCIL_OP_KEEP;
    }

    static VkBlendFactor VulkanBlendFactor( BlendConstant constant )
    {
        switch ( constant )
        {
            case BlendConstant::Zero:                   return VK_BLEND_FACTOR_ZERO;
            case BlendConstant::One:                    return VK_BLEND_FACTOR_ONE;
            case BlendConstant::SrcColor:               return VK_BLEND_FACTOR_SRC_COLOR;
            case BlendConstant::OneMinusSrcColor:       return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case BlendConstant::DstColor:               return VK_BLEND_FACTOR_DST_COLOR;
            case BlendConstant::OneMinusDstColor:       return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case BlendConstant::SrcAlpha:               return VK_BLEND_FACTOR_SRC_ALPHA;
            case BlendConstant::OneMinusSrcAlpha:       return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BlendConstant::DstAlpha:               return VK_BLEND_FACTOR_DST_ALPHA;
            case BlendConstant::OneMinusDstAlpha:       return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case BlendConstant::SrcAlphaSaturate:       return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            case BlendConstant::BlendFactor:            return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case BlendConstant::OneMinusBlendFactor:    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        }

        EE_UNREACHABLE_CODE();
        return VK_BLEND_FACTOR_ONE;
    }

    static VkBlendOp VulkanBlendOp( BlendMode mode )
    {
        switch ( mode )
        {
            case BlendMode::Add:                return VK_BLEND_OP_ADD;
            case BlendMode::Subtract:           return VK_BLEND_OP_SUBTRACT;
            case BlendMode::ReverseSubtract:    return VK_BLEND_OP_REVERSE_SUBTRACT;
            case BlendMode::Min:                return VK_BLEND_OP_MIN;
            case BlendMode::Max:                return VK_BLEND_OP_MAX;
        }

        EE_UNREACHABLE_CODE();
        return VK_BLEND_OP_ADD;
    }

    static VkShaderStageFlagBits VulkanShaderStage( ShaderStage stage )
    {
        switch ( stage )
        {
            case ShaderStage::Vertex:       return VK_SHADER_STAGE_VERTEX_BIT;
            case ShaderStage::Pixel:        return VK_SHADER_STAGE_FRAGMENT_BIT;
            case ShaderStage::Task:         return VK_SHADER_STAGE_TASK_BIT_EXT;
            case ShaderStage::Mesh:         return VK_SHADER_STAGE_MESH_BIT_EXT;
            case ShaderStage::Compute:      return VK_SHADER_STAGE_COMPUTE_BIT;
            case ShaderStage::RayTracing:   return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        }

        EE_UNREACHABLE_CODE();
        return VK_SHADER_STAGE_COMPUTE_BIT;
    }

    static VkShaderStageFlags VulkanShaderStages( TBitFlags<ShaderStage> stages )
    {
        VkShaderStageFlags result = 0;
        if ( stages.IsFlagSet( ShaderStage::Vertex ) )       { result |= VK_SHADER_STAGE_VERTEX_BIT; }
        if ( stages.IsFlagSet( ShaderStage::Pixel ) )        { result |= VK_SHADER_STAGE_FRAGMENT_BIT; }
        if ( stages.IsFlagSet( ShaderStage::Task ) )         { result |= VK_SHADER_STAGE_TASK_BIT_EXT; }
        if ( stages.IsFlagSet( ShaderStage::Mesh ) )         { result |= VK_SHADER_STAGE_MESH_BIT_EXT; }
        if ( stages.IsFlagSet( ShaderStage::Compute ) )      { result |= VK_SHADER_STAGE_COMPUTE_BIT; }
        if ( stages.IsFlagSet( ShaderStage::RayTracing ) )   { result |= VK_SHADER_STAGE_ALL; }
        return result;
    }

    //-------------------------------------------------------------------------
    // Reflection
    //-------------------------------------------------------------------------
    // SPIRV-Reflect replaces ID3D12ShaderReflection. The output has to be the same ShaderReflection
    // the engine already reads, so this mirrors the D3D12 version rather than exposing anything new.

    static bool IsRootConstant( char const* pName )
    {
        // RHI.esh declares the block as a named ConstantBuffer, so the name is the marker. The
        // D3D12 backend matches on the same name.
        if ( pName == nullptr )
        {
            return false;
        }

        char const* pRootConstantName = "rootconstants";
        size_t const length = strlen( pRootConstantName );

        if ( strlen( pName ) != length )
        {
            return false;
        }

        for ( size_t charIndex = 0; charIndex < length; ++charIndex )
        {
            if ( char( tolower( pName[charIndex] ) ) != pRootConstantName[charIndex] )
            {
                return false;
            }
        }

        return true;
    }

    static TBitFlags<DescriptorTypeFlags> DescriptorTypeFromReflection( SpvReflectDescriptorBinding const& binding )
    {
        // resource_type carries the read against read-write distinction that D3D12 gets
        // from D3D_SIT_STRUCTURED versus D3D_SIT_UAV_RWSTRUCTURED, so it is the discriminator
        // rather than the SPIR-V type alone.
        bool const isReadWrite = ( binding.resource_type & SPV_REFLECT_RESOURCE_FLAG_UAV ) != 0;

        switch ( binding.descriptor_type )
        {
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:                   return DescriptorTypeFlags::Sampler;
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:             return DescriptorTypeFlags::Texture;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:             return DescriptorTypeFlags::RWTexture;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:      return DescriptorTypeFlags::Buffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:      return DescriptorTypeFlags::RWBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:            return DescriptorTypeFlags::ConstantBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:            return isReadWrite ? DescriptorTypeFlags::RWBuffer : DescriptorTypeFlags::Buffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:    return DescriptorTypeFlags::Texture;
            case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return DescriptorTypeFlags::AccelerationStructure;

            default:
            {
                EE_UNREACHABLE_CODE();
                return DescriptorTypeFlags::Buffer;
            }
        }
    }

    static ViewDimension ViewDimensionFromReflection( SpvReflectDescriptorBinding const& binding )
    {
        if ( binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
             binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
             binding.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER )
        {
            return ViewDimension::Undefined;
        }

        bool const isArray = binding.image.arrayed != 0;

        switch ( binding.image.dim )
        {
            case SpvDim1D:      return isArray ? ViewDimension::Texture1DArray : ViewDimension::Texture1D;
            case SpvDim2D:      return isArray ? ViewDimension::Texture2DArray : ViewDimension::Texture2D;
            case SpvDim3D:      return ViewDimension::Texture3D;
            case SpvDimCube:    return isArray ? ViewDimension::TextureCubeArray : ViewDimension::TextureCube;
            case SpvDimBuffer:  return ViewDimension::Buffer;
            default:            return ViewDimension::Undefined;
        }
    }

    static ShaderReflection ExtractReflection( TArrayView<uint8_t const> spirv, ShaderStage shaderStage )
    {
        ShaderReflection reflection = {};
        reflection.m_shaderStages.AppendFlags( shaderStage );

        SpvReflectShaderModule module = {};
        [[maybe_unused]] SpvReflectResult const createResult = spvReflectCreateShaderModule( spirv.size(), spirv.data(), &module );
        EE_ASSERT( createResult == SPV_REFLECT_RESULT_SUCCESS );

        if ( shaderStage == ShaderStage::Compute || shaderStage == ShaderStage::Task || shaderStage == ShaderStage::Mesh )
        {
            EE_ASSERT( module.entry_point_count > 0 );
            reflection.m_threadsPerGroup[0] = module.entry_points[0].local_size.x;
            reflection.m_threadsPerGroup[1] = module.entry_points[0].local_size.y;
            reflection.m_threadsPerGroup[2] = module.entry_points[0].local_size.z;
        }

        uint32_t numBindings = 0;
        spvReflectEnumerateDescriptorBindings( &module, &numBindings, nullptr );

        TInlineVector<SpvReflectDescriptorBinding*, 32> bindings( numBindings );
        spvReflectEnumerateDescriptorBindings( &module, &numBindings, bindings.data() );

        reflection.m_shaderResources.reserve( numBindings );

        for ( SpvReflectDescriptorBinding const* pBinding : bindings )
        {
            // Set 1 is the bindless heap, shared by every pipeline, so it is not a root parameter
            // and must not become one.
            if ( pBinding->set == g_heapSet )
            {
                continue;
            }

            ShaderResource shaderResource = {};
            shaderResource.m_setIndex = pBinding->set;
            // The Vulkan binding, not the HLSL register it came from. The b/t/u/s registers are
            // shifted to 0/8/16/24, and un-shifting here only to re-shift in CreateRootSignature
            // would be two chances to get it wrong. Nothing outside this file reads it.
            shaderResource.m_registerIndex = pBinding->binding;
            shaderResource.m_numConstants = pBinding->count;
            shaderResource.m_usedStages = shaderStage;
            shaderResource.m_name = ( pBinding->name != nullptr ) ? pBinding->name : "";
            shaderResource.m_viewDimension = ViewDimensionFromReflection( *pBinding );

            if ( IsRootConstant( pBinding->name ) )
            {
                shaderResource.m_descriptorTypeFlags = DescriptorTypeFlags::RootConstant;
            }
            else
            {
                shaderResource.m_descriptorTypeFlags = DescriptorTypeFromReflection( *pBinding );
            }

            size_t const resourceIndex = reflection.m_shaderResources.size();
            reflection.m_shaderResources.push_back( shaderResource );

            // Constant buffer members, which CreateRootSignature adds up to size the root constants.
            if ( pBinding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER )
            {
                for ( uint32_t memberIndex = 0; memberIndex < pBinding->block.member_count; ++memberIndex )
                {
                    SpvReflectBlockVariable const& member = pBinding->block.members[memberIndex];

                    ShaderVariable variable = {};
                    variable.m_name = ( member.name != nullptr ) ? member.name : "";
                    variable.m_parentResourceIndex = uint32_t( resourceIndex );
                    variable.m_offset = member.offset;
                    variable.m_size = member.size;

                    reflection.m_shaderVariables.push_back( variable );
                }
            }
        }

        spvReflectDestroyShaderModule( &module );

        return reflection;
    }

    //-------------------------------------------------------------------------

    Shader* CreateShader( Context* pContext, TInlineVector<ShaderByteCode, 2> const& shaderParameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanShader* pVulkanShader = pVulkanContext->CreateObject<VulkanShader>();

        size_t const numStages = shaderParameters.size();
        pVulkanShader->m_device = pVulkanContext->m_device;
        pVulkanShader->m_shaderModules.resize( numStages, VK_NULL_HANDLE );
        pVulkanShader->m_stageReflections.resize( numStages );
        pVulkanShader->m_stageEntryNames.resize( numStages );

        for ( size_t shaderIndex = 0; shaderIndex < numStages; ++shaderIndex )
        {
            ShaderStage const stage = shaderParameters[shaderIndex].m_stage;
            pVulkanShader->m_stages.SetFlag( stage );

            // ShaderByteCode carries base85-encoded, compressed SPIR-V, as it carries DXIL on Windows.
            Blob byteCode = Embed::DecompressEmbeddedFile( shaderParameters[shaderIndex].m_pCompressedData, shaderParameters[shaderIndex].m_decodedSize, shaderParameters[shaderIndex].m_decompressedSize );

            EE_ASSERT( !byteCode.empty() );
            EE_ASSERT( ( byteCode.size() % sizeof( uint32_t ) ) == 0 ); // SPIR-V is a stream of 32 bit words

            // A module the device cannot accept is not created at all. The engine creates every
            // shader at startup whatever the device supports, and vkCreateShaderModule rejects a
            // capability the device did not enable. Reflection still runs, because it reads the
            // SPIR-V rather than the module, so the root signature is built either way.
            bool const isMeshStage = ( stage == ShaderStage::Task ) || ( stage == ShaderStage::Mesh );
            if ( !isMeshStage || pVulkanContext->m_meshShader )
            {
                VkShaderModuleCreateInfo moduleCreateInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                moduleCreateInfo.codeSize = byteCode.size();
                moduleCreateInfo.pCode = reinterpret_cast<uint32_t const*>( byteCode.data() );

                [[maybe_unused]] VkResult const result = vkCreateShaderModule( pVulkanContext->m_device, &moduleCreateInfo, nullptr, &pVulkanShader->m_shaderModules[shaderIndex] );
                EE_ASSERT( result == VK_SUCCESS );
            }

            pVulkanShader->m_stageReflections[shaderIndex] = ExtractReflection( TArrayView<uint8_t const>( byteCode.data(), byteCode.size() ), stage );

            // Read back rather than assumed, because a wrong entry point name fails at pipeline
            // creation with an unhelpful message.
            SpvReflectShaderModule module = {};
            if ( spvReflectCreateShaderModule( byteCode.size(), byteCode.data(), &module ) == SPV_REFLECT_RESULT_SUCCESS )
            {
                EE_ASSERT( module.entry_point_count > 0 );
                pVulkanShader->m_stageEntryNames[shaderIndex] = module.entry_point_name;
                spvReflectDestroyShaderModule( &module );
            }

            switch ( stage )
            {
                case ShaderStage::Vertex:   EE_ASSERT( pVulkanShader->m_vertexStageIndex == -1 );  pVulkanShader->m_vertexStageIndex = int32_t( shaderIndex ); break;
                case ShaderStage::Pixel:    EE_ASSERT( pVulkanShader->m_pixelStageIndex == -1 );   pVulkanShader->m_pixelStageIndex = int32_t( shaderIndex ); break;
                case ShaderStage::Task:     EE_ASSERT( pVulkanShader->m_taskStageIndex == -1 );    pVulkanShader->m_taskStageIndex = int32_t( shaderIndex ); break;
                case ShaderStage::Mesh:     EE_ASSERT( pVulkanShader->m_meshStageIndex == -1 );    pVulkanShader->m_meshStageIndex = int32_t( shaderIndex ); break;
                case ShaderStage::Compute:  EE_ASSERT( pVulkanShader->m_computeStageIndex == -1 ); pVulkanShader->m_computeStageIndex = int32_t( shaderIndex ); break;
                case ShaderStage::RayTracing: break;
            }

            SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_SHADER_MODULE, uint64_t( pVulkanShader->m_shaderModules[shaderIndex] ), shaderParameters[shaderIndex].m_ID.c_str() );
        }

        return pVulkanShader;
    }

    void DestroyShader( Context* pContext, Shader*&& pShader )
    {
        if ( pShader != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanShader* pVulkanShader = static_cast<VulkanShader*>( pShader );

            for ( VkShaderModule shaderModule : pVulkanShader->m_shaderModules )
            {
                if ( shaderModule != VK_NULL_HANDLE )
                {
                    vkDestroyShaderModule( pVulkanContext->m_device, shaderModule, nullptr );
                }
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanShader ) );
            pShader = nullptr;
        }
    }

    //-------------------------------------------------------------------------

    RootSignature* CreateRootSignature( Context* pContext, RootSignatureParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanRootSignature* pVulkanRootSignature = pVulkanContext->CreateObject<VulkanRootSignature>();

        pVulkanRootSignature->m_device = pVulkanContext->m_device;
        pVulkanRootSignature->m_heapDescriptorSet = pVulkanContext->m_heapDescriptorSet;

        // Merge the per-stage reflections into one resource list, first seen wins, as the D3D12
        // backend does. The order decides m_parameterIndex, which CmdSetRootParameter and
        // EngineShader.cpp both index by position.
        THashMap<StringView, size_t> descriptorNameToIndex{ Memory::Allocators::g_RHI };
        TVector<uint32_t> constantSizes{ Memory::Allocators::g_RHI };

        for ( ShaderReflection const& shaderReflection : parameters.m_pShader->m_stageReflections )
        {
            for ( ShaderResource const& shaderResource : shaderReflection.m_shaderResources )
            {
                if ( auto foundDescriptorIndex = descriptorNameToIndex.find( shaderResource.m_name ); foundDescriptorIndex == descriptorNameToIndex.end() )
                {
                    size_t const shaderResourceIndex = pVulkanRootSignature->m_shaderResources.size();
                    pVulkanRootSignature->m_shaderResources.push_back( shaderResource );

                    uint32_t constantSize = 0;
                    if ( shaderResource.m_descriptorTypeFlags.AreAnyFlagsSet( DescriptorTypeFlags::ConstantBuffer, DescriptorTypeFlags::RootConstant ) )
                    {
                        for ( ShaderVariable const& variable : shaderReflection.m_shaderVariables )
                        {
                            size_t const parentResourceIndex = size_t( &shaderResource - shaderReflection.m_shaderResources.data() );
                            if ( variable.m_parentResourceIndex == parentResourceIndex )
                            {
                                constantSize += variable.m_size;
                            }
                        }
                    }
                    constantSizes.push_back( constantSize );

                    descriptorNameToIndex.insert( { shaderResource.m_name, shaderResourceIndex } );
                }
                else
                {
                    ShaderResource& targetShaderResource = pVulkanRootSignature->m_shaderResources[foundDescriptorIndex->second];
                    EE_ASSERT( shaderResource.m_descriptorTypeFlags == targetShaderResource.m_descriptorTypeFlags );
                    EE_ASSERT( shaderResource.m_registerIndex == targetShaderResource.m_registerIndex );
                    EE_ASSERT( shaderResource.m_setIndex == targetShaderResource.m_setIndex );

                    targetShaderResource.m_usedStages.AppendFlags( shaderResource.m_usedStages );
                }
            }
        }

        pVulkanRootSignature->m_shaderResources.shrink_to_fit();
        pVulkanRootSignature->m_descriptorReflections.reserve( pVulkanRootSignature->m_shaderResources.size() );

        TInlineVector<VkDescriptorSetLayoutBinding, 32> rootParameterBindings;

        for ( uint32_t shaderResourceIndex = 0; shaderResourceIndex < pVulkanRootSignature->m_shaderResources.size(); ++shaderResourceIndex )
        {
            ShaderResource const& shaderResource = pVulkanRootSignature->m_shaderResources[shaderResourceIndex];

            DescriptorReflection descriptorReflection = {};
            descriptorReflection.m_name = shaderResource.m_name;
            descriptorReflection.m_descriptorTypeFlags = shaderResource.m_descriptorTypeFlags;
            descriptorReflection.m_viewDimension = shaderResource.m_viewDimension;
            descriptorReflection.m_numConstants = shaderResource.m_numConstants;
            descriptorReflection.m_parameterIndex = int32_t( rootParameterBindings.size() );
            descriptorReflection.m_setIndex = int32_t( shaderResource.m_setIndex );

            VkDescriptorSetLayoutBinding binding = {};
            binding.binding = shaderResource.m_registerIndex;
            binding.descriptorCount = 1;
            binding.stageFlags = VulkanShaderStages( shaderResource.m_usedStages );

            if ( descriptorReflection.m_descriptorTypeFlags.IsFlagSet( DescriptorTypeFlags::RootConstant ) )
            {
                // Not Vulkan push constants. RHI.esh declares the block as a ConstantBuffer, so DXC
                // emits a uniform buffer and CmdSetRootConstants pushes a descriptor at a ring.
                // The one push constant block RHI.esh does declare carries the indirect argument
                // address, not this.
                descriptorReflection.m_numConstants = constantSizes[shaderResourceIndex] / sizeof( uint32_t );
                binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            }
            else if ( descriptorReflection.m_descriptorTypeFlags.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) )
            {
                binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            }
            else if ( descriptorReflection.m_descriptorTypeFlags.AreAnyFlagsSet( DescriptorTypeFlags::RWBuffer, DescriptorTypeFlags::RWTexture ) )
            {
                binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            else if ( descriptorReflection.m_descriptorTypeFlags.AreAnyFlagsSet( DescriptorTypeFlags::Buffer, DescriptorTypeFlags::Texture ) )
            {
                binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            else if ( descriptorReflection.m_descriptorTypeFlags.IsFlagSet( DescriptorTypeFlags::Sampler ) )
            {
                binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            }
            else
            {
                EE_UNREACHABLE_CODE();
            }

            rootParameterBindings.push_back( binding );
            pVulkanRootSignature->m_descriptorReflections.push_back( descriptorReflection );
        }

        // Kept for CmdExecuteIndirect, which has to know the descriptor type of a binding the engine
        // never wrote. Recomputing it there would be a second copy of the mapping above.
        pVulkanRootSignature->m_rootParameterBindings = rootParameterBindings;

        // No shader currently declares a static sampler, so there are no immutable samplers here.
        // The warning matches the D3D12 one, so a shader that starts using one is noticed.
        for ( size_t staticSamplerIndex = 0; staticSamplerIndex < parameters.m_staticSamplerNames.size(); ++staticSamplerIndex )
        {
            auto pSamplerShaderResource = eastl::find_if( pVulkanRootSignature->m_shaderResources.begin(), pVulkanRootSignature->m_shaderResources.end(),
                                                          [&] ( ShaderResource const& shaderResource )
            {
                return shaderResource.m_name == parameters.m_staticSamplerNames[staticSamplerIndex];
            } );

            if ( pSamplerShaderResource == pVulkanRootSignature->m_shaderResources.end() )
            {
                EE_LOG_WARNING( LogCategory::Render, "RHI/CreateRootSignature", "Static sampler \"%s\" not found in RootSignature \"%s\"",
                                parameters.m_staticSamplerNames[staticSamplerIndex], parameters.m_debugName.data() );
                continue;
            }

            EE_UNIMPLEMENTED_FUNCTION(); // A shader started using a static sampler.
        }

        // Set 0, the root parameters. PUSH_DESCRIPTOR_BIT, so CmdSetRootParameter is a
        // vkCmdPushDescriptorSetKHR rather than a set allocated per draw.
        VkDescriptorSetLayoutCreateInfo rootParameterLayoutCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        rootParameterLayoutCreateInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        rootParameterLayoutCreateInfo.bindingCount = uint32_t( rootParameterBindings.size() );
        rootParameterLayoutCreateInfo.pBindings = rootParameterBindings.data();

        [[maybe_unused]] VkResult result = vkCreateDescriptorSetLayout( pVulkanContext->m_device, &rootParameterLayoutCreateInfo, nullptr, &pVulkanRootSignature->m_rootParameterSetLayout );
        EE_ASSERT( result == VK_SUCCESS );

        // Both sets, in order. Set 1 is the same layout for every pipeline in the engine.
        VkDescriptorSetLayout const setLayouts[2] = { pVulkanRootSignature->m_rootParameterSetLayout, pVulkanContext->m_heapSetLayout };

        // The one push constant range, which indirect draws read their own command through. Given
        // to every layout rather than only the signatures that need it, because a root signature
        // does not know whether a command signature will later be built from it, and 24 bytes of a
        // guaranteed 128 costs nothing.
        VkPushConstantRange indirectPushConstantRange = {};
        indirectPushConstantRange.stageFlags = VK_SHADER_STAGE_ALL;
        indirectPushConstantRange.offset = 0;
        indirectPushConstantRange.size = sizeof( IndirectRootPushConstants );

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutCreateInfo.setLayoutCount = 2;
        pipelineLayoutCreateInfo.pSetLayouts = setLayouts;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
        pipelineLayoutCreateInfo.pPushConstantRanges = &indirectPushConstantRange;

        result = vkCreatePipelineLayout( pVulkanContext->m_device, &pipelineLayoutCreateInfo, nullptr, &pVulkanRootSignature->m_pipelineLayout );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_PIPELINE_LAYOUT, uint64_t( pVulkanRootSignature->m_pipelineLayout ), parameters.m_debugName );

        return pVulkanRootSignature;
    }

    void DestroyRootSignature( Context* pContext, RootSignature*&& pRootSignature )
    {
        if ( pRootSignature != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pRootSignature );

            if ( pVulkanRootSignature->m_pipelineLayout != VK_NULL_HANDLE )
            {
                vkDestroyPipelineLayout( pVulkanContext->m_device, pVulkanRootSignature->m_pipelineLayout, nullptr );
            }

            if ( pVulkanRootSignature->m_rootParameterSetLayout != VK_NULL_HANDLE )
            {
                vkDestroyDescriptorSetLayout( pVulkanContext->m_device, pVulkanRootSignature->m_rootParameterSetLayout, nullptr );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanRootSignature ) );
            pRootSignature = nullptr;
        }
    }

    //-------------------------------------------------------------------------

    PipelineCache* CreatePipelineCache( Context* pContext, PipelineCacheParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanPipelineCache* pVulkanPipelineCache = pVulkanContext->CreateObject<VulkanPipelineCache>();

        pVulkanPipelineCache->m_device = pVulkanContext->m_device;

        VkPipelineCacheCreateInfo cacheCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if ( parameters.m_flags.IsFlagSet( PipelineCacheFlags::ExternallySynchronized ) )
        {
            cacheCreateInfo.flags |= VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
        }
        cacheCreateInfo.initialDataSize = parameters.m_initialCacheData.size();
        cacheCreateInfo.pInitialData = parameters.m_initialCacheData.data();

        [[maybe_unused]] VkResult const result = vkCreatePipelineCache( pVulkanContext->m_device, &cacheCreateInfo, nullptr, &pVulkanPipelineCache->m_pipelineCache );
        EE_ASSERT( result == VK_SUCCESS );

        return pVulkanPipelineCache;
    }

    void DestroyPipelineCache( Context* pContext, PipelineCache*&& pPipelineCache )
    {
        if ( pPipelineCache != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanPipelineCache* pVulkanPipelineCache = static_cast<VulkanPipelineCache*>( pPipelineCache );

            if ( pVulkanPipelineCache->m_pipelineCache != VK_NULL_HANDLE )
            {
                vkDestroyPipelineCache( pVulkanContext->m_device, pVulkanPipelineCache->m_pipelineCache, nullptr );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanPipelineCache ) );
            pPipelineCache = nullptr;
        }
    }

    TArrayView<uint8_t> GetPipelineCacheData( Context* pContext, PipelineCache* pPipelineCache )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanPipelineCache* pVulkanPipelineCache = static_cast<VulkanPipelineCache*>( pPipelineCache );

        size_t dataSize = 0;
        [[maybe_unused]] VkResult result = vkGetPipelineCacheData( pVulkanContext->m_device, pVulkanPipelineCache->m_pipelineCache, &dataSize, nullptr );
        EE_ASSERT( result == VK_SUCCESS );

        // The view has to outlive the call, so the bytes live on the cache object.
        pVulkanPipelineCache->m_cacheData.resize( dataSize );

        result = vkGetPipelineCacheData( pVulkanContext->m_device, pVulkanPipelineCache->m_pipelineCache, &dataSize, pVulkanPipelineCache->m_cacheData.data() );
        EE_ASSERT( result == VK_SUCCESS );

        return TArrayView<uint8_t>( pVulkanPipelineCache->m_cacheData.data(), pVulkanPipelineCache->m_cacheData.size() );
    }

    //-------------------------------------------------------------------------

    static void BuildGraphicsPipelineStages( VulkanShader const* pVulkanShader, TInlineVector<VkPipelineShaderStageCreateInfo, 3>& outStages, TArrayView<ShaderStage const> stagesWanted )
    {
        for ( ShaderStage stage : stagesWanted )
        {
            int32_t stageIndex = -1;
            switch ( stage )
            {
                case ShaderStage::Vertex:   stageIndex = pVulkanShader->m_vertexStageIndex; break;
                case ShaderStage::Pixel:    stageIndex = pVulkanShader->m_pixelStageIndex; break;
                case ShaderStage::Task:     stageIndex = pVulkanShader->m_taskStageIndex; break;
                case ShaderStage::Mesh:     stageIndex = pVulkanShader->m_meshStageIndex; break;
                case ShaderStage::Compute:  stageIndex = pVulkanShader->m_computeStageIndex; break;
                default: break;
            }

            if ( stageIndex < 0 )
            {
                continue;
            }

            VkPipelineShaderStageCreateInfo stageCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stageCreateInfo.stage = VulkanShaderStage( stage );
            stageCreateInfo.module = pVulkanShader->m_shaderModules[stageIndex];
            stageCreateInfo.pName = pVulkanShader->m_stageEntryNames[stageIndex].c_str();

            outStages.push_back( stageCreateInfo );
        }
    }

    // One body for both the graphics and the mesh pipeline, because Vulkan builds both with
    // vkCreateGraphicsPipelines and the whole difference is which stages are wanted and whether
    // there is an input assembler. A second copy would only be somewhere for the two to drift.
    static Pipeline* CreateGraphicsOrMeshPipeline( Context* pContext, GraphicsPipelineParameters const& parameters, bool isMeshPipeline )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanPipelineCache* pVulkanPipelineCache = static_cast<VulkanPipelineCache*>( parameters.m_pPipelineCache );
        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( parameters.m_pRootSignature );
        VulkanShader* pVulkanShader = static_cast<VulkanShader*>( parameters.m_pShader );
        VulkanPipeline* pVulkanPipeline = pVulkanContext->CreateObject<VulkanPipeline>();

        pVulkanPipeline->m_device = pVulkanContext->m_device;
        pVulkanPipeline->m_pRootSignature = pVulkanRootSignature;
        pVulkanPipeline->m_pipelineType = PipelineType::Graphics;
        pVulkanPipeline->m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        pVulkanPipeline->m_primitiveTopology = ( parameters.m_primitiveTopology == PrimitiveTopology::TriangleStrip )
                                             ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                             : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // A task stage is optional even on a mesh pipeline, and BuildGraphicsPipelineStages
        // skips a stage the shader does not carry, so asking for all three is safe.
        ShaderStage const wantedGraphicsStages[2] = { ShaderStage::Vertex, ShaderStage::Pixel };
        ShaderStage const wantedMeshStages[3] = { ShaderStage::Task, ShaderStage::Mesh, ShaderStage::Pixel };

        TInlineVector<VkPipelineShaderStageCreateInfo, 3> stages;
        if ( isMeshPipeline )
        {
            EE_ASSERT( pVulkanShader->m_stages.IsFlagSet( ShaderStage::Mesh ) );
            BuildGraphicsPipelineStages( pVulkanShader, stages, TArrayView<ShaderStage const>( wantedMeshStages, 3 ) );
        }
        else
        {
            BuildGraphicsPipelineStages( pVulkanShader, stages, TArrayView<ShaderStage const>( wantedGraphicsStages, 2 ) );
        }

        // No vertex input state: the engine pulls vertices out of buffers in the shader rather than
        // binding them, so GraphicsPipelineParameters carries no input layout.
        VkPipelineVertexInputStateCreateInfo vertexInputState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssemblyState.topology = pVulkanPipeline->m_primitiveTopology;

        // Viewport and scissor are dynamic, set by CmdSetViewport and CmdSetScissor, so the
        // counts are all that matter here.
        VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizationState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rasterizationState.polygonMode = ( parameters.m_rasterizerState.m_fillMode == FillMode::Wireframe ) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rasterizationState.lineWidth = 1.0f;

        switch ( parameters.m_rasterizerState.m_cullMode )
        {
            case CullMode::None:    rasterizationState.cullMode = VK_CULL_MODE_NONE; break;
            case CullMode::Back:    rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT; break;
            case CullMode::Front:   rasterizationState.cullMode = VK_CULL_MODE_FRONT_BIT; break;
        }

        // Paired with the Y flip in CmdSetViewport, whose negative height already reverses winding
        // in framebuffer space. The mapping below looks like one inversion too many and is not:
        // reasoning it out rather than measuring it back-face culls every triangle in the engine.
        // Do not simplify it.
        rasterizationState.frontFace = ( parameters.m_rasterizerState.m_frontFace == FrontFace::ClockWise ) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;

        // D3D12's DepthClipEnable is the inverse of depthClampEnable, and the two are not identical:
        // clipping discards the primitive, clamping keeps it at the near or far plane.
        // VK_EXT_depth_clip_enable gives exact control and is not enabled. Nothing sets m_depthClip.
        rasterizationState.depthClampEnable = parameters.m_rasterizerState.m_depthClip ? VK_FALSE : VK_TRUE;

        rasterizationState.depthBiasEnable = ( parameters.m_rasterizerState.m_depthBias != 0 ) || ( parameters.m_rasterizerState.m_slopeScaledDepthBias != 0.0f );
        rasterizationState.depthBiasConstantFactor = float( parameters.m_rasterizerState.m_depthBias );
        rasterizationState.depthBiasClamp = parameters.m_rasterizerState.m_depthBiasClamp;
        rasterizationState.depthBiasSlopeFactor = parameters.m_rasterizerState.m_slopeScaledDepthBias;

        VkPipelineMultisampleStateCreateInfo multisampleState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisampleState.rasterizationSamples = VkSampleCountFlagBits( parameters.m_numSamples );
        multisampleState.alphaToCoverageEnable = parameters.m_blendState.m_alphaToCoverage;
        // m_sampleQuality has no Vulkan equivalent. Vulkan exposes only the sample count.

        VkPipelineDepthStencilStateCreateInfo depthStencilState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depthStencilState.depthTestEnable = parameters.m_depthStencilState.m_depthTest;
        depthStencilState.depthWriteEnable = parameters.m_depthStencilState.m_depthWrite;
        depthStencilState.depthCompareOp = VulkanCompareOp( parameters.m_depthStencilState.m_depthCompareMode );
        depthStencilState.stencilTestEnable = parameters.m_depthStencilState.m_stencilTest;

        depthStencilState.front.failOp = VulkanStencilOp( parameters.m_depthStencilState.m_stencilFrontFail );
        depthStencilState.front.passOp = VulkanStencilOp( parameters.m_depthStencilState.m_stencilFrontPass );
        depthStencilState.front.depthFailOp = VulkanStencilOp( parameters.m_depthStencilState.m_depthFrontFail );
        depthStencilState.front.compareOp = VulkanCompareOp( parameters.m_depthStencilState.m_stencilFrontCompareMode );
        depthStencilState.front.compareMask = parameters.m_depthStencilState.m_stencilReadMask;
        depthStencilState.front.writeMask = parameters.m_depthStencilState.m_stencilWriteMask;
        // reference is dynamic, set by CmdSetStencilReference.

        depthStencilState.back.failOp = VulkanStencilOp( parameters.m_depthStencilState.m_stencilBackFail );
        depthStencilState.back.passOp = VulkanStencilOp( parameters.m_depthStencilState.m_stencilBackPass );
        depthStencilState.back.depthFailOp = VulkanStencilOp( parameters.m_depthStencilState.m_depthBackFail );
        depthStencilState.back.compareOp = VulkanCompareOp( parameters.m_depthStencilState.m_stencilBackCompareMode );
        depthStencilState.back.compareMask = parameters.m_depthStencilState.m_stencilReadMask;
        depthStencilState.back.writeMask = parameters.m_depthStencilState.m_stencilWriteMask;

        TInlineVector<VkPipelineColorBlendAttachmentState, MaxRenderTargets> colorBlendAttachments;
        for ( uint32_t renderTargetIndex = 0; renderTargetIndex < parameters.m_numRenderTargets; ++renderTargetIndex )
        {
            VkPipelineColorBlendAttachmentState attachment = {};

            // D3D12 leaves a target outside the mask entirely default, which is blending off and no
            // colour written. Reproduced here rather than defaulting to a full write mask.
            if ( parameters.m_blendState.m_renderTargetMask.IsFlagSet( BlendStateTargetFlags( renderTargetIndex ) ) )
            {
                attachment.blendEnable = parameters.m_blendState.m_blendEnabled;
                attachment.srcColorBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_srcFactors[renderTargetIndex] );
                attachment.dstColorBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_dstFactors[renderTargetIndex] );
                attachment.colorBlendOp = VulkanBlendOp( parameters.m_blendState.m_blendModes[renderTargetIndex] );
                attachment.srcAlphaBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_srcAlphaFactors[renderTargetIndex] );
                attachment.dstAlphaBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_dstAlphaFactors[renderTargetIndex] );
                attachment.alphaBlendOp = VulkanBlendOp( parameters.m_blendState.m_blendModesAlpha[renderTargetIndex] );
                // The same bit order and values as D3D12, so the engine's 0x0F default is RGBA on both.
                attachment.colorWriteMask = VkColorComponentFlags( parameters.m_blendState.m_writeMasks[renderTargetIndex] );
            }

            colorBlendAttachments.push_back( attachment );
        }

        VkPipelineColorBlendStateCreateInfo colorBlendState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlendState.attachmentCount = uint32_t( colorBlendAttachments.size() );
        colorBlendState.pAttachments = colorBlendAttachments.data();
        // m_independentBlend has no Vulkan switch, and the engine fills every attachment either way.

        TInlineVector<VkDynamicState, 4> dynamicStates;
        dynamicStates.emplace_back( VK_DYNAMIC_STATE_VIEWPORT );
        dynamicStates.emplace_back( VK_DYNAMIC_STATE_SCISSOR );
        dynamicStates.emplace_back( VK_DYNAMIC_STATE_STENCIL_REFERENCE );

        // Declaring a dynamic state from a disabled extension is a validation error.
        // BeginCommandBuffer sets the full rate once, so the state is never left undefined.
        if ( g_fragmentShadingRateEnabled )
        {
            dynamicStates.emplace_back( VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR );
        }

        VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = uint32_t( dynamicStates.size() );
        dynamicState.pDynamicStates = dynamicStates.data();

        // Dynamic rendering, so there is no VkRenderPass and no framebuffer. The formats the
        // pipeline will be used with are declared here instead.
        TInlineVector<VkFormat, MaxRenderTargets> colorFormats;
        for ( DataFormat colorFormat : parameters.m_colorFormats )
        {
            // The same relabel CreateTexture applies to a render target, so the pipeline and the
            // image it draws into agree. See SubstituteSwapchainColorFormat.
            colorFormats.push_back( VulkanFormat( SubstituteSwapchainColorFormat( colorFormat ) ) );
        }

        VkPipelineRenderingCreateInfo renderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        renderingCreateInfo.colorAttachmentCount = uint32_t( colorFormats.size() );
        renderingCreateInfo.pColorAttachmentFormats = colorFormats.data();

        // D3D12 has one DSVFormat covering both aspects; Vulkan splits them, so a
        // depth-stencil format has to be named twice and a stencil-only format only once.
        VkFormat const depthStencilFormat = VulkanFormat( parameters.m_depthStencilFormat );
        if ( depthStencilFormat != VK_FORMAT_UNDEFINED )
        {
            bool const hasDepth = depthStencilFormat != VK_FORMAT_S8_UINT;
            bool const hasStencil = ( depthStencilFormat == VK_FORMAT_S8_UINT ) ||
                                    ( depthStencilFormat == VK_FORMAT_D16_UNORM_S8_UINT ) ||
                                    ( depthStencilFormat == VK_FORMAT_D24_UNORM_S8_UINT ) ||
                                    ( depthStencilFormat == VK_FORMAT_D32_SFLOAT_S8_UINT );

            renderingCreateInfo.depthAttachmentFormat = hasDepth ? depthStencilFormat : VK_FORMAT_UNDEFINED;
            renderingCreateInfo.stencilAttachmentFormat = hasStencil ? depthStencilFormat : VK_FORMAT_UNDEFINED;
        }

        VkGraphicsPipelineCreateInfo pipelineCreateInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineCreateInfo.pNext = &renderingCreateInfo;
        pipelineCreateInfo.stageCount = uint32_t( stages.size() );
        pipelineCreateInfo.pStages = stages.data();
        // Null on a mesh pipeline: there is no input assembler in front of a mesh shader, and the
        // specification says to leave both structures out.
        pipelineCreateInfo.pVertexInputState = isMeshPipeline ? nullptr : &vertexInputState;
        pipelineCreateInfo.pInputAssemblyState = isMeshPipeline ? nullptr : &inputAssemblyState;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pRasterizationState = &rasterizationState;
        pipelineCreateInfo.pMultisampleState = &multisampleState;
        pipelineCreateInfo.pDepthStencilState = &depthStencilState;
        pipelineCreateInfo.pColorBlendState = &colorBlendState;
        pipelineCreateInfo.pDynamicState = &dynamicState;
        pipelineCreateInfo.layout = pVulkanRootSignature->m_pipelineLayout;

        VkPipelineCache const cache = ( pVulkanPipelineCache != nullptr ) ? pVulkanPipelineCache->m_pipelineCache : VK_NULL_HANDLE;

        [[maybe_unused]] VkResult const result = vkCreateGraphicsPipelines( pVulkanContext->m_device, cache, 1, &pipelineCreateInfo, nullptr, &pVulkanPipeline->m_pipeline );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_PIPELINE, uint64_t( pVulkanPipeline->m_pipeline ), parameters.m_debugName );

        return pVulkanPipeline;
    }

    Pipeline* CreatePipeline( Context* pContext, GraphicsPipelineParameters const& parameters )
    {
        return CreateGraphicsOrMeshPipeline( pContext, parameters, false );
    }

    Pipeline* CreatePipeline( Context* pContext, MeshPipelineParameters const& parameters )
    {
        // A device without VK_EXT_mesh_shader gets an empty pipeline rather than a halt, for the
        // reason CreateShader skips a mesh module: this runs at startup for every shader in the
        // engine, so halting here would stop the engine over a pass it may never run.
        // CmdSetPipeline halts instead, which names the pass.
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        if ( !pVulkanContext->m_meshShader )
        {
            VulkanPipeline* pVulkanPipeline = pVulkanContext->CreateObject<VulkanPipeline>();
            pVulkanPipeline->m_device = pVulkanContext->m_device;
            pVulkanPipeline->m_pRootSignature = parameters.m_pRootSignature;
            pVulkanPipeline->m_pipelineType = PipelineType::Graphics;
            pVulkanPipeline->m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return pVulkanPipeline;
        }

        return CreateGraphicsOrMeshPipeline( pContext, parameters, true );
    }

    Pipeline* CreatePipeline( Context* pContext, ComputePipelineParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanPipelineCache* pVulkanPipelineCache = static_cast<VulkanPipelineCache*>( parameters.m_pPipelineCache );
        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( parameters.m_pRootSignature );
        VulkanShader* pVulkanShader = static_cast<VulkanShader*>( parameters.m_pShader );
        VulkanPipeline* pVulkanPipeline = pVulkanContext->CreateObject<VulkanPipeline>();

        EE_ASSERT( pVulkanShader->m_computeStageIndex >= 0 );

        pVulkanPipeline->m_device = pVulkanContext->m_device;
        pVulkanPipeline->m_pRootSignature = pVulkanRootSignature;
        pVulkanPipeline->m_pipelineType = PipelineType::Compute;
        pVulkanPipeline->m_bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;

        VkPipelineShaderStageCreateInfo stageCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageCreateInfo.module = pVulkanShader->m_shaderModules[pVulkanShader->m_computeStageIndex];
        stageCreateInfo.pName = pVulkanShader->m_stageEntryNames[pVulkanShader->m_computeStageIndex].c_str();

        VkComputePipelineCreateInfo pipelineCreateInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineCreateInfo.stage = stageCreateInfo;
        pipelineCreateInfo.layout = pVulkanRootSignature->m_pipelineLayout;

        VkPipelineCache const cache = ( pVulkanPipelineCache != nullptr ) ? pVulkanPipelineCache->m_pipelineCache : VK_NULL_HANDLE;

        [[maybe_unused]] VkResult const result = vkCreateComputePipelines( pVulkanContext->m_device, cache, 1, &pipelineCreateInfo, nullptr, &pVulkanPipeline->m_pipeline );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_PIPELINE, uint64_t( pVulkanPipeline->m_pipeline ), parameters.m_debugName );

        return pVulkanPipeline;
    }

    Pipeline* CreatePipeline( Context* pContext, RaytracingPipelineParameters const& parameters )
    {
        VulkanContext*       pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanPipelineCache* pVulkanPipelineCache = static_cast<VulkanPipelineCache*>( parameters.m_pPipelineCache );
        VulkanRootSignature* pVulkanGlobalRootSignature = static_cast<VulkanRootSignature*>( parameters.m_pGlobalRootSignature );
        VulkanPipeline*      pVulkanPipeline = pVulkanContext->CreateObject<VulkanPipeline>();

        EE_ASSERT( pVulkanContext->m_raytracing );
        EE_ASSERT( pVulkanPipelineCache == nullptr ); // Not implemented, as on the D3D12 backend
        EE_ASSERT( pVulkanGlobalRootSignature != nullptr );

        pVulkanPipeline->m_device = pVulkanContext->m_device;
        pVulkanPipeline->m_pRootSignature = pVulkanGlobalRootSignature;
        pVulkanPipeline->m_pipelineType = PipelineType::RayTracing;
        pVulkanPipeline->m_bindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;

        // RHI.h has one ShaderStage::RayTracing for all five roles, so the role comes from which
        // field the Shader arrived in rather than from the shader itself.
        TInlineVector<VkPipelineShaderStageCreateInfo, 8> stages;
        TInlineVector<VkRayTracingShaderGroupCreateInfoKHR, 8> groups;

        // Vulkan wants null terminated entry point names and RHI.h hands over StringViews. The
        // copies have to outlive vkCreateRayTracingPipelinesKHR, so they live here.
        //
        // Reserved to the exact maximum up front, because every pName points into this vector and
        // one reallocation part way through would dangle every pointer taken so far.
        size_t const maxEntryPointNames = 1 + parameters.m_rayMissShaders.size() + parameters.m_hitGroups.size() * 3;

        TVector<TInlineString<MaxEntryPointNameLength>> entryPointNames{ Memory::Allocators::g_RHI };
        entryPointNames.reserve( maxEntryPointNames );

        auto AddStage = [&stages, &entryPointNames] ( Shader const* pShader, VkShaderStageFlagBits stage, StringView entryPoint ) -> uint32_t
        {
            VulkanShader const* pVulkanShader = static_cast<VulkanShader const*>( pShader );

            // One module per raytracing Shader. Each role is created as its own Shader, so there
            // is nothing to pick between.
            EE_ASSERT( pVulkanShader->m_shaderModules.size() == 1 );

            entryPointNames.emplace_back( entryPoint.data(), entryPoint.size() );

            VkPipelineShaderStageCreateInfo stageCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stageCreateInfo.stage = stage;
            stageCreateInfo.module = pVulkanShader->m_shaderModules[0];
            stageCreateInfo.pName = entryPointNames.back().c_str();

            uint32_t const stageIndex = uint32_t( stages.size() );
            stages.emplace_back( stageCreateInfo );
            return stageIndex;
        };

        // Ray generation first, then the miss shaders, then the hit groups. That is the order
        // CmdDispatchRays reads the shader binding table in, and the group order is what the
        // table records map onto.
        {
            EE_ASSERT( parameters.m_pRayGenShader != nullptr );
            uint32_t const rayGenStageIndex = AddStage( parameters.m_pRayGenShader, VK_SHADER_STAGE_RAYGEN_BIT_KHR, parameters.m_rayGenEntryPoint );

            VkRayTracingShaderGroupCreateInfoKHR group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            group.generalShader = rayGenStageIndex;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
            groups.emplace_back( group );
        }

        EE_ASSERT( parameters.m_rayMissShaders.size() == parameters.m_rayMissEntryPoints.size() );
        for ( size_t missIndex = 0; missIndex < parameters.m_rayMissShaders.size(); ++missIndex )
        {
            uint32_t const missStageIndex = AddStage( parameters.m_rayMissShaders[missIndex], VK_SHADER_STAGE_MISS_BIT_KHR, parameters.m_rayMissEntryPoints[missIndex] );

            VkRayTracingShaderGroupCreateInfoKHR group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            group.generalShader = missStageIndex;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
            groups.emplace_back( group );
        }

        for ( RaytracingHitGroup const& hitGroup : parameters.m_hitGroups )
        {
            VkRayTracingShaderGroupCreateInfoKHR group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            group.generalShader = VK_SHADER_UNUSED_KHR;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;

            // An intersection shader is what makes a group procedural; without one the geometry is
            // triangles. D3D12 decides the same way.
            group.type = ( hitGroup.m_pIntersectionShader != nullptr )
                       ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
                       : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;

            if ( hitGroup.m_pIntersectionShader != nullptr )
            {
                group.intersectionShader = AddStage( hitGroup.m_pIntersectionShader, VK_SHADER_STAGE_INTERSECTION_BIT_KHR, hitGroup.m_intersectionEntryPoint );
            }

            if ( hitGroup.m_pAnyHitShader != nullptr )
            {
                group.anyHitShader = AddStage( hitGroup.m_pAnyHitShader, VK_SHADER_STAGE_ANY_HIT_BIT_KHR, hitGroup.m_anyHitEntryPoint );
            }

            if ( hitGroup.m_pClosestHitShader != nullptr )
            {
                group.closestHitShader = AddStage( hitGroup.m_pClosestHitShader, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, hitGroup.m_closestHitEntryPoint );
            }

            groups.emplace_back( group );
        }

        VkRayTracingPipelineCreateInfoKHR pipelineCreateInfo = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        pipelineCreateInfo.stageCount = uint32_t( stages.size() );
        pipelineCreateInfo.pStages = stages.data();
        pipelineCreateInfo.groupCount = uint32_t( groups.size() );
        pipelineCreateInfo.pGroups = groups.data();
        pipelineCreateInfo.maxPipelineRayRecursionDepth = parameters.m_maxTraceRecursionDepth;
        pipelineCreateInfo.layout = pVulkanGlobalRootSignature->m_pipelineLayout;

        // Several parameters have no Vulkan equivalent and are dropped on purpose:
        //  - The local root signatures let each D3D12 shader record carry its own bindings. Vulkan
        //    has one pipeline layout for the whole pipeline, so that data would have to move into
        //    the shader binding table and be read by the shader.
        //  - m_payloadSize and m_attributeSize are D3D12's shader config. Vulkan reads both out of
        //    the SPIR-V.
        //  - m_maxNumRays has no counterpart at all.
        EE_ASSERT( parameters.m_rayMissRootSignatures.empty() );

        VkPipelineCache const cache = ( pVulkanPipelineCache != nullptr ) ? pVulkanPipelineCache->m_pipelineCache : VK_NULL_HANDLE;

        [[maybe_unused]] VkResult const result = pVulkanContext->m_vkCreateRayTracingPipelines( pVulkanContext->m_device, VK_NULL_HANDLE, cache, 1, &pipelineCreateInfo, nullptr, &pVulkanPipeline->m_pipeline );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_PIPELINE, uint64_t( pVulkanPipeline->m_pipeline ), parameters.m_debugName );

        return pVulkanPipeline;
    }

    void DestroyPipeline( Context* pContext, Pipeline*&& pPipeline )
    {
        if ( pPipeline != nullptr )
        {
            VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanPipeline* pVulkanPipeline = static_cast<VulkanPipeline*>( pPipeline );

            if ( pVulkanPipeline->m_pipeline != VK_NULL_HANDLE )
            {
                vkDestroyPipeline( pVulkanContext->m_device, pVulkanPipeline->m_pipeline, nullptr );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanPipeline ) );
            pPipeline = nullptr;
        }
    }


    QueryPool* CreateQueryPool( Context* pContext, QueryPoolParameters const& parameters )
    {
        VulkanContext*   pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanQueryPool* pVulkanQueryPool = pVulkanContext->CreateObject<VulkanQueryPool>();

        // Vulkan has no linked-node adapter, so CreateContext always reports a single node.
        EE_ASSERT( parameters.m_nodeIndex == 0 );
        EE_ASSERT( parameters.m_numQueries > 0 );

        VkQueryPoolCreateInfo queryPoolCreateInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryPoolCreateInfo.queryCount = parameters.m_numQueries;

        switch ( parameters.m_queryType )
        {
            case QueryType::Timestamp:
            {
                queryPoolCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            }
            break;

            case QueryType::PipelineStatistics:
            {
                // Optional rather than required, so a device without it refuses the pool rather
                // than failing device selection. See CreateContext.
                EE_ASSERT( pVulkanContext->m_pipelineStatisticsQuery );

                queryPoolCreateInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                // The eleven counters of D3D12_QUERY_DATA_PIPELINE_STATISTICS, in the order that
                // structure declares them. Every one has a Vulkan equivalent, which is unusual
                // enough to be worth saying.
                queryPoolCreateInfo.pipelineStatistics =
                    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT |
                    VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
            }
            break;
        }

        [[maybe_unused]] VkResult const result = vkCreateQueryPool( pVulkanContext->m_device, &queryPoolCreateInfo, nullptr, &pVulkanQueryPool->m_queryPool );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanQueryPool->m_queryType = queryPoolCreateInfo.queryType;
        pVulkanQueryPool->m_numQueries = parameters.m_numQueries;

        SetDebugName( pVulkanContext, pVulkanQueryPool, parameters.m_debugName );

        return pVulkanQueryPool;
    }

    void DestroyQueryPool( Context* pContext, QueryPool*&& pQueryPool )
    {
        if ( pQueryPool != nullptr )
        {
            VulkanContext*   pVulkanContext = static_cast<VulkanContext*>( pContext );
            VulkanQueryPool* pVulkanQueryPool = static_cast<VulkanQueryPool*>( pQueryPool );

            if ( pVulkanQueryPool->m_queryPool != VK_NULL_HANDLE )
            {
                vkDestroyQueryPool( pVulkanContext->m_device, pVulkanQueryPool->m_queryPool, nullptr );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanQueryPool ) );
            pQueryPool = nullptr;
        }
    }

    double GetQueryTimestampFrequency( Queue* pQueue )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        // Vulkan reports nanoseconds per tick and D3D12 reports ticks per second, hence the
        // inversion. A period of 1.0 is one tick per nanosecond, which is a frequency of 1e9.
        EE_ASSERT( pVulkanQueue->m_timestampPeriod > 0.0F );

        return 1.0e9 / double( pVulkanQueue->m_timestampPeriod );
    }

    //-------------------------------------------------------------------------
    // Debug names
    //-------------------------------------------------------------------------
    // All nine reach SetVulkanObjectName, which already does nothing when the name is empty or the
    // extension is missing. The D3D12 backend asserts on an empty name because it converts to wide
    // characters first; there is no conversion here, and Create* passes an empty name whenever the
    // caller left one out.

    void SetDebugName( Context* pContext, Queue* pQueue, StringView debugName )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_QUEUE, uint64_t( pVulkanQueue->m_queue ), debugName );
    }

    void SetDebugName( Context* pContext, QueryPool* pQueryPool, StringView debugName )
    {
        VulkanQueryPool* pVulkanQueryPool = static_cast<VulkanQueryPool*>( pQueryPool );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_QUERY_POOL, uint64_t( pVulkanQueryPool->m_queryPool ), debugName );
    }

    void SetDebugName( Context* pContext, Buffer* pBuffer, StringView debugName )
    {
        VulkanBuffer* pVulkanBuffer = static_cast<VulkanBuffer*>( pBuffer );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_BUFFER, uint64_t( pVulkanBuffer->m_buffer ), debugName );
    }

    void SetDebugName( Context* pContext, Texture* pTexture, StringView debugName )
    {
        VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( pTexture );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_IMAGE, uint64_t( pVulkanTexture->m_image ), debugName );
    }

    void SetDebugName( Context* pContext, RootSignature* pRootSignature, StringView debugName )
    {
        // A root signature is a VkPipelineLayout here.
        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pRootSignature );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_PIPELINE_LAYOUT, uint64_t( pVulkanRootSignature->m_pipelineLayout ), debugName );
    }

    // A command signature records one command's byte layout and creates no Vulkan object, so there
    // is no handle for a name to reach.
    void SetDebugName( Context*, CommandSignature*, StringView )
    {
    }

    void SetDebugName( Context* pContext, Pipeline* pPipeline, StringView debugName )
    {
        VulkanPipeline* pVulkanPipeline = static_cast<VulkanPipeline*>( pPipeline );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_PIPELINE, uint64_t( pVulkanPipeline->m_pipeline ), debugName );
    }

    void SetDebugName( Context* pContext, CommandPool* pCommandPool, StringView debugName )
    {
        VulkanCommandPool* pVulkanCommandPool = static_cast<VulkanCommandPool*>( pCommandPool );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_COMMAND_POOL, uint64_t( pVulkanCommandPool->m_commandPool ), debugName );
    }

    void SetDebugName( Context* pContext, CommandBuffer* pCommandBuffer, StringView debugName )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        SetVulkanObjectName( static_cast<VulkanContext*>( pContext ), VK_OBJECT_TYPE_COMMAND_BUFFER, uint64_t( pVulkanCommandBuffer->m_commandBuffer ), debugName );
    }

    void ReportDeviceMemoryLeaks()
    {
        // Runs after DestroyContext, and Vulkan exposes no global live-object report, so this reads
        // what DestroyContext recorded from VMA while the allocator still existed. The validation
        // layers report leaked handles separately, through the debug messenger.
        if ( g_leakedDeviceAllocations > 0 )
        {
            EE_LOG_ERROR( LogCategory::Render, "RHI/ReportDeviceMemoryLeaks", "%llu device allocations leaked, totalling %llu bytes", g_leakedDeviceAllocations, g_leakedDeviceAllocationBytes );
        }
        else
        {
            EE_LOG_MESSAGE( LogCategory::Render, "RHI/ReportDeviceMemoryLeaks", "No device memory leaked" );
        }
    }

}
#endif
