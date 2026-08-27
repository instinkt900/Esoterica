#pragma once

//-------------------------------------------------------------------------

#if EE_DLL
    #if defined( __linux__ )
        #define EE_ENGINETOOLS_API __attribute__(( visibility( "default" ) ))
    #elif defined( ESOTERICA_ENGINE_TOOLS )
        #define EE_ENGINETOOLS_API __declspec(dllexport)
    #else
        #define EE_ENGINETOOLS_API __declspec(dllimport)
    #endif
#else
    #define EE_ENGINETOOLS_API
#endif