#include "Platform.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace
{
/**
 * dlopen and dlsym do not report through errno; they report through dlerror(), whose result is
 * only valid until the next call. The runtime's existing call sites all do
 * "GetLastError() ... FormatLastError()", so we make that pair work by stashing the dlerror
 * text and handing back a sentinel code that FormatSystemMessage recognises.
 */
constexpr uint32_t kDynamicLoaderError = 0xD1D0E770u;

thread_local std::string g_lastDynamicLoaderError;

void CaptureDynamicLoaderError()
{
    const auto* err = dlerror();
    g_lastDynamicLoaderError = err ? err : "no error reported by the dynamic loader";
    errno = 0;
}

const struct mach_header_64* FindExecutableHeader(uintptr_t* aSlide)
{
    // Index 0 is this dylib, not the executable, whenever RED4ext is injected with
    // DYLD_INSERT_LIBRARIES. Look for the MH_EXECUTE image instead.
    const auto count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(i));
        if (!header || header->magic != MH_MAGIC_64 || header->filetype != MH_EXECUTE)
        {
            continue;
        }

        if (aSlide)
        {
            *aSlide = static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(i));
        }

        return header;
    }

    return nullptr;
}

CFStringRef MakeCFString(const std::wstring_view aText)
{
    const auto utf8 = Platform::Narrow(aText);
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                   static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
}

vm_prot_t ToVmProt(uint32_t aProtection)
{
    switch (aProtection)
    {
    case PAGE_NOACCESS:
        return VM_PROT_NONE;
    case PAGE_READONLY:
        return VM_PROT_READ;
    case PAGE_READWRITE:
        return VM_PROT_READ | VM_PROT_WRITE;
    case PAGE_EXECUTE:
        return VM_PROT_EXECUTE;
    case PAGE_EXECUTE_READ:
        return VM_PROT_READ | VM_PROT_EXECUTE;
    case PAGE_EXECUTE_READWRITE:
        return VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    default:
        return VM_PROT_READ;
    }
}

uint32_t FromVmProt(vm_prot_t aProt)
{
    const bool read = (aProt & VM_PROT_READ) != 0;
    const bool write = (aProt & VM_PROT_WRITE) != 0;
    const bool exec = (aProt & VM_PROT_EXECUTE) != 0;

    if (exec)
    {
        return write ? PAGE_EXECUTE_READWRITE : (read ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    }

    if (write)
    {
        return PAGE_READWRITE;
    }

    return read ? PAGE_READONLY : PAGE_NOACCESS;
}

/**
 * Protection the pages currently carry. VirtualProtect hands the caller the old value so it can
 * be restored; mach_vm_protect does not, so read it back from the region info.
 */
uint32_t QueryProtection(void* aAddress)
{
    mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(aAddress);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
    {
        return PAGE_EXECUTE_READ;
    }

    return FromVmProt(info.protection);
}
} // namespace

std::filesystem::path Platform::GetExecutablePath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        return {};
    }

    // _NSGetExecutablePath can hand back a path containing symlinks or "..", which
    // GetModuleFileNameW never would. Canonicalise so both platforms behave the same.
    char resolved[PATH_MAX] = {};
    if (realpath(buffer.data(), resolved))
    {
        return std::filesystem::path(resolved);
    }

    return std::filesystem::path(buffer.data());
}

uintptr_t Platform::GetImageBase()
{
    return reinterpret_cast<uintptr_t>(FindExecutableHeader(nullptr));
}

uintptr_t Platform::GetImageSlide()
{
    uintptr_t slide = 0;
    FindExecutableHeader(&slide);

    return slide;
}

