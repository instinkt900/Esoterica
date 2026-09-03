#ifdef __linux__
#include "PlatformUtils_Linux.h"
#include "Base/FileSystem/FileSystemPath.h"
#include "Base/Logging/Log.h"
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

            // The executable name, not /proc/<pid>/comm. comm truncates to 15 characters, and this
            // fork's ResourceServer and ResourceCompiler binaries both truncate to "Esoterica.Appli"
            // and so compare equal. /proc/<pid>/exe is the full path.
            //
            // readlink fails with EACCES for a process owned by another user, which is the right
            // answer here: every caller is looking for a process it started itself.
            char linkPath[64];
            snprintf( linkPath, sizeof( linkPath ), "/proc/%ld/exe", processID );

            char resolvedPath[PATH_MAX] = { 0 };
            ssize_t const length = readlink( linkPath, resolvedPath, sizeof( resolvedPath ) - 1 );
            if ( length < 0 )
            {
                continue;
            }

            resolvedPath[length] = 0;

            char const* pExecutableName = strrchr( resolvedPath, '/' );
            pExecutableName = ( pExecutableName != nullptr ) ? pExecutableName + 1 : resolvedPath;

            if ( strcmp( pExecutableName, processName ) == 0 )
            {
                foundProcessID = (uint32_t) processID;
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

    // Percent-encodes a filesystem path into a file:// URI. Everything outside the unreserved set
    // is encoded, apart from the path delimiter itself, so a name with a space or a '#' in it
    // still names the file it came from.
    static String EncodePathAsFileURI( char const* path )
    {
        static char const* const pHexDigits = "0123456789ABCDEF";

        String uri( "file://" );

        for ( char const* pChar = path; *pChar != 0; pChar++ )
        {
            unsigned char const c = (unsigned char) *pChar;
            bool const isUnreserved = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';

            if ( isUnreserved )
            {
                uri += (char) c;
            }
            else
            {
                uri += '%';
                uri += pHexDigits[c >> 4];
                uri += pHexDigits[c & 0x0F];
            }
        }

        return uri;
    }

    void OpenInExplorer( char const* path )
    {
        EE_ASSERT( path != nullptr && path[0] != 0 );

        // A directory path carries a trailing delimiter. Strip it, so the file manager selects the
        // directory inside its parent rather than opening it, which is what Windows does.
        String targetPath( path );
        while ( targetPath.length() > 1 && targetPath.back() == '/' )
        {
            targetPath.pop_back();
        }

        InlineString const uriArgument( InlineString::CtorSprintf(), "array:string:%s", EncodePathAsFileURI( targetPath.c_str() ).c_str() );

        // The fallback target, for a desktop with no FileManager1 provider.
        String containingDirectory( targetPath );
        size_t const lastDelimiterIdx = containingDirectory.find_last_of( '/' );
        containingDirectory.resize( ( lastDelimiterIdx == String::npos ) ? 0 : lastDelimiterIdx + 1 );

        // Every string the child processes need is built above, before the fork. A forked child of
        // a threaded process must not allocate.
        //-------------------------------------------------------------------------

        pid_t const childProcessID = fork();
        if ( childProcessID == 0 )
        {
            // Fork twice, so the grandchild is reparented to init. Nothing here waits for the file
            // manager, and a single fork would leave a zombie behind on every call.
            if ( fork() == 0 )
            {
                // org.freedesktop.FileManager1.ShowItems opens the containing folder and selects
                // the item, which is what "explorer.exe /select," does on Windows. Nautilus,
                // Dolphin, Thunar, Nemo and PCManFM all implement it, and D-Bus starts the file
                // manager if it is not already running.
                char* const showItemsArguments[] =
                {
                    const_cast<char*>( "dbus-send" ),
                    const_cast<char*>( "--session" ),
                    const_cast<char*>( "--print-reply" ),
                    const_cast<char*>( "--dest=org.freedesktop.FileManager1" ),
                    const_cast<char*>( "--type=method_call" ),
                    const_cast<char*>( "/org/freedesktop/FileManager1" ),
                    const_cast<char*>( "org.freedesktop.FileManager1.ShowItems" ),
                    const_cast<char*>( uriArgument.c_str() ),
                    const_cast<char*>( "string:" ),
                    nullptr
                };

                pid_t const showItemsProcessID = fork();
                if ( showItemsProcessID == 0 )
                {
                    // --print-reply is what makes dbus-send wait for the call and report a failure,
                    // which is the only way to know whether to fall back. The reply itself is
                    // noise, so it and any error go to /dev/null rather than the editor's terminal.
                    int const nullFileDescriptor = open( "/dev/null", O_WRONLY );
                    if ( nullFileDescriptor != -1 )
                    {
                        dup2( nullFileDescriptor, STDOUT_FILENO );
                        dup2( nullFileDescriptor, STDERR_FILENO );
                    }

                    execvp( "dbus-send", showItemsArguments );
                    _exit( 127 );
                }

                int status = 0;
                if ( showItemsProcessID > 0 && waitpid( showItemsProcessID, &status, 0 ) == showItemsProcessID && WIFEXITED( status ) && WEXITSTATUS( status ) == 0 )
                {
                    _exit( 0 );
                }

                // xdg-open is not an equivalent and is never given the path itself: it launches
                // whatever application claims the file type, which for a .map on the development
                // machine is an ebook reader. The containing directory is the closest it gets.
                char* const openDirectoryArguments[] = { const_cast<char*>( "xdg-open" ), const_cast<char*>( containingDirectory.c_str() ), nullptr };
                execvp( "xdg-open", openDirectoryArguments );
                _exit( 127 );
            }

            _exit( 0 );
        }

        if ( childProcessID > 0 )
        {
            waitpid( childProcessID, nullptr, 0 );
        }
    }

    // Vulkan surface
    //-------------------------------------------------------------------------

    void* CreateVulkanSurface( void* pVulkanInstance, void* pNativeWindowHandle )
    {
        EE_ASSERT( pVulkanInstance != nullptr );

        // Headless. The swapchain runs with no surface at all, which is how anything without a
        // window renders.
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
