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

    // Defined in the draw commands section, which sits after EndCommandBuffer. Dynamic
    // rendering has a begin and an end, and several things have to close it.
    struct VulkanCommandBuffer;
    static void FlushRendering( VulkanCommandBuffer* pVulkanCommandBuffer );
    static void SuspendRendering( VulkanCommandBuffer* pVulkanCommandBuffer );
    static void FlushBarriers( VulkanCommandBuffer* pVulkanCommandBuffer );

    // The one DataFormat to VkFormat mapping, defined in the resources section below next to
    // its first caller. FillDeviceCapabilities is far above it and asks the device about every
    // format, so it needs the declaration here.
    static VkFormat VulkanFormat( DataFormat format );

    // Defined in the state mapping section, which sits after the samplers that need it.
    static VkCompareOp VulkanCompareOp( CompareMode mode );

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

        // The distinct queue families in use, which is what a CONCURRENT resource has to list.
        // See SetSharingMode, next to the buffers and textures that read it.
        TInlineVector<uint32_t, 3>                                      m_sharingQueueFamilies;

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
        // Every type in the heap's mutable list has to support update-after-bind, and the list
        // holds a storage texel buffer for RWBuffer<T>. Missed by P5.5; see Docs/Linux/Progress.md.
        if ( !available.m_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind ) { return "descriptorBindingStorageTexelBufferUpdateAfterBind"; }
        // FilterMode::Min and FilterMode::Max on a sampler. RenderSystem::Initialize creates
        // COMMON_SAMPLER_LINEAR_CLAMP_MAX, so this is used on the first frame.
        if ( !available.m_vulkan12.samplerFilterMinmax )            { return "samplerFilterMinmax"; }
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

        // The distinct families, for SetSharingMode. One entry means the device has a single
        // family and nothing is ever shared across one.
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

        // What the device can do with each DataFormat, asked one format at a time. Mirrors the
        // loop in RHI_Direct3D12.cpp:2205, including which question each array answers:
        //
        //   D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE   -> VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        //   D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE -> VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
        //   D3D12_FORMAT_SUPPORT1_RENDER_TARGET   -> VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
        //
        // m_canRenderTargetWriteTo is colour only on both backends. Direct3D 12 reports depth
        // through a separate D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL bit that the reference does not
        // read, so a depth format reads false here exactly as it does there.
        //
        // Optimal tiling, because CreateTexture creates every image with VK_IMAGE_TILING_OPTIMAL.
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
                // Not fatal, and worth saying out loud: Phase 5 asks for validation to stay on
                // throughout, and a missing layer package is the usual reason it silently is not.
                EE_LOG_WARNING( LogCategory::Render, "RHI/CreateContext", "VK_LAYER_KHRONOS_validation is not installed, so validation is off. Install vulkan-validationlayers." );
            }
        }
        else
        {
            // Object names and command buffer markers are worth having whenever the extension
            // is present, with or without the validation layer. P5.12 uses them.
            if ( HasExtension( availableInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) )
            {
                instanceExtensions.emplace_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
            }
        }

        // Surface extensions, for P5.3
        //-------------------------------------------------------------------------
        // **The instance has to carry these even though no window exists yet.** A VkSurfaceKHR
        // may only be created from an instance that enabled its platform extension, the instance
        // is created once, and the window arrives in Phase 6. So they go on now.
        //
        // Named by string rather than by macro on purpose. VK_KHR_XLIB_SURFACE_EXTENSION_NAME
        // only exists once VK_USE_PLATFORM_XLIB_KHR is defined, which drags X11 headers into a
        // file that has no other reason to see them. The strings are frozen by the specification.
        //
        // Every platform the loader reports is enabled, because which one the window system
        // turns out to be is Phase 6's answer, not this file's.
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
        enabledFeatures.m_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
        enabledFeatures.m_vulkan12.samplerFilterMinmax = VK_TRUE;
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

        // Dynamic rendering, deferred.
        //
        // **CmdSetRenderTargets does not begin the pass; the first draw does.** The engine
        // records image layout barriers *between* the two - RenderPass_SMAA.cpp:154 and
        // RenderPass_GTAO.cpp:435 both do, and both assert that no barrier is pending at the
        // CmdSetRenderTargets itself - and a barrier may not run inside dynamic rendering.
        // Direct3D 12 has the same shape for its own reason: OMSetRenderTargets is state, and
        // its batched barriers flush at the draw.
        //
        // m_isRendering means a pass is open now. m_needsRenderingBegin means one is configured
        // and not yet open. The attachment configuration below outlives both, so a pass that has
        // to be left for a barrier can be resumed by the next draw.
        bool                                                m_isRendering = false;
        bool                                                m_needsRenderingBegin = false;

        VkRenderingInfo                                     m_renderingInfo = {};
        TInlineVector<VkRenderingAttachmentInfo, MaxRenderTargets> m_colorAttachments;
        VkRenderingAttachmentInfo                           m_depthAttachment = {};
        VkRenderingAttachmentInfo                           m_stencilAttachment = {};
        bool                                                m_hasDepthAttachment = false;
        bool                                                m_hasStencilAttachment = false;

        // Barriers are batched exactly as Direct3D 12 batches them at
        // RHI_Direct3D12.cpp:1516, and flushed at the same points. Vulkan gains a second reason
        // to batch: one vkCmdPipelineBarrier2 for the whole set lets the driver see them
        // together, and the flush is the single place that has to leave the render pass.
        TVector<VkMemoryBarrier2>                           m_globalBarriers{ Memory::Allocators::g_RHI };
        TVector<VkBufferMemoryBarrier2>                     m_bufferBarriers{ Memory::Allocators::g_RHI };
        TVector<VkImageMemoryBarrier2>                      m_imageBarriers{ Memory::Allocators::g_RHI };

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
        VmaAllocation                                       m_allocation = VK_NULL_HANDLE;
        uint64_t                                            m_allocationSize = 0;

        // Named m_vulkanFormat rather than m_format on purpose. Texture::m_format is the
        // DataFormat the caller asked for, and a member of the same name here would hide it
        // behind a different type depending on which pointer the reader has.
        VkFormat                                            m_vulkanFormat = VK_FORMAT_UNDEFINED;
        VkExtent3D                                          m_extent = {};

        // Every aspect the image has: colour, or depth and stencil. A view picks a subset.
        VkImageAspectFlags                                  m_aspectMask = 0;

        // False when the image belongs to somebody else: a swapchain image handed in through
        // TextureParameters::m_pNativeHandle, or the texture this one aliases. The views are
        // still ours, the image is not.
        bool                                                m_ownsImage = true;

        // **A Vulkan image is always created in VK_IMAGE_LAYOUT_UNDEFINED**, whatever
        // TextureParameters::m_initialState says, because those are the only two layouts
        // vkCreateImage accepts and the other one is for linear tiling. Direct3D 12 takes the
        // initial layout directly. So the engine believes this texture is already in
        // m_initialState and the image is not, and P5.9 has to transition from what is recorded
        // here rather than from the state the caller passes to the first barrier.
        VkImageLayout                                       m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // The layout the sampled-image descriptor in the heap was written with, and therefore
        // the layout P5.9 has to put this texture in before a shader reads it. GENERAL for a
        // texture that is also an RWTexture, because one image cannot be in two layouts and a
        // storage image descriptor has to say GENERAL.
        VkImageLayout                                       m_shaderReadLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // The view a shader samples, covering every mip and layer. One per texture.
        VkImageView                                         m_shaderResourceView = VK_NULL_HANDLE;

        // One storage view per mip level, because an RWTexture handle names a mip level.
        TVector<VkImageView>                                m_storageViews{ Memory::Allocators::g_RHI };

        // One attachment view per subresource, indexed the way Direct3D 12 indexes its render
        // target descriptors at RHI_Direct3D12.cpp:1387: m_mipLevels * arrayLayer + mipLevel.
        TVector<VkImageView>                                m_renderTargetViews{ Memory::Allocators::g_RHI };

        // A contiguous run in the resource heap, in the same order Direct3D 12 uses: the read
        // view first if present, then one read-write view per mip level.
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

    // **A Direct3D 12 queue executes its command lists in submission order, and a Vulkan queue
    // does not.** Two `vkQueueSubmit2` calls on one `VkQueue` may overlap unless something
    // orders them, and nothing in `RHI.h` can say so: `QueueDeviceWait` asserts that the two
    // queues differ, so the engine has no way to make a queue wait on itself. It does not need
    // one on Direct3D, and it relies on that - `ForwardShadingRenderer::SubmitGraphicsCommandBuffer`
    // submits several graphics command buffers a frame with the barriers recorded across them.
    //
    // So every submit waits on the value the previous submit on that queue signalled. That is
    // the Direct3D semantics exactly, and it is what makes the swapchain sound as well: the
    // acquire wait that the first submit after `AcquireNextImage` carries then holds back every
    // later submit too, including the one that writes the swapchain image.
    //
    // ALL_COMMANDS on both ends, because "the previous submit finished" is the whole meaning.
    // Recorded as an ALL_COMMANDS site in Docs/Linux/Progress.md.
    static void RecordQueueOrderingWait( VulkanQueue* pVulkanQueue )
    {
        uint64_t const previousValue = pVulkanQueue->m_nextSemaphoreValue - 1;

        // Zero is the timeline's initial value and is always satisfied, so the first submit on a
        // queue has nothing to wait for.
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

    // The body of QueueSubmit, and of the submit QueuePresent has to make before it can present.
    // The only difference is the binary semaphore: VkPresentInfoKHR cannot wait on a timeline, so
    // a present needs one signalled next to the timeline value. VK_NULL_HANDLE for an ordinary
    // submit.
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

        // Direct3D 12 skips ExecuteCommandLists on an empty list but still signals. Signalling
        // with no work is exactly what an empty submit does here, so the shape is the same.
        VkResult const result = vkQueueSubmit2( pVulkanQueue->m_queue, 1, &submitInfo, VK_NULL_HANDLE );
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
    // **SwapchainParameters::m_pNativeWindowHandle is a VkSurfaceKHR on Linux, and the
    // application owns it.** Direct3D 12 receives an HWND and asks DXGI for a swapchain. Vulkan
    // needs a VkSurfaceKHR, and creating one needs a window system library. Base/Render depends
    // on no such library and must not start to, so the application creates the surface from the
    // instance and hands it over. SDL3's SDL_Vulkan_CreateSurface returns exactly this, which is
    // the answer Phase 5 owes Phase 6. CreateContext enables the surface instance extensions so
    // that call can succeed; DestroySwapchain never destroys the surface.
    //
    // **A null handle means headless**, which is the state of the whole of Phase 5: there is no
    // window until Phase 6. The swapchain is then a ring of ordinary offscreen render targets
    // with no VkSwapchainKHR, AcquireNextImage cycles the index, and QueuePresent signals its
    // timeline value and presents nothing. That is the phase document's bring-up order - render
    // offscreen first, wire the real surface later - and it is what lets steps 6 and 7 of the
    // ladder run the moment there is any entry point to run them from.
    //
    // **The application drives swapchain recreation, not the RHI.** Engine.cpp:754 and
    // ImguiRenderer.cpp:91 both compare the window size against GetSwapchainSize() and call
    // Window::ResizeSwapchain, and each one waits the graphics queue idle first. So this file
    // tolerates VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR rather than recreating behind the
    // engine's back. That is the second answer Phase 5 owes Phase 6.

    // Declared here rather than with the swapchain functions below, because QueuePresent reads
    // it and is defined first.
    struct VulkanSwapchain final : Swapchain
    {
        VkDevice                                            m_device = VK_NULL_HANDLE;

        // Borrowed, never destroyed. The application created it and Window::ResizeSwapchain
        // hands the same one back after a DestroySwapchain.
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

        // **Binary semaphores, because VkPresentInfoKHR has no timeline path.** This is what
        // P5.2 left to this group. The acquire semaphores are a ring rather than one per image,
        // because vkAcquireNextImageKHR is told which semaphore to signal before it says which
        // image it gave. The present semaphores are one per image, which is safe because an
        // image is not presented again until it has been acquired again.
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
            // Nothing to present to. Direct3D 12 signals its fence after the present and returns
            // that value, and so does this; the frame pacing the engine builds on the returned
            // value is unchanged.
            return SubmitToQueue( pVulkanQueue, {}, VK_NULL_HANDLE );
        }

        VkSemaphore const presentSemaphore = pVulkanSwapchain->m_presentSemaphores[imageIndex];

        // The submit comes first here where Direct3D 12 presents first and signals after, and it
        // has to: vkQueuePresentKHR waits on a semaphore that only a submit can signal. The
        // returned value still means "the frame is done", which is all the engine reads it for.
        uint64_t const signalSemaphore = SubmitToQueue( pVulkanQueue, {}, presentSemaphore );

        VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &presentSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &pVulkanSwapchain->m_swapchain;
        presentInfo.pImageIndices = &imageIndex;

        VkResult const result = vkQueuePresentKHR( pVulkanQueue->m_queue, &presentInfo );

        // Neither of these is an error here. The engine resizes the swapchain itself, from the
        // window size, so a stale swapchain is already on its way to being replaced.
        EE_ASSERT( result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR );

        return signalSemaphore;
    }

    void WaitQueueIdle( Queue* pQueue )
    {
        VulkanQueue* pVulkanQueue = static_cast<VulkanQueue*>( pQueue );

        // Direct3D 12 signals its fence and blocks on an event, because it has no queue-idle
        // call. Vulkan has one, and it means precisely this.
        VkResult const result = vkQueueWaitIdle( pVulkanQueue->m_queue );
        EE_ASSERT( result == VK_SUCCESS );
    }

    // The render targets, which are the same textures on both paths. On the real path they wrap
    // images the presentation engine owns, which is what TextureParameters::m_pNativeHandle is
    // for; headless they are ordinary textures this call allocates. Mirrors the parameters
    // RHI_Direct3D12.cpp:2736 fills in.
    static void CreateSwapchainRenderTargets( Context* pContext, VulkanSwapchain* pVulkanSwapchain, SwapchainParameters const& parameters, DataFormat renderTargetFormat, VkExtent2D extent, TArrayView<VkImage const> images )
    {
        TextureParameters textureParameters = {};
        // The surface's extent, not the one that was asked for. A window system that has already
        // decided the size - which is the usual case on Wayland - gives back its own, and the
        // texture has to describe the image the presentation engine actually made.
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

        // Direct3D 12 takes several present queues in Linked device mode and calls
        // ResizeBuffers1 with a node mask per queue. Vulkan presents from one queue, and the
        // engine only ever passes one.
        EE_ASSERT( parameters.m_presentQueues.size() == 1 );

        pVulkanSwapchain->m_device = pVulkanContext->m_device;
        pVulkanSwapchain->m_pPresentQueue = static_cast<VulkanQueue*>( parameters.m_presentQueues[0] );
        pVulkanSwapchain->m_surface = static_cast<VkSurfaceKHR>( parameters.m_pNativeWindowHandle );
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
        // **Direct3D 12 creates the swapchain UNorm and puts an sRGB render target view on it.
        // Vulkan creates the image sRGB and the view matches.** The two produce the same
        // conversion on write and the same picture on screen, and the Vulkan spelling needs no
        // VK_KHR_swapchain_mutable_format. So m_renderTargetFormat, which is the sRGB one, drives
        // both the image and the views; m_colorFormat is the fallback when the surface refuses it.
        uint32_t numSurfaceFormats = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &numSurfaceFormats, nullptr );
        TVector<VkSurfaceFormatKHR> surfaceFormats( numSurfaceFormats );
        vkGetPhysicalDeviceSurfaceFormatsKHR( pVulkanContext->m_physicalDevice, pVulkanSwapchain->m_surface, &numSurfaceFormats, surfaceFormats.data() );

        DataFormat         renderTargetFormat = parameters.m_renderTargetFormat;
        VkSurfaceFormatKHR chosenSurfaceFormat = {};

        for ( DataFormat const candidate : { parameters.m_renderTargetFormat, parameters.m_colorFormat } )
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
        // The transfer bits only when the surface allows them, because P5.10's copies and clears
        // can name any texture and a swapchain image is one.
        swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         ( surfaceCapabilities.supportedUsageFlags & ( VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT ) );
        // One queue presents and one queue renders, and they are the same queue. See
        // SetSharingMode for why every other resource is CONCURRENT instead.
        swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
        swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchainCreateInfo.presentMode = pVulkanSwapchain->m_presentMode;
        swapchainCreateInfo.clipped = VK_TRUE;

        VkResult result = vkCreateSwapchainKHR( pVulkanContext->m_device, &swapchainCreateInfo, nullptr, &pVulkanSwapchain->m_swapchain );
        EE_ASSERT( result == VK_SUCCESS );

        SetVulkanObjectName( pVulkanContext, VK_OBJECT_TYPE_SWAPCHAIN_KHR, uint64_t( pVulkanSwapchain->m_swapchain ), "Swapchain" );

        // Images
        //-------------------------------------------------------------------------
        // **minImageCount is a minimum, so the driver may hand back more images than were
        // asked for.** Swapchain::m_renderTargets is a fixed TArray of MaxPendingFrames, which
        // is 2, and several Linux drivers want three or four. If this halts, the fix is
        // MaxPendingFrames in RHI.h, which is an upstream change and a human decision.
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

        // Every caller waits the present queue idle first - Engine.cpp:756,
        // ImguiRenderer.cpp:64 and :93 - which is what Vulkan needs before the images go away.
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

        // **The surface is not destroyed here.** The application created it and hands the same
        // one back to the next CreateSwapchain; Window::ResizeSwapchain destroys and recreates
        // around an unchanged m_pNativeWindowHandle.

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
            // **No image was acquired and the semaphore was not signalled**, so recording a wait
            // on it would hang the queue. The engine compares the window size every frame and
            // recreates the swapchain, so this is the last frame before that happens; it renders
            // into the image it already held rather than stopping.
            return pVulkanSwapchain->m_currentImageIndex;
        }

        EE_ASSERT( result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR );

        // **Direct3D 12 needs nothing here at all**, because GetCurrentBackBufferIndex answers
        // without waiting for anything. Vulkan hands back an image the presentation engine may
        // still be reading, and the wait for it goes on the present queue, where the next submit
        // drains it. RecordQueueOrderingWait then carries the order to every submit after that,
        // which is what makes this sound when the submit that writes the image is not the first
        // one after this call. ForwardShadingRenderer submits several before it.
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

        // FIFO is the one mode every implementation has to support, and it is vsync. Without
        // vsync, MAILBOX if the surface has it and IMMEDIATE otherwise: the first tears nothing
        // and the second is what Direct3D's DXGI_PRESENT_ALLOW_TEARING asks for.
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

        // **A present mode is fixed when the swapchain is created, so a later call takes effect
        // at the next recreation.** Direct3D 12 changes a sync interval per present and needs no
        // such thing. Nothing in the engine calls this outside CreateSwapchain, so the two
        // behave identically today; a caller that starts to has to resize the swapchain as well.
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
        pVulkanCommandBuffer->m_needsRenderingBegin = false;
        pVulkanCommandBuffer->m_rootConstantRingOffset = 0;

        // A barrier that outlived its command buffer would apply to the wrong work. Direct3D 12
        // asserts the same three lists are empty at RHI_Direct3D12.cpp:2906.
        EE_ASSERT( pVulkanCommandBuffer->m_globalBarriers.empty() );
        EE_ASSERT( pVulkanCommandBuffer->m_bufferBarriers.empty() );
        EE_ASSERT( pVulkanCommandBuffer->m_imageBarriers.empty() );
    }

    void EndCommandBuffer( CommandBuffer* pCommandBuffer )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        EE_ASSERT( pVulkanCommandBuffer->m_stage == VulkanCommandBuffer::Stage::Recording );

        // Dynamic rendering has to be closed before the command buffer is. A configuration that
        // never reached a draw is begun and ended here, so the clear it carries still happens.
        FlushRendering( pVulkanCommandBuffer );

        // The same flush Direct3D 12 does at RHI_Direct3D12.cpp:2941. A barrier recorded and
        // never flushed would simply not happen.
        FlushBarriers( pVulkanCommandBuffer );

        VkResult const result = vkEndCommandBuffer( pVulkanCommandBuffer->m_commandBuffer );
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

        vkCmdBeginRendering( pVulkanCommandBuffer->m_commandBuffer, &renderingInfo );

        pVulkanCommandBuffer->m_needsRenderingBegin = false;
        pVulkanCommandBuffer->m_isRendering = true;
    }

    // Leaves the render pass so something that may not run inside one can run: a barrier, a
    // dispatch, a copy. The next draw begins it again, with every load op forced to LOAD so the
    // restart keeps what the first half drew.
    //
    // The store ops were fixed when the pass began, so a caller that asked for
    // StoreActionType::None would lose that half of the pass. No engine pass puts a barrier or a
    // dispatch between two draws, so this is a safety net rather than a path anything takes.
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
    // ended, because its load and store ops are the clear the caller asked for: a Direct3D 12
    // render target that is bound and cleared with no draw still gets cleared.
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

    // **LoadActionType::DontCare means "the caller said nothing", not "discard".**
    //
    // Direct3D 12 has no load actions at all: binding a render target preserves it, and the
    // backend reads m_loadActionsColor only to decide whether to call ClearRenderTargetView.
    // LoadAction is zero initialised and DontCare is the zero, so every action the engine leaves
    // alone arrives here as DontCare. RenderPass_DebugDraw.cpp:1316 builds a LoadAction that
    // sets only the depth action to Clear and binds the frame's final colour target with it;
    // mapping DontCare to VK_ATTACHMENT_LOAD_OP_DONT_CARE would discard the whole rendered frame
    // at that line. So DontCare preserves, which is what the reference backend does.
    //
    // Clear and Load still map exactly, and nothing loses the ability to say what it means.
    // Recorded under "Upstream issues observed" in Docs/Linux/Progress.md.
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

    // The same reasoning, and it matters more. **No engine pass sets a store action at all**, so
    // every attachment arrives here as StoreActionType::DontCare, and discarding on that would
    // throw away the output of every render pass in the frame.
    //
    // StoreActionType::None is untouched and still maps to VK_ATTACHMENT_STORE_OP_NONE, so a
    // caller that really wants the attachment left alone has a value that says so.
    static VkAttachmentStoreOp VulkanStoreOp( StoreActionType action )
    {
        switch ( action )
        {
            case StoreActionType::Store:    return VK_ATTACHMENT_STORE_OP_STORE;
            case StoreActionType::DontCare: return VK_ATTACHMENT_STORE_OP_STORE;
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
        // a begin and an end, so the previous one is finished here.
        FlushRendering( pVulkanCommandBuffer );

        // The same flush Direct3D 12 does at this point. The barriers that put these very
        // textures into their attachment layouts are pending right now, and the layouts below
        // are read from what they leave behind.
        FlushBarriers( pVulkanCommandBuffer );

        pVulkanCommandBuffer->m_colorAttachments.clear();
        pVulkanCommandBuffer->m_hasDepthAttachment = false;
        pVulkanCommandBuffer->m_hasStencilAttachment = false;

        if ( renderTargets.empty() && pDepthStencil == nullptr )
        {
            return;
        }

        // Load and store actions are what dynamic rendering is for. Direct3D 12 has no such
        // concept and clears with a separate ClearRenderTargetView call after binding; here the
        // clear is the load op, which is what the phase document's mapping asks for and what a
        // tiler needs.
        VkExtent2D renderArea = {};

        for ( size_t renderTargetIndex = 0; renderTargetIndex < renderTargets.size(); ++renderTargetIndex )
        {
            VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( renderTargets[renderTargetIndex] );

            // Which subresource of the target to draw into. Direct3D 12 picks a different
            // render target view; here it is a different VkImageView, created by CreateTexture.
            uint32_t const colorArraySlice = colorArraySlices.empty() ? 0 : colorArraySlices[renderTargetIndex];
            uint32_t const colorMipSlice = colorMipSlices.empty() ? 0 : colorMipSlices[renderTargetIndex];

            VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            attachment.imageView = pVulkanTexture->RenderTargetView( colorArraySlice, colorMipSlice );
            // The layout the texture is actually in, not the one an attachment is usually in.
            // The engine transitions a render target before binding it, and a caller that did
            // not is a bug worth naming here rather than a validation message later.
            EE_ASSERT( pVulkanTexture->m_currentLayout != VK_IMAGE_LAYOUT_UNDEFINED );
            attachment.imageLayout = pVulkanTexture->m_currentLayout;
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
            // Read from the texture for the same reason as the colour targets, and it matters
            // more here: RenderPass_DebugDraw.cpp:1342 binds a depth target it only reads, which
            // is DEPTH_STENCIL_READ_ONLY_OPTIMAL rather than the attachment layout.
            EE_ASSERT( pVulkanTexture->m_currentLayout != VK_IMAGE_LAYOUT_UNDEFINED );
            depthAttachment.imageLayout = pVulkanTexture->m_currentLayout;
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
        // Direct3D 12 has no render area; it draws wherever the viewport and scissor allow. The
        // full extent of the attachments is the same thing, and the viewport still restricts it.
        pVulkanCommandBuffer->m_renderingInfo.renderArea.extent = renderArea;
        pVulkanCommandBuffer->m_renderingInfo.layerCount = 1;

        // Configured, not begun. See VulkanCommandBuffer::m_needsRenderingBegin for why.
        pVulkanCommandBuffer->m_needsRenderingBegin = true;
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

    // Every draw does the same two things first, in this order, and both matter. The barriers
    // have to reach the device before the pass opens, because a barrier may not run inside one
    // and because they are what put the attachments into the layouts the pass names. Then the
    // pass opens, which CmdSetRenderTargets deliberately did not do.
    static void PrepareDraw( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        FlushBarriers( pVulkanCommandBuffer );
        BeginRenderingIfPending( pVulkanCommandBuffer );
    }

    void CmdDraw( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t firstVertex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        PrepareDraw( pVulkanCommandBuffer );
        vkCmdDraw( pVulkanCommandBuffer->m_commandBuffer, numVertices, 1, firstVertex, 0 );
    }

    void CmdDrawInstanced( CommandBuffer* pCommandBuffer, uint32_t numVertices, uint32_t numInstances, uint32_t firstVertex, uint32_t firstInstance )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        PrepareDraw( pVulkanCommandBuffer );
        vkCmdDraw( pVulkanCommandBuffer->m_commandBuffer, numVertices, numInstances, firstVertex, firstInstance );
    }

    void CmdDrawIndexed( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t firstIndex )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        PrepareDraw( pVulkanCommandBuffer );
        vkCmdDrawIndexed( pVulkanCommandBuffer->m_commandBuffer, numIndices, 1, firstIndex, 0, 0 );
    }

    void CmdDrawIndexedInstanced( CommandBuffer* pCommandBuffer, uint32_t numIndices, uint32_t numInstances, uint32_t firstIndex, uint32_t firstInstance )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        PrepareDraw( pVulkanCommandBuffer );
        vkCmdDrawIndexed( pVulkanCommandBuffer->m_commandBuffer, numIndices, numInstances, firstIndex, 0, firstInstance );
    }

    void CmdDispatchCompute( CommandBuffer* pCommandBuffer, uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );

        // A dispatch may not run inside a render pass. Direct3D 12 has no such rule, so the
        // engine does not close anything before dispatching. The pass is left rather than
        // finished, so a draw that follows in the same pass resumes it.
        FlushBarriers( pVulkanCommandBuffer );
        SuspendRendering( pVulkanCommandBuffer );

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

    // Copies and clears, and the four things they all have to do first. The commands themselves
    // are further down, in the order RHI.h declares them; these helpers sit here because the two
    // clears are the first callers.

    // A copy and a clear are transfer commands, and Vulkan lets neither run inside dynamic
    // rendering. Direct3D 12 has no such rule, so the engine closes nothing before either. This
    // is the same pair CmdDispatchCompute makes, in the same order: a barrier the transfer
    // depends on has to reach the device before the transfer does, and the flush is what leaves
    // the render pass.
    static void PrepareTransfer( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        FlushBarriers( pVulkanCommandBuffer );
        SuspendRendering( pVulkanCommandBuffer );
    }

    // **The engine never barriers a texture into a copy layout, because Direct3D 12 has none.**
    // A texture that is copied to is created in TextureState::Common - RenderSystem.cpp:650
    // asserts exactly that - and D3D12_BARRIER_LAYOUT_COMMON is already a legal copy source,
    // copy destination and unordered access view clear target. Vulkan needs GENERAL or one of
    // the TRANSFER layouts, and the image is still in the VK_IMAGE_LAYOUT_UNDEFINED that
    // vkCreateImage gave it, because nothing has barriered it yet. See
    // VulkanTexture::m_currentLayout, which P5.6 wrote for this.
    //
    // GENERAL, not TRANSFER_DST_OPTIMAL, so that the engine's belief stays true. The next
    // barrier the engine records on this texture names TextureState::Common as its source,
    // Common is GENERAL, and CmdBarrier asserts that the two agree.
    //
    // The texture is const because RHI.h passes it that way to every copy. The layout it is in
    // is not part of what the caller sees, so recording the change is not a change to it.
    static void TransitionTextureForTransfer( VulkanCommandBuffer* pVulkanCommandBuffer, VulkanTexture const* pVulkanTexture )
    {
        if ( pVulkanTexture->m_currentLayout == VK_IMAGE_LAYOUT_GENERAL )
        {
            return;
        }

        // Any other layout means the engine moved this texture somewhere and then copied it with
        // no barrier in between, which Direct3D 12 would not accept either.
        EE_ASSERT( pVulkanTexture->m_currentLayout == VK_IMAGE_LAYOUT_UNDEFINED );

        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = pVulkanTexture->m_currentLayout;
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

        const_cast<VulkanTexture*>( pVulkanTexture )->m_currentLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // **A Direct3D 12 clear is a shader write and a Vulkan clear is a transfer write, and the
    // engine barriers for the first one.** Renderer_ForwardShading.cpp:753 follows its clears
    // with ResourceAccess::UnorderedAccess as the source, which is a shader storage write and
    // does not cover vkCmdFillBuffer at all, so the cleared counters would be read stale. The
    // clear therefore records the transfer half of its own visibility barrier. It is batched
    // like every other barrier and reaches the device with them at the next dispatch.
    //
    // ALL_COMMANDS on the destination, listed in Docs/Linux/Progress.md as a site to narrow:
    // nothing here knows what reads the cleared resource next.
    static void RecordClearVisibilityBarrier( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        pVulkanCommandBuffer->m_globalBarriers.emplace_back( barrier );
    }

    // One aspect per copy. TextureCopyRegion has no plane index, and Direct3D 12 builds its
    // subresource index with plane 0 at RHI_Direct3D12.cpp:3547, which is the depth plane of a
    // depth-stencil texture.
    static VkImageAspectFlags TransferAspectMask( VulkanTexture const* pVulkanTexture )
    {
        if ( ( pVulkanTexture->m_aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT ) != 0 )
        {
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        }

        return pVulkanTexture->m_aspectMask;
    }

    // A buffer image copy takes its row length in texels and the engine lays its rows out at the
    // byte stride GetTextureCopyRowStride reports, so this converts the one into the other.
    // That function's own comment names this conversion as P5.10's obligation.
    static uint32_t CopyRowLengthInTexels( VulkanTexture const* pVulkanTexture, uint32_t mipLevel, uint32_t arrayLayer )
    {
        uint32_t const rowStride = GetTextureCopyRowStride( pVulkanTexture, mipLevel, arrayLayer );
        uint32_t const blockByteSize = FormatBlockBitSize( pVulkanTexture->m_format ) / 8;

        // A row length is a whole number of blocks or it cannot be expressed at all. The stride
        // is one for every format the engine uploads: ComputeFormatRowStride multiplies by the
        // block size, and the alignment GetTextureCopyRowStride then rounds up to is a power of
        // two, as is every block size. A 96-bit format would break it, and nothing uses one.
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

        // **The two backends read the clear value differently, and nothing calls this today.**
        // ClearUnorderedAccessViewUint writes the raw bits through a typed view, and
        // vkCmdClearColorImage converts the value to the image format, so the two agree on an
        // integer format and disagree on a normalised one. Recorded in Docs/Linux/Progress.md.
        VkClearColorValue vulkanClearValue = {};
        vulkanClearValue.uint32[0] = clearValue;
        vulkanClearValue.uint32[1] = clearValue;
        vulkanClearValue.uint32[2] = clearValue;
        vulkanClearValue.uint32[3] = clearValue;

        // Direct3D 12 clears one view per mip level. One Vulkan subresource range covers them
        // all, and the array layers with them.
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

        // vkCmdFillBuffer repeats one 32-bit value over the whole buffer, and
        // ClearUnorderedAccessViewUint with four identical components does the same thing to
        // every 32-bit component of the view. The two agree for every buffer the engine clears,
        // which are counters and 32-bit typed buffers. They would disagree on a 16-bit format.
        // VK_WHOLE_SIZE rounds the size down to a multiple of 4, which is what a fill needs.
        vkCmdFillBuffer( pVulkanCommandBuffer->m_commandBuffer, pVulkanBuffer->m_buffer, 0, VK_WHOLE_SIZE, clearValue );

        RecordClearVisibilityBarrier( pVulkanCommandBuffer );
    }

    void CmdBuildAccelerationStructure( CommandBuffer* pCommandBuffer, TArrayView<AccelerationStructure* const> accelerationStructures, TArrayView<uint32_t const> bottomLevelAccelerationStructureIndices )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    //-------------------------------------------------------------------------
    // Barriers
    //-------------------------------------------------------------------------
    // synchronization2, which is core in 1.3. The three CmdBarrier overloads mirror Direct3D
    // 12's enhanced barriers one for one, including the batching: a barrier is recorded on the
    // command buffer and the whole set goes to the device in one vkCmdPipelineBarrier2 at the
    // next draw, dispatch or EndCommandBuffer, exactly as RHI_Direct3D12.cpp:1586 does it.
    //
    // The phase document says to avoid reaching for ALL_COMMANDS and MEMORY_READ|WRITE
    // everywhere. Two entries below are that broad and both are recorded in Progress.md:
    // PipelineStage::All, which means exactly ALL_COMMANDS, and ResourceAccess::Common, which is
    // Direct3D's "any access" and has no narrower Vulkan spelling.

    static VkPipelineStageFlags2 VulkanPipelineStage( TBitFlags<PipelineStage> pipelineStages )
    {
        // The early return mirrors RHI_Direct3D12.cpp:617. "All" is not one bit among many; it
        // is the answer.
        if ( pipelineStages.IsFlagSet( PipelineStage::All ) ) { return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; }

        VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_NONE;

        // D3D12_BARRIER_SYNC_DRAW is every stage a draw runs through, which is what
        // ALL_GRAPHICS means here. It covers the depth test stages, and the engine relies on
        // that: a depth target is transitioned with PipelineStage::Draw, never with a depth
        // stage of its own.
        if ( pipelineStages.IsFlagSet( PipelineStage::Draw ) ) { stageMask |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT; }
        if ( pipelineStages.IsFlagSet( PipelineStage::PixelShader ) ) { stageMask |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; }
        // D3D12_BARRIER_SYNC_NON_PIXEL_SHADING includes compute, so this does too. The task and
        // mesh stages belong in here as well and are left out on purpose: their stage bits are
        // only legal once VK_EXT_mesh_shader is enabled, which is P5.14's job.
        if ( pipelineStages.IsFlagSet( PipelineStage::NonPixelShader ) )
        {
            stageMask |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        }
        if ( pipelineStages.IsFlagSet( PipelineStage::ComputeShader ) ) { stageMask |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; }
        if ( pipelineStages.IsFlagSet( PipelineStage::AllShader ) )
        {
            stageMask |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        }
        // ALL_TRANSFER rather than COPY, because Direct3D's SYNC_COPY sits next to SYNC_CLEAR
        // and SYNC_RESOLVE and the RHI has no separate flag for either, so a clear arrives here
        // as Copy. P5.10 records its clears against this.
        if ( pipelineStages.IsFlagSet( PipelineStage::Copy ) ) { stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; }
        if ( pipelineStages.IsFlagSet( PipelineStage::ExecuteIndirect ) ) { stageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT; }

        // The four below name stages that only exist once their extension is enabled, and none
        // of the three extensions is. Nothing can reach them: P5.16 owns raytracing, and no
        // video queue exists. They are mapped rather than left out so that the group that
        // enables the extension finds the mapping already correct.
        if ( pipelineStages.IsFlagSet( PipelineStage::Raytracing ) ) { stageMask |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::BuildAccelerationStructure ) ) { stageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::CopyAccelerationStructure ) ) { stageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::VideoDecode ) ) { stageMask |= VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR; }
        if ( pipelineStages.IsFlagSet( PipelineStage::VideoEncode ) ) { stageMask |= VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR; }
        // Vulkan has no video processing stage at all. Direct3D 12 has a whole video process
        // queue type; there is nothing to map it to, and nothing asks for it.

        return stageMask;
    }

    static VkAccessFlags2 VulkanAccess( TBitFlags<ResourceAccess> resourceAccess )
    {
        VkAccessFlags2 accessMask = VK_ACCESS_2_NONE;

        // D3D12_BARRIER_ACCESS_COMMON is "any access", and Vulkan spells that MEMORY_READ plus
        // MEMORY_WRITE. It is the broad mapping the phase document warns about, and it is also
        // the honest one: DeviceTextureState starts every texture at ResourceAccess::Common, so
        // this is what the first barrier on any texture uses as its source.
        if ( resourceAccess.IsFlagSet( ResourceAccess::Common ) ) { accessMask |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; }
        // Direct3D 12 maps Present onto COMMON. A Vulkan presentation engine read needs no
        // access bits at all, only the PRESENT_SRC layout, so NONE is both correct and narrower.
        if ( resourceAccess.IsFlagSet( ResourceAccess::Present ) ) { accessMask |= VK_ACCESS_2_NONE; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::ConstantBuffer ) ) { accessMask |= VK_ACCESS_2_UNIFORM_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::IndexBuffer ) ) { accessMask |= VK_ACCESS_2_INDEX_READ_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::RenderTarget ) ) { accessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::UnorderedAccess ) ) { accessMask |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::DepthWrite ) ) { accessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; }
        if ( resourceAccess.IsFlagSet( ResourceAccess::DepthRead ) ) { accessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT; }
        // A Direct3D shader resource view covers both a sampled texture and a read-only
        // structured buffer, and Vulkan splits those into two access bits.
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
        // VideoProcessRead and VideoProcessWrite have no Vulkan equivalent, for the same reason
        // PipelineStage::VideoProcess has none.

        return accessMask;
    }

    // A TextureState is a Direct3D barrier layout, and this is its Vulkan one. The texture is
    // needed because one state does not answer on its own: see the ShaderResource case.
    static VkImageLayout VulkanImageLayout( TextureState textureState, VulkanTexture const* pVulkanTexture )
    {
        switch ( textureState )
        {
            case TextureState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
            // D3D12_BARRIER_LAYOUT_COMMON allows any access, and GENERAL is the Vulkan layout
            // that does.
            case TextureState::Common: return VK_IMAGE_LAYOUT_GENERAL;
            // **Not always SHADER_READ_ONLY_OPTIMAL.** P5.6 wrote the sampled descriptor with
            // the texture's m_shaderReadLayout, which is GENERAL when the texture is also an
            // RWTexture, and a descriptor's layout has to match the layout the image is in.
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

    // A stage mask of NONE may carry no access bits. The two are built from independent
    // arguments, so a caller can produce an empty sync with a non-empty access, and the queue
    // filtering below can produce one too.
    static void NormalizeBarrierMasks( VkPipelineStageFlags2 stageMask, VkAccessFlags2& accessMask )
    {
        if ( stageMask == VK_PIPELINE_STAGE_2_NONE )
        {
            accessMask = VK_ACCESS_2_NONE;
        }
    }

    // Copied from RHI_Direct3D12.cpp:3408, TODO comment and all: the graphics-only stages are
    // dropped from the source on a queue that has no such stages, and asserted absent from the
    // destination. Vulkan is stricter than Direct3D here, so this is not optional; naming a
    // graphics stage in a barrier on a compute queue is a validation error.
    static void ClearGraphicsOnlyStages( VulkanCommandBuffer const* pVulkanCommandBuffer, TBitFlags<PipelineStage>& sourceSync, TBitFlags<PipelineStage> destinationSync )
    {
        if ( pVulkanCommandBuffer->m_pQueue->m_queueType != QueueType::Graphics )
        {
            sourceSync.ClearFlags( PipelineStageFlags_GraphicsQueueOnly );
            EE_ASSERT( !destinationSync.AreAnyFlagsSet( PipelineStage::Draw, PipelineStage::PixelShader ) );
        }
    }

    // Everything recorded since the last flush, in one call. Called by every draw, every
    // dispatch and EndCommandBuffer, which are the points Direct3D 12 flushes at too.
    static void FlushBarriers( VulkanCommandBuffer* pVulkanCommandBuffer )
    {
        if ( pVulkanCommandBuffer->m_globalBarriers.empty() &&
             pVulkanCommandBuffer->m_bufferBarriers.empty() &&
             pVulkanCommandBuffer->m_imageBarriers.empty() )
        {
            return;
        }

        // **A barrier may not run inside dynamic rendering.** This is the one place that has to
        // know it, which is why every other caller goes through the flush rather than reaching
        // for the render pass itself. The pass resumes at the next draw.
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
        // No queue ownership transfer. CreateBuffer and CreateTexture give a resource
        // CONCURRENT sharing across every family the context uses, which is what a Direct3D 12
        // resource has: none. See the sharing mode helper in CreateContext.
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = pVulkanBuffer->m_buffer;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;

        NormalizeBarrierMasks( barrier.srcStageMask, barrier.srcAccessMask );
        NormalizeBarrierMasks( barrier.dstStageMask, barrier.dstAccessMask );

        pVulkanCommandBuffer->m_bufferBarriers.emplace_back( barrier );
    }

    void CmdBarrier( CommandBuffer* pCommandBuffer, Texture* pTexture, TBitFlags<PipelineStage> sourceSync, TBitFlags<PipelineStage> destinationSync, TBitFlags<ResourceAccess> sourceAccess, TBitFlags<ResourceAccess> destinationAccess, TextureState sourceState, TextureState destinationState, TextureBarrierRegion region, TBitFlags<TextureBarrierFlags> flags )
    {
        VulkanCommandBuffer* pVulkanCommandBuffer = static_cast<VulkanCommandBuffer*>( pCommandBuffer );
        VulkanTexture* pVulkanTexture = static_cast<VulkanTexture*>( pTexture );

        ClearGraphicsOnlyStages( pVulkanCommandBuffer, sourceSync, destinationSync );

        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VulkanPipelineStage( sourceSync );
        barrier.dstStageMask = VulkanPipelineStage( destinationSync );
        barrier.srcAccessMask = VulkanAccess( sourceAccess );
        barrier.dstAccessMask = VulkanAccess( destinationAccess );

        // **The old layout comes from the texture, not from sourceState.** This is P5.6's first
        // obligation: vkCreateImage only accepts VK_IMAGE_LAYOUT_UNDEFINED, so a texture the
        // engine believes is already in its m_initialState is in fact in UNDEFINED until the
        // first barrier moves it. The caller's belief is asserted against the truth below rather
        // than used, so the two cannot drift silently.
        barrier.oldLayout = pVulkanTexture->m_currentLayout;
        barrier.newLayout = VulkanImageLayout( destinationState, pVulkanTexture );

        EE_ASSERT( pVulkanTexture->m_currentLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
                   pVulkanTexture->m_currentLayout == VulkanImageLayout( sourceState, pVulkanTexture ) );

        // Direct3D's discard flag says the old contents are not needed. UNDEFINED as the old
        // layout says exactly that, and it lets the driver skip decompressing what it is about
        // to overwrite.
        if ( flags.IsFlagSet( TextureBarrierFlags::Discard ) )
        {
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pVulkanTexture->m_image;
        // Every aspect the image has. A barrier that moved the depth and left the stencil behind
        // would put one image in two layouts, which is the thing m_currentLayout cannot express.
        barrier.subresourceRange.aspectMask = pVulkanTexture->m_aspectMask;
        barrier.subresourceRange.baseMipLevel = region.m_mipLevel;
        barrier.subresourceRange.levelCount = region.m_numMipLevels ? region.m_numMipLevels : pVulkanTexture->m_mipLevels;
        barrier.subresourceRange.baseArrayLayer = region.m_arraySlice;
        barrier.subresourceRange.layerCount = region.m_numArraySlices ? region.m_numArraySlices : pVulkanTexture->m_arrayLayers;

        NormalizeBarrierMasks( barrier.srcStageMask, barrier.srcAccessMask );
        NormalizeBarrierMasks( barrier.dstStageMask, barrier.dstAccessMask );

        pVulkanCommandBuffer->m_imageBarriers.emplace_back( barrier );

        // One layout for the whole image, which is exact only while callers barrier the whole
        // texture. Every engine barrier does: DeviceResourceStates::FlushBarriers passes an empty
        // TextureBarrierRegion. A caller that barriers one mip would leave this describing the
        // rest of the image wrongly, and the assert above is what would catch it.
        pVulkanTexture->m_currentLayout = barrier.newLayout;
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
        // Zero means the rows are packed to imageExtent.height, which is how the engine writes
        // them: RenderSystem.h:545 advances the staging offset by exactly the rows of this one
        // region. Direct3D 12 reads the same thing out of the subresource footprint.
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = TransferAspectMask( pVulkanDstTexture );
        region.imageSubresource.mipLevel = dstRegion.m_mipLevel;
        // One layer per copy, the way the subresource index Direct3D 12 builds names one. A 3D
        // texture has one layer and puts its slices in m_z and m_depth instead.
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
        // **The readback stride is GetTextureCopyRowStride too, and nothing calls this yet.**
        // Direct3D 12 uses the destination buffer's own footprint here, which for a buffer
        // resource is the whole buffer as a single row and says nothing about texture rows. One
        // rule for both directions is the useful answer, so a caller sizes its readback buffer
        // with the same function the upload path already uses.
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

    // **The one DataFormat to VkFormat mapping. There must never be a second one.** The phase
    // document warns that two mappings which disagree corrupt textures in a way that looks like
    // a bug somewhere else. Everything that needs a VkFormat comes here: buffer views, image
    // creation, pipeline attachment formats and the device capability query.
    //
    // Read next to DXGIFormat in RHI_Direct3D12.cpp:276, which is the specification. Two notes
    // on where the two backends do not line up one for one:
    //
    // - **Vulkan names packed formats most significant component first, and DXGI names them
    //   least significant first.** So DXGI_FORMAT_B5G6R5_UNORM is VK_FORMAT_R5G6B5_UNORM_PACK16,
    //   not VK_FORMAT_B5G6R5_UNORM_PACK16, and the same reversal applies to every other packed
    //   entry below. Getting one of these backwards swaps red and blue on that format alone.
    // - **RGB565_UNorm and BGR565_UNorm both map to the same VkFormat**, because the Direct3D 12
    //   backend maps both to DXGI_FORMAT_B5G6R5_UNORM. Vulkan can tell them apart and Direct3D
    //   cannot, so mapping them faithfully here would make the two backends draw the same asset
    //   differently. Nothing in the engine uses either format. Recorded in Docs/Linux/Progress.md.
    static VkFormat VulkanFormat( DataFormat format )
    {
        switch ( format )
        {
            case DataFormat::Undefined: return VK_FORMAT_UNDEFINED;

                // Uncompressed formats
                //
                // R1_UNorm has no Vulkan equivalent at all. Returning UNDEFINED without
                // asserting mirrors what the Direct3D 12 backend does with the ASTC formats it
                // cannot express.
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
                // Direct3D 12 has no stencil-only format and maps this to
                // DXGI_FORMAT_D24_UNORM_S8_UINT. Vulkan has the exact format, and support for it
                // is optional, so a device without it now reports the format unusable in
                // DeviceCapabilities rather than silently getting a depth buffer it never asked
                // for.
            case DataFormat::S8_Uint: return VK_FORMAT_S8_UINT;

                // Compressed DXBC formats
                //
                // Vulkan separates BC1 with and without alpha; DXGI_FORMAT_BC1_UNORM covers both.
                // The DataFormat enum makes the same distinction Vulkan does, so this is one
                // place where the mapping is more exact than the Direct3D 12 one.
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

                // Compressed ASTC formats. Direct3D has none of these and returns
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

    // The view type a shader reads the whole texture through. Mirrors
    // D3D12ShaderResourceViewDimension at RHI_Direct3D12.cpp:400, and reuses the same
    // TextureViewDimension decision so both backends classify a texture identically.
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

    // Which ViewDimension a texture is, by the same rules the Direct3D 12 backend uses at
    // RHI_Direct3D12.cpp:854. Kept as one function for the same reason the format mapping is:
    // two backends that classify a texture differently disagree about what its views mean.
    static ViewDimension VulkanTextureViewDimension( uint32_t width, uint32_t height, uint32_t depth, uint32_t arrayLayers, uint32_t numSamples, TBitFlags<DescriptorTypeFlags> const& descriptorTypes )
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

    // **Direct3D 12 resources have no queue ownership at all.** A resource written on the
    // compute queue is read on the graphics queue with only a barrier between them, and the
    // engine's async compute path relies on that. Vulkan's EXCLUSIVE sharing needs an ownership
    // transfer for the same thing, and nothing in RHI.h says which queue last touched a
    // resource, so there is nothing to build a transfer out of.
    //
    // CONCURRENT reproduces the Direct3D semantics exactly. It costs some compression on some
    // hardware, and it costs nothing when the device exposes one queue family, which is the case
    // this leaves EXCLUSIVE. P5.5 recorded the decision as P5.9's; this is it.
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
        VkResult const result = vkCreateImageView( device, &viewCreateInfo, nullptr, &imageView );
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

        VkFormat const vulkanFormat = VulkanFormat( parameters.m_format );
        EE_ASSERT( vulkanFormat != VK_FORMAT_UNDEFINED );

        VkImageAspectFlags const aspectMask = VulkanImageAspect( vulkanFormat );
        bool const isDepthStencil = ( aspectMask & ( VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT ) ) != 0;

        TBitFlags<DescriptorTypeFlags> const descriptorTypes = parameters.m_descriptorTypes;
        bool const isShaderResource = descriptorTypes.AreAnyFlagsSet( DescriptorTypeFlags::Texture, DescriptorTypeFlags::TextureCube );
        bool const isStorage = descriptorTypes.IsFlagSet( DescriptorTypeFlags::RWTexture );
        bool const isRenderTarget = descriptorTypes.IsFlagSet( DescriptorTypeFlags::RenderTarget );

        // The flags below need external memory or a console, and no device extension here
        // provides any of them. Nothing in the engine sets one; halting names the caller that
        // starts to. TextureFlags::DisableCompression and AllowDisplayTarget have no Vulkan
        // control at all and are ignored rather than refused, which is what Direct3D 12's own
        // heap flags amount to.
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

        // Vulkan wants the usage up front where Direct3D 12 derives it from the views that get
        // created, exactly as with buffers. Both transfer bits, because the engine uploads every
        // texture it loads and P5.10's copies and clears can name any of them.
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
        // vkCreateImage accepts UNDEFINED or PREINITIALIZED and nothing else, and the second is
        // for linear tiling. See VulkanTexture::m_currentLayout for what that costs P5.9.
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if ( parameters.m_pNativeHandle != nullptr )
        {
            // A VkImage somebody else owns and allocated. P5.3's swapchain images arrive this
            // way, which is why the views below are still built for it.
            pVulkanTexture->m_image = static_cast<VkImage>( parameters.m_pNativeHandle );
            pVulkanTexture->m_ownsImage = false;
        }
        else if ( parameters.m_pTextureToAlias != nullptr )
        {
            // Direct3D 12 aliases by sharing the ID3D12Resource pointer. Sharing the VkImage is
            // the same thing, and the aliased texture keeps ownership of the memory.
            pVulkanTexture->m_image = static_cast<VulkanTexture*>( parameters.m_pTextureToAlias )->m_image;
            pVulkanTexture->m_ownsImage = false;
        }
        else
        {
            VmaAllocationCreateInfo allocationCreateInfo = {};
            allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

            if ( parameters.m_memoryType != ResourceMemoryType::DeviceLocal )
            {
                // Direct3D 12 puts every texture on D3D12_HEAP_TYPE_DEFAULT and uploads through
                // a staging buffer; the RHI still lets a caller ask for host memory.
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
            VkResult const result = vmaCreateImage( pVulkanContext->m_resourceAllocator, &imageCreateInfo, &allocationCreateInfo, &pVulkanTexture->m_image, &pVulkanTexture->m_allocation, &allocationInfo );
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
        pVulkanTexture->m_format = parameters.m_format;
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

        // A texture that is also an RWTexture has to sit in VK_IMAGE_LAYOUT_GENERAL, because a
        // storage image descriptor may name no other layout and one image cannot be in two
        // layouts at once. Recorded on the texture so P5.9 transitions to the layout the
        // descriptor was actually written with rather than guessing from TextureState.
        pVulkanTexture->m_shaderReadLayout = isStorage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Views
        //-------------------------------------------------------------------------
        // Direct3D 12 puts the subresource selection in the descriptor. Vulkan puts it in the
        // view, so every subresource the engine can name needs one built up front.

        if ( isShaderResource )
        {
            // A sampled view of a depth-stencil image must name exactly one aspect. Depth is the
            // one the engine reads, which is the same choice RHI_Direct3D12.cpp:4597 makes.
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
            // One view per subresource, in the order Direct3D 12 allocates its render target
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
        // One contiguous run in the resource heap, laid out exactly as Direct3D 12 lays it out
        // at RHI_Direct3D12.cpp:4562: the read view first if present, then one read-write view
        // per mip level. GetTextureHandle does the same arithmetic on both backends.

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

        // Direct3D 12 reads this out of GetCopyableFootprints, which aligns the row pitch to
        // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT. Vulkan has no such call, because the layout of the
        // staging buffer is the caller's to choose; the equivalent alignment is the device's
        // optimalBufferCopyRowPitchAlignment.
        //
        // **P5.10 must derive vkCmdCopyBufferToImage's bufferRowLength from this same number.**
        // The engine writes its rows at this stride and the copy has to read them at it.
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

    // Direct3D 12 takes any border colour; Vulkan takes one of six fixed ones unless
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

        // The same pairing RHI_Direct3D12.cpp:497 asserts on, where a comparison filter and a
        // comparison function are two halves of one D3D12_FILTER value.
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

        // SamplerParameters::m_setLODRange has no Direct3D 12 use either; CreateSampler there
        // ignores it too.

        VkResult const result = vkCreateSampler( pVulkanContext->m_device, &samplerCreateInfo, nullptr, &pVulkanSampler->m_sampler );
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
