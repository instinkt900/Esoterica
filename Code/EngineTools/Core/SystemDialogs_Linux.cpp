#ifdef __linux__
#include "SystemDialogs.h"
#include "Base/TypeSystem/TypeRegistry.h"
#include "Base/TypeSystem/ResourceInfo.h"
#include "ToolsContext.h"
#include "Base/FileSystem/IDataFile.h"
#include "Base/Logging/SystemLog.h"
#include "Base/Memory/Memory.h"
#include "EngineTools/ThirdParty/subprocess/subprocess.h"

#include <cstdlib>
#include <unistd.h>

//-------------------------------------------------------------------------
// Linux has no system dialog API. Every desktop ships a command line helper instead, and zenity's
// command line is the one they agree on: qarma is its Qt port and matedialog the MATE fork, and both
// take the same arguments. So whichever is on PATH gets spawned, and the selection is read off its
// stdout.
//
// With no display there is nothing to show. The file dialogs then return an empty result, which the
// caller reads as a cancel, and the message dialogs log instead. That is how the ResourceCompiler
// workers run.
//-------------------------------------------------------------------------

namespace EE
{
    static char const* const g_dialogToolNames[] = { "zenity", "qarma", "matedialog" };

    static bool FindExecutableOnPath( char const* pExecutableName, String& outPath )
    {
        char const* pPathEnvironmentVariable = getenv( "PATH" );
        if ( pPathEnvironmentVariable == nullptr )
        {
            return false;
        }

        String const searchPaths( pPathEnvironmentVariable );
        size_t startIdx = 0;
        while ( startIdx <= searchPaths.length() )
        {
            size_t const separatorIdx = searchPaths.find( ':', startIdx );
            size_t const endIdx = ( separatorIdx == String::npos ) ? searchPaths.length() : separatorIdx;

            if ( endIdx > startIdx )
            {
                String candidatePath = searchPaths.substr( startIdx, endIdx - startIdx );
                candidatePath += '/';
                candidatePath += pExecutableName;

                if ( access( candidatePath.c_str(), X_OK ) == 0 )
                {
                    outPath = candidatePath;
                    return true;
                }
            }

            if ( separatorIdx == String::npos )
            {
                break;
            }

            startIdx = separatorIdx + 1;
        }

        return false;
    }

    static String FindDialogTool()
    {
        String toolPath;

        if ( getenv( "DISPLAY" ) == nullptr && getenv( "WAYLAND_DISPLAY" ) == nullptr )
        {
            EE_LOG_WARNING( "Tools", "Dialog", "No display available, dialogs will be logged instead of shown" );
            return toolPath;
        }

        for ( char const* pToolName : g_dialogToolNames )
        {
            if ( FindExecutableOnPath( pToolName, toolPath ) )
            {
                return toolPath;
            }
        }

        EE_LOG_WARNING( "Tools", "Dialog", "No dialog tool found on PATH (zenity, qarma or matedialog), dialogs will be logged instead of shown" );
        return toolPath;
    }

    // Resolved once. A missing tool is a machine property, so re-checking it per dialog would only
    // walk PATH again to reach the same answer.
    static char const* GetDialogTool()
    {
        static String const s_toolPath = FindDialogTool();
        return s_toolPath.empty() ? nullptr : s_toolPath.c_str();
    }

