#pragma once

#include "Platform.hpp"

#include <scc.h>

/**
 * @brief Load redscript's C API out of an already-opened scc library.
 *
 * scc.h provides scc_load_api itself, but only inside `#if defined(_WIN32)`: it is written
 * against HMODULE and GetProcAddress. The struct it fills is public, so the non-Windows path
 * below fills the same fields through Platform::GetSymbol.
 *
 * Keep the field order identical to scc.h's SccApi. Every entry is a plain symbol lookup, and
 * several are documented as legitimately null on older scc builds, so a missing symbol is not
 * treated as an error here -- callers already null-check the optional ones.
 */
namespace Platform
{
inline SccApi LoadSccApi(HMODULE aModule)
{
#if defined(_WIN32)
    return scc_load_api(aModule);
#else
    const auto symbol = [aModule](const char* aName)
    {
        return Platform::GetSymbol(aModule, aName);
    };

    SccApi api = {
        (scc_settings_new*)symbol("scc_settings_new"),
        (scc_settings_set_custom_cache_file*)symbol("scc_settings_set_custom_cache_file"),
        (scc_settings_set_output_cache_file*)symbol("scc_settings_set_output_cache_file"),
        (scc_settings_add_script_path*)symbol("scc_settings_add_script_path"),
        (scc_settings_disable_error_popup*)symbol("scc_settings_disable_error_popup"),
        (scc_settings_register_never_ref_type*)symbol("scc_settings_register_never_ref_type"),
        (scc_settings_register_mixed_ref_type*)symbol("scc_settings_register_mixed_ref_type"),
        (scc_compile*)symbol("scc_compile"),
        (scc_free_result*)symbol("scc_free_result"),
        (scc_get_success*)symbol("scc_get_success"),
        (scc_copy_error*)symbol("scc_copy_error"),
        (scc_output_get_source_ref*)symbol("scc_output_get_source_ref"),
        (scc_output_source_ref_count*)symbol("scc_output_source_ref_count"),
        (scc_source_ref_type*)symbol("scc_source_ref_type"),
        (scc_source_ref_is_native*)symbol("scc_source_ref_is_native"),
        (scc_source_ref_name*)symbol("scc_source_ref_name"),
        (scc_source_ref_parent_name*)symbol("scc_source_ref_parent_name"),
        (scc_source_ref_path*)symbol("scc_source_ref_path"),
        (scc_source_ref_line*)symbol("scc_source_ref_line"),
    };

    return api;
#endif
}

/**
 * @brief Name of the scc executable the game shells out to.
 */
inline constexpr const char* GetSccExecutableName()
{
#if defined(_WIN32)
    return "scc.exe";
#else
    return "scc";
#endif
}

/**
 * @brief File name of the scc shared library sitting next to that executable.
 */
inline constexpr const wchar_t* GetSccLibraryName()
{
#if defined(_WIN32)
    return L"scc_lib.dll";
#else
    // redscript's own macOS build produces this name, and it is what the working redscript
    // setup already installs into the game's engine/tools directory.
    return L"libscc_lib.dylib";
#endif
}
} // namespace Platform
