#ifdef __linux__
#pragma once

#include "../ResourceServer.h"
#include "../ResourceServerUI.h"
#include "Engine/Render/RenderSystem.h"
#include "Engine/Render/Imgui/ImguiRenderer.h"
#include "Base/Application/Platform/Application_Linux.h"
#include "Base/Render/RenderWindow.h"
#include "Base/Imgui/ImguiSystem.h"
#include "Base/TypeSystem/TypeRegistry.h"
#include "Base/Types/String.h"
#include "Base/Esoterica.h"

//-------------------------------------------------------------------------

namespace EE
{
    // The Linux Resource Server runs as an ordinary window, and the Win32 system tray and
    // taskbar members are gone rather than replaced. See P7.3 in Docs/Linux/Progress.md:
    //
    // - No tray icon. A tray needs libayatana-appindicator or a StatusNotifierItem over D-Bus,
    //   neither is present, and half the desktops this has to run on have no tray at all.
    // - Because there is no tray, the window is neither started minimized nor hidden on first
    //   show. Hiding it with nothing to restore it from would strand the user.
    // - No taskbar progress overlay. ITaskbarList3 has no portable equivalent. The busy state
    //   is already on screen in the UI's own request list.
    // The single-instance guard is kept, as an flock rather than a named mutex. See the .cpp.
    //
    // The Win32 sibling also declares an unused InternalUpdateContext. It is dropped here.
    class ResourceServerApplication final : public LinuxApplication
    {
    public:

        ResourceServerApplication();

    private:

        virtual bool Initialize( int32_t argc, char** argv ) override;
        virtual bool Shutdown() override;
        virtual bool ApplicationLoop() override;
        virtual bool OnUserExitRequest() override;
        virtual void ResizeMainWindow( Int2 const& newWindowSize ) override;
        virtual void GetBorderlessTitleBarInfo( Math::ScreenSpaceRectangle& outTitlebarRect, bool& isInteractibleWidgetHovered ) const override { m_resourceServerUI.GetBorderlessTitleBarInfo( outTitlebarRect, isInteractibleWidgetHovered ); }

    private:

        Seconds                                 m_deltaTime = 0.0f;

        TypeSystem::TypeRegistry                m_typeRegistry;
        ImGuiX::ImguiSystem                     m_imguiSystem;

        // Rendering
        Render::RenderSystem                    m_renderSystem;
        Render::ImguiRenderer                   m_imguiRenderer;
        Render::Window                          m_renderWindow;

        // Resource
        Resource::ResourceServer                m_resourceServer;
        Resource::ResourceServerUI              m_resourceServerUI;
    };
}

#endif
