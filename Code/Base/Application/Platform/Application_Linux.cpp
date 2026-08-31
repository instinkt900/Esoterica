#ifdef __linux__
#include "Application_Linux.h"
#include "Base/Settings/IniFile.h"
#include "Base/Imgui/Platform/ImguiPlatform_Linux.h"
#include "Base/FileSystem/FileSystemPath.h"
#include "Base/FileSystem/FileSystemUtils.h"
#include "Base/Logging/SystemLog.h"
#include "Base/Math/Rectangle.h"
#include "Base/Platform/Platform.h"

#include <SDL3/SDL.h>

//-------------------------------------------------------------------------

namespace EE
{
    // Win32Application handles WM_GETMINMAXINFO to clamp the window; SDL3 has a setter for it.
    static constexpr int32_t const g_minimumWindowWidth = 320;
    static constexpr int32_t const g_minimumWindowHeight = 240;

    //-------------------------------------------------------------------------

    // The sibling of wndProc: a file static with the signature the platform demands, which reads
    // the application out of the user data pointer and forwards to a method on it.
    static SDL_HitTestResult SDLCALL HitTestCallback( SDL_Window*, SDL_Point const* pArea, void* pUserData )
    {
        auto pApplication = reinterpret_cast<LinuxApplication*>( pUserData );
        return (SDL_HitTestResult) pApplication->BorderlessWindowHitTest( Int2( pArea->x, pArea->y ) );
    }

    //-------------------------------------------------------------------------

    LinuxApplication::LinuxApplication( char const* applicationName, char const* iconFilePath, char const* splashScreenFilePath, TBitFlags<InitOptions> options )
        : m_applicationName( applicationName )
        , m_applicationNameNoWhitespace( StringUtils::StripAllWhitespace( String( applicationName ) ) )
        , m_iconFilePath( iconFilePath != nullptr ? iconFilePath : "" )
        , m_splashScreenFilePath( splashScreenFilePath != nullptr ? splashScreenFilePath : "" )
        , m_startMinimized( options.IsFlagSet( InitOptions::StartMinimized ) )
        , m_isBorderLess( options.IsFlagSet( InitOptions::Borderless ) )
    {}

    LinuxApplication::~LinuxApplication()
    {
        // Guarded, unlike the Win32 sibling. Run() can fail before the window exists, and
        // ClearMainWindowHandle asserts that a handle was set.
        if ( m_pWindow != nullptr )
        {
            Platform::ClearMainWindowHandle();
            SDL_DestroyWindow( m_pWindow );
            m_pWindow = nullptr;
        }

        if ( m_pWindowIcon != nullptr )
        {
            SDL_DestroySurface( m_pWindowIcon );
            m_pWindowIcon = nullptr;
        }

        if ( m_sdlInitialized )
        {
            SDL_Quit();
            m_sdlInitialized = false;
        }
    }

