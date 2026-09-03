#ifdef __linux__
#include "Base/Input/InputDevices/InputDevice_XBoxController.h"
#include <SDL3/SDL.h>

//-------------------------------------------------------------------------

namespace EE::Input
{
    // SDL reports stick axes over the full joystick range, the same as XInput's thumbsticks.
    static float const g_maxThumbstickRange = 32767.0f;

    // Triggers are the one range that differs. XInput reports a byte, 0 to 255; SDL reports
    // 0 to SDL_JOYSTICK_AXIS_MAX. Both normalize to 0..1, so only this divisor changes and the
    // threshold below stays the XInput value.
    static float const g_maxTriggerRange = 32767.0f;
    static float const g_maxXInputTriggerRange = 255.0f;

    //-------------------------------------------------------------------------

    namespace
    {
        // XBoxControllerInputDevice has no member to hold an SDL_Gamepad* and its header is upstream,
        // so the open handles live here instead, indexed by hardware controller index. XInput needs
        // no such thing: it polls a slot number and the OS owns the connection.
        //
        // Four is headroom over the two devices InputSystem creates. An index outside it is treated
        // as permanently disconnected rather than as an error.
        constexpr uint32_t const g_maxTrackedControllers = 4;

        SDL_Gamepad* g_pOpenGamepads[g_maxTrackedControllers] = {};
        SDL_JoystickID g_openGamepadIDs[g_maxTrackedControllers] = {};

        // Returns the joystick ID currently sitting at the given slot, or 0 for an empty slot.
        // This is the SDL equivalent of asking XInput about controller N: the slot is a position
        // in SDL_GetGamepads' list, and a device unplugged earlier in the list shifts the rest
        // down, which is why the ID is re-read every frame rather than cached.
        SDL_JoystickID GetGamepadIDForSlot( uint32_t slot )
        {
            int numGamepads = 0;
            SDL_JoystickID* pGamepadIDs = SDL_GetGamepads( &numGamepads );
            if ( pGamepadIDs == nullptr )
            {
                return 0;
            }

            SDL_JoystickID const gamepadID = ( (int) slot < numGamepads ) ? pGamepadIDs[slot] : 0;
            SDL_free( pGamepadIDs );
            return gamepadID;
        }

        // Opens, closes or reopens the slot's gamepad so that it matches what is plugged in now,
        // and returns the handle to read this frame.
        SDL_Gamepad* UpdateOpenGamepadForSlot( uint32_t slot )
        {
            if ( slot >= g_maxTrackedControllers )
            {
                return nullptr;
            }

            SDL_JoystickID const gamepadID = GetGamepadIDForSlot( slot );
            if ( gamepadID == g_openGamepadIDs[slot] )
            {
                return g_pOpenGamepads[slot];
            }

            // The slot changed: either it emptied, or a different device is there now.
            if ( g_pOpenGamepads[slot] != nullptr )
            {
                SDL_CloseGamepad( g_pOpenGamepads[slot] );
                g_pOpenGamepads[slot] = nullptr;
            }

            g_openGamepadIDs[slot] = gamepadID;

            if ( gamepadID != 0 )
            {
                g_pOpenGamepads[slot] = SDL_OpenGamepad( gamepadID );
                if ( g_pOpenGamepads[slot] == nullptr )
                {
                    EE_LOG_WARNING( LogCategory::Input, nullptr, "Failed to open gamepad %u: %s", gamepadID, SDL_GetError() );
                    g_openGamepadIDs[slot] = 0;
                }
                else
                {
                    EE_LOG_MESSAGE( LogCategory::Input, nullptr, "Controller %u connected: %s", slot, SDL_GetGamepadName( g_pOpenGamepads[slot] ) );
                }
            }

            return g_pOpenGamepads[slot];
        }

        void CloseGamepadForSlot( uint32_t slot )
        {
            if ( slot < g_maxTrackedControllers && g_pOpenGamepads[slot] != nullptr )
            {
                SDL_CloseGamepad( g_pOpenGamepads[slot] );
                g_pOpenGamepads[slot] = nullptr;
                g_openGamepadIDs[slot] = 0;
            }
        }
    }

    //-------------------------------------------------------------------------

    void XBoxControllerInputDevice::Initialize()
    {
        // The subsystem is initialized here rather than by the application, so that anything
        // holding an InputSystem gets working gamepads without knowing about SDL. SDL reference
        // counts subsystems, so both controller devices doing this is correct.
        if ( !SDL_InitSubSystem( SDL_INIT_GAMEPAD ) )
        {
            EE_LOG_ERROR( LogCategory::Input, nullptr, "SDL gamepad subsystem failed to initialize: %s", SDL_GetError() );
            m_isConnected = false;
            return;
        }

        m_isConnected = UpdateOpenGamepadForSlot( m_hardwareControllerIdx ) != nullptr;
    }

