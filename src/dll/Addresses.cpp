#include "Addresses.hpp"

#include <charconv>
#include <ios>
#include <string>
#include <string_view>

#include <RED4ext/Relocation.hpp>
#include <spdlog/spdlog.h>

#include "Detail/AddressHashes.hpp"
#include "Platform.hpp"
#include "Utils.hpp"

namespace
{
std::unique_ptr<Addresses> g_addresses;

#ifdef _WIN32
constexpr auto kAddressesFileName = L"cyberpunk2077_addresses.json";
#else
constexpr auto kAddressesFileName = L"cyberpunk2077_addresses_mac.json";
#endif
} // namespace

Addresses::Addresses(const Paths& aPaths)
    : m_codeOffset(0)
    , m_dataOffset(0)
    , m_rdataOffset(0)
{
#ifdef _WIN32
    auto filePath = aPaths.GetX64Dir() / kAddressesFileName;
#else
    // The app bundle is code-signed and nothing should write into it, so the Mac map lives
    // with the rest of RED4ext's files rather than beside the executable.
    auto filePath = aPaths.GetRED4extDir() / kAddressesFileName;
#endif

    LoadSections();
    LoadAddresses(filePath);
}

void Addresses::Construct(const Paths& aPaths)
{
    g_addresses.reset(new Addresses(aPaths));
}

Addresses* Addresses::Instance()
{
    return g_addresses.get();
}

std::uintptr_t Addresses::Resolve(std::uint32_t aHash) const
{
    const auto it = m_addresses.find(aHash);
    if (it == m_addresses.end())
    {
        return 0;
    }

    const auto address = it->second;
    return address;
}

#ifdef _WIN32

void Addresses::LoadAddresses(const std::filesystem::path& aPath)
{
    if (!exists(aPath))
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"The addresses JSON does not exists\n\nPath: {}", aPath);
        return;
    }

    spdlog::info(L"Loading game's addresses from '{}'...", aPath);

    simdjson::ondemand::parser parser;
    simdjson::padded_string json = simdjson::padded_string::load(aPath.string());
    simdjson::ondemand::document document = parser.iterate(json);

    simdjson::ondemand::array root;
    auto error = document["Addresses"].get_array().get(root);
    if (error)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"Could not get the root array for the addresses: {}",
                                            Utils::Widen(simdjson::error_message(error)));
        return;
    }

    auto base = Platform::GetImageBase();

    root.reset();

    for (auto entry : root)
    {
        auto hashField = entry.find_field("hash");
        auto offsetField = entry.find_field("offset");

        if (!hashField.error() && !offsetField.error())
        {
            std::uint64_t hash;
            error = hashField.get_uint64_in_string().get(hash);
            if (error)
            {
                SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"Could not get the hash for an address: {}",
                                                    Utils::Widen(simdjson::error_message(error)));
                return;
            }

            std::string_view offsetStr;
            error = offsetField.get_string().get(offsetStr);
            if (error)
            {
                SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"Could not get the offset for an address: {}",
                                                    Utils::Widen(simdjson::error_message(error)));
                return;
            }

            std::stringstream stream;
            stream << offsetStr;

            std::uint32_t segment;
            char separator;
            std::uint32_t offset;
            stream >> std::hex >> segment >> separator >> offset;

            switch (segment)
            {
            case 1:
                offset += m_codeOffset;
                break;
            case 2:
                offset += m_rdataOffset;
                break;
            case 3:
                offset += m_dataOffset;
                break;
            }

            auto address = offset + base;
            m_addresses.emplace(static_cast<std::uint32_t>(hash), address);
        }
    }

    spdlog::info("{} game addresses loaded", m_addresses.size());
}

void Addresses::LoadSections()
{
    auto hModule = reinterpret_cast<HMODULE>(Platform::GetImageBase());
    if (hModule == NULL)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"Error: Could not get module handle.");
        return;
    }

    // Access the DOS header
    IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)hModule;
    // Access the PE header
    IMAGE_NT_HEADERS* peHeader = (IMAGE_NT_HEADERS*)((BYTE*)hModule + dosHeader->e_lfanew);

    // Check for PE signature
    if (peHeader->Signature != IMAGE_NT_SIGNATURE)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"Error: PE signature not found.");
        return;
    }

    // Access the section headers
    IMAGE_SECTION_HEADER* sectionHeaders = IMAGE_FIRST_SECTION(peHeader);
    const int numberOfSections = peHeader->FileHeader.NumberOfSections;

    // List the sections
    for (int i = 0; i < numberOfSections; i++)
    {
        IMAGE_SECTION_HEADER* sectionHeader = &sectionHeaders[i];
        if (strcmp(reinterpret_cast<const char*>(sectionHeader->Name), ".text") == 0)
            m_codeOffset = sectionHeader->VirtualAddress;
        else if (strcmp(reinterpret_cast<const char*>(sectionHeader->Name), ".data") == 0)
            m_dataOffset = sectionHeader->VirtualAddress;
        else if (strcmp(reinterpret_cast<const char*>(sectionHeader->Name), ".rdata") == 0)
            m_rdataOffset = sectionHeader->VirtualAddress;
    }
}

