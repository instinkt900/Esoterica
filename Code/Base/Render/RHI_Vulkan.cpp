#ifdef __linux__
#include "Base/Esoterica.h"

#include "RHI.h"

#include "Base/Math/Math.h"
#include "Base/Types/HashMap.h"
#include "Base/Render/HandleAllocator.h"
#include "Base/Encoding/Embed.h"

#include "EASTL/algorithm.h"

#include <vulkan/vulkan.h>
#include <dlfcn.h>

// SPIRV-Reflect replaces ID3D12ShaderReflection. See the P4.4 entry in Docs/Linux/Progress.md
// for why the dependency belongs to Phase 5 rather than Phase 4.
#include "spirv_reflect.h"

#include "renderdoc_app.h"

// VMA's implementation goes in exactly one translation unit, and this is the only Vulkan one.
//
// The pragma silences roughly 200 -Wnullability-completeness warnings raised by the header
// under -Wall -Wextra. They are noise: nothing first-party uses nullability attributes, and the
// Linux build passes no -Werror, so this keeps the build log readable rather than changing what
// is diagnosed anywhere else.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

//-------------------------------------------------------------------------
// Vulkan RHI backend
//-------------------------------------------------------------------------
// The Linux sibling of RHI_Direct3D12.cpp, which is the specification. Phase 5 replaces the
// Phase 1 stubs group by group; Docs/Linux/Progress.md records which groups are real.
//
// Implemented so far: P5.1, device, context and memory. Everything else still halts. A stub
// that returned quietly would surface as a baffling failure much later; one that halts names
// the function Phase 5 still owes you.
//
// Two decisions from Phase 4 are hard prerequisites, both recorded in Docs/Linux/Progress.md:
// the bindless binding model, which CreateContext below checks the device against, and
// clip-space Y, which the viewport inverts in P5.8 and the shader compiler does not.
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

    // RHI_Direct3D12.cpp defines this too, at :98. Exactly one backend is compiled per platform,
    // so the definition has to exist here or nothing in RHI.h that names the allocator links.
    MemoryAllocator g_RHI( "RHI" );
}

//-------------------------------------------------------------------------

namespace EE::Render::RHI
{
    //-------------------------------------------------------------------------
    // Device requirements
    //-------------------------------------------------------------------------
    // Fixed by the Phase 4 binding model. See the "P4.3 The bindless binding model" entry in
    // Docs/Linux/Progress.md, which names every set, binding and feature bit.
    //
    // The shaders are already compiled for this model, and there is no fallback path, so a
    // device that cannot meet these is refused rather than worked around.

