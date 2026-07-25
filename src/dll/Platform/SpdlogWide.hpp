#pragma once

// spdlog only compiles its wide-string overloads on Windows: SPDLOG_WCHAR_TO_UTF8_SUPPORT is
// guarded by _WIN32, because wchar_t is UTF-16 there and UTF-8 conversion is a Win32 call.
// The runtime logs wide strings in 63 places.
//
// Rewriting those call sites would mean touching almost every file for no reason other than
// the platform, so instead we add the missing overloads. They format with fmt -- which does
// support wchar_t everywhere -- and hand spdlog the UTF-8 result.
//
// Only included on non-Windows, so these can never be ambiguous with spdlog's own overloads.

#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <spdlog/spdlog.h>

namespace Platform
{
std::string Narrow(const std::wstring_view aText);
}

namespace spdlog
{
namespace red4ext_wide_detail
{
template<typename... Args>
inline std::string Format(fmt::wformat_string<Args...> aFormat, Args&&... aArgs)
{
    return Platform::Narrow(fmt::format(aFormat, std::forward<Args>(aArgs)...));
}

inline std::string Format(const std::wstring& aText)
{
    return Platform::Narrow(aText);
}

inline std::string Format(const wchar_t* aText)
{
    return Platform::Narrow(aText);
}
} // namespace red4ext_wide_detail

// The function name and the enumerator differ for one level: spdlog::error logs at
// spdlog::level::err.
#define RED4EXT_DEFINE_WIDE_LOG_LEVEL(name, levelName)                                                                 \
    template<typename... Args>                                                                                         \
    inline void name(fmt::wformat_string<Args...> aFormat, Args&&... aArgs)                                            \
    {                                                                                                                  \
        if (!should_log(level::levelName))                                                                             \
        {                                                                                                              \
            return;                                                                                                    \
        }                                                                                                              \
        name(red4ext_wide_detail::Format(aFormat, std::forward<Args>(aArgs)...));                                      \
    }                                                                                                                  \
                                                                                                                       \
    inline void name(const std::wstring& aText)                                                                        \
    {                                                                                                                  \
        if (!should_log(level::levelName))                                                                             \
        {                                                                                                              \
            return;                                                                                                    \
        }                                                                                                              \
        name(red4ext_wide_detail::Format(aText));                                                                      \
    }                                                                                                                  \
                                                                                                                       \
    inline void name(const wchar_t* aText)                                                                             \
    {                                                                                                                  \
        if (!should_log(level::levelName))                                                                             \
        {                                                                                                              \
            return;                                                                                                    \
        }                                                                                                              \
        name(red4ext_wide_detail::Format(aText));                                                                      \
    }

RED4EXT_DEFINE_WIDE_LOG_LEVEL(trace, trace)
RED4EXT_DEFINE_WIDE_LOG_LEVEL(debug, debug)
RED4EXT_DEFINE_WIDE_LOG_LEVEL(info, info)
RED4EXT_DEFINE_WIDE_LOG_LEVEL(warn, warn)
RED4EXT_DEFINE_WIDE_LOG_LEVEL(error, err)
RED4EXT_DEFINE_WIDE_LOG_LEVEL(critical, critical)

#undef RED4EXT_DEFINE_WIDE_LOG_LEVEL
} // namespace spdlog
