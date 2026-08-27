#ifdef __linux__
#include "Base/Imgui/ImguiSystem.h"

//-------------------------------------------------------------------------
// Imgui platform backend
//-------------------------------------------------------------------------
// Phase 1 stubs, for the same reason as Code/Base/Render/RHI_Vulkan.cpp: the Win32 backend is
// excluded from the Linux build, and without definitions here Esoterica.Base does not link, so
// nothing downstream can be built or tested.
//
// Phase 6 replaces these with the SDL3 backend. The vendored ImguiPlatform_Win32.cpp is a copy
// of upstream imgui_impl_win32.cpp, so upstream imgui_impl_sdl3.cpp is close to a drop-in
// replacement rather than a rewrite. See Docs/Linux/Phases/Phase6-WindowingInput.md.
//
// These halt rather than returning quietly. A silent no-op here would surface in Phase 6 as a
// window that never draws, with nothing to point at.
//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::ImGuiX
{
    void ImguiSystem::InitializePlatform()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void ImguiSystem::ShutdownPlatform()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void ImguiSystem::PlatformNewFrame()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }
}
#endif
#endif
