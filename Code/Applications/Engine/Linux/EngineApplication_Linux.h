#ifdef __linux__
#pragma once

#include "Game/_Module/GameModule.h"
#include "Game/ToolsUI/GameDebugUI.h"
#include "Engine/Engine.h"
#include "Base/Application/Platform/Application_Linux.h"

//-------------------------------------------------------------------------

namespace EE
{
    class StandaloneEngine final : public Engine
    {
        friend class ResourceEditorApplication;

    public:

        StandaloneEngine( TFunction<bool( EE::String const& error )>&& errorHandler );

        void RegisterTypes() override;
        void UnregisterTypes() override;

        virtual void PostInitialize() override;
        virtual void PreShutdown() override;

        virtual void ResizeMainWindow( Int2 newMainWindowDimensions ) override;

        #if EE_DEVELOPMENT_TOOLS
        virtual void CreateToolsUI() override { m_pToolsUI = EE::New<GameDebugUI>(); }
        #endif

    private:

        Viewport* m_pGameViewport = nullptr;
    };

    //-------------------------------------------------------------------------

    class EngineApplication : public LinuxApplication
    {

    public:

        EngineApplication();

    private:

        virtual bool Initialize( int32_t argc, char** argv ) override;
        virtual bool Shutdown() override;

        virtual void ResizeMainWindow( Int2 const& newWindowSize ) override;
        virtual void ProcessInputEvent( SDL_Event const& event ) override;

        virtual bool ApplicationLoop() override;

        // The Win32 sibling carries LivePP_PreReload and LivePP_PostReload here. Live++ has no Linux
        // build, so EE_ENABLE_LPP stays unset and the hooks do not appear.

    private:

        StandaloneEngine            m_engine;
    };
}

#endif
