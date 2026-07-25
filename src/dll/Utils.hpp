#pragma once

#include <RED4ext/GameStates.hpp>

#include <fmt/format.h>
#include <spdlog/logger.h>

#include "Platform.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class Config;
class DevConsole;
class Paths;

namespace Utils
{
std::shared_ptr<spdlog::logger> CreateLogger(const std::wstring_view aLogName, const std::wstring_view aFilename,
                                             const Paths& aPaths, const Config& aConfig, const DevConsole& aDevConsole);

std::wstring GetStateName(RED4ext::EGameStateType aStateType);

std::wstring FormatSystemMessage(uint32_t aMessageId);
std::wstring FormatLastError();
std::wstring FormatCurrentTimestamp();

int32_t ShowMessageBoxEx(const std::wstring_view aCaption, const std::wstring_view aText, uint32_t aType = MB_OK);
int32_t ShowMessageBox(const std::wstring_view aText, uint32_t aType = MB_OK);

std::string Narrow(const std::wstring_view aText);
std::wstring Widen(const std::string_view aText);

/**
 * @brief UTF-8 form of a path.
 *
 * std::filesystem::path stores wchar_t on Windows and char elsewhere, so Narrow cannot simply
 * be handed path::c_str(). Deliberately not path::string(), which on MSVC converts through the
 * active code page rather than UTF-8 and would change what the Windows build produces.
 */
inline std::string NarrowPath(const std::filesystem::path& aPath)
{
    // Not "if constexpr": the condition does not depend on a template parameter, so both
    // branches would still have to compile, and only one of them does on any given platform.
#ifdef _WIN32
    return Narrow(aPath.native());
#else
    return aPath.native();
#endif
}

/**
 * @brief Number of elements in a DynArray.
 *
 * RED4ext.SDK exposed `size` as a public member up to 1.0.0 and replaced it with an accessor
 * afterwards. The runtime's Windows build is pinned to 1.0.0 while the macOS build needs a
 * newer SDK for its Clang support, so read it in a way that compiles against both rather than
 * migrating the runtime to one of them.
 */
template<typename T>
constexpr uint32_t ArraySize(const T& aArray)
{
    if constexpr (requires { aArray.size(); })
    {
        return static_cast<uint32_t>(aArray.size());
    }
    else
    {
        return static_cast<uint32_t>(aArray.size);
    }
}

/**
 * @brief Element of a DynArray by index.
 *
 * Same version split as ArraySize. Indexing is spelled the same way in both SDK versions,
 * which the raw `entries` pointer is not -- it became private.
 */
template<typename T>
constexpr auto& ArrayAt(T& aArray, uint32_t aIndex)
{
    return aArray[aIndex];
}

std::wstring ToLower(const std::wstring& acText);

template<typename... Args>
int32_t ShowMessageBox(uint32_t aType, const std::wstring_view aText, Args&&... aArgs)
{
    return ShowMessageBox(fmt::format(fmt::runtime(aText), std::forward<Args>(aArgs)...), aType);
}

template<typename... Args>
void ShowLastErrorMessage(uint32_t aType, const std::wstring_view aAdditionalText = L"", Args&&... aArgs)
{
    auto msg = FormatLastError();
    if (!aAdditionalText.empty())
    {
        msg += L"\n\n";
        msg += aAdditionalText;

        if constexpr (sizeof...(Args) > 0)
        {
            msg = fmt::format(fmt::runtime(msg), std::forward<Args>(aArgs)...);
        }
    }

    auto error = Platform::GetLastErrorCode();
    auto caption = fmt::format(L"RED4ext (error {})", error);
    ShowMessageBoxEx(caption.c_str(), msg.c_str(), aType);
}
} // namespace Utils

// std::filesystem::path stores wchar_t on Windows and char everywhere else, so a path cannot
// simply be handed to a formatter of the requested character type. Convert when they differ.
template<typename Char>
struct fmt::formatter<std::filesystem::path, Char> : formatter<basic_string_view<Char>, Char>
{
    template<typename FormatContext>
    auto format(const std::filesystem::path& path, FormatContext& ctx) const
    {
        using Base = formatter<basic_string_view<Char>, Char>;

        if constexpr (std::is_same_v<Char, std::filesystem::path::value_type>)
        {
            return Base::format(path.native(), ctx);
        }
        else if constexpr (std::is_same_v<Char, wchar_t>)
        {
            const auto text = path.wstring();
            return Base::format(text, ctx);
        }
        else
        {
            const auto text = path.string();
            return Base::format(text, ctx);
        }
    }
};

#ifndef SHOW_LAST_ERROR_MESSAGE_FILE_LINE
#define SHOW_LAST_ERROR_MESSAGE_FILE_LINE(additionalText, ...)                                                         \
    Utils::ShowLastErrorMessage(MB_ICONWARNING | MB_OK, additionalText L"\n\n{}:{}", ##__VA_ARGS__, TEXT(__FILE__),    \
                                __LINE__)
#endif

#ifndef SHOW_LAST_ERROR_MESSAGE_AND_EXIT_FILE_LINE
#define SHOW_LAST_ERROR_MESSAGE_AND_EXIT_FILE_LINE(additionalText, ...)                                                \
    Utils::ShowLastErrorMessage(                                                                                       \
        MB_ICONERROR | MB_OK, additionalText L"\n\n{}:{}\n\nThe game will close now to prevent unexpected behavior.",  \
        ##__VA_ARGS__, TEXT(__FILE__), __LINE__);                                                                      \
    Platform::TerminateCurrentProcess(1)
#endif

#ifndef SHOW_MESSAGE_BOX_FILE_LINE
#define SHOW_MESSAGE_BOX_FILE_LINE(type, msg, ...)                                                                     \
    Utils::ShowMessageBox(type, msg L"\n\n{}:{}", ##__VA_ARGS__, TEXT(__FILE__), __LINE__)
#endif

#ifndef SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE
#define SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(msg, ...)                                                                  \
    Utils::ShowMessageBox(MB_ICONERROR | MB_OK,                                                                        \
                          msg L"\n\n{}:{}\n\nThe game will close now to prevent unexpected behavior.", ##__VA_ARGS__,  \
                          TEXT(__FILE__), __LINE__);                                                                   \
    Platform::TerminateCurrentProcess(1)
#endif
