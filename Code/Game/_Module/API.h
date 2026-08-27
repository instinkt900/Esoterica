#pragma once
//-------------------------------------------------------------------------

#if EE_DLL
    #if defined( __linux__ )
        #define EE_GAME_API __attribute__(( visibility( "default" ) ))
    #elif defined( ESOTERICA_GAME_RUNTIME )
        #define EE_GAME_API __declspec(dllexport)
    #else
        #define EE_GAME_API __declspec(dllimport)
    #endif
#else
    #define EE_GAME_API
#endif