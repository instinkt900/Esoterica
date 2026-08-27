#ifdef __linux__
#include "SystemDialogs.h"
#include "Base/TypeSystem/TypeRegistry.h"
#include "ToolsContext.h"
#include "Base/Logging/SystemLog.h"

//-------------------------------------------------------------------------
// Native file dialogs
//-------------------------------------------------------------------------
// Phase 7 implements these properly, most likely through the XDG desktop portal so that the
// dialog matches whatever desktop the user is running.
//
// They exist now because EngineTools has to *link*, not because anything calls them: the
// ResourceCompiler and the Reflector never open a dialog, but the editor UI code that does is
// compiled into the same library.
//
// Every one halts. A dialog that silently returns "no file selected" would look to the caller
// exactly like a user pressing Cancel, and Phase 7 would inherit a UI that quietly does nothing.
//-------------------------------------------------------------------------

namespace EE
{
    FileDialog::ExtensionFilter::ExtensionFilter( FileSystem::Extension ext, String displayText )
        : m_extension( ext )
    {
        // Not halting: this is a plain value type that callers construct while building filter
        // lists, well before any dialog is opened. Halting here would stop the editor starting.
        (void) displayText;
    }

    //-------------------------------------------------------------------------

    FileDialog::Result FileDialog::SelectFolder( FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    FileDialog::Result FileDialog::Load( TInlineVector<ExtensionFilter, 2> const& filters, String const& title, bool allowMultiselect, FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    FileDialog::Result FileDialog::LoadResourceOrDataFile( ToolsContext const* pToolsContext, FileSystem::Extension extension, FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    FileDialog::Result FileDialog::LoadResourceOrDataFile( ToolsContext const* pToolsContext, TypeSystem::TypeID typeID, FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    FileDialog::Result FileDialog::Save( TInlineVector<ExtensionFilter, 2> const& filters, String const& title, FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    FileDialog::Result FileDialog::SaveResourceOrDataFile( ToolsContext const* pToolsContext, FileSystem::Extension extension, FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    FileDialog::Result FileDialog::SaveResourceOrDataFile( ToolsContext const* pToolsContext, TypeSystem::TypeID typeID, FileSystem::Path const& startingPath )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return Result();
    }

    //-------------------------------------------------------------------------
    // Message dialogs
    //-------------------------------------------------------------------------

    // The single funnel every MessageDialog::Info / Warning / Error / Confirmation call reaches.
    // Those wrappers are non-virtual statics defined in the header, so EngineTools does not link
    // without this even though the ResourceCompiler never shows a dialog.
    //
    // The message goes to the log rather than nowhere. Unlike the file dialogs, a message box is
    // often the only report of an error a caller gives, and dropping it silently would hide real
    // failures during data compilation. Halting is wrong here for the same reason: a warning
    // dialog is not a programmer error.
    MessageDialog::Result MessageDialog::ShowInternal( Severity severity, Type type, String const& title, String const& message )
    {
        switch ( severity )
        {
            case Severity::Warning : EE_LOG_WARNING( "Tools", "Dialog", "%s: %s", title.c_str(), message.c_str() ); break;
            case Severity::Error :   EE_LOG_ERROR( "Tools", "Dialog", "%s: %s", title.c_str(), message.c_str() ); break;
            default :                EE_LOG_MESSAGE( "Tools", "Dialog", "%s: %s", title.c_str(), message.c_str() ); break;
        }

        // Cancel, not Yes. Nobody confirmed anything, so a confirmation prompt with no user in
        // front of it has to decline. Type::Ok callers ignore the result.
        (void) type;
        return Result::Cancel;
    }
}
#endif