uintptr_t Platform::GetEntryPoint()
{
    const auto* header = FindExecutableHeader(nullptr);
    if (!header)
    {
        return 0;
    }

    const auto* command = reinterpret_cast<const struct load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i)
    {
        if (command->cmd == LC_MAIN)
        {
            const auto* entry = reinterpret_cast<const struct entry_point_command*>(command);

            // entryoff is relative to the start of the mach header, which is where the image
            // is loaded, so the slide is already accounted for.
            return reinterpret_cast<uintptr_t>(header) + static_cast<uintptr_t>(entry->entryoff);
        }

        command =
            reinterpret_cast<const struct load_command*>(reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }

    return 0;
}

namespace
{
/**
 * Mac builds carry a marketing version ("2.3.1") and an opaque build number, not the four-part
 * FILEVERSION resource RED4ext's version gate is written against. The two numbering schemes
 * cannot be derived from each other, so map them.
 *
 * The product version does line up: the runtime prints Windows patch 2.31 as major 2, minor 3,
 * patch 1, which is exactly what CFBundleShortVersionString says on Mac.
 */
struct KnownVersion
{
    uint8_t productMajor;
    uint16_t productMinor;
    uint32_t productPatch;

    uint16_t fileMajor;
    uint16_t fileMinor;
    uint16_t fileBuild;
    uint16_t fileRevision;
};

constexpr KnownVersion kKnownVersions[] = {
    // Mac 2.3.1 (build 5314028) is the same game release as Windows patch 2.31.
    {2, 3, 1, 3, 0, 80, 51928},
};

/**
 * Read one <key>/<string> pair out of an XML property list.
 *
 * Deliberately hand-rolled rather than CFBundle/CFPropertyList. This runs from a dyld
 * initializer, before the main executable's own initializers, and CFBundleCreate reaches into
 * Foundation -- which at that point is not ready. The result is an infinite recursion inside
 * +[NSString stringWithFormat:] and a segfault before the game ever starts.
 *
 * Only the two scalar string values below are needed, so a targeted scan is enough and keeps
 * the whole startup path free of Objective-C.
 */
std::string ReadPlistString(const std::string& aXml, std::string_view aKey)
{
    const std::string keyTag = "<key>" + std::string(aKey) + "</key>";

    auto keyPos = aXml.find(keyTag);
    if (keyPos == std::string::npos)
    {
        return {};
    }

    auto openPos = aXml.find("<string>", keyPos + keyTag.size());
    if (openPos == std::string::npos)
    {
        return {};
    }

    // Guard against matching a <string> that belongs to a later key: anything other than
    // whitespace between the two tags means this key's value is not a string.
    for (auto i = keyPos + keyTag.size(); i < openPos; ++i)
    {
        if (!std::isspace(static_cast<unsigned char>(aXml[i])))
        {
            return {};
        }
    }

    openPos += std::strlen("<string>");

    const auto closePos = aXml.find("</string>", openPos);
    if (closePos == std::string::npos)
    {
        return {};
    }

    return aXml.substr(openPos, closePos - openPos);
}

/** Parse "2.3.1" into its three components; missing components stay 0. */
void ParseSemVer(const std::string& aText, uint8_t& aMajor, uint16_t& aMinor, uint32_t& aPatch)
{
    unsigned major = 0, minor = 0, patch = 0;
    std::sscanf(aText.c_str(), "%u.%u.%u", &major, &minor, &patch);

    aMajor = static_cast<uint8_t>(major);
    aMinor = static_cast<uint16_t>(minor);
    aPatch = static_cast<uint32_t>(patch);
}
} // namespace

