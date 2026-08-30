#ifdef __linux__
#pragma once

#include "Base/_Module/API.h"
#include "Base/Esoterica.h"

//-------------------------------------------------------------------------

// Forward declared, so that this header does not put SDL3 on the include path of every file that
// dispatches an event. Application_Linux.h does the same, for the same reason.
union SDL_Event;

//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::ImGuiX::Platform
{
    // Returns true when imgui consumed the event. LinuxApplication::ProcessEvent calls this
    // first, the way Win32Application calls WindowMessageProcessor.
    EE_BASE_API bool ProcessEvent( SDL_Event const& event );
}
#endif
#endif
