#pragma once
#ifdef __linux__

//-------------------------------------------------------------------------
// Minimal Microsoft::WRL::ComPtr replacement
//-------------------------------------------------------------------------
// ShaderReflection_ShaderCompiler.cpp holds DXC interfaces in Microsoft::WRL::ComPtr, from
// <wrl/client.h>. That header is part of the Windows SDK and has no Linux equivalent.
//
// DXC's own WinAdapter.h ships CComPtr, but its API is the older ATL shape: it has no Get(),
// GetAddressOf() or ReleaseAndGetAddressOf(), which is what the call sites use. Providing those
// three here keeps all eleven call sites unedited, which is the point.
//
// This is deliberately not a general-purpose COM pointer. It does what the shader compiler
// needs and nothing else.
//-------------------------------------------------------------------------

#include "dxcapi.h"

namespace EE::Reflection
{
    template<typename T>
    class ComPtr
    {
    public:

        ComPtr() = default;
        ComPtr( ComPtr const& rhs ) : m_pPointer( rhs.m_pPointer ) { AddRef(); }
        ~ComPtr() { Release(); }

        ComPtr& operator=( ComPtr const& rhs )
        {
            if ( this != &rhs )
            {
                Release();
                m_pPointer = rhs.m_pPointer;
                AddRef();
            }
            return *this;
        }

        // The call sites write `ComPtr<T> x = {};`, so this has to accept an empty braced list.
        ComPtr( decltype( nullptr ) ) {}

        T* Get() const { return m_pPointer; }
        T* operator->() const { return m_pPointer; }
        explicit operator bool() const { return m_pPointer != nullptr; }

        // Hands out the slot for an out-parameter without releasing first. Matches WRL.
        T** GetAddressOf() { return &m_pPointer; }

        // Releases any existing pointer first, then hands out the slot. Matches WRL.
        T** ReleaseAndGetAddressOf()
        {
            Release();
            return &m_pPointer;
        }

    private:

        void AddRef()
        {
            if ( m_pPointer != nullptr )
            {
                m_pPointer->AddRef();
            }
        }

        void Release()
        {
            if ( m_pPointer != nullptr )
            {
                m_pPointer->Release();
                m_pPointer = nullptr;
            }
        }

    private:

        T* m_pPointer = nullptr;
    };
}

using EE::Reflection::ComPtr;

#endif
