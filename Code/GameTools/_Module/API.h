#pragma once

//-------------------------------------------------------------------------

#if EE_DLL
    #if defined( __linux__ )
        #define EE_GAMETOOLS_API __attribute__(( visibility( "default" ) ))
    #elif defined( ESOTERICA_GAME_TOOLS )
        #define EE_GAMETOOLS_API __declspec(dllexport)
    #else
        #define EE_GAMETOOLS_API __declspec(dllimport)
    #endif
#else
    #define EE_GAMETOOLS_API
#endif