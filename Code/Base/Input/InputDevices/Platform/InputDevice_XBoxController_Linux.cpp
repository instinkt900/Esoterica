#ifdef __linux__
#include "../InputDevice_XBoxController.h"

//-------------------------------------------------------------------------
// Xbox controller input
//-------------------------------------------------------------------------
// Phase 1 stubs. The Win32 device uses XInput and is excluded from the Linux build, so without
// these the XBoxControllerInputDevice vtable is undefined and Esoterica.Base does not link.
//
// Phase 6 replaces them with SDL3's game controller API. See
// Docs/Linux/Phases/Phase6-WindowingInput.md.
//
// The dead zone and threshold getters return the Win32 values rather than halting: they are
// pure constants, they are read during device construction before anything is plugged in, and
// halting there would stop the engine starting at all.
//-------------------------------------------------------------------------

namespace EE::Input
{
    void XBoxControllerInputDevice::Initialize()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void XBoxControllerInputDevice::Shutdown()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void XBoxControllerInputDevice::Update( Seconds deltaTime )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    //-------------------------------------------------------------------------

    // The XInput constants, so the Linux device reports the same dead zones as the Win32 one:
    // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE 7849, RIGHT 8689, TRIGGER_THRESHOLD 30, over ranges of
    // 32767 and 255. Named here because <Xinput.h> is not available.
    namespace
    {
        constexpr float const g_maxThumbstickRange = 32767.0f;
        constexpr float const g_maxTriggerRange = 255.0f;
    }

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
        return 30.0f / g_maxTriggerRange;
    }
}
#endif