    bool LinuxApplication::FatalError( String const& error ) const
    {
        // The log entry is not a duplicate of the dialog. A dialog needs a desktop, and this
        // runs from a terminal often enough that the message has to survive without one.
        EE_LOG_ERROR( LogCategory::System, "Application", "%s", error.c_str() );
        SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "Fatal Error Occurred!", error.c_str(), m_pWindow );
        return false;
    }

    //-------------------------------------------------------------------------

    bool LinuxApplication::TryCreateMainWindow()
    {
        EE_ASSERT( m_windowSize.m_x > 0 );
        EE_ASSERT( m_windowSize.m_y > 0 );

        // Get window icon
        //-------------------------------------------------------------------------

        // A BMP on disk, not a resource ID. SDL_LoadBMP is the only image loader SDL3 has
        // without SDL_image, which is why the file has to be a BMP and not the .ico the Windows
        // build uses. Either path may be empty, and both are today: see P6.7.
        if ( m_pWindowIcon == nullptr && !m_iconFilePath.empty() )
        {
            m_pWindowIcon = SDL_LoadBMP( m_iconFilePath.c_str() );
            if ( m_pWindowIcon == nullptr )
            {
                EE_LOG_WARNING( LogCategory::System, "Application", "Failed to load window icon \"%s\": %s", m_iconFilePath.c_str(), SDL_GetError() );
            }
        }

        // Create the window
        //-------------------------------------------------------------------------

        SDL_WindowFlags windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;

        // Ask for a real pixel buffer on a HiDPI display rather than a scaled one, so that the
        // swapchain is created at the size the display actually has.
        windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

        if ( m_isBorderLess )
        {
            windowFlags |= SDL_WINDOW_BORDERLESS;
        }

        // Handle recorded window state
        if ( m_wasMaximized )
        {
            windowFlags |= SDL_WINDOW_MAXIMIZED;
        }

        if ( m_startMinimized )
        {
            windowFlags |= SDL_WINDOW_MINIMIZED;
        }

        // The size passed here is in logical desktop coordinates, which is what ReadWindowSettings
        // stored. The pixel size is read back below, once the window is on a display and its
        // scale is known.
        int32_t const windowDesiredWidth = Math::Max( m_windowSize.m_x, g_minimumWindowWidth );
        int32_t const windowDesiredHeight = Math::Max( m_windowSize.m_y, g_minimumWindowHeight );

        m_pWindow = SDL_CreateWindow( m_applicationName.c_str(), windowDesiredWidth, windowDesiredHeight, windowFlags );
        if ( m_pWindow == nullptr )
        {
            EE_LOG_ERROR( LogCategory::System, "Application", "SDL_CreateWindow failed: %s", SDL_GetError() );
            return false;
        }

        Platform::SetMainWindowHandle( m_pWindow );

        //-------------------------------------------------------------------------

        if ( m_pWindowIcon != nullptr )
        {
            SDL_SetWindowIcon( m_pWindow, m_pWindowIcon );
        }

        SDL_SetWindowMinimumSize( m_pWindow, g_minimumWindowWidth, g_minimumWindowHeight );

        // Wayland does not let a client position its own window, so this is silently ignored
        // there and the compositor places the window instead. That is expected, not a failure.
        SDL_SetWindowPosition( m_pWindow, m_windowPosition.m_x, m_windowPosition.m_y );

        // Without a title bar there is nothing for the window manager to drag or resize from, so
        // the application has to say which regions do what.
        if ( m_isBorderLess )
        {
            SDL_SetWindowHitTest( m_pWindow, HitTestCallback, this );
        }

        //-------------------------------------------------------------------------

        // Update the window size to the exact pixel size of the created window
        int32_t pixelWidth = 0, pixelHeight = 0;
        if ( SDL_GetWindowSizeInPixels( m_pWindow, &pixelWidth, &pixelHeight ) && pixelWidth > 0 && pixelHeight > 0 )
        {
            m_windowSize = Int2( pixelWidth, pixelHeight );
        }

        return true;
    }

    void LinuxApplication::ShowMainWindow()
    {
        SDL_ShowWindow( m_pWindow );
    }

    bool LinuxApplication::IsMainWindowMinimized() const
    {
        if ( m_pWindow == nullptr )
        {
            return false;
        }

        return ( SDL_GetWindowFlags( m_pWindow ) & SDL_WINDOW_MINIMIZED ) != 0;
    }

    bool LinuxApplication::TryCreateSplashScreen()
    {
        if ( m_splashScreenFilePath.empty() )
        {
            return true;
        }

        m_pSplashScreenImage = SDL_LoadBMP( m_splashScreenFilePath.c_str() );
        if ( m_pSplashScreenImage == nullptr )
        {
            EE_LOG_ERROR( LogCategory::System, "Application", "Failed to load splash screen \"%s\": %s", m_splashScreenFilePath.c_str(), SDL_GetError() );
            return false;
        }

        // Create splash screen window
        //-------------------------------------------------------------------------

        // Borderless, always on top, and not in the taskbar. SDL3 centers a window it is not
        // told where to put, which is what the Win32 version computes by hand from the monitor
        // work area.
        SDL_WindowFlags const splashScreenFlags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY | SDL_WINDOW_NOT_FOCUSABLE;
        m_pSplashScreenWindow = SDL_CreateWindow( "SplashScreen", m_pSplashScreenImage->w, m_pSplashScreenImage->h, splashScreenFlags );
        if ( m_pSplashScreenWindow == nullptr )
        {
            EE_LOG_ERROR( LogCategory::System, "Application", "Failed to create splash screen window: %s", SDL_GetError() );
            return false;
        }

        SDL_Surface* pWindowSurface = SDL_GetWindowSurface( m_pSplashScreenWindow );
        if ( pWindowSurface != nullptr )
        {
            SDL_BlitSurface( m_pSplashScreenImage, nullptr, pWindowSurface, nullptr );
            SDL_UpdateWindowSurface( m_pSplashScreenWindow );
        }

        return true;
    }

    void LinuxApplication::DestroySplashScreen()
    {
        if ( m_pSplashScreenWindow != nullptr )
        {
            SDL_DestroyWindow( m_pSplashScreenWindow );
            m_pSplashScreenWindow = nullptr;
        }

        if ( m_pSplashScreenImage != nullptr )
        {
            SDL_DestroySurface( m_pSplashScreenImage );
            m_pSplashScreenImage = nullptr;
        }
    }

    bool LinuxApplication::ProcessEvent( SDL_Event const& event )
    {
        // imgui sees the event first, the way Win32Application calls
        // ImGuiX::Platform::WindowMessageProcessor first.
        //
        // **The return value is deliberately ignored**, unlike the Win32 sibling, which returns
        // early when the message is handled. The two backends do not mean the same thing by it.
        // A wnd proc returns non-zero only for a message it truly consumed, and
        // imgui_impl_win32.cpp returns 0 for nearly everything. imgui_impl_sdl3.cpp returns true
        // for every event it recognises, including SDL_EVENT_WINDOW_CLOSE_REQUESTED and the
        // focus events, so returning early here would swallow the application's own close and
        // stop input reaching the engine. Upstream's own SDL3 examples ignore it too.
        #if EE_DEVELOPMENT_TOOLS
        if ( WasInitialized() )
        {
            ImGuiX::Platform::ProcessEvent( event );
        }
        #endif

        //-------------------------------------------------------------------------

        // Window events name the window they belong to. imgui multi-viewport creates windows of
        // its own, so only the main window drives the application. Read this only in the window
        // cases below: SDL_EVENT_QUIT carries no window id.
        SDL_WindowID const mainWindowID = ( m_pWindow != nullptr ) ? SDL_GetWindowID( m_pWindow ) : 0;

        switch ( event.type )
        {
            //-------------------------------------------------------------------------

            // The pixel size, not SDL_EVENT_WINDOW_RESIZED, which reports logical coordinates.
            // The swapchain is sized in pixels.
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            {
                if ( WasInitialized() && event.window.windowID == mainWindowID )
                {
                    Int2 const newWindowSize( event.window.data1, event.window.data2 );
                    if ( newWindowSize.m_x > 0 && newWindowSize.m_y > 0 )
                    {
                        m_windowSize = newWindowSize;
                        ResizeMainWindow( newWindowSize );
                    }
                }

                return true;
            }

            //-------------------------------------------------------------------------

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_TEXT_INPUT:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
            {
                // There are deliberately no SDL_EVENT_GAMEPAD_* cases.
                // XBoxControllerInputDevice polls, the way the XInput sibling does, and
                // SDL_UpdateGamepads picks up hot plug and unplug on its own.
                if ( WasInitialized() )
                {
                    ProcessInputEvent( event );
                }
            }
            break;

            //-------------------------------------------------------------------------

            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            {
                // Closing an imgui viewport window is not a request to close the application.
                if ( event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID != mainWindowID )
                {
                    break;
                }

                if ( OnUserExitRequest() )
                {
                    RequestApplicationExit();
                }

                return true;
            }

            //-------------------------------------------------------------------------

            case SDL_EVENT_WINDOW_DESTROYED:
            {
                if ( event.window.windowID == mainWindowID )
                {
                    ProcessWindowDestructionMessage();
                    RequestApplicationExit();
                }
            }
            break;
        }

        //-------------------------------------------------------------------------

        return false;
    }

    void LinuxApplication::ProcessWindowDestructionMessage()
    {
        WriteWindowSettings();
    }

    //-------------------------------------------------------------------------

    void LinuxApplication::ReadWindowSettings()
    {
        FileSystem::Path const layoutIniFilePath = FileSystem::GetCurrentProcessPath() + m_applicationNameNoWhitespace + ".layout.ini";
        IniFile layoutIni;
        if ( !layoutIni.Load( layoutIniFilePath ) )
        {
            return;
        }

        //-------------------------------------------------------------------------

        // The same four keys as the Win32 sibling writes, so a layout file is portable between
        // the two builds. Left/Top is the position and Right/Bottom is the opposite corner.
        int32_t const left = (int32_t) layoutIni.GetInt( "WindowSettings", "Left", m_windowPosition.m_x );
        int32_t const top = (int32_t) layoutIni.GetInt( "WindowSettings", "Top", m_windowPosition.m_y );
        int32_t const right = (int32_t) layoutIni.GetInt( "WindowSettings", "Right", m_windowPosition.m_x + m_windowSize.m_x );
        int32_t const bottom = (int32_t) layoutIni.GetInt( "WindowSettings", "Bottom", m_windowPosition.m_y + m_windowSize.m_y );

        EE_ASSERT( ( right - left ) > 0 );
        EE_ASSERT( ( bottom - top ) > 0 );

        m_windowPosition = Int2( left, top );
        m_windowSize = Int2( right - left, bottom - top );

        m_wasMaximized = layoutIni.GetBool( "WindowSettings", "WasMaximized", m_wasMaximized );
    }

    void LinuxApplication::WriteWindowSettings()
    {
        // We should always have a valid window when calling this function
        EE_ASSERT( m_pWindow != nullptr );

        // Logical coordinates, matching what SDL_CreateWindow and SDL_SetWindowPosition take.
        // SDL reports the restored position and size while a window is maximized, which is the
        // same thing GetWindowPlacement's rcNormalPosition gives the Win32 version.
        int32_t x = 0, y = 0, width = 0, height = 0;
        SDL_GetWindowPosition( m_pWindow, &x, &y );
        SDL_GetWindowSize( m_pWindow, &width, &height );

        IniFile layoutIni;
        layoutIni.SetInt( "WindowSettings", "Left", x );
        layoutIni.SetInt( "WindowSettings", "Right", x + width );
        layoutIni.SetInt( "WindowSettings", "Top", y );
        layoutIni.SetInt( "WindowSettings", "Bottom", y + height );
        layoutIni.SetBool( "WindowSettings", "WasMaximized", ( SDL_GetWindowFlags( m_pWindow ) & SDL_WINDOW_MAXIMIZED ) != 0 );

        FileSystem::Path const layoutIniFilePath = FileSystem::GetCurrentProcessPath() + m_applicationNameNoWhitespace + ".layout.ini";
        layoutIni.Save( layoutIniFilePath );
    }

    //-------------------------------------------------------------------------

    int32_t LinuxApplication::BorderlessWindowHitTest( Int2 const& cursor ) const
    {
        EE_ASSERT( m_isBorderLess );

        // SDL gives the point in window coordinates and the window size on demand, so unlike the
        // Win32 sibling there is no screen-to-window conversion and no system border metric to
        // look up. The border width is ours to pick.
        constexpr int32_t const borderSize = 8;

        int32_t windowWidth = 0, windowHeight = 0;
        if ( !SDL_GetWindowSize( m_pWindow, &windowWidth, &windowHeight ) )
        {
            return SDL_HITTEST_NORMAL;
        }

        //-------------------------------------------------------------------------

        enum region_mask
        {
            client = 0b0000,
            left = 0b0001,
            right = 0b0010,
            top = 0b0100,
            bottom = 0b1000,
        };

        const auto result =
            left * ( cursor.m_x < borderSize ) |
            right * ( cursor.m_x >= ( windowWidth - borderSize ) ) |
            top * ( cursor.m_y < borderSize ) |
            bottom * ( cursor.m_y >= ( windowHeight - borderSize ) );

        switch ( result )
        {
            case left: return SDL_HITTEST_RESIZE_LEFT;
            case right: return SDL_HITTEST_RESIZE_RIGHT;
            case top: return SDL_HITTEST_RESIZE_TOP;
            case bottom: return SDL_HITTEST_RESIZE_BOTTOM;
            case top | left: return SDL_HITTEST_RESIZE_TOPLEFT;
            case top | right: return SDL_HITTEST_RESIZE_TOPRIGHT;
            case bottom | left: return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            case bottom | right: return SDL_HITTEST_RESIZE_BOTTOMRIGHT;

            case client:
            {
                Math::ScreenSpaceRectangle titleBarRect;
                bool isAnyInteractibleWidgetHovered = false;
                GetBorderlessTitleBarInfo( titleBarRect, isAnyInteractibleWidgetHovered );

                //-------------------------------------------------------------------------

                if ( !isAnyInteractibleWidgetHovered )
                {
                    int32_t const titleBarTop = (int32_t) titleBarRect.GetTL().m_y;
                    int32_t const titleBarLeft = (int32_t) titleBarRect.GetTL().m_x;
                    int32_t const titleBarBottom = titleBarTop + (int32_t) titleBarRect.GetSize().m_y;
                    int32_t const titleBarRight = titleBarLeft + (int32_t) titleBarRect.GetSize().m_x;

                    bool const isCursorWithinTitleBarX = cursor.m_x > titleBarLeft && cursor.m_x < titleBarRight;
                    bool const isCursorWithinTitleBarY = cursor.m_y > titleBarTop && cursor.m_y < titleBarBottom;
                    if ( isCursorWithinTitleBarX && isCursorWithinTitleBarY )
                    {
                        return SDL_HITTEST_DRAGGABLE;
                    }
                }

                return SDL_HITTEST_NORMAL;
            }

            default: return SDL_HITTEST_NORMAL;
        }
    }

    //-------------------------------------------------------------------------

    int32_t LinuxApplication::Run( int32_t argc, char** argv )
    {
        // SDL
        //-------------------------------------------------------------------------

        // Win32Application has no equivalent step: the Win32 window API needs no initialization.
        // Video only. The gamepad subsystem is not initialized here: XBoxControllerInputDevice
        // calls SDL_InitSubSystem itself, so anything holding an InputSystem gets working
        // gamepads without the application having to know about SDL.
        if ( !SDL_Init( SDL_INIT_VIDEO ) )
        {
            return FatalError( String( "SDL failed to initialize: " ) + SDL_GetError() );
        }

        m_sdlInitialized = true;

        // Window
        //-------------------------------------------------------------------------

        ReadWindowSettings();

        if ( !TryCreateMainWindow() )
        {
            return FatalError( "Application failed to create window!" );
        }

        if ( !TryCreateSplashScreen() )
        {
            return FatalError( "Application failed to create splash screen!" );
        }

        // Initialization
        //-------------------------------------------------------------------------

        if ( !Initialize( argc, argv ) )
        {
            Shutdown();
            return FatalError( "Application failed to initialize correctly!" );
        }
        else
        {
            m_initialized = true;
        }

        // Window Size
        //-------------------------------------------------------------------------

        ShowMainWindow();
        DestroySplashScreen();
        OnFirstShowMainWindow();

        // Application loop
        //-------------------------------------------------------------------------

        // Win32Application takes its exit code from the WM_QUIT message. SDL has no such value,
        // so a clean run is always 0 and a failure returns through FatalError.
        int32_t const exitCode = 0;

        bool shouldExit = false;
        while ( !shouldExit )
        {
            // Drain the queue, then run one frame. The Win32 sibling handles a single message
            // per iteration and only updates once the queue is empty, which comes to the same
            // thing.
            SDL_Event event;
            while ( SDL_PollEvent( &event ) )
            {
                ProcessEvent( event );
            }

            SDL_GetWindowPosition( m_pWindow, &m_windowPosition.m_x, &m_windowPosition.m_y );

            if ( m_applicationRequestedExit )
            {
                shouldExit = true;
            }
            else
            {
                shouldExit = !ApplicationLoop();
            }
        }

        // Shutdown
        //-------------------------------------------------------------------------

        // The window is destroyed in the destructor, so nothing raises
        // SDL_EVENT_WINDOW_DESTROYED while this loop is still running. Write the settings here,
        // the way WM_DESTROY does on Windows.
        ProcessWindowDestructionMessage();

        bool const shutdownResult = Shutdown();
        m_initialized = false;

        FileSystem::Path const logFilePath = FileSystem::GetCurrentProcessPath() + m_applicationNameNoWhitespace + "Log.txt";
        SystemLog::SaveToFile( logFilePath );

        //-------------------------------------------------------------------------

        if ( !shutdownResult )
        {
            return FatalError( "Application failed to shutdown correctly!" );
        }

        return exitCode;
    }
}
#endif
