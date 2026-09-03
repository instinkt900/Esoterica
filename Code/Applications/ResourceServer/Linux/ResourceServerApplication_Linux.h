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
    // This runs as an ordinary window. The Win32 tray and taskbar members are gone rather than
    // replaced:
    //
    //  - No tray icon. A tray needs libayatana-appindicator or a StatusNotifierItem over D-Bus, and
    //    many of the desktops this has to run on have no tray at all.
    //  - With no tray, the window never starts minimized or hidden. Hiding it with nothing to
    //    restore it from would strand the user.
    //  - No taskbar progress overlay. ITaskbarList3 has no portable equivalent, and the busy state
    //    is already on screen in the request list.
    //
    // The single-instance guard is kept, as an flock rather than a named mutex.
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
        //
        // m_renderSettings is a member and not a local in Initialize, because
        // RenderSystem::Initialize stores its address - m_pRenderSettings, added upstream in
        // 47e6293 - and then reads it every frame. Declared above m_renderSystem so it outlives it.
        // The Win32 sibling passes a local and dangles; that is upstream's to fix, not ours.
        Render::RenderSettings                  m_renderSettings;
        Render::RenderSystem                    m_renderSystem;
        Render::ImguiRenderer                   m_imguiRenderer;
        Render::Window                          m_renderWindow;

        // Resource
        Resource::ResourceServer                m_resourceServer;
        Resource::ResourceServerUI              m_resourceServerUI;
    };
}

#endif
