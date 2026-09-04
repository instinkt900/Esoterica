#ifdef __linux__
#include "../FileSystem.h"
#include "Base/Platform/PlatformUtils_Linux.h"
#include "Base/Encoding/Hash.h"
#include "Base/Math/Math.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

//-------------------------------------------------------------------------

namespace EE::FileSystem
{
    Path GetCurrentProcessPath()
    {
        return Path( EE::Platform::Linux::GetCurrentModulePath() ).GetParentDirectory();
    }

    bool Exists( char const* pPath )
    {
        struct stat pathInfo;
        return stat( pPath, &pathInfo ) == 0;
    }

    bool IsReadOnly( char const* pPath )
    {
        struct stat pathInfo;
        if ( stat( pPath, &pathInfo ) != 0 )
        {
            return false;
        }

        // Win32 reads the FILE_ATTRIBUTE_READONLY bit. The nearest thing here is whether this
        // process may write to the path, which is what every caller actually wants to know.
        return access( pPath, W_OK ) != 0;
    }

    bool IsExistingFile( char const* pPath )
    {
        struct stat pathInfo;
        return stat( pPath, &pathInfo ) == 0 && !S_ISDIR( pathInfo.st_mode );
    }

    bool IsExistingDirectory( char const* pPath )
    {
        struct stat pathInfo;
        return stat( pPath, &pathInfo ) == 0 && S_ISDIR( pathInfo.st_mode );
    }

    bool IsFileReadOnly( char const* pPath )
    {
        struct stat pathInfo;
        if ( stat( pPath, &pathInfo ) != 0 || S_ISDIR( pathInfo.st_mode ) )
        {
            return false;
        }

        return access( pPath, W_OK ) != 0;
    }

    uint64_t GetFileModifiedTime( char const* path )
    {
        // The return value is opaque: callers only compare it for equality, to decide whether a
        // source file changed, so it need not match the Win32 FILETIME.
        //
        // Nanoseconds since the epoch, because it is the finest granularity the filesystem offers.
        // Seconds would let two edits inside one second look identical, which shows up as a resource
        // that silently fails to recompile.
        struct stat fileInfo;
        if ( stat( path, &fileInfo ) != 0 )
        {
            return 0;
        }

        return ( (uint64_t) fileInfo.st_mtim.tv_sec * 1000000000ull ) + (uint64_t) fileInfo.st_mtim.tv_nsec;
    }

    // Data paths are lowercase by design, so a path derived from one is written back out in that
    // spelling. Reads survive it because they go through Path::GetCorrectCaseForPath, which walks an
    // existing path. A file being created cannot use that directly - its own name is not on disk yet
    // and the walk fails on the last component - so only the parent directory is resolved here and
    // the filename is kept verbatim.
    //
    // Returns false when there is nothing to correct, which includes a parent directory that really
    // does not exist. Creating it is deliberately not done here: a missing directory is a caller
    // error on both platforms, and guessing would put a second lowercase copy of a data directory
    // beside the real one.
    static bool TryResolveParentDirectoryCase( char const* pPath, String& outPath )
    {
        String const requestedPath( pPath );
        size_t const lastDelimiterIdx = requestedPath.rfind( Path::s_pathDelimiter );
        if ( lastDelimiterIdx == String::npos )
        {
            return false;
        }

        // Keep the trailing delimiter: GetCorrectCaseForPath preserves it, and it is what marks the
        // string as a directory path.
        String resolvedParentDirectory;
        if ( !Path::GetCorrectCaseForPath( requestedPath.substr( 0, lastDelimiterIdx + 1 ).c_str(), resolvedParentDirectory ) )
        {
            return false;
        }

        outPath = resolvedParentDirectory + requestedPath.substr( lastDelimiterIdx + 1 );
        return outPath != requestedPath;
    }

    bool WriteFileToDisk( char const* pPath, void const* pData, size_t size, bool overwrite = true, bool flushToDisk = false )
    {
        int const creationFlags = overwrite ? ( O_CREAT | O_TRUNC ) : ( O_CREAT | O_EXCL );
        int fileDescriptor = open( pPath, O_WRONLY | creationFlags, 0644 );

        // ENOENT means a directory component is missing, which may only be a case mismatch
        String correctedPath;
        if ( fileDescriptor < 0 && errno == ENOENT && TryResolveParentDirectoryCase( pPath, correctedPath ) )
        {
            fileDescriptor = open( correctedPath.c_str(), O_WRONLY | creationFlags, 0644 );
        }

        if ( fileDescriptor < 0 )
        {
            String const errorString = Platform::Linux::GetLastErrorMessage();
            EE_LOG_ERROR( LogCategory::FileSystem, "WriteFile", "Failed to open file handle for write: %s, Error: %s", pPath, errorString.c_str() );
            return false;
        }

        bool success = true;
        uint8_t const* pWritePtr = static_cast<uint8_t const*>( pData );
        size_t remainingBytes = size;

        while ( remainingBytes > 0 )
        {
            ssize_t const bytesWritten = write( fileDescriptor, pWritePtr, remainingBytes );
            if ( bytesWritten <= 0 )
            {
                String const errorString = Platform::Linux::GetLastErrorMessage();
                EE_LOG_ERROR( LogCategory::FileSystem, "WriteFile", "Failed to write file: %s, Error: %s", pPath, errorString.c_str() );
                success = false;
                break;
            }

            pWritePtr += bytesWritten;
            remainingBytes -= (size_t) bytesWritten;
        }

        if ( success && flushToDisk )
        {
            success = ( fsync( fileDescriptor ) == 0 );
        }

        close( fileDescriptor );
        return success;
    }

    bool ReadBinaryFile( char const* pPath, Blob& fileData )
    {
        EE_ASSERT( pPath != nullptr );

        // Open file handle
        int const fileDescriptor = open( pPath, O_RDONLY );
        if ( fileDescriptor < 0 )
        {
            String const errorString = Platform::Linux::GetLastErrorMessage();
            EE_LOG_ERROR( LogCategory::FileSystem, "ReadBinaryFile", "Failed to open file handle for read: %s, Error: %s", pPath, errorString.c_str() );
            return false;
        }

        // Get file size
        struct stat fileInfo;
        if ( fstat( fileDescriptor, &fileInfo ) != 0 )
        {
            String const errorString = Platform::Linux::GetLastErrorMessage();
            EE_LOG_ERROR( LogCategory::FileSystem, "ReadBinaryFile", "Failed to get file size for: %s, Error: %s", pPath, errorString.c_str() );
            close( fileDescriptor );
            return false;
        }

        // Allocate destination memory
        size_t const fileSize = (size_t) fileInfo.st_size;
        fileData.resize( fileSize );

        // Read file
        size_t remainingBytesToRead = fileSize;
        uint8_t* pBuffer = fileData.data();

        while ( remainingBytesToRead != 0 )
        {
            ssize_t const bytesRead = read( fileDescriptor, pBuffer, remainingBytesToRead );
            if ( bytesRead <= 0 )
            {
                fileData.clear();
                String const errorString = Platform::Linux::GetLastErrorMessage();
                EE_LOG_ERROR( LogCategory::FileSystem, "ReadBinaryFile", "Failed to get read from binary file: %s, Error: %s", pPath, errorString.c_str() );
                close( fileDescriptor );
                return false;
            }

            pBuffer += bytesRead;
            remainingBytesToRead -= (size_t) bytesRead;
        }

        close( fileDescriptor );
        return true;
    }
}
#endif
