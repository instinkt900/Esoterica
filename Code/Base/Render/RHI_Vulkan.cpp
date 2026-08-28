#ifdef __linux__
#include "Base/Esoterica.h"

#include "RHI.h"

#include "Base/Math/Math.h"
#include "Base/Types/HashMap.h"

#include "EASTL/algorithm.h"

#include <vulkan/vulkan.h>
#include <dlfcn.h>

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

        //-------------------------------------------------------------------------

        pVulkanContext->m_vkSetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>( vkGetInstanceProcAddr( pVulkanContext->m_instance, "vkSetDebugUtilsObjectNameEXT" ) );

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

    // P5.4 owns command pools and buffers and will extend this. QueueSubmit needs the handle
    // out of it, so the type has to exist now; one member is the whole of what P5.2 uses.
    struct VulkanCommandBuffer final : CommandBuffer
    {
        VkCommandBuffer                                     m_commandBuffer = VK_NULL_HANDLE;
    };

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

    // Clip-space Y is inverted HERE, and nowhere else. This is Phase 4's criterion 9 decision;
    // the full reasoning is in Docs/Linux/Progress.md.
    //
    // The engine builds its projection matrices with DirectXMath's right-handed conventions
    // (Math::CreatePerspectiveProjectionMatrix, "Taken from DirectXMath: XMMatrixPerspectiveFovRH"),
    // so NDC is Y-up with a 0..1 depth range. Vulkan shares the depth range and disagrees on Y.
    //
    //     vkViewport.x        = x;
    //     vkViewport.y        = y + height;   // flip
    //     vkViewport.width    = width;
    //     vkViewport.height   = -height;      // flip
    //     vkViewport.minDepth = minDepth;
    //     vkViewport.maxDepth = maxDepth;
    //
    // The shader compiler does NOT flip: the Reflector deliberately does not pass -fvk-invert-y.
    // Do not add it, and do not flip the projection matrices. Doing this twice is the classic
    // porting bug, and the second flip is silent.
    //
    // A negative viewport height needs no extension; it is core Vulkan since 1.1 and the baseline
    // here is 1.3.
    //
    // CONSEQUENCE FOR CreatePipeline: mirroring the viewport inverts triangle winding in
    // framebuffer space, so the front-face mapping has to absorb it. See the note there.
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

    // FrontFace, and why it maps literally here while Direct3D 12 maps it inverted.
    //
    // RHI_Direct3D12.cpp:5287 sets FrontCounterClockwise = ( m_frontFace == FrontFace::ClockWise ),
    // which reads backwards and is upstream's business, not ours. Taking it at face value:
    // FrontFace::CounterClockWise, the default, means front faces are CLOCKWISE in screen space
    // on Direct3D.
    //
    // Two inversions apply on the way to Vulkan and they cancel:
    //
    //   1. To match Direct3D with no Y flip, VkFrontFace would have to be the opposite of the
    //      enum's name, exactly as the Direct3D mapping is.
    //   2. CmdSetViewport flips Y with a negative viewport height, which mirrors winding in
    //      framebuffer space and inverts it again.
    //
    // So the mapping here is the literal one, FrontFace::ClockWise -> VK_FRONT_FACE_CLOCKWISE,
    // and it is only correct BECAUSE of the viewport flip. If anyone ever removes that flip,
    // this has to be swapped in the same commit.
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
