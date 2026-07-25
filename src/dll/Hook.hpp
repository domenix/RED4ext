#pragma once

#include "Addresses.hpp"
#include "Platform/HookEngine.hpp"

template<typename T>
class Hook
{
public:
    Hook(T aAddress, T aDetour)
        : m_isAttached(false)
        , m_address(aAddress)
        , m_detour(aDetour)
        , m_hash(0)
    {
    }

    Hook(std::uint32_t aHash, T aDetour)
        : Hook(reinterpret_cast<T>(0), aDetour)
    {
        m_hash = aHash;
    }

    operator T() const
    {
        return m_address;
    }

    uintptr_t GetAddress() const
    {
        if (m_address == 0)
        {
            const auto address = Addresses::Instance();
            m_address = reinterpret_cast<T>(address->Resolve(m_hash));
        }

        return reinterpret_cast<uintptr_t>(m_address);
    }

    int32_t Attach()
    {
        if (m_isAttached)
        {
            return 0;
        }

        if (m_address == 0)
        {
            m_address = reinterpret_cast<T>(GetAddress());
        }

        // Nothing resolved this target's address. On macOS that is the normal outcome for a
        // hook whose entry point has not been located in the Mac binary yet; attaching to
        // address 0 would take the process down, so report it instead.
        if (m_address == 0)
        {
            return HookEngine::ErrorInvalidTarget;
        }

        auto result = HookEngine::Attach(reinterpret_cast<void**>(&m_address), reinterpret_cast<void*>(m_detour));
        m_isAttached = result == NO_ERROR;

        return result;
    }

    int32_t Detach()
    {
        if (!m_isAttached)
        {
            return 0;
        }

        auto result = HookEngine::Detach(reinterpret_cast<void**>(&m_address), reinterpret_cast<void*>(m_detour));
        m_isAttached = result == NO_ERROR;

        return result;
    }

private:
    bool m_isAttached;
    mutable T m_address;
    T m_detour;

    uint32_t m_hash;
};
