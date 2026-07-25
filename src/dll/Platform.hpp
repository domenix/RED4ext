#pragma once

// The single place where the runtime differs by platform.
//
// Same approach RED4ext.SDK took for its own macOS port: on Windows every function here
// forwards to the exact Win32 call the code used before, so the Windows build keeps its
// previous behaviour byte for byte. Only the non-Windows path is new.
//
// Implementations live in Platform/WinPlatform.cpp and Platform/MacPlatform.cpp; the build
// compiles exactly one of them.

#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#else

// A handful of Win32 spellings appear throughout the runtime. Defining them is far less
// invasive than renaming them at every call site, and keeps the Windows sources untouched.

using HMODULE = void*;
using HINSTANCE = void*;
using PWSTR = wchar_t*;
using BOOL = int;
using DWORD = uint32_t;
using LPVOID = void*;

// arm64 has a single calling convention, so the Win32 convention macros expand to nothing.
// They still have to exist: without them, "int WINAPI Foo(...)" parses WINAPI as the
// declarator's name and the errors that follow are unrecognisable.
#ifndef WINAPI
#define WINAPI
#define APIENTRY
#define CALLBACK
#endif

#ifndef NO_ERROR
#define NO_ERROR 0L
#endif

#ifndef MB_OK
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_YESNO 0x00000004L
#define MB_ICONERROR 0x00000010L
#define MB_ICONWARNING 0x00000030L
#define MB_ICONINFORMATION 0x00000040L
#endif

#ifndef IDOK
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
#endif

// Memory protection constants, spelled the Win32 way. Platform::ProtectMemory translates.
#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#endif

// Two-step, exactly as the Win32 headers do it. A single-step "L##x" pastes before its
// argument is expanded, so TEXT(__FILE__) would produce the identifier L__FILE__.
#ifndef TEXT
#define RED4EXT_PLATFORM_WIDEN_(x) L##x
#define TEXT(x) RED4EXT_PLATFORM_WIDEN_(x)
#endif

#endif // _WIN32

/**
 * @brief Whether spdlog itself accepts wide strings on this platform.
 *
 * spdlog gates its wide-string support on _WIN32. Platform/SpdlogWide.hpp supplies the free
 * functions elsewhere, but member calls on spdlog::logger cannot be extended that way, so the
 * few places that make one have to convert first.
 */
#ifdef _WIN32
#define RED4EXT_PLATFORM_WIDE_LOGGING 1
#else
#define RED4EXT_PLATFORM_WIDE_LOGGING 0
#endif

namespace Platform
{
/**
 * @brief Full path of the running executable.
 *
 * Windows: GetModuleFileNameW(nullptr). macOS: _NSGetExecutablePath, resolved through realpath
 * so the result is canonical the way GetModuleFileNameW's is.
 */
std::filesystem::path GetExecutablePath();

/**
 * @brief Load address of the main executable image.
 *
 * Windows: GetModuleHandle(nullptr). macOS: the mach_header of the MH_EXECUTE image, which is
 * NOT image index 0 when the runtime is injected with DYLD_INSERT_LIBRARIES -- index 0 is the
 * inserted dylib itself.
 */
uintptr_t GetImageBase();

/**
 * @brief ASLR slide of the main executable, i.e. runtime address minus link-time address.
 *
 * Always 0 on Windows, where addresses are expressed relative to the module handle. On macOS
 * the static addresses recorded for the game are link-time ones and need this added.
 */
uintptr_t GetImageSlide();

/**
 * @brief The executable's entry point, or 0 if it cannot be determined.
 *
 * This is the closest thing the Mac build has to WinMain: LC_MAIN records the offset of main,
 * which dyld calls once every initializer has run. Reading it from the load commands means the
 * address survives game updates, unlike a recorded offset.
 *
 * Returns 0 on Windows, where the entry point is found through the address table instead.
 */
uintptr_t GetEntryPoint();

/**
 * @brief Identity and version of an executable image.
 *
 * Windows reads this from the VERSIONINFO resource; macOS from the app bundle's Info.plist,
 * which has no equivalent of the four-part FILEVERSION, so the Mac implementation maps the
 * marketing version onto the numbering RED4ext already uses.
 */
struct ImageVersion
{
    bool isCyberpunk = false;

