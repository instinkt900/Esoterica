#include "RenderMaterialShaderClusterCapacity.h"

//-------------------------------------------------------------------------

namespace EE::Render
{
    void MaterialShaderClusterCapacity::Initialize()
    {
        m_clusterCapacityPerShader.clear();
    }

    void MaterialShaderClusterCapacity::Shutdown()
    {
        for ( uint32_t capacity : m_clusterCapacityPerShader )
        {
            EE_ASSERT( capacity == 1 );
        }

        m_clusterCapacityPerShader.clear();
    }

    void MaterialShaderClusterCapacity::AddShaderClusters( size_t shaderIndex, uint32_t numClusters )
    {
        if ( shaderIndex >= m_clusterCapacityPerShader.size() )
        {
            // New shader slots start with a baseline capacity of 1 cluster
            m_clusterCapacityPerShader.resize( shaderIndex + 1, 1 );
        }

        m_clusterCapacityPerShader[shaderIndex] += numClusters;
    }

    void MaterialShaderClusterCapacity::RemoveShaderClusters( size_t shaderIndex, uint32_t numClusters )
    {
        EE_ASSERT( shaderIndex < m_clusterCapacityPerShader.size() );
        EE_ASSERT( m_clusterCapacityPerShader[shaderIndex] > numClusters );

        m_clusterCapacityPerShader[shaderIndex] -= numClusters;
    }

    void MaterialShaderClusterCapacity::Validate( size_t numShaders )
    {
        m_clusterCapacityPerShader.resize( numShaders, 1 );
    }

    TArrayView<uint32_t const> MaterialShaderClusterCapacity::GetShaderClusterCapacity() const
    {
        return m_clusterCapacityPerShader;
    }

    uint32_t MaterialShaderClusterCapacity::GetAllShadersClusterCapacity() const
    {
        uint32_t totalCapacity = 0;
        for ( uint32_t capacity : m_clusterCapacityPerShader )
        {
            totalCapacity += capacity;
        }
        return totalCapacity;
    }
}
