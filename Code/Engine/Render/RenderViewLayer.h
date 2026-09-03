#pragma once

#include "Engine/_Module/API.h"
#include "Base/TypeSystem/ReflectedType.h"
#include "Base/Types/BitFlags.h"

//-------------------------------------------------------------------------

namespace EE::Render
{
    // Has to match `typedef uint RenderViewLayerFlags`!
    enum class ViewLayer
    {
        EE_REFLECT_ENUM

        ShadowMap,
        ForwardShading,
        GlobalEnvironmentMap,
    };
}