    uint16_t fileMajor = 0;
    uint16_t fileMinor = 0;
    uint16_t fileBuild = 0;
    uint16_t fileRevision = 0;

    uint8_t productMajor = 0;
    uint16_t productMinor = 0;
    uint32_t productPatch = 0;
};

/**
 * @brief Read the identity and version of an executable.
 * @return false if the information could not be read at all. An executable that simply is not
 *         Cyberpunk returns true with isCyberpunk left false.
 */
bool ReadImageVersion(const std::filesystem::path& aPath, ImageVersion& aVersion);

/* ---------------------------------------------------------------------------------------- */
/* Modules                                                                                    */
/* ---------------------------------------------------------------------------------------- */

/**
 * @brief File extension a plugin is expected to have, dot included.
 */
std::wstring_view GetPluginExtension();

/**
 * @brief Load a plugin.
 * @param aUseAlteredSearchPath resolve the plugin's own dependencies next to it. On Windows
 *        this is LOAD_WITH_ALTERED_SEARCH_PATH; on macOS the dynamic linker already searches
 *        the loaded image's directory for @loader_path references, so the flag has no effect.
 */
HMODULE LoadModule(const std::filesystem::path& aPath, bool aUseAlteredSearchPath);

/**
 * @brief Handle for the main program itself, not for a library on disk.
 *
 * Windows: GetModuleHandle(nullptr). macOS: dlopen(nullptr), which returns a handle whose
 * symbol lookups search the main program.
 */
HMODULE GetMainModule();

/**
 * @brief Release a module loaded by LoadModule.
 *
 * Named UnloadModule rather than the more symmetrical FreeModule because <Windows.h> defines
 * FreeModule as a macro expanding to FreeLibrary, which mangles the declaration even inside a
 * namespace.
 */
void UnloadModule(HMODULE aModule);
void* GetSymbol(HMODULE aModule, const char* aName);

/**
 * @brief Owning module handle. Replaces wil::unique_hmodule, which is Windows-only.
 */
class UniqueModule
{
public:
    UniqueModule() noexcept = default;
    explicit UniqueModule(HMODULE aModule) noexcept
        : m_module(aModule)
    {
    }

    ~UniqueModule()
    {
        reset();
    }

    UniqueModule(const UniqueModule&) = delete;
    UniqueModule& operator=(const UniqueModule&) = delete;

    UniqueModule(UniqueModule&& aOther) noexcept
        : m_module(aOther.m_module)
    {
        aOther.m_module = nullptr;
    }

    UniqueModule& operator=(UniqueModule&& aOther) noexcept
    {
        if (this != &aOther)
        {
            reset(aOther.m_module);
            aOther.m_module = nullptr;
        }

        return *this;
    }

    HMODULE get() const noexcept
    {
        return m_module;
    }

    explicit operator bool() const noexcept
    {
        return m_module != nullptr;
    }

    void reset(HMODULE aModule = nullptr) noexcept
    {
        if (m_module && m_module != aModule)
        {
            UnloadModule(m_module);
        }

        m_module = aModule;
    }

