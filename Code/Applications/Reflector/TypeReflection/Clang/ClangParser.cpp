#include "ClangParser.h"
#include <EASTL/algorithm.h>
#if !_WIN32
#include <dirent.h>
#endif
#include "ClangVisitors_TranslationUnit.h"
#include "Applications/Reflector/TypeReflection/TypeReflection_ReflectionDatabase.h"
#include "Base/Time/Timers.h"
#if _WIN32
#include "Base/Platform/PlatformUtils_Win32.h"
#elif defined( __linux__ )
#include "Base/Platform/PlatformUtils_Linux.h"
#endif
#include "Base/FileSystem/FileSystemUtils.h"
#include <fstream>

//-------------------------------------------------------------------------

namespace EE::Reflection
{
    static char const* const g_includePaths[] =
    {
        "Code\\",
        "Code\\Base\\ThirdParty\\EA\\EABase\\include\\common\\",
        "Code\\Base\\ThirdParty\\EA\\EASTL\\include\\",
        "Code\\Base\\ThirdParty\\",
        "Code\\Base\\ThirdParty\\imgui\\",
        "External\\Optick\\include\\",
        #if EE_ENABLE_NAVPOWER
        "External\\NavPower\\include\\"
        #endif
    };

    constexpr static int const g_numIncludePaths = sizeof( g_includePaths ) / sizeof( g_includePaths[0] );

    //-------------------------------------------------------------------------

    ClangParser::ClangParser( FileSystem::Path const& solutionDirectoryPath, ReflectionDatabase* pDatabase )
        : m_context( solutionDirectoryPath, pDatabase )
        , m_totalParsingTime( 0 )
        , m_totalVisitingTime( 0 )
        , m_reflectionDataPath( solutionDirectoryPath.GetAppended( Settings::g_buildTempFolderPath, true ) )
    {}