#else

// The Windows map is "hash -> segment:offset", with offsets computed from the Windows
// executable. None of it transfers.
//
// The Mac build reads its own file instead, mapping the same RED4ext hashes to absolute
// link-time addresses in the Mac executable. Sections do not come into it: a Mach-O address
// already includes the image's link base, so turning one into a runtime address is only a
// matter of adding the ASLR slide.
//
// Note how small this file needs to be. Everything the engine exposes through RTTI -- every
// class, its size, and every property offset -- is read from the game's own type registry at
// runtime and needs no table at all. What remains is the handful of plain functions RED4ext
// hooks, which have neither reflection data nor an exported symbol. See mac-rtti/README.md in
// the porting repo.

void Addresses::LoadSections()
{
    // Mach-O needs no equivalent step; addresses in the map are absolute.
    //
    // What does belong here is the one entry point the image describes itself. LC_MAIN records
    // the offset of main, which dyld calls after every initializer -- the same place in startup
    // that WinMain occupies on Windows, and what RED4ext's WinMain hook is really after.
    //
    // Reading it from the load commands rather than recording an offset means it keeps working
    // across game updates.
    if (const auto entryPoint = Platform::GetEntryPoint())
    {
        m_addresses.emplace(Hashes::WinMain, entryPoint);
        spdlog::info("Entry point (LC_MAIN) resolved to {:#x}", entryPoint);
    }
    else
    {
        spdlog::warn("Could not read LC_MAIN; the startup hook will not be attached");
    }
}

void Addresses::LoadAddresses(const std::filesystem::path& aPath)
{
    if (!exists(aPath))
    {
        // Deliberately not fatal, unlike the Windows path. There, a missing address file means
        // a broken install. Here it means the entry points have not been located in the Mac
        // binary yet -- a known, partial state -- so start anyway and let each hook report
        // itself as unresolved.
        spdlog::warn(L"No Mac address map at '{}'. Hooks that need one will not be attached.", aPath);
        return;
    }

    spdlog::info(L"Loading game's addresses from '{}'...", aPath);

    try
    {
        simdjson::ondemand::parser parser;
        simdjson::padded_string json = simdjson::padded_string::load(aPath.string());
        simdjson::ondemand::document document = parser.iterate(json);

        simdjson::ondemand::array root;
        auto error = document["Addresses"].get_array().get(root);
        if (error)
        {
            spdlog::error(L"Could not get the root array for the addresses: {}",
                          Utils::Widen(simdjson::error_message(error)));
            return;
        }

        const auto slide = Platform::GetImageSlide();

        root.reset();

        for (auto entry : root)
        {
            auto hashField = entry.find_field("hash");
            auto addressField = entry.find_field("address");

            if (hashField.error() || addressField.error())
            {
                continue;
            }

            std::uint64_t hash;
            if (hashField.get_uint64_in_string().get(hash))
            {
                spdlog::error("Skipping an entry whose hash could not be read");
                continue;
            }

            std::string_view addressStr;
            if (addressField.get_string().get(addressStr))
            {
                spdlog::error("Skipping entry {} because its address could not be read", hash);
                continue;
            }

            const auto prefix = addressStr.starts_with("0x") || addressStr.starts_with("0X") ? 2u : 0u;

            std::uintptr_t staticAddress = 0;
            const auto result =
                std::from_chars(addressStr.data() + prefix, addressStr.data() + addressStr.size(), staticAddress, 16);
            if (result.ec != std::errc())
            {
                spdlog::error("Skipping entry {} because '{}' is not a hexadecimal address", hash, addressStr);
                continue;
            }

            if (staticAddress == 0)
            {
                // A placeholder for an entry point that is known about but not yet located.
                // Leaving it out of the map makes Hook::Attach report it cleanly.
                continue;
            }

            m_addresses.insert_or_assign(static_cast<std::uint32_t>(hash), staticAddress + slide);
        }
    }
    catch (const std::exception& e)
    {
        spdlog::error("Could not read the Mac address map: {}", e.what());
        return;
    }

    spdlog::info("{} game addresses loaded", m_addresses.size());
}

#endif

RED4EXT_C_EXPORT std::uintptr_t RED4EXT_CALL RED4ext_ResolveAddress(const std::uint32_t aHash)
{
    return Addresses::Instance()->Resolve(aHash);
}
