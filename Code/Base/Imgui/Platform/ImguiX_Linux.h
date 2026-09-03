#ifdef __linux__
#pragma once

#include "Base/_Module/API.h"
#include "Base/Esoterica.h"

//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::ImGuiX::Platform
{
    // The parts of the borderless title bar that must never drag the window. ApplicationTitleBar::Draw
    // records them every frame and LinuxApplication::BorderlessWindowHitTest tests the cursor.
    //
    // Rectangles rather than the Win32 sibling's IsAnyItemHovered(), because SDL runs the hit test
    // while it drains the motion event, before imgui has processed that motion. The hovered flag
    // therefore describes the previous cursor position, and the menus become unreachable.
    //
    // The point is in window coordinates, which is what SDL hands the hit test.
    EE_BASE_API bool IsPointInTitleBarInteractiveSection( int32_t windowX, int32_t windowY );
}
#endif
#endif
