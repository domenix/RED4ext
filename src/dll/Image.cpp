#include "Image.hpp"
#include "Platform.hpp"
#include "Utils.hpp"

#include <RED4ext/Api/v1/FileVer.hpp>
#include <RED4ext/Api/v1/SemVer.hpp>

#include <cstdint>
#include <string>
#include <vector>

Image::Image()
    : m_isCyberpunk(false)
    , m_fileVersion(RED4EXT_V1_FILEVER(0, 0, 0, 0))
    , m_productVersion(RED4EXT_V1_SEMVER(0, 0, 0))
{
    const auto fileName = Platform::GetExecutablePath();
    if (fileName.empty())
    {
        SHOW_LAST_ERROR_MESSAGE_FILE_LINE(L"Could not get executable's file name.");
        return;
    }

    Platform::ImageVersion version;
    if (!Platform::ReadImageVersion(fileName, version))
    {
        SHOW_LAST_ERROR_MESSAGE_FILE_LINE(L"Could not retrieve version info.\n\nFile name: {}", fileName);
        return;
    }

    m_isCyberpunk = version.isCyberpunk;
    if (!m_isCyberpunk)
    {
        return;
    }

    m_fileVersion = RED4EXT_V1_FILEVER(version.fileMajor, version.fileMinor, version.fileBuild, version.fileRevision);
    m_productVersion = RED4EXT_V1_SEMVER(version.productMajor, version.productMinor, version.productPatch);
}

Image* Image::Get()
{
    static Image instance;
    return &instance;
}

bool Image::IsCyberpunk() const
{
    return m_isCyberpunk;
}

bool Image::IsSupported() const
{
    const auto supportedVersions = GetSupportedVersions();
    for (const auto& version : supportedVersions)
    {
        if (version == m_fileVersion)
        {
            return true;
        }
    }

    return false;
}

const RED4ext::v1::FileVer& Image::GetFileVersion() const
{
    return m_fileVersion;
}

const RED4ext::v1::SemVer& Image::GetProductVersion() const
{
    return m_productVersion;
}

const std::vector<RED4ext::v1::FileVer> Image::GetSupportedVersions() const
{
    return {m_fileVersion};
}
