#pragma once

#include <cstdint>

/**
 * @brief Detour-style hooking, batched into transactions.
 *
 * On Windows this is Microsoft Detours, called exactly as the runtime called it before.
 * Detours only targets x86 and x64, so the macOS build supplies its own arm64 implementation
 * in HookEngineArm64.cpp.
 *
 * The contract matches Detours':
 *   - Begin, then any number of Attach/Detach, then Commit (or Abort).
 *   - Attach takes a pointer to the caller's function pointer. On success that pointer is
 *     rewritten to something that calls the original function, so existing call sites keep
 *     working and reach the original behind the hook.
 *   - Return values are 0 (NO_ERROR) on success, non-zero otherwise.
 */
namespace HookEngine
{
/**
 * @brief Error codes returned by the arm64 engine. Detours has its own numbering on Windows.
 */
enum : int32_t
{
    Success = 0,
    ErrorNoTransaction = 1,
    ErrorInvalidTarget = 2,
    ErrorNotAttached = 3,
    ErrorAllocationFailed = 4,
    ErrorProtectionFailed = 5,

    /**
     * The first instructions of the target could not be moved to a trampoline. Rather than
     * write a half-correct copy, the engine refuses the hook. See RelocateInstruction.
     */
    ErrorUnrelocatableInstruction = 6,

    /** The trampoline was allocated but could not be made executable. */
    ErrorTrampolineProtectionFailed = 7,

    /** The target's code page could not be made writable. */
    ErrorTargetProtectionFailed = 8,
};

bool Begin();
bool Commit();
bool Abort();

/**
 * @brief Result of the last operation that failed during Commit, or Success if none did.
 *
 * Commit applies a batch and reports only whether all of it worked, which is not enough to act
 * on -- "the hook did not attach" and "the hook could not be relocated" call for different
 * responses. Detours has no equivalent, so on Windows this always reports Success.
 */
int32_t LastResult();

int32_t Attach(void** aTarget, void* aDetour);
int32_t Detach(void** aTarget, void* aDetour);
} // namespace HookEngine
