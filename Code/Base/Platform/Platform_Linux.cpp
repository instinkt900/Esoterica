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
    // Note on the Win32 sibling: its stack walker, crash dump writer and vectored exception
    // handler are all inside an `#if 0` block, and Initialize/Shutdown have their bodies
    // commented out. None of it runs. So there is no behaviour here to match, and this is not a
    // port of that code.
    //
    // What is implemented instead is the minimum that makes a Linux crash legible: a signal
    // handler that prints a backtrace and then re-raises so the default action still applies.
    // Core dumps stay the kernel's job, through `ulimit -c` and /proc/sys/kernel/core_pattern.
    //
    // **The handler is async-signal-safe, and must stay that way.** Only backtrace,
    // backtrace_symbols_fd and write are called from it. The Win32 code allocated strings and
    // formatted messages inside its handler, which is legal on Windows and undefined behaviour
    // in a POSIX signal handler, so that structure is deliberately not copied. In particular:
    // no malloc, no EE_LOG, no String, and no printf family.

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
        // Warm up backtrace() before any signal can arrive.
        //
        // The handler itself is written to be async-signal-safe - write() rather than fputs,
        // backtrace_symbols_fd rather than backtrace_symbols - but **backtrace() is not safe on
        // its first call**. glibc loads the unwinder from libgcc_s.so lazily, so the first call
        // reaches dlopen and mallocs. A crash that happened inside the allocator would then
        // deadlock in the handler that is trying to report it, which is exactly the case the
        // handler exists for.
        //
        // Calling it once here forces the load now, on the main thread, with nothing held. Found
        // by ThreadSanitizer in P8.6, as "signal-unsafe call inside of a signal" with
        // _dl_map_object_deps under CrashSignalHandler.
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