    // Runs the dialog tool and blocks until the user dismisses it. Returns false if the tool could
    // not be run at all, which is different from the user cancelling.
    static bool RunDialogTool( TInlineVector<String, 8> const& args, String& outStdOut, int32_t& outExitCode )
    {
        char const* pToolPath = GetDialogTool();
        if ( pToolPath == nullptr )
        {
            return false;
        }

        // subprocess takes an argv array rather than a command line, so nothing here needs quoting
        // or escaping. Filenames with spaces and quotes in them arrive at the tool intact.
        TInlineVector<char const*, 10> argv;
        argv.emplace_back( pToolPath );
        for ( String const& arg : args )
        {
            argv.emplace_back( arg.c_str() );
        }
        argv.emplace_back( nullptr );

        subprocess_s process;
        Memory::MemsetZero( &process );
        if ( subprocess_create( argv.data(), subprocess_option_inherit_environment, &process ) != 0 )
        {
            EE_LOG_ERROR( "Tools", "Dialog", "Failed to run dialog tool: %s", pToolPath );
            return false;
        }

        // Not async: the tool writes a handful of paths and then exits, which is far below the pipe
        // buffer, so reading after the join cannot deadlock.
        if ( subprocess_join( &process, &outExitCode ) != 0 )
        {
            EE_LOG_ERROR( "Tools", "Dialog", "Failed to wait on dialog tool: %s", pToolPath );
            subprocess_destroy( &process );
            return false;
        }

        outStdOut.clear();

        if ( FILE* pStdOut = subprocess_stdout( &process ) )
        {
            char readBuffer[1024];
            size_t numBytesRead = 0;
            while ( ( numBytesRead = fread( readBuffer, 1, sizeof( readBuffer ), pStdOut ) ) > 0 )
            {
                outStdOut.append( readBuffer, numBytesRead );
            }
        }

        subprocess_destroy( &process );
        return true;
    }

    // The tool writes one path per line, and always ends the last one with a newline.
    static void ParseSelectedPaths( String const& toolOutput, FileDialog::Result& result )
    {
        size_t startIdx = 0;
        while ( startIdx < toolOutput.length() )
        {
            size_t endIdx = toolOutput.find( '\n', startIdx );
            if ( endIdx == String::npos )
            {
                endIdx = toolOutput.length();
            }

            if ( endIdx > startIdx )
            {
                result.m_filePaths.emplace_back( String( toolOutput.substr( startIdx, endIdx - startIdx ) ) );
            }

            startIdx = endIdx + 1;
        }
    }

    static void AddExtensionFilterArgs( TInlineVector<FileDialog::ExtensionFilter, 2> const& filters, TInlineVector<String, 8>& args )
    {
        for ( FileDialog::ExtensionFilter const& extFilter : filters )
        {
            if ( !extFilter.IsValid() )
            {
                continue;
            }

            // The filter argument is "--file-filter=<name> | <pattern>". m_filter holds the pattern
            // in the Windows wide-string format, so it is built from m_extension instead.
            String const displayText( String::CtorConvert(), extFilter.m_displayText.c_str() );
            args.emplace_back( String( String::CtorSprintf(), "--file-filter=%s | *.%s", displayText.c_str(), extFilter.m_extension.c_str() ) );
        }
    }

    static void AddStartingPathArg( FileSystem::Path const& startingPath, TInlineVector<String, 8>& args )
    {
        if ( startingPath.IsValid() )
        {
            // A directory path keeps its trailing slash, which is what tells the tool to open there
            // rather than to preselect a file of that name.
            args.emplace_back( String( String::CtorSprintf(), "--filename=%s", startingPath.c_str() ) );
        }
    }

    static void ValidateResult( TInlineVector<FileDialog::ExtensionFilter, 2> const& filters, FileDialog::Result& result )
    {
        // Ensure all results are files and have extensions
        //-------------------------------------------------------------------------

        // Ensure results have an extension
        for ( int32_t i = int32_t( result.m_filePaths.size() ) - 1; i >= 0; i-- )
        {
            // Remove any non-file paths
            if ( !result.m_filePaths[i].IsFilePath() )
            {
                MessageDialog::Error( "Error", "Invalid file selected: %s", result.m_filePaths[i].c_str() );
                result.m_filePaths.erase_unsorted( result.m_filePaths.begin() + i );
                continue;
            }

            // Try to fix-up missing extensions
            FileSystem::Extension const ext = result.m_filePaths[i].GetExtensionAsString();
            if ( ext.empty() )
            {
                // We dont know what the extension should be so remove the invalid result
                if ( filters.empty() )
                {
                    MessageDialog::Error( "Error", "File with no extension selected: %s", result.m_filePaths[i].c_str() );
                    result.m_filePaths.erase_unsorted( result.m_filePaths.begin() + i );
                    continue;
                }
                else // Only a single filter so we can assume what the extension should be
                {
                    result.m_filePaths[i].AppendExtension( filters[0].m_extension );
                }
            }
        }

        // Ensure all extensions are valid
        //-------------------------------------------------------------------------

        if ( !filters.empty() )
        {
            for ( int32_t i = int32_t( result.m_filePaths.size() ) - 1; i >= 0; i-- )
            {
                EE_ASSERT( result.m_filePaths[i].IsFilePath() );
                FileSystem::Extension const ext = result.m_filePaths[i].GetExtensionAsString();
                EE_ASSERT( !ext.empty() );

                bool validExtension = false;
                for ( auto const& filter : filters )
                {
                    if ( ext.comparei( filter.m_extension ) == 0 )
                    {
                        validExtension = true;
                        continue;
                    }
                }

                if ( !validExtension )
                {
                    MessageDialog::Error( "Error", "Invalid extension detected: %s", result.m_filePaths[i].c_str() );
                    result.m_filePaths.erase_unsorted( result.m_filePaths.begin() + i );
                    continue;
                }
            }
        }
    }