    HMODULE release() noexcept
    {
        auto module = m_module;
        m_module = nullptr;

        return module;
    }

private:
    HMODULE m_module = nullptr;
};

/* ---------------------------------------------------------------------------------------- */
/* Errors                                                                                     */
/* ---------------------------------------------------------------------------------------- */

/**
 * @brief Last error raised by a platform call. GetLastError on Windows, errno elsewhere.
 */
uint32_t GetLastErrorCode();

/**
 * @brief Human-readable text for an error code from GetLastErrorCode.
 */
std::wstring FormatSystemMessage(uint32_t aMessageId);

/* ---------------------------------------------------------------------------------------- */
/* User interaction                                                                           */
/* ---------------------------------------------------------------------------------------- */

/**
 * @brief Modal alert. Returns one of the ID* values.
 *
 * macOS uses CFUserNotificationDisplayAlert, which needs no Objective-C and no app bundle.
 */
int32_t ShowMessageBox(const std::wstring_view aCaption, const std::wstring_view aText, uint32_t aType);

/**
 * @brief Kill the current process without running static destructors.
 */
[[noreturn]] void TerminateCurrentProcess(uint32_t aExitCode);

/**
 * @brief Whether a debugger is attached right now.
 */
bool IsDebuggerAttached();

/* ---------------------------------------------------------------------------------------- */
/* Memory                                                                                     */
/* ---------------------------------------------------------------------------------------- */

/**
 * @brief Change page protection, VirtualProtect-style.
 * @param aProtection one of the PAGE_* constants.
 * @param aOldProtection receives the previous protection, for restoring later.
 * @return false on failure; the reason is available from GetLastErrorCode.
 *
 * On macOS this goes through mach_vm_protect with VM_PROT_COPY, which succeeds on the game's
 * __TEXT even though its maxprot is r-x. VM_PROT_COPY makes the page a private copy, so the
 * file on disk is never modified and the executable's signature stays valid.
 *
 * Two macOS constraints worth knowing before calling this:
 *   - Apple Silicon enforces W^X. PAGE_EXECUTE_READWRITE is refused; ask for PAGE_READWRITE,
 *     write, then restore.
 *   - Changing the protection of the page the calling thread is executing from raises SIGBUS.
 *     That never arises for the runtime, whose own code is in a separate image from the game
 *     code it patches, but it does bite test harnesses that patch themselves.
 */
bool ProtectMemory(void* aAddress, size_t aSize, uint32_t aProtection, uint32_t& aOldProtection);

/**
 * @brief Make newly written instructions visible to the instruction fetcher.
 *
 * A no-op on x86, which has a coherent instruction cache. Required on arm64.
 */
void FlushInstructionCache(void* aAddress, size_t aSize);

/* ---------------------------------------------------------------------------------------- */
/* Strings and time                                                                           */
/* ---------------------------------------------------------------------------------------- */

std::string Narrow(const std::wstring_view aText);
std::wstring Widen(const std::string_view aText);

/* ---------------------------------------------------------------------------------------- */
/* printf-style formatting                                                                    */
/* ---------------------------------------------------------------------------------------- */

/**
 * @brief Characters a formatted string would occupy, excluding the terminator.
 *
 * Windows has _vscprintf/_vscwprintf for this. There is no portable wide equivalent --
 * vswprintf cannot measure, it only reports failure when the buffer is too small -- so the
 * macOS implementation grows a scratch buffer until the result fits.
 *
 * @return -1 if the string could not be formatted.
 */
int FormattedLength(const char* aFormat, va_list aArgs);
int FormattedLength(const wchar_t* aFormat, va_list aArgs);

/**
 * @brief Write a formatted string into a caller-supplied buffer, truncating if it does not fit.
 *
 * Mirrors vsnprintf_s: @p aSize is the buffer's capacity and @p aCount the most characters to
 * write. The result is always null-terminated.
 *
 * @return characters written, or -1 on failure.
 */
int FormatInto(char* aBuffer, size_t aSize, size_t aCount, const char* aFormat, va_list aArgs);
int FormatInto(wchar_t* aBuffer, size_t aSize, size_t aCount, const wchar_t* aFormat, va_list aArgs);

/**
 * @brief Thread-safe localtime. localtime_s on Windows, localtime_r elsewhere.
 */
std::tm LocalTime(std::time_t aTime);
} // namespace Platform

// spdlog's wide-string overloads are compiled only on Windows, but 63 call sites in the
// runtime log wide strings. Supplying the overloads keeps every one of them unchanged.
//
// RED4EXT_PLATFORM_NO_LOGGING lets a test link the platform layer without dragging in spdlog;
// nothing in the runtime itself defines it.
#if !defined(_WIN32) && !defined(RED4EXT_PLATFORM_NO_LOGGING)
#include "Platform/SpdlogWide.hpp"
#endif
