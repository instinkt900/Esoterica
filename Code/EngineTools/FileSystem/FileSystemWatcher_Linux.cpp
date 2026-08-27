#ifdef __linux__
#include "FileSystemWatcher.h"
#include "Base/Types/HashMap.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

//-------------------------------------------------------------------------
// inotify file system watcher
//-------------------------------------------------------------------------
// The Linux counterpart to FileSystemWatcher.cpp. The header is shared and its members are
// deliberately platform-neutral, so this file reuses them rather than changing the layout:
//
//   m_pDirectoryHandle    the inotify fd, encoded. See "Handle encoding" below.
//   m_pOverlappedEvent    the watch-descriptor to directory map
//   m_pResultBuffer       the inotify read buffer
//   m_numBytesReturned    bytes in that buffer
//   m_requestPending      unused; inotify has no in-flight request to track
//
//-------------------------------------------------------------------------
// Handle encoding
//-------------------------------------------------------------------------
// IsWatching() is an inline in the shared header that tests `m_pDirectoryHandle != nullptr`, and
// inotify_init1 can legitimately return fd 0. Storing the raw fd would make IsWatching() report
// false for a perfectly good watcher.
//
// So the fd is stored **offset by one**: (void*)( intptr_t )( fd + 1 ), and 1 is subtracted on
// every use. Nothing else in this file may touch m_pDirectoryHandle directly.
//-------------------------------------------------------------------------
// Recursion
//-------------------------------------------------------------------------
// ReadDirectoryChangesW watches a whole tree with one call. **inotify does not**: it watches a
// single directory, so every subdirectory needs its own watch, and the set has to be maintained
// as directories are created, deleted and renamed. That bookkeeping is most of this file.
//-------------------------------------------------------------------------

namespace EE::FileSystem
{
    namespace
    {
        // A single read has to hold a burst of events. Each is sizeof( inotify_event ) plus a
        // variable-length name, so this is sized for roughly a few hundred events.
        constexpr static size_t const g_resultBufferSize = 64 * 1024;

        // Maps an inotify watch descriptor to the directory it watches, so an event's name can
        // be resolved to a full path. inotify only reports the name relative to the watch.
        using WatchMap = THashMap<int32_t, FileSystem::Path>;

        WatchMap* GetWatchMap( void* pOverlappedEvent )
        {
            return reinterpret_cast<WatchMap*>( pOverlappedEvent );
        }

        int32_t GetNotifyFileDescriptor( void* pDirectoryHandle )
        {
            // See "Handle encoding" above.
            return (int32_t) ( (intptr_t) pDirectoryHandle - 1 );
        }

        void* EncodeNotifyFileDescriptor( int32_t fileDescriptor )
        {
            return (void*) (intptr_t) ( fileDescriptor + 1 );
        }

        constexpr static uint32_t const g_watchedEvents =
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO;

        //-------------------------------------------------------------------------

        // Free functions rather than members: the shared header is one line different from
        // upstream and must stay that way, so this file cannot add to the class.

        bool AddWatchRecursive( int32_t notifyFileDescriptor, WatchMap& watchMap, FileSystem::Path const& directoryPath )
        {
            int32_t const watchDescriptor = inotify_add_watch( notifyFileDescriptor, directoryPath.c_str(), g_watchedEvents );

            if ( watchDescriptor < 0 )
            {
                if ( errno == ENOSPC )
                {
                    // Silently watching only part of the tree would produce missed events that
                    // look like engine bugs, so this names the limit and the sysctl to raise.
                    EE_LOG_ERROR( LogCategory::FileSystem, "File System Watcher",
                                  "Ran out of inotify watches while watching %s. Raise the limit: "
                                  "sysctl fs.inotify.max_user_watches (see /proc/sys/fs/inotify/max_user_watches)",
                                  directoryPath.c_str() );
                    return false;
                }

                EE_LOG_ERROR( LogCategory::FileSystem, "File System Watcher", "Failed to watch %s: %s",
                              directoryPath.c_str(), strerror( errno ) );
                return false;
            }

            watchMap[watchDescriptor] = directoryPath;

            // inotify watches one directory, not a tree, so descend.
            DIR* pDirectory = opendir( directoryPath.c_str() );
            if ( pDirectory == nullptr )
            {
                return true; // Disappeared underneath us, which is not an error
            }

            bool result = true;

            while ( dirent const* pEntry = readdir( pDirectory ) )
            {
                if ( pEntry->d_type != DT_DIR )
                {
                    continue;
                }

                if ( strcmp( pEntry->d_name, "." ) == 0 || strcmp( pEntry->d_name, ".." ) == 0 )
                {
                    continue;
                }

                FileSystem::Path subdirectoryPath = directoryPath;
                subdirectoryPath.Append( pEntry->d_name, true );

                if ( !AddWatchRecursive( notifyFileDescriptor, watchMap, subdirectoryPath ) )
                {
                    result = false;
                    break;
                }
            }

            closedir( pDirectory );
            return result;
        }