    //-------------------------------------------------------------------------

    // Identical to the Windows version. m_filter and m_displayText stay in the wide-string format
    // the shared header declares, so both platforms build the same value.
    FileDialog::ExtensionFilter::ExtensionFilter( FileSystem::Extension ext, String displayText )
        : m_extension( ext )
    {
        if ( !m_extension.empty() )
        {
            m_filter = WString( WString::CtorConvert(), String( String::CtorSprintf(), "*.%s", m_extension.c_str() ) );

            if ( displayText.empty() )
            {
                m_displayText = m_filter;
            }
            else
            {
                m_displayText = WString( WString::CtorConvert(), displayText );
            }
        }
    }

    //-------------------------------------------------------------------------
    // Folder Dialog
    //-------------------------------------------------------------------------

    FileDialog::Result FileDialog::SelectFolder( FileSystem::Path const& startingPath )
    {
        Result result;

        TInlineVector<String, 8> args;
        args.emplace_back( "--file-selection" );
        args.emplace_back( "--directory" );

        if ( startingPath.IsValid() )
        {
            EE_ASSERT( startingPath.IsDirectoryPath() );
        }

        AddStartingPathArg( startingPath, args );

        //-------------------------------------------------------------------------

        String toolOutput;
        int32_t exitCode = 0;
        if ( RunDialogTool( args, toolOutput, exitCode ) && exitCode == 0 )
        {
            ParseSelectedPaths( toolOutput, result );
        }

        return result;
    }

    //-------------------------------------------------------------------------
    // Load Dialog
    //-------------------------------------------------------------------------

    FileDialog::Result FileDialog::Load( TInlineVector<ExtensionFilter, 2> const& filters, String const& title, bool allowMultiselect, FileSystem::Path const& startingPath )
    {
        Result result;

        TInlineVector<String, 8> args;
        args.emplace_back( "--file-selection" );

        if ( allowMultiselect )
        {
            args.emplace_back( "--multiple" );

            // The default separator is '|', which does turn up in filenames. A newline does not.
            args.emplace_back( "--separator=\n" );
        }

        if ( !title.empty() )
        {
            args.emplace_back( String( String::CtorSprintf(), "--title=%s", title.c_str() ) );
        }

        AddStartingPathArg( startingPath, args );
        AddExtensionFilterArgs( filters, args );

        //-------------------------------------------------------------------------

        String toolOutput;
        int32_t exitCode = 0;
        if ( RunDialogTool( args, toolOutput, exitCode ) && exitCode == 0 )
        {
            ParseSelectedPaths( toolOutput, result );
        }

        ValidateResult( filters, result );

        return result;
    }

