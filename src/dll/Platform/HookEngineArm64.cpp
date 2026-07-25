#include "Platform/HookEngine.hpp"
#include "Platform.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

// arm64 inline hooking, no third-party hooking library.
//
// The mechanism was proven against the live game before any of this was written (see
// probes/b2-arm64-inline-hook.c): the game ships with library validation disabled, its __TEXT
// can be made writable via mach_vm_protect with VM_PROT_COPY, and a 16-byte absolute branch
// patch works. What that probe lacked, and what the runtime needs, is the ability to displace
// ARBITRARY leading instructions -- the probe's target happened to have a position-independent
// prologue.
//
// Patch written over the target (16 bytes, four instructions' worth):
//
//     LDR  X16, #8        ; load the detour address from the two words below
//     BR   X16
//     .quad detour
//
// X16 is IP0. The procedure call standard lets a veneer clobber it at a call boundary, which
// a function's first instruction is, so the patch itself is safe.
//
// Branches back into the middle of the function are a weaker case: there the compiler could in
// principle have a live value in X16. So the trampoline is placed within a B instruction's
// +/-128 MB of the target when possible, which lets those branches be plain PC-relative ones
// that need no register at all.
//
// That is not always possible. Against this game it is in fact never possible at startup: the
// executable is 110 MB, the regions following it are contiguous, and everything below it is
// __PAGEZERO, so no address within reach can be claimed. The engine therefore falls back to an
// X16 veneer for far trampolines, which is what every arm64 hooking library does. The exposure
// is a function holding a live value in X16 across its own first four instructions -- X16 is
// IP0, which compilers use only transiently -- and the near path is preferred whenever the
// address space allows it.
//
// Anything the relocator does not fully understand also refuses the hook. A wrong relocation
// corrupts the game silently and at a distance; a refused hook is a log line.

