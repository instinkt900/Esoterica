#pragma once

#include "Engine/_Module/API.h"
#include "Base/Types/BitFlags.h"
#include "Base/TypeSystem/ReflectedType.h"

//-------------------------------------------------------------------------

namespace EE::Render
{
    class EE_ENGINE_API MaterialShaderClusterCapacity
    {
    public:

        void Initialize();
        void Shutdown();

        void AddShaderClusters( size_t shaderIndex, uint32_t numClusters );
        void RemoveShaderClusters( size_t shaderIndex, uint32_t numClusters );

        void Validate( size_t numShaders );

        TArrayView<uint32_t const> GetShaderClusterCapacity() const;
        uint32_t GetAllShadersClusterCapacity() const;

    private:

        TVector<uint32_t>               m_clusterCapacityPerShader;
    };
}
