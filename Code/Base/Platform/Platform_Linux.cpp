#ifdef __linux__
#include "Platform.h"
#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

//-------------------------------------------------------------------------

namespace EE::Platform
{
    //-------------------------------------------------------------------------
    // Crash Handling
    //-------------------------------------------------------------------------
    // Not a port of the Win32 sibling. Its stack walker, dump writer and exception handler are all
    // inside an #if 0 block with the Initialize and Shutdown bodies commented out, so there is no
    // behaviour to match. This does the minimum that makes a Linux crash legible: print a backtrace
    // and re-raise so the default action still applies. Core dumps stay the kernel's job.
    //
    // The handler is async-signal-safe and must stay that way: no malloc, no EE_LOG, no String, no
    // printf family. The Win32 code formatted messages inside its handler, which is legal there and
    // undefined behaviour in a POSIX signal handler, so do not copy that shape back in.

    namespace
    {
        constexpr int32_t const g_maxCallstackDepth = 64;

        // Written to before the handler can run, and read only from within it.
        void* g_callstackBuffer[g_maxCallstackDepth];

        void WriteLiteral( char const* pText )
        {
            // write() is async-signal-safe; fputs and friends are not.
            ssize_t const ignored = write( STDERR_FILENO, pText, strlen( pText ) );
            (void) ignored;
        }

        void CrashSignalHandler( int signalNumber, siginfo_t* pSignalInfo, void* pContext )
        {
            (void) pSignalInfo;
            (void) pContext;

            WriteLiteral( "\n=======================================================\n" );
            WriteLiteral( "Caught signal: " );
            WriteLiteral( strsignal( signalNumber ) );
            WriteLiteral( "\n=======================================================\n" );

            int32_t const numFrames = backtrace( g_callstackBuffer, g_maxCallstackDepth );
            backtrace_symbols_fd( g_callstackBuffer, numFrames, STDERR_FILENO );
            WriteLiteral( "=======================================================\n" );

            // Restore the default action and re-raise, so the process still dies the way the
            // shell and the kernel expect: correct exit status, and a core file if one is
            // enabled. Swallowing the signal here would hide the crash from everything else.
            signal( signalNumber, SIG_DFL );
            raise( signalNumber );
        }

        int const g_handledSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    }

    //-------------------------------------------------------------------------
    // Platform
    //-------------------------------------------------------------------------

    void Initialize()
    {
        // Warm up backtrace() before any signal can arrive. It is not async-signal-safe on its
        // first call: glibc loads the unwinder from libgcc_s.so lazily, so that call reaches dlopen
        // and mallocs. A crash inside the allocator would then deadlock in the handler reporting
        // it, which is the case the handler exists for. Calling it once here forces the load on the
        // main thread with nothing held.
        void* warmupBuffer[1];
        int32_t const ignoredFrames = backtrace( warmupBuffer, 1 );
        (void) ignoredFrames;

        struct sigaction action;
        memset( &action, 0, sizeof( action ) );
        action.sa_sigaction = CrashSignalHandler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset( &action.sa_mask );

        for ( int const signalNumber : g_handledSignals )
        {
            sigaction( signalNumber, &action, nullptr );
        }
    }

    void Shutdown()
    {
        for ( int const signalNumber : g_handledSignals )
        {
            signal( signalNumber, SIG_DFL );
        }
    }
}
#endif
