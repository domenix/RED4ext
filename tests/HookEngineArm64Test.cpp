// Standalone test for the arm64 hook engine's instruction relocator.
//
// The relocator is the one piece of the port that fails silently and at a distance when it is
// wrong: a mis-relocated prologue does not crash where the mistake is, it corrupts whatever the
// displaced instruction was supposed to compute. So test it away from the game first.
//
// Each case below is a function written in assembly with a deliberately chosen prologue, one
// per relocation class the engine claims to handle. For each we check three things:
//
//   1. the function returns its known value before hooking,
//   2. after hooking, calling it reaches the detour,
//   3. calling through the trampoline still returns the original value -- which is only true if
//      the displaced instructions were rewritten correctly.
//
// Built and run by the macOS CI job, and by probes/d1-hookengine-relocator-test.sh in the
// porting repo.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Platform/HookEngine.hpp"

extern "C"
{
    // 1. Position-independent prologue. Nothing to relocate; the baseline.
    uint32_t TestPlain();

    // 2. ADR -- PC-relative address of a nearby label.
    uint32_t TestAdr();

    // 3. ADRP + ADD -- PC-relative page address, the most common prologue shape in real code.
    uint32_t TestAdrp();

    // 4. LDR literal -- loads a constant through a PC-relative offset.
    uint32_t TestLdrLiteral();

    // 5. B -- an unconditional branch out of the displaced region.
    uint32_t TestBranch();

    // 6. CBZ -- a conditional branch out of the displaced region (imm19).
    uint32_t TestCompareBranch();

    // 7. TBZ -- a bit-test branch out of the displaced region (imm14).
    uint32_t TestTestBranch();

    // 8. BL -- a call out of the displaced region; LR must survive.
    uint32_t TestBranchLink();
}

