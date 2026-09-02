#pragma once
#ifdef __linux__

//-------------------------------------------------------------------------

// MSVC's <stdint.h> drags in size_t and va_list, so Esoterica.h gets them for free on Windows.
// libc does not, and hundreds of headers in this codebase use both without including anything.
// Pulling them in here keeps the Esoterica.h edit to the 2-line include switch.
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>   // INT_MIN, INT_MAX, UINT_MAX
#include <math.h>     // ldexp
#include <alloca.h>   // EE_STACK_ALLOC, EE_STACK_ARRAY_ALLOC

//-------------------------------------------------------------------------

#define EE_FORCE_INLINE __attribute__(( always_inline )) inline

//-------------------------------------------------------------------------
// Microsoft CRT compatibility
//-------------------------------------------------------------------------
// A handful of platform-neutral files call the MSVC "secure" CRT functions directly. Providing
// them here keeps those files unedited, which is the whole point of Conventions rule 1.
//
// The vendored libraries under Code/**/ThirdParty also use these, but each already selects the
// portable variant behind its own _MSC_VER guard, so nothing here is for their benefit.

#include <string.h>
#include <stdio.h>

EE_FORCE_INLINE int _stricmp( char const* pLHS, char const* pRHS )
{
    return strcasecmp( pLHS, pRHS );
}

EE_FORCE_INLINE int _strnicmp( char const* pLHS, char const* pRHS, size_t count )
{
    return strncasecmp( pLHS, pRHS, count );
}

// The unprefixed spelling, which the Reflector's shader code generator uses. It is a deprecated
// POSIX name that glibc does not declare under a strict -std=c++20.
EE_FORCE_INLINE int stricmp( char const* pLHS, char const* pRHS )
{
    return strcasecmp( pLHS, pRHS );
}

// MSVC's strncpy_s always null-terminates and returns 0 on success. strncpy does neither, so
// this is a bounded copy plus an explicit terminator rather than a straight forward.
EE_FORCE_INLINE int strncpy_s( char* pDestination, size_t destinationSize, char const* pSource, size_t count )
{
    if ( pDestination == nullptr || pSource == nullptr || destinationSize == 0 )
    {
        return 22; // EINVAL, which is what the MSVC version returns
    }

    size_t const numCharsToCopy = ( count < destinationSize - 1 ) ? count : destinationSize - 1;
    memcpy( pDestination, pSource, numCharsToCopy );
    pDestination[numCharsToCopy] = 0;
    return 0;
}

// MSVC also supplies a template overload that deduces the destination size from an array, and
// the call sites use both forms.
template<size_t N>
EE_FORCE_INLINE int strncpy_s( char ( &destination )[N], char const* pSource, size_t count )
{
    return strncpy_s( destination, N, pSource, count );
}

// MSVC's strcpy_s truncates nothing - it fails if the source does not fit. Mirror that, since
// callers rely on the destination being untouched rather than silently cut short.
EE_FORCE_INLINE int strcpy_s( char* pDestination, size_t destinationSize, char const* pSource )
{
    if ( pDestination == nullptr || pSource == nullptr || destinationSize == 0 )
    {
        return 22; // EINVAL, which is what the MSVC version returns
    }

    size_t const sourceLength = strlen( pSource );
    if ( sourceLength >= destinationSize )
    {
        pDestination[0] = 0;
        return 34; // ERANGE, which is what the MSVC version returns
    }

    memcpy( pDestination, pSource, sourceLength + 1 );
    return 0;
}

// MSVC provides a template overload that deduces the size of a fixed array destination.
template<size_t N>
EE_FORCE_INLINE int strcpy_s( char ( &destination )[N], char const* pSource )
{
    return strcpy_s( destination, N, pSource );
}

EE_FORCE_INLINE int vsprintf_s( char* pBuffer, size_t bufferSize, char const* pFormat, va_list args )
{
    return vsnprintf( pBuffer, bufferSize, pFormat, args );
}

//-------------------------------------------------------------------------
// Type name compatibility
//-------------------------------------------------------------------------
// MSVC's `typeid( T ).name()` returns a human readable name - "enum EE::Physics::ObjectCategory" -
// and callers skip the leading "enum " to recover the fully qualified name the Reflector
// registered. The Itanium ABI returns the *mangled* name instead, "N2EE7Physics14ObjectCategoryE",
// so the same arithmetic yields "Physics14ObjectCategoryE" and every lookup misses.

#include <cxxabi.h>
#include <stdlib.h>

namespace EE::Platform
{
    // Writes the demangled, fully qualified name of a `typeid().name()` result into the supplied
    // buffer - "EE::Physics::ObjectCategory". Returns false and leaves the buffer untouched if the
    // name cannot be demangled.
    //
    // __cxa_demangle requires any output buffer it is handed to have come from malloc, so it always
    // allocates its own here and the result is copied out. Callers cache the result per type.
    inline bool DemangleTypeInfoName( char const* pMangledName, char* pOutBuffer, size_t outBufferSize )
    {
        int status = 0;
        char* pDemangledName = abi::__cxa_demangle( pMangledName, nullptr, nullptr, &status );
        bool const succeeded = ( status == 0 ) && ( pDemangledName != nullptr );
        if ( succeeded )
        {
            strncpy_s( pOutBuffer, outBufferSize, pDemangledName, strlen( pDemangledName ) );
        }

        free( pDemangledName );
        return succeeded;
    }
}

//-------------------------------------------------------------------------
// Dev Defines
//-------------------------------------------------------------------------

#define EE_DISABLE_OPTIMIZATION _Pragma( "clang optimize off" )
#define EE_ENABLE_OPTIMIZATION _Pragma( "clang optimize on" )

#if EE_DEVELOPMENT_TOOLS
    #define EE_DEBUG_BREAK() __builtin_debugtrap()
#endif

#endif
