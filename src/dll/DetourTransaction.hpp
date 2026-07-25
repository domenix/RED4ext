#pragma once

/**
 * @brief RAII scope for a batch of hook attach/detach operations.
 *
 * The Windows-specific machinery -- Detours itself, the process heap lock, and enumerating
 * threads to queue for update -- now lives behind HookEngine, so this class is identical on
 * every platform and its behaviour on Windows is unchanged.
 */
class DetourTransaction
{
public:
    DetourTransaction(const std::source_location aSource = std::source_location::current());
    ~DetourTransaction();

    DetourTransaction(DetourTransaction&) = delete;
    DetourTransaction(DetourTransaction&&) = delete;

    DetourTransaction& operator=(const DetourTransaction&) = delete;
    DetourTransaction& operator=(DetourTransaction&&) = delete;

    const bool IsValid() const;

    bool Commit();
    bool Abort();

private:
    enum class State : uint8_t
    {
        Invalid,
        Started,
        Committed,
        Aborted,
        Failed
    };

    void SetState(const State aState);

    const std::source_location m_source;
    State m_state;
};