    static char const* const g_requiredDeviceExtensions[] =
    {
        // Set 1 binding 0 aliases six descriptor types on one binding, which is what this
        // extension exists for. DXC's emulated heap emits exactly that.
        VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
        // CmdSetRootConstants and CmdSetRootParameter are push descriptor writes into set 0.
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        // P5.3 needs it, and a device is created once, so it is enabled here rather than
        // forcing device recreation when the swapchain arrives.
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // The two heaps, in set 1. The counts are Direct3D 12's, from its CreateContext at
    // RHI_Direct3D12.cpp:2229 and :2236. Keeping them identical is what makes a handle mean the
    // same thing on both backends, and both fit under UINT16_MAX so InvalidResourceHandle stays
    // outside either heap.
    static constexpr uint32_t g_resourceHeapSize = 64 * 1023;
    static constexpr uint32_t g_samplerHeapSize = 2048;

    static constexpr uint32_t g_rootParameterSet = 0;
    static constexpr uint32_t g_heapSet = 1;
    static constexpr uint32_t g_resourceHeapBinding = 0;
    static constexpr uint32_t g_samplerHeapBinding = 1;

    // Every type the engine's shaders pull out of ResourceDescriptorHeap. DXC's emulated heap
    // emits one OpTypeRuntimeArray per HLSL resource type, all decorated with this one binding,
    // which is what VK_EXT_mutable_descriptor_type exists for.
    static constexpr VkDescriptorType g_resourceHeapMutableTypes[] =
    {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,           // Texture2D, Texture2DArray, Texture3D, TextureCube
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           // RWTexture2D
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,    // Buffer<T>
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,    // RWBuffer<T>
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          // StructuredBuffer<T>, RWStructuredBuffer<T>, ByteAddressBuffer
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          // no shader today, but GetBufferHandle accepts ConstantBuffer
    };

    // Vulkan 1.3 is the baseline, so dynamic rendering, synchronization2, timeline semaphores,
    // descriptor indexing and buffer device address are all core. Only the feature bits that
    // are optional within core need asking for.
    struct RequiredFeatures
    {
        VkPhysicalDeviceVulkan13Features                    m_vulkan13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features                    m_vulkan12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan11Features                    m_vulkan11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT    m_mutableDescriptorType = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
        VkPhysicalDeviceFeatures2                           m_features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

        // Chains the structures onto m_features2. Call after any copy, because copying moves
        // the structures and leaves every pNext pointing at the original.
        void Chain()
        {
            m_features2.pNext = &m_vulkan11;
            m_vulkan11.pNext = &m_vulkan12;
            m_vulkan12.pNext = &m_vulkan13;
            m_vulkan13.pNext = &m_mutableDescriptorType;
            m_mutableDescriptorType.pNext = nullptr;
        }
    };

    // RHI.h declares this and RHI_Direct3D12.cpp defines it at :1291. Exactly one backend is
    // compiled per platform, so the definition has to exist here too: Context derives from it,
    // and instantiating VulkanContext emits references to its vtable, typeinfo and destructor.
    EE_BASE_API GenericResource::~GenericResource() = default;

    // Defined in the queues section below, next to the first caller that needed it.
    struct VulkanContext;
    static void SetVulkanObjectName( VulkanContext* pVulkanContext, VkObjectType objectType, uint64_t objectHandle, StringView debugName );

    // Defined in the draw commands section. EndCommandBuffer has to close any open dynamic
    // rendering, and it is defined before that section.
    struct VulkanCommandBuffer;
    static void EndRenderingIfActive( VulkanCommandBuffer* pVulkanCommandBuffer );

    //-------------------------------------------------------------------------

    struct ResourceAllocStats
    {
        uint64_t                                            m_numAllocations = 0;
        uint64_t                                            m_numBytes = 0;
    };

    // Vulkan has no equivalent of DXGI's live-object report, which the Direct3D 12 backend uses
    // in ReportDeviceMemoryLeaks. That call runs after DestroyContext, by which point the VMA
    // allocator that knows the answer is gone, so DestroyContext records what it saw here.
    static uint64_t g_leakedDeviceAllocations = 0;
    static uint64_t g_leakedDeviceAllocationBytes = 0;

    struct VulkanContext : Context
    {
        VkInstance                                                      m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT                                        m_debugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice                                                m_physicalDevice = VK_NULL_HANDLE;
        VkDevice                                                        m_device = VK_NULL_HANDLE;
        VmaAllocator                                                    m_resourceAllocator = VK_NULL_HANDLE;

        // Stored without its pNext chain. The subgroup properties it is queried with live on
        // the stack in FillDeviceCapabilities, and keeping the pointer would dangle.
        VkPhysicalDeviceProperties                                      m_physicalDeviceProperties = {};
        VkPhysicalDeviceMemoryProperties                                m_memoryProperties = {};

        // VMA holds this by pointer for the allocator's whole life, so it cannot be a local.
        VkAllocationCallbacks                                           m_hostAllocationCallbacks = {};

        // Set 1 of the Phase 4 binding model: one layout for every pipeline in the engine,
        // allocated once and bound once. CmdSetPipeline binds it, because a pipeline with a
        // different set 0 layout disturbs it.
        VkDescriptorSetLayout                                           m_heapSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool                                                m_heapDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet                                                 m_heapDescriptorSet = VK_NULL_HANDLE;

        // Index allocators for the two heaps. HandleAllocator is platform-neutral and is the
        // same one the Direct3D 12 backend uses for its descriptor heaps, so a handle is
        // allocated the same way on both sides.
        HandleAllocator<GenericResourceHandle>                          m_resourceHeapAllocator;
        HandleAllocator<GenericResourceHandle>                          m_samplerHeapAllocator;

        // Chosen here because vkCreateDevice takes the queue create infos. P5.2 turns these
        // into Queue objects. An index of ~0U means the device exposes no such family and the
        // graphics family stands in, which is legal: every graphics family also supports
        // compute and transfer.
        uint32_t                                                        m_graphicsQueueFamily = ~0U;
        uint32_t                                                        m_computeQueueFamily = ~0U;
        uint32_t                                                        m_transferQueueFamily = ~0U;

        // How many VkQueues were actually created from each family, and which one CreateQueue
        // hands out next. A family the device only exposes one queue on gives every RHI queue
        // the same VkQueue, which is correct but serialises them.
        struct QueueFamilyAllocation
        {
            uint32_t                                        m_familyIndex = ~0U;
            uint32_t                                        m_numQueues = 0;
            uint32_t                                        m_nextQueueIndex = 0;
        };

        TInlineVector<QueueFamilyAllocation, 3>                         m_queueFamilyAllocations;

        // Queue::m_unifiedMemory, which Direct3D 12 reads from D3D12MA's IsUMA(). True when
        // every device-local memory type is also host-visible, which is what a shared-memory
        // GPU looks like from here.
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

        // VK_EXT_debug_utils is an extension, so its entry points are not exported by the
        // loader and have to be looked up. Null when the extension is not present.
        PFN_vkSetDebugUtilsObjectNameEXT                                m_vkSetDebugUtilsObjectName = nullptr;
        // Core in Vulkan 1.4 as vkCmdPushDescriptorSet, but the baseline here is 1.3, so it is
        // the KHR entry point and has to be looked up.
        PFN_vkCmdPushDescriptorSetKHR                                   m_vkCmdPushDescriptorSet = nullptr;

        void*                                                           m_pRenderDocLibrary = nullptr;
        RENDERDOC_API_1_0_0*                                            m_pRenderDocAPI = nullptr;

        // Filled by P5.5 and P5.6 as buffers and textures are created. GetResourceAllocation-
        // Statistics reads them, and DestroyContext asserts they are back to zero.
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

    // Every reason a device can be rejected, so that the log can name the missing thing rather
    // than saying "no suitable device".
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
        // The constant buffer layout rule: the Reflector passes -fvk-use-dx-layout, so the
        // device has to accept scalar block layout or every cbuffer reads at the wrong offsets.
        if ( !available.m_vulkan12.scalarBlockLayout )              { return "scalarBlockLayout"; }
        if ( !available.m_vulkan12.descriptorIndexing )             { return "descriptorIndexing"; }
        if ( !available.m_vulkan12.runtimeDescriptorArray )         { return "runtimeDescriptorArray"; }
        if ( !available.m_vulkan12.descriptorBindingPartiallyBound ){ return "descriptorBindingPartiallyBound"; }
        if ( !available.m_vulkan12.descriptorBindingVariableDescriptorCount ) { return "descriptorBindingVariableDescriptorCount"; }
        // The engine writes handles into the heap while command buffers that reference it are
        // recording, exactly as CopyDescriptorsSimple does on Direct3D 12.
        if ( !available.m_vulkan12.descriptorBindingSampledImageUpdateAfterBind )  { return "descriptorBindingSampledImageUpdateAfterBind"; }
        if ( !available.m_vulkan12.descriptorBindingStorageImageUpdateAfterBind )  { return "descriptorBindingStorageImageUpdateAfterBind"; }
        if ( !available.m_vulkan12.descriptorBindingStorageBufferUpdateAfterBind ) { return "descriptorBindingStorageBufferUpdateAfterBind"; }
        if ( !available.m_vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind ) { return "descriptorBindingUniformTexelBufferUpdateAfterBind"; }
        // NonUniformResourceIndex in the shaders indexes the heap with a divergent index.
        if ( !available.m_vulkan12.shaderSampledImageArrayNonUniformIndexing )  { return "shaderSampledImageArrayNonUniformIndexing"; }
        if ( !available.m_vulkan12.shaderStorageImageArrayNonUniformIndexing )  { return "shaderStorageImageArrayNonUniformIndexing"; }
        if ( !available.m_vulkan12.shaderStorageBufferArrayNonUniformIndexing ) { return "shaderStorageBufferArrayNonUniformIndexing"; }
        if ( !available.m_vulkan12.shaderUniformTexelBufferArrayNonUniformIndexing ) { return "shaderUniformTexelBufferArrayNonUniformIndexing"; }
        if ( !available.m_mutableDescriptorType.mutableDescriptorType ) { return "mutableDescriptorType"; }

        return nullptr;
    }

    // Higher is better. Mirrors DXGI_GPU_PREFERENCE, which is what the Direct3D 12 backend asks
    // the factory for, rather than inventing a different notion of "best".
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
            // CPU and Other. The Direct3D 12 backend skips DXGI_ADAPTER_FLAG3_SOFTWARE outright,
            // so a software device is a last resort here too.
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

        // A device with no dedicated async family is normal, and the graphics family stands in.
        // Every graphics-capable family also supports compute and transfer, so this is correct
        // rather than a fallback that loses work.
        if ( pVulkanContext->m_computeQueueFamily == ~0U )
        {
            pVulkanContext->m_computeQueueFamily = pVulkanContext->m_graphicsQueueFamily;
        }

        if ( pVulkanContext->m_transferQueueFamily == ~0U )
        {
            pVulkanContext->m_transferQueueFamily = pVulkanContext->m_graphicsQueueFamily;
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

        // Direct3D 12 reports DedicatedVideoMemory from the adapter description. The Vulkan
        // equivalent is the size of the heaps marked device local; on an integrated GPU that is
        // shared system memory, which is also what DXGI reports there.
        for ( uint32_t heapIndex = 0; heapIndex < pVulkanContext->m_memoryProperties.memoryHeapCount; ++heapIndex )
        {
            VkMemoryHeap const& heap = pVulkanContext->m_memoryProperties.memoryHeaps[heapIndex];
            if ( heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
            {
                capabilities.m_dedicatedVideoMemory += heap.size;
            }
        }

        // Direct3D 12 reads this from D3D12MA's IsUMA(). The Vulkan equivalent is that no
        // memory is device-only: every device-local type can also be mapped.
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

        // 13 DWORDs, copied from the Direct3D 12 backend rather than derived. It is an AMD
        // packet-size heuristic that has no Vulkan meaning, and the value only feeds the
        // engine's own root signature sizing, which must agree across both backends.
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

        // Matches the Direct3D 12 backend, which sets these to NotSupported with a TODO. P5.15
        // owns variable rate shading, and reporting a capability the backend cannot honour
        // would make the engine issue calls that halt.
        capabilities.m_shadingRate = ShadingRate::NotSupported;
        capabilities.m_shadingRateCaps = ShadingRateCaps::NotSupported;
        capabilities.m_shadingRateTexelWidth = 0;
        capabilities.m_shadingRateTexelHeight = 0;

        // drawIndirectCount is required above, so multi-draw indirect is always available.
        capabilities.m_multiDrawIndirect = true;
        // Direct3D 12 command signatures can set root constants per draw and Vulkan's indirect
        // draws cannot. P5.13 decides whether a compute pre-pass covers the gap; until then
        // this is false, which is the honest answer and the one that keeps the engine off the
        // path that does not exist yet.
        capabilities.m_indirectRootConstant = false;
        // VK_EXT_fragment_shader_interlock is the equivalent, and nothing enables it yet.
        capabilities.m_rasterizerOrderViews = false;
        // Breadcrumbs are DRED, whose equivalent is VK_AMD_buffer_marker or
        // VK_NV_device_diagnostic_checkpoints. Neither is wired up.
        capabilities.m_breadcrumbs = false;
        // HDR needs a swapchain colour space, which is P5.3's business.
        capabilities.m_hdr = false;

        // m_canShaderReadFrom, m_canShaderWriteTo and m_canRenderTargetWriteTo stay false.
        // Filling them needs the complete DataFormat to VkFormat mapping, which is P5.6's task
        // and its largest piece. Duplicating a partial mapping here would be the worst of both:
        // the phase document warns that a disagreement between the two corrupts textures in a
        // way that looks like a bug somewhere else.
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
        // The validation layers call this from whichever thread tripped the check, which may be
        // one the engine's allocator has never seen. RHI_Direct3D12.cpp does the same in its
        // info queue callback.
        if ( !Memory::HasInitializedThreadHeap() )
        {
            Memory::InitializeThreadHeap();
        }

        if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT )
        {
            // Phase 5 treats a validation error as a build break. Halting here is what makes
            // that true rather than aspirational.
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

        // RenderDoc
        //-------------------------------------------------------------------------
        // dlopen with RTLD_NOLOAD, so this attaches to a RenderDoc that already injected itself
        // and never loads one that is not there. That mirrors GetModuleHandleA on Windows,
        // which also only finds an already-loaded module.

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

        if ( enableValidation )
        {
            if ( validationLayerAvailable )
            {
                instanceLayers.emplace_back( "VK_LAYER_KHRONOS_validation" );
                instanceExtensions.emplace_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
            }
            else
            {
                // Not fatal, and worth saying out loud: Phase 5 asks for validation to stay on
                // throughout, and a missing layer package is the usual reason it silently is not.
                EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "VK_LAYER_KHRONOS_validation is not installed, so validation is off. Install vulkan-validationlayers." );
            }
        }
        else
        {
            // Object names and command buffer markers are worth having whenever the extension
            // is present, with or without the validation layer. P5.12 uses them.
            uint32_t numInstanceExtensions = 0;
            vkEnumerateInstanceExtensionProperties( nullptr, &numInstanceExtensions, nullptr );
            TVector<VkExtensionProperties> availableInstanceExtensions( numInstanceExtensions );
            vkEnumerateInstanceExtensionProperties( nullptr, &numInstanceExtensions, availableInstanceExtensions.data() );

            if ( HasExtension( availableInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) )
            {
                instanceExtensions.emplace_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
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
        // Raytracing core counts are not exposed by any Vulkan query. Direct3D 12 does not fill
        // this either; it is vendor telemetry, and nothing in the engine reads it.
        pVulkanContext->m_vendorInfo.m_numRaytracingCores = 0;

        EE_LOG_MESSAGE
        (
            LogCategory::Render, "RHI/CreateContext", "GPU %s %s %s",
            pVulkanContext->m_vendorInfo.m_vendorID.c_str(),
            pVulkanContext->m_vendorInfo.m_deviceID.c_str(),
            pVulkanContext->m_vendorInfo.m_deviceName.c_str()
        );

        // Vulkan has no equivalent of a Direct3D 12 linked node adapter. Multi-GPU is explicit
        // device groups, which the engine never asks for, so the mode is always Single and
        // there is exactly one node.
        pVulkanContext->m_numLinkedNodes = 1;
        pVulkanContext->m_unlinkedNodeIndex = 0;
        pVulkanContext->m_deviceMode = DeviceMode::Single;
        pVulkanContext->m_shaderModel = parameters.m_shaderModel;

        // Device
        //-------------------------------------------------------------------------

        // One VkQueue per RHI queue where the family allows it. Two RHI queues sharing one
        // VkQueue is legal but it makes a cross-queue QueueDeviceWait between them a deadlock:
        // the wait would be for a value that only a later submit on the same VkQueue signals.
        // Asking for distinct queues up front is what keeps that from being possible.
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

            // Never ask for more queues than the family has. A family with one queue is normal
            // on integrated parts.
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

        // Ask only for what GetDeviceRejectionReason already verified is present. Enabling a
        // feature the device does not have is a validation error, and enabling one nothing uses
        // costs performance on some drivers.
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
        enabledFeatures.m_vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        enabledFeatures.m_vulkan12.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;

        enabledFeatures.m_mutableDescriptorType.mutableDescriptorType = VK_TRUE;

        VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceCreateInfo.pNext = &enabledFeatures.m_features2;
        deviceCreateInfo.queueCreateInfoCount = uint32_t( queueCreateInfos.size() );
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.enabledExtensionCount = uint32_t( eastl::size( g_requiredDeviceExtensions ) );
        deviceCreateInfo.ppEnabledExtensionNames = g_requiredDeviceExtensions;

        result = vkCreateDevice( pVulkanContext->m_physicalDevice, &deviceCreateInfo, nullptr, &pVulkanContext->m_device );
        EE_ASSERT( result == VK_SUCCESS );

        // Resolved here rather than later, because everything created from this point on names
        // itself and SetVulkanObjectName reads it.
        pVulkanContext->m_vkSetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkSetDebugUtilsObjectNameEXT" ) );
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
        // Set 1 of the Phase 4 binding model. See the binding model entry in
        // Docs/Linux/Progress.md, which fixes every number here.

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

        // PARTIALLY_BOUND because most of a 65472-slot heap is empty at any moment, and
        // UPDATE_AFTER_BIND because the engine writes handles into the heap while command
        // buffers that reference it are recording, exactly as CopyDescriptorsSimple does on
        // Direct3D 12.
        //
        // No VARIABLE_DESCRIPTOR_COUNT. The binding model entry lists it for both bindings and
        // Vulkan does not allow that: only the highest-numbered binding in a set may carry it.
        // It is also not needed, because both heaps are allocated at their full declared size.
        // See the correction recorded in Docs/Linux/Progress.md.
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

        EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "ShaderResource descriptor pool size: %i", g_resourceHeapSize );
        EE_LOG_MESSAGE( LogCategory::Render, "RHI/CreateContext", "Sampler descriptor pool size: %i", g_samplerHeapSize );

        //-------------------------------------------------------------------------

        pVulkanContext->m_hostValidation = parameters.m_enableHostValidation && validationLayerAvailable;
        pVulkanContext->m_deviceValidation = parameters.m_enableDeviceValidation && validationLayerAvailable;
        // DRED has no portable Vulkan equivalent, and nothing here provides one.
        pVulkanContext->m_deviceBreadcrumbs = false;
        pVulkanContext->m_renderDoc = pVulkanContext->m_pRenderDocAPI != nullptr;
        // AGS is Direct3D 12 only by construction. See Docs/Linux/03-Dependencies.md.
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

        // Every per-descriptor-type allocation must have been freed, the same check the
        // Direct3D 12 backend makes.
        for ( auto const& pair : pVulkanContext->m_bufferStats )
        {
            EE_ASSERT( pair.second.m_numAllocations == 0 );
            EE_ASSERT( pair.second.m_numBytes == 0 );
        }

        for ( auto const& pair : pVulkanContext->m_textureStats )
        {
            EE_ASSERT( pair.second.m_numAllocations == 0 );
            EE_ASSERT( pair.second.m_numBytes == 0 );
        }

        pVulkanContext->m_resourceHeapAllocator.Shutdown();
        pVulkanContext->m_samplerHeapAllocator.Shutdown();

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
            // Vulkan has no global live-object registry, so anything VMA still holds is recorded
            // here while the allocator still exists. ReportDeviceMemoryLeaks reads it, and it
            // runs after this point; see BaseModule::ShutdownModule.
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

        // "Local" is Direct3D 12's word for device memory. The Vulkan split is the
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
            // RENDERDOC_DEVICEPOINTER for Vulkan is the dispatch table pointer at the start of
            // the VkInstance, not the instance handle. RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE
            // is the macro RenderDoc's own header provides for exactly this.
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
    // P5.12 owns the nine SetDebugName overloads. This is the one call underneath all of them,
    // written here because CreateQueue needs it and the phase document says to do debug utils
    // early: a named object makes every later group easier to debug.

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
    // RHI.h exposes a monotonic counter, which is a Vulkan timeline semaphore and not a binary
    // one. One timeline per queue, and the counter the RHI hands back is a value on it.
    //
    // The counter runs the way Direct3D 12 runs its fence: m_nextSemaphoreValue is the value the
    // *next* submit will signal, so QueueGetCurrentSemaphore returns a value that has not been
    // signalled yet. That is what the Direct3D 12 backend does with m_fenceValue, and the engine
    // is written against it.

    struct VulkanQueue final : Queue
    {
        // QueueGetCompletedSemaphore and QueueHostWait take no Context, so the queue has to
        // carry the device it belongs to.
        VkDevice                                            m_device = VK_NULL_HANDLE;
        VkQueue                                             m_queue = VK_NULL_HANDLE;
        VkSemaphore                                         m_timelineSemaphore = VK_NULL_HANDLE;
        uint64_t                                            m_nextSemaphoreValue = 1;
        uint32_t                                            m_queueFamilyIndex = ~0U;

        // Vulkan has no standalone queue wait. ID3D12CommandQueue::Wait blocks everything
        // submitted to the queue after it, and the only Vulkan construct with that meaning is a
        // wait attached to a submit. So QueueDeviceWait records the wait here and the next
        // QueueSubmit drains it.
        //
        // An empty submit carrying just the wait would be the obvious alternative and it is
        // wrong: submits on one queue may overlap, so a wait in submit N does not hold back
        // submit N+1.
        TInlineVector<VkSemaphoreSubmitInfo, 8>             m_pendingWaits;

        TVector<VkCommandBufferSubmitInfo>                  m_submitCommandBuffers{ Memory::Allocators::g_RHI };
    };

    // Declared here rather than in the P5.4 section below, because QueueSubmit reads the handle
    // out of it and is defined first.
    struct VulkanCommandBuffer final : CommandBuffer
    {
        // Mirrors Direct3D12CommandBuffer::Stage. Vulkan tracks the same lifecycle itself and
        // the validation layers enforce it, but an EE_ASSERT names the caller that got it wrong
        // instead of a message from the layer three frames later.
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

        // Direct3D 12 has no begin and end around a set of render targets; Vulkan's dynamic
        // rendering does. CmdSetRenderTargets ends the previous one before beginning the next,
        // and EndCommandBuffer ends the last. Anything that may not run inside a render pass -
        // a dispatch, a copy, a barrier - has to end it too.
        bool                                                m_isRendering = false;

        // The pipeline layout of the currently bound pipeline. Push descriptors need it, and
        // CommandBuffer::m_pBoundPipeline only carries the platform-neutral pointer.
        VkPipelineLayout                                    m_boundPipelineLayout = VK_NULL_HANDLE;
        VkPipelineBindPoint                                 m_boundBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

        // Root constants are not Vulkan push constants; see CmdSetRootConstants. Each set of
        // them is copied into this ring and a descriptor is pushed at the copy. One ring per
        // command buffer, reset in BeginCommandBuffer, which is safe because Vulkan already
        // requires the previous submission of a command buffer to have completed before it can
        // be re-recorded.
        Buffer*                                             m_pRootConstantRing = nullptr;
        uint64_t                                            m_rootConstantRingOffset = 0;

        // CmdSetRootConstants and CmdSetRootParameter take no Context, so the entry point comes
        // along on the command buffer.
        PFN_vkCmdPushDescriptorSetKHR                       m_vkCmdPushDescriptorSet = nullptr;
    };

    //-------------------------------------------------------------------------
    // Resource types
    //-------------------------------------------------------------------------
    // All in one place, because RHI.h declares the draw commands before the buffers, textures
    // and pipelines they act on, so a definition next to its own functions would come too late.
    // The functions stay in RHI.h's section order; only the types are gathered.

    struct VulkanCommandPool final : CommandPool
    {
        VkCommandPool                                       m_commandPool = VK_NULL_HANDLE;
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

        // BufferFlags::SubAllocations. The Direct3D 12 backend uses a D3D12MA virtual block for
        // exactly this, and VMA has the same thing.
        VmaVirtualBlock                                     m_virtualBlock = VK_NULL_HANDLE;

        // A contiguous run in the resource heap, laid out the way Direct3D 12 lays it out:
        // constant buffer first if present, then the read view, then the read-write view.
        HandleAllocator<GenericResourceHandle>::Handle      m_descriptorHandles = {};
        int8_t                                              m_srvDescriptorOffset = -1;
        int8_t                                              m_uavDescriptorOffset = -1;

        ReadRange                                           m_mappedRange = {};
    };

    struct VulkanTexture final : Texture
    {
        VkImage                                             m_image = VK_NULL_HANDLE;
        VkImageView                                         m_imageView = VK_NULL_HANDLE;
        VkFormat                                            m_format = VK_FORMAT_UNDEFINED;
        VkExtent3D                                          m_extent = {};
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

    // Enough for every root constant set in one command buffer. Root constants are a handful of
    // uint32s each, and RHI.esh declares one block per shader, so this is generous by a wide
    // margin. It is asserted rather than wrapped: silently wrapping would overwrite constants
    // the GPU is still reading, and the failure would look like a shader bug.
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

        // Hand out a distinct VkQueue from the family while there is one left, and repeat the
        // last one after that. Repeating is legal, and CreateContext already asked for as many
        // as the family allows, so it only happens on a device that cannot do better.
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

        VkSemaphoreTypeCreateInfo semaphoreTypeCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        semaphoreTypeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        semaphoreTypeCreateInfo.initialValue = 0;

        VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        semaphoreCreateInfo.pNext = &semaphoreTypeCreateInfo;

        VkResult const result = vkCreateSemaphore( pVulkanContext->m_device, &semaphoreCreateInfo, nullptr, &pVulkanQueue->m_timelineSemaphore );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_QUEUE, uint64_t( pVulkanQueue->m_queue ), parameters.m_debugName );

        // QueuePriority is not honoured. Vulkan fixes queue priorities at vkCreateDevice, and
        // CreateQueue runs long after that, so the priority would need the device recreated.
        // Nothing in the engine sets it: RenderSystem::Initialize leaves all three queues on
        // Normal. VK_EXT_global_priority is what GlobalRealtime would need, also at device
        // creation time. Revisit only when a caller actually asks for a priority.
        //
        // QueueFlags::DisableTimeout is D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT, which has
        // no Vulkan equivalent at all. Nothing sets it either.

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
        VkResult const result = vkGetSemaphoreCounterValue( pVulkanQueue->m_device, pVulkanQueue->m_timelineSemaphore, &completedValue );
        EE_ASSERT( result == VK_SUCCESS );

        return completedValue;
    }

    void QueueHostWait( Queue* pQueue, uint64_t semaphore )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        // Value 0 is the timeline's initial value, so it is always already satisfied. The
        // Direct3D 12 backend skips it for the same reason, and says so at its own call site.
        if ( semaphore == 0 )
        {
            return;
        }

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &pVulkanQueue->m_timelineSemaphore;
        waitInfo.pValues = &semaphore;

        VkResult const result = vkWaitSemaphores( pVulkanQueue->m_device, &waitInfo, UINT64_MAX );
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
        // mean the same thing. It is the correct-but-untuned choice the phase document allows,
        // and it is recorded as an ALL_COMMANDS site in Docs/Linux/Progress.md. Narrowing it
        // needs to know what the waiting submit will do, which the caller does not tell us.
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        pVulkanQueueThatWaits->m_pendingWaits.emplace_back( waitInfo );
    }

    uint64_t QueueSubmit( Queue* pQueue, TArrayView<CommandBuffer*> commandBuffers )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        pVulkanQueue->m_submitCommandBuffers.clear();
        pVulkanQueue->m_submitCommandBuffers.reserve( commandBuffers.size() );

        for ( CommandBuffer* pCommandBuffer : commandBuffers )
        {
            VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

            VkCommandBufferSubmitInfo commandBufferSubmitInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            commandBufferSubmitInfo.commandBuffer = pVulkanCommandBuffer->m_commandBuffer;
            pVulkanQueue->m_submitCommandBuffers.emplace_back( commandBufferSubmitInfo );
        }

        uint64_t const signalSemaphore = pVulkanQueue->m_nextSemaphoreValue++;

        VkSemaphoreSubmitInfo signalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signalInfo.semaphore = pVulkanQueue->m_timelineSemaphore;
        signalInfo.value = signalSemaphore;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.waitSemaphoreInfoCount = uint32_t( pVulkanQueue->m_pendingWaits.size() );
        submitInfo.pWaitSemaphoreInfos = pVulkanQueue->m_pendingWaits.data();
        submitInfo.commandBufferInfoCount = uint32_t( pVulkanQueue->m_submitCommandBuffers.size() );
        submitInfo.pCommandBufferInfos = pVulkanQueue->m_submitCommandBuffers.data();
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;

        // Direct3D 12 skips ExecuteCommandLists on an empty list but still signals. Signalling
        // with no work is exactly what an empty submit does here, so the shape is the same.
        VkResult const result = vkQueueSubmit2( pVulkanQueue->m_queue, 1, &submitInfo, VK_NULL_HANDLE );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanQueue->m_pendingWaits.clear();

        return signalSemaphore;
    }

