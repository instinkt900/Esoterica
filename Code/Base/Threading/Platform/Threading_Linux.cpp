#ifdef __linux__
#include "Base/Threading/Threading.h"
#include <condition_variable>
#include <dirent.h>
#include <mutex>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unordered_set>

//-------------------------------------------------------------------------

namespace EE::Threading
{
    ProcessorInfo GetProcessorInfo()
    {
        ProcessorInfo procInfo;

        // sysconf gives the logical count directly. The physical count has no equivalent call,
        // so it comes from deduplicating ( package id, core id ) pairs across the topology in
        // sysfs: two hyperthreads on one core share both values.
        long const numOnlineProcessors = sysconf( _SC_NPROCESSORS_ONLN );
        procInfo.m_numLogicalCores = (uint16_t) ( numOnlineProcessors > 0 ? numOnlineProcessors : 1 );

        std::unordered_set<uint64_t> uniqueCores;

        for ( long cpuIndex = 0; cpuIndex < numOnlineProcessors; cpuIndex++ )
        {
            auto ReadTopologyValue = [cpuIndex] ( char const* pFileName, int32_t& outValue )
            {
                char path[256];
                snprintf( path, sizeof( path ), "/sys/devices/system/cpu/cpu%ld/topology/%s", cpuIndex, pFileName );

                FILE* pFile = fopen( path, "r" );
                if ( pFile == nullptr )
                {
                    return false;
                }

                bool const read = ( fscanf( pFile, "%d", &outValue ) == 1 );
                fclose( pFile );
                return read;
            };

            int32_t coreID = 0;
            int32_t packageID = 0;

            if ( ReadTopologyValue( "core_id", coreID ) && ReadTopologyValue( "physical_package_id", packageID ) )
            {
                uniqueCores.insert( ( (uint64_t) (uint32_t) packageID << 32 ) | (uint32_t) coreID );
            }
        }

        // sysfs is absent in some containers, so fall back to the logical count rather than
        // reporting zero physical cores, which would divide by zero in the task system.
        procInfo.m_numPhysicalCores = uniqueCores.empty() ? procInfo.m_numLogicalCores : (uint16_t) uniqueCores.size();

        return procInfo;
    }

    //-------------------------------------------------------------------------

    ThreadID GetCurrentThreadID()
    {
        // gettid() is only wrapped by glibc from 2.30 onwards, and the syscall is always there.
        return (ThreadID) syscall( SYS_gettid );
    }

    void SetCurrentThreadName( char const* pName )
    {
        EE_ASSERT( pName != nullptr );

        // pthread_setname_np fails outright if the name exceeds 16 bytes including the null
        // terminator, so truncate rather than silently ending up with no name at all.
        constexpr size_t const maxThreadNameLength = 16;
        char truncatedName[maxThreadNameLength];

        strncpy( truncatedName, pName, maxThreadNameLength - 1 );
        truncatedName[maxThreadNameLength - 1] = 0;

        pthread_setname_np( pthread_self(), truncatedName );
    }

    //-------------------------------------------------------------------------
    // SyncEvent
    //-------------------------------------------------------------------------
    // The Win32 version passes bManualReset, so this is a manual reset event: once signalled it
    // stays signalled, releasing every current and future waiter, until Reset() is called. Waiting
    // does not clear it.
    //
    // Hence a condition variable plus an explicit flag rather than a semaphore or an eventfd, and
    // notify_all rather than notify_one. Getting this wrong gives intermittent hangs.

    namespace
    {
        struct SyncEventState
        {
            std::mutex                  m_mutex;
            std::condition_variable     m_conditionVariable;
            bool                        m_isSignalled = false;
        };
    }

    SyncEvent::SyncEvent()
        : m_pNativeHandle( nullptr )
    {
        m_pNativeHandle = EE::New<SyncEventState>();
        EE_ASSERT( m_pNativeHandle != nullptr );
    }

    SyncEvent::~SyncEvent()
    {
        if ( m_pNativeHandle != nullptr )
        {
            auto pState = reinterpret_cast<SyncEventState*>( m_pNativeHandle );
            EE::Delete( pState );
            m_pNativeHandle = nullptr;
        }
    }

    void SyncEvent::Signal()
    {
        EE_ASSERT( m_pNativeHandle != nullptr );
        auto pState = reinterpret_cast<SyncEventState*>( m_pNativeHandle );

        {
            std::unique_lock<std::mutex> lock( pState->m_mutex );
            pState->m_isSignalled = true;
        }

        pState->m_conditionVariable.notify_all();
    }

    void SyncEvent::Reset()
    {
        EE_ASSERT( m_pNativeHandle != nullptr );
        auto pState = reinterpret_cast<SyncEventState*>( m_pNativeHandle );

        std::unique_lock<std::mutex> lock( pState->m_mutex );
        pState->m_isSignalled = false;
    }

    void SyncEvent::Wait() const
    {
        EE_ASSERT( m_pNativeHandle != nullptr );
        auto pState = reinterpret_cast<SyncEventState*>( m_pNativeHandle );

        std::unique_lock<std::mutex> lock( pState->m_mutex );
        pState->m_conditionVariable.wait( lock, [pState] () { return pState->m_isSignalled; } );
    }

    void SyncEvent::Wait( Milliseconds maxWaitTime ) const
    {
        EE_ASSERT( m_pNativeHandle != nullptr );
        auto pState = reinterpret_cast<SyncEventState*>( m_pNativeHandle );

        std::unique_lock<std::mutex> lock( pState->m_mutex );
        pState->m_conditionVariable.wait_for( lock, std::chrono::milliseconds( (int64_t) (float) maxWaitTime ),
                                              [pState] () { return pState->m_isSignalled; } );
    }
}
#endif
