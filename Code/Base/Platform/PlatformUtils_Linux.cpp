#ifdef __linux__
#include "PlatformUtils_Linux.h"
#include "Base/FileSystem/FileSystemPath.h"
#include "Base/Logging/Log.h"
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

//-------------------------------------------------------------------------

namespace EE::Platform::Linux
{
    // File system
    //-------------------------------------------------------------------------

    String GetShortPath( String const& origPath )
    {
        // Win32 returns the 8.3 form. Linux has no such thing, so the path is already as short
        // as it gets.
        return origPath;
    }

    String GetLongPath( String const& origPath )
    {
        // See GetShortPath: there is no short form to expand.
        return origPath;
    }

    // Processes
    //-------------------------------------------------------------------------

    uint32_t GetProcessID( char const* processName )
    {
        EE_ASSERT( processName != nullptr );

        DIR* pProcDirectory = opendir( "/proc" );
        if ( pProcDirectory == nullptr )
        {
            return 0;
        }

        uint32_t foundProcessID = 0;

        while ( dirent const* pEntry = readdir( pProcDirectory ) )
        {
            // Every process is a numerically named directory under /proc
            char* pEnd = nullptr;
            long const processID = strtol( pEntry->d_name, &pEnd, 10 );
            if ( pEnd == pEntry->d_name || *pEnd != 0 || processID <= 0 )
            {
                continue;
            }

            char commandPath[64];
            snprintf( commandPath, sizeof( commandPath ), "/proc/%ld/comm", processID );

            FILE* pCommandFile = fopen( commandPath, "r" );
            if ( pCommandFile == nullptr )
            {
                continue;
            }

            char commandName[256] = { 0 };
            if ( fgets( commandName, sizeof( commandName ), pCommandFile ) != nullptr )
            {
                // /proc/<pid>/comm is newline terminated, and truncated to 15 characters
                size_t const length = strlen( commandName );
                if ( length > 0 && commandName[length - 1] == '\n' )
                {
                    commandName[length - 1] = 0;
                }

                if ( strcmp( commandName, processName ) == 0 )
                {
                    foundProcessID = (uint32_t) processID;
                }
            }

            fclose( pCommandFile );

            if ( foundProcessID != 0 )
            {
                break;
            }
        }

        closedir( pProcDirectory );
        return foundProcessID;
    }

    String GetProcessPath( uint32_t processID )
    {
        char linkPath[64];
        snprintf( linkPath, sizeof( linkPath ), "/proc/%u/exe", processID );

        char resolvedPath[PATH_MAX] = { 0 };
        ssize_t const length = readlink( linkPath, resolvedPath, sizeof( resolvedPath ) - 1 );
        if ( length < 0 )
        {
            return String();
        }

        resolvedPath[length] = 0;
        return String( resolvedPath );
    }

    String GetCurrentModulePath()
    {
        char resolvedPath[PATH_MAX] = { 0 };
        ssize_t const length = readlink( "/proc/self/exe", resolvedPath, sizeof( resolvedPath ) - 1 );
        if ( length < 0 )
        {
            return String();
        }

        resolvedPath[length] = 0;
        return String( resolvedPath );
    }

    String GetLastErrorMessage()
    {
        char buffer[256] = { 0 };
        // The GNU strerror_r may return a pointer to its own static string rather than filling
        // the buffer, so use what it returns rather than assuming the buffer was written.
        char const* pMessage = strerror_r( errno, buffer, sizeof( buffer ) );
        return String( pMessage );
    }

    uint32_t StartProcess( char const* exePath, char const* cmdLine )
    {
        EE_ASSERT( exePath != nullptr );

        pid_t const childProcessID = fork();
        if ( childProcessID < 0 )
        {
            return 0;
        }

        if ( childProcessID == 0 )
        {
            // Win32 takes a single command line string and the process splits it itself. execv
            // takes an argument vector, so the command line is passed through as one argument
            // rather than being split here: guessing at quoting rules would be worse than the
            // callers, all of which pass a single simple argument, needing to know.
            char* arguments[3] = { const_cast<char*>( exePath ), nullptr, nullptr };
            if ( cmdLine != nullptr && cmdLine[0] != 0 )
            {
                arguments[1] = const_cast<char*>( cmdLine );
            }

            execv( exePath, arguments );
            _exit( 127 ); // Only reached if execv failed
        }

        return (uint32_t) childProcessID;
    }

    bool KillProcess( uint32_t processID )
    {
        if ( kill( (pid_t) processID, SIGTERM ) != 0 )
        {
            return false;
        }

        // Reap the child if it is ours, so it does not linger as a zombie
        int status = 0;
        waitpid( (pid_t) processID, &status, WNOHANG );
        return true;
    }

    void OpenInExplorer( char const* path )
    {
        EE_ASSERT( path != nullptr );

        pid_t const childProcessID = fork();
        if ( childProcessID == 0 )
        {
            char* arguments[3] = { const_cast<char*>( "xdg-open" ), const_cast<char*>( path ), nullptr };
            execvp( "xdg-open", arguments );
            _exit( 127 );
        }
    }

    // Vulkan surface
    //-------------------------------------------------------------------------

    void* CreateVulkanSurface( void* pVulkanInstance, void* pNativeWindowHandle )
    {
        EE_ASSERT( pVulkanInstance != nullptr );

        // Headless. Phase 5 built the swapchain to run with no surface at all, and that path is
        // still how anything without a window renders.
        if ( pNativeWindowHandle == nullptr )
        {
            return nullptr;
        }

        // SDL loaded the Vulkan library when the window was created with SDL_WINDOW_VULKAN, so
        // there is no SDL_Vulkan_LoadLibrary call here.
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if ( !SDL_Vulkan_CreateSurface( reinterpret_cast<SDL_Window*>( pNativeWindowHandle ), reinterpret_cast<VkInstance>( pVulkanInstance ), nullptr, &surface ) )
        {
            EE_LOG_ERROR( LogCategory::Render, "Platform", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError() );
            return nullptr;
        }

        return surface;
    }

    void DestroyVulkanSurface( void* pVulkanInstance, void* pSurface )
    {
        if ( pSurface == nullptr )
        {
            return;
        }

        EE_ASSERT( pVulkanInstance != nullptr );
        SDL_Vulkan_DestroySurface( reinterpret_cast<VkInstance>( pVulkanInstance ), reinterpret_cast<VkSurfaceKHR>( pSurface ), nullptr );
    }
}
#endif