    uint64_t QueuePresent( Queue* pQueue, Swapchain* pSwapchain, uint32_t imageIndex )
    {
        // P5.3 finishes this, and it cannot be written before the swapchain exists.
        //
        // VkPresentInfoKHR takes binary semaphores only; it has no timeline path. So the
        // swapchain has to carry a binary semaphore per image, the submit before the present
        // has to signal it alongside the timeline value, and the present waits on that binary
        // semaphore. Acquire needs the mirror image of the same thing.
        //
        // Everything else here is ready: the timeline value this returns is the one the queue
        // signals, exactly as QueueSubmit does.
        EE_UNIMPLEMENTED_FUNCTION();
        return 0;
    }

    void WaitQueueIdle( Queue* pQueue )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        // Direct3D 12 signals its fence and blocks on an event, because it has no queue-idle
        // call. Vulkan has one, and it means precisely this.
        VkResult const result = vkQueueWaitIdle( pVulkanQueue->m_queue );
        EE_ASSERT( result == VK_SUCCESS );
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

    //-------------------------------------------------------------------------
    // Command pools and buffers
    //-------------------------------------------------------------------------


    //-------------------------------------------------------------------------

    CommandPool* CreateCommandPool( Context* pContext, CommandPoolParameters const& parameters )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( parameters.m_pQueue );
        VulkanCommandPool* pVulkanCommandPool = pVulkanContext->CreateObject<VulkanCommandPool>();

