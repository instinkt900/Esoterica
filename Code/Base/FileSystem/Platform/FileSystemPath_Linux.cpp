#ifdef __linux__
#include "../FileSystemPath.h"
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

//-------------------------------------------------------------------------

namespace EE::FileSystem
{
    char const Path::s_pathDelimiter = '/';
    constexpr static size_t const g_maxPathBufferLength = 1024;

    //-------------------------------------------------------------------------

    void Path::EnsureCorrectPathStringFormat()
    {
        struct stat pathInfo;
        if ( stat( m_fullpath.c_str(), &pathInfo ) != 0 )
        {
            return;
        }

        bool const isPathADirectory = S_ISDIR( pathInfo.st_mode );

        //-------------------------------------------------------------------------

        // Add trailing delimiter for directories
        if ( isPathADirectory && !IsDirectoryPath() )
        {
            m_fullpath += s_pathDelimiter;
            UpdatePathInternals();
        }

        // Remove trailing delimiter for files
        else if ( !isPathADirectory && IsDirectoryPath() )
        {
            m_fullpath.pop_back();
            UpdatePathInternals();
        }
    }

    bool Path::GetFullPathString( char const* pPath, String& outPath )
    {
        if ( pPath != nullptr && pPath[0] != 0 )
        {
            // Warning: this function is slow, so use sparingly
            char workingBuffer[PATH_MAX];

            // realpath resolves symlinks and requires the path to exist, which GetFullPathNameA
            // does not. Fall back to the input when it fails, so a path being built up for a
            // file that does not exist yet still normalises.
            if ( realpath( pPath, workingBuffer ) == nullptr )
            {
                outPath = pPath;
            }
            else
            {
                outPath = workingBuffer;
            }

            // Ensure directory paths have the final slash appended
            struct stat pathInfo;
            if ( stat( outPath.c_str(), &pathInfo ) == 0 && S_ISDIR( pathInfo.st_mode ) )
            {
                if ( outPath.empty() || outPath.back() != s_pathDelimiter )
                {
                    outPath += s_pathDelimiter;
                }
            }

            return true;
        }

        outPath.clear();
        return false;
    }

    bool Path::GetCorrectCaseForPath( char const* pPath, String& outPath )
    {
        // This is a Windows concept. Linux filesystems are case sensitive, so the correct case
        // for a path *is* the case it was given in: if it were wrong, the file would not exist.
        //
        // Emulating case-insensitive resolution would mean walking every directory component
        // and comparing entries, which is slow, wrong on a case-insensitive mount, and only
        // ever hides a real bug in the caller.
        outPath = pPath;

        struct stat pathInfo;
        return stat( pPath, &pathInfo ) == 0;
    }
}
#endif