// The test targets are forced onto pages of their own.
//
// Hooking makes the target's page copy-on-write, and doing that to the page the CPU is
// currently executing from raises SIGBUS. That cannot happen in the real runtime -- RED4ext
// lives in its own dylib, always a different page from the game code it patches -- but it does
// happen here if the harness and the targets share a page, which is purely an artefact of them
// being in one binary.
asm(R"(
    .section __TEXT,__text
    .p2align 14

// --- 1. plain -------------------------------------------------------------------------------
_TestPlain:
    mov  w0, #0x11
    nop
    nop
    ret

// --- 2. ADR ---------------------------------------------------------------------------------
// The literal has to sit outside the first 16 bytes, or the engine would copy data as code.
_TestAdr:
    adr  x1, LAdrData
    nop
    nop
    ldr  w0, [x1]
    ret
    .p2align 2
LAdrData:
    .word 0x22

// --- 3. ADRP + ADD --------------------------------------------------------------------------
_TestAdrp:
    adrp x1, _gAdrpValue@PAGE
    add  x1, x1, _gAdrpValue@PAGEOFF
    nop
    nop
    ldr  w0, [x1]
    ret

// --- 4. LDR literal -------------------------------------------------------------------------
_TestLdrLiteral:
    ldr  w0, LLdrData
    nop
    nop
    ret
    .p2align 2
LLdrData:
    .word 0x44

// --- 5. B -----------------------------------------------------------------------------------
_TestBranch:
    b    LBranchTarget
    nop
    nop
    nop
LBranchTarget:
    mov  w0, #0x55
    ret

// --- 6. CBZ ---------------------------------------------------------------------------------
// Called with w0 == 0, so the branch is always taken.
_TestCompareBranch:
    mov  w0, wzr
    cbz  w0, LCbzTarget
    mov  w0, #0xBAD
    ret
LCbzTarget:
    mov  w0, #0x66
    ret

// --- 7. TBZ ---------------------------------------------------------------------------------
_TestTestBranch:
    mov  w0, wzr
    tbz  w0, #0, LTbzTarget
    mov  w0, #0xBAD
    ret
LTbzTarget:
    mov  w0, #0x77
    ret

// --- 8. BL ----------------------------------------------------------------------------------
// The BL is inside the displaced region, so the engine keeps the BL opcode and points it at a
// stub. If the stub used BLR it would clobber the link register and this would never return.
_TestBranchLink:
    stp  x29, x30, [sp, #-16]!
    bl   LBlHelper
    nop
    ldp  x29, x30, [sp], #16
    ret
LBlHelper:
    mov  w0, #0x88
    ret

    // Pad out to a page boundary so nothing the harness executes shares a page with the
    // targets above.
    .p2align 14
    .space 16384

    .section __DATA,__data
    .p2align 2
_gAdrpValue:
    .word 0x33
)");

namespace
{
int g_failures = 0;
int g_detourHits = 0;

uint32_t DetourFn()
{
    ++g_detourHits;
    return 0xDEAD;
}

using Fn = uint32_t (*)();

struct Case
{
    const char* name;
    Fn function;
    uint32_t expected;
};

void Check(const char* aWhat, bool aCondition)
{
    printf("    %-46s %s\n", aWhat, aCondition ? "ok" : "FAILED");
    if (!aCondition)
    {
        ++g_failures;
    }
}

void RunCase(const Case& aCase)
{
    printf("  %s\n", aCase.name);

    const auto before = aCase.function();
    Check("returns its value before hooking", before == aCase.expected);
    if (before != aCase.expected)
    {
        printf("      expected %#x, got %#x\n", aCase.expected, before);
    }

    // Mirrors how Hook<T> drives the engine: the slot is rewritten to the trampoline on commit.
    Fn slot = aCase.function;

    if (!HookEngine::Begin())
    {
        Check("transaction started", false);
        return;
    }

    const auto attachResult =
        HookEngine::Attach(reinterpret_cast<void**>(&slot), reinterpret_cast<void*>(&DetourFn));
    Check("attach queued", attachResult == HookEngine::Success);

    const auto committed = HookEngine::Commit();
    Check("transaction committed", committed);
    if (!committed)
    {
        printf("      the engine refused this prologue, LastResult=%d\n", HookEngine::LastResult());
        return;
    }

    Check("slot now points at a trampoline", reinterpret_cast<void*>(slot) != reinterpret_cast<void*>(aCase.function));

    const auto hitsBefore = g_detourHits;
    const auto hooked = aCase.function();
    Check("calling the function reaches the detour", g_detourHits == hitsBefore + 1 && hooked == 0xDEAD);

    const auto viaTrampoline = slot();
    Check("trampoline still returns the original value", viaTrampoline == aCase.expected);
    if (viaTrampoline != aCase.expected)
    {
        printf("      expected %#x, got %#x  <-- relocation is wrong\n", aCase.expected, viaTrampoline);
    }

    // Detach and confirm the original bytes came back.
    HookEngine::Begin();
    HookEngine::Detach(reinterpret_cast<void**>(&slot), reinterpret_cast<void*>(&DetourFn));
    HookEngine::Commit();

    Check("original restored after detach", aCase.function() == aCase.expected);
}
} // namespace

int main()
{
    const Case cases[] = {
        {"plain prologue (nothing to relocate)", &TestPlain, 0x11},
        {"ADR", &TestAdr, 0x22},
        {"ADRP + ADD", &TestAdrp, 0x33},
        {"LDR literal", &TestLdrLiteral, 0x44},
        {"B out of the displaced region", &TestBranch, 0x55},
        {"CBZ out of the displaced region", &TestCompareBranch, 0x66},
        {"TBZ out of the displaced region", &TestTestBranch, 0x77},
        {"BL out of the displaced region", &TestBranchLink, 0x88},
    };

    printf("arm64 hook engine relocator test\n\n");
    for (const auto& testCase : cases)
    {
        RunCase(testCase);
        printf("\n");
    }

    if (g_failures == 0)
    {
        printf("all checks passed\n");
        return 0;
    }

    printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