namespace
{
constexpr size_t kPatchSize = 16;
constexpr size_t kPatchInstructions = kPatchSize / 4;

/** Reach of a B/BL immediate: 26 bits of word offset, signed. */
constexpr int64_t kBranchReach = 128ll * 1024 * 1024;

/** Words reserved per jump site, enough for the register-based form. */
constexpr size_t kJumpWords = 4;

constexpr uint32_t kNop = 0xD503201Fu;

/* ------------------------------------------------------------------------------------------ */
/* Encoding helpers                                                                             */
/* ------------------------------------------------------------------------------------------ */

/** LDR <Xt>, #<byteOffset> -- PC-relative literal load of a 64-bit value. */
uint32_t EncodeLdrLiteral64(uint32_t aRt, int32_t aByteOffset)
{
    const auto imm19 = static_cast<uint32_t>((aByteOffset / 4) & 0x7FFFF);
    return 0x58000000u | (imm19 << 5) | (aRt & 0x1F);
}

/** BR <Xn> */
uint32_t EncodeBr(uint32_t aRn)
{
    return 0xD61F0000u | ((aRn & 0x1F) << 5);
}

/** B #<byteOffset> */
uint32_t EncodeB(int64_t aByteOffset)
{
    const auto imm26 = static_cast<uint32_t>((aByteOffset / 4) & 0x3FFFFFF);
    return 0x14000000u | imm26;
}

int64_t SignExtend(uint64_t aValue, uint32_t aBits)
{
    const uint64_t sign = 1ull << (aBits - 1);
    return static_cast<int64_t>((aValue ^ sign) - sign);
}

/* ------------------------------------------------------------------------------------------ */
/* Instruction classification                                                                   */
/* ------------------------------------------------------------------------------------------ */

bool IsAdr(uint32_t aInsn)
{
    return (aInsn & 0x9F000000u) == 0x10000000u;
}

bool IsAdrp(uint32_t aInsn)
{
    return (aInsn & 0x9F000000u) == 0x90000000u;
}

bool IsUncondBranch(uint32_t aInsn) // B
{
    return (aInsn & 0xFC000000u) == 0x14000000u;
}

bool IsBranchLink(uint32_t aInsn) // BL
{
    return (aInsn & 0xFC000000u) == 0x94000000u;
}

bool IsCondBranch(uint32_t aInsn) // B.cond
{
    return (aInsn & 0xFF000010u) == 0x54000000u;
}

bool IsCompareBranch(uint32_t aInsn) // CBZ / CBNZ
{
    return (aInsn & 0x7E000000u) == 0x34000000u;
}

bool IsTestBranch(uint32_t aInsn) // TBZ / TBNZ
{
    return (aInsn & 0x7E000000u) == 0x36000000u;
}

bool IsLoadLiteral(uint32_t aInsn) // LDR / LDRSW / PRFM, literal form
{
    return (aInsn & 0x3B000000u) == 0x18000000u;
}

int64_t Adr21Offset(uint32_t aInsn)
{
    const uint64_t immlo = (aInsn >> 29) & 0x3;
    const uint64_t immhi = (aInsn >> 5) & 0x7FFFF;
    return SignExtend((immhi << 2) | immlo, 21);
}

int64_t Imm19Offset(uint32_t aInsn)
{
    return SignExtend((aInsn >> 5) & 0x7FFFF, 19) * 4;
}

int64_t Imm26Offset(uint32_t aInsn)
{
    return SignExtend(aInsn & 0x3FFFFFF, 26) * 4;
}

int64_t Imm14Offset(uint32_t aInsn)
{
    return SignExtend((aInsn >> 5) & 0x3FFF, 14) * 4;
}

/* ------------------------------------------------------------------------------------------ */
/* Relocation                                                                                   */
/* ------------------------------------------------------------------------------------------ */

/**
 * A displaced branch whose destination lies outside the copied region.
 *
 * The original opcode is kept -- so condition code, register and tested bit are preserved
 * exactly -- and only its immediate is rewritten to reach a stub appended to the trampoline.
 * The stub is a plain B to the real destination, which is why the trampoline has to be within
 * branch reach of the target.
 *
 * A displaced BL keeps its BL opcode, so it still sets LR, and the stub must NOT use BLR or it
 * would overwrite the link register the BL just set. LR ends up pointing at the next
 * trampoline instruction rather than into the middle of the patched prologue, which is the
 * behaviour we want.
 */
struct BranchFixup
{
    size_t insnIndex;  // index into the emitted word buffer
    uint64_t target;   // absolute destination
    uint32_t immBits;  // 26, 19 or 14
};

struct Relocation
{
    std::vector<uint32_t> words;
    std::vector<BranchFixup> fixups;
};

void EmitAbsoluteValueIntoRegister(std::vector<uint32_t>& aWords, uint32_t aRd, uint64_t aValue)
{
    // LDR Xd, #8 / B #12 / .quad value -- branch over the inline constant.
    aWords.push_back(EncodeLdrLiteral64(aRd, 8));
    aWords.push_back(EncodeB(12));
    aWords.push_back(static_cast<uint32_t>(aValue & 0xFFFFFFFFu));
    aWords.push_back(static_cast<uint32_t>(aValue >> 32));
}

/**
 * Rewrite one instruction so executing it from the trampoline has the effect it would have had
 * at aPc. Returns false for anything not handled, which refuses the whole hook.
 */
bool RelocateInstruction(uint32_t aInsn, uint64_t aPc, Relocation& aOut)
{
    // ADR Xd, label -> materialise the absolute address in Xd.
    if (IsAdr(aInsn))
    {
        EmitAbsoluteValueIntoRegister(aOut.words, aInsn & 0x1F, aPc + static_cast<uint64_t>(Adr21Offset(aInsn)));
        return true;
    }

    // ADRP Xd, page -> materialise the absolute page address in Xd.
    if (IsAdrp(aInsn))
    {
        const uint64_t base = aPc & ~0xFFFull;
        EmitAbsoluteValueIntoRegister(aOut.words, aInsn & 0x1F,
                                      base + (static_cast<uint64_t>(Adr21Offset(aInsn)) << 12));
        return true;
    }

    // LDR <Xt>, label -> put the literal's address in Xt, then load through it.
    if (IsLoadLiteral(aInsn))
    {
        const uint32_t opc = (aInsn >> 30) & 0x3;
        const uint32_t isVector = (aInsn >> 26) & 0x1;
        const uint32_t rt = aInsn & 0x1F;

        // A SIMD literal load needs a scratch integer register to hold the address, and no
        // register can be proven dead here. PRFM (opc 0b11, scalar) has no destination
        // register to borrow either. Refuse both rather than guess.
        if (isVector || opc == 0x3)
        {
            return false;
        }

        // Xt == XZR would make the address materialisation a no-op and the load wrong.
        if (rt == 31)
        {
            return false;
        }

        EmitAbsoluteValueIntoRegister(aOut.words, rt, aPc + static_cast<uint64_t>(Imm19Offset(aInsn)));

        switch (opc)
        {
        case 0x0: // LDR Wt, label   -> LDR Wt, [Xt]
            aOut.words.push_back(0xB9400000u | (rt << 5) | rt);
            break;
        case 0x1: // LDR Xt, label   -> LDR Xt, [Xt]
            aOut.words.push_back(0xF9400000u | (rt << 5) | rt);
            break;
        case 0x2: // LDRSW Xt, label -> LDRSW Xt, [Xt]
            aOut.words.push_back(0xB9800000u | (rt << 5) | rt);
            break;
        default:
            return false;
        }

        return true;
    }

    // B / BL: keep the opcode, retarget at a stub.
    if (IsUncondBranch(aInsn) || IsBranchLink(aInsn))
    {
        aOut.fixups.push_back({aOut.words.size(), aPc + static_cast<uint64_t>(Imm26Offset(aInsn)), 26});
        aOut.words.push_back(aInsn & 0xFC000000u);
        return true;
    }

    // B.cond / CBZ / CBNZ: imm19.
    if (IsCondBranch(aInsn) || IsCompareBranch(aInsn))
    {
        aOut.fixups.push_back({aOut.words.size(), aPc + static_cast<uint64_t>(Imm19Offset(aInsn)), 19});
        aOut.words.push_back(aInsn & ~(0x7FFFFu << 5));
        return true;
    }

    // TBZ / TBNZ: imm14.
    if (IsTestBranch(aInsn))
    {
        aOut.fixups.push_back({aOut.words.size(), aPc + static_cast<uint64_t>(Imm14Offset(aInsn)), 14});
        aOut.words.push_back(aInsn & ~(0x3FFFu << 5));
        return true;
    }

    // Everything else is position-independent and copies verbatim. BR/BLR/RET are
    // register-indirect and therefore fine.
    //
    // Not detectable here: a constant pool word sitting inside the first four instructions
    // would be decoded as an instruction. That is an inherent limitation of inline hooking,
    // shared with every library that does it.
    aOut.words.push_back(aInsn);
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* Trampoline allocation                                                                        */
/* ------------------------------------------------------------------------------------------ */

size_t PageAlign(size_t aSize)
{
    const auto pageSize = static_cast<size_t>(getpagesize());
    return (aSize + pageSize - 1) & ~(pageSize - 1);
}

/**
 * Allocate executable scratch within branch reach of aTarget.
 *
 * The trampoline has to land within a B instruction's reach of the target. If it does not, the
 * displaced branches would need a scratch register to reach their destinations, and the
 * procedure call standard does not let us clobber one in the middle of a function.
 *
 * Neither obvious allocator can be asked for a location on macOS: mmap treats its address as a
 * hint it is free to ignore, and mach_vm_allocate with VM_FLAGS_ANYWHERE ignores the address
 * outright -- pass it zero and it still returns a high address of the kernel's choosing.
 *
 * So find the hole first. Walk the process's VM regions across the reachable window, and the
 * moment a gap big enough turns up, claim it with VM_FLAGS_FIXED, which allocates exactly there
 * and fails if anything is already mapped -- so this can never clobber an existing mapping.
 *
 * Never use MAP_JIT for the resulting pages. It is granted even without the allow-jit
 * entitlement and then wedges the process at 100% CPU in a fault loop on first write, because
 * MAP_JIT pages start in execute mode and need pthread_jit_write_protect_np. RW-then-RX works.
 */
void* AllocateTrampolineNear(void* aTarget, size_t aSize)
{
    const auto target = reinterpret_cast<uintptr_t>(aTarget);
    const auto size = PageAlign(aSize);
    const auto pageSize = static_cast<uintptr_t>(getpagesize());
    const auto reach = static_cast<uintptr_t>(kBranchReach);

    // Leave a page of slack at each end so the furthest usable address is still comfortably
    // inside the branch's range.
    const uintptr_t low = target > reach ? (target - reach) + pageSize : pageSize;
    const uintptr_t high = target + reach - size - pageSize;

    spdlog::trace("Looking for {} bytes of trampoline space in [{:#x}, {:#x}] for target {:#x}", size, low, high,
                  target);

    mach_vm_address_t cursor = low;
    for (int guard = 0; guard < 4096 && cursor < high; ++guard)
    {
        mach_vm_address_t regionStart = cursor;
        mach_vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;

        const auto found = mach_vm_region(mach_task_self(), &regionStart, &regionSize, VM_REGION_BASIC_INFO_64,
                                          reinterpret_cast<vm_region_info_t>(&info), &count, &object);

        // Nothing mapped from here on, or a gap before the next region begins.
        const uintptr_t gapEnd = found == KERN_SUCCESS ? std::min<uintptr_t>(regionStart, high) : high;

        if (gapEnd > cursor && gapEnd - cursor >= size)
        {
            // Work from the end of the gap downward. That end is the side nearest the target,
            // and a gap's leading edge is often reserved rather than genuinely free -- the
            // 4 GB of __PAGEZERO below the image being the obvious case. Trying only the first
            // address in each gap made this fail against the real game while passing in a test
            // binary, where the address space is far emptier.
            // Probe from the end of the gap backward, doubling the stride each time. A gap can
            // be tens of megabytes wide while its leading pages are reserved rather than free,
            // so a fixed one-page step only ever examines a sliver of it; doubling covers the
            // whole gap in a few dozen tries while still preferring addresses near the target.
            uintptr_t candidate = (gapEnd - size) & ~(pageSize - 1);
            uintptr_t stride = pageSize;
            kern_return_t lastError = KERN_SUCCESS;

            for (int probe = 0; probe < 64 && candidate >= cursor; ++probe)
            {
                mach_vm_address_t address = candidate;
                lastError = mach_vm_allocate(mach_task_self(), &address, size, VM_FLAGS_FIXED);
                if (lastError == KERN_SUCCESS)
                {
                    return reinterpret_cast<void*>(address);
                }

                if (candidate < cursor + stride)
                {
                    break;
                }

                candidate -= stride;
                if (stride < 16u * 1024 * 1024)
                {
                    stride *= 2;
                }
            }

            spdlog::trace("    no address in this gap could be claimed, last kr={}", lastError);
        }

        spdlog::trace("  region at {:#x} size {:#x} (kr={}), gap [{:#x}, {:#x}]", static_cast<uint64_t>(regionStart),
                      static_cast<uint64_t>(regionSize), found, static_cast<uint64_t>(cursor), gapEnd);

        if (found != KERN_SUCCESS || regionSize == 0)
        {
            break;
        }

        cursor = regionStart + regionSize;
    }

    spdlog::debug("Nothing within branch reach of {:#x} is free; falling back to a far trampoline", target);
    return nullptr;
}

/**
 * Trampoline space anywhere at all, for when nothing within branch reach can be claimed. Jumps
 * out of a far trampoline have to go through X16.
 */
void* AllocateTrampolineAnywhere(size_t aSize)
{
    mach_vm_address_t address = 0;
    if (mach_vm_allocate(mach_task_self(), &address, PageAlign(aSize), VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
    {
        return nullptr;
    }

    return reinterpret_cast<void*>(address);
}

void FreeTrampoline(void* aMemory, size_t aSize)
{
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(aMemory), PageAlign(aSize));
}

bool MakeTrampolineExecutable(void* aMemory, size_t aSize)
{
    if (mprotect(aMemory, PageAlign(aSize), PROT_READ | PROT_EXEC) != 0)
    {
        return false;
    }

    Platform::FlushInstructionCache(aMemory, aSize);
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* Hook bookkeeping                                                                             */
/* ------------------------------------------------------------------------------------------ */

struct Hook
{
    void* target = nullptr;
    void* detour = nullptr;
    void* trampoline = nullptr;
    size_t trampolineSize = 0;
    uint8_t original[kPatchSize] = {};
};

struct PendingOperation
{
    enum class Kind
    {
        Attach,
        Detach
    };

    Kind kind;
    void** slot;
    void* detour;
};

/**
 * All of the engine's mutable state, reached through a function-local static.
 *
 * Not namespace-scope globals, deliberately. RED4ext enters through
 * __attribute__((constructor)), and dyld does not guarantee that a constructor function runs
 * after the C++ static initializers of its own image -- the order depends on how the linker
 * laid the initializers out. Getting that backwards means touching an unconstructed
 * std::unordered_map, which surfaces as "__next_prime overflow" thrown from deep inside libc++
 * with no indication of the real cause.
 *
 * A function-local static is constructed on first use, so the ordering question disappears.
 */
struct EngineState
{
    std::mutex mutex;
    bool inTransaction = false;
    int32_t lastResult = HookEngine::Success;
    std::vector<PendingOperation> pending;
    std::unordered_map<void*, Hook> hooks; // keyed by target
};

EngineState& State()
{
    static EngineState state;
    return state;
}

bool WritePatch(void* aTarget, const uint8_t* aBytes, size_t aSize)
{
    // Read-write, NOT execute-read-write. Apple Silicon enforces W^X: a request for write and
    // execute together is refused outright, whatever the page's maximum protection says. We
    // only need to write here, and the original protection is restored below before anything
    // executes from the page again.
    uint32_t oldProtection = 0;
    if (!Platform::ProtectMemory(aTarget, aSize, PAGE_READWRITE, oldProtection))
    {
        return false;
    }

    std::memcpy(aTarget, aBytes, aSize);

    uint32_t ignored = 0;
    Platform::ProtectMemory(aTarget, aSize, oldProtection, ignored);
    Platform::FlushInstructionCache(aTarget, aSize);

    return true;
}

int32_t ApplyAttach(void** aSlot, void* aDetour)
{
    auto* target = *aSlot;
    if (!target || !aDetour)
    {
        return HookEngine::ErrorInvalidTarget;
    }

    if (auto existing = State().hooks.find(target); existing != State().hooks.end())
    {
        // Already hooked. Hand back the existing trampoline so the caller still reaches the
        // original, which is what a repeated DetourAttach would do.
        *aSlot = existing->second.trampoline;
        return HookEngine::Success;
    }

    const auto* source = reinterpret_cast<const uint32_t*>(target);
    const auto targetAddress = reinterpret_cast<uint64_t>(target);

    Relocation relocation;
    for (size_t i = 0; i < kPatchInstructions; ++i)
    {
        if (!RelocateInstruction(source[i], targetAddress + i * 4, relocation))
        {
            return HookEngine::ErrorUnrelocatableInstruction;
        }
    }

    // Return branch, plus one stub per displaced branch. Each gets a fixed four-word slot,
    // which is what the register-based form needs; the PC-relative form uses one word and
    // pads. The addresses are only known once the trampoline has been placed, so reserve the
    // slots now and fill them in below.
    const size_t returnBranchIndex = relocation.words.size();
    relocation.words.insert(relocation.words.end(), kJumpWords, 0);

    std::vector<size_t> stubIndices;
    stubIndices.reserve(relocation.fixups.size());

    for (size_t i = 0; i < relocation.fixups.size(); ++i)
    {
        stubIndices.push_back(relocation.words.size());
        relocation.words.insert(relocation.words.end(), kJumpWords, 0);
    }

    const size_t trampolineSize = relocation.words.size() * sizeof(uint32_t);

    // Prefer a trampoline within branch reach: its jumps then need no scratch register at all.
    bool nearby = true;
    auto* trampoline = AllocateTrampolineNear(target, trampolineSize);
    if (!trampoline)
    {
        nearby = false;
        trampoline = AllocateTrampolineAnywhere(trampolineSize);
    }

    if (!trampoline)
    {
        return HookEngine::ErrorAllocationFailed;
    }

    const auto trampolineAddress = reinterpret_cast<uint64_t>(trampoline);

    // Write an absolute jump into a reserved slot, PC-relative when the destination is in
    // range and via X16 when it is not.
    const auto emitJump = [&](size_t aIndex, uint64_t aDestination)
    {
        const auto from = trampolineAddress + aIndex * 4;
        const auto delta = static_cast<int64_t>(aDestination) - static_cast<int64_t>(from);

        if (delta > -kBranchReach && delta < kBranchReach)
        {
            relocation.words[aIndex + 0] = EncodeB(delta);
            relocation.words[aIndex + 1] = kNop;
            relocation.words[aIndex + 2] = kNop;
            relocation.words[aIndex + 3] = kNop;
        }
        else
        {
            relocation.words[aIndex + 0] = EncodeLdrLiteral64(16, 8);
            relocation.words[aIndex + 1] = EncodeBr(16);
            relocation.words[aIndex + 2] = static_cast<uint32_t>(aDestination & 0xFFFFFFFFu);
            relocation.words[aIndex + 3] = static_cast<uint32_t>(aDestination >> 32);
        }
    };

    // Resume at the first instruction after the patch.
    emitJump(returnBranchIndex, targetAddress + kPatchSize);

    for (size_t i = 0; i < relocation.fixups.size(); ++i)
    {
        const auto& fixup = relocation.fixups[i];
        const auto stubIndex = stubIndices[i];

        emitJump(stubIndex, fixup.target);

        // Point the displaced branch at its stub. These immediates are small -- the stub is a
        // few words away inside the same trampoline -- so they are always in range, including
        // TBZ's 14 bits.
        const auto imm = static_cast<uint64_t>(static_cast<int64_t>(stubIndex) - static_cast<int64_t>(fixup.insnIndex));
        switch (fixup.immBits)
        {
        case 26:
            relocation.words[fixup.insnIndex] |= static_cast<uint32_t>(imm & 0x3FFFFFF);
            break;
        case 19:
            relocation.words[fixup.insnIndex] |= static_cast<uint32_t>((imm & 0x7FFFF) << 5);
            break;
        case 14:
            relocation.words[fixup.insnIndex] |= static_cast<uint32_t>((imm & 0x3FFF) << 5);
            break;
        default:
            FreeTrampoline(trampoline, trampolineSize);
            return HookEngine::ErrorUnrelocatableInstruction;
        }
    }

    spdlog::debug("Trampoline for {:#x} at {:#x} ({})", targetAddress, trampolineAddress,
                  nearby ? "within branch reach" : "far, jumps go through X16");

    std::memcpy(trampoline, relocation.words.data(), trampolineSize);
    if (!MakeTrampolineExecutable(trampoline, trampolineSize))
    {
        FreeTrampoline(trampoline, trampolineSize);
        return HookEngine::ErrorTrampolineProtectionFailed;
    }

    Hook hook;
    hook.target = target;
    hook.detour = aDetour;
    hook.trampoline = trampoline;
    hook.trampolineSize = trampolineSize;
    std::memcpy(hook.original, target, kPatchSize);

    uint8_t patch[kPatchSize];
    const uint32_t ldr = EncodeLdrLiteral64(16, 8);
    const uint32_t br = EncodeBr(16);
    const auto detourAddress = reinterpret_cast<uint64_t>(aDetour);

    std::memcpy(patch + 0, &ldr, 4);
    std::memcpy(patch + 4, &br, 4);
    std::memcpy(patch + 8, &detourAddress, 8);

    if (!WritePatch(target, patch, kPatchSize))
    {
        FreeTrampoline(trampoline, trampolineSize);
        return HookEngine::ErrorTargetProtectionFailed;
    }

    State().hooks.emplace(target, hook);

    // Hand the caller the trampoline: calls through the original pointer now run the displaced
    // prologue and then the untouched remainder of the function.
    *aSlot = trampoline;
    return HookEngine::Success;
}

int32_t ApplyDetach(void** aSlot, void* aDetour)
{
    // After a successful Attach the slot holds the trampoline, so match on either.
    for (auto it = State().hooks.begin(); it != State().hooks.end(); ++it)
    {
        auto& hook = it->second;
        if (hook.trampoline != *aSlot && hook.target != *aSlot)
        {
            continue;
        }

        if (aDetour && hook.detour != aDetour)
        {
            continue;
        }

        if (!WritePatch(hook.target, hook.original, kPatchSize))
        {
            return HookEngine::ErrorTargetProtectionFailed;
        }

        *aSlot = hook.target;

        // The trampoline is deliberately leaked. Another thread may be executing inside it
        // right now and there is no cheap way to know; it is at most a page per hook, and
        // hooks are detached once, at shutdown.
        State().hooks.erase(it);
        return HookEngine::Success;
    }

    return HookEngine::ErrorNotAttached;
}
} // namespace

bool HookEngine::Begin()
{
    std::lock_guard lock(State().mutex);
    if (State().inTransaction)
    {
        return false;
    }

    State().pending.clear();
    State().inTransaction = true;

    return true;
}

bool HookEngine::Commit()
{
    std::lock_guard lock(State().mutex);
    if (!State().inTransaction)
    {
        return false;
    }

    bool success = true;
    State().lastResult = Success;

    for (const auto& op : State().pending)
    {
        const auto result = op.kind == PendingOperation::Kind::Attach ? ApplyAttach(op.slot, op.detour)
                                                                     : ApplyDetach(op.slot, op.detour);
        if (result != Success)
        {
            State().lastResult = result;
            success = false;
        }
    }

    State().pending.clear();
    State().inTransaction = false;

    return success;
}

int32_t HookEngine::LastResult()
{
    std::lock_guard lock(State().mutex);
    return State().lastResult;
}

bool HookEngine::Abort()
{
    std::lock_guard lock(State().mutex);
    if (!State().inTransaction)
    {
        return false;
    }

    State().pending.clear();
    State().inTransaction = false;

    return true;
}

int32_t HookEngine::Attach(void** aTarget, void* aDetour)
{
    std::lock_guard lock(State().mutex);
    if (!State().inTransaction)
    {
        return ErrorNoTransaction;
    }

    if (!aTarget || !*aTarget || !aDetour)
    {
        return ErrorInvalidTarget;
    }

    State().pending.push_back({PendingOperation::Kind::Attach, aTarget, aDetour});
    return Success;
}

int32_t HookEngine::Detach(void** aTarget, void* aDetour)
{
    std::lock_guard lock(State().mutex);
    if (!State().inTransaction)
    {
        return ErrorNoTransaction;
    }

    if (!aTarget || !*aTarget)
    {
        return ErrorInvalidTarget;
    }

    State().pending.push_back({PendingOperation::Kind::Detach, aTarget, aDetour});
    return Success;
}
