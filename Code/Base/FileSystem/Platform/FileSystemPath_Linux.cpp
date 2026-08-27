#ifdef __linux__
#include "../FileSystemPath.h"
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
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
        // The fast path, and the only one that runs for paths the engine produced itself.
        outPath = pPath;

        struct stat pathInfo;
        if ( stat( pPath, &pathInfo ) == 0 )
        {
            return true;
        }

        // Nothing is there under that spelling, so walk the path a component at a time and look
        // for an entry that differs only in case.
        //
        // On Windows this function asks the filesystem for a path's canonical case. Here it is
        // doing something subtly different: recovering the real path when the caller was given
        // the wrong case. That matters because several .vcxproj entries disagree with the disk,
        // for example ThirdParty\enkits against ThirdParty/EnkiTS, and MSBuild never notices.
        // The Reflector reads those paths directly.
        String const requestedPath( pPath );
        String resolvedPath;
        resolvedPath.reserve( requestedPath.length() );

        size_t componentStart = 0;
        if ( !requestedPath.empty() && requestedPath[0] == s_pathDelimiter )
        {
            resolvedPath += s_pathDelimiter;
            componentStart = 1;
        }

        while ( componentStart <= requestedPath.length() )
        {
            size_t componentEnd = requestedPath.find( s_pathDelimiter, componentStart );
            if ( componentEnd == String::npos )
            {
                componentEnd = requestedPath.length();
            }

            String const component = requestedPath.substr( componentStart, componentEnd - componentStart );
            if ( component.empty() )
            {
                break;
            }

            String candidate = resolvedPath + component;

            if ( stat( candidate.c_str(), &pathInfo ) != 0 )
            {
                // Search the parent directory for an entry matching case-insensitively.
                String const searchDirectory = resolvedPath.empty() ? String( "." ) : resolvedPath;
                DIR* pDirectory = opendir( searchDirectory.c_str() );
                if ( pDirectory == nullptr )
                {
                    return false;
                }

                bool found = false;
                while ( dirent const* pEntry = readdir( pDirectory ) )
                {
                    if ( strcasecmp( pEntry->d_name, component.c_str() ) == 0 )
                    {
                        candidate = resolvedPath + pEntry->d_name;
                        found = true;
                        break;
                    }
                }

                closedir( pDirectory );

                if ( !found )
                {
                    return false;
                }
            }

            resolvedPath = candidate;

            if ( componentEnd == requestedPath.length() )
            {
                break;
            }

            resolvedPath += s_pathDelimiter;
            componentStart = componentEnd + 1;
        }

        if ( stat( resolvedPath.c_str(), &pathInfo ) != 0 )
        {
            return false;
        }

        outPath = resolvedPath;
        return true;
    }
}
#endif
