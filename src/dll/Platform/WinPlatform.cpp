#include "Platform.hpp"

#include <cstdio>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

// Every function here forwards to the exact Win32 call the runtime made before the platform
// layer existed, so the Windows build behaves as it always did.

std::filesystem::path Platform::GetExecutablePath()
{
    std::wstring fileName;
    if (FAILED(wil::GetModuleFileNameW(nullptr, fileName)))
    {
        return {};
    }

    return fileName;
}

uintptr_t Platform::GetImageBase()
{
    return reinterpret_cast<uintptr_t>(::GetModuleHandle(nullptr));
}

uintptr_t Platform::GetImageSlide()
{
    // Windows addresses are already expressed relative to the module handle.
    return 0;
}

uintptr_t Platform::GetEntryPoint()
{
    // Windows resolves WinMain through the address table; nothing here needs the PE entry.
    return 0;
}

bool Platform::ReadImageVersion(const std::filesystem::path& aPath, ImageVersion& aVersion)
{
    // Unchanged from the VERSIONINFO reading Image.cpp did before the platform split.
    const auto fileName = aPath.wstring();

    auto size = ::GetFileVersionInfoSize(fileName.c_str(), nullptr);
    if (!size)
    {
        auto lastError = ::GetLastError();
        if (lastError == ERROR_RESOURCE_DATA_NOT_FOUND || lastError == ERROR_RESOURCE_TYPE_NOT_FOUND)
        {
            // Fail silently, executables might not have the version information.
            return true;
        }

        return false;
    }

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]());
    if (!data)
    {
        return false;
    }

    if (!::GetFileVersionInfo(fileName.c_str(), 0, size, data.get()))
    {
        return false;
    }

    struct LangAndCodePage
    {
        WORD language;
        WORD codePage;
    }* translations;
    uint32_t translationsBytes;

    if (!::VerQueryValue(data.get(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                         &translationsBytes))
    {
        return false;
    }

    for (uint32_t i = 0; i < (translationsBytes / sizeof(LangAndCodePage)); i++)
    {
        wchar_t* productName;
        auto subBlock = fmt::format(L"\\StringFileInfo\\{:04x}{:04x}\\ProductName", translations[i].language,
                                    translations[i].codePage);

        if (::VerQueryValue(data.get(), subBlock.c_str(), reinterpret_cast<void**>(&productName), &translationsBytes))
        {
            constexpr std::wstring_view expectedProductName = L"Cyberpunk 2077";
            if (productName == expectedProductName)
            {
                aVersion.isCyberpunk = true;
                break;
            }
        }
    }

    if (!aVersion.isCyberpunk)
    {
        return true;
    }

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT fileInfoBytes;

    if (!::VerQueryValue(data.get(), L"\\", reinterpret_cast<LPVOID*>(&fileInfo), &fileInfoBytes))
    {
        return false;
    }

    constexpr auto signature = 0xFEEF04BD;
    if (fileInfo->dwSignature != signature)
    {
        return false;
    }

    aVersion.fileMajor = (fileInfo->dwFileVersionMS >> 16) & 0xFF;
    aVersion.fileMinor = fileInfo->dwFileVersionMS & 0xFFFF;
    aVersion.fileBuild = (fileInfo->dwFileVersionLS >> 16) & 0xFFFF;
    aVersion.fileRevision = fileInfo->dwFileVersionLS & 0xFFFF;

    aVersion.productMajor = static_cast<uint8_t>((fileInfo->dwProductVersionMS >> 16) & 0xFF);
    aVersion.productMinor = static_cast<uint16_t>(fileInfo->dwProductVersionMS & 0xFFFF);
    aVersion.productPatch = static_cast<uint32_t>((fileInfo->dwProductVersionLS >> 16) & 0xFFFF);

    return true;
}

std::wstring_view Platform::GetPluginExtension()
{
    return L".dll";
}

HMODULE Platform::LoadModule(const std::filesystem::path& aPath, bool aUseAlteredSearchPath)
{
    const DWORD flags = aUseAlteredSearchPath ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
    return ::LoadLibraryEx(aPath.c_str(), nullptr, flags);
}

HMODULE Platform::GetMainModule()
{
    return ::GetModuleHandle(nullptr);
}

void Platform::UnloadModule(HMODULE aModule)
{
    if (aModule)
    {
        ::FreeLibrary(aModule);
    }
}

void* Platform::GetSymbol(HMODULE aModule, const char* aName)
{
    return reinterpret_cast<void*>(::GetProcAddress(aModule, aName));
}