    FileDialog::Result FileDialog::LoadResourceOrDataFile( ToolsContext const* pToolsContext, FileSystem::Extension extension, FileSystem::Path const& startingPath )
    {
        EE_ASSERT( !extension.empty() );
        Result result = Load( { extension }, String(), false, startingPath );

        // Ensure that all selected files are within the source data directory
        for ( int32_t i = int32_t( result.m_filePaths.size() ) - 1; i >= 0; i-- )
        {
            if ( !result.m_filePaths[i].IsUnderDirectory( pToolsContext->GetSourceDataDirectory() ) )
            {
                MessageDialog::Error( "Error", "Selected file is not with the raw resource folder!" );
                result.m_filePaths.erase_unsorted( result.m_filePaths.begin() + i );
            }
        }

        return result;
    }

    FileDialog::Result FileDialog::LoadResourceOrDataFile( ToolsContext const* pToolsContext, TypeSystem::TypeID typeID, FileSystem::Path const& startingPath )
    {
        auto pTypeInfo = pToolsContext->m_pTypeRegistry->GetTypeInfo( typeID );
        EE_ASSERT( pTypeInfo->IsDerivedFrom( IDataFile::GetStaticTypeID() ) );
        return LoadResourceOrDataFile( pToolsContext, pTypeInfo->GetDefaultInstance<IDataFile>()->GetExtension(), startingPath );
    }

    //-------------------------------------------------------------------------
    // Save Dialog
    //-------------------------------------------------------------------------

    FileDialog::Result FileDialog::Save( TInlineVector<ExtensionFilter, 2> const& filters, String const& title, FileSystem::Path const& startingPath )
    {
        Result result;

        TInlineVector<String, 8> args;
        args.emplace_back( "--file-selection" );
        args.emplace_back( "--save" );

        // zenity 4 confirms overwrites itself and ignores this, zenity 3 needs it.
        args.emplace_back( "--confirm-overwrite" );

        if ( !title.empty() )
        {
            args.emplace_back( String( String::CtorSprintf(), "--title=%s", title.c_str() ) );
        }

        AddStartingPathArg( startingPath, args );
        AddExtensionFilterArgs( filters, args );

        //-------------------------------------------------------------------------

        String toolOutput;
        int32_t exitCode = 0;
        if ( RunDialogTool( args, toolOutput, exitCode ) && exitCode == 0 )
        {
            ParseSelectedPaths( toolOutput, result );
        }

        // The tool does not append the filter extension to a typed-in name. ValidateResult does it.
        ValidateResult( filters, result );

        return result;
    }

    FileDialog::Result FileDialog::SaveResourceOrDataFile( ToolsContext const* pToolsContext, FileSystem::Extension extension, FileSystem::Path const& startingPath )
    {
        EE_ASSERT( !extension.empty() );
        Result result = Save( { extension }, String(), startingPath );

        //-------------------------------------------------------------------------

        // Ensure that all selected files are within the source data directory
        for ( int32_t i = int32_t( result.m_filePaths.size() ) - 1; i >= 0; i-- )
        {
            if ( !result.m_filePaths[i].IsUnderDirectory( pToolsContext->GetSourceDataDirectory() ) )
            {
                MessageDialog::Error( "Error", "Selected file is not with the raw resource folder!" );
                result.m_filePaths.erase_unsorted( result.m_filePaths.begin() + i );
            }
        }

        return result;
    }

    FileDialog::Result FileDialog::SaveResourceOrDataFile( ToolsContext const* pToolsContext, TypeSystem::TypeID typeID, FileSystem::Path const& startingPath )
    {
        auto pTypeInfo = pToolsContext->m_pTypeRegistry->GetTypeInfo( typeID );
        EE_ASSERT( pTypeInfo->IsDerivedFrom( IDataFile::GetStaticTypeID() ) );
        return SaveResourceOrDataFile( pToolsContext, pTypeInfo->GetDefaultInstance<IDataFile>()->GetExtension(), startingPath );
    }

    //-------------------------------------------------------------------------
    // Message Box
    //-------------------------------------------------------------------------

