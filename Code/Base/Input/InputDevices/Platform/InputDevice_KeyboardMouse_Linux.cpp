#ifdef __linux__
#include "../InputDevice_KeyboardMouse.h"

//-------------------------------------------------------------------------
// Keyboard and mouse input
//-------------------------------------------------------------------------
// Phase 1 stubs. The Win32 device is excluded from the Linux build, and without these
// definitions the KeyboardMouseDevice vtable is undefined and Esoterica.Base does not link.
//
// Phase 6 replaces them with an SDL3 implementation. See
// Docs/Linux/Phases/Phase6-WindowingInput.md.
//-------------------------------------------------------------------------

namespace EE::Input
{
    void KeyboardMouseDevice::Initialize()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void KeyboardMouseDevice::Shutdown()
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void KeyboardMouseDevice::ProcessMessage( GenericMessage const& message )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }

    void KeyboardMouseDevice::Update( Seconds deltaTime )
    {
        EE_UNIMPLEMENTED_FUNCTION();
    }
}
#endif
