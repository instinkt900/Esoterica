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
}

//-------------------------------------------------------------------------
// Win32 name alias
//-------------------------------------------------------------------------
// Shared tools code calls these helpers as "Platform::Win32::X", because upstream only ever had a
// Win32 implementation. Aliasing the namespace keeps those call sites compiling unchanged, which
// matters more than the name reading oddly here: the alternative is a platform guard around every
// call, in exactly the editor UI code upstream edits most often.

namespace EE::Platform
{
    namespace Win32 = Linux;
}
#endif