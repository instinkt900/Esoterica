#include "Base/Imgui/ImguiX.h"

#if EE_DEVELOPMENT_TOOLS
#ifdef __linux__
#include "ImguiX_Linux.h"
#include <SDL3/SDL.h>

//-------------------------------------------------------------------------

#define EE_ENABLE_IMGUIX_DEBUG 0

//-------------------------------------------------------------------------

namespace EE::ImGuiX
{
    // The title bar's non-draggable sub-rects, in window coordinates. The hit test runs on the
    // same thread as the draw, one frame behind it. See ImguiX_Linux.h for why it cannot instead
    // ask imgui what is hovered.
    struct TitleBarInteractiveSection
    {
        float m_left = 0, m_top = 0, m_right = 0, m_bottom = 0;
    };

    constexpr static int32_t const g_maxTitleBarInteractiveSections = 3; // menus, controls, window controls
    static TitleBarInteractiveSection g_titleBarInteractiveSections[g_maxTitleBarInteractiveSections];
    static int32_t g_numTitleBarInteractiveSections = 0;

    static void RecordTitleBarInteractiveSection( ImVec2 const& pos, ImVec2 const& size )
    {
        if ( size.x <= 0.0f || size.y <= 0.0f )
        {
            return;
        }

        EE_ASSERT( g_numTitleBarInteractiveSections < g_maxTitleBarInteractiveSections );
        TitleBarInteractiveSection& section = g_titleBarInteractiveSections[g_numTitleBarInteractiveSections];
        section.m_left = pos.x;
        section.m_top = pos.y;
        section.m_right = pos.x + size.x;
        section.m_bottom = pos.y + size.y;
        g_numTitleBarInteractiveSections++;
    }

    //-------------------------------------------------------------------------

