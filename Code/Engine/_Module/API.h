#pragma once

//-------------------------------------------------------------------------

#if EE_DLL
    #if defined( __linux__ )
        #define EE_ENGINE_API __attribute__(( visibility( "default" ) ))
    #elif defined( ESOTERICA_ENGINE_RUNTIME )
        #define EE_ENGINE_API __declspec(dllexport)
    #else
        #define EE_ENGINE_API __declspec(dllimport)
    #endif
#else
    #define EE_ENGINE_API
#endif