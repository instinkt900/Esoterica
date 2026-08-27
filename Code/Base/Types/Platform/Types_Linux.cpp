#ifdef __linux__
#include "../UUID.h"
#include <string.h>
#include <sys/random.h>

//-------------------------------------------------------------------------

namespace EE
{
    UUID UUID::GenerateID()
    {
        // Win32 uses CoCreateGuid. The engine only needs uniqueness, not any of the structure a
        // real GUID carries, so this generates a random RFC 4122 version 4 UUID from getrandom
        // rather than taking a libuuid dependency for it.
        static_assert( sizeof( UUID ) == 16, "UUIDs are expected to be 16 bytes" );

        UUID newID;
        uint8_t* pBytes = (uint8_t*) &newID;

        size_t numBytesRead = 0;
        while ( numBytesRead < sizeof( UUID ) )
        {
            ssize_t const result = getrandom( pBytes + numBytesRead, sizeof( UUID ) - numBytesRead, 0 );
            if ( result <= 0 )
            {
                continue; // Only fails on interruption, so retry
            }
            numBytesRead += (size_t) result;
        }

        // Stamp the version and variant fields, so the result is a well-formed v4 UUID
        pBytes[6] = (uint8_t) ( ( pBytes[6] & 0x0F ) | 0x40 );
        pBytes[8] = (uint8_t) ( ( pBytes[8] & 0x3F ) | 0x80 );

        return newID;
    }

    //-------------------------------------------------------------------------

    namespace StringUtils
    {
        int32_t CompareInsensitive( char const* pStr0, char const* pStr1 )
        {
            return strcasecmp( pStr0, pStr1 );
        }

        int32_t CompareInsensitive( char const* pStr0, char const* pStr1, size_t n )
        {
            return strncasecmp( pStr0, pStr1, n );
        }
    }
}
#endif