uint32_t Platform::GetLastErrorCode()
{
    return ::GetLastError();
}

std::wstring Platform::FormatSystemMessage(uint32_t aMessageId)
{
    wil::last_error_context last_error;
    wil::unique_hlocal_ptr<wchar_t> buffer;

    auto len =
        ::FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        nullptr, aMessageId, LANG_USER_DEFAULT, wil::out_param_ptr<LPWSTR>(buffer), 0, nullptr);
    if (!len)
    {
        return fmt::format(L"Could not format the system message for the specified message id ({}), error code: {}",
                           aMessageId, ::GetLastError());
    }

    std::wstring_view res = buffer.get();

    // Remove the new lines at the end of the message, they are annoying.
    if (res.ends_with(L'\n'))
    {
        res.remove_suffix(1);
    }

    if (res.ends_with(L'\r'))
    {
        res.remove_suffix(1);
    }

    return std::wstring(res);
}

int32_t Platform::ShowMessageBox(const std::wstring_view aCaption, const std::wstring_view aText, uint32_t aType)
{
    return ::MessageBox(nullptr, aText.data(), aCaption.data(), aType);
}

void Platform::TerminateCurrentProcess(uint32_t aExitCode)
{
    ::TerminateProcess(::GetCurrentProcess(), aExitCode);

    // TerminateProcess is asynchronous with respect to the caller; the function is marked
    // noreturn, so make that true.
    for (;;)
    {
    }
}

bool Platform::IsDebuggerAttached()
{
    return ::IsDebuggerPresent();
}

bool Platform::ProtectMemory(void* aAddress, size_t aSize, uint32_t aProtection, uint32_t& aOldProtection)
{
    return ::VirtualProtect(aAddress, aSize, aProtection, reinterpret_cast<PDWORD>(&aOldProtection)) != FALSE;
}

void Platform::FlushInstructionCache(void* aAddress, size_t aSize)
{
    ::FlushInstructionCache(::GetCurrentProcess(), aAddress, aSize);
}

std::string Platform::Narrow(const std::wstring_view aText)
{
    if (aText.empty())
    {
        return "";
    }

    std::string result;

    auto len =
        ::WideCharToMultiByte(CP_UTF8, 0, aText.data(), static_cast<int32_t>(aText.size()), nullptr, 0, NULL, NULL);
    if (len)
    {
        result.resize(len);
        len = ::WideCharToMultiByte(CP_UTF8, 0, aText.data(), static_cast<int32_t>(aText.size()), result.data(),
                                    static_cast<int32_t>(result.size()), NULL, NULL);
    }

    // Second pass.
    if (len <= 0)
    {
        result = fmt::format("Failed to convert wide to narrow string, last error is {}", ::GetLastError());
    }

    return result;
}

std::wstring Platform::Widen(const std::string_view aText)
{
    if (aText.empty())
    {
        return L"";
    }

    std::wstring result;

    auto len = ::MultiByteToWideChar(CP_UTF8, 0, aText.data(), static_cast<int32_t>(aText.size()), nullptr, 0);
    if (len)
    {
        result.resize(len);
        len = ::MultiByteToWideChar(CP_UTF8, 0, aText.data(), static_cast<int32_t>(aText.size()), result.data(),
                                    static_cast<int32_t>(result.size()));
    }

    // Second pass.
    if (len <= 0)
    {
        result = fmt::format(L"Failed to convert narrow to wide string, last error is {}", ::GetLastError());
    }

    return result;
}

std::tm Platform::LocalTime(std::time_t aTime)
{
    std::tm result = {};
    localtime_s(&result, &aTime);

    return result;
}

int Platform::FormattedLength(const char* aFormat, va_list aArgs)
{
    return ::_vscprintf(aFormat, aArgs);
}

int Platform::FormattedLength(const wchar_t* aFormat, va_list aArgs)
{
    return ::_vscwprintf(aFormat, aArgs);
}

int Platform::FormatInto(char* aBuffer, size_t aSize, size_t aCount, const char* aFormat, va_list aArgs)
{
    return ::vsnprintf_s(aBuffer, aSize, aCount, aFormat, aArgs);
}

int Platform::FormatInto(wchar_t* aBuffer, size_t aSize, size_t aCount, const wchar_t* aFormat, va_list aArgs)
{
    return ::_vsnwprintf_s(aBuffer, aSize, aCount, aFormat, aArgs);
}
