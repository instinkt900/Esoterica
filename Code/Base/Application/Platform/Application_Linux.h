#ifdef __linux__
#pragma once

#include "Base/Application/ApplicationGlobalState.h"
#include "Base/Esoterica.h"
#include "Base/Types/String.h"
#include "Base/Math/Math.h"
#include "Base/Types/BitFlags.h"

//-------------------------------------------------------------------------

// Forward declared so that this header does not put SDL3 on the include path of everything that
// derives from LinuxApplication. ImguiPlatform_Win32.h forward declares HWND__ for the same
// reason. Both spellings match SDL3: SDL_Window is a struct, SDL_Event is a union.
struct SDL_Window;
struct SDL_Surface;
union SDL_Event;

//-------------------------------------------------------------------------

namespace EE
{
    namespace Math { class ScreenSpaceRectangle; }

    //-------------------------------------------------------------------------

    // The Linux sibling of Win32Application. The two are not related by inheritance and must not be:
    // do not refactor Win32Application into a shared base.
    //
    // Everything platform neutral keeps its name and signature, so a subclass reads the same on both
    // platforms. Only the Win32 typed members differ, and each is named below next to the SDL3 call
    // that replaces it.
    class EE_BASE_API LinuxApplication
    {
    protected:

        enum class InitOptions
        {
            StartMinimized,
            Borderless
        };

    public:

        // No HINSTANCE, and no resource IDs. Windows resources have no Linux equivalent and .rc
        // files are not parsed, so the icon and splash screen are BMP paths and either may be null.
        LinuxApplication( char const* applicationName, char const* iconFilePath = nullptr, char const* splashScreenFilePath = nullptr, TBitFlags<InitOptions> options = TBitFlags<InitOptions>() );
        virtual ~LinuxApplication();

        int32_t Run( int32_t argc, char** argv );

        inline bool WasInitialized() const { return m_initialized; }

        // SDL3 event handler. Replaces WindowMessageProcessor. Return true when the event is
        // fully handled and no further processing is wanted.
        virtual bool ProcessEvent( SDL_Event const& event );

        // Called whenever we receive an application exit request. Return true to allow the exit
        virtual bool OnUserExitRequest() { return true; }

        // Get the application icon
        inline SDL_Surface* GetIcon() const { return m_pWindowIcon; }

        // Is the main window currently minimized? Replaces IsIconic. Defined out of line so
        // that a subclass can skip its render work without taking SDL3 on its include path.
        bool IsMainWindowMinimized() const;

        // Hit test for border less windows, in place of handling WM_NCHITTEST. Returns an
        // SDL_HitTestResult widened to int32_t, so that this header needs no SDL enum.
        //
        // Public because the SDL_SetWindowHitTest callback is a file static in the .cpp and has to
        // reach it, as Win32Application's wndProc reaches WindowMessageProcessor.
        int32_t BorderlessWindowHitTest( Int2 const& cursor ) const;

    protected:

        virtual bool FatalError( String const& error ) const;

        //-------------------------------------------------------------------------

        // Called after showing the main window for the first time
        virtual void OnFirstShowMainWindow() {}

        // Called just before destroying the window
        virtual void ProcessWindowDestructionMessage();

        // Handle window resize events
        virtual void ResizeMainWindow( Int2 const& newWindowSize ) = 0;

        // Handle user application input events. Replaces ProcessInputMessage.
        virtual void ProcessInputEvent( SDL_Event const& event ) {};

        // Get title bar region for border less windows
        virtual void GetBorderlessTitleBarInfo( Math::ScreenSpaceRectangle& outTitlebarRect, bool& isInteractibleWidgetHovered ) const {};

        //-------------------------------------------------------------------------

        // These function allows the application to read/write any window layout/positioning specific settings it needs
        virtual void WriteWindowSettings();
        virtual void ReadWindowSettings();

        // Initialize/Shutdown
        virtual bool Initialize( int32_t argc, char** argv ) = 0;
        virtual bool Shutdown() = 0;

        // The actual application loop
        virtual bool ApplicationLoop() = 0;

        //-------------------------------------------------------------------------

        void RequestApplicationExit() { m_applicationRequestedExit = true; }

        // Live++ has no Linux build, so EE_ENABLE_LPP stays unset and the agent members and
        // hooks that Win32Application carries do not appear here at all.

    private:

        bool TryCreateMainWindow();
        void ShowMainWindow();

        bool TryCreateSplashScreen();
        void DestroySplashScreen();

    protected:

        String const                    m_applicationName;
        String const                    m_applicationNameNoWhitespace;
        String const                    m_iconFilePath;
        SDL_Surface*                    m_pWindowIcon = nullptr;
        SDL_Window*                     m_pWindow = nullptr;

        // Win32Application keeps a single RECT. These are separate because SDL3 separates them,
        // and because they are in different units: the position is in logical desktop
        // coordinates, which is what SDL_SetWindowPosition takes, and the size is in pixels,
        // which is what the swapchain needs. They are the same number on a non-HiDPI display.
        Int2                            m_windowPosition = Int2( 100, 100 );
        Int2                            m_windowSize = Int2( 640, 480 );

        String const                    m_splashScreenFilePath;
        SDL_Window*                     m_pSplashScreenWindow = nullptr;
        SDL_Surface*                    m_pSplashScreenImage = nullptr;

    private:

        bool                            m_wasMaximized = false; // Read from the layout settings
        bool                            m_startMinimized = false; // Specifies the initial state of the application
        bool                            m_initialized = false;
        bool                            m_applicationRequestedExit = false;
        bool                            m_isBorderLess = false;
        bool                            m_sdlInitialized = false;
    };
}
#endif
