#include "Utils.hpp"
#include "Config.hpp"
#include "DevConsole.hpp"
#include "Paths.hpp"
#include "Platform.hpp"

#include <ctime>
#include <cwctype>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

std::shared_ptr<spdlog::logger> Utils::CreateLogger(const std::wstring_view aLogName, const std::wstring_view aFilename,
                                                    const Paths& aPaths, const Config& aConfig,
                                                    const DevConsole& aDevConsole)
{
    try
    {
        auto dir = aPaths.GetLogsDir();

        std::error_code err;
        auto exists = std::filesystem::exists(dir, err);
        if (err)
        {
            auto errVal = err.value();
            const auto& category = err.category();
            auto msg = category.message(errVal);

            SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(
                L"An error occurred while checking logs directory existence:\n{}\n\nDirectory: {}", Utils::Widen(msg),
                dir);

            return nullptr;
        }

        if (!exists)
        {
            std::filesystem::create_directories(dir, err);
            if (err)
            {
                auto errVal = err.value();
                const auto& category = err.category();
                auto msg = category.message(errVal);

                SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(
                    L"An error occurred while creating the logs directory:\n{}\n\nDirectory: {}", Utils::Widen(msg),
                    dir);

                return nullptr;
            }
        }

        constexpr auto oneByte = 1;
        constexpr auto oneKbInB = 1024 * oneByte;
        constexpr auto oneMbInB = 1024 * oneKbInB;

        const auto& loggingConfig = aConfig.GetLogging();
        const size_t maxFiles = loggingConfig.maxFiles;
        const size_t maxFileSize = static_cast<size_t>(loggingConfig.maxFileSize) * oneMbInB;

        const std::string logName = Narrow(aLogName);
        std::vector<spdlog::sink_ptr> sinks;

        auto file = dir / aFilename;
        sinks.emplace_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, maxFileSize, maxFiles, true));

        const auto& dev = aConfig.GetDev();
        if (dev.hasConsole && aDevConsole.IsOutputRedirected())
        {
            sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }

        auto logger = std::make_shared<spdlog::logger>(logName, sinks.begin(), sinks.end());

        logger->set_level(loggingConfig.level);
        logger->flush_on(loggingConfig.flushOn);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%6t] [%n] %v");

        spdlog::register_logger(logger);

        return logger;
    }
    catch (const std::exception& e)
    {
        std::string_view msg = e.what();
        if (msg.starts_with("rotating_file_sink: failed renaming"))
        {
            SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(
                L"Unable to rotate the log file. Please ensure that the game is completely closed and not running in "
                L"the background.\n\n{}",
                Utils::Widen(msg));
        }
        else
        {
            SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"An exception occurred while creating the logger:\n{}",
                                                Utils::Widen(msg));
        }
    }

    return nullptr;
}

std::wstring Utils::GetStateName(RED4ext::EGameStateType aStateType)
{
    using enum RED4ext::EGameStateType;
    switch (aStateType)
    {
    case BaseInitialization:
    {
        return L"BaseInitialization";
    }
    case Initialization:
    {
        return L"Initialization";
    }
    case Running:
    {
        return L"Running";
    }
    case Shutdown:
    {
        return L"Shutdown";
    }
    default:
    {
        return L"unknown";
    }
    }
}

std::wstring Utils::FormatSystemMessage(uint32_t aMessageId)
{
    return Platform::FormatSystemMessage(aMessageId);
}

std::wstring Utils::FormatLastError()
{
    return Platform::FormatSystemMessage(Platform::GetLastErrorCode());
}

std::wstring Utils::FormatCurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    // Convert to std::tm for formatting
    std::tm now_tm = Platform::LocalTime(now_c);

    return fmt::format(L"{:04d}-{:02d}-{:02d}-{:02d}-{:02d}-{:02d}", now_tm.tm_year + 1900, now_tm.tm_mon + 1,
                       now_tm.tm_mday, now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec);
}

int32_t Utils::ShowMessageBoxEx(const std::wstring_view aCaption, const std::wstring_view aText, uint32_t aType)
{
    return Platform::ShowMessageBox(aCaption, aText, aType);
}

int32_t Utils::ShowMessageBox(const std::wstring_view aText, uint32_t aType)
{
    return ShowMessageBoxEx(L"RED4ext", aText, aType);
}

std::string Utils::Narrow(const std::wstring_view aText)
{
    return Platform::Narrow(aText);
}

std::wstring Utils::Widen(const std::string_view aText)
{
    return Platform::Widen(aText);
}

std::wstring Utils::ToLower(const std::wstring& acText)
{
    std::wstring text = acText;

    std::transform(text.begin(), text.end(), text.begin(), std::towlower);
    return text;
}