bool Platform::ReadImageVersion(const std::filesystem::path& aPath, ImageVersion& aVersion)
{
    // aPath is .../Cyberpunk2077.app/Contents/MacOS/Cyberpunk2077, so Info.plist is two
    // directories up.
    const auto plistPath = aPath.parent_path().parent_path() / "Info.plist";

    std::error_code ec;
    if (!std::filesystem::exists(plistPath, ec) || ec)
    {
        // Not running from an app bundle. Not an error: it simply is not Cyberpunk.
        return true;
    }

    std::ifstream stream(plistPath, std::ios::binary);
    if (!stream)
    {
        return false;
    }

    const std::string xml((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    const auto identifier = ReadPlistString(xml, "CFBundleIdentifier");
    const auto shortVersion = ReadPlistString(xml, "CFBundleShortVersionString");

    // The bundle identifier is a far more reliable marker than the display name, which is
    // localised and differs between editions ("Cyberpunk 2077: Ultimate" on this install).
    if (!identifier.starts_with("com.cdprojektred.cyberpunk"))
    {
        return true;
    }

    aVersion.isCyberpunk = true;
    ParseSemVer(shortVersion, aVersion.productMajor, aVersion.productMinor, aVersion.productPatch);

    const auto product = std::make_tuple(aVersion.productMajor, aVersion.productMinor, aVersion.productPatch);

    const KnownVersion* best = nullptr;
    for (const auto& known : kKnownVersions)
    {
        const auto candidate = std::make_tuple(known.productMajor, known.productMinor, known.productPatch);
        if (candidate > product)
        {
            continue;
        }

        if (!best || candidate > std::make_tuple(best->productMajor, best->productMinor, best->productPatch))
        {
            best = &known;
        }
    }

    if (best)
    {
        // A game newer than anything in the table reports the newest file version we know, so
        // the runtime's minimum-version gate lets it through rather than refusing a release
        // that has simply not been added here yet.
        aVersion.fileMajor = best->fileMajor;
        aVersion.fileMinor = best->fileMinor;
        aVersion.fileBuild = best->fileBuild;
        aVersion.fileRevision = best->fileRevision;
    }

    return true;
}

std::wstring_view Platform::GetPluginExtension()
{
    return L".dylib";
}

HMODULE Platform::LoadModule(const std::filesystem::path& aPath, bool aUseAlteredSearchPath)
{
    // On Windows this flag makes the loader resolve the plugin's dependencies from the
    // plugin's own directory. dyld already searches the loading image's directory for
    // @loader_path references, so there is nothing to switch on here.
    (void)aUseAlteredSearchPath;

    dlerror();

    auto* module = dlopen(aPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        CaptureDynamicLoaderError();
    }

    return module;
}

HMODULE Platform::GetMainModule()
{
    return dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
}

void Platform::UnloadModule(HMODULE aModule)
{
    if (aModule)
    {
        dlclose(aModule);
    }
}

void* Platform::GetSymbol(HMODULE aModule, const char* aName)
{
    if (!aModule || !aName)
    {
        return nullptr;
    }

    dlerror();

    auto* symbol = dlsym(aModule, aName);
    if (!symbol)
    {
        CaptureDynamicLoaderError();
    }

    return symbol;
}

uint32_t Platform::GetLastErrorCode()
{
    if (!g_lastDynamicLoaderError.empty() && errno == 0)
    {
        return kDynamicLoaderError;
    }

    return static_cast<uint32_t>(errno);
}

std::wstring Platform::FormatSystemMessage(uint32_t aMessageId)
{
    if (aMessageId == kDynamicLoaderError)
    {
        return Widen(g_lastDynamicLoaderError);
    }

    if (aMessageId == 0)
    {
        return L"no error";
    }

    char buffer[256] = {};
    if (strerror_r(static_cast<int>(aMessageId), buffer, sizeof(buffer)) != 0)
    {
        return L"unknown error " + std::to_wstring(aMessageId);
    }

    return Widen(buffer);
}

int32_t Platform::ShowMessageBox(const std::wstring_view aCaption, const std::wstring_view aText, uint32_t aType)
{
    // Always write the message out first.
    //
    // RED4ext raises its fatal errors from a dyld initializer, before the game has become a GUI
    // application at all. A modal alert there does not merely fail: CFUserNotification blocks
    // waiting for a response that can never arrive, and the process hangs in its initializer
    // with nothing on screen and nothing in the log. Printing first means the message survives
    // regardless of what the alert does.
    fprintf(stderr, "[RED4ext] %s: %s\n", Narrow(aCaption).c_str(), Narrow(aText).c_str());
    fflush(stderr);

    auto caption = MakeCFString(aCaption);
    auto text = MakeCFString(aText);

    CFOptionFlags level = kCFUserNotificationNoteAlertLevel;
    if (aType & MB_ICONERROR)
    {
        level = kCFUserNotificationStopAlertLevel;
    }
    else if (aType & MB_ICONWARNING)
    {
        level = kCFUserNotificationCautionAlertLevel;
    }

    // CFUserNotification's buttons are (default, alternate, other). Map the two Win32 layouts
    // the runtime actually asks for; anything else gets a lone OK.
    CFStringRef defaultButton = CFSTR("OK");
    CFStringRef alternateButton = nullptr;

    if ((aType & MB_YESNO) == MB_YESNO)
    {
        defaultButton = CFSTR("Yes");
        alternateButton = CFSTR("No");
    }
    else if ((aType & MB_OKCANCEL) == MB_OKCANCEL)
    {
        alternateButton = CFSTR("Cancel");
    }

    // A finite timeout, never 0. Zero means "wait forever", which is exactly the hang above.
    constexpr CFTimeInterval kTimeoutSeconds = 30.0;

    CFOptionFlags response = 0;
    const auto status = CFUserNotificationDisplayAlert(kTimeoutSeconds, level, nullptr, nullptr, nullptr, caption, text,
                                                       defaultButton, alternateButton, nullptr, &response);

    if (caption)
    {
        CFRelease(caption);
    }

    if (text)
    {
        CFRelease(text);
    }

    if (status != 0)
    {
        // No window server, or the alert timed out. The message already went to stderr.
        return IDOK;
    }

    switch (response & 0x3)
    {
    case kCFUserNotificationDefaultResponse:
        return (aType & MB_YESNO) == MB_YESNO ? IDYES : IDOK;
    case kCFUserNotificationAlternateResponse:
        return (aType & MB_YESNO) == MB_YESNO ? IDNO : IDCANCEL;
    default:
        return IDOK;
    }
}

void Platform::TerminateCurrentProcess(uint32_t aExitCode)
{
    _exit(static_cast<int>(aExitCode));
}

bool Platform::IsDebuggerAttached()
{
    // The documented way to ask on Darwin: look for P_TRACED in our own kinfo_proc.
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};

    struct kinfo_proc info = {};
    size_t size = sizeof(info);

    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
    {
        return false;
    }

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

bool Platform::ProtectMemory(void* aAddress, size_t aSize, uint32_t aProtection, uint32_t& aOldProtection)
{
    aOldProtection = QueryProtection(aAddress);

    // VirtualProtect accepts any address and rounds outward itself. mach_vm_protect does not:
    // an unaligned address is rejected with KERN_INVALID_ARGUMENT. Round here so callers can
    // keep passing the exact byte range they care about.
    const auto pageSize = static_cast<uintptr_t>(getpagesize());
    const auto raw = reinterpret_cast<uintptr_t>(aAddress);
    const auto address = static_cast<mach_vm_address_t>(raw & ~(pageSize - 1));
    const auto size = static_cast<mach_vm_size_t>(((raw + aSize + pageSize - 1) & ~(pageSize - 1)) - address);

    auto prot = ToVmProt(aProtection);

    // VM_PROT_COPY is what makes this work on the game's __TEXT, whose maxprot is r-x: it asks
    // the kernel for a private copy-on-write page rather than write access to the mapped file.
    // Nothing on disk changes and the code signature stays valid.
    if (prot & VM_PROT_WRITE)
    {
        prot |= VM_PROT_COPY;
    }

    auto result = mach_vm_protect(mach_task_self(), address, size, FALSE, prot);
    if (result != KERN_SUCCESS && (prot & VM_PROT_COPY))
    {
        // Already a private page: VM_PROT_COPY is then rejected, so retry without it.
        result = mach_vm_protect(mach_task_self(), address, size, FALSE, prot & ~VM_PROT_COPY);
    }

    if (result != KERN_SUCCESS)
    {
        errno = EPERM;
        return false;
    }

    return true;
}

void Platform::FlushInstructionCache(void* aAddress, size_t aSize)
{
    sys_icache_invalidate(aAddress, aSize);
}

std::string Platform::Narrow(const std::wstring_view aText)
{
    // wchar_t is UTF-32 on Darwin, so this is a plain UTF-32 -> UTF-8 encode.
    std::string result;
    result.reserve(aText.size());

    for (wchar_t wide : aText)
    {
        auto cp = static_cast<uint32_t>(wide);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            cp = 0xFFFD; // Not a scalar value; emit the replacement character.
        }

        if (cp < 0x80)
        {
            result.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    return result;
}

std::wstring Platform::Widen(const std::string_view aText)
{
    std::wstring result;
    result.reserve(aText.size());

    size_t i = 0;
    while (i < aText.size())
    {
        const auto lead = static_cast<uint8_t>(aText[i]);
        uint32_t cp = 0;
        size_t extra = 0;

        if (lead < 0x80)
        {
            cp = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            cp = lead & 0x1F;
            extra = 1;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            cp = lead & 0x0F;
            extra = 2;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            cp = lead & 0x07;
            extra = 3;
        }
        else
        {
            // Stray continuation or invalid lead byte; skip it rather than derail the string.
            result.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }

        if (i + extra >= aText.size())
        {
            result.push_back(static_cast<wchar_t>(0xFFFD));
            break;
        }

        bool valid = true;
        for (size_t k = 1; k <= extra; ++k)
        {
            const auto cont = static_cast<uint8_t>(aText[i + k]);
            if ((cont & 0xC0) != 0x80)
            {
                valid = false;
                break;
            }

            cp = (cp << 6) | (cont & 0x3F);
        }

        if (!valid)
        {
            result.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }

        result.push_back(static_cast<wchar_t>(cp));
        i += extra + 1;
    }

    return result;
}

std::tm Platform::LocalTime(std::time_t aTime)
{
    std::tm result = {};
    localtime_r(&aTime, &result);

    return result;
}

int Platform::FormattedLength(const char* aFormat, va_list aArgs)
{
    va_list copy;
    va_copy(copy, aArgs);

    const auto length = vsnprintf(nullptr, 0, aFormat, copy);
    va_end(copy);

    return length;
}

int Platform::FormattedLength(const wchar_t* aFormat, va_list aArgs)
{
    // vswprintf cannot measure: given a null buffer it is undefined, and on overflow it just
    // returns -1 without saying how much room it needed. Grow until it fits.
    constexpr size_t kMaxLength = 1u << 20;

    for (size_t capacity = 256; capacity <= kMaxLength; capacity *= 2)
    {
        std::vector<wchar_t> buffer(capacity);

        va_list copy;
        va_copy(copy, aArgs);

        const auto length = vswprintf(buffer.data(), buffer.size(), aFormat, copy);
        va_end(copy);

        if (length >= 0)
        {
            return length;
        }
    }

    return -1;
}

int Platform::FormatInto(char* aBuffer, size_t aSize, size_t aCount, const char* aFormat, va_list aArgs)
{
    if (!aBuffer || aSize == 0)
    {
        return -1;
    }

    const auto limit = std::min(aSize, aCount + 1);
    const auto written = vsnprintf(aBuffer, limit, aFormat, aArgs);

    // vsnprintf reports what it would have written; the caller wants what it did write.
    if (written < 0)
    {
        return -1;
    }

    return static_cast<int>(std::min(static_cast<size_t>(written), limit - 1));
}

int Platform::FormatInto(wchar_t* aBuffer, size_t aSize, size_t aCount, const wchar_t* aFormat, va_list aArgs)
{
    if (!aBuffer || aSize == 0)
    {
        return -1;
    }

    const auto limit = std::min(aSize, aCount + 1);
    return vswprintf(aBuffer, limit, aFormat, aArgs);
}
