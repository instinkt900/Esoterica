#pragma once

//-------------------------------------------------------------------------

#if EE_DLL
    #if defined( __linux__ )
        #define EE_BASE_API __attribute__(( visibility( "default" ) ))
    #elif ESOTERICA_BASE
        #define EE_BASE_API __declspec(dllexport)
    #else
        #define EE_BASE_API __declspec(dllimport)
    #endif
#else
    #define EE_BASE_API
#endif