        void RemoveWatchRecursive( int32_t notifyFileDescriptor, WatchMap& watchMap, FileSystem::Path const& directoryPath )
        {
            String const prefix = directoryPath.GetString();

            // Drop the directory and everything beneath it. The kernel removes watches for
            // deleted directories itself, so a failed inotify_rm_watch here is expected.
            for ( auto iter = watchMap.begin(); iter != watchMap.end(); )
            {
                if ( iter->second.GetString().substr( 0, prefix.length() ) == prefix )
                {
                    inotify_rm_watch( notifyFileDescriptor, iter->first );
                    iter = watchMap.erase( iter );
                }
                else
                {
                    ++iter;
                }
            }
        }
    }

    //-------------------------------------------------------------------------

    Watcher::Watcher()
    {}

    Watcher::~Watcher()
    {
        EE_ASSERT( m_pDirectoryHandle == nullptr );
        StopWatching();
    }

    //-------------------------------------------------------------------------

    bool Watcher::StartWatching( Path const& directoryToWatch )
    {
        EE_ASSERT( directoryToWatch.IsValid() && directoryToWatch.IsDirectoryPath() );
        EE_ASSERT( !IsWatching() );

        // IN_NONBLOCK keeps Update() from blocking, which the shared contract requires.
        int32_t const notifyFileDescriptor = inotify_init1( IN_NONBLOCK | IN_CLOEXEC );
        if ( notifyFileDescriptor < 0 )
        {
            EE_LOG_ERROR( LogCategory::FileSystem, "File System Watcher", "Failed to create inotify instance: %s", strerror( errno ) );
            return false;
        }

        m_pDirectoryHandle = EncodeNotifyFileDescriptor( notifyFileDescriptor );
        m_pOverlappedEvent = EE::New<WatchMap>();
        m_pResultBuffer = EE::NewArray<uint8_t>( g_resultBufferSize );
        m_directoryToWatch = directoryToWatch;

        if ( !AddWatchRecursive( notifyFileDescriptor, *GetWatchMap( m_pOverlappedEvent ), directoryToWatch ) )
        {
            StopWatching();
            return false;
        }

        return true;
    }

    void Watcher::StopWatching()
    {
        if ( m_pDirectoryHandle != nullptr )
        {
            close( GetNotifyFileDescriptor( m_pDirectoryHandle ) );
            m_pDirectoryHandle = nullptr;
        }

        if ( m_pOverlappedEvent != nullptr )
        {
            auto pWatchMap = GetWatchMap( m_pOverlappedEvent );
            EE::Delete( pWatchMap );
            m_pOverlappedEvent = nullptr;
        }

        if ( m_pResultBuffer != nullptr )
        {
            EE::DeleteArray( m_pResultBuffer );
            m_pResultBuffer = nullptr;
        }

        m_directoryToWatch = Path();
        m_unhandledEvents.clear();
        m_numBytesReturned = 0;
        m_requestPending = false;
    }

    //-------------------------------------------------------------------------

    bool Watcher::Update()
    {
        EE_ASSERT( IsWatching() );
        m_unhandledEvents.clear();

        int32_t const notifyFileDescriptor = GetNotifyFileDescriptor( m_pDirectoryHandle );

        // Drain everything queued. The fd is non-blocking, so read returns EAGAIN once empty.
        bool anyEventsRead = false;

        for ( ;; )
        {
            ssize_t const numBytesRead = read( notifyFileDescriptor, m_pResultBuffer, g_resultBufferSize );

            if ( numBytesRead <= 0 )
            {
                if ( numBytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK )
                {
                    EE_LOG_ERROR( LogCategory::FileSystem, "File System Watcher", "inotify read failed: %s", strerror( errno ) );
                }
                break;
            }

            anyEventsRead = true;
            m_numBytesReturned = (unsigned long) numBytesRead;
            ProcessListOfDirectoryChanges();
        }

        return anyEventsRead && !m_unhandledEvents.empty();
    }

    //-------------------------------------------------------------------------

    void Watcher::RequestListOfDirectoryChanges()
    {
        // Nothing to do. The Win32 version issues an asynchronous ReadDirectoryChangesW here and
        // polls it in Update(). inotify queues events in the kernel, so Update() simply reads.
    }

