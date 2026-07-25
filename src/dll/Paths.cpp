#include "Paths.hpp"
#include "Platform.hpp"
#include "Utils.hpp"

Paths::Paths()
{
    auto fileName = Platform::GetExecutablePath();
    if (fileName.empty())
    {
        SHOW_LAST_ERROR_MESSAGE_AND_EXIT_FILE_LINE(L"Could not get game's file name.");
        return;
    }

    m_exe = fileName;

#ifdef _WIN32
    m_root = m_exe
                 .parent_path()  // Resolve to "x64" directory.
                 .parent_path()  // Resolve to "bin" directory.
                 .parent_path(); // Resolve to game's root directory.
#else
    // The Mac release keeps the same tree as the Windows one -- r6, archive, engine all sit in
    // the game root -- but the executable lives inside an app bundle instead of bin/x64:
    //
    //   <root>/Cyberpunk2077.app/Contents/MacOS/Cyberpunk2077
    m_root = m_exe
                 .parent_path()  // Resolve to "MacOS" directory.
                 .parent_path()  // Resolve to "Contents" directory.
                 .parent_path()  // Resolve to the ".app" bundle.
                 .parent_path(); // Resolve to game's root directory.
#endif
}

std::filesystem::path Paths::GetRootDir() const
{
    return m_root;
}

std::filesystem::path Paths::GetX64Dir() const
{
#ifdef _WIN32
    return GetRootDir() / L"bin" / L"x64";
#else
    // There is no bin/x64 in the Mac release. The meaning callers want is "the directory the
    // game executable lives in", which is the bundle's MacOS directory.
    return m_exe.parent_path();
#endif
}

std::filesystem::path Paths::GetExe() const
{
    return m_exe;
}

std::filesystem::path Paths::GetRED4extDir() const
{
    return GetRootDir() / L"red4ext";
}

std::filesystem::path Paths::GetLogsDir() const
{
    return GetRED4extDir() / L"logs";
}

std::filesystem::path Paths::GetPluginsDir() const
{
    return GetRED4extDir() / L"plugins";
}

std::filesystem::path Paths::GetRedscriptPathsFile() const
{
    return GetRED4extDir() / L"redscript_paths.txt";
}

std::filesystem::path Paths::GetR6Scripts() const
{
    return GetRootDir() / L"r6" / L"scripts";
}

std::filesystem::path Paths::GetDefaultScriptsBlob() const
{
    return GetRootDir() / L"r6" / L"cache" / "final.redscripts";
}

std::filesystem::path Paths::GetR6CacheModded() const
{
    return GetRootDir() / L"r6" / L"cache" / L"modded";
}

std::filesystem::path Paths::GetR6Dir() const
{
    return GetRootDir() / L"r6";
}

const std::filesystem::path Paths::GetConfigFile() const
{
    return GetRED4extDir() / L"config.ini";
}