    void XBoxControllerInputDevice::Shutdown()
    {
        CloseGamepadForSlot( m_hardwareControllerIdx );
        SDL_QuitSubSystem( SDL_INIT_GAMEPAD );
        m_isConnected = false;
    }

    void XBoxControllerInputDevice::Update( Seconds deltaTime )
    {
        // XInputGetState reads the device there and then, so this polls rather than relying on
        // the application's event loop to have pumped recently.
        SDL_UpdateGamepads();

        SDL_Gamepad* pGamepad = UpdateOpenGamepadForSlot( m_hardwareControllerIdx );
        m_isConnected = ( pGamepad != nullptr );
        if ( m_isConnected )
        {
            // Set stick and trigger raw normalized values
            SetTriggerValues( SDL_GetGamepadAxis( pGamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER ) / g_maxTriggerRange, SDL_GetGamepadAxis( pGamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ) / g_maxTriggerRange );

            // The vertical axes are negated. SDL follows the joystick convention where down is
            // positive, XInput's sThumbLY is positive upwards, and the engine is written against
            // XInput. Without this every controller looks inverted on Linux only.
            Float2 const leftStick( SDL_GetGamepadAxis( pGamepad, SDL_GAMEPAD_AXIS_LEFTX ) / g_maxThumbstickRange, -SDL_GetGamepadAxis( pGamepad, SDL_GAMEPAD_AXIS_LEFTY ) / g_maxThumbstickRange );
            Float2 const rightStick( SDL_GetGamepadAxis( pGamepad, SDL_GAMEPAD_AXIS_RIGHTX ) / g_maxThumbstickRange, -SDL_GetGamepadAxis( pGamepad, SDL_GAMEPAD_AXIS_RIGHTY ) / g_maxThumbstickRange );
            SetAnalogStickValues( leftStick, rightStick );

            // Set button state. SDL3 names the face buttons by position rather than by letter,
            // which is what the engine's InputIDs mean, so SOUTH is the XInput A button.
            UpdateControllerButtonState( InputID::Controller_DPadUp, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_DPAD_UP ) );
            UpdateControllerButtonState( InputID::Controller_DPadDown, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN ) );
            UpdateControllerButtonState( InputID::Controller_DPadLeft, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT ) );
            UpdateControllerButtonState( InputID::Controller_DPadRight, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT ) );
            UpdateControllerButtonState( InputID::Controller_System0, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_BACK ) );
            UpdateControllerButtonState( InputID::Controller_System1, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_START ) );
            UpdateControllerButtonState( InputID::Controller_LeftStick, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK ) );
            UpdateControllerButtonState( InputID::Controller_RightStick, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK ) );
            UpdateControllerButtonState( InputID::Controller_LeftShoulder, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ) );
            UpdateControllerButtonState( InputID::Controller_RightShoulder, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ) );
            UpdateControllerButtonState( InputID::Controller_FaceButtonDown, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_SOUTH ) );
            UpdateControllerButtonState( InputID::Controller_FaceButtonRight, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_EAST ) );
            UpdateControllerButtonState( InputID::Controller_FaceButtonLeft, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_WEST ) );
            UpdateControllerButtonState( InputID::Controller_FaceButtonUp, SDL_GetGamepadButton( pGamepad, SDL_GAMEPAD_BUTTON_NORTH ) );
        }

        ControllerDevice::Update( deltaTime );
    }

    //-------------------------------------------------------------------------

    // The XInput constants, so a controller reports the same dead zones on both platforms:
    // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE 7849, RIGHT 8689, TRIGGER_THRESHOLD 30. Named here
    // because <Xinput.h> is not available. All three are normalized, so they carry over even
    // though SDL's raw trigger range differs.

    Float2 XBoxControllerInputDevice::GetDefaultLeftStickDeadZones() const
    {
        return Float2( 7849.0f / g_maxThumbstickRange, 0.0f );
    }

    Float2 XBoxControllerInputDevice::GetDefaultRightStickDeadZones() const
    {
        return Float2( 8689.0f / g_maxThumbstickRange, 0.0f );
    }

    float XBoxControllerInputDevice::GetDefaultTriggerThreshold() const
    {
        return 30.0f / g_maxXInputTriggerRange;
    }
}
#endif
