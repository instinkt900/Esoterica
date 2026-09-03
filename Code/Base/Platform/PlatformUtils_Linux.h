#pragma once
#ifdef __linux__

#include "Base/_Module/API.h"
#include "Base/Types/String.h"

//-------------------------------------------------------------------------
// Platform Specific Helpers/Functions
//-------------------------------------------------------------------------

namespace EE::Platform::Linux
{
    // File system
    //-------------------------------------------------------------------------

    EE_BASE_API String GetShortPath( String const& origPath );
    EE_BASE_API String GetLongPath( String const& origPath );

    // Processes
    //-------------------------------------------------------------------------

    EE_BASE_API uint32_t GetProcessID( char const* processName );
    EE_BASE_API String GetProcessPath( uint32_t processID );
    EE_BASE_API String GetCurrentModulePath();
    EE_BASE_API String GetLastErrorMessage();

    // Try to start a window process and returns the process ID
    EE_BASE_API uint32_t StartProcess( char const* exePath, char const* cmdLine = nullptr );

    // Kill a running process
    EE_BASE_API bool KillProcess( uint32_t processID );

    // Check if a named process is currently running
    inline bool IsProcessRunning( char const* processName, uint32_t* pProcessID ) { return GetProcessID( processName ) != 0; }

    // Open a path in explorer
    EE_BASE_API void OpenInExplorer( char const* path );

    // Vulkan surface
    //-------------------------------------------------------------------------
    // The one place in the engine that knows both SDL3 and Vulkan. Creating a VkSurfaceKHR needs a
    // window system library and Base/Render must not depend on one, so RHI_Vulkan.cpp calls through
    // here. The handles cross as void*, so this header needs neither SDL nor Vulkan either.
    //
    // pNativeWindowHandle is the SDL_Window* that Platform::SetMainWindowHandle holds.

    // Returns a VkSurfaceKHR, or nullptr on failure. The caller owns it.
    EE_BASE_API void* CreateVulkanSurface( void* pVulkanInstance, void* pNativeWindowHandle );

    // Safe to call with a null surface.
    EE_BASE_API void DestroyVulkanSurface( void* pVulkanInstance, void* pSurface );
}

//-------------------------------------------------------------------------
// Win32 name alias
//-------------------------------------------------------------------------
// Shared tools code calls these helpers as "Platform::Win32::X", because upstream only ever had a
// Win32 implementation. The alias keeps those call sites compiling unchanged. The alternative is a
// platform guard around every call, in the editor UI code upstream edits most often.

namespace EE::Platform
{
    namespace Win32 = Linux;
}
#endif