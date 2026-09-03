#pragma once
#include "Base/Esoterica.h"

namespace EE::Math
{
    EE_FORCE_INLINE uint32_t GetMostSignificantBit( uint64_t value )
    {
        // The intrinsic produces an undefined value if the input is 0, so we need to handle it explicitly
        if ( value == 0 )
        {
            return 0;
        }

        //-------------------------------------------------------------------------

        // Deliberately different from Math_Win32.h, which casts to unsigned long before the scan and
        // so truncates every value above 2^32. That is an upstream bug. This is correct for the full
        // 64-bit range. Do not "fix" it to match Win32.
        return 63u - (uint32_t) __builtin_clzll( value );
    }
}
