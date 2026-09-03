#ifdef __linux__
#include "Base/Input/InputDevices/InputDevice_KeyboardMouse.h"
#include "Base/Types/HashMap.h"
#include <SDL3/SDL.h>

//-------------------------------------------------------------------------

namespace EE::Input
{
    namespace SDLKeyMap
    {
        // SDL scancode to EE keyboard buttons.
        //
        // Scancodes, not keycodes. A scancode names the physical key and does not move with the
        // layout, which is what the Win32 sibling gets from raw input. A keycode would put W,A,S,D
        // somewhere else on an AZERTY or Dvorak layout.
        static THashMap<uint32_t, InputID> g_keyMappings;

        static bool ConvertScancodeToInputID( SDL_Scancode scancode, InputID& inputID )
        {
            // The Win32 sibling has 60 lines here, fixing up VK_SHIFT, VK_CONTROL, VK_MENU and
            // every numpad key from the message's scan code and extended bit, because a Windows
            // virtual key does not distinguish left from right or numpad from cursor block.
            // Scancodes already do, so there is nothing to fix up.
            auto const keyIter = g_keyMappings.find( (uint32_t) scancode );
            if ( keyIter != g_keyMappings.end() )
            {
                inputID = keyIter->second;
                return true;
            }

            return false;
        }

