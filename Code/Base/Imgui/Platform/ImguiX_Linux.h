#ifdef __linux__
#pragma once

#include "Base/_Module/API.h"
#include "Base/Esoterica.h"

//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::ImGuiX::Platform
{
    // The parts of the borderless title bar that must never drag the window: the menu section, the
    // optional controls section, and the window controls. ApplicationTitleBar::Draw records them
    // every frame and LinuxApplication::BorderlessWindowHitTest tests the cursor against them.
    //
    // The Win32 sibling asks imgui whether an item is hovered instead. That cannot work here: SDL
    // runs the hit test while it drains the motion event, before imgui has processed that motion,
    // so ImGui::IsAnyItemHovered() describes the *previous* cursor position and the application
    // menus become unreachable. These rectangles do not depend on the frame order.
    //
    // The point is in window coordinates, which is what SDL hands the hit test.
    EE_BASE_API bool IsPointInTitleBarInteractiveSection( int32_t windowX, int32_t windowY );
}
#endif
#endif
