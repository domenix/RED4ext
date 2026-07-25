#include "DetourTransaction.hpp"
#include "Platform/HookEngine.hpp"
#include "Utils.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <source_location>

DetourTransaction::DetourTransaction(const std::source_location aSource)
    : m_source(aSource)
    , m_state(State::Invalid)
{
    spdlog::trace("Trying to start a detour transaction in '{}' ({}:{})", m_source.function_name(),
                  m_source.file_name(), m_source.line());

    if (HookEngine::Begin())
    {
        spdlog::trace("Transaction was started successfully in '{}' ({}:{})", m_source.function_name(),
                      m_source.file_name(), m_source.line());

        SetState(State::Started);
    }
    else
    {
        spdlog::error("Could not start the detour transaction in '{}' ({}:{})", m_source.function_name(),
                      m_source.file_name(), m_source.line());
    }
}

DetourTransaction::~DetourTransaction()
{
    // Abort if the transaction is dangling.
    if (m_state == State::Started)
    {
        Abort();
    }
}

const bool DetourTransaction::IsValid() const
{
    return m_state != State::Invalid;
}

bool DetourTransaction::Commit()
{
    spdlog::trace("Committing the transaction...");

    if (m_state != State::Started && m_state != State::Failed)
    {
        switch (m_state)
        {
        case State::Invalid:
        {
            spdlog::warn("The transaction is in an invalid state");
            break;
        }
        case State::Committed:
        {
            spdlog::warn("The transaction is already committed");
            break;
        }
        case State::Aborted:
        {
            spdlog::warn("The transaction is aborted, can not commit it");
            break;
        }
        default:
        {
            spdlog::warn("Unknown transaction state. State: {}", static_cast<int32_t>(m_state));
            break;
        }
        }

        return false;
    }

    if (!HookEngine::Commit())
    {
        // The engine aborts the transaction itself when a commit fails.
        spdlog::error("Could not commit the transaction. Hook engine result: {}", HookEngine::LastResult());
        SetState(State::Aborted);

        return false;
    }

    SetState(State::Committed);
    spdlog::trace("The transaction was committed successfully");

    return true;
}

bool DetourTransaction::Abort()
{
    spdlog::trace("Aborting the transaction...");

    if (m_state != State::Started && m_state != State::Failed)
    {
        switch (m_state)
        {
        case State::Invalid:
        {
            spdlog::warn("The transaction is in an invalid state");
            break;
        }
        case State::Committed:
        {
            spdlog::warn("The transaction is committed, can not abort it");
            break;
        }
        case State::Aborted:
        {
            spdlog::warn("The transaction is already aborted");
            break;
        }
        default:
        {
            spdlog::warn("Unknown transaction state. State: {}", static_cast<int32_t>(m_state));
            break;
        }
        }

        return false;
    }

    if (!HookEngine::Abort())
    {
        // If this happens, we can't abort it.
        SetState(State::Failed);
        return false;
    }

    SetState(State::Aborted);
    spdlog::trace("The transaction was aborted successfully");

    return true;
}

void DetourTransaction::SetState(const State aState)
{
    m_state = aState;
}
