#pragma once

#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

// Brings in <Windows.h> on Windows, and the handful of Win32 spellings the runtime uses
// elsewhere. Must come before the third-party headers, as <Windows.h> did.
#include "Platform.hpp"

#ifdef _WIN32
#include <winternl.h>

#include <detours.h>

#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#endif

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/xchar.h>

#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <RED4ext/RED4ext.hpp>