        static void Initialize()
        {
            g_keyMappings[SDL_SCANCODE_A] = InputID::Keyboard_A;
            g_keyMappings[SDL_SCANCODE_B] = InputID::Keyboard_B;
            g_keyMappings[SDL_SCANCODE_C] = InputID::Keyboard_C;
            g_keyMappings[SDL_SCANCODE_D] = InputID::Keyboard_D;
            g_keyMappings[SDL_SCANCODE_E] = InputID::Keyboard_E;
            g_keyMappings[SDL_SCANCODE_F] = InputID::Keyboard_F;
            g_keyMappings[SDL_SCANCODE_G] = InputID::Keyboard_G;
            g_keyMappings[SDL_SCANCODE_H] = InputID::Keyboard_H;
            g_keyMappings[SDL_SCANCODE_I] = InputID::Keyboard_I;
            g_keyMappings[SDL_SCANCODE_J] = InputID::Keyboard_J;
            g_keyMappings[SDL_SCANCODE_K] = InputID::Keyboard_K;
            g_keyMappings[SDL_SCANCODE_L] = InputID::Keyboard_L;
            g_keyMappings[SDL_SCANCODE_M] = InputID::Keyboard_M;
            g_keyMappings[SDL_SCANCODE_N] = InputID::Keyboard_N;
            g_keyMappings[SDL_SCANCODE_O] = InputID::Keyboard_O;
            g_keyMappings[SDL_SCANCODE_P] = InputID::Keyboard_P;
            g_keyMappings[SDL_SCANCODE_Q] = InputID::Keyboard_Q;
            g_keyMappings[SDL_SCANCODE_R] = InputID::Keyboard_R;
            g_keyMappings[SDL_SCANCODE_S] = InputID::Keyboard_S;
            g_keyMappings[SDL_SCANCODE_T] = InputID::Keyboard_T;
            g_keyMappings[SDL_SCANCODE_U] = InputID::Keyboard_U;
            g_keyMappings[SDL_SCANCODE_V] = InputID::Keyboard_V;
            g_keyMappings[SDL_SCANCODE_W] = InputID::Keyboard_W;
            g_keyMappings[SDL_SCANCODE_X] = InputID::Keyboard_X;
            g_keyMappings[SDL_SCANCODE_Y] = InputID::Keyboard_Y;
            g_keyMappings[SDL_SCANCODE_Z] = InputID::Keyboard_Z;
            g_keyMappings[SDL_SCANCODE_0] = InputID::Keyboard_0;
            g_keyMappings[SDL_SCANCODE_1] = InputID::Keyboard_1;
            g_keyMappings[SDL_SCANCODE_2] = InputID::Keyboard_2;
            g_keyMappings[SDL_SCANCODE_3] = InputID::Keyboard_3;
            g_keyMappings[SDL_SCANCODE_4] = InputID::Keyboard_4;
            g_keyMappings[SDL_SCANCODE_5] = InputID::Keyboard_5;
            g_keyMappings[SDL_SCANCODE_6] = InputID::Keyboard_6;
            g_keyMappings[SDL_SCANCODE_7] = InputID::Keyboard_7;
            g_keyMappings[SDL_SCANCODE_8] = InputID::Keyboard_8;
            g_keyMappings[SDL_SCANCODE_9] = InputID::Keyboard_9;
            g_keyMappings[SDL_SCANCODE_COMMA] = InputID::Keyboard_Comma;
            g_keyMappings[SDL_SCANCODE_PERIOD] = InputID::Keyboard_Period;
            g_keyMappings[SDL_SCANCODE_SLASH] = InputID::Keyboard_ForwardSlash;
            g_keyMappings[SDL_SCANCODE_SEMICOLON] = InputID::Keyboard_SemiColon;
            g_keyMappings[SDL_SCANCODE_APOSTROPHE] = InputID::Keyboard_Quote;
            g_keyMappings[SDL_SCANCODE_LEFTBRACKET] = InputID::Keyboard_LBracket;
            g_keyMappings[SDL_SCANCODE_RIGHTBRACKET] = InputID::Keyboard_RBracket;
            g_keyMappings[SDL_SCANCODE_BACKSLASH] = InputID::Keyboard_BackSlash;
            g_keyMappings[SDL_SCANCODE_MINUS] = InputID::Keyboard_Minus;
            g_keyMappings[SDL_SCANCODE_EQUALS] = InputID::Keyboard_Equals;
            g_keyMappings[SDL_SCANCODE_BACKSPACE] = InputID::Keyboard_Backspace;
            g_keyMappings[SDL_SCANCODE_GRAVE] = InputID::Keyboard_Tilde;
            g_keyMappings[SDL_SCANCODE_TAB] = InputID::Keyboard_Tab;
            g_keyMappings[SDL_SCANCODE_CAPSLOCK] = InputID::Keyboard_CapsLock;
            g_keyMappings[SDL_SCANCODE_RETURN] = InputID::Keyboard_Enter;
            g_keyMappings[SDL_SCANCODE_ESCAPE] = InputID::Keyboard_Escape;
            g_keyMappings[SDL_SCANCODE_SPACE] = InputID::Keyboard_Space;
            g_keyMappings[SDL_SCANCODE_LEFT] = InputID::Keyboard_Left;
            g_keyMappings[SDL_SCANCODE_UP] = InputID::Keyboard_Up;
            g_keyMappings[SDL_SCANCODE_RIGHT] = InputID::Keyboard_Right;
            g_keyMappings[SDL_SCANCODE_DOWN] = InputID::Keyboard_Down;
            g_keyMappings[SDL_SCANCODE_NUMLOCKCLEAR] = InputID::Keyboard_NumLock;
            g_keyMappings[SDL_SCANCODE_KP_0] = InputID::Keyboard_Numpad0;
            g_keyMappings[SDL_SCANCODE_KP_1] = InputID::Keyboard_Numpad1;
            g_keyMappings[SDL_SCANCODE_KP_2] = InputID::Keyboard_Numpad2;
            g_keyMappings[SDL_SCANCODE_KP_3] = InputID::Keyboard_Numpad3;
            g_keyMappings[SDL_SCANCODE_KP_4] = InputID::Keyboard_Numpad4;
            g_keyMappings[SDL_SCANCODE_KP_5] = InputID::Keyboard_Numpad5;
            g_keyMappings[SDL_SCANCODE_KP_6] = InputID::Keyboard_Numpad6;
            g_keyMappings[SDL_SCANCODE_KP_7] = InputID::Keyboard_Numpad7;
            g_keyMappings[SDL_SCANCODE_KP_8] = InputID::Keyboard_Numpad8;
            g_keyMappings[SDL_SCANCODE_KP_9] = InputID::Keyboard_Numpad9;
            g_keyMappings[SDL_SCANCODE_KP_ENTER] = InputID::Keyboard_NumpadEnter;
            g_keyMappings[SDL_SCANCODE_KP_MULTIPLY] = InputID::Keyboard_NumpadMultiply;
            g_keyMappings[SDL_SCANCODE_KP_PLUS] = InputID::Keyboard_NumpadPlus;
            g_keyMappings[SDL_SCANCODE_KP_MINUS] = InputID::Keyboard_NumpadMinus;
            g_keyMappings[SDL_SCANCODE_KP_PERIOD] = InputID::Keyboard_NumpadPeriod;
            g_keyMappings[SDL_SCANCODE_KP_DIVIDE] = InputID::Keyboard_NumpadDivide;
            g_keyMappings[SDL_SCANCODE_F1] = InputID::Keyboard_F1;
            g_keyMappings[SDL_SCANCODE_F2] = InputID::Keyboard_F2;
            g_keyMappings[SDL_SCANCODE_F3] = InputID::Keyboard_F3;
            g_keyMappings[SDL_SCANCODE_F4] = InputID::Keyboard_F4;
            g_keyMappings[SDL_SCANCODE_F5] = InputID::Keyboard_F5;
            g_keyMappings[SDL_SCANCODE_F6] = InputID::Keyboard_F6;
            g_keyMappings[SDL_SCANCODE_F7] = InputID::Keyboard_F7;
            g_keyMappings[SDL_SCANCODE_F8] = InputID::Keyboard_F8;
            g_keyMappings[SDL_SCANCODE_F9] = InputID::Keyboard_F9;
            g_keyMappings[SDL_SCANCODE_F10] = InputID::Keyboard_F10;
            g_keyMappings[SDL_SCANCODE_F11] = InputID::Keyboard_F11;
            g_keyMappings[SDL_SCANCODE_F12] = InputID::Keyboard_F12;
            g_keyMappings[SDL_SCANCODE_F13] = InputID::Keyboard_F13;
            g_keyMappings[SDL_SCANCODE_F14] = InputID::Keyboard_F14;
            g_keyMappings[SDL_SCANCODE_F15] = InputID::Keyboard_F15;
            g_keyMappings[SDL_SCANCODE_PRINTSCREEN] = InputID::Keyboard_PrintScreen;
            g_keyMappings[SDL_SCANCODE_SCROLLLOCK] = InputID::Keyboard_ScrollLock;
            g_keyMappings[SDL_SCANCODE_PAUSE] = InputID::Keyboard_Pause;
            g_keyMappings[SDL_SCANCODE_INSERT] = InputID::Keyboard_Insert;
            g_keyMappings[SDL_SCANCODE_DELETE] = InputID::Keyboard_Delete;
            g_keyMappings[SDL_SCANCODE_HOME] = InputID::Keyboard_Home;
            g_keyMappings[SDL_SCANCODE_END] = InputID::Keyboard_End;
            g_keyMappings[SDL_SCANCODE_PAGEUP] = InputID::Keyboard_PageUp;
            g_keyMappings[SDL_SCANCODE_PAGEDOWN] = InputID::Keyboard_PageDown;
            g_keyMappings[SDL_SCANCODE_APPLICATION] = InputID::Keyboard_Application;
            g_keyMappings[SDL_SCANCODE_LSHIFT] = InputID::Keyboard_LShift;
            g_keyMappings[SDL_SCANCODE_RSHIFT] = InputID::Keyboard_RShift;
            g_keyMappings[SDL_SCANCODE_LCTRL] = InputID::Keyboard_LCtrl;
            g_keyMappings[SDL_SCANCODE_RCTRL] = InputID::Keyboard_RCtrl;
            g_keyMappings[SDL_SCANCODE_LALT] = InputID::Keyboard_LAlt;
            g_keyMappings[SDL_SCANCODE_RALT] = InputID::Keyboard_RAlt;
        }