    void Watcher::ProcessListOfDirectoryChanges()
    {
        auto pWatchMap = GetWatchMap( m_pOverlappedEvent );

        // Pairs IN_MOVED_FROM with the IN_MOVED_TO that shares its cookie, which is how inotify
        // expresses a rename. The two arrive in the same read in practice, but not necessarily
        // adjacently, so the source path is held until its partner appears.
        THashMap<uint32_t, FileSystem::Path> pendingMoves;
        THashMap<uint32_t, bool> pendingMoveIsDirectory;

        size_t offset = 0;
        while ( offset < (size_t) m_numBytesReturned )
        {
            auto pEvent = reinterpret_cast<inotify_event const*>( m_pResultBuffer + offset );
            offset += sizeof( inotify_event ) + pEvent->len;

            // The kernel dropped events because the queue filled. The caller has to rescan by
            // hand; reporting anything more specific would be a lie.
            if ( pEvent->mask & IN_Q_OVERFLOW )
            {
                m_massiveChangeDetectedEvent.Execute();
                continue;
            }

            if ( pEvent->len == 0 )
            {
                continue; // An event about the watched directory itself, not its contents
            }

            auto watchIter = pWatchMap->find( pEvent->wd );
            if ( watchIter == pWatchMap->end() )
            {
                continue; // A watch removed since this event was queued
            }

            bool const isDirectory = ( pEvent->mask & IN_ISDIR ) != 0;

            FileSystem::Path fullPath = watchIter->second;
            fullPath.Append( pEvent->name, isDirectory );

            //-------------------------------------------------------------------------

            if ( pEvent->mask & IN_MOVED_FROM )
            {
                pendingMoves[pEvent->cookie] = fullPath;
                pendingMoveIsDirectory[pEvent->cookie] = isDirectory;

                if ( isDirectory )
                {
                    RemoveWatchRecursive( GetNotifyFileDescriptor( m_pDirectoryHandle ), *pWatchMap, fullPath );
                }
                continue;
            }

            if ( pEvent->mask & IN_MOVED_TO )
            {
                auto moveIter = pendingMoves.find( pEvent->cookie );
                if ( moveIter != pendingMoves.end() )
                {
                    Event renameEvent;
                    renameEvent.m_type = isDirectory ? Event::DirectoryRenamed : Event::FileRenamed;
                    renameEvent.m_path = fullPath;
                    renameEvent.m_oldPath = moveIter->second;
                    m_unhandledEvents.emplace_back( renameEvent );

                    pendingMoves.erase( moveIter );
                    pendingMoveIsDirectory.erase( pEvent->cookie );
                }
                else // Moved in from outside the watched tree, so it is a creation as far as we know
                {
                    Event createEvent;
                    createEvent.m_type = isDirectory ? Event::DirectoryCreated : Event::FileCreated;
                    createEvent.m_path = fullPath;
                    m_unhandledEvents.emplace_back( createEvent );
                }

                if ( isDirectory )
                {
                    AddWatchRecursive( GetNotifyFileDescriptor( m_pDirectoryHandle ), *pWatchMap, fullPath );
                }
                continue;
            }

            //-------------------------------------------------------------------------

            Event newEvent;
            newEvent.m_path = fullPath;

            if ( pEvent->mask & IN_CREATE )
            {
                newEvent.m_type = isDirectory ? Event::DirectoryCreated : Event::FileCreated;

                // A directory created after StartWatching still has to be watched, and its
                // contents may already exist if it was created and populated quickly.
                if ( isDirectory )
                {
                    AddWatchRecursive( GetNotifyFileDescriptor( m_pDirectoryHandle ), *pWatchMap, fullPath );
                }
            }
            else if ( pEvent->mask & IN_DELETE )
            {
                newEvent.m_type = isDirectory ? Event::DirectoryDeleted : Event::FileDeleted;

                if ( isDirectory )
                {
                    RemoveWatchRecursive( GetNotifyFileDescriptor( m_pDirectoryHandle ), *pWatchMap, fullPath );
                }
            }
            else if ( pEvent->mask & ( IN_CLOSE_WRITE | IN_MODIFY ) )
            {
                newEvent.m_type = isDirectory ? Event::DirectoryModified : Event::FileModified;
            }
            else
            {
                continue;
            }

            // The Win32 implementation batches, because ReadDirectoryChangesW reports several
            // events for one logical operation. inotify has the same problem in a different
            // shape: a single write produces IN_MODIFY and then IN_CLOSE_WRITE. Collapsing
            // duplicates of the same type and path within one Update is what matches Win32's
            // observable behaviour.
            bool isDuplicate = false;
            for ( auto const& existingEvent : m_unhandledEvents )
            {
                if ( existingEvent.m_type == newEvent.m_type && existingEvent.m_path == newEvent.m_path )
                {
                    isDuplicate = true;
                    break;
                }
            }

            if ( !isDuplicate )
            {
                m_unhandledEvents.emplace_back( newEvent );
            }
        }

        // An IN_MOVED_FROM with no partner means the entry left the watched tree entirely.
        for ( auto const& pendingMove : pendingMoves )
        {
            Event deleteEvent;
            deleteEvent.m_type = pendingMoveIsDirectory[pendingMove.first] ? Event::DirectoryDeleted : Event::FileDeleted;
            deleteEvent.m_path = pendingMove.second;
            m_unhandledEvents.emplace_back( deleteEvent );
        }

        m_numBytesReturned = 0;
    }
}
#endif