    void ApplicationTitleBar::Draw( TFunction<void()>&& menuDrawFunction, float menuSectionDesiredWidth, TFunction<void()>&& controlsSectionDrawFunction, float controlsSectionDesiredWidth )
    {
        m_rect.Reset();
        g_numTitleBarInteractiveSections = 0;

        //-------------------------------------------------------------------------

        ImVec2 const titleBarPadding( 0, 8 );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, titleBarPadding );
        ImGuiViewport* pViewport = ImGui::GetMainViewport();
        if ( ImGui::BeginViewportSideBar( "TitleBar", pViewport, ImGuiDir_Up, 40, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse ) )
        {
            ImGui::PopStyleVar();

            // Calculate sizes
            //-------------------------------------------------------------------------

            float const titleBarWidth = ImGui::GetWindowSize().x;
            float const titleBarHeight = ImGui::GetWindowSize().y;
            m_rect = Math::ScreenSpaceRectangle( Float2::Zero, Float2( titleBarWidth, titleBarHeight ) );

            float const windowControlsWidth = GetWindowsControlsWidth();
            float const windowControlsStartPosX = Math::Max( 0.0f, ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - windowControlsWidth );
            ImVec2 const windowControlsStartPos( windowControlsStartPosX, ImGui::GetCursorPosY() - titleBarPadding.y );

            // Calculate the available space
            float const availableSpace = titleBarWidth - windowControlsWidth - s_minimumDraggableGap - ( s_sectionPadding * 2 );
            float remainingSpace = availableSpace;

            // Calculate section widths
            float const menuSectionFinalWidth = ( remainingSpace - menuSectionDesiredWidth ) > 0 ? menuSectionDesiredWidth : Math::Max( 0.0f, remainingSpace );
            remainingSpace -= menuSectionFinalWidth;
            ImVec2 const menuSectionStartPos( s_sectionPadding, ImGui::GetCursorPosY() );

            float const controlSectionFinalWidth = ( remainingSpace - controlsSectionDesiredWidth ) > 0 ? controlsSectionDesiredWidth : Math::Max( 0.0f, remainingSpace );
            remainingSpace -= controlSectionFinalWidth;
            ImVec2 const controlSectionStartPos( windowControlsStartPos.x - s_sectionPadding - controlSectionFinalWidth, ImGui::GetCursorPosY() - titleBarPadding.y );

            // Draw sections
            //-------------------------------------------------------------------------

            if ( menuSectionFinalWidth > 0 )
            {
                #if EE_ENABLE_IMGUIX_DEBUG
                ImGui::PushStyleColor( ImGuiCol_ChildBg, Colors::Green ); // Debug Color
                #endif

                RecordTitleBarInteractiveSection( menuSectionStartPos, ImVec2( menuSectionFinalWidth, titleBarHeight ) );

                ImGui::SetCursorPos( menuSectionStartPos );
                ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
                ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImGui::GetStyle().FramePadding + ImVec2( 0, 2 ) );
                bool const drawMenuSection = ImGui::BeginChild( "Left", ImVec2( menuSectionFinalWidth, titleBarHeight ), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar );
                ImGui::PopStyleVar( 2 );

                if ( drawMenuSection )
                {
                    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 16, 8 ) );
                    if ( ImGui::BeginMenuBar() )
                    {
                        menuDrawFunction();
                        ImGui::EndMenuBar();
                    }
                    ImGui::PopStyleVar();
                }
                ImGui::EndChild();

                #if EE_ENABLE_IMGUIX_DEBUG
                ImGui::PopStyleColor();
                #endif
            }

            if ( controlSectionFinalWidth > 0 )
            {
                #if EE_ENABLE_IMGUIX_DEBUG
                ImGui::PushStyleColor( ImGuiCol_ChildBg, Colors::Red ); // Debug Color
                #endif

                RecordTitleBarInteractiveSection( controlSectionStartPos, ImVec2( controlSectionFinalWidth, titleBarHeight ) );

                ImGui::SetCursorPos( controlSectionStartPos );
                ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
                bool const drawControlsSection = ImGui::BeginChild( "Right", ImVec2( controlSectionFinalWidth, titleBarHeight ), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoDecoration );
                ImGui::PopStyleVar();

                if ( drawControlsSection )
                {
                    controlsSectionDrawFunction();
                }
                ImGui::EndChild();

                #if EE_ENABLE_IMGUIX_DEBUG
                ImGui::PopStyleColor();
                #endif
            }

            // Draw window controls
            //-------------------------------------------------------------------------

            #if EE_ENABLE_IMGUIX_DEBUG
            ImGui::PushStyleColor( ImGuiCol_ChildBg, Colors::Blue ); // Debug Color
            #endif

            RecordTitleBarInteractiveSection( windowControlsStartPos, ImVec2( windowControlsWidth, titleBarHeight ) );

            ImGui::SetCursorPos( windowControlsStartPos );
            if ( ImGui::BeginChild( "WindowControls", ImVec2( windowControlsWidth, titleBarHeight ), 0, ImGuiWindowFlags_NoDecoration ) )
            {
                DrawWindowControls();
            }
            ImGui::EndChild();

            #if EE_ENABLE_IMGUIX_DEBUG
            ImGui::PopStyleColor();
            #endif

            //-------------------------------------------------------------------------

            ImGui::End();
        }
    }

    void ApplicationTitleBar::DrawWindowControls()
    {
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 0 );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );

        //-------------------------------------------------------------------------

        // The SDL3 imgui backend stores the SDL_WindowID in PlatformHandle, so the window is
        // looked up by id here, where the Win32 sibling casts PlatformHandleRaw to an HWND.
        // See ImGui_ImplSDL3_SetupPlatformHandles.
        auto const windowID = (SDL_WindowID) (intptr_t) ImGui::GetWindowViewport()->PlatformHandle;
        SDL_Window* pWindow = SDL_GetWindowFromID( windowID );

        // Minimize
        //-------------------------------------------------------------------------

        if ( ImGuiX::FlatIconButton( EE_ICON_WINDOW_MINIMIZE, "##Min", Colors::White, ImVec2( s_windowControlButtonWidth, -1 ), true ) )
        {
            if ( pWindow )
            {
                SDL_MinimizeWindow( pWindow );
            }
        }

        // Maximize/Restore
        //-------------------------------------------------------------------------

        bool isMaximized = false;
        if ( pWindow )
        {
            isMaximized = ( SDL_GetWindowFlags( pWindow ) & SDL_WINDOW_MAXIMIZED ) != 0;
        }

        ImGui::SameLine();

        if ( isMaximized )
        {
            if ( ImGuiX::FlatIconButton( EE_ICON_WINDOW_RESTORE, "##Res", Colors::White, ImVec2( s_windowControlButtonWidth, -1 ), true ) )
            {
                if ( pWindow )
                {
                    SDL_RestoreWindow( pWindow );
                }
            }
        }
        else
        {
            if ( ImGuiX::FlatIconButton( EE_ICON_WINDOW_MAXIMIZE, "##Max", Colors::White, ImVec2( s_windowControlButtonWidth, -1 ), true ) )
            {
                if ( pWindow )
                {
                    SDL_MaximizeWindow( pWindow );
                }
            }
        }

        // Close
        //-------------------------------------------------------------------------

        ImGui::SameLine();

        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, 0xFF1C2BC4 );

        ImU32 const backgroundColor = ImGui::ColorConvertFloat4ToU32( ImGui::GetStyle().Colors[ImGuiCol_Button] );
        ImU32 const hoverColor = 0xFF1C2BC4;
        ImU32 const activeColor = 0xFF141E89;

        ImGuiX::ButtonSettings settings{ .m_backgroundColor = Colors::Transparent, .m_hoverColor = hoverColor, .m_activeColor = activeColor, .m_iconColor = Colors::White, .m_foregroundColor = Colors::White, .m_shouldCenterContents = true };
        if ( ImGuiX::ButtonEx( EE_ICON_WINDOW_CLOSE, "##X", ImVec2( s_windowControlButtonWidth, -1 ), settings ) )
        {
            if ( pWindow )
            {
                // SendMessage( hwnd, WM_CLOSE ) has no SDL equivalent, so the close request is
                // pushed onto the queue instead. LinuxApplication::ProcessEvent handles it, and
                // it reaches OnUserExitRequest exactly as a window manager close would.
                SDL_Event closeEvent = {};
                closeEvent.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
                closeEvent.window.timestamp = SDL_GetTicksNS();
                closeEvent.window.windowID = windowID;
                SDL_PushEvent( &closeEvent );
            }
        }

        //-------------------------------------------------------------------------

        ImGui::PopStyleColor();
        ImGui::PopStyleVar( 2 );
    }
}

//-------------------------------------------------------------------------

namespace EE::ImGuiX::Platform
{
    bool IsPointInTitleBarInteractiveSection( int32_t windowX, int32_t windowY )
    {
        float const x = (float) windowX;
        float const y = (float) windowY;

        for ( int32_t i = 0; i < g_numTitleBarInteractiveSections; i++ )
        {
            TitleBarInteractiveSection const& section = g_titleBarInteractiveSections[i];
            if ( x >= section.m_left && x < section.m_right && y >= section.m_top && y < section.m_bottom )
            {
                return true;
            }
        }

        return false;
    }
}
#endif
#endif