    bool ClangParser::Parse( TVector<ReflectedHeader*> const& headers, Pass pass )
    {
        m_context.m_detectDevOnlyTypesAndProperties = ( pass == NoDevToolsPass );

        // Create single amalgamated header file for all headers to parse
        //-------------------------------------------------------------------------

        std::ofstream reflectorFileStream;
        FileSystem::Path const reflectorHeader = m_reflectionDataPath + "Reflector.h";
        reflectorHeader.EnsureDirectoryExists();
        reflectorFileStream.open( reflectorHeader.c_str(), std::ios::out | std::ios::trunc );
        EE_ASSERT( !reflectorFileStream.fail() );

        String includeStr;
        m_context.m_headersToVisit.clear();
        for ( ReflectedHeader const* pHeader : headers )
        {
            EE_ASSERT( pHeader->m_path.IsValid() && pHeader->m_path.IsFilePath() );

            // Exclude dev tools
            if ( pass == NoDevToolsPass && pHeader->m_isToolsOnlyHeader )
            {
                continue;
            }

            m_context.m_headersToVisit.emplace_back( pHeader->m_ID, pHeader );
            includeStr += "#include \"" + pHeader->m_path.GetString() + "\"\n";
        }

        reflectorFileStream.write( includeStr.c_str(), includeStr.size() );
        reflectorFileStream.close();

        // Clang args
        TInlineVector<String, 10> fullIncludePaths;
        TInlineVector<char const*, 10> clangArgs;
        for ( auto i = 0; i < g_numIncludePaths; i++ )
        {
            String fullPath = m_context.m_solutionDirectoryPath.GetString() + g_includePaths[i];

            #if !_WIN32
            // g_includePaths is written with Windows separators, and two of its entries have
            // the wrong case for a case-sensitive filesystem ("EABase\include\common" against
            // EABase/include/Common, and "EASTL\include" against EASTL/Include). Normalise the
            // separators, then recover the real case from disk.
            eastl::replace( fullPath.begin(), fullPath.end(), '\\', '/' );

            String correctlyCasedPath;
            if ( FileSystem::Path::GetCorrectCaseForPath( fullPath.c_str(), correctlyCasedPath ) )
            {
                fullPath = correctlyCasedPath;
            }
            #endif

            #if _WIN32
            String const shortPath = Platform::Win32::GetShortPath( fullPath );
            #else
            String const shortPath = Platform::Linux::GetShortPath( fullPath );
            #endif
            fullIncludePaths.push_back( "-I" + shortPath );
            clangArgs.push_back( fullIncludePaths.back().c_str() );

            if ( !FileSystem::Exists( fullPath ) )
            {
                m_context.LogError( "Invalid include path: %s", fullPath.c_str() );
                return false;
            }
        }

        #if !_WIN32
        // libclang normally locates its own builtin headers - stddef.h, stdarg.h and the rest -
        // relative to the clang executable. The Reflector links libclang directly, so there is
        // no executable to derive it from and the resource directory has to be given.
        //
        // On Windows this does not arise: the MSVC toolchain headers are found through the
        // registry and the environment instead.
        String resourceDirArg;
        {
            String const clangLibraryRoot = m_context.m_solutionDirectoryPath.GetString() + "External/LLVM/lib/clang";

            // One versioned directory lives here, and its name changes with the pinned LLVM, so
            // it is discovered rather than written down.
            if ( DIR* pDirectory = opendir( clangLibraryRoot.c_str() ) )
            {
                while ( dirent const* pEntry = readdir( pDirectory ) )
                {
                    if ( pEntry->d_name[0] != '.' )
                    {
                        resourceDirArg = "-resource-dir=" + clangLibraryRoot + "/" + pEntry->d_name;
                        break;
                    }
                }

                closedir( pDirectory );
            }

            if ( resourceDirArg.empty() )
            {
                m_context.LogError( "Could not find clang's resource directory under %s", clangLibraryRoot.c_str() );
                return false;
            }

            clangArgs.push_back( resourceDirArg.c_str() );
        }
        #endif

        clangArgs.push_back( "-x" );
        clangArgs.push_back( "c++" );
        clangArgs.push_back( "-std=c++20" );
        clangArgs.push_back( "-O0" );
        clangArgs.push_back( "-D NDEBUG" );
        clangArgs.push_back( "-Werror" );
        clangArgs.push_back( "-Wno-multichar" );
        clangArgs.push_back( "-Wno-deprecated-builtins" );
        clangArgs.push_back( "-fparse-all-comments" );
        #if _WIN32
        // MSVC emulation, so libclang can parse the Windows toolchain headers and the
        // __declspec that _Module/API.h expands to there.
        //
        // Off on Linux, and it has to be: -fms-compatibility makes clang behave enough like
        // MSVC to break glibc's headers, which show up as "__STRICT_ANSI__ seems to have been
        // undefined" followed by char16_t and char32_t going undeclared. Nothing needs it here,
        // because API.h takes its visibility( "default" ) branch on this platform.
        clangArgs.push_back( "-fms-extensions" );
        clangArgs.push_back( "-fms-compatibility" );
        #endif
        clangArgs.push_back( "-Wno-unknown-warning-option" );
        clangArgs.push_back( "-Wno-return-type-c-linkage" );
        clangArgs.push_back( "-Wno-gnu-folding-constant" );
        clangArgs.push_back( "-Wno-vla-extension-static-assert" );

        // Exclude dev tools
        if ( pass == NoDevToolsPass )
        {
            clangArgs.push_back( "-D EE_SHIPPING" );
        }

        //-------------------------------------------------------------------------

        // Set up clang
        auto idx = clang_createIndex( 0, 1 );
        uint32_t const clangOptions = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_IncludeBriefCommentsInCodeCompletion;

        // Parse Headers
        CXTranslationUnit tu;
        CXErrorCode result = CXError_Failure;
        {
            ScopedTimer<PlatformClock> timer( m_totalParsingTime );
            result = clang_parseTranslationUnit2( idx, reflectorHeader.c_str(), clangArgs.data(), clangArgs.size(), 0, 0, clangOptions, &tu );
        }

        // Handle result of parse
        if ( result == CXError_Success )
        {
            ScopedTimer<PlatformClock> timer( m_totalVisitingTime );
            m_context.Reset( &tu );
            auto cursor = clang_getTranslationUnitCursor( tu );
            clang_visitChildren( cursor, VisitTranslationUnit, &m_context );
        }
        else
        {
            switch ( result )
            {
                case CXError_Failure:
                m_context.LogError( "Clang Unknown failure" );
                break;

                case CXError_Crashed:
                m_context.LogError( "Clang crashed" );
                break;

                case CXError_InvalidArguments:
                m_context.LogError( "Clang Invalid arguments" );
                break;

                case CXError_ASTReadError:
                m_context.LogError( "Clang AST read error" );
                break;
            }
        }
        clang_disposeIndex( idx );

        //-------------------------------------------------------------------------

        // Check that we've processed all detected macros
        if ( !m_context.HasErrorOccured() )
        {
            m_context.CheckForUnhandledReflectionMacros();
        }

        // If we have an error from the parser, prepend the header to it
        if ( m_context.HasErrorOccured() )
        {
            m_context.LogError( "\n%s", m_context.GetErrorMessage() );
        }

        return !m_context.HasErrorOccured();
    }
}