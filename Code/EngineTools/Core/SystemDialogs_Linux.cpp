#ifdef __linux__
#include "SystemDialogs.h"

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
        EE_UNUSED( displayText );
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
}
#endif