        static void Shutdown()
        {
            g_keyMappings.clear( true );
        }
    }

    //-------------------------------------------------------------------------

    void KeyboardMouseDevice::Initialize()
    {
        // There is no equivalent of RegisterRawInputDevices. SDL delivers keyboard and mouse
        // events from SDL_INIT_VIDEO, which LinuxApplication::Run has already done.
        SDLKeyMap::Initialize();
    }

    void KeyboardMouseDevice::Shutdown()
    {
        SDLKeyMap::Shutdown();
    }

    void KeyboardMouseDevice::ProcessMessage( GenericMessage const& msg )
    {
        // GenericMessage is four uint64_t and an SDL_Event is 128 bytes, so the event travels by
        // pointer. This is safe because InputSystem::ForwardInputMessageToInputDevices dispatches
        // synchronously, from inside LinuxApplication's event loop, while the event is still on
        // the stack. The Win32 sibling passes an HRAWINPUT handle through the same field.
        auto pEvent = reinterpret_cast<SDL_Event const*>( msg.m_data0 );
        EE_ASSERT( pEvent != nullptr );

        switch ( pEvent->type )
        {
            // Focus handling
            //-------------------------------------------------------------------------

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            {
                Clear();
            }
            break;

            // Record char inputs
            //-------------------------------------------------------------------------

            case SDL_EVENT_TEXT_INPUT:
            {
                // SDL_EVENT_TEXT_INPUT is UTF-8 and m_charKeyPressed is one byte, so only the
                // ASCII range fits. That matches what the Win32 sibling stores: it takes the
                // WM_CHAR code point and truncates it to a char. Anything an IME or a dead key
                // produces above 0x7F is dropped here rather than stored as a broken byte.
                // imgui does its own text input and is not affected.
                char const firstChar = pEvent->text.text[0];
                if ( firstChar > 0 )
                {
                    m_charKeyPressed = (uint8_t) firstChar;
                }
            }
            break;

            // Mouse absolute positioning and movement
            //-------------------------------------------------------------------------

            case SDL_EVENT_MOUSE_MOTION:
            {
                m_position.m_x = (int32_t) pEvent->motion.x;
                m_position.m_y = (int32_t) pEvent->motion.y;

                // xrel and yrel replace the raw input deltas the Win32 sibling accumulates.
                m_movementDelta.m_x += pEvent->motion.xrel;
                m_movementDelta.m_y += pEvent->motion.yrel;
            }
            break;

            // Mouse buttons
            //-------------------------------------------------------------------------

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                InputID buttonID = InputID::Mouse_Left;
                switch ( pEvent->button.button )
                {
                    case SDL_BUTTON_LEFT: buttonID = InputID::Mouse_Left; break;
                    case SDL_BUTTON_MIDDLE: buttonID = InputID::Mouse_Middle; break;
                    case SDL_BUTTON_RIGHT: buttonID = InputID::Mouse_Right; break;
                    case SDL_BUTTON_X1: buttonID = InputID::Mouse_Button4; break;
                    case SDL_BUTTON_X2: buttonID = InputID::Mouse_Button5; break;
                    default: return; // SDL reports any button the device has; the engine names five
                }

                if ( pEvent->type == SDL_EVENT_MOUSE_BUTTON_DOWN )
                {
                    Press( buttonID );
                }
                else
                {
                    Release( buttonID );
                }
            }
            break;

            // Mouse wheel
            //-------------------------------------------------------------------------

            case SDL_EVENT_MOUSE_WHEEL:
            {
                // Already in notches, so there is no WHEEL_DELTA to divide by. A natural
                // scrolling setting arrives as SDL_MOUSEWHEEL_FLIPPED rather than negated
                // values, so undo it here to match what Windows reports.
                float const directionScale = ( pEvent->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ) ? -1.0f : 1.0f;

                if ( pEvent->wheel.y != 0.0f )
                {
                    ApplyImpulse( InputID::Mouse_WheelVertical, pEvent->wheel.y * directionScale );
                }

                if ( pEvent->wheel.x != 0.0f )
                {
                    ApplyImpulse( InputID::Mouse_WheelHorizontal, pEvent->wheel.x * directionScale );
                }
            }
            break;

            // Keyboard
            //-------------------------------------------------------------------------

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                // Auto-repeat is dropped. Raw input does not repeat, so the Win32 sibling never
                // sees one, and Press on an already held key would restart its state.
                if ( pEvent->key.repeat )
                {
                    return;
                }

                InputID inputID;
                if ( SDLKeyMap::ConvertScancodeToInputID( pEvent->key.scancode, inputID ) )
                {
                    if ( pEvent->type == SDL_EVENT_KEY_DOWN )
                    {
                        Press( inputID );
                    }
                    else
                    {
                        Release( inputID );
                    }
                }
            }
            break;
        }
    }

    void KeyboardMouseDevice::Update( Seconds deltaTime )
    {
        SetValue( InputID::Mouse_DeltaMovementHorizontal, m_movementDelta.m_x );
        SetValue( InputID::Mouse_DeltaMovementVertical, m_movementDelta.m_y );
        InputDevice::Update( deltaTime );
    }
}
#endif