    // The tool has three buttons at most: OK, Cancel, and one extra. Win32 has seven MessageBox
    // layouts, so the ones with three buttons map onto the extra button. It reports a click on it
    // by printing its label and exiting non-zero, which is why the labels are compared below.
    MessageDialog::Result MessageDialog::ShowInternal( Severity severity, Type type, String const& title, String const& message )
    {
        TInlineVector<String, 8> args;

        char const* pExtraButtonLabel = nullptr;

        if ( type == Type::Ok )
        {
            switch ( severity )
            {
                case Severity::Warning: args.emplace_back( "--warning" ); break;
                case Severity::Error:
                case Severity::FatalError: args.emplace_back( "--error" ); break;
                default: args.emplace_back( "--info" ); break;
            }
        }
        else
        {
            args.emplace_back( "--question" );

            switch ( type )
            {
                case Type::OkCancel:
                {
                    args.emplace_back( "--ok-label=OK" );
                    args.emplace_back( "--cancel-label=Cancel" );
                }
                break;

                case Type::YesNo:
                {
                    args.emplace_back( "--ok-label=Yes" );
                    args.emplace_back( "--cancel-label=No" );
                }
                break;

                case Type::YesNoCancel:
                {
                    args.emplace_back( "--ok-label=Yes" );
                    args.emplace_back( "--cancel-label=Cancel" );
                    pExtraButtonLabel = "No";
                }
                break;

                case Type::RetryCancel:
                {
                    args.emplace_back( "--ok-label=Retry" );
                    args.emplace_back( "--cancel-label=Cancel" );
                }
                break;

                case Type::AbortRetryIgnore:
                {
                    args.emplace_back( "--ok-label=Retry" );
                    args.emplace_back( "--cancel-label=Abort" );
                    pExtraButtonLabel = "Ignore";
                }
                break;

                case Type::CancelTryContinue:
                {
                    args.emplace_back( "--ok-label=Try Again" );
                    args.emplace_back( "--cancel-label=Cancel" );
                    pExtraButtonLabel = "Continue";
                }
                break;

                default: break;
            }

            if ( pExtraButtonLabel != nullptr )
            {
                args.emplace_back( String( String::CtorSprintf(), "--extra-button=%s", pExtraButtonLabel ) );
            }
        }

        args.emplace_back( String( String::CtorSprintf(), "--title=%s", title.c_str() ) );
        args.emplace_back( String( String::CtorSprintf(), "--text=%s", message.c_str() ) );

        // Messages carry paths and type names, so angle brackets in them are text and not markup.
        args.emplace_back( "--no-markup" );

        //-------------------------------------------------------------------------

        String toolOutput;
        int32_t exitCode = 0;
        if ( !RunDialogTool( args, toolOutput, exitCode ) )
        {
            // No dialog was shown, so the message is only in the log. Halting is wrong here: a
            // warning dialog is not a programmer error, and the ResourceCompiler runs headless.
            switch ( severity )
            {
                case Severity::Warning: EE_LOG_WARNING( "Tools", "Dialog", "%s: %s", title.c_str(), message.c_str() ); break;
                case Severity::Error:
                case Severity::FatalError: EE_LOG_ERROR( "Tools", "Dialog", "%s: %s", title.c_str(), message.c_str() ); break;
                default: EE_LOG_MESSAGE( "Tools", "Dialog", "%s: %s", title.c_str(), message.c_str() ); break;
            }

            // Cancel, not Yes. Nobody confirmed anything, so a confirmation prompt with no user in
            // front of it has to decline. Type::Ok callers ignore the result.
            return ( type == Type::Ok ) ? Result::Yes : Result::Cancel;
        }

        //-------------------------------------------------------------------------

        if ( type == Type::Ok )
        {
            return Result::Yes;
        }

        if ( exitCode == 0 )
        {
            return ( type == Type::RetryCancel || type == Type::AbortRetryIgnore || type == Type::CancelTryContinue ) ? Result::Retry : Result::Yes;
        }

        if ( pExtraButtonLabel != nullptr && toolOutput.find( pExtraButtonLabel ) == 0 )
        {
            return ( type == Type::YesNoCancel ) ? Result::No : Result::Continue;
        }

        return ( type == Type::YesNo ) ? Result::No : Result::Cancel;
    }
}
#endif
