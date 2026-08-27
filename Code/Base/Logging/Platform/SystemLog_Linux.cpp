#ifdef __linux__
#include "Base/Esoterica.h"
#include <stdio.h>

//-------------------------------------------------------------------------

namespace EE::SystemLog
{
    void TraceMessage( const char* format, ... )
    {
        constexpr size_t const bufferSize = 2048;
        char messageBuffer[bufferSize]; // Dont make this static as we need this to be threadsafe!!!

        va_list args;
        va_start( args, format );
        int32_t numCharsWritten = vsnprintf( messageBuffer, bufferSize, format, args );
        va_end( args );

        // Add newline
        //-------------------------------------------------------------------------
        // Note: this deliberately differs from SystemLog_Win32.cpp, which bounds this check at
        // 509 even though the buffer is 2048, so messages between 509 and 2045 characters
        // silently lose their newline. That is an upstream bug, recorded in
        // Docs/Linux/Progress.md. The bound here is the real buffer size.

        if ( numCharsWritten > 0 && numCharsWritten < (int32_t) ( bufferSize - 2 ) )
        {
            messageBuffer[numCharsWritten] = '\n';
            messageBuffer[numCharsWritten + 1] = 0;
        }

        // Win32 writes to the debugger with OutputDebugStringA. There is no debugger channel on
        // Linux, so this goes to stderr, which is where a native debugger and a terminal both
        // see it.
        fputs( messageBuffer, stderr );
    }
}

#endif