        VkCommandPoolCreateInfo commandPoolCreateInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commandPoolCreateInfo.queueFamilyIndex = pVulkanQueue->m_queueFamilyIndex;
        // RESET_COMMAND_BUFFER_BIT, so that vkBeginCommandBuffer implicitly resets the one
        // buffer. That is what ID3D12GraphicsCommandList::Reset( allocator, nullptr ) does, and
        // the engine calls BeginCommandBuffer per buffer rather than resetting the pool every
        // time. Without the bit, a second Begin without an intervening ResetCommandPool is
        // invalid. It can cost a little, because some drivers give such a pool per-buffer
        // allocators, and correctness comes first.
        commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkResult const result = vkCreateCommandPool( pVulkanContext->m_device, &commandPoolCreateInfo, nullptr, &pVulkanCommandPool->m_commandPool );
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

            // Destroying the pool frees every command buffer allocated from it, so a buffer
            // that outlives its pool is already a use-after-free. The engine destroys buffers
            // first; see RenderSystem::Shutdown.
            if ( pVulkanCommandPool->m_commandPool != VK_NULL_HANDLE )
            {
                vkDestroyCommandPool( pVulkanContext->m_device, pVulkanCommandPool->m_commandPool, nullptr );
            }

            pVulkanContext->DestroyObject( eastl::move( pVulkanCommandPool ) );
            pCommandPool = nullptr;
        }
    }

    void ResetCommandPool( Context* pContext, CommandPool* pCommandPool )
    {
        VulkanContext* pVulkanContext = static_cast<VulkanContext*>( pContext );
        VulkanCommandPool* pVulkanCommandPool = static_cast<VulkanCommandPool*>( pCommandPool );

        // No RELEASE_RESOURCES flag. ID3D12CommandAllocator::Reset keeps the memory for reuse,
        // and this is called once per frame per pool, so handing the memory back to the driver
        // every frame would be the opposite of what the caller wants.
        VkResult const result = vkResetCommandPool( pVulkanContext->m_device, pVulkanCommandPool->m_commandPool, 0 );
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

        VkResult const result = vkAllocateCommandBuffers( pVulkanContext->m_device, &allocateInfo, &pVulkanCommandBuffer->m_commandBuffer );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandBuffer->m_device = pVulkanContext->m_device;
        pVulkanCommandBuffer->m_vkCmdPushDescriptorSet = pVulkanContext->m_vkCmdPushDescriptorSet;
        pVulkanCommandBuffer->m_pQueue = parameters.m_pCommandPool->m_pQueue;
        pVulkanCommandBuffer->m_pCommandPool = parameters.m_pCommandPool;
        pVulkanCommandBuffer->m_nodeIndex = parameters.m_pCommandPool->m_pQueue->m_nodeIndex;

        // Direct3D 12 creates a command list already recording and closes it straight away to
        // match every other API. Vulkan already starts in the initial state, so there is
        // nothing to undo here.
        pVulkanCommandBuffer->m_stage = VulkanCommandBuffer::Stage::Closed;

        // The root constant ring. Written by the CPU, read by the GPU, never given a descriptor
        // of its own: CmdSetRootConstants pushes a descriptor at an offset into it.
        BufferParameters ringParameters = {};
        ringParameters.m_bufferSize = g_rootConstantRingSize;
        ringParameters.m_memoryType = ResourceMemoryType::HostToDevice;
        ringParameters.m_flags.SetMultipleFlags( BufferFlags::NoDescriptors, BufferFlags::PersistentMap );
        ringParameters.m_descriptorTypes = DescriptorTypeFlags::ConstantBuffer;
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

            if ( pVulkanCommandBuffer->m_commandBuffer != VK_NULL_HANDLE )
            {
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
        // No ONE_TIME_SUBMIT. It would be faster and it is not safe to assume: Direct3D 12
        // allows a closed command list to be submitted more than once without re-recording, and
        // nothing here proves the engine never does. Set it only once that has been checked.
        beginInfo.flags = 0;

        VkResult const result = vkBeginCommandBuffer( pVulkanCommandBuffer->m_commandBuffer, &beginInfo );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandBuffer->m_stage = VulkanCommandBuffer::Stage::Recording;

        // Direct3D 12 calls SetDescriptorHeaps here, once per command buffer. The Vulkan
        // equivalent is binding the heap descriptor set, and the Phase 4 binding model puts
        // that in CmdSetPipeline instead, because set 1 is disturbed whenever a pipeline with a
        // different set 0 layout is bound. See the binding model entry in Docs/Linux/Progress.md.
        pVulkanCommandBuffer->m_pBoundRootSignature = nullptr;
        pVulkanCommandBuffer->m_pBoundPipeline = nullptr;
        pVulkanCommandBuffer->m_boundPipelineLayout = VK_NULL_HANDLE;
        pVulkanCommandBuffer->m_isRendering = false;
        pVulkanCommandBuffer->m_rootConstantRingOffset = 0;
    }

    void EndCommandBuffer( CommandBuffer* pCommandBuffer )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        EE_ASSERT( pVulkanCommandBuffer->m_stage == VulkanCommandBuffer::Stage::Recording );

        // Dynamic rendering has to be closed before the command buffer is.
        EndRenderingIfActive( pVulkanCommandBuffer );

        // Direct3D 12 flushes pending barriers here. P5.9 decides whether the Vulkan side
        // batches barriers the same way; if it does, the flush belongs at this line.
        VkResult const result = vkEndCommandBuffer( pVulkanCommandBuffer->m_commandBuffer );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanCommandBuffer->m_stage = VulkanCommandBuffer::Stage::Closed;
    }

    //-------------------------------------------------------------------------
    // Render pass and draw commands
    //-------------------------------------------------------------------------

    // P5.6 owns textures and will extend this. CmdSetRenderTargets needs the view, the extent
    // and the format out of one, so the type has to exist now.

    //-------------------------------------------------------------------------

    static void EndRenderingIfActive( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        if ( pVulkanCommandBuffer->m_isRendering )
        {
            vkCmdEndRendering( pVulkanCommandBuffer->m_commandBuffer );
            pVulkanCommandBuffer->m_isRendering = false;
        }
    }

    static VkAttachmentLoadOp VulkanLoadOp( LoadActionType action )
    {
        switch ( action )
        {
            case LoadActionType::Load:      return VK_ATTACHMENT_LOAD_OP_LOAD;
            case LoadActionType::Clear:     return VK_ATTACHMENT_LOAD_OP_CLEAR;
            case LoadActionType::DontCare:  return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }

        EE_UNREACHABLE_CODE();
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    static VkAttachmentStoreOp VulkanStoreOp( StoreActionType action )
    {
        switch ( action )
        {
            case StoreActionType::Store:    return VK_ATTACHMENT_STORE_OP_STORE;
            case StoreActionType::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // "None" means the attachment is untouched. VK_ATTACHMENT_STORE_OP_NONE says
            // exactly that and is core in 1.3.
            case StoreActionType::None:     return VK_ATTACHMENT_STORE_OP_NONE;
        }

        EE_UNREACHABLE_CODE();
        return VK_ATTACHMENT_STORE_OP_STORE;
    }

    void CmdSetRenderTargets( CommandBuffer* pCommandBuffer, TArrayView<Texture* const> renderTargets, Texture* pDepthStencil, LoadAction* pLoadAction, TArrayView<uint32_t const> colorArraySlices, TArrayView<uint32_t const> colorMipSlices, uint32_t depthArraySlice, uint32_t depthMipSlice )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // Direct3D 12's OMSetRenderTargets simply replaces what is bound. Dynamic rendering has
        // a begin and an end, so the previous one is closed here.
        EndRenderingIfActive( pVulkanCommandBuffer );

        if ( renderTargets.empty() && pDepthStencil == nullptr )
        {
            return;
        }

        // Load and store actions are what dynamic rendering is for. Direct3D 12 has no such
        // concept and clears with a separate ClearRenderTargetView call after binding; here the
        // clear is the load op, which is what the phase document's mapping asks for and what a
        // tiler needs.
        TInlineVector<VkRenderingAttachmentInfo, MaxRenderTargets> colorAttachments;
        VkExtent2D renderArea = {};

        for ( size_t renderTargetIndex = 0; renderTargetIndex < renderTargets.size(); ++renderTargetIndex )
        {
            VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( renderTargets[renderTargetIndex] );

            VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            attachment.imageView = pVulkanTexture->m_imageView;
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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

            colorAttachments.push_back( attachment );

            renderArea.width = Math::Max( renderArea.width, pVulkanTexture->m_extent.width );
            renderArea.height = Math::Max( renderArea.height, pVulkanTexture->m_extent.height );
        }

        VkRenderingAttachmentInfo depthAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        VkRenderingAttachmentInfo stencilAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        bool hasDepth = false;
        bool hasStencil = false;

        if ( pDepthStencil != nullptr )
        {
            VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( pDepthStencil );

            hasDepth = pVulkanTexture->m_format != VK_FORMAT_S8_UINT;
            hasStencil = ( pVulkanTexture->m_format == VK_FORMAT_S8_UINT ) ||
                         ( pVulkanTexture->m_format == VK_FORMAT_D16_UNORM_S8_UINT ) ||
                         ( pVulkanTexture->m_format == VK_FORMAT_D24_UNORM_S8_UINT ) ||
                         ( pVulkanTexture->m_format == VK_FORMAT_D32_SFLOAT_S8_UINT );

            depthAttachment.imageView = pVulkanTexture->m_imageView;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            stencilAttachment = depthAttachment;

            if ( pLoadAction != nullptr )
            {
                depthAttachment.loadOp = VulkanLoadOp( pLoadAction->m_loadActionDepth );
                depthAttachment.storeOp = VulkanStoreOp( pLoadAction->m_storeActionsDepth );
                depthAttachment.clearValue.depthStencil.depth = pLoadAction->m_depthClearValue.m_depth;

                stencilAttachment.loadOp = VulkanLoadOp( pLoadAction->m_loadActionStencil );
                stencilAttachment.storeOp = VulkanStoreOp( pLoadAction->m_storeActionStencil );
                stencilAttachment.clearValue.depthStencil.stencil = pLoadAction->m_depthClearValue.m_stencil;
            }

            renderArea.width = Math::Max( renderArea.width, pVulkanTexture->m_extent.width );
            renderArea.height = Math::Max( renderArea.height, pVulkanTexture->m_extent.height );
        }

        VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        // Direct3D 12 has no render area; it draws wherever the viewport and scissor allow. The
        // full extent of the attachments is the same thing, and the viewport still restricts it.
        renderingInfo.renderArea.extent = renderArea;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = uint32_t( colorAttachments.size() );
        renderingInfo.pColorAttachments = colorAttachments.data();
        renderingInfo.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
        renderingInfo.pStencilAttachment = hasStencil ? &stencilAttachment : nullptr;

        vkCmdBeginRendering( pVulkanCommandBuffer->m_commandBuffer, &renderingInfo );
        pVulkanCommandBuffer->m_isRendering = true;

        // colorArraySlices, colorMipSlices, depthArraySlice and depthMipSlice select a subresource
        // of the target. Direct3D 12 does it by picking a different render target view; Vulkan
        // does it with a different VkImageView. P5.6 creates per-subresource views on the
        // texture, so this is left asserting rather than silently rendering to mip 0 of slice 0.
        EE_ASSERT( colorArraySlices.empty() && colorMipSlices.empty() );
        EE_ASSERT( depthArraySlice == 0 && depthMipSlice == 0 );
    }

    void CmdSetShadingRate( CommandBuffer* pCommandBuffer, ShadingRate shadingRate, Texture* pShadingRateTexture, ShadingRateCombiner postRasterizerCombiner, ShadingRateCombiner finalCombiner )
    {
        // P5.15. It needs VK_KHR_fragment_shading_rate at device creation, and
        // FillDeviceCapabilities has to stop reporting ShadingRate::NotSupported before the
        // engine will ever call this.
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void CmdSetViewport( CommandBuffer* pCommandBuffer, float x, float y, float width, float height, float minDepth, float maxDepth )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // **This is where clip-space Y is inverted, and it happens exactly once.**
        //
        // Phase 4 decided it: the Vulkan viewport flips Y with a negative height, the shader
        // compiler does not, and -fvk-invert-y must never be added. See the clip-space Y entry
        // in Docs/Linux/Progress.md. Direct3D's clip space has +Y up and Vulkan's has +Y down,
        // so without this everything renders upside down.
        //
        // The origin moves to the bottom of the rectangle and the height goes negative, which
        // is the standard formulation and is what VK_KHR_maintenance1 made legal.
        //
        // Do not add a second flip anywhere. The other half of this decision is the front face
        // in CreatePipeline, which accounts for the winding this reverses.
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

        vkCmdBindPipeline( pVulkanCommandBuffer->m_commandBuffer, pVulkanPipeline->m_bindPoint, pVulkanPipeline->m_pipeline );

        pVulkanCommandBuffer->m_pBoundPipeline = pVulkanPipeline;
        pVulkanCommandBuffer->m_pBoundRootSignature = pVulkanRootSignature;
        pVulkanCommandBuffer->m_boundPipelineLayout = pVulkanRootSignature->m_pipelineLayout;
        pVulkanCommandBuffer->m_boundBindPoint = pVulkanPipeline->m_bindPoint;

        // **Heap set 1 is bound here, not in BeginCommandBuffer.**
        //
        // Direct3D 12 calls SetDescriptorHeaps once per command buffer, at :2917. Vulkan cannot
        // do that: binding a pipeline whose layout differs from set N onwards disturbs every set
        // from N up, set 0 varies per shader, so set 1 is disturbed on every pipeline-layout
        // change. The binding model chose this spot deliberately and accepted the redundant
        // rebind; one vkCmdBindDescriptorSets per pipeline change is a rounding error next to
        // the alternative, which was a shader-compiler flag list tracking every register
        // upstream ever writes. See the binding model entry in Docs/Linux/Progress.md.
        vkCmdBindDescriptorSets( pVulkanCommandBuffer->m_commandBuffer, pVulkanPipeline->m_bindPoint, pVulkanRootSignature->m_pipelineLayout,
                                 g_heapSet, 1, &pVulkanRootSignature->m_heapDescriptorSet, 0, nullptr );
    }

    void CmdSetRootConstants( CommandBuffer* pCommandBuffer, uint32_t constantIndex, void const* pConstantData, size_t constantSize )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanRootSignature* pVulkanRootSignature = static_cast<VulkanRootSignature*>( pVulkanCommandBuffer->m_pBoundRootSignature );

        DescriptorReflection const& descriptorReflection = pVulkanRootSignature->m_descriptorReflections[constantIndex];
        EE_ASSERT( descriptorReflection.m_descriptorTypeFlags == TBitFlags( DescriptorTypeFlags::RootConstant ) );
        EE_ASSERT( constantSize == sizeof( uint32_t ) * descriptorReflection.m_numConstants );

        if ( pConstantData == nullptr )
        {
            return;
        }

        // **Not Vulkan push constants, and this is the reason.**
        //
        // RHI.esh declares the block through EE_DECLARE_ROOT_CONSTANTS as
        // "ConstantBuffer<T> RootConstants : register( b0 )", so DXC emits a uniform buffer.
        // Turning it into a push constant block needs [[vk::push_constant]] in RHI.esh, which
        // Phase 4 rule 4 forbids. So the constants are copied into a per-command-buffer ring and
        // a descriptor is pushed at the copy, which is what the binding model recorded.
        VulkanBuffer* pRing = static_cast<VulkanBuffer*>( pVulkanCommandBuffer->m_pRootConstantRing );
        EE_ASSERT( pRing != nullptr && pRing->m_pMappedAddress_WriteCombined != nullptr );

        uint64_t const offset = pVulkanCommandBuffer->m_rootConstantRingOffset;
        // Asserted rather than wrapped. Wrapping would overwrite constants the GPU is still
        // reading and surface as a shader reading the wrong values, which is a miserable thing
        // to chase.
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
    }

    void CmdSetRootParameter( CommandBuffer* pCommandBuffer, uint32_t parameterIndex, Buffer* pBuffer, size_t bufferOffset )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
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
        // VK_WHOLE_SIZE, because a Direct3D 12 root descriptor is an address with no size and
        // this has to mean the same thing. The binding model says so explicitly.
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstBinding = pVulkanRootSignature->m_shaderResources[parameterIndex].m_registerIndex;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType;
        write.pBufferInfo = &bufferInfo;

        EE_ASSERT( pVulkanCommandBuffer->m_vkCmdPushDescriptorSet != nullptr );
        pVulkanCommandBuffer->m_vkCmdPushDescriptorSet( pVulkanCommandBuffer->m_commandBuffer, pVulkanCommandBuffer->m_boundBindPoint,
                                                        pVulkanCommandBuffer->m_boundPipelineLayout, g_rootParameterSet, 1, &write );
    }

    void CmdSetIndexBuffer( CommandBuffer* pCommandBuffer, Buffer const* pIndexBuffer, IndexType indexType, uint64_t offset )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanBuffer const* pVulkanBuffer = static_cast<VulkanBuffer const*>( pIndexBuffer );

        VkIndexType const vulkanIndexType = ( indexType == IndexType::Uint16 ) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        vkCmdBindIndexBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, offset, vulkanIndexType );
    }

    void CmdDraw( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t firstVertex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        vkCmdDraw( pVulkanCommandBuffer->m_commandBuffer, numVertices, 1, firstVertex, 0 );
    }

    void CmdDrawInstanced( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t numInstances, uint32_t firstVertex, uint32_t firstInstance )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        vkCmdDraw( pVulkanCommandBuffer->m_commandBuffer, numVertices, numInstances, firstVertex, firstInstance );
    }

    void CmdDrawIndexed( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t firstIndex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        vkCmdDrawIndexed( pVulkanCommandBuffer->m_commandBuffer, numIndices, 1, firstIndex, 0, 0 );
    }

    void CmdDrawIndexedInstanced( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t numInstances, uint32_t firstIndex, uint32_t firstInstance )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        vkCmdDrawIndexed( pVulkanCommandBuffer->m_commandBuffer, numIndices, numInstances, firstIndex, 0, firstInstance );
    }

    void CmdDispatchCompute( CommandBuffer* pCommandBuffer, uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // A dispatch may not run inside a render pass. Direct3D 12 has no such rule, so the
        // engine does not close anything before dispatching. P5.9 and P5.10 need this same call
        // before a barrier or a copy.
        EndRenderingIfActive( pVulkanCommandBuffer );

        EE_ASSERT( numGroupsX <= MaxDispatchSize && numGroupsY <= MaxDispatchSize && numGroupsZ <= MaxDispatchSize );

        vkCmdDispatch( pVulkanCommandBuffer->m_commandBuffer, numGroupsX, numGroupsY, numGroupsZ );
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

    //-------------------------------------------------------------------------
    // Buffers
    //-------------------------------------------------------------------------


    //-------------------------------------------------------------------------

    // The one DataFormat to VkFormat mapping. **P5.6 completes this function; it must never
    // write a second one.** The phase document warns that two mappings which disagree corrupt
    // textures in a way that looks like a bug somewhere else, so the entries buffers need are
    // filled in here and everything else asserts rather than guessing.
    //
    // Only three formats reach a buffer today, all typed texel buffers: R32_UInt, RG32_UInt and
    // R32_SFloat. Measured by reading every BufferParameters::m_format assignment in
    // Code/Engine, not assumed.
    static VkFormat VulkanFormat( DataFormat format )
    {
        switch ( format )
        {
            case DataFormat::Undefined:     return VK_FORMAT_UNDEFINED;

            case DataFormat::R32_UInt:      return VK_FORMAT_R32_UINT;
            case DataFormat::R32_SInt:      return VK_FORMAT_R32_SINT;
            case DataFormat::R32_SFloat:    return VK_FORMAT_R32_SFLOAT;
            case DataFormat::RG32_UInt:     return VK_FORMAT_R32G32_UINT;
            case DataFormat::RG32_SInt:     return VK_FORMAT_R32G32_SINT;
            case DataFormat::RG32_SFloat:   return VK_FORMAT_R32G32_SFLOAT;

            // Render target and depth formats, added by P5.7. Measured the same way: every
            // DataFormat a texture or a pipeline is created with in Code/Engine.
            case DataFormat::R8_UNorm:      return VK_FORMAT_R8_UNORM;
            case DataFormat::R16_SFloat:    return VK_FORMAT_R16_SFLOAT;
            case DataFormat::RG16_SFloat:   return VK_FORMAT_R16G16_SFLOAT;
            case DataFormat::RGBA8_UNorm:   return VK_FORMAT_R8G8B8A8_UNORM;
            case DataFormat::RGBA8_sRGB:    return VK_FORMAT_R8G8B8A8_SRGB;
            case DataFormat::RGBA16_SFloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case DataFormat::RGBA32_SFloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case DataFormat::RG11_B10_UFloat: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case DataFormat::D32_SFloat:    return VK_FORMAT_D32_SFLOAT;
            case DataFormat::S8_Uint:       return VK_FORMAT_S8_UINT;

            default:
            {
                // P5.6 fills in the remaining ~109 entries, in the same task as the texture
                // work that needs them.
                EE_UNIMPLEMENTED_FUNCTION();
                return VK_FORMAT_UNDEFINED;
            }
        }
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

    // Writes one slot of the resource heap. Writing a mutable descriptor uses the *actual*
    // type in VkWriteDescriptorSet, never VK_DESCRIPTOR_TYPE_MUTABLE_EXT, which only ever
    // appears in the layout.
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
        // Direct3D 12 needs almost none of this: a buffer is a buffer and the view decides what
        // it is. Vulkan wants the usage up front, so it is derived from the descriptor types
        // the caller asked for, plus the transfer bits, which every buffer here can need.

        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::ConstantBuffer ) )         { usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; }
        if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::IndexBuffer ) )            { usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; }
        if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::IndirectArgumentBuffer ) ) { usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; }

        bool const isTypedBuffer = parameters.m_format != DataFormat::Undefined && !descriptorTypes.IsFlagSet( DescriptorTypeFlags::Raw );

        if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::Buffer ) )
        {
            usage |= isTypedBuffer ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }

        if ( descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWBuffer ) )
        {
            usage |= isTypedBuffer ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }

        // bufferDeviceAddress is a device feature CreateContext requires, and Buffer holds an
        // m_deviceAddress the engine reads, so every buffer carries the bit.
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferCreateInfo.size = allocationSize;
        bufferCreateInfo.usage = usage;
        // Direct3D 12 buffers have no queue ownership at all. CONCURRENT would let any queue
        // touch this without an ownership transfer and reproduce that, at a cost on some
        // hardware; EXCLUSIVE is the faster and stricter choice. P5.9 owns barriers, so the
        // ownership transfers belong there. This is EXCLUSIVE because most buffers never move
        // between queues, and P5.9 has to get the transfers right regardless.
        bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

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
        VkResult result = vmaCreateBuffer( pVulkanContext->m_resourceAllocator, &bufferCreateInfo, &allocationCreateInfo, &pVulkanBuffer->m_buffer, &pVulkanBuffer->m_allocation, &allocationInfo );
        EE_ASSERT( result == VK_SUCCESS );

        pVulkanBuffer->m_allocationSize = allocationInfo.size;
        TrackResourceAllocation( pVulkanContext, descriptorTypes, false, true, allocationInfo.size );

        VkBufferDeviceAddressInfo deviceAddressInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        deviceAddressInfo.buffer = pVulkanBuffer->m_buffer;
        pVulkanBuffer->m_deviceAddress = vkGetBufferDeviceAddress( pVulkanContext->m_device, &deviceAddressInfo );
        EE_ASSERT( pVulkanBuffer->m_deviceAddress != 0 );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_BUFFER, uint64_t( pVulkanBuffer->m_buffer ), parameters.m_debugName );

        // VMA_ALLOCATION_CREATE_MAPPED_BIT already mapped it, so there is no second Map call
        // the way Direct3D 12 needs one.
        if ( parameters.m_flags.IsFlagSet( BufferFlags::PersistentMap ) && parameters.m_memoryType != ResourceMemoryType::DeviceLocal )
        {
            pVulkanBuffer->m_pMappedAddress_WriteCombined = allocationInfo.pMappedData;
            EE_ASSERT( pVulkanBuffer->m_pMappedAddress_WriteCombined != nullptr );
        }

        // Descriptors
        //-------------------------------------------------------------------------
        // One contiguous run in the resource heap, in the same order Direct3D 12 uses, because
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
                    // element offset that Direct3D 12 puts in the SRV becomes the descriptor's
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

                // BufferParameters::m_pCounterBuffer has nowhere to go. Direct3D 12 hands the
                // counter resource to CreateUnorderedAccessView and Vulkan has no such thing.
                // The engine does not need it: AppendBuffer.esh carries its own explicit
                // RWBuffer<uint> counter and does its own InterlockedAdd, and no .esh or .esf
                // in the repository uses IncrementCounter, DecrementCounter,
                // AppendStructuredBuffer or ConsumeStructuredBuffer. See the binding model
                // entry in Docs/Linux/Progress.md, which reaches the same conclusion from the
                // shader side and reserves set 1 binding 2 for a counter heap that is never
                // created.
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
                // either way. Direct3D 12 frees its descriptors the same way, without writing
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

        // vmaMapMemory maps the whole allocation; Direct3D 12's Map takes a read range as a
        // hint about what the CPU will touch. The offset is applied to the pointer so the
        // caller sees the same address either way.
        void* pMappedAddress = nullptr;
        VkResult const result = vmaMapMemory( pVulkanContext->m_resourceAllocator, pVulkanBuffer->m_allocation, &pMappedAddress );
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
    // SPIRV-Reflect replaces ID3D12ShaderReflection, which RHI_Direct3D12.cpp uses in
    // ExtractReflection at :1003. The output has to be the same ShaderReflection the engine
    // already reads, so this mirrors that function rather than exposing anything new.

    static bool IsRootConstant( char const* pName )
    {
        // Matches RHI_Direct3D12.cpp:990. RHI.esh declares the block as
        // "ConstantBuffer<T> RootConstants : register( ... )", so the name is the marker.
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
        // resource_type carries the read against read-write distinction that Direct3D 12 gets
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
        SpvReflectResult const createResult = spvReflectCreateShaderModule( spirv.size(), spirv.data(), &module );
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
            // Set 1 is the bindless heap. Its layout is fixed by the Phase 4 binding model and
            // shared by every pipeline, so it is not a root parameter and must not become one.
            if ( pBinding->set == g_heapSet )
            {
                continue;
            }

            ShaderResource shaderResource = {};
            shaderResource.m_setIndex = pBinding->set;
            // The **Vulkan** binding, not the HLSL register it came from. The binding model
            // shifts b/t/u/s registers to 0/8/16/24, and un-shifting here only to re-shift in
            // CreateRootSignature would be two chances to get it wrong. Nothing outside this
            // file reads m_registerIndex: EngineShader.cpp only reads m_descriptorTypeFlags and
            // m_numConstants, and uses position in m_descriptorReflections as the parameter
            // index.
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

            // Constant buffer members, which CreateRootSignature adds up to size the root
            // constants. Direct3D 12 reads these through ID3D12ShaderReflectionConstantBuffer.
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

            // ShaderByteCode carries base85-encoded, compressed SPIR-V, exactly as it carried
            // DXIL on Windows. Embed does the decode; the Reflector wrote it.
            Blob byteCode = Embed::DecompressEmbeddedFile( shaderParameters[shaderIndex].m_pCompressedData, shaderParameters[shaderIndex].m_decodedSize, shaderParameters[shaderIndex].m_decompressedSize );

            EE_ASSERT( !byteCode.empty() );
            EE_ASSERT( ( byteCode.size() % sizeof( uint32_t ) ) == 0 ); // SPIR-V is a stream of 32 bit words

            VkShaderModuleCreateInfo moduleCreateInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            moduleCreateInfo.codeSize = byteCode.size();
            moduleCreateInfo.pCode = reinterpret_cast<uint32_t const*>( byteCode.data() );

            VkResult const result = vkCreateShaderModule( pVulkanContext->m_device, &moduleCreateInfo, nullptr, &pVulkanShader->m_shaderModules[shaderIndex] );
            EE_ASSERT( result == VK_SUCCESS );

            pVulkanShader->m_stageReflections[shaderIndex] = ExtractReflection( TArrayView<uint8_t const>( byteCode.data(), byteCode.size() ), stage );

            // DXC names every entry point "main" in SPIR-V unless told otherwise, and the
            // Reflector does not tell it otherwise. Read it back rather than assume, because a
            // wrong entry point name fails at pipeline creation with an unhelpful message.
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
                case ShaderStage::RayTracing: break; // P5.16
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

        // Merge the per-stage reflections into one resource list, first seen wins, exactly as
        // RHI_Direct3D12.cpp:4946 does. The order decides m_parameterIndex, which
        // CmdSetRootParameter and EngineShader.cpp both index by position.
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
                // Not Vulkan push constants. RHI.esh declares the block through
                // EE_DECLARE_ROOT_CONSTANTS as a ConstantBuffer, so DXC emits a uniform buffer,
                // and making it a push constant block needs [[vk::push_constant]] in RHI.esh,
                // which Phase 4 rule 4 forbids. CmdSetRootConstants copies into a per-frame
                // upload ring and pushes a descriptor at it. See the binding model entry.
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

        // Static samplers find no matching shader resource in any current shader, which is what
        // the binding model recorded, so there are no Vulkan immutable samplers here either.
        // The warning matches the Direct3D 12 one, so a shader that starts using one is noticed.
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

            EE_UNIMPLEMENTED_FUNCTION(); // A shader started using a static sampler. See the binding model entry.
        }

        // Set 0, the root parameters. PUSH_DESCRIPTOR_BIT, so CmdSetRootParameter is a
        // vkCmdPushDescriptorSetKHR rather than a set allocated per draw.
        VkDescriptorSetLayoutCreateInfo rootParameterLayoutCreateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        rootParameterLayoutCreateInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        rootParameterLayoutCreateInfo.bindingCount = uint32_t( rootParameterBindings.size() );
        rootParameterLayoutCreateInfo.pBindings = rootParameterBindings.data();

        VkResult result = vkCreateDescriptorSetLayout( pVulkanContext->m_device, &rootParameterLayoutCreateInfo, nullptr, &pVulkanRootSignature->m_rootParameterSetLayout );
        EE_ASSERT( result == VK_SUCCESS );

        // Both sets, in order. Set 1 is the same layout for every pipeline in the engine.
        VkDescriptorSetLayout const setLayouts[2] = { pVulkanRootSignature->m_rootParameterSetLayout, pVulkanContext->m_heapSetLayout };

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutCreateInfo.setLayoutCount = 2;
        pipelineLayoutCreateInfo.pSetLayouts = setLayouts;

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

        VkResult const result = vkCreatePipelineCache( pVulkanContext->m_device, &cacheCreateInfo, nullptr, &pVulkanPipelineCache->m_pipelineCache );
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
        VkResult result = vkGetPipelineCacheData( pVulkanContext->m_device, pVulkanPipelineCache->m_pipelineCache, &dataSize, nullptr );
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

    Pipeline* CreatePipeline( Context* pContext, GraphicsPipelineParameters const& parameters )
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

        ShaderStage const wantedStages[2] = { ShaderStage::Vertex, ShaderStage::Pixel };
        TInlineVector<VkPipelineShaderStageCreateInfo, 3> stages;
        BuildGraphicsPipelineStages( pVulkanShader, stages, TArrayView<ShaderStage const>( wantedStages, 2 ) );

        // No vertex input state. GraphicsPipelineParameters carries no input layout, because the
        // engine pulls vertices out of buffers in the shader rather than binding them. An empty
        // VkPipelineVertexInputStateCreateInfo says exactly that.
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

        // **Winding, and the Y flip.** This is the classic porting bug and it is reasoned, not
        // verified.
        //
        // The Direct3D 12 backend sets FrontCounterClockwise = ( m_frontFace == ClockWise ),
        // which is already an inversion of the name. The Vulkan viewport inverts Y with a
        // negative height, which Phase 4 recorded and P5.8 applies, and that reverses triangle
        // winding in framebuffer space. Inverting the inversion lands back on the name, so
        // ClockWise means VK_FRONT_FACE_CLOCKWISE here.
        //
        // If back faces turn out inside out, this line and the sign of the viewport height in
        // CmdSetViewport are the only two places that can be responsible.
        rasterizationState.frontFace = ( parameters.m_rasterizerState.m_frontFace == FrontFace::ClockWise ) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // Direct3D 12's DepthClipEnable is the inverse of Vulkan's depthClampEnable, and the two
        // are not identical: clipping discards the primitive, clamping keeps it at the near or
        // far plane. VK_EXT_depth_clip_enable gives the exact control and is not enabled.
        // Nothing in the engine sets m_depthClip today.
        rasterizationState.depthClampEnable = parameters.m_rasterizerState.m_depthClip ? VK_FALSE : VK_TRUE;

        rasterizationState.depthBiasEnable = ( parameters.m_rasterizerState.m_depthBias != 0 ) || ( parameters.m_rasterizerState.m_slopeScaledDepthBias != 0.0f );
        rasterizationState.depthBiasConstantFactor = float( parameters.m_rasterizerState.m_depthBias );
        rasterizationState.depthBiasClamp = parameters.m_rasterizerState.m_depthBiasClamp;
        rasterizationState.depthBiasSlopeFactor = parameters.m_rasterizerState.m_slopeScaledDepthBias;

        VkPipelineMultisampleStateCreateInfo multisampleState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisampleState.rasterizationSamples = VkSampleCountFlagBits( parameters.m_numSamples );
        multisampleState.alphaToCoverageEnable = parameters.m_blendState.m_alphaToCoverage;
        // m_sampleQuality has no Vulkan equivalent. It is a Direct3D quality level for a given
        // sample count, and Vulkan exposes only the count.

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

            // Direct3D 12 leaves a target outside the mask entirely default, which is blending
            // off and no colour written. The same shape is reproduced here rather than
            // defaulting to a full write mask.
            if ( parameters.m_blendState.m_renderTargetMask.IsFlagSet( BlendStateTargetFlags( renderTargetIndex ) ) )
            {
                attachment.blendEnable = parameters.m_blendState.m_blendEnabled;
                attachment.srcColorBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_srcFactors[renderTargetIndex] );
                attachment.dstColorBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_dstFactors[renderTargetIndex] );
                attachment.colorBlendOp = VulkanBlendOp( parameters.m_blendState.m_blendModes[renderTargetIndex] );
                attachment.srcAlphaBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_srcAlphaFactors[renderTargetIndex] );
                attachment.dstAlphaBlendFactor = VulkanBlendFactor( parameters.m_blendState.m_dstAlphaFactors[renderTargetIndex] );
                attachment.alphaBlendOp = VulkanBlendOp( parameters.m_blendState.m_blendModesAlpha[renderTargetIndex] );
                // The write mask bits are the same order and values as Direct3D 12's, so the
                // engine's 0x0F default is RGBA on both.
                attachment.colorWriteMask = VkColorComponentFlags( parameters.m_blendState.m_writeMasks[renderTargetIndex] );
            }

            colorBlendAttachments.push_back( attachment );
        }

        VkPipelineColorBlendStateCreateInfo colorBlendState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlendState.attachmentCount = uint32_t( colorBlendAttachments.size() );
        colorBlendState.pAttachments = colorBlendAttachments.data();
        // m_independentBlend has no Vulkan switch. Vulkan is always independent per attachment
        // when independentBlend is supported, and writes the same state to every attachment
        // otherwise. The engine fills every attachment either way, so the flag has no effect.

        VkDynamicState const dynamicStates[3] =
        {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };

        VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = 3;
        dynamicState.pDynamicStates = dynamicStates;

        // Dynamic rendering, so there is no VkRenderPass and no framebuffer. The formats the
        // pipeline will be used with are declared here instead.
        TInlineVector<VkFormat, MaxRenderTargets> colorFormats;
        for ( DataFormat colorFormat : parameters.m_colorFormats )
        {
            colorFormats.push_back( VulkanFormat( colorFormat ) );
        }

        VkPipelineRenderingCreateInfo renderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        renderingCreateInfo.colorAttachmentCount = uint32_t( colorFormats.size() );
        renderingCreateInfo.pColorAttachmentFormats = colorFormats.data();

        // Direct3D 12 has one DSVFormat covering both aspects; Vulkan splits them, so a
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
        pipelineCreateInfo.pVertexInputState = &vertexInputState;
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pRasterizationState = &rasterizationState;
        pipelineCreateInfo.pMultisampleState = &multisampleState;
        pipelineCreateInfo.pDepthStencilState = &depthStencilState;
        pipelineCreateInfo.pColorBlendState = &colorBlendState;
        pipelineCreateInfo.pDynamicState = &dynamicState;
        pipelineCreateInfo.layout = pVulkanRootSignature->m_pipelineLayout;

        VkPipelineCache const cache = ( pVulkanPipelineCache != nullptr ) ? pVulkanPipelineCache->m_pipelineCache : VK_NULL_HANDLE;

        VkResult const result = vkCreateGraphicsPipelines( pVulkanContext->m_device, cache, 1, &pipelineCreateInfo, nullptr, &pVulkanPipeline->m_pipeline );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_PIPELINE, uint64_t( pVulkanPipeline->m_pipeline ), parameters.m_debugName );

        return pVulkanPipeline;
    }

    Pipeline* CreatePipeline( Context* pContext, MeshPipelineParameters const& parameters )
    {
        // P5.14. It is a small delta on the graphics path above: the same state, with task and
        // mesh stages instead of vertex, and no vertex input state at all.
        //
        // It needs two things this backend does not have yet, both at device creation time in
        // CreateContext: the VK_EXT_mesh_shader extension, and
        // VkPhysicalDeviceMeshShaderFeaturesEXT with meshShader and taskShader enabled. Adding
        // them there is the first step of P5.14.
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
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

        VkResult const result = vkCreateComputePipelines( pVulkanContext->m_device, cache, 1, &pipelineCreateInfo, nullptr, &pVulkanPipeline->m_pipeline );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_PIPELINE, uint64_t( pVulkanPipeline->m_pipeline ), parameters.m_debugName );

        return pVulkanPipeline;
    }

    Pipeline* CreatePipeline( Context* pContext, RaytracingPipelineParameters const& parameters )
    {
        // P5.16. It needs VK_KHR_ray_tracing_pipeline, VK_KHR_acceleration_structure and
        // VK_KHR_deferred_host_operations at device creation, and it also has to settle the one
        // question the binding model left open: whether
        // VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR may appear in a mutable descriptor type
        // list, since RHI.esh reads an acceleration structure straight out of the heap.
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
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
        // Runs after DestroyContext; see BaseModule::ShutdownModule. Vulkan exposes no global
        // live-object report, so this reads what DestroyContext recorded from VMA while the
        // allocator still existed. The validation layers report leaked Vulkan handles
        // separately, through the debug messenger.
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
