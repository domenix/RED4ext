#include "Platform/HookEngine.hpp"

#include <Windows.h>
#include <winternl.h>

#include <detours.h>
#include <spdlog/spdlog.h>
#include <wil/resource.h>

#include <vector>

// Windows hooking, unchanged from what DetourTransaction.cpp did before the platform split:
// Microsoft Detours, with the process heap locked and every other thread queued for update
// while the transaction is open.

extern "C" NTSYSCALLAPI NTSTATUS NTAPI NtGetNextThread(_In_ HANDLE ProcessHandle, _In_opt_ HANDLE ThreadHandle,
                                                       _In_ ACCESS_MASK DesiredAccess, _In_ ULONG HandleAttributes,
                                                       _In_opt_ _Reserved_ ULONG Flags, _Out_ PHANDLE NewThreadHandle);

namespace
{
bool g_hasHeapLock = false;
std::vector<wil::unique_handle> g_handles;

bool QueueThreadsForUpdate()
{
    spdlog::trace("Queueing threads for detour update...");

    const HANDLE currentProcess = GetCurrentProcess();
    const DWORD currentThreadId = GetCurrentThreadId();

    static constexpr ACCESS_MASK threadAccess =
        THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME;

    HANDLE thread = nullptr;
    bool closePrevThread = false;

    while (true)
    {
        HANDLE nextThread = nullptr;

        NTSTATUS status = NtGetNextThread(currentProcess, thread, threadAccess, 0, 0, &nextThread);
        if (closePrevThread)
        {
            CloseHandle(thread);
        }

        if (!NT_SUCCESS(status))
        {
            break;
        }

        thread = nextThread;
        closePrevThread = true;

        const DWORD threadId = GetThreadId(thread);
        if (threadId == 0)
        {
            auto lastError = GetLastError();
            spdlog::warn("Could not get thread ID. handle: {}, lastError: {}", thread, lastError);

            continue;
        }

        if (threadId == currentThreadId)
        {
            continue;
        }

        // https://ntdoc.m417z.com/threadinfoclass
        static constexpr THREADINFOCLASS ThreadIsTerminated = (THREADINFOCLASS)0x14;

        BOOL isTerminated = FALSE;
        status = NtQueryInformationThread(thread, ThreadIsTerminated, &isTerminated, sizeof(isTerminated), nullptr);
        if (!NT_SUCCESS(status))
        {
            spdlog::warn("Could not query thread information. threadId: {}, handle: {}, status: {}", threadId, thread,
                         status);
            continue;
        }

        if (isTerminated)
        {
            continue;
        }

        const LONG result = DetourUpdateThread(thread);
        if (result == NO_ERROR)
        {
            g_handles.emplace_back(thread);
            closePrevThread = false;
        }
        else
        {
            spdlog::warn("Could not queue the thread for update. threadId: {}, handle: {}, error code: {}", threadId,
                         thread, result);
            return false;
        }
    }

    spdlog::trace("{} thread(s) queued for detour update (excl. current thread)", g_handles.size());
    return true;
}

void ReleaseHeapLock()
{
    if (g_hasHeapLock)
    {
        HeapUnlock(GetProcessHeap());
        g_hasHeapLock = false;
    }
}
} // namespace

bool HookEngine::Begin()
{
    if (!HeapLock(GetProcessHeap()))
    {
        spdlog::error("Could not lock the process heap. Last error: {}", GetLastError());
        return false;
    }

    g_hasHeapLock = true;
    g_handles.clear();

    auto result = DetourTransactionBegin();
    if (result != NO_ERROR)
    {
        spdlog::error("Could not start the detour transaction. Detour error code: {}", result);
        ReleaseHeapLock();

        return false;
    }

    return true;
}

bool HookEngine::Commit()
{
    if (!QueueThreadsForUpdate())
    {
        spdlog::error("Cannot continue with committing the transaction due to failure in queuing threads for update");
        Abort();

        return false;
    }

    auto result = DetourTransactionCommit();
    g_handles.clear();
    ReleaseHeapLock();

    if (result != NO_ERROR)
    {
        // Detours already aborts the transaction if commit fails.
        spdlog::error("Could not commit the transaction. Detours error code: {}", result);
        return false;
    }

    return true;
}

bool HookEngine::Abort()
{
    auto result = DetourTransactionAbort();
    g_handles.clear();
    ReleaseHeapLock();

    if (result != NO_ERROR)
    {
        spdlog::error("Could not abort the transaction. Detours error code: {}", result);
        return false;
    }

    return true;
}

int32_t HookEngine::LastResult()
{
    // Detours reports per-call, not per-commit; there is nothing extra to remember.
    return Success;
}

int32_t HookEngine::Attach(void** aTarget, void* aDetour)
{
    return DetourAttach(aTarget, aDetour);
}

int32_t HookEngine::Detach(void** aTarget, void* aDetour)
{
    return DetourDetach(aTarget, aDetour);
}